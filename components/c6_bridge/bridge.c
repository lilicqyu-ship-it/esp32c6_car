/*
 * bridge.c - command pump / telemetry broadcaster / OTA relay pump (LLDD 4.6)
 */
#include "bridge.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "http_server.h"
#include "link.h"
#include "pair.h"
#include "proto_frames.h"

static const char *TAG = "c6_bridge";

#define BRIDGE_TASK_STACK  6144u
#define BRIDGE_TASK_PRIO   10u
#define BRIDGE_TICK_MS     20u
#define CMD_QUEUE_LEN      32            /* LLDD 2.4 */
#define RELAY_CHUNK_SIZE   240u          /* SF OTA_D/0x31 {u16 idx, data[<=240]} (22 §5.5) */
#define RELAY_WINDOW       8u            /* 8 x 62 B in flight  */
#define RELAY_ACK_TMO_MS   2000u
#define RELAY_MAX_RESEND   1u

typedef struct
{
    proto_frame_t f;
    int sd;
} cmd_msg_t;

typedef struct
{
    uint16_t idx;
    uint16_t len;
    uint8_t  data[RELAY_CHUNK_SIZE];
} relay_chunk_t;

typedef struct
{
    /* command pump */
    QueueHandle_t q_cmd;

    /* telemetry mailbox (single slot, newest wins - decision D1) */
    SemaphoreHandle_t mbox_mtx;
    proto_telemetry_t mbox;
    bool              mbox_fresh;

    /* page status cache */
    uint8_t  car_state;
    uint16_t fault_code;

    /* OTA relay pump */
    SemaphoreHandle_t relay_mtx;
    SemaphoreHandle_t credit;                /* counting, init RELAY_WINDOW */
    bool     relay_active;
    int      relay_sd;
    uint32_t relay_total;
    uint32_t relay_acked;                    /* chunks acknowledged */
    uint32_t relay_chunk_count;
    uint16_t relay_next_idx;
    uint16_t relay_resend;
    uint32_t relay_last_progress_ms;
    uint32_t relay_crc;                      /* reserved: whole-image crc */
    relay_chunk_t inflight[RELAY_WINDOW];    /* resend ring */
    uint16_t inflight_head;
    uint16_t inflight_count;

    volatile bool link_up;
    uint8_t  last_link_state_sent;
    bool     tc_on_sent;                     /* last "car online" pushed to page */
    bool     tc_on_valid;                    /* false until the first push       */
} bridge_ctx_t;

static bridge_ctx_t s_br;
static TaskHandle_t s_bridge_task;

/* ---- LINK_STATE (0x42) - only C6 sends it, event driven (LLDD 3.1) ---------- */

static void bridge_send_link_state(void)
{
    proto_frame_t f;
    uint8_t st;

    if (ws_controller_sd() >= 0)
    {
        st = 2u;                             /* controller online */
    }
    else if (ws_client_count() > 0)
    {
        st = 1u;                             /* spectators only */
    }
    else
    {
        st = 0u;                             /* no client */
    }
    if (st == s_br.last_link_state_sent)
    {
        return;
    }
    s_br.last_link_state_sent = st;

    f.ver = PROTO_VER;
    f.cmd = PROTO_CMD_LINK_STATE;
    f.seq = 0u;
    f.len = 1u;
    f.data[0] = st;
    if (link_send(&f) != ESP_OK)
    {
        /* keep the edge alive so the 20 ms tick / next change retries it */
        s_br.last_link_state_sent = 0xFFu;
    }
}

/* A WS session changed (new client connected, or one left). The hello frame
 * always tells the page "tc: down" (http_server has no link visibility), so a
 * phone that connects AFTER the link is already up would otherwise stay grey
 * until the next link edge - which may never come on a stable link. Broadcast
 * the current car-online state now so a freshly connected page turns green
 * immediately. Idempotent for clients that already had the right state. */
void bridge_notify_clients(void)
{
    bridge_send_link_state();
    http_broadcast_ctl(link_is_up() ? "{\"t\":\"tc\",\"on\":true}"
                                     : "{\"t\":\"tc\",\"on\":false}");
}

void bridge_notify_pair(void)
{
    bridge_send_link_state();
}

bool bridge_link_up(void)
{
    return s_br.link_up;
}

const proto_telemetry_t *bridge_telemetry_snapshot(void)
{
    return s_br.mbox_fresh ? &s_br.mbox : NULL;
}

/* ---- producers ---------------------------------------------------------------*/

