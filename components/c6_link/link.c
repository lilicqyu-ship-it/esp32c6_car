/*
 * link.c - SPI half-duplex slave link (spi_slave_hd segment mode)
 *
 * Transaction model (myCar doc 22 §4):
 *   - The TC275 master owns the clock; this slave only loads data.
 *   - RX (host -> slave, master CMD3 WRDMA): we keep two 512 B DMA buffers
 *     queued; SF_RX_ROOM advertises the queued capacity.  Completed segments
 *     are parsed into SF frames, mapped to v2 frames and handed to the bridge.
 *   - TX (slave -> host, master CMD4 RDDMA): SF frames from the internal queue
 *     are assembled into one <=512 B segment (multi-frame, 4-byte padded);
 *     SF_TX_PENDING carries its length and the IRQ line signals readiness.
 *   - Registers (master CMD2 RDBUF / CMD1 WRBUF): the 6-word handshake block
 *     (22 §4.3) plus the GEN slot at offset 24 (master writes a 4-byte command
 *     there; we answer in SF_CMDRSP).
 *   - Liveness: every master transaction (any CMD) refreshes the watchdog.
 *     500 ms of silence -> LINK DOWN (22 §5.4).
 */
#include "link.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_slave_hd.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "proto_frames.h"
#include "sf_frame.h"
#include "sdkconfig.h"

static const char *TAG = "c6_link";

#ifndef CONFIG_C6_LINK_SPI_SCLK_GPIO
#define CONFIG_C6_LINK_SPI_SCLK_GPIO 19
#endif
#ifndef CONFIG_C6_LINK_SPI_MOSI_GPIO
#define CONFIG_C6_LINK_SPI_MOSI_GPIO 18
#endif
#ifndef CONFIG_C6_LINK_SPI_MISO_GPIO
#define CONFIG_C6_LINK_SPI_MISO_GPIO 20
#endif
#ifndef CONFIG_C6_LINK_SPI_CS_GPIO
#define CONFIG_C6_LINK_SPI_CS_GPIO   23
#endif
#ifndef CONFIG_C6_LINK_SPI_IRQ_GPIO
#define CONFIG_C6_LINK_SPI_IRQ_GPIO  21
#endif

#define LINK_SPI_HOST         SPI2_HOST
#define LINK_RX_BUFFERS       2u                     /* queued DMA RX segs  */
#define LINK_TASK_PRIO        12u
#define LINK_TASK_STACK       5120u
#define LINK_WATCHDOG_MS      500u                   /* 22 §5.4             */
#define LINK_LOST_CRC_RUN     5u                     /* 5 CRC fails -> lost */

typedef struct
{
    sf_frame_t f;
} link_tx_msg_t;

typedef struct
{
    QueueHandle_t     q_tx;                          /* link_tx_msg_t       */
    QueueHandle_t     q_rx;                          /* proto_frame_t       */
    QueueHandle_t     q_evt;                         /* link_event_t        */
    SemaphoreHandle_t tx_mtx;
    TaskHandle_t      task;
    void (*tap)(const proto_frame_t *f);

    /* SPI DMA buffers */
    uint8_t  rxBuf[LINK_RX_BUFFERS][LINK_SEG_SIZE];
    spi_slave_hd_data_t rxTrans[LINK_RX_BUFFERS];
    uint8_t  txBuf[LINK_SEG_SIZE];
    spi_slave_hd_data_t txTrans;
    bool     tx_in_flight;

    /* shared-register block (28 bytes, published via write_buffer) */
    uint8_t  regs[SF_REG_COUNT];
    uint32_t alive;
    uint32_t crc_errs_total, fmt_errs_total, seq_errs_total;
    uint32_t rxovfl, txovfl, trun;
    uint32_t crc_run;                                /* consecutive CRC err */
    uint32_t frames_tx_total;

    /* ISR -> task notification flags */
    volatile bool tx_done_notif;
    volatile bool rx_done_notif;
    volatile bool host_event_notif;
    volatile bool gen_notif;

    /* health */
    link_health_t health;
    uint32_t last_host_ev1, last_host_ev2;           /* poll cadence est.   */
    uint32_t host_ev_total;                          /* bench diag: any master transaction seen at all */
    /* bench diag: per-callback ISR counts, incremented in the ISR itself so
     * they are NOT lost to the bool-flag merge that host_ev_total suffers.
     * cb_tx = master RDBUF (reads our regs), cb_rx = master WRBUF (GEN slot),
     * sent = a queued TX DMA seg went out, recv = a queued RX DMA seg filled. */
    volatile uint32_t isr_buftx, isr_bufrx, isr_sent, isr_recv;
    uint8_t  seq_tx, seq_rx_last;                    /* per-direction SEQ   */
    bool     seq_synced;                             /* false until the first
                                                       master frame locks the
                                                       window (myCar's parser
                                                       has the same guard:
                                                       haveLastSeq)         */
    bool     up_reported;
    bool     silent;                                 /* GEN silence mode    */

    sf_parser_t parser;
} link_ctx_t;

