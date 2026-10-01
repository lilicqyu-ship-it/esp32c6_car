/*
 * http_server.c - httpd + WebSocket + REST endpoints (LLDD 3.2 / 4.3)
 */
#include "http_server.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "lwip/sockets.h"

#include "assets_store.h"
#include "pair.h"
#include "proto_frames.h"
#include "ws_sessions.h"

static const char *TAG = "c6_http";

#define RX_BUF_LEN        320          /* proto frame (72) / ctl JSON (~300) */
#define OTA_CHUNK         512
#define OTA_TOTAL_MAX     (3u * 1024u * 1024u)

typedef struct
{
    httpd_handle_t      hd;
    ws_bin_cb_t         bin_cb;
    http_diag_fn        diag_fn;
    void              (*tcver_cb)(void);      /* {"t":"tcver"} request -> bridge */
    http_upload_sink_t  sink_c6;
    http_upload_sink_t  sink_tc;
    bool                have_c6;
    bool                have_tc;
    char                fw_ver[24];
    uint32_t            tick;                 /* paced broadcast counter */
    SemaphoreHandle_t   tx_mtx;               /* serialises every WS transmit */
} http_ctx_t;

static http_ctx_t s_http;

/* httpd_ws_send_frame_async() writes the WS header and the payload with two
 * separate send() calls and takes no lock.  WS frames leave this box from
 * several tasks (httpd task: hello/pong/err; bridge broadcaster; pair), so two
 * frames on one socket could interleave byte-wise - the S3 remote then parses
 * payload bytes as a header ("Non-zero RSV bits (rsv=0x20)"), drops the link
 * and the vehicle is stopped by RADIO LOST.  Every transmit goes through here.
 * The lock wait exceeds the 500 ms SO_SNDTIMEO so a lock timeout can only mean
 * a genuinely stalled peer, never a false "dead" count on a healthy one. */
#define WS_TX_LOCK_MS  600u

static esp_err_t ws_tx(int fd, httpd_ws_frame_t *pkt)
{
    esp_err_t err;

    if ((s_http.tx_mtx == NULL) ||
        (xSemaphoreTake(s_http.tx_mtx, pdMS_TO_TICKS(WS_TX_LOCK_MS)) != pdTRUE))
    {
        return ESP_ERR_TIMEOUT;
    }
    err = httpd_ws_send_frame_async(s_http.hd, fd, pkt);
    (void)xSemaphoreGive(s_http.tx_mtx);
    return err;
}

static int http_send_frame(int fd, const uint8_t *payload, size_t len, bool text);
uint32_t http_sock_zombies(void);

/* ========================================================================== */
/* small helpers                                                              */
/* ========================================================================== */

static esp_err_t send_json(httpd_req_t *req, int code, const char *json)
{
    const char *msg = "OK";
    switch (code)
    {
        case 200: msg = "OK"; break;
        case 400: msg = "Bad Request"; break;
        case 401: msg = "Unauthorized"; break;
        case 403: msg = "Forbidden"; break;
        case 404: msg = "Not Found"; break;
        case 409: msg = "Conflict"; break;
        case 413: msg = "Payload Too Large"; break;
        case 502: msg = "Bad Gateway"; break;
        case 503: msg = "Service Unavailable"; break;
        case 504: msg = "Gateway Timeout"; break;
        case 507: msg = "Insufficient Storage"; break;
        default:  msg = "Internal Server Error"; break;
    }
    /* httpd_resp_set_status() takes the WHOLE status text ("200 OK", cf.
     * HTTPD_500).  Passing only the reason phrase produced the status line
     * "HTTP/1.1 OK": browsers shrug it off, but esp_http_client's http_parser
     * rejects it, so the S3 remote saw /api/health and /api/pair time out
     * (status -1).  The buffer only has to outlive httpd_resp_send() below. */
    char status[40];
    (void)snprintf(status, sizeof(status), "%d %s", code, msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* extract ?token= or X-Session-Token header */
static bool get_request_token(httpd_req_t *req, char *out, size_t cap)
{
    char query[128];
    bool found = false;

    if (cap == 0u)
    {
        return false;
    }
    out[0] = '\0';
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        found = (httpd_query_key_value(query, "token", out, cap) == ESP_OK);
    }
    if (!found &&
        httpd_req_get_hdr_value_str(req, "X-Session-Token", out, cap) == ESP_OK)
    {
        found = (out[0] != '\0');
    }
    out[cap - 1u] = '\0';                /* httpd fills cap without NUL */
    return found;
}

static const char *content_type_for(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL)
    {
        return "application/octet-stream";
    }
    if (strcmp(dot, ".html") == 0) { return "text/html"; }
    if (strcmp(dot, ".js")   == 0) { return "application/javascript"; }
    if (strcmp(dot, ".css")  == 0) { return "text/css"; }
    if (strcmp(dot, ".svg")  == 0) { return "image/svg+xml"; }
    if (strcmp(dot, ".png")  == 0) { return "image/png"; }
    if (strcmp(dot, ".ico")  == 0) { return "image/x-icon"; }
    if (strcmp(dot, ".json") == 0) { return "application/json"; }
    return "application/octet-stream";
}

/* ========================================================================== */
/* static assets                                                              */
/* ========================================================================== */

static esp_err_t assets_handler(httpd_req_t *req)
{
    const char *path = req->uri;
    assets_entry_t e;
    uint8_t buf[4096];

    ESP_LOGI(TAG, "GET %s", req->uri);
    if (strcmp(path, "/") == 0)
    {
        path = "/index.html";
    }
    if (assets_find(path, &e))
    {
        httpd_resp_set_type(req, content_type_for(e.name));
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        /* no immutable/max-age: the page and the firmware ship together and
         * speak one protocol - a browser running a stale cached app.js
         * against new firmware fails in ways that are painful to debug.
         * Assets are a few KB gzipped, refetching them is negligible. */
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        uint32_t off = 0u;
        while (off < e.gz_len)
        {
            int n = assets_read(&e, off, buf, sizeof(buf));
            if (n <= 0)
            {
                ESP_LOGW(TAG, "GET %s read fail @%lu", path, (unsigned long)off);
                return ESP_FAIL;
            }
            if (httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)n) != ESP_OK)
            {
                ESP_LOGW(TAG, "GET %s send fail @%lu", path, (unsigned long)off);
                return ESP_FAIL;
            }
            off += (uint32_t)n;
        }
        (void)httpd_resp_send_chunk(req, NULL, 0);
        ESP_LOGI(TAG, "GET %s ok (%lu B gz)", path, (unsigned long)e.gz_len);
        return ESP_OK;
    }

    /* embedded fallback for "/" when the assets partition has no page (C10) */
    if (strcmp(req->uri, "/") == 0)
    {
        size_t len = 0;
        const uint8_t *html = assets_embedded_html(&len);
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, (const char *)html, len);
    }
    return send_json(req, 404, "{\"err\":\"not found\"}");
}

