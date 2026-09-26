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
    http_upload_sink_t  sink_c6;
    http_upload_sink_t  sink_tc;
    bool                have_c6;
    bool                have_tc;
    char                fw_ver[24];
    uint32_t            tick;                 /* paced broadcast counter */
} http_ctx_t;

static http_ctx_t s_http;

static int http_send_frame(int fd, const uint8_t *payload, size_t len, bool text);

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
    httpd_resp_set_status(req, msg);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* extract ?token= or X-Session-Token header */
static bool get_request_token(httpd_req_t *req, char *out, size_t cap)
{
    char query[128];

    out[0] = '\0';
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    {
        if (httpd_query_key_value(query, "token", out, cap) == ESP_OK)
        {
            return true;
        }
    }
    if (httpd_req_get_hdr_value_str(req, "X-Session-Token", out, cap) == ESP_OK)
    {
        return (out[0] != '\0');
    }
    return false;
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

    if (strcmp(path, "/") == 0)
    {
        path = "/index.html";
    }
    if (assets_find(path, &e))
    {
        httpd_resp_set_type(req, content_type_for(e.name));
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Cache-Control", "immutable");
        uint32_t off = 0u;
        while (off < e.gz_len)
        {
            int n = assets_read(&e, off, buf, sizeof(buf));
            if (n <= 0)
            {
                return ESP_FAIL;
            }
            if (httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)n) != ESP_OK)
            {
                return ESP_FAIL;
            }
            off += (uint32_t)n;
        }
        (void)httpd_resp_send_chunk(req, NULL, 0);
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

/* pre-handshake: token -> role. Rejecting keeps the socket in HTTP mode. */
static esp_err_t ws_pre_handshake(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    char token[80];
    ws_session_t *s = ws_sess_open(fd);           /* SPECTATOR by default */

    if (s == NULL)
    {
        return ESP_FAIL;                          /* table full -> refuse upgrade */
    }
    ws_sess_set_ws(fd);
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

static void ws_handle_binary(const uint8_t *payload, size_t len, int fd)
{
    proto_parser_t parser;
    proto_frame_t f;

    /* C6 first gate (LLDD 4.3): role + monotonic SEQ; final verdict on TC275 */
    if (!ws_sess_check_cmd(fd, payload[4]))
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
         * callback kconfig off, do the bookkeeping here instead */
        if (ws_pre_handshake(req) != ESP_OK)
        {
            return ESP_FAIL;
        }
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
        return ESP_OK;                            /* control frame (ping/close) */
    }
    if (pkt.len > (RX_BUF_LEN - 1u))
    {
        (void)ws_send_ctl(fd, "{\"t\":\"err\",\"e\":\"big\"}");
        return ESP_OK;
    }
    pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret != ESP_OK)
    {
        return ret;
    }

    switch (pkt.type)
    {
        case HTTPD_WS_TYPE_BINARY:
            ws_handle_binary(pkt.payload, pkt.len, fd);
            break;
        case HTTPD_WS_TYPE_TEXT:
        {
            pkt.payload[pkt.len] = '\0';
            if (strncmp((const char *)pkt.payload, "{\"t\":\"ping\"}", 13u) == 0)
            {
                (void)ws_send_ctl(fd, "{\"t\":\"pong\"}");
            }
            break;
        }
        case HTTPD_WS_TYPE_CLOSE:
            ws_sess_close(fd);
            break;
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
    return httpd_ws_send_frame_async(s_http.hd, sd, &pkt);
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
    esp_err_t err = httpd_ws_send_frame_async(s_http.hd, fd, &pkt);
    if (err == ESP_OK)
    {
        httpd_sess_update_lru_counter(s_http.hd, fd);
        return 0;
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
    char json[512];

    if (s_http.diag_fn != NULL)
    {
        s_http.diag_fn(json, sizeof(json));
        return send_json(req, 200, json);
    }
    return send_json(req, 200, "{\"err\":\"no diag provider\"}");
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
    (void)hd;
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
    ws_sess_close(sockfd);
}

/* ========================================================================== */
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

esp_err_t http_start(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t uris[] = {
        { .uri = "/",          .method = HTTP_GET,  .handler = assets_handler },
        { .uri = "/index.html",.method = HTTP_GET,  .handler = assets_handler },
        { .uri = "/app.js",    .method = HTTP_GET,  .handler = assets_handler },
        { .uri = "/style.css", .method = HTTP_GET,  .handler = assets_handler },
        { .uri = "/logo.svg",  .method = HTTP_GET,  .handler = assets_handler },
        { .uri = "/favicon.ico", .method = HTTP_GET, .handler = assets_handler },
        { .uri = "/api/health",.method = HTTP_GET,  .handler = api_health_handler },
        { .uri = "/api/diag",  .method = HTTP_GET,  .handler = api_diag_handler },
        { .uri = "/api/pair",  .method = HTTP_POST, .handler = api_pair_handler },
        { .uri = "/ota/c6",    .method = HTTP_POST, .handler = ota_upload_handler },
        { .uri = "/ota/tc275", .method = HTTP_POST, .handler = ota_upload_handler },
        { .uri = "/ws",        .method = HTTP_GET,  .handler = ws_handler,
          .is_websocket = true, .handle_ws_control_frames = true,
          .ws_pre_handshake_cb = ws_pre_handshake },
    };

    strncpy(s_http.fw_ver, app->version, sizeof(s_http.fw_ver) - 1u);
    cfg.max_open_sockets   = 8;
    cfg.stack_size         = 8192;
    cfg.lru_purge_enable   = true;
    cfg.recv_wait_timeout  = 10;
    cfg.send_wait_timeout  = 10;
    cfg.close_fn           = http_close_cb;

    ws_sessions_init();
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
            ESP_LOGW(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "httpd up (v%s)", s_http.fw_ver);
    return ESP_OK;
}