static link_ctx_t s_link;

static void link_post_event(link_event_id_t id);
static void regs_publish(void);
static void link_tx_kick(void);

/* ================= register / IRQ helpers ================= */

static void regs_set_u32(int idx, uint32_t v)
{
    s_link.regs[idx]     = (uint8_t)(v & 0xFFu);
    s_link.regs[idx + 1] = (uint8_t)((v >> 8) & 0xFFu);
    s_link.regs[idx + 2] = (uint8_t)((v >> 16) & 0xFFu);
    s_link.regs[idx + 3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t regs_get_u32(int idx)
{
    return (uint32_t)s_link.regs[idx] |
           ((uint32_t)s_link.regs[idx + 1] << 8) |
           ((uint32_t)s_link.regs[idx + 2] << 16) |
           ((uint32_t)s_link.regs[idx + 3] << 24);
}

static void regs_publish(void)
{
    spi_slave_hd_write_buffer(LINK_SPI_HOST, 0, s_link.regs, SF_REG_COUNT);
}

static void irq_set(bool high)
{
    /* Open-drain: drive 0 = assert (pull low), drive 1 = release (high-Z).
     * The boards are jumper wires only with no external pull-up (22 §3.2 /
     * §9.3), so the released-high level is held by the TC275 P23.0 internal
     * pull-up alone - slow-riding and noisy. The master samples this level,
     * never counts edges, and never treats "high" as "slave present";
     * liveness is SF_ALIVE only. */
    gpio_set_level(CONFIG_C6_LINK_SPI_IRQ_GPIO, high ? 1u : 0u);
}

/* ================= liveness / events ================= */

static void link_note_host_event(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    s_link.last_host_ev2 = s_link.last_host_ev1;
    s_link.last_host_ev1 = now;
    s_link.health.last_rx_ms = now;
    s_link.host_ev_total++;

    if (!s_link.up_reported)
    {
        s_link.up_reported = true;
        s_link.health.state = LINK_UP;
        s_link.crc_run = 0u;
        link_post_event(LINK_EV_UP);
        ESP_LOGI(TAG, "LINK UP");
    }
}

static void link_post_event(link_event_id_t id)
{
    link_event_t ev = { .id = id, .reserved = 0u };
    (void)xQueueSend(s_link.q_evt, &ev, 0);
}

/* ================= v2 <-> SF mapping (22 §5.5) ================= */

/* v2 driving-family commands that ride SF CMD/CID_DRV as {u8 op, i16 v, i16 w} */
static bool v2_is_drv(uint8_t cmd)
{
    return (cmd == PROTO_CMD_DRIVE)   || (cmd == PROTO_CMD_STOP)       ||
           (cmd == PROTO_CMD_FORWARD) || (cmd == PROTO_CMD_BACKWARD)   ||
           (cmd == PROTO_CMD_LEFT)    || (cmd == PROTO_CMD_RIGHT)      ||
           (cmd == PROTO_CMD_FORWARD_LEFT) || (cmd == PROTO_CMD_FORWARD_RIGHT) ||
           (cmd == PROTO_CMD_ROTATE_LEFT)  || (cmd == PROTO_CMD_ROTATE_RIGHT) ||
           (cmd == PROTO_CMD_SET_SPEED)    || (cmd == PROTO_CMD_GET_STATUS)    ||
           (cmd == PROTO_CMD_HEARTBEAT)    || (cmd == PROTO_CMD_RESET)         ||
           (cmd == PROTO_CMD_CLEAR_FAULT)  || (cmd == PROTO_CMD_EMERGENCY_STOP);
}

static size_t v2_to_sf(const proto_frame_t *vf, sf_frame_t *sf)
{
    memset(sf, 0, sizeof(*sf));
    sf->seq   = s_link.seq_tx++;
    sf->flags = 0u;

    if (v2_is_drv(vf->cmd))
    {
        uint16_t v = 0u, w = 0u;

        if ((vf->cmd == PROTO_CMD_DRIVE || vf->cmd == PROTO_CMD_SET_SPEED) &&
            (vf->len >= 4u))
        {
            v = proto_get_u16(&vf->data[0]);
            w = proto_get_u16(&vf->data[2]);
        }
        sf->type = SF_TYPE_CMD;
        sf->cid  = SF_CID_DRV;
        sf->len  = 5u;
        sf->data[0] = vf->cmd;                       /* op = v2 command      */
        proto_put_u16(&sf->data[1], v);
        proto_put_u16(&sf->data[3], w);
        return 1u;
    }
    if (vf->cmd == PROTO_CMD_PAIR)
    {
        sf->type = SF_TYPE_CMD;  sf->cid = SF_CID_PAIR;
        sf->len  = vf->len;      memcpy(sf->data, vf->data, vf->len);
        return 1u;
    }
    if (vf->cmd == PROTO_CMD_CFG)
    {
        sf->type = SF_TYPE_CMD;  sf->cid = SF_CID_CFG;
        sf->len  = vf->len;      memcpy(sf->data, vf->data, vf->len);
        return 1u;
    }
    if ((vf->cmd == PROTO_CMD_DIAG) || (vf->cmd == PROTO_CMD_LINK_STATE))
    {
        sf->type = SF_TYPE_CMD;  sf->cid = SF_CID_DIAG;
        sf->len  = (uint16_t)(vf->len + 1u);
        sf->data[0] = vf->cmd;                       /* op: 0x53 diag / 0x42 linkstate */
        memcpy(&sf->data[1], vf->data, vf->len);
        return 1u;
    }
    if ((vf->cmd >= PROTO_CMD_DPT_ENTER) && (vf->cmd <= PROTO_CMD_DPT_SELFTEST))
    {
        sf->type = SF_TYPE_CMD;  sf->cid = SF_CID_DPT;
        sf->len  = (uint16_t)(vf->len + 1u);
        sf->data[0] = vf->cmd;
        memcpy(&sf->data[1], vf->data, vf->len);
        return 1u;
    }
    if (vf->cmd == PROTO_CMD_OTA_BEGIN)
    {
        sf->type = SF_TYPE_OTA_D;  sf->cid = SF_CID_OTA_BEGIN;
        sf->len  = vf->len;        memcpy(sf->data, vf->data, vf->len);
        return 1u;
    }
    if (vf->cmd == PROTO_CMD_OTA_ABORT)
    {
        sf->type = SF_TYPE_OTA_D;  sf->cid = SF_CID_OTA_ABORT;
        sf->len  = vf->len;        memcpy(sf->data, vf->data, vf->len);
        return 1u;
    }
    return 0u;    /* 0x61 CHUNK goes via link_send_ota_chunk; 0x62-0x64 are
                     host->slave only; 0x43/0x44 no longer exist (T2)       */
}

static bool sf_to_v2(const sf_frame_t *sf, proto_frame_t *vf)
{
    memset(vf, 0, sizeof(*vf));
    vf->ver = PROTO_VER;
    vf->seq = sf->seq;

    switch (sf->type)
    {
        case SF_TYPE_TEL:
            if ((sf->cid != SF_CID_TELEMETRY) || (sf->len < PROTO_TELEMETRY_LEN))
            {
                return false;
            }
            vf->cmd = PROTO_CMD_TELEMETRY;
            vf->len = (uint8_t)sf->len;
            memcpy(vf->data, sf->data, sf->len);
            return true;

        case SF_TYPE_OTA_C:
            if ((sf->cid == SF_CID_OTA_ACK) && (sf->len >= 3u))
            {
                vf->cmd = PROTO_CMD_OTA_ACK;  vf->len = 3u;
                memcpy(vf->data, sf->data, 3u);
                return true;
            }
            if ((sf->cid == SF_CID_OTA_STATUS) && (sf->len >= 2u))
            {
                vf->cmd = PROTO_CMD_OTA_STATUS;  vf->len = 2u;
                memcpy(vf->data, sf->data, 2u);
                return true;
            }
            if ((sf->cid == SF_CID_OTA_SWAP) && (sf->len == 0u))
            {
                vf->cmd = PROTO_CMD_OTA_SWAP;  vf->len = 0u;
                return true;
            }
            return false;

        case SF_TYPE_EVT:
            /* payload {u8 kind, ...}: kind 1 = pairing reply -> v2 0x51 */
            if ((sf->cid == SF_CID_EVT_STATE) && (sf->len >= 2u) &&
                (sf->data[0] == 0x01u))
            {
                vf->cmd = PROTO_CMD_PAIR;
                vf->len = (uint8_t)(sf->len - 1u);
                memcpy(vf->data, &sf->data[1], vf->len);
                return true;
            }
            /* error events (CID 0x20) and unknown kinds -> v2 DIAG tunnel */
            vf->cmd = PROTO_CMD_DIAG;
            vf->len = (uint8_t)(sf->len + 1u);
            vf->data[0] = sf->cid;
            memcpy(&vf->data[1], sf->data,
                   (sf->len <= (PROTO_MAX_PAYLOAD - 1u)) ? sf->len : (PROTO_MAX_PAYLOAD - 1u));
            return true;

        case SF_TYPE_ACK:
            /* generic ack (DPT commands) -> v2 DIAG tunnel */
            vf->cmd = PROTO_CMD_DIAG;
            vf->len = (uint8_t)(sf->len + 1u);
            vf->data[0] = SF_TYPE_ACK;
            memcpy(&vf->data[1], sf->data,
                   (sf->len <= (PROTO_MAX_PAYLOAD - 1u)) ? sf->len : (PROTO_MAX_PAYLOAD - 1u));
            return true;

        case SF_TYPE_HBT:
            return false;                            /* liveness only        */

        case SF_TYPE_CMD:
        case SF_TYPE_OTA_D:
        case SF_TYPE_DBG:
        case SF_TYPE_VND:
        default:
            return false;                            /* not our direction    */
    }
}

/* ================= TX segment assembly ================= */

static void link_tx_kick(void)
{
    size_t used = 0u;
    link_tx_msg_t msg;

    if (s_link.tx_in_flight)
    {
        return;
    }
    if (uxQueueMessagesWaiting(s_link.q_tx) == 0u)
    {
        return;
    }

    while (xQueueReceive(s_link.q_tx, &msg, 0) == pdTRUE)
    {
        uint8_t wire[SF_MAX_FRAME];
        size_t n = sf_encode(&msg.f, wire, sizeof(wire));
        if (n == 0u)
        {
            s_link.txovfl++;                          /* malformed -> drop   */
            continue;
        }
        if (used + n > LINK_SEG_SIZE)
        {
            (void)xQueueSendToFront(s_link.q_tx, &msg, 0);   /* refit next seg */
            break;
        }
        memcpy(&s_link.txBuf[used], wire, n);
        used += n;
        if (used + (size_t)SF_MIN_FRAME > LINK_SEG_SIZE)
        {
            break;                                    /* no room for another */
        }
    }
    if (used == 0u)
    {
        return;
    }
    used = sf_segment_pad(s_link.txBuf, used);

    memset(&s_link.txTrans, 0, sizeof(s_link.txTrans));
    s_link.txTrans.data  = s_link.txBuf;
    s_link.txTrans.len   = used;
    s_link.txTrans.flags = SPI_SLAVE_HD_TRANS_DMA_BUFFER_ALIGN_AUTO;
    if (spi_slave_hd_queue_trans(LINK_SPI_HOST, SPI_SLAVE_CHAN_TX,
                                 &s_link.txTrans, 0) == ESP_OK)
    {
        s_link.tx_in_flight = true;
        regs_set_u32(SF_REG_TX_PENDING, (uint32_t)used);
        regs_publish();
        if (!s_link.silent)
        {
            irq_set(true);
        }
    }
    else
    {
        s_link.txovfl++;
        regs_set_u32(SF_REG_ERRSTAT, regs_get_u32(SF_REG_ERRSTAT) | SF_ERR_TXOVFL);
        regs_publish();
    }
}

/* ================= RX segment handling ================= */

static void link_handle_rx_segment(const uint8_t *data, size_t len)
{
    sf_frame_t sf;
    proto_frame_t vf;

    for (size_t i = 0u; i < len; i++)
    {
        sf_rx_ev_t ev = sf_parser_feed(&s_link.parser, data[i], &sf);
        switch (ev)
        {
            case SF_RX_FRAME:
                s_link.crc_run = 0u;
                if (!s_link.seq_synced)
                {
                    /* First frame of the stream (boot, or after GEN reset):
                     * the master's counter keeps running across our resets, so
                     * there is no meaningful window yet - lock onto this seq
                     * instead of rejecting seq==last as a replay. */
                    s_link.seq_rx_last = sf.seq;
                    s_link.seq_synced  = true;
                }
                else if (!sf_seq_ok(sf.seq, &s_link.seq_rx_last))
                {
                    s_link.seq_errs_total++;
                    s_link.health.seq_errs++;
                    regs_set_u32(SF_REG_ERRSTAT,
                                 regs_get_u32(SF_REG_ERRSTAT) | SF_ERR_SEQ);
                    regs_publish();
                    break;
                }
                if (sf_to_v2(&sf, &vf))
                {
                    s_link.health.frames_rx++;
                    if (s_link.tap != NULL)
                    {
                        s_link.tap(&vf);
                    }
                    if (xQueueSend(s_link.q_rx, &vf, 0) != pdTRUE)
                    {
                        s_link.rxovfl++;
                        regs_set_u32(SF_REG_ERRSTAT,
                                     regs_get_u32(SF_REG_ERRSTAT) | SF_ERR_RXOVFL);
                        regs_publish();
                    }
                }
                break;

            case SF_RX_CRC_ERR:
                s_link.crc_errs_total++;
                s_link.health.crc_errs++;
                s_link.crc_run++;
                regs_set_u32(SF_REG_ERRSTAT, regs_get_u32(SF_REG_ERRSTAT) | SF_ERR_CRC);
                regs_publish();
                if (s_link.crc_run >= LINK_LOST_CRC_RUN)
                {
                    s_link.crc_run = 0u;
                    if (s_link.up_reported)
                    {
                        s_link.up_reported = false;
                        s_link.health.state = LINK_DOWN;
                        link_post_event(LINK_EV_DOWN);
                        ESP_LOGW(TAG, "LINK DOWN (CRC burst)");
                    }
                }
                break;

            case SF_RX_FMT_ERR:
            case SF_RX_VER_ERR:
                s_link.fmt_errs_total++;
                s_link.health.fmt_errs++;
                regs_set_u32(SF_REG_ERRSTAT, regs_get_u32(SF_REG_ERRSTAT) | SF_ERR_FMT);
                regs_publish();
                break;

            default:
                break;
        }
    }
}

/* re-queue one RX DMA buffer; RX_ROOM = queued capacity */
static void link_rx_requeue(int slot)
{
    memset(&s_link.rxTrans[slot], 0, sizeof(s_link.rxTrans[slot]));
    s_link.rxTrans[slot].data  = s_link.rxBuf[slot];
    s_link.rxTrans[slot].len   = LINK_SEG_SIZE;
    s_link.rxTrans[slot].arg   = (void *)(intptr_t)slot;
    s_link.rxTrans[slot].flags = SPI_SLAVE_HD_TRANS_DMA_BUFFER_ALIGN_AUTO;
    if (spi_slave_hd_queue_trans(LINK_SPI_HOST, SPI_SLAVE_CHAN_RX,
                                 &s_link.rxTrans[slot], 0) == ESP_OK)
    {
        regs_set_u32(SF_REG_RX_ROOM, regs_get_u32(SF_REG_RX_ROOM) + LINK_SEG_SIZE);
        regs_publish();
    }
}

/* ================= GEN commands (master WRBUF -> SF_REG_GEN) ================= */

static void link_handle_gen(void)
{
    uint8_t cmd[4];

    spi_slave_hd_read_buffer(LINK_SPI_HOST, SF_REG_GEN, cmd, 4u);
    uint32_t payload = (uint32_t)cmd[1] | ((uint32_t)cmd[2] << 8) |
                       ((uint32_t)cmd[3] << 16);
    uint8_t result = SF_GEN_RSP_OK;

    switch (cmd[0])
    {
        case SF_GEN_RESET_LINK:
            sf_parser_init(&s_link.parser);
            s_link.seq_rx_last = 0u;
            s_link.seq_synced  = false;
            break;
        case SF_GEN_SILENCE_ON:
            s_link.silent = true;
            irq_set(false);
            break;
        case SF_GEN_SILENCE_OFF:
            s_link.silent = false;
            irq_set(s_link.tx_in_flight ||
                    (uxQueueMessagesWaiting(s_link.q_tx) > 0u));
            break;
        case SF_GEN_CLOCK_SET:
            /* clock is a master-side property; record for diag */
            s_link.health.clock_hz = payload * 1000000u;
            break;
        default:
            result = SF_GEN_RSP_UNKNOWN;
            break;
    }
    regs_set_u32(SF_REG_CMDRSP, (uint32_t)cmd[0] | ((uint32_t)result << 8));
    regs_publish();
}

/* ================= ISR callbacks ================= */

static bool IRAM_ATTR cb_sent(void *arg, spi_slave_hd_event_t *e, int *awoken)
{
    link_ctx_t *L = (link_ctx_t *)arg;
    BaseType_t hpw = pdFALSE;

    (void)e;
    L->isr_sent++;
    L->tx_done_notif = true;
    L->host_event_notif = true;
    vTaskNotifyGiveFromISR(L->task, &hpw);
    *awoken = (hpw == pdTRUE);
    return true;
}

static bool IRAM_ATTR cb_recv(void *arg, spi_slave_hd_event_t *e, int *awoken)
{
    link_ctx_t *L = (link_ctx_t *)arg;
    BaseType_t hpw = pdFALSE;

    (void)e;
    L->isr_recv++;
    L->rx_done_notif = true;
    L->host_event_notif = true;
    vTaskNotifyGiveFromISR(L->task, &hpw);
    *awoken = (hpw == pdTRUE);
    return true;
}

static bool IRAM_ATTR cb_buffer_tx(void *arg, spi_slave_hd_event_t *e, int *awoken)
{
    /* master read our registers (CMD2/RDBUF) - counts as host activity */
    link_ctx_t *L = (link_ctx_t *)arg;
    BaseType_t hpw = pdFALSE;

    (void)e;
    L->isr_buftx++;
    L->host_event_notif = true;
    vTaskNotifyGiveFromISR(L->task, &hpw);
    *awoken = (hpw == pdTRUE);
    return true;
}

static bool IRAM_ATTR cb_buffer_rx(void *arg, spi_slave_hd_event_t *e, int *awoken)
{
    /* master wrote a register (CMD1/WRBUF) - the GEN slot lives there */
    link_ctx_t *L = (link_ctx_t *)arg;
    BaseType_t hpw = pdFALSE;

    (void)e;
    L->isr_bufrx++;
    L->host_event_notif = true;
    L->gen_notif = true;
    vTaskNotifyGiveFromISR(L->task, &hpw);
    *awoken = (hpw == pdTRUE);
    return true;
}

/* ================= link task ================= */

static void link_task(void *arg)
{
    link_ctx_t *L = &s_link;
    spi_slave_hd_data_t *done;
    TickType_t last_1s = xTaskGetTickCount();

    (void)arg;
    (void)esp_task_wdt_add(NULL);

    for (int i = 0; i < (int)LINK_RX_BUFFERS; i++)
    {
        link_rx_requeue(i);
    }
    regs_set_u32(SF_REG_READY, SF_READY_MAGIC);
    regs_publish();

    for (;;)
    {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));  /* health fallback */

        if (L->host_event_notif)
        {
            L->host_event_notif = false;
            link_note_host_event();
        }

        if (L->gen_notif)
        {
            L->gen_notif = false;
            link_handle_gen();
        }

        if (L->tx_done_notif)
        {
            L->tx_done_notif = false;
            if (spi_slave_hd_get_trans_res(LINK_SPI_HOST, SPI_SLAVE_CHAN_TX,
                                           &done, 0) == ESP_OK)
            {
                L->tx_in_flight = false;
                regs_set_u32(SF_REG_TX_PENDING, 0u);
                regs_publish();
                if (!L->silent)
                {
                    irq_set(false);
                }
            }
        }

        while (L->rx_done_notif &&
               (spi_slave_hd_get_trans_res(LINK_SPI_HOST, SPI_SLAVE_CHAN_RX,
                                           &done, 0) == ESP_OK))
        {
            int slot = (int)(intptr_t)done->arg;
            size_t n = done->trans_len;
            if (n > LINK_SEG_SIZE)
            {
                n = LINK_SEG_SIZE;
            }
            if (done->len <= regs_get_u32(SF_REG_RX_ROOM))
            {
                regs_set_u32(SF_REG_RX_ROOM,
                             regs_get_u32(SF_REG_RX_ROOM) - done->len);
            }
            else
            {
                regs_set_u32(SF_REG_RX_ROOM, 0u);
            }
            regs_publish();
            link_handle_rx_segment(L->rxBuf[slot], n);
            link_rx_requeue(slot);
            if (uxQueueMessagesWaiting(L->q_rx) >= (UBaseType_t)LINK_RX_QUEUE_LEN)
            {
                break;                                   /* bridge is behind  */
            }
        }

        link_tx_kick();

        /* health watchdog (500 ms, 22 §5.4) */
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        if (L->up_reported &&
            ((now - L->health.last_rx_ms) > LINK_WATCHDOG_MS))
        {
            L->up_reported = false;
            L->health.state = LINK_DOWN;
            link_post_event(LINK_EV_DOWN);
            ESP_LOGW(TAG, "LINK DOWN (silent)");
        }

        /* poll-cadence estimate for the telemetry rtt display */
        if ((xTaskGetTickCount() - last_1s) >= pdMS_TO_TICKS(1000))
        {
            last_1s = xTaskGetTickCount();
            L->health.frames_tx = L->frames_tx_total;
            if (L->up_reported)
            {
                L->health.rtt_ms =
                    (uint16_t)((L->last_host_ev1 - L->last_host_ev2) & 0xFFFFu);
            }

            /* Bench bring-up diagnostic (myCar LINKDBG counterpart): the single
             * most useful fact is whether the master's SPI transactions reach
             * this slave at all. host_ev counts every CS-driven callback
             * (RDBUF/WRBUF/RDDMA/WRDMA); if it stays 0 the wire from the master
             * (CS/SCLK/MOSI/GND) is not getting here - nothing on this side can
             * fix that. up=1 means the 500 ms watchdog is fed. */
            ESP_LOGI(TAG,
                     "SPIDBG host_ev=%lu up=%d rx=%lu tx=%lu crc=%lu fmt=%lu seq=%lu "
                     "| isr rdbuf=%lu wrbuf=%lu rddma=%lu wrdma=%lu tx_infl=%d",
                     (unsigned long)L->host_ev_total,
                     (int)L->up_reported,
                     (unsigned long)L->health.frames_rx,
                     (unsigned long)L->frames_tx_total,
                     (unsigned long)L->crc_errs_total,
                     (unsigned long)L->fmt_errs_total,
                     (unsigned long)L->seq_errs_total,
                     (unsigned long)L->isr_buftx,
                     (unsigned long)L->isr_bufrx,
                     (unsigned long)L->isr_recv,
                     (unsigned long)L->isr_sent,
                     (int)L->tx_in_flight);
        }

        (void)esp_task_wdt_reset();
    }
}