esp_err_t bridge_post_cmd(const proto_frame_t *f, int sd)
{
    cmd_msg_t m;

    if (f == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_br.q_cmd == NULL)
    {
        return ESP_ERR_INVALID_STATE;           /* bridge_start failed */
    }
    m.f  = *f;
    m.sd = sd;
    if (xQueueSend(s_br.q_cmd, &m, 0) != pdTRUE)
    {
        if (f->cmd != PROTO_CMD_DRIVE)
        {
            return ESP_ERR_NO_MEM;              /* busy -> WS error, no drop */
        }
        /* DRIVE is periodic newest-wins (30 Hz joystick, doubles as heartbeat):
         * a pump stall behind a slow broadcast leaves the queue full of
         * superseded positions. Receiving the stale frame HERE is forbidden,
         * though: q_cmd is a queue-set member and a receive outside the
         * select flow leaves a phantom entry in the set - the set's count
         * drifts up until prvNotifyQueueSetContainer asserts and reboots the
         * chip mid-drive (coredump 09-27: queue.c:3362). Drop the NEW frame
         * instead - the bridge pump is only milliseconds behind and the next
         * joystick sample is 33 ms away, so nothing observable is lost. */
        if (f->cmd != PROTO_CMD_DRIVE)
        {
            return ESP_ERR_NO_MEM;              /* busy -> WS error, no drop */
        }
        return ESP_OK;
    }
    return ESP_OK;
}

void bridge_send_frame(const proto_frame_t *f)
{
    (void)link_send(f);
}

/* ---- relay pump internals ------------------------------------------------------
 * All relay_* state (incl. the resend ring) is touched under relay_mtx.
 * Lock order: relay_mtx -> link tx_mtx (never the reverse).  The 2 s credit
 * wait in feed runs OUTSIDE the lock so ACK processing can make progress.
 */

static void relay_lock(void)
{
    (void)xSemaphoreTake(s_br.relay_mtx, portMAX_DELAY);
}

static void relay_unlock(void)
{
    (void)xSemaphoreGive(s_br.relay_mtx);
}

static void relay_reset_nolock(void)
{
    s_br.relay_active      = false;
    s_br.relay_sd          = -1;
    s_br.relay_acked       = 0u;
    s_br.relay_chunk_count = 0u;
    s_br.relay_next_idx    = 0u;
    s_br.relay_resend      = 0u;
    s_br.inflight_head     = 0u;
    s_br.inflight_count    = 0u;
    if (s_br.credit != NULL)
    {
        while (uxSemaphoreGetCount(s_br.credit) < RELAY_WINDOW)
        {
            if (xSemaphoreGive(s_br.credit) != pdTRUE)
            {
                break;
            }
        }
    }
}

static void relay_abort_send(void)
{
    proto_frame_t f;

    if (!s_br.relay_active)
    {
        return;
    }
    f.ver = PROTO_VER;
    f.cmd = PROTO_CMD_OTA_ABORT;
    f.seq = 0u;
    f.len = 0u;
    (void)link_send(&f);
    ESP_LOGW(TAG, "ota relay aborted");
}

static void relay_send_chunk(const relay_chunk_t *c)
{
    /* SPI link fast path: SF OTA_D/0x31 {u16 idx, data[<=240]} (T5) */
    (void)link_send_ota_chunk(c->idx, c->data, c->len);
}

/* called with a fresh 0x62 ACK from the LINK (bridge task, no lock held) */
static void relay_on_ack(const proto_frame_t *f)
{
    uint16_t idx;
    uint32_t popped = 0u;
    bool complete = false;
    bool failed = false;

    if (f->len < 3u)
    {
        return;
    }
    idx = proto_get_u16(&f->data[0]);

    relay_lock();
    if (!s_br.relay_active)
    {
        relay_unlock();
        return;
    }
    if (f->data[2] != 0u)
    {
        ESP_LOGW(TAG, "TC275 chunk %u NAK (%u)", idx, f->data[2]);
        relay_abort_send();
        relay_reset_nolock();
        failed = true;
    }
    else if ((uint32_t)(idx + 1u) > s_br.relay_acked)
    {
        s_br.relay_acked = (uint32_t)idx + 1u;
        s_br.relay_last_progress_ms = (uint32_t)(esp_timer_get_time() / 1000);
        s_br.relay_resend = 0u;

        /* pop everything up to and including idx, then refill the window with
         * exactly one credit per popped chunk (cumulative ACKs included) */
        while ((s_br.inflight_count > 0u) &&
               (s_br.inflight[s_br.inflight_head].idx <= idx))
        {
            s_br.inflight_head =
                (uint16_t)((s_br.inflight_head + 1u) % RELAY_WINDOW);
            s_br.inflight_count--;
            popped++;
        }
        while (popped-- > 0u)
        {
            (void)xSemaphoreGive(s_br.credit);
        }
        complete = (s_br.relay_acked >= s_br.relay_chunk_count);
    }
    relay_unlock();

    if (failed)
    {
        return;
    }
    if (complete)
    {
        proto_frame_t r;
        r.ver = PROTO_VER;
        r.cmd = PROTO_CMD_OTA_STATUS;
        r.seq = 0u;
        r.len = 2u;
        r.data[0] = PROTO_OTA_STATE_DONE;
        r.data[1] = 100u;
        (void)link_send(&r);
        ESP_LOGI(TAG, "ota transfer complete (TC275 verifying+signing)");
    }
}

