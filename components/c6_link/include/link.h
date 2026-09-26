/*
 * link.h - LINK 2 Mbps full-duplex frame link to TC275 (LLDD 4.5)
 *
 * UART1 (Kconfig pins, default TX=GPIO10 RX=GPIO11), 921600 baud start,
 * 30 s zero-CRC-error window then proposes 2 Mbps via 0x44; >=10 CRC errors
 * in the sliding window falls back one step (921600 <-> 2M, two-step table).
 *
 * Contexts:
 *   - uart event task  : RX bytes -> proto parser -> q_link_rx (QueueSet member)
 *   - link_tx_task     : drains the TX frame queue written by link_send()
 *   - esp_timer 20 ms  : health monitor; every 5th tick emits PING
 *   - any caller       : link_send() is mutex-protected, non-blocking
 */
#ifndef C6_LINK_H
#define C6_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "proto_frames.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_BAUD_BASE        921600u
#define LINK_BAUD_FAST        2000000u

#define LINK_TX_QUEUE_LEN     8
#define LINK_RX_QUEUE_LEN     16                     /* LLDD 2.4 */

typedef enum
{
    LINK_DOWN = 0,
    LINK_UP,
} link_state_t;

typedef struct
{
    link_state_t state;
    uint32_t baud;                                      /* current          */
    uint32_t rtt_ms;                                    /* last PING rtt    */
    uint16_t crc_errs;                                  /* since last query */
    uint16_t fmt_errs;
    uint32_t frames_rx;
    uint32_t frames_tx;
    uint32_t tx_busy;                                   /* link_send BUSY   */
    uint32_t last_rx_ms;                                /* esp_timer ms     */
} link_health_t;

typedef enum
{
    LINK_EV_NONE = 0,
    LINK_EV_UP,                                         /* first frames / heartbeat alive */
    LINK_EV_DOWN,                                       /* 500 ms silent                  */
    LINK_EV_BAUD_CHANGED,
} link_event_id_t;

typedef struct
{
    link_event_id_t id;
    uint32_t baud;                                      /* valid on BAUD_CHANGED */
} link_event_t;

/* Create UART driver + tasks + timers. Safe to call once from app_main. */
esp_err_t link_init(void);

/* Enqueue one frame for TX. ESP_ERR_NO_MEM (=BUSY) when the queue is full. */
esp_err_t link_send(const proto_frame_t *f);

/* QueueSet member delivering inbound frames (bridge owns the QueueSet). */
QueueHandle_t link_rx_queue(void);

/* QueueSet member delivering link_event_t (bridge owns the QueueSet). */
QueueHandle_t link_event_queue(void);

/* Optional mirror of every inbound frame (legacy bridge, diag sniffers).
 * Called from the RX task context - keep it short, never block. */
void link_set_tap(void (*tap)(const proto_frame_t *f));

/* Snapshot of health counters (lock-protected, cheap). */
void link_get_health(link_health_t *out);

/* Hot path used by the health monitor; also usable from diag handlers. */
esp_err_t link_request_baud(uint32_t baud);

/* True when the 500 ms watchdog sees traffic (page "车端已连接"). */
bool link_is_up(void);

#ifdef __cplusplus
}
#endif

#endif /* C6_LINK_H */