/* ================= ALIVE timer (10 ms, ISR) ================= */

static void alive_timer_cb(void *arg)
{
    (void)arg;
    s_link.alive++;
    regs_set_u32(SF_REG_ALIVE, s_link.alive);
    regs_publish();
}

/* ================= public API ================= */

esp_err_t link_send(const proto_frame_t *f)
{
    sf_frame_t sf;
    size_t n;
    esp_err_t err = ESP_OK;
    link_tx_msg_t msg;

    if (f == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_link.q_tx == NULL)                         /* link not initialized */
    {
        return ESP_ERR_INVALID_STATE;
    }
    n = v2_to_sf(f, &sf);
    if (n == 0u)
    {
        return ESP_ERR_NOT_SUPPORTED;                /* not mapped (local)   */
    }
    if (xSemaphoreTake(s_link.tx_mtx, pdMS_TO_TICKS(10)) != pdTRUE)
    {
        return ESP_ERR_NO_MEM;
    }
    msg.f = sf;
    if (xQueueSend(s_link.q_tx, &msg, 0) != pdTRUE)
    {
        s_link.health.tx_busy++;
        s_link.txovfl++;
        err = ESP_ERR_NO_MEM;
    }
    else
    {
        s_link.frames_tx_total++;
    }
    (void)xSemaphoreGive(s_link.tx_mtx);
    return err;
}