/* ========================================================================== */
/* WebSocket                                                                  */
/* ========================================================================== */

/* pre-handshake: token -> role. Rejecting keeps the socket in HTTP mode.
 * NOTE: the session opened here must NOT become a broadcast target yet. Any
 * frame written before the 101 response goes out interleaves into the
 * handshake bytes and the browser rejects the upgrade (observed: WS connect
 * loop, peer RST ~3 ms after every handshake). ws_sess_open/ws_sess_promote
 * fire the change callback -> bridge_notify_clients -> http_broadcast_ctl,
 * so eligibility is what keeps those sends off this socket; the flag is set
 * in ws_post_handshake once the response is on the wire. */
static esp_err_t ws_pre_handshake(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    char token[80];
    ws_session_t *s = ws_sess_open(fd);           /* SPECTATOR by default */

    if (s == NULL)
    {
        return ESP_FAIL;                          /* table full -> refuse upgrade */
    }
#if CONFIG_C6_BENCH_CTRL
    /* bench: the TC275 build has no PAIR consumer yet, so the token flow can
     * never complete - grant CTRL directly (production keeps the gate) */
    (void)token;
    ws_sess_promote(fd, NULL);
    ESP_LOGI(TAG, "ws fd=%d CTRL (bench bypass)", fd);
#else
    if (get_request_token(req, token, sizeof(token)) && pair_token_ok(token))
    {
        uint8_t hash[16];
        pair_hash_token(token, hash);
        ws_sess_promote(fd, hash);
        ESP_LOGI(TAG, "ws fd=%d CTRL (token ok)", fd);
    }
    else
    {
        ESP_LOGI(TAG, "ws fd=%d spectator", fd);
    }
#endif
    return ESP_OK;
}

static esp_err_t ws_send_hello(int fd)
{
    char json[160];
    pair_state_t ps = pair_state();

    (void)snprintf(json, sizeof(json),
                   "{\"t\":\"hello\",\"role\":\"%s\",\"ver\":\"%s\",\"tc\":\"%s\","
                   "\"pair\":\"%s\",\"ctrl\":%s}",
                   (ws_sess_ctrl_fd() == fd) ? "ctrl" : "spectator",
                   s_http.fw_ver,
                   "down",                        /* bridge refreshes via link_state frames */
                   (ps == PAIR_OPEN) ? "open" : ((ps == PAIR_CLAIMED) ? "claimed" : "idle"),
                   (ws_sess_ctrl_fd() >= 0) ? "true" : "false");
    return ws_send_ctl(fd, json);
}

/* post-handshake: the 101 response has been sent, so WS frames on this socket
 * are legal from here on. Mark the session as an established broadcast target
 * and push hello proactively - the page waits for hello before sending
 * anything (sendDrive is gated on ctrl), so a hello sent only on the first
 * received frame would deadlock the connection. */
