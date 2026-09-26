/*
 * link.c - LINK UART frame link implementation (LLDD 4.5)
 */
#include "link.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "sdkconfig.h"

static const char *TAG = "c6_link";

/* Kconfig (LLDD Q1: pins pending EE review, defaults here are candidates) */
#ifndef CONFIG_C6_LINK_TX_GPIO
#define CONFIG_C6_LINK_TX_GPIO 10
#endif
#ifndef CONFIG_C6_LINK_RX_GPIO
#define CONFIG_C6_LINK_RX_GPIO 11
#endif

#define LINK_UART_NUM          UART_NUM_1
#define LINK_RX_RING           4096                    /* LLDD 4.5 */
#define LINK_TX_RING           2048
#define LINK_EVT_QUEUE         20
#define LINK_HEALTH_PERIOD_MS  20
#define LINK_PING_EVERY_TICKS  5                       /* 100 ms    */
#define LINK_SILENT_TIMEOUT_MS 500
#define LINK_BAUD_PROBE_MS     (30u * 1000u)           /* 30 s clean window */
#define LINK_FALLBACK_ERRS     10

typedef struct
{
    uint8_t buf[PROTO_MAX_FRAME];
    uint16_t len;
} link_tx_msg_t;

typedef struct
{
    QueueHandle_t   q_rx;                               /* proto_frame_t    */
    QueueHandle_t   q_evt;                              /* link_event_t     */
    QueueHandle_t   q_tx;                               /* link_tx_msg_t    */
    SemaphoreHandle_t tx_mtx;
    proto_parser_t  parser;
    link_health_t   health;
    uint32_t        ticks;                              /* 20 ms health ticks */
    uint32_t        crc_err_window;                     /* sliding window    */
    uint32_t        last_ping_ms;
    bool            baud_req_pending;
    bool            up_reported;
    void (*tap)(const proto_frame_t *f);                /* optional mirror */
} link_ctx_t;

static link_ctx_t s_link;

static void link_post_event(link_event_id_t id, uint32_t baud);
static void link_handle_pong(const proto_frame_t *f);
static void link_handle_baud_frame(const proto_frame_t *f);

static void link_post_event(link_event_id_t id, uint32_t baud)
{
    link_event_t ev = { .id = id, .baud = baud };
    (void)xQueueSend(s_link.q_evt, &ev, 0);
}

static void link_note_rx_activity(void)
{
    s_link.health.last_rx_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (!s_link.up_reported)
    {
        s_link.up_reported   = true;
        s_link.health.state  = LINK_UP;
        link_post_event(LINK_EV_UP, 0u);
        ESP_LOGI(TAG, "LINK UP");
    }
}

static bool link_send_raw(const uint8_t *buf, size_t len)
{
    link_tx_msg_t msg;

    if (len > PROTO_MAX_FRAME)
    {
        return false;
    }
    memcpy(msg.buf, buf, len);
    msg.len = (uint16_t)len;
    return xQueueSend(s_link.q_tx, &msg, 0) == pdTRUE;
}

/* ---- tasks ------------------------------------------------------------------ */

static void link_tx_task(void *arg)
{
    link_tx_msg_t msg;

    (void)arg;
    for (;;)
    {
        if (xQueueReceive(s_link.q_tx, &msg, portMAX_DELAY) == pdTRUE)
        {
            (void)uart_write_bytes(LINK_UART_NUM, msg.buf, msg.len);
            s_link.health.frames_tx++;
        }
    }
}

static void link_rx_task(void *arg)
{
    proto_frame_t f;
    int n;
    const int chunk = 128;
    uint8_t buf[128];

    (void)arg;
    for (;;)
    {
        n = uart_read_bytes(LINK_UART_NUM, buf, chunk, pdMS_TO_TICKS(50));
        if (n <= 0)
        {
            continue;
        }
        for (int i = 0; i < n; i++)
        {
            proto_rx_ev_t ev = proto_parser_feed(&s_link.parser, buf[i], &f);
            switch (ev)
            {
                case PROTO_RX_FRAME:
                    s_link.health.frames_rx++;
                    /* any downlink frame refreshes the health window */
                    link_note_rx_activity();
                    if (s_link.tap != NULL)
                    {
                        s_link.tap(&f);
                    }
                    /* link-internal frames are consumed here, not bridged */
                    if (f.cmd == PROTO_CMD_PING)
                    {
                        link_handle_pong(&f);
                    }
                    else if (f.cmd == PROTO_CMD_BAUD)
                    {
                        link_handle_baud_frame(&f);
                    }
                    else
                    {
                        (void)xQueueSend(s_link.q_rx, &f, 0);
                    }
                    break;
                case PROTO_RX_CRC_ERR:
                    s_link.health.crc_errs++;
                    s_link.crc_err_window++;
                    break;
                case PROTO_RX_FMT_ERR:
                case PROTO_RX_VER_ERR:
                    s_link.health.fmt_errs++;
                    break;
                default:
                    break;
            }
        }
    }
}