esp_err_t link_send_ota_chunk(uint16_t idx, const uint8_t *data, size_t n)
{
    link_tx_msg_t msg;
    esp_err_t err = ESP_OK;

    if ((data == NULL) || (n == 0u) || (n > LINK_OTA_CHUNK_MAX))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if ((s_link.q_tx == NULL) || (s_link.tx_mtx == NULL))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_link.tx_mtx, pdMS_TO_TICKS(10)) != pdTRUE)
    {
        return ESP_ERR_NO_MEM;
    }
    memset(&msg.f, 0, sizeof(msg.f));
    msg.f.type  = SF_TYPE_OTA_D;
    msg.f.cid   = SF_CID_OTA_CHUNK;
    msg.f.seq   = s_link.seq_tx++;
    msg.f.flags = 0u;
    msg.f.len   = (uint16_t)(n + 2u);
    proto_put_u16(&msg.f.data[0], idx);
    memcpy(&msg.f.data[2], data, n);

    if (xQueueSend(s_link.q_tx, &msg, 0) != pdTRUE)
    {
        s_link.health.tx_busy++;
        s_link.txovfl++;
        err = ESP_ERR_NO_MEM;
    }
    else
    {
        s_link.frames_tx_total++;
    }
    (void)xSemaphoreGive(s_link.tx_mtx);
    return err;
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
        s_link.health.crc_errs = 0u;
        s_link.health.fmt_errs = 0u;
        s_link.health.seq_errs = 0u;
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
    spi_bus_config_t bus = { 0 };
    spi_slave_hd_slot_config_t slot = { 0 };
    esp_timer_handle_t alive_timer;
    const esp_timer_create_args_t targs = {
        .callback = alive_timer_cb,
        .name     = "sf_alive",
    };

    memset(&s_link, 0, sizeof(s_link));
    sf_parser_init(&s_link.parser);
    s_link.health.state    = LINK_DOWN;
    s_link.health.clock_hz = CONFIG_C6_LINK_SPI_CLOCK_HZ;
    regs_set_u32(SF_REG_READY, 0u);

    s_link.q_tx   = xQueueCreate(LINK_TX_QUEUE_LEN, sizeof(link_tx_msg_t));
    s_link.q_rx   = xQueueCreate(LINK_RX_QUEUE_LEN, sizeof(proto_frame_t));
    s_link.q_evt  = xQueueCreate(8, sizeof(link_event_t));
    s_link.tx_mtx = xSemaphoreCreateMutex();
    if ((s_link.q_tx == NULL) || (s_link.q_rx == NULL) ||
        (s_link.q_evt == NULL) || (s_link.tx_mtx == NULL))
    {
        return ESP_ERR_NO_MEM;
    }

    bus.mosi_io_num    = CONFIG_C6_LINK_SPI_MOSI_GPIO;
    bus.miso_io_num    = CONFIG_C6_LINK_SPI_MISO_GPIO;
    bus.sclk_io_num    = CONFIG_C6_LINK_SPI_SCLK_GPIO;
    bus.quadwp_io_num  = -1;
    bus.quadhd_io_num  = -1;
    bus.max_transfer_sz = (int)LINK_SEG_SIZE;

    slot.spics_io_num = CONFIG_C6_LINK_SPI_CS_GPIO;
    slot.mode         = 0;
    slot.command_bits = 8;                           /* 22 §4.4 (E3)        */
    slot.address_bits = 8;
    slot.dummy_bits   = 8;
    slot.queue_size   = 8;
    slot.dma_chan     = SPI_DMA_CH_AUTO;
    slot.flags        = 0;
    slot.cb_config.cb_sent      = cb_sent;
    slot.cb_config.cb_recv      = cb_recv;
    slot.cb_config.cb_buffer_tx = cb_buffer_tx;
    slot.cb_config.cb_buffer_rx = cb_buffer_rx;
    slot.cb_config.arg          = &s_link;

    err = spi_slave_hd_init(LINK_SPI_HOST, &bus, &slot);
    if (err != ESP_OK)
    {
        return err;
    }

    /* IRQ: open-drain output, low until data is pending */
    /* IRQ line: open-drain, low = data pending, released = high.
     * Wiring is jumper wires only, no external pull-up resistor (22 §3.2 /
     * §9.3). The far-end TC275 P23.0 has its own internal pull-up, but that is
     * weak over a jumper lead, so enable this pin's internal pull-up too: on an
     * open-drain output it does not fight the low drive (a low still wins) and
     * it stiffens the released-high level and cleans up the rising edge. */
    gpio_config_t io = { 0 };
    io.pin_bit_mask = 1ULL << CONFIG_C6_LINK_SPI_IRQ_GPIO;
    io.mode         = GPIO_MODE_OUTPUT_OD;
    io.pull_up_en   = GPIO_PULLUP_ENABLE;            /* no external pull-up: rely
                                                      * on internal pull-ups     */
    err = gpio_config(&io);
    if (err != ESP_OK)
    {
        return err;
    }
    irq_set(false);

    if (xTaskCreate(link_task, "link_task", LINK_TASK_STACK, NULL,
                    LINK_TASK_PRIO, &s_link.task) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    err = esp_timer_create(&targs, &alive_timer);
    if (err != ESP_OK)
    {
        return err;
    }
    return esp_timer_start_periodic(alive_timer, 10ull * 1000ull);
}
