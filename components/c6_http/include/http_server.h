/*
 * http_server.h - httpd: static assets + WS + REST endpoints (LLDD 4.3)
 */
#ifndef C6_HTTP_SERVER_H
#define C6_HTTP_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "proto_frames.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ws_bin_cb_t)(const proto_frame_t *f, int sd);

/* Upload sink implemented by c6_bridge (TC275 relay) / c6_ota (self),
 * registered by app_main - keeps the star dependency rule (LLDD 2.2). */
typedef struct
{
    esp_err_t (*begin)(int sd, size_t total);
    esp_err_t (*feed)(int sd, const uint8_t *chunk, size_t n);
    esp_err_t (*finish)(int sd, char *json, size_t cap);
    void (*abort)(int sd);                 /* socket died / stream broken   */
} http_upload_sink_t;

/* Diag JSON provider, registered by app_state (keeps c6_http blind to link). */
typedef void (*http_diag_fn)(char *json, size_t cap);

esp_err_t http_start(void);

/* ---- called from bridge context ---- */
esp_err_t ws_broadcast_binary(const proto_frame_t *f);   /* paced, LLDD 4.3 */
esp_err_t ws_send_ctl(int sd, const char *json);         /* text frame      */
void      ws_on_binary(ws_bin_cb_t cb);                  /* command entry   */
int       ws_controller_sd(void);                        /* -1 = no ctrl    */
int       ws_client_count(void);                         /* ws sockets      */

/* Broadcast a text frame to every ws client (pair/ota state pages). */
void http_broadcast_ctl(const char *json);

/* ---- wiring (composition root = app_main) ---- */
void http_on_session_change(void (*cb)(void));           /* -> LINK_STATE   */
void http_register_upload_sink(const char *uri, const http_upload_sink_t *s);
void http_set_diag_provider(http_diag_fn fn);
/* {"t":"tcver"} text from any WS client -> cb (bridge_request_tcver). */
void http_on_tcver_request(void (*cb)(void));

/* JSON helper shared by handlers: true when the socket owns CTRL. */
bool http_sd_is_ctrl(int sd);

#ifdef __cplusplus
}
#endif

#endif /* C6_HTTP_SERVER_H */
