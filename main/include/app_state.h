/*
 * app_state.h - global application state machine + diagnostics (LLDD 4.1/4.10)
 */
#ifndef C6_APP_STATE_H
#define C6_APP_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    APP_BOOT = 0,
    APP_FACTORY_WAIT,        /* no factory data, DEV_OVERRIDE off (LLDD 4.1) */
    APP_NET_START,
    APP_ONLINE,              /* AP up; LINK may still be negotiating          */
} app_state_t;

typedef struct
{
    uint32_t heap_min;                       /* since boot                  */
    uint16_t crc_errs;                       /* LINK CRC errors (lifetime)  */
    uint16_t fmt_errs;
    uint32_t frames_rx;
    uint32_t frames_tx;
    uint32_t tx_busy;
    uint32_t baud;
    int32_t  rtt_ms;
    bool     link_up;
    uint8_t  reset_reason;                   /* esp_reset_reason()          */
    uint8_t  self_check;
    bool     coredump_present;
    bool     factory_mode;
    uint32_t uptime_s;
} app_diag_t;

/* diagnostics snapshot used by /api/diag (JSON rendered in app_state.c) */
void app_diag_snapshot(app_diag_t *out);
void app_diag_render(char *json, size_t cap);

app_state_t app_state_get(void);
const char *app_state_name(void);
void app_state_enter(app_state_t st);

/* call once from app_main before anything else */
void app_state_init(bool factory_mode);

/* set self-check result used for rollback confirmation */
void app_state_set_self_check(int ok);

#ifdef __cplusplus
}
#endif

#endif /* C6_APP_STATE_H */