static void ws_tighten_send_timeout(int fd)
{
    /* WS frames go out from the bridge broadcaster too; a peer whose TCP
     * window has filled (stalled / going away) must not park the sender for
     * the 10 s httpd default - 500 ms is ample for a ~100 B frame, and long
     * sender stalls are what backs the command queue up into "busy" replies. */
    const struct timeval tv = { .tv_sec = 0, .tv_usec = 500u * 1000u };
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    /* Small control/telemetry frames must leave immediately: with Nagle on,
     * each frame waits for the ACK of the previous one and the S3 remote's
     * lwIP delays ACKs - telemetry and pong arrive 100+ ms late.  Browsers
     * already disable Nagle on their side; this matches it on ours. */
    const int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static esp_err_t ws_post_handshake(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    ws_session_t *sess;

    ws_tighten_send_timeout(fd);
    ws_sess_set_ws(fd);
    sess = ws_sess_get(fd);
    ESP_LOGI(TAG, "post hs fd=%d sess=%d", fd, sess != NULL);
    if ((sess != NULL) && (sess->hello_sent == 0u))
    {
        sess->hello_sent = 1u;
        esp_err_t hrc = ws_send_hello(fd);
        ESP_LOGI(TAG, "hello fd=%d rc=%s", fd, esp_err_to_name(hrc));
    }
    return ESP_OK;
}

static void ws_handle_binary(const uint8_t *payload, size_t len, int fd)
{
    proto_parser_t parser;
    proto_frame_t f;

    /* C6 first gate (LLDD 4.3): role + monotonic SEQ; final verdict on TC275 */
    if (len < PROTO_HEADER_LEN)
    {
        (void)ws_send_ctl(fd, "{\"t\":\"err\",\"e\":\"frame\"}");
        return;
    }
    if (!ws_sess_check_cmd(fd, payload[PROTO_SEQ_OFF]))
    {
        (void)ws_send_ctl(fd, "{\"t\":\"err\",\"e\":\"auth\"}");
        return;
    }
    /* parse the whole proto frame out of the WS payload */
    proto_parser_init(&parser);
    proto_rx_ev_t ev = PROTO_RX_NONE;
    for (size_t i = 0u; i < len; i++)
    {
        ev = proto_parser_feed(&parser, payload[i], &f);
        if (ev == PROTO_RX_FRAME)
        {
            break;
        }
    }
    if (ev != PROTO_RX_FRAME)
    {
        (void)ws_send_ctl(fd, "{\"t\":\"err\",\"e\":\"frame\"}");
        return;
    }
    if (s_http.bin_cb != NULL)
    {
        s_http.bin_cb(&f, fd);
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    httpd_ws_frame_t pkt;
    uint8_t buf[RX_BUF_LEN];
    esp_err_t ret;

    ws_session_t *sess = ws_sess_get(fd);
    if (sess == NULL)
    {
        /* upgrade went through the pre-handshake callback; if a build has the
         * callback kconfig off, do the bookkeeping here instead. Either way
         * the handler only runs post-101, so the session may go live now. */
        if (ws_pre_handshake(req) != ESP_OK)
        {
            return ESP_FAIL;
        }
        ws_tighten_send_timeout(fd);
        ws_sess_set_ws(fd);
        sess = ws_sess_get(fd);
    }
    if ((sess != NULL) && (sess->hello_sent == 0u))
    {
        sess->hello_sent = 1u;
        (void)ws_send_hello(fd);
    }

    memset(&pkt, 0, sizeof(pkt));
    ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (pkt.len == 0u)
    {
        /* a CLOSE frame arrives with no payload, so the generic control-frame
         * early-out would swallow it and the session would linger until the
         * browser's FIN fallback - answer the hangup explicitly instead */
        if (pkt.type == HTTPD_WS_TYPE_CLOSE)
        {
            ESP_LOGI(TAG, "ws close frame fd=%d", fd);
            ws_sess_close(fd);
            return ESP_FAIL;
        }
        if (pkt.type == HTTPD_WS_TYPE_PING)
        {
            /* handle_ws_control_frames=true hands PINGs to us and httpd no
             * longer answers them. The S3 remote's esp_websocket_client pings
             * every 10 s and drops the link after 120 s without a PONG - the
             * periodic "RADIO LOST - vehicle stop" every ~2 min. */
            httpd_ws_frame_t pong = { .type = HTTPD_WS_TYPE_PONG, .final = true };
            (void)ws_tx(fd, &pong);
        }
        return ESP_OK;                            /* pong / other control frame */
    }
    if (pkt.len > (RX_BUF_LEN - 1u))
    {
        (void)ws_send_ctl(fd, "{\"t\":\"err\",\"e\":\"big\"}");
        /* the frame body is still queued in the socket: staying in sync is
         * not worth partial-drain logic, drop the connection instead */
        return ESP_FAIL;
    }
    pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret != ESP_OK)
    {
        return ret;
    }

    switch (pkt.type)
    {
        case HTTPD_WS_TYPE_PING:
        {
            /* RFC 6455 5.5.3: PONG echoes the PING payload */
            httpd_ws_frame_t pong = { .type = HTTPD_WS_TYPE_PONG, .final = true,
                                      .payload = pkt.payload, .len = pkt.len };
            (void)ws_tx(fd, &pong);
            break;
        }
        case HTTPD_WS_TYPE_BINARY:
            ws_handle_binary(pkt.payload, pkt.len, fd);
            break;
        case HTTPD_WS_TYPE_TEXT:
        {
            pkt.payload[pkt.len] = '\0';
            if (strstr((const char *)pkt.payload, "tcver") != NULL)
            {
                ESP_LOGI(TAG, "ws text fd=%d len=%u: %s", fd, (unsigned)pkt.len,
                         (const char *)pkt.payload);
            }
            if (strncmp((const char *)pkt.payload, "{\"t\":\"ping\"}", 13u) == 0)
            {
                (void)ws_send_ctl(fd, "{\"t\":\"pong\"}");
            }
            else if (strncmp((const char *)pkt.payload, "{\"t\":\"tcver\"}", 13u) == 0)
            {
                /* on-demand TC275 version: the bridge sends DIAG 0x53/0x24 down
                 * the SPI link; the reply returns as the usual tcver broadcast */
                if (s_http.tcver_cb != NULL)
                {
                    s_http.tcver_cb();
                }
            }
            break;
        }
        case HTTPD_WS_TYPE_CLOSE:
            ESP_LOGI(TAG, "ws close frame(fd) fd=%d", fd);
            ws_sess_close(fd);
            return ESP_FAIL;                        /* hang up: a later frame
                                                       would silently re-open
                                                       the closed session */
        default:
            break;
    }
    return ESP_OK;
}

/* ---- bridge-facing WS senders --------------------------------------------- */

esp_err_t ws_send_ctl(int sd, const char *json)
{
    httpd_ws_frame_t pkt;
    size_t len;

    if ((s_http.hd == NULL) || (json == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }
    len = strlen(json);
    memset(&pkt, 0, sizeof(pkt));
    pkt.type     = HTTPD_WS_TYPE_TEXT;
    pkt.payload  = (uint8_t *)json;
    pkt.len      = len;
    pkt.final    = true;
    return ws_tx(sd, &pkt);
}

void http_broadcast_ctl(const char *json)
{
    if (s_http.hd == NULL)
    {
        return;
    }
    (void)ws_sessions_foreach_send(http_send_frame, (const uint8_t *)json,
                                   strlen(json), true, ++s_http.tick);
}

/* raw per-fd sender used by the pacing iterator; 0 = ok */
static int http_send_frame(int fd, const uint8_t *payload, size_t len, bool text)
{
    httpd_ws_frame_t pkt;

    memset(&pkt, 0, sizeof(pkt));
    pkt.type    = text ? HTTPD_WS_TYPE_TEXT : HTTPD_WS_TYPE_BINARY;
    pkt.payload = (uint8_t *)payload;
    pkt.len     = len;
    pkt.final   = true;
    esp_err_t err = ws_tx(fd, &pkt);
    if (err == ESP_OK)
    {
        httpd_sess_update_lru_counter(s_http.hd, fd);
        return 0;
    }
    /* The peer is not taking frames. ws_sessions_foreach_send bumps this fd's
     * failure counter on our -1; once it crosses WS_DEAD_CLOSE the peer is gone
     * for good (a phone that locked / left the AP never sends a WS CLOSE), so
     * ask httpd to close the socket. That fires http_close_cb, which frees the
     * session and the lwIP fd - without this the socket leaks on every silent
     * disconnect until accept() ENFILEs and the page can no longer load. */
    if (ws_sess_should_close(fd))
    {
        (void)httpd_sess_trigger_close(s_http.hd, fd);
    }
    return -1;
}

esp_err_t ws_broadcast_binary(const proto_frame_t *f)
{
    uint8_t wire[PROTO_MAX_FRAME];
    size_t n;

    if ((s_http.hd == NULL) || (f == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }
    n = proto_encode(f, wire, sizeof(wire));
    if (n == 0u)
    {
        return ESP_ERR_INVALID_ARG;
    }
    (void)ws_sessions_foreach_send(http_send_frame, wire, n, false, ++s_http.tick);
    return ESP_OK;
}

void ws_on_binary(ws_bin_cb_t cb)
{
    s_http.bin_cb = cb;
}

int ws_controller_sd(void)
{
    return ws_sess_ctrl_fd();
}

int ws_client_count(void)
{
    return ws_sess_count();
}

bool http_sd_is_ctrl(int sd)
{
    return (ws_sess_ctrl_fd() == sd);
}

/* ========================================================================== */
/* REST endpoints                                                             */
/* ========================================================================== */

static esp_err_t api_health_handler(httpd_req_t *req)
{
    char json[256];
    const esp_partition_t *run = esp_ota_get_running_partition();

    ESP_LOGI(TAG, "health");
    (void)snprintf(json, sizeof(json),
                   "{\"up\":true,\"ver\":\"%s\",\"slot\":\"%s\",\"ctrl\":%s,"
                   "\"heap\":%u}",
                   s_http.fw_ver,
                   (run != NULL) ? run->label : "?",
                   (ws_sess_ctrl_fd() >= 0) ? "true" : "false",
                   (unsigned)esp_get_free_heap_size());
    return send_json(req, 200, json);
}

static esp_err_t api_diag_handler(httpd_req_t *req)
{
    static char json[768];

    if (s_http.diag_fn != NULL)
    {
        /* diag_fn renders the app object WITH its closing brace; strip it and
         * append the http-layer view (clients, reaped zombies) before closing
         * - the app layer must not need an http_server.h dependency */
        s_http.diag_fn(json, sizeof(json) - 64);
        size_t len = strlen(json);
        if ((len > 0u) && (json[len - 1u] == '}'))
        {
            json[--len] = '\0';
        }
        int n = (int)len;
        n += (int)snprintf(json + n, sizeof(json) - (size_t)n,
                           ",\"cli\":%d,\"zs\":%lu}",
                           ws_client_count(), (unsigned long)http_sock_zombies());
        ESP_LOGI(TAG, "GET /api/diag (%u B)", (unsigned)n);
        /* text/plain, not application/json: a phone navigating here directly
         * rendered the JSON content type as a blank/eternal-loading tab
         * (09-28); plain text always displays, and the /diag page's
         * response.json() parses the body regardless of content type */
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    }
    return send_json(req, 200, "{\"err\":\"no diag provider\"}");
}

/* Human-readable diag view (bench, 09-28): phones showed a blank tab for the
 * bare application/json /api/diag document (rendering, not transport - the
 * 275 B response left the server cleanly), and a raw JSON wall is no diag UI
 * anyway.  /diag serves a self-contained HTML shell that fetch()es the JSON
 * every 2 s and renders it readably; a failed fetch prints the error instead
 * of a silent blank - that alone separates "phone left the AP" from "link
 * degraded".  /api/diag stays pure JSON for tooling. */
static const char DIAG_PAGE[] =
"<!DOCTYPE html><html><head><meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>C6 诊断</title>\n"
"<style>\n"
"body{font-family:-apple-system,sans-serif;margin:12px;background:#14181f;color:#e8eaed}\n"
"h1{font-size:1.05rem;margin:0 0 8px}\n"
"#age{font-size:.72rem;color:#9fb3c8;font-weight:400}\n"
"#age.bad{color:#e57373}\n"
"h2{font-size:.78rem;color:#9fb3c8;margin:14px 0 2px;font-weight:600;letter-spacing:.06em}\n"
"table{border-collapse:collapse;width:100%}\n"
"td,th{padding:4px 8px;border-bottom:1px solid #2a3140;text-align:left;font-size:.85rem;vertical-align:top}\n"
"th{color:#9fb3c8;font-weight:500;white-space:nowrap;width:32%}\n"
"td{font-variant-numeric:tabular-nums}\n"
".ok{color:#7dd087}.bad{color:#e57373}.warn{color:#ffb224}.dim{color:#9fb3c8}\n"
".big{font-size:1.1rem;font-weight:600}\n"
".hint{font-size:.7rem;color:#9fb3c8;line-height:1.5;padding:4px 8px}\n"
"#raw{white-space:pre-wrap;word-break:break-all;font-size:.66rem;color:#7fbf7f;padding:8px 0}\n"
"summary{font-size:.76rem;color:#9fb3c8;cursor:pointer;margin-top:14px}\n"
"</style></head><body>\n"
"<h1>C6 诊断 <span id=\"age\">加载中…</span></h1>\n"
"<div id=\"v\">加载中…</div>\n"
"<details><summary>原始数据（JSON）</summary><div id=\"raw\"></div></details>\n"
"<script>\n"
"\"use strict\";\n"
"function p(j,k){return (j&&j[k]!=null)?j[k]:\"-\"}\n"
"function row(k,v){return \"<tr><th>\"+k+\"</th><td>\"+v+\"</td></tr>\"}\n"
"function tag(v,c){return '<span class=\"'+c+'\">'+v+'</span>'}\n"
"function fmtUp(s){s=Math.max(0,+s||0);var d=(s/86400)|0,h=((s%86400)/3600)|0,m=((s%3600)/60)|0;\n"
" return (d?d+\"天\":\"\")+(d||h?h+\"时\":\"\")+(d||h||m?m+\"分\":\"\")+(s%60)+\"秒\"}\n"
"var RESETS={0:\"未知\",1:\"上电\",2:\"软件复位\",3:\"程序崩溃\",4:\"中断看门狗\",5:\"任务看门狗\",6:\"RTC看门狗\",7:\"掉电\",8:\"掉电复位\"};\n"
"function fmtReset(v){return RESETS[+v]||(\"代码 \"+v)}\n"
"var ok0=function(v){return v==0?\"ok\":\"bad\"};\n"
"let n=0;\n"
"async function tick(){try{\n"
"const c=new AbortController();const t=setTimeout(()=>c.abort(),3000);\n"
"const r=await fetch(\"/api/diag\",{cache:\"no-store\",signal:c.signal});\n"
"clearTimeout(t);\n"
"if(!r.ok){throw new Error(\"HTTP \"+r.status)}\n"
"const j=await r.json();const L=j.link||{};const I=j.imu;\n"
"let h=\"\";\n"
"h+=\"<h2>系统</h2><table>\";\n"
"h+=row(\"固件版本\",p(j,\"ver\"));\n"
"h+=row(\"运行状态\",tag(p(j,\"state\"),p(j,\"state\")===\"online\"?\"ok\":\"warn\"));\n"
"h+=row(\"OTA 槽位\",p(j,\"slot\")+(j.factory?' <span class=\"warn\">出厂模式</span>':\"\"));\n"
"h+=row(\"运行时长\",fmtUp(j.uptime_s));\n"
"h+=row(\"历史最低内存\",((Math.max(0,+j.heap_min||0))/1024).toFixed(1)+\" KB\");\n"
"h+=row(\"上次复位原因\",fmtReset(j.reset)+(j.coredump?' <span class=\"bad\">有转储</span>':\"\"));\n"
"h+=row(\"开机自检\",j.selfcheck?'<span class=\"ok\">通过</span>':'<span class=\"dim\">未记录</span>');\n"
"h+=\"</table>\";\n"
"h+=\"<h2>TC275 车辆链路（SPI）</h2><table>\";\n"
"h+=row(\"链路状态\",L.up?'<span class=\"ok\">在线</span>':'<span class=\"bad\">离线</span>');\n"
"h+=row(\"时钟\",((Math.max(0,+L.clock||0))/1000000).toFixed(1)+\" MHz\");\n"
"const rtt=+L.rtt||0;\n"
"h+=row(\"往返延迟\",tag(rtt+\" ms\",rtt<=10?\"ok\":rtt<=50?\"warn\":\"bad\"));\n"
"h+=row(\"收帧 / 发帧\",(L.rx||0)+\" / \"+(L.tx||0));\n"
"h+=row(\"CRC / 格式错误\",tag((L.crc_err||0)+\" / \"+(L.fmt_err||0),ok0((L.crc_err||0)+(L.fmt_err||0))));\n"
"h+=row(\"发送拥塞\",(L.busy||0)+( (L.busy||0)>0?' <span class=\"warn\">偏高</span>':\"\"));\n"
"h+=row(\"配对状态\",p(j,\"pair\"));\n"
"h+=\"</table>\";\n"
"h+=\"<h2>IMU 三轴加速度（ADXL345）</h2>\";\n"
"if(I&&I.ok){\n"
" const mg=I.mg||[0,0,0];\n"
" const g=function(v){return ((+v)/1000).toFixed(3)};\n"
" const pitch=Math.atan2(mg[0],mg[2])*180/Math.PI;\n"
" const roll=Math.atan2(mg[1],mg[2])*180/Math.PI;\n"
" const snap=function(a){return Math.abs(a)<2?0:a};\n"
" const P=snap(pitch),R=snap(roll);\n"
" const parts=[];\n"
" if(Math.abs(P)>=0.5)parts.push((P>0?\"前倾 \":\"后仰 \")+Math.abs(P).toFixed(1)+\"°\");\n"
" if(Math.abs(R)>=0.5)parts.push((R>0?\"左倾 \":\"右倾 \")+Math.abs(R).toFixed(1)+\"°\");\n"
" if(!parts.length)parts.push(\"水平放置\");\n"
" const mag=+I.mag||0;\n"
" const magCls=(mag>=950&&mag<=1050)?\"ok\":\"warn\";\n"
" h+=row(\"三轴读数\",'<span class=\"big\">'+g(mg[0])+\" / \"+g(mg[1])+\" / \"+g(mg[2])+'</span> g');\n"
" h+=row(\"姿态\",'<span class=\"big\">'+parts.join(\" · \")+\"</span>\");\n"
" h+=row(\"合幅值\",tag(g(mag)+\" g\",magCls)+(magCls===\"ok\"?\"（≈1 g 静止正常）\":\"（偏离 1 g，正在运动？）\"));\n"
" h+=row(\"采样\",((I.upd||0))+\" 次\"+((I.err||0)>0?' · <span class=\"bad\">异常 '+I.err+\"</span>\":\"\"));\n"
"}else if(I&&I.probe){\n"
" h+=row(\"传感器\",'<span class=\"bad\">未检测到</span>');\n"
" h+=row(\"总线探测值\",'<span class=\"big\">'+I.probe+\"</span>（要求 0xe5）\");\n"
" h+='<div class=\"hint\">判读：0xff = MISO 悬空或 SDO 未接 · 稳定的错误值 = 时钟/接线错位 · 数值跳动 = 接触不良。<br>核对接线：CS→GPIO7 · SCL→GPIO10 · SDA→GPIO11 · SDO→GPIO6 · VCC→3V3 · GND</div>';\n"
"}else{\n"
" h+=row(\"传感器\",'<span class=\"dim\">未启动（imu:null）</span>');\n"
"}\n"
"h+=\"<table>\";\n"
"h+=row(\"重力方向参考\",\"三轴为板上坐标系；倾角方向依模块安装方向而定\");\n"
"h+=\"</table>\";\n"
"h+=\"<h2>Web 服务</h2><table>\";\n"
"h+=row(\"在线客户端\",(j.cli||0)+\" 个\");\n"
"h+=row(\"累计回收异常连接\",(j.zs||0)+\" 个\");\n"
"h+=\"</table>\";\n"
"document.getElementById(\"v\").innerHTML=h;\n"
"document.getElementById(\"raw\").textContent=JSON.stringify(j);\n"
"const age=document.getElementById(\"age\");\n"
"age.className=\"\";\n"
"age.textContent=\"更新于 \"+new Date().toLocaleTimeString();\n"
"}catch(e){n++;\n"
"const age=document.getElementById(\"age\");\n"
"age.className=\"bad\";\n"
"age.textContent=\"读取失败: \"+e+\"（第 \"+n+\" 次）· 保留上次数据 · 2 秒后自动重试\";\n"
"if(n===1){document.getElementById(\"v\").innerHTML='<div class=\"hint\">无法读取诊断数据。若持续失败，手机可能已不在车的 Wi-Fi 上（设置里确认 Wi-Fi 与是否选择“保持连接”）。</div>'}\n"
"}}\n"
"tick();setInterval(tick,2000);\n"
"</script></body></html>";

static esp_err_t diag_page_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET /diag");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, DIAG_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_pair_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    char token[80];
    char json[160];
    pair_result_t r = pair_request(fd, token, sizeof(token));

    switch (r)
    {
        case PAIR_RES_OK:
        {
            uint8_t hash[16];
            pair_hash_token(token, hash);
            ws_sess_promote(fd, hash);
            (void)snprintf(json, sizeof(json),
                           "{\"ok\":true,\"role\":\"ctrl\",\"token\":\"%s\"}", token);
            return send_json(req, 200, json);
        }
        case PAIR_RES_NO_WINDOW:
            return send_json(req, 403,
                             "{\"ok\":false,\"e\":\"no window\",\"hint\":\"press the car button 3s\"}");
        case PAIR_RES_BUSY:
            return send_json(req, 409, "{\"ok\":false,\"e\":\"controller already paired\"}");
        case PAIR_RES_TMO:
            return send_json(req, 504, "{\"ok\":false,\"e\":\"timeout\"}");
        case PAIR_RES_LINK_ERR:
            return send_json(req, 503, "{\"ok\":false,\"e\":\"car not connected\"}");
        default:
            return send_json(req, 403, "{\"ok\":false,\"e\":\"rejected\"}");
    }
}

/* ---- firmware upload (sinks registered by app_main) ------------------------ */

static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    const bool self = (strcmp(req->uri, "/ota/c6") == 0);
    const http_upload_sink_t *sink = self ? &s_http.sink_c6 : &s_http.sink_tc;
    const bool have = self ? s_http.have_c6 : s_http.have_tc;
    int fd = httpd_req_to_sockfd(req);
    char token[80];
    char json[192];
    uint8_t *chunk;
    esp_err_t err;

    if (!have)
    {
        return send_json(req, 503, "{\"ok\":false,\"e\":\"ota not ready\"}");
    }
    /* control端 token 必须 (LLDD 3.2) */
    if (!get_request_token(req, token, sizeof(token)) || !pair_token_ok(token))
    {
        return send_json(req, 401, "{\"ok\":false,\"e\":\"auth\"}");
    }

    size_t remaining = req->content_len;
    if (remaining > OTA_TOTAL_MAX)
    {
        return send_json(req, 413, "{\"ok\":false,\"e\":\"too big\"}");
    }

    err = sink->begin(fd, remaining);
    if (err != ESP_OK)
    {
        (void)snprintf(json, sizeof(json), "{\"ok\":false,\"e\":\"begin\",\"code\":%d}", err);
        return send_json(req, 503, json);
    }

    chunk = malloc(OTA_CHUNK);
    if (chunk == NULL)
    {
        return send_json(req, 507, "{\"ok\":false,\"e\":\"nomem\"}");
    }
    while (remaining > 0u)
    {
        size_t want = (remaining < OTA_CHUNK) ? remaining : OTA_CHUNK;
        int n = httpd_req_recv(req, (char *)chunk, (size_t)want);
        if (n <= 0)
        {
            free(chunk);
            if (sink->abort != NULL)
            {
                sink->abort(fd);
            }
            return send_json(req, 400, "{\"ok\":false,\"e\":\"abort\"}");
        }
        err = sink->feed(fd, chunk, (size_t)n);   /* credit window may block here */
        if (err != ESP_OK)
        {
            free(chunk);
            if (sink->abort != NULL)
            {
                sink->abort(fd);
            }
            (void)snprintf(json, sizeof(json), "{\"ok\":false,\"e\":\"feed\",\"code\":%d}", err);
            return send_json(req, 502, json);
        }
        remaining -= (size_t)n;
    }
    free(chunk);

    err = sink->finish(fd, json, sizeof(json));
    if (err != ESP_OK)
    {
        return send_json(req, 502, "{\"ok\":false,\"e\":\"finish\"}");
    }
    return send_json(req, 200, json);
}

/* ========================================================================== */
/* socket close hook                                                          */
/* ========================================================================== */

static void http_close_cb(httpd_handle_t hd, int sockfd)
{
    bool was_ctrl;

    (void)hd;
    ESP_LOGI(TAG, "close fd=%d", sockfd);
    /* LLDD 4.6.3: phone disconnect must terminate an in-flight relay/self
     * OTA immediately (0x65 ABORT to TC275 / ota_task abort flag) */
    if (s_http.have_c6 && (s_http.sink_c6.abort != NULL))
    {
        s_http.sink_c6.abort(sockfd);
    }
    if (s_http.have_tc && (s_http.sink_tc.abort != NULL))
    {
        s_http.sink_tc.abort(sockfd);
    }
    was_ctrl = http_sd_is_ctrl(sockfd);            /* must read role before close */
    ws_sess_close(sockfd);
    if (was_ctrl)
    {
        pair_ctrl_gone();                          /* release CLAIMED pairing gate */
    }
}

/* ========================================================================== */
/* Connected-socket inventory + zombie reaper (bench diagnostic, 09-27 "page
 * never loads again" incident and its 09-28 relapse): that incident presented
 * only as accept() EMFILE with no further clues, and IDF's lwIP keeps no
 * per-pool stats (MEMP_MEM_MALLOC=1).  Enumerate fds via SO_TYPE +
 * getpeername so the next occurrence carries its own evidence: which fd holds
 * which peer, how many live sockets there are.
 *
 * 09-28 relapse evidence: accept() EMFILE persisted while the connected-only
 * inventory showed live=0 - the 24-slot socket table was full of TCP sockets
 * whose pcb was already gone (peer left, fd never closed), which getpeername
 * reports as ENOTCONN and the old inventory skipped by design.  So besides
 * naming them, the timer now REAPS them: TCP + not-listening + no peer means
 * nobody owns that fd's connection anymore - close it so accept() recovers.
 * UDP (mdns) and the listening socket are skipped; every TCP socket in this
 * firmware belongs to the httpd. */
#define DIAG_FD_MAX 128
#define DIAG_PEERS_CAP 320
/* reaped-zombie total, surfaced through /api/diag for the bench UI */
static uint32_t s_zombies_total;

uint32_t http_sock_zombies(void)
{
    return s_zombies_total;
}

static void http_pool_diag(void *unused)
{
    (void)unused;
    static uint8_t tick_div;                 /* full inventory log every 8th run */
    int live = 0;
    int zombies = 0;
    char peers[DIAG_PEERS_CAP];
    char zlist[48];
    size_t off = 0;
    size_t zoff = 0;

    peers[0] = '\0';
    zlist[0] = '\0';
    for (int fd = 0; fd < DIAG_FD_MAX; fd++)
    {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        int type = 0;
        socklen_t tl = sizeof(type);
        int listening = 0;

        if ((getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) != 0) ||
            (tl != sizeof(type)))
        {
            continue;                    /* not an open socket */
        }
        (void)getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &tl);
        const bool is_tcp = (type == SOCK_STREAM);
        const bool has_peer =
            (getpeername(fd, (struct sockaddr *)&ss, &sl) == 0);

        if (is_tcp && !listening && !has_peer)
        {
            /* dead connection, fd still open: the socket-table eater */
            if (zoff < (sizeof(zlist) - 8u))
            {
                zoff += (size_t)snprintf(zlist + zoff, sizeof(zlist) - zoff,
                                         "%s%d", (zombies != 0) ? "," : "", fd);
            }
            zombies++;
            if ((s_http.hd != NULL) &&
                (httpd_sess_trigger_close(s_http.hd, fd) != ESP_OK))
            {
                (void)closesocket(fd);   /* not an httpd session: raw close */
            }
            continue;
        }
        if (!has_peer)
        {
            continue;                    /* UDP / listening: healthy by design */
        }
        /* accepted IPv4 connections on the dual-stack listener report
         * AF_INET6 with a ::ffff:x.y.z.w mapped peer - decode it back */
        if (ss.ss_family == AF_INET)
        {
            const struct sockaddr_in *a = (const struct sockaddr_in *)&ss;
            off += (size_t)snprintf(peers + off, sizeof(peers) - off,
                                    " %d=%s:%u", fd,
                                    inet_ntoa(a->sin_addr),
                                    (unsigned)ntohs(a->sin_port));
        }
        else if (ss.ss_family == AF_INET6)
        {
            const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)&ss;
            const uint8_t *b = (const uint8_t *)&a6->sin6_addr;
            if ((b[0] == 0u) && (b[1] == 0u) && (b[2] == 0u) && (b[3] == 0u) &&
                (b[4] == 0u) && (b[5] == 0u) && (b[6] == 0u) && (b[7] == 0u) &&
                (b[8] == 0u) && (b[9] == 0u) && (b[10] == 0xffu) && (b[11] == 0xffu))
            {
                off += (size_t)snprintf(peers + off, sizeof(peers) - off,
                                        " %d=v4:%u.%u.%u.%u:%u", fd,
                                        b[12], b[13], b[14], b[15],
                                        (unsigned)ntohs(a6->sin6_port));
            }
            else
            {
                off += (size_t)snprintf(peers + off, sizeof(peers) - off,
                                        " %d=v6:%02x%02x:%02x%02x:%u", fd,
                                        b[0], b[1], b[2], b[3],
                                        (unsigned)ntohs(a6->sin6_port));
            }
        }
        else
        {
            off += (size_t)snprintf(peers + off, sizeof(peers) - off, " %d=af%u",
                                    fd, (unsigned)ss.ss_family);
        }
        live++;
        if (off > (sizeof(peers) - 48u))
        {
            break;                       /* keep the log line bounded */
        }
    }
    if (zombies != 0)
    {
        s_zombies_total += (uint32_t)zombies;
        ESP_LOGW(TAG, "SOCK reaped %d zombie fd(s): [%s]", zombies, zlist);
    }
    if (tick_div++ == 0u)
    {
        ESP_LOGI(TAG, "SOCK live=%d ws_sess=%d:%s", live, ws_sess_count(), peers);
    }
}

