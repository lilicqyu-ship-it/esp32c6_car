/*
 * link.h - SPI half-duplex frame link to TC275 (tc275_car doc 22, SDD §3.7/§6.1a)
 *
 * Transport: TC275 QSPI3 master <-> ESP32-C6 SPI2 slave (spi_slave_hd, segment
 * mode), 5 wires SCLK/MOSI/MISO/CS + open-drain IRQ (data-ready).  Frames on
 * the wire are SF frames (components/c6_sf); this component performs the
 * v2<->SF field mapping (22 §5.5) so bridge/pair keep speaking v2 frames and
 * stay unaware of the physical layer.
 *
 * Shared-register handshake (22 §4.3): the slave publishes
 * SF_READY / SF_TX_PENDING / SF_RX_ROOM / SF_ALIVE / SF_ERRSTAT / SF_CMDRSP;
 * the master reads with the read-twice rule and never issues a data
 * transaction while SF_READY is absent.
 *
 * Contexts:
 *   - link_task (prio 12): queues RX DMA buffers, parses segments -> v2 frames
 *     into q_rx; assembles TX segments from the SF frame queue; refreshes the
 *     shared registers; host-liveness watchdog (500 ms -> LINK DOWN).
 *   - ISR callbacks: only bump counters / notify the task.
 *   - ALIVE esp_timer (10 ms): SF_ALIVE++ published to the shared registers.
 *   - any caller: link_send() / link_send_ota_chunk() are mutex-protected,
 *     non-blocking (BUSY on overflow).
 */
#ifndef C6_LINK_H
#define C6_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "proto_frames.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Kconfig mirror of the production clock step (master drives the clock; this
 * value is used for diag/telemetry reporting and TC275-side alignment). */
#ifndef CONFIG_C6_LINK_SPI_CLOCK_HZ
#define CONFIG_C6_LINK_SPI_CLOCK_HZ 5000000
#endif

#define LINK_TX_QUEUE_LEN     32                     /* SF frames queued    */
#define LINK_RX_QUEUE_LEN     16                     /* v2 frames to bridge */

#define LINK_SEG_SIZE         512u                   /* DMA segment buffers */
#define LINK_OTA_CHUNK_MAX    240u                   /* 22 §5.5             */

typedef enum
{
    LINK_DOWN = 0,
    LINK_UP,                       /* host transactions observed           */
} link_state_t;

typedef struct
{
    link_state_t state;
    uint32_t clock_hz;                              /* production step     */
    uint32_t rtt_ms;                                /* host poll cadence   */
    uint16_t crc_errs;                              /* since last query    */
    uint16_t fmt_errs;
    uint16_t seq_errs;
    uint32_t frames_rx;
    uint32_t frames_tx;
    uint32_t tx_busy;
    uint32_t last_rx_ms;                            /* last host event     */
} link_health_t;

typedef enum
{
    LINK_EV_NONE = 0,
    LINK_EV_UP,
    LINK_EV_DOWN,
} link_event_id_t;

typedef struct
{
    link_event_id_t id;
    uint32_t reserved;
} link_event_t;

/* Create SPI slave HD + task + timers. Call once from app_main. */
esp_err_t link_init(void);

/*
 * Enqueue one v2 frame for TX (mapped to SF per 22 §5.5).
 * ESP_ERR_NO_MEM (=BUSY) when the frame queue is full - callers must treat
 * commands as backpressure, never drop silently.
 */
esp_err_t link_send(const proto_frame_t *f);

/*
 * OTA relay fast path: map (idx, chunk) directly into SF OTA_D/0x31
 * {u16 idx, data[<=LINK_OTA_CHUNK_MAX]}.  ESP_ERR_NO_MEM on overflow,
 * ESP_ERR_INVALID_ARG when n exceeds the SF chunk limit.
 */
esp_err_t link_send_ota_chunk(uint16_t idx, const uint8_t *data, size_t n);

/* Deliver v2 frames / link events to the bridge (bridge drains them in its
 * 20 ms loop; no queue set - coredump 09-27 showed set accounting desync). */
QueueHandle_t link_rx_queue(void);
QueueHandle_t link_event_queue(void);

/* Snapshot of health counters (lock-protected). */
void link_get_health(link_health_t *out);

/* True when host transactions keep the 500 ms watchdog fed. */
bool link_is_up(void);

/* Mirror of every inbound v2 frame (legacy bridge, diag sniffers).
 * Called from the link task context - keep it short, never block. */
void link_set_tap(void (*tap)(const proto_frame_t *f));

#ifdef __cplusplus
}
#endif

#endif /* C6_LINK_H */