/* ---- baud handshake (LLDD 3.1 / 4.5) ------------------------------------------ */

static void link_apply_baud(uint32_t baud)
{
    if (s_link.health.baud != baud)
    {
        (void)uart_set_baudrate(LINK_UART_NUM, (int)baud);
        s_link.health.baud = baud;
        ESP_LOGI(TAG, "baud -> %u", (unsigned)baud);
        link_post_event(LINK_EV_BAUD_CHANGED, baud);
    }
}

esp_err_t link_request_baud(uint32_t baud)
{
    proto_frame_t f;
    uint8_t out[PROTO_MAX_FRAME];
    size_t n;

    f.ver = PROTO_VER;
    f.cmd = PROTO_CMD_BAUD;
    f.seq = 0u;
    f.len = 5u;
    proto_put_u32(&f.data[0], baud);
    f.data[4] = PROTO_BAUD_OP_REQ;
    n = proto_encode(&f, out, sizeof(out));
    if (n == 0u)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_link.baud_req_pending = true;
    return link_send_raw(out, n) ? ESP_OK : ESP_ERR_NO_MEM;
}

static void link_handle_baud_frame(const proto_frame_t *f)
{
    if (f->len < 5u)
    {
        return;
    }
    uint32_t baud = proto_get_u32(&f->data[0]);
    uint8_t op    = f->data[4];

    if (op == PROTO_BAUD_OP_ACK)
    {
        if (s_link.baud_req_pending && (baud == LINK_BAUD_FAST || baud == LINK_BAUD_BASE))
        {
            s_link.baud_req_pending = false;
            s_link.crc_err_window   = 0u;
            link_apply_baud(baud);
        }
    }
    else if (op == PROTO_BAUD_OP_REQ)
    {
        /* TC275-initiated switch (e.g. its own fallback) - accept both steps */
        if ((baud == LINK_BAUD_FAST) || (baud == LINK_BAUD_BASE))
        {
            proto_frame_t r;
            uint8_t out[PROTO_MAX_FRAME];
            size_t n;

            link_apply_baud(baud);
            /* answer ACK so both sides switch deterministically */
            r.ver = PROTO_VER; r.cmd = PROTO_CMD_BAUD; r.seq = f->seq;
            r.len = 5u;
            proto_put_u32(&r.data[0], baud);
            r.data[4] = PROTO_BAUD_OP_ACK;
            n = proto_encode(&r, out, sizeof(out));
            if (n != 0u)
            {
                (void)link_send_raw(out, n);
            }
        }
    }
    else
    {
        /* NAK: stay at current rate */
        s_link.baud_req_pending = false;
    }
}

/* ---- health monitor (20 ms esp_timer, LLDD 4.5) -------------------------------- */

static void link_handle_pong(const proto_frame_t *f)
{
    if ((f->len >= 1u) && (f->data[0] == PROTO_PING_TYPE_PONG))
    {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        s_link.health.rtt_ms = now - s_link.last_ping_ms;
    }
}

static void link_health_timer_cb(void *arg)
{
    (void)arg;
    link_ctx_t *L = &s_link;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    L->ticks++;

    /* silence watchdog -> DOWN (500 ms, LLDD 4.5) */
    if (L->up_reported &&
        ((now - L->health.last_rx_ms) > LINK_SILENT_TIMEOUT_MS))
    {
        L->up_reported  = false;
        L->health.state = LINK_DOWN;
        L->health.baud  = LINK_BAUD_BASE;    /* peer resets to base on its side too */
        link_post_event(LINK_EV_DOWN, 0u);
        ESP_LOGW(TAG, "LINK DOWN (silent %u ms)", (unsigned)(now - L->health.last_rx_ms));
    }

    /* PING every 100 ms */
    if ((L->ticks % LINK_PING_EVERY_TICKS) == 0u)
    {
        proto_frame_t f;
        uint8_t out[PROTO_MAX_FRAME];
        f.ver = PROTO_VER; f.cmd = PROTO_CMD_PING; f.seq = (uint8_t)L->ticks; f.len = 1u;
        f.data[0] = PROTO_PING_TYPE_PING;
        if (proto_encode(&f, out, sizeof(out)) != 0u)
        {
            (void)link_send_raw(out, (size_t)PROTO_HEADER_LEN + 1u + 2u);
        }
        L->last_ping_ms = now;
    }

    /* error-rate fallback: >= 10 CRC errors -> drop one step */
    if (L->crc_err_window >= LINK_FALLBACK_ERRS)
    {
        L->crc_err_window = 0u;
        if (L->health.baud == LINK_BAUD_FAST)
        {
            ESP_LOGW(TAG, "CRC burst -> fallback to %u", LINK_BAUD_BASE);
            (void)link_request_baud(LINK_BAUD_BASE);
        }
    }

    /* clean-window upgrade: 30 s at base with no CRC error -> propose 2 Mbps */
    if ((L->health.baud == LINK_BAUD_BASE) && (L->health.state == LINK_UP) &&
        (L->crc_err_window == 0u) && (!L->baud_req_pending) &&
        (L->ticks >= (LINK_BAUD_PROBE_MS / LINK_HEALTH_PERIOD_MS)))
    {
        (void)link_request_baud(LINK_BAUD_FAST);
    }
}