/* lifecycle                                                                  */
/* ========================================================================== */

void http_on_session_change(void (*cb)(void))
{
    ws_on_change(cb);
}

void http_register_upload_sink(const char *uri, const http_upload_sink_t *s)
{
    if (uri == NULL || s == NULL)
    {
        return;
    }
    if (strcmp(uri, "/ota/c6") == 0)
    {
        s_http.sink_c6 = *s;
        s_http.have_c6 = true;
    }
    else if (strcmp(uri, "/ota/tc275") == 0)
    {
        s_http.sink_tc = *s;
        s_http.have_tc = true;
    }
}

void http_set_diag_provider(http_diag_fn fn)
{
    s_http.diag_fn = fn;
}

void http_on_tcver_request(void (*cb)(void))
{
    s_http.tcver_cb = cb;
}

/* Wildcard catch-all for phone connectivity probes (/hotspot-detect.html,
 * /generate_204, ...) and unmatched paths.
 *
 * CONFIG_C6_CAPTIVE_PORTAL=y : 302 every probe to the control page - that
 *   redirect is what makes iOS/Android auto-pop the captive-portal webview.
 *
 * CONFIG_C6_CAPTIVE_PORTAL=n (default, manual-URL mode) : answer the probe
 *   with exactly what the OS expects for "internet is reachable" so it does
 *   NOT pop a window - iOS/macOS want a tiny body containing "Success",
 *   Android wants HTTP 204 with no body.  The user then opens the page by
 *   typing http://192.168.4.1/ (or http://mycar.local/ via mDNS).  Any other
 *   unknown path returns 404. The AP IP is fixed by c6_net. */
