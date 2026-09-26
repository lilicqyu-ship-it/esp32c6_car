/*
 * bridge.h - the three pumps: command / telemetry / OTA relay (LLDD 4.6)
 *
 * bridge_task (prio 10) owns every state machine: it is the single context
 * that feeds LINK TX and WS broadcasts (LLDD 2.3).  Wiring performed here:
 *   - pair_set_output(bridge_send_frame)
 *   - net/http session-change hooks -> 0x42 LINK_STATE
 *   - /ota/tc275 upload sink -> credit-window relay pump
 */
#ifndef C6_BRIDGE_H
#define C6_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "proto_frames.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create queues + task + wire hooks (net/link/http callbacks registered by
 * app_main composition code call the bridge_post_* helpers below). */
esp_err_t bridge_start(void);

/* ---- producers (any context) ---- */

/* WS command entry: role/SEQ already checked by c6_http (first gate). */
esp_err_t bridge_post_cmd(const proto_frame_t *f, int sd);

/* LINK_STATE refresh trigger (client set changed). */
void bridge_notify_clients(void);

/* page status cache refresh trigger: pair state changed */
void bridge_notify_pair(void);

/* LINK TX path shared with c6_pair (registered via pair_set_output). */
void bridge_send_frame(const proto_frame_t *f);

/* Latest telemetry snapshot for first-screen / diag (NULL before first 0x41). */
const proto_telemetry_t *bridge_telemetry_snapshot(void);

/* car-side connection state for pages/diag */
bool bridge_link_up(void);

/* ---- /ota/tc275 upload sink (LLDD 4.6.3) ---- */
esp_err_t ota_relay_begin(int sd, size_t total);
esp_err_t ota_relay_feed(int sd, const uint8_t *chunk, size_t n);
esp_err_t ota_relay_finish(int sd, char *json, size_t cap);
void      ota_relay_abort(int sd);

#ifdef __cplusplus
}
#endif

#endif /* C6_BRIDGE_H */