/* 0x63 STATUS from TC275 (programming progress / swap notice) */
static void relay_on_status(const proto_frame_t *f)
{
    char json[96];

    if (f->len < 2u)
    {
        return;
    }
    (void)snprintf(json, sizeof(json),
                   "{\"t\":\"otastatus\",\"state\":%u,\"pct\":%u}",
                   f->data[0], f->data[1]);
    http_broadcast_ctl(json);
    if (f->data[0] == PROTO_OTA_STATE_FAILED)
    {
        relay_lock();
        relay_reset_nolock();
        relay_unlock();
    }
}

/* 0x64 SWAP_REQ from TC275: it is about to reset into the new slot */
static void relay_on_swap(const proto_frame_t *f)
{
    (void)f;
    relay_lock();
    if (!s_br.relay_active)
    {
        relay_unlock();
        return;
    }
    relay_reset_nolock();
    relay_unlock();
    http_broadcast_ctl("{\"t\":\"otaswap\"}");
}

/* tick: resend on 2 s ACK silence (LLDD 4.6.3) */
static void relay_tick(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    bool abort = false;

    relay_lock();
    if (!s_br.relay_active)
    {
        relay_unlock();
        return;
    }
    if (!link_is_up())
    {
        relay_abort_send();
        relay_reset_nolock();
        relay_unlock();
        return;
    }
    if ((s_br.inflight_count > 0u) &&
        ((now - s_br.relay_last_progress_ms) > RELAY_ACK_TMO_MS))
    {
        if (s_br.relay_resend < RELAY_MAX_RESEND)
        {
            s_br.relay_resend++;
            s_br.relay_last_progress_ms = now;
            ESP_LOGW(TAG, "ACK timeout -> resend chunk %u",
                     s_br.inflight[s_br.inflight_head].idx);
            relay_send_chunk(&s_br.inflight[s_br.inflight_head]);
        }
        else
        {
            relay_abort_send();
            relay_reset_nolock();
            abort = true;
        }
    }
    relay_unlock();

    if (abort)
    {
        http_broadcast_ctl("{\"t\":\"otaerror\",\"e\":\"relay\"}");
    }
}

/* ---- upload sink (/ota/tc275, httpd context) ------------------------------------*/