static esp_err_t portal_redirect(httpd_req_t *req)
{
#if CONFIG_C6_CAPTIVE_PORTAL
    ESP_LOGI(TAG, "portal %s", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
#else
    ESP_LOGI(TAG, "probe %s -> success (no popup)", req->uri);

    /* Android/Chrome connectivity check expects an empty 204. */
    if ((strstr(req->uri, "generate_204") != NULL) ||
        (strstr(req->uri, "gen_204") != NULL))
    {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, NULL, 0);
    }

    /* iOS/macOS/Windows CNA probes: a 200 whose body is the exact success
     * marker makes the OS mark the network online and suppress the popup. */
    static const char cna_ok[] =
        "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, cna_ok, HTTPD_RESP_USE_STRLEN);
#endif
}

esp_err_t http_start(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t uris[] = {
        { .uri = "/",          .method = HTTP_GET,  .handler = assets_handler },
        { .uri = "/index.html",.method = HTTP_GET,  .handler = assets_handler },
        { .uri = "/app.js",    .method = HTTP_GET,  .handler = assets_handler },
        { .uri = "/style.css", .method = HTTP_GET,  .handler = assets_handler },
        { .uri = "/calib.html",.method = HTTP_GET,  .handler = assets_handler },
        { .uri = "/calib.js",  .method = HTTP_GET,  .handler = assets_handler },
        { .uri = "/logo.svg",  .method = HTTP_GET,  .handler = assets_handler },
        { .uri = "/favicon.ico", .method = HTTP_GET, .handler = assets_handler },
        { .uri = "/api/health",.method = HTTP_GET,  .handler = api_health_handler },
        { .uri = "/api/diag",  .method = HTTP_GET,  .handler = api_diag_handler },
        { .uri = "/diag",      .method = HTTP_GET,  .handler = diag_page_handler },
        { .uri = "/api/pair",  .method = HTTP_POST, .handler = api_pair_handler },
        { .uri = "/ota/c6",    .method = HTTP_POST, .handler = ota_upload_handler },
        { .uri = "/ota/tc275", .method = HTTP_POST, .handler = ota_upload_handler },
        { .uri = "/ws",        .method = HTTP_GET,  .handler = ws_handler,
          .is_websocket = true, .handle_ws_control_frames = true,
          .ws_pre_handshake_cb = ws_pre_handshake,
          .ws_post_handshake_cb = ws_post_handshake },
        /* catch-all must stay LAST: with the wildcard matcher the first
         * registered match wins, exact entries above shadow these */
        { .uri = "/*",         .method = HTTP_GET,  .handler = portal_redirect },
        { .uri = "/*",         .method = HTTP_POST, .handler = portal_redirect },
    };

    strncpy(s_http.fw_ver, app->version, sizeof(s_http.fw_ver) - 1u);
    /* CNA webview + browser + app probes hit httpd concurrently on one phone;
     * keep above lwip pool headroom so LRU purge, not ENFILE, absorbs bursts */
    cfg.max_open_sockets   = 10;
    cfg.max_uri_handlers   = (uint8_t)(sizeof(uris) / sizeof(uris[0]));
    cfg.stack_size         = 8192;
    cfg.uri_match_fn       = httpd_uri_match_wildcard;
    cfg.lru_purge_enable   = true;
    /* 10 s parks Safari's speculative preconnects - connections that get RST
     * before any request bytes - as pcb-less sessions that hog the lwIP
     * socket table through a refresh burst (09-28 "refresh until dead").
     * 3 s releases them fast; genuine phones send within milliseconds. */
    cfg.recv_wait_timeout  = 3;
    cfg.send_wait_timeout  = 3;
    cfg.close_fn           = http_close_cb;
    /* Phones disconnect silently (lock screen / left the AP / app killed): no
     * FIN ever arrives, httpd has no idle timeout, and nothing else would ever
     * free the socket - the httpd slot and lwIP pcb stay parked until refresh
     * bursts ENFILE accept() and the page stops loading (multi-refresh repro).
     * Kernel keepalive probes reap such a peer in <=9 s, close_fn frees the
     * session and the pcb goes back to the pool. */
    cfg.keep_alive_enable  = true;
    cfg.keep_alive_idle    = 5;    /* s of silence before probing starts */
    cfg.keep_alive_interval = 2;   /* s between probes                   */
    cfg.keep_alive_count   = 2;    /* unanswered probes -> peer is gone  */

    ws_sessions_init();
    if (s_http.tx_mtx == NULL)
    {
        s_http.tx_mtx = xSemaphoreCreateMutex();
    }
    (void)assets_store_init();

    esp_err_t err = httpd_start(&s_http.hd, &cfg);
    if (err != ESP_OK)
    {
        return err;
    }
    for (size_t i = 0u; i < sizeof(uris) / sizeof(uris[0]); i++)
    {
        err = httpd_register_uri_handler(s_http.hd, &uris[i]);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(err));
            (void)httpd_stop(s_http.hd);
            s_http.hd = NULL;
            return err;
        }
    }
    ESP_LOGI(TAG, "httpd up (v%s)", s_http.fw_ver);
    {
        /* see http_pool_diag() above; bench-visible at default log level.
         * 1 s reap cadence: tab-switching phones reload pages in bursts that
         * leave ~10 dead fds each - at 15 s the table filled between runs and
         * accept() EMFILEd for seconds (09-28 relapse, 239 s window). */
        const esp_timer_create_args_t dtargs = {
            .callback = http_pool_diag,
            .name     = "http_pool",
        };
        esp_timer_handle_t dtimer = NULL;
        if ((esp_timer_create(&dtargs, &dtimer) == ESP_OK) &&
            (esp_timer_start_periodic(dtimer, 1000000u) != ESP_OK))
        {
            (void)esp_timer_delete(dtimer);
        }
    }
    return ESP_OK;
}