/* ---- public API ------------------------------------------------------------------ */

esp_err_t link_send(const proto_frame_t *f)
{
    uint8_t out[PROTO_MAX_FRAME];
    size_t n;
    bool ok;

    if (f == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    n = proto_encode(f, out, sizeof(out));
    if (n == 0u)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_link.tx_mtx, pdMS_TO_TICKS(10)) != pdTRUE)
    {
        return ESP_ERR_NO_MEM;
    }
    ok = link_send_raw(out, n);
    if (!ok)
    {
        s_link.health.tx_busy++;
    }
    (void)xSemaphoreGive(s_link.tx_mtx);
    return ok ? ESP_OK : ESP_ERR_NO_MEM;
}

QueueHandle_t link_rx_queue(void)
{
    return s_link.q_rx;
}

QueueHandle_t link_event_queue(void)
{
    return s_link.q_evt;
}

void link_get_health(link_health_t *out)
{
    if (out == NULL)
    {
        return;
    }
    if (xSemaphoreTake(s_link.tx_mtx, pdMS_TO_TICKS(10)) == pdTRUE)
    {
        *out = s_link.health;
        s_link.health.crc_errs = 0u;      /* consumer-driven window reset */
        s_link.health.fmt_errs = 0u;
        (void)xSemaphoreGive(s_link.tx_mtx);
    }
    else
    {
        *out = s_link.health;
    }
}

bool link_is_up(void)
{
    return s_link.up_reported;
}

void link_set_tap(void (*tap)(const proto_frame_t *f))
{
    s_link.tap = tap;
}

esp_err_t link_init(void)
{
    esp_err_t err;
    uart_config_t cfg = { 0 };
    const esp_timer_create_args_t targs = {
        .callback = link_health_timer_cb,
        .name     = "link_health",
    };

    memset(&s_link, 0, sizeof(s_link));
    proto_parser_init(&s_link.parser);
    s_link.health.baud  = LINK_BAUD_BASE;
    s_link.health.state = LINK_DOWN;

    s_link.q_rx  = xQueueCreate(LINK_RX_QUEUE_LEN, sizeof(proto_frame_t));
    s_link.q_evt = xQueueCreate(8, sizeof(link_event_t));
    s_link.q_tx  = xQueueCreate(LINK_TX_QUEUE_LEN, sizeof(link_tx_msg_t));
    s_link.tx_mtx = xSemaphoreCreateMutex();
    if ((s_link.q_rx == NULL) || (s_link.q_evt == NULL) ||
        (s_link.q_tx == NULL) || (s_link.tx_mtx == NULL))
    {
        return ESP_ERR_NO_MEM;
    }

    cfg.baud_rate    = LINK_BAUD_BASE;
    cfg.data_bits    = UART_DATA_8_BITS;
    cfg.parity       = UART_PARITY_DISABLE;
    cfg.stop_bits    = UART_STOP_BITS_1;
    cfg.flow_ctrl    = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk   = UART_SCLK_DEFAULT;
    err = uart_param_config(LINK_UART_NUM, &cfg);
    if (err != ESP_OK)
    {
        return err;
    }
    err = uart_set_pin(LINK_UART_NUM, CONFIG_C6_LINK_TX_GPIO, CONFIG_C6_LINK_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK)
    {
        return err;
    }
    err = uart_driver_install(LINK_UART_NUM, LINK_RX_RING, LINK_TX_RING,
                              LINK_EVT_QUEUE, NULL, 0);
    if (err != ESP_OK)
    {
        return err;
    }

    if (xTaskCreate(link_rx_task, "link_rx", 4096, NULL, 12, NULL) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(link_tx_task, "link_tx", 3072, NULL, 11, NULL) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_timer_handle_t th;
    err = esp_timer_create(&targs, &th);
    if (err != ESP_OK)
    {
        return err;
    }
    return esp_timer_start_periodic(th, LINK_HEALTH_PERIOD_MS * 1000);
}