esp_err_t ota_relay_begin(int sd, size_t total)
{
    proto_frame_t f;

    if (!link_is_up())
    {
        return ESP_ERR_INVALID_STATE;         /* "car not connected" */
    }
    if (xSemaphoreTake(s_br.relay_mtx, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_br.relay_active)
    {
        (void)xSemaphoreGive(s_br.relay_mtx);
        return ESP_ERR_INVALID_STATE;
    }
    s_br.relay_active      = true;
    s_br.relay_sd          = sd;
    s_br.relay_total       = (uint32_t)total;
    s_br.relay_chunk_count = ((uint32_t)total + RELAY_CHUNK_SIZE - 1u) / RELAY_CHUNK_SIZE;
    s_br.relay_acked       = 0u;
    s_br.relay_next_idx    = 0u;
    s_br.relay_resend      = 0u;
    s_br.relay_last_progress_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_br.relay_crc         = 0u;
    s_br.inflight_head     = 0u;
    s_br.inflight_count    = 0u;
    while (uxSemaphoreGetCount(s_br.credit) < RELAY_WINDOW)
    {
        (void)xSemaphoreGive(s_br.credit);
    }
    (void)xSemaphoreGive(s_br.relay_mtx);

    f.ver = PROTO_VER;
    f.cmd = PROTO_CMD_OTA_BEGIN;
    f.seq = 0u;
    f.len = 8u;
    proto_put_u32(&f.data[0], s_br.relay_total);
    proto_put_u32(&f.data[4], s_br.relay_crc);   /* reserved: whole-image crc */
    if (link_send(&f) != ESP_OK)
    {
        relay_lock();
        relay_reset_nolock();
        relay_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "ota relay begin: %" PRIu32 " B in %" PRIu32 " chunks",
             s_br.relay_total, s_br.relay_chunk_count);
    return ESP_OK;
}

esp_err_t ota_relay_feed(int sd, const uint8_t *chunk, size_t n)
{
    while (n > 0u)
    {
        relay_chunk_t msg;
        size_t sub = (n < RELAY_CHUNK_SIZE) ? n : RELAY_CHUNK_SIZE;

        /* credit window: pause reading HTTP here (LLDD 4.6.3) */
        if (xSemaphoreTake(s_br.credit, pdMS_TO_TICKS(RELAY_ACK_TMO_MS)) != pdTRUE)
        {
            relay_lock();
            relay_abort_send();
            relay_reset_nolock();
            relay_unlock();
            return ESP_ERR_TIMEOUT;
        }

        relay_lock();
        if (!s_br.relay_active || (sd != s_br.relay_sd))
        {
            relay_unlock();
            (void)xSemaphoreGive(s_br.credit);
            return ESP_ERR_INVALID_STATE;
        }
        memset(&msg, 0, sizeof(msg));
        msg.idx = s_br.relay_next_idx;
        msg.len = (uint16_t)sub;
        memcpy(msg.data, chunk, sub);
        relay_send_chunk(&msg);
        s_br.inflight[(s_br.inflight_head + s_br.inflight_count) % RELAY_WINDOW] = msg;
        s_br.inflight_count++;
        s_br.relay_next_idx = (uint16_t)(msg.idx + 1u);
        s_br.relay_last_progress_ms = (uint32_t)(esp_timer_get_time() / 1000);
        relay_unlock();

        chunk += sub;
        n     -= sub;
    }
    return ESP_OK;
}

esp_err_t ota_relay_finish(int sd, char *json, size_t cap)
{
    int64_t t0 = esp_timer_get_time() / 1000;
    bool ok;
    uint32_t acked = 0u, chunks = 0u;

    if (!s_br.relay_active || (sd != s_br.relay_sd))
    {
        return ESP_ERR_INVALID_STATE;
    }
    /* wait for TC275 to ACK everything (its STATUS/SWAP frames flow onward) */
    while ((s_br.relay_acked < s_br.relay_chunk_count) &&
           (((esp_timer_get_time() / 1000) - t0) < 30000))
    {
        if (!link_is_up() || !s_br.relay_active)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    relay_lock();
    ok = (s_br.relay_acked >= s_br.relay_chunk_count) && link_is_up() &&
         s_br.relay_active && (sd == s_br.relay_sd);
    acked  = s_br.relay_acked;
    chunks = s_br.relay_chunk_count;
    if (!ok)
    {
        relay_abort_send();
    }
    relay_reset_nolock();
    relay_unlock();

    if (json != NULL && cap > 0u)
    {
        (void)snprintf(json, cap,
                       "{\"ok\":%s,\"acked\":%" PRIu32 ",\"total_chunks\":%" PRIu32 "}",
                       ok ? "true" : "false", acked, chunks);
    }
    return ok ? ESP_OK : ESP_FAIL;
}

void ota_relay_abort(int sd)
{
    relay_lock();
    if (s_br.relay_active && (sd == s_br.relay_sd))
    {
        relay_abort_send();
        relay_reset_nolock();
    }
    relay_unlock();
}

/* ---- command pump ---------------------------------------------------------------*/

static void pump_command(const cmd_msg_t *m)
{
    if (link_send(&m->f) != ESP_OK)
    {
        (void)ws_send_ctl(m->sd, "{\"t\":\"err\",\"e\":\"link_busy\"}");
    }
}

/* ---- telemetry broadcaster --------------------------------------------------------*/

static void pump_telemetry_frame(const proto_frame_t *f)
{
    proto_telemetry_t t;

    if (proto_telemetry_decode(f->data, f->len, &t) == 0)
    {
        if (xSemaphoreTake(s_br.mbox_mtx, pdMS_TO_TICKS(5)) == pdTRUE)
        {
            s_br.mbox       = t;
            s_br.mbox_fresh = true;
            s_br.car_state  = t.state;
            s_br.fault_code = t.fault_code;
            (void)xSemaphoreGive(s_br.mbox_mtx);
        }
    }
}

static void broadcast_telemetry(void)
{
    proto_frame_t f;

    if (!s_br.mbox_fresh)
    {
        return;
    }
    if (xSemaphoreTake(s_br.mbox_mtx, pdMS_TO_TICKS(5)) != pdTRUE)
    {
        return;
    }
    f.ver = PROTO_VER;
    f.cmd = PROTO_CMD_TELEMETRY;              /* page decodes payload */
    f.seq = 0u;
    f.len = (uint8_t)proto_telemetry_encode(&s_br.mbox, f.data, PROTO_MAX_PAYLOAD);
    (void)xSemaphoreGive(s_br.mbox_mtx);
    (void)ws_broadcast_binary(&f);
}

/* ---- LINK events / frames -----------------------------------------------------------*/

static void pump_link_frame(const proto_frame_t *f)
{
    switch (f->cmd)
    {
        case PROTO_CMD_TELEMETRY:
            pump_telemetry_frame(f);
            break;
        case PROTO_CMD_PAIR:
            pair_on_frame(f);
            bridge_notify_pair();
            break;
        case PROTO_CMD_OTA_ACK:
            relay_on_ack(f);
            break;
        case PROTO_CMD_OTA_STATUS:
            relay_on_status(f);
            break;
        case PROTO_CMD_OTA_SWAP:
            relay_on_swap(f);
            break;
        case PROTO_CMD_DIAG:
            /* SF EVT / generic-ACK tunnel from the SPI link (22 §5.5) */
            if (f->len >= 1u)
            {
                char json[96];
                (void)snprintf(json, sizeof(json),
                               "{\"t\":\"evt\",\"cid\":%u,\"n\":%u}",
                               f->data[0], (unsigned)(f->len - 1u));
                http_broadcast_ctl(json);
            }
            break;
        case PROTO_CMD_LINK_STATE:
        default:
            break;                            /* handled in c6_link or ignored */
    }
}

/* Broadcast the "car online" state to the page and mirror LINK_STATE to the
 * TC275, but only on a real edge (tracked by s_br.tc_on_sent). Idempotent:
 * safe to call from both the event handler and the 20 ms reconcile tick. */
static void bridge_publish_car_online(bool on)
{
    s_br.link_up = on;
    s_br.last_link_state_sent = 0xFFu;      /* force LINK_STATE resend */
    bridge_send_link_state();

    if (s_br.tc_on_valid && (s_br.tc_on_sent == on))
    {
        return;                             /* page already knows this state */
    }
    s_br.tc_on_valid = true;
    s_br.tc_on_sent  = on;
    http_broadcast_ctl(on ? "{\"t\":\"tc\",\"on\":true}"
                          : "{\"t\":\"tc\",\"on\":false}");
}

/* Periodic reconcile (20 ms tick). The LINK_EV_UP event is edge-triggered and
 * fires exactly once; if the bridge task had not yet joined the event queue
 * when the link came up (a boot-order race between the two boards powering up
 * at different times), that single event is lost and the page stays grey while
 * telemetry actually flows. Reconciling against link_is_up() every tick makes
 * the "car online" light self-heal within one tick regardless of boot order or
 * a dropped event - state is derived, never assumed from a one-shot signal. */
static void bridge_reconcile_link_state(void)
{
    bool up = link_is_up();

    if (!s_br.tc_on_valid || (up != s_br.tc_on_sent))
    {
        if (!up && s_br.link_up)
        {
            /* falling edge also tears down any relay in flight */
            relay_lock();
            relay_abort_send();
            relay_reset_nolock();
            relay_unlock();
        }
        bridge_publish_car_online(up);
    }
}

static void pump_link_event(const link_event_t *ev)
{
    switch (ev->id)
    {
        case LINK_EV_UP:
            bridge_publish_car_online(true);
            break;
        case LINK_EV_DOWN:
            relay_lock();
            relay_abort_send();
            relay_reset_nolock();
            relay_unlock();
            bridge_publish_car_online(false);
            break;
        default:
            break;
    }
}

/* ---- bench heartbeat ---------------------------------------------------------*/
#if CONFIG_C6_BENCH_CTRL
/* CPU0 auto-stops when no HEARTBEAT arrives within 100 ms, and the page never
 * sends 0x21 (doc 6.2 has DRIVE double as the heartbeat - but the bench TC275
 * build refuses 0x50 before it could reach CPU0).  Inject 0x21 every 3rd tick
 * (60 ms) while the link is up; production (real DRIVE heartbeat) keeps this
 * off. */
static void bench_heartbeat_tick(void)
{
    static uint8_t div = 0u;
    proto_frame_t f;

    if (!link_is_up())
    {
        return;
    }
    if (++div < 3u)
    {
        return;
    }
    div = 0u;
    f.ver = PROTO_VER;
    f.cmd = PROTO_CMD_HEARTBEAT;
    f.seq = 0u;
    f.len = 0u;
    (void)link_send(&f);
}
#endif

/* ---- bridge task -----------------------------------------------------------------------*/

static void bridge_task(void *arg)
{
    QueueSetHandle_t set;
    QueueHandle_t member;

    (void)arg;
    /* the task publishes/withdraws its own handle: on a wiring failure it
     * self-deletes, and the 20 ms timer must never notify a stale TCB */
    s_bridge_task = xTaskGetCurrentTaskHandle();
    set = xQueueCreateSet(8u + CMD_QUEUE_LEN + LINK_RX_QUEUE_LEN);
    if (set == NULL)
    {
        ESP_LOGE(TAG, "queue set alloc failed");
        s_bridge_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    if ((xQueueAddToSet(s_br.q_cmd, set) != pdTRUE) ||
        (xQueueAddToSet(link_rx_queue(), set) != pdTRUE) ||
        (xQueueAddToSet(link_event_queue(), set) != pdTRUE))
    {
        ESP_LOGE(TAG, "queue set wiring failed");
        s_bridge_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    (void)esp_task_wdt_add(NULL);

    for (;;)
    {
        member = (QueueHandle_t)xQueueSelectFromSet(set, pdMS_TO_TICKS(BRIDGE_TICK_MS));
        if (member == s_br.q_cmd)
        {
            cmd_msg_t m;
            if (xQueueReceive(member, &m, 0) == pdTRUE)
            {
                pump_command(&m);
            }
        }
        else if (member == link_rx_queue())
        {
            proto_frame_t f;
            if (xQueueReceive(member, &f, 0) == pdTRUE)
            {
                pump_link_frame(&f);
            }
        }
        else if (member == link_event_queue())
        {
            link_event_t ev;
            if (xQueueReceive(member, &ev, 0) == pdTRUE)
            {
                pump_link_event(&ev);
            }
        }
        if (ulTaskNotifyTake(pdTRUE, 0) != 0u)
        {
            /* 20 ms pacing tick: mailbox drain + relay watchdog */
            broadcast_telemetry();
            relay_tick();
            bridge_reconcile_link_state();   /* self-heal "car online" (boot race) */
            bridge_send_link_state();   /* retry edges dropped earlier by BUSY */
#if CONFIG_C6_BENCH_CTRL
            bench_heartbeat_tick();
#endif
        }
        (void)esp_task_wdt_reset();
    }
}

/* 20 ms tick -> task notification (LLDD 2.3: esp_timer paces the broadcast) */
static void bridge_tick_timer_cb(void *arg)
{
    (void)arg;
    if (s_bridge_task != NULL)
    {
        (void)xTaskNotifyGive(s_bridge_task);
    }
}

esp_err_t bridge_start(void)
{
    esp_timer_handle_t tick_timer;
    const esp_timer_create_args_t args = {
        .callback = bridge_tick_timer_cb,
        .name     = "bridge_tick",
    };

    memset(&s_br, 0, sizeof(s_br));
    s_br.relay_sd = -1;
    s_bridge_task = NULL;
    s_br.q_cmd     = xQueueCreate(CMD_QUEUE_LEN, sizeof(cmd_msg_t));
    s_br.mbox_mtx  = xSemaphoreCreateMutex();
    s_br.relay_mtx = xSemaphoreCreateMutex();
    s_br.credit    = xSemaphoreCreateCounting(RELAY_WINDOW, RELAY_WINDOW);
    if ((s_br.q_cmd == NULL) || (s_br.mbox_mtx == NULL) ||
        (s_br.relay_mtx == NULL) || (s_br.credit == NULL))
    {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(bridge_task, "bridge", BRIDGE_TASK_STACK, NULL,
                    BRIDGE_TASK_PRIO, NULL) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    if (esp_timer_create(&args, &tick_timer) != ESP_OK)
    {
        return ESP_ERR_NO_MEM;
    }
    return esp_timer_start_periodic(tick_timer, BRIDGE_TICK_MS * 1000u);
}
