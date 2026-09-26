/*
 * app_state.c - application state machine, heap guard, diag aggregation
 */
#include "include/app_state.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "link.h"
#include "ota_self.h"
#include "pair.h"

static const char *TAG = "c6_app";

#define HEAP_GUARD_MIN_FREE (64u * 1024u)     /* LLDD 4.10 */

typedef struct
{
    app_state_t state;
    int         self_check;
    bool        factory_mode;
    uint32_t    heap_min;
    esp_timer_handle_t guard_timer;
    esp_timer_handle_t rollback_timer;
} app_ctx_t;

static app_ctx_t s_app;

app_state_t app_state_get(void)
{
    return s_app.state;
}

const char *app_state_name(void)
{
    switch (s_app.state)
    {
        case APP_BOOT:         return "boot";
        case APP_FACTORY_WAIT: return "factory_wait";
        case APP_NET_START:    return "net_start";
        case APP_ONLINE:       return "online";
        default:               return "?";
    }
}

void app_state_set_self_check(int ok)
{
    s_app.self_check = ok ? 1 : 0;
}

/* ---- heap guard (10 s sampling, LLDD 4.10) ---------------------------------- */

static void guard_timer_cb(void *arg)
{
    (void)arg;
    uint32_t now_min = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);

    if (now_min < s_app.heap_min)
    {
        s_app.heap_min = now_min;
    }
    if (s_app.heap_min < HEAP_GUARD_MIN_FREE)
    {
        ESP_LOGW(TAG, "heap watermark low: %u", (unsigned)s_app.heap_min);
    }
}

/* ---- deferred rollback confirmation (LLDD 4.1 / 4.7 / 4.10) ------------------- */

static bool self_check_pass(void)
{
    /* minimal self-check set: NVS was readable (factory_init ok), heap above
     * the watermark, TWDT armed (link/bridge subscribed) - LLDD 4.10 */
    return (s_app.heap_min >= HEAP_GUARD_MIN_FREE) &&
           (s_app.self_check != 0);
}

static void rollback_timer_cb(void *arg)
{
    (void)arg;
    if (!link_is_up())
    {
        /* no LINK 45 s after boot: page still usable, but do not confirm a
         * pending-verify image without the LINK handshake check */
        ESP_LOGW(TAG, "rollback check: LINK not up in time");
        return;
    }
    (void)ota_self_confirm_rollback(self_check_pass() ? 1 : 0);
}

/* ---- diagnostics ---------------------------------------------------------------- */

void app_diag_snapshot(app_diag_t *out)
{
    link_health_t lh;

    memset(out, 0, sizeof(*out));
    link_get_health(&lh);
    out->heap_min   = s_app.heap_min;
    out->crc_errs   = lh.crc_errs;
    out->fmt_errs   = lh.fmt_errs;
    out->frames_rx  = lh.frames_rx;
    out->frames_tx  = lh.frames_tx;
    out->tx_busy    = lh.tx_busy;
    out->baud       = lh.baud;
    out->rtt_ms     = (int32_t)lh.rtt_ms;
    out->link_up    = lh.state == LINK_UP;
    out->reset_reason = (uint8_t)esp_reset_reason();
    out->self_check = (uint8_t)s_app.self_check;
    out->factory_mode = s_app.factory_mode;
    out->uptime_s   = (uint32_t)(esp_timer_get_time() / 1000000ull);

    const esp_partition_t *cd = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                         ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
                                                         "coredump");
    if (cd != NULL)
    {
        size_t addr = 0;
        size_t sz = 0;
        out->coredump_present =
            (esp_core_dump_image_get(&addr, &sz) == ESP_OK) && (sz > 0u);
    }
}

void app_diag_render(char *json, size_t cap)
{
    app_diag_t d;

    app_diag_snapshot(&d);
    (void)snprintf(json, cap,
        "{\"ver\":\"%s\",\"state\":\"%s\",\"slot\":\"%s\",\"factory\":%s,"
        "\"reset\":%u,\"selfcheck\":%u,\"coredump\":%s,\"heap_min\":%u,"
        "\"link\":{\"up\":%s,\"baud\":%u,\"rtt\":%u,\"crc_err\":%u,"
        "\"fmt_err\":%u,\"rx\":%u,\"tx\":%u,\"busy\":%u},"
        "\"pair\":\"%s\",\"uptime_s\":%u}",
        esp_app_get_description()->version,
        app_state_name(),
        esp_ota_get_running_partition()->label,
        d.factory_mode ? "true" : "false",
        d.reset_reason,
        d.self_check,
        d.coredump_present ? "true" : "false",
        (unsigned)d.heap_min,
        d.link_up ? "true" : "false",
        (unsigned)d.baud,
        (unsigned)d.rtt_ms,
        d.crc_errs, d.fmt_errs,
        (unsigned)d.frames_rx, (unsigned)d.frames_tx, (unsigned)d.tx_busy,
        (pair_state() == PAIR_CLAIMED) ? "claimed" :
        ((pair_state() == PAIR_OPEN) ? "open" : "idle"),
        (unsigned)d.uptime_s);
}

/* ---- transitions ------------------------------------------------------------------ */

void app_state_enter(app_state_t st)
{
    s_app.state = st;
    ESP_LOGI(TAG, "app state -> %s", app_state_name());
}

void app_state_init(bool factory_mode)
{
    const esp_timer_create_args_t gargs = {
        .callback = guard_timer_cb,
        .name     = "heap_guard",
    };
    const esp_timer_create_args_t rargs = {
        .callback = rollback_timer_cb,
        .name     = "rollback_chk",
    };

    memset(&s_app, 0, sizeof(s_app));
    s_app.state        = APP_BOOT;
    s_app.factory_mode = factory_mode;
    s_app.self_check   = 1;                   /* optimistic; events may clear */
    s_app.heap_min     = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);

    (void)esp_timer_create(&gargs, &s_app.guard_timer);
    (void)esp_timer_start_periodic(s_app.guard_timer, 10ull * 1000000ull);
    (void)esp_timer_create(&rargs, &s_app.rollback_timer);
    (void)esp_timer_start_once(s_app.rollback_timer, 45ull * 1000000ull);
}
