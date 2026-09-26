/*
 * proto_frames.h - SmartDrive LINK protocol v2 codec (single shared implementation)
 *
 * This file is THE single source of truth for frame encode/decode on both sides
 * of the LINK (coding-plan decision C1 / LLDD D2):
 *   - ESP32-C6 : components/c6_proto/proto_frames.[ch]
 *   - TC275    : adopt the identical file verbatim (CI compares hashes)
 *
 * Hard rules:
 *   - Pure C99, no OS/IDF/TriCore headers, no dynamic memory, host-compilable.
 *   - All multi-byte wire fields are EXPLICIT LITTLE-ENDIAN. The two ends have
 *     opposite endianness (TriCore BE / RISC-V LE) so payload structs are never
 *     cast onto wire bytes; use the proto_*_encode/decode helpers.
 *
 * Frame (SDD 6.1):
 *   | AA | 55 | VER | CMD | SEQ | LEN | DATA[LEN] | CRC16 |
 *   VER  = 0x02, LEN <= PROTO_MAX_PAYLOAD(64)
 *   CRC  = CRC16-CCITT-FALSE: poly 0x1021, init 0xFFFF, MSB-first,
 *          no reflection, no final xor.  check("123456789") == 0x29B1.
 */
#ifndef PROTO_FRAMES_H
#define PROTO_FRAMES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- frame constants ---------------------------------------------------- */
#define PROTO_SYNC1          0xAAu
#define PROTO_SYNC2          0x55u
#define PROTO_VER            0x02u
#define PROTO_MAX_PAYLOAD    64u
#define PROTO_HEADER_LEN     6u                                  /* sync ver cmd seq len */
#define PROTO_MAX_FRAME      (PROTO_HEADER_LEN + PROTO_MAX_PAYLOAD + 2u)

/* ---- command table (SDD 6.2 + LLDD 3.1) --------------------------------- */
/* legacy demo commands 0x01..0x32 pass through untouched (semantics on TC275) */
#define PROTO_CMD_STOP              0x01u
#define PROTO_CMD_FORWARD           0x02u
#define PROTO_CMD_BACKWARD          0x03u
#define PROTO_CMD_LEFT              0x04u
#define PROTO_CMD_RIGHT             0x05u
#define PROTO_CMD_FORWARD_LEFT      0x06u
#define PROTO_CMD_FORWARD_RIGHT     0x07u
#define PROTO_CMD_ROTATE_LEFT       0x08u
#define PROTO_CMD_ROTATE_RIGHT      0x09u
#define PROTO_CMD_SET_SPEED         0x10u
#define PROTO_CMD_GET_STATUS        0x20u
#define PROTO_CMD_HEARTBEAT         0x21u
#define PROTO_CMD_RESET             0x30u
#define PROTO_CMD_CLEAR_FAULT       0x31u
#define PROTO_CMD_EMERGENCY_STOP    0x32u

/* v2 transport frames */
#define PROTO_CMD_TELEMETRY         0x41u  /* TC275 -> C6, 20 ms             */
#define PROTO_CMD_LINK_STATE        0x42u  /* C6 -> TC275, client set change */
#define PROTO_CMD_PING              0x43u  /* both ways, PONG echoes SEQ     */
#define PROTO_CMD_BAUD              0x44u  /* handshake, LLDD 4.5            */

/* v2 control frames (payload[0] = sub-op) */
#define PROTO_CMD_DRIVE             0x50u  /* v:i16, omega:i16 (doubles as heartbeat) */
#define PROTO_CMD_PAIR              0x51u
#define PROTO_CMD_CFG               0x52u
#define PROTO_CMD_DIAG              0x53u

/* OTA frame group */
#define PROTO_CMD_OTA_BEGIN         0x60u  /* {u32 total, u32 crc32}          */
#define PROTO_CMD_OTA_CHUNK         0x61u  /* {u16 idx, data[<=62]}           */
#define PROTO_CMD_OTA_ACK           0x62u  /* {u16 idx, u8 result}            */
#define PROTO_CMD_OTA_STATUS        0x63u  /* {u8 state, u8 pct} END==DONE    */
#define PROTO_CMD_OTA_SWAP          0x64u  /* TC275 reboots into new slot     */
#define PROTO_CMD_OTA_ABORT         0x65u  /* drop half-written slot          */

/* production test group */
#define PROTO_CMD_DPT_ENTER         0x70u
#define PROTO_CMD_DPT_LED           0x71u
#define PROTO_CMD_DPT_MOTOR_RUN     0x72u
#define PROTO_CMD_DPT_ENC_READ      0x73u
#define PROTO_CMD_DPT_CAL           0x74u
#define PROTO_CMD_DPT_CAL_SAVE      0x75u
#define PROTO_CMD_DPT_SN_WRITE      0x76u
#define PROTO_CMD_DPT_AGING         0x77u
#define PROTO_CMD_DPT_REPORT        0x78u
#define PROTO_CMD_DPT_SELFTEST      0x79u  /* C6-local items (LLDD 3.1)       */

/* ---- payload sub-ops -----------------------------------------------------*/
/* 0x43 PING */
#define PROTO_PING_TYPE_PING        0x00u
#define PROTO_PING_TYPE_PONG        0x01u

/* 0x44 BAUD */
#define PROTO_BAUD_OP_REQ           0x00u
#define PROTO_BAUD_OP_ACK           0x01u
#define PROTO_BAUD_OP_NAK           0x02u

/* 0x42 LINK_STATE */
#define PROTO_LINKSTATE_NONE        0x00u  /* no client at all               */
#define PROTO_LINKSTATE_SPECTATORS  0x01u  /* spectators only                */
#define PROTO_LINKSTATE_CTRL        0x02u  /* controller online              */

/* 0x51 PAIR */
#define PROTO_PAIR_OP_REQ           0x00u  /* phone -> C6 -> TC275           */
#define PROTO_PAIR_OP_CONFIRM       0x01u  /* TC275 -> C6 {result, token16}  */
#define PROTO_PAIR_OP_NOTIFY        0x02u  /* TC275 -> C6 {window_s}         */
#define PROTO_PAIR_OP_REJECT        0x03u  /* TC275 -> C6 {reason}           */

/* 0x60 OTA status states */
#define PROTO_OTA_STATE_RUNNING     0x00u
#define PROTO_OTA_STATE_DONE        0x01u
#define PROTO_OTA_STATE_FAILED      0x02u

/* ---- telemetry payload (SDD 6.3, 38 bytes, explicit LE) ------------------ */
#define PROTO_TELEMETRY_LEN         38u

typedef struct
{
    uint32_t seq;               /* E2E sequence                          */
    uint32_t uptime_ms;         /* TC275 uptime                          */
    uint8_t  state;             /* mission state (demo-compatible codes) */
    uint16_t fault_code;        /* active highest-severity error         */
    int16_t  v_target_l;        /* mm/s                                  */
    int16_t  v_target_r;
    int16_t  v_meas_l;
    int16_t  v_meas_r;
    uint16_t battery_mv;
    uint8_t  battery_pct;
    uint32_t odo_session_mm;
    uint32_t odo_total_mm;
    uint16_t link_rtt_ms;       /* filled by C6 from its own PING (LLDD 3.1) */
    uint8_t  link_err_rate;     /* 0.1% units                            */
    uint32_t fw_ver;            /* 0x00MMmmpp                            */
    uint8_t  hw_rev;
} proto_telemetry_t;

/* ---- encode / decode ------------------------------------------------------*/
typedef struct
{
    uint8_t ver;                            /* always PROTO_VER            */
    uint8_t cmd;
    uint8_t seq;
    uint8_t len;                            /* valid bytes in data[]       */
    uint8_t data[PROTO_MAX_PAYLOAD];        /* native (host) byte order    */
} proto_frame_t;

uint16_t proto_crc16(const uint8_t *data, size_t len);

/*
 * Serialize f into out.  Returns total frame length (>0) or 0 on
 * null/oversize/len-overflow error.
 */
size_t proto_encode(const proto_frame_t *f, uint8_t *out, size_t cap);

/* Convenience build+serialize. Returns frame length or 0. */
size_t proto_build(uint8_t cmd, uint8_t seq, const uint8_t *data, size_t len,
                   uint8_t *out, size_t cap);

/* ---- byte-wise parser -----------------------------------------------------*/
typedef enum
{
    PROTO_RX_NONE = 0,      /* byte consumed, nothing complete yet        */
    PROTO_RX_FRAME,         /* *out holds a CRC-valid frame               */
    PROTO_RX_CRC_ERR,       /* frame dropped: CRC mismatch                */
    PROTO_RX_FMT_ERR,       /* frame dropped: bad LEN / encoding          */
    PROTO_RX_VER_ERR        /* frame dropped: unknown VER                 */
} proto_rx_ev_t;

typedef struct
{
    int      state;                              /* parser state, opaque */
    uint8_t  buf[PROTO_MAX_FRAME];
    uint16_t pos;
    uint16_t need;
} proto_parser_t;

void proto_parser_init(proto_parser_t *p);
proto_rx_ev_t proto_parser_feed(proto_parser_t *p, uint8_t byte, proto_frame_t *out);

/* ---- telemetry LE helpers --------------------------------------------------*/
/* Serialize/parse the telemetry payload into DATA[38]. Endianness-safe. */
size_t proto_telemetry_encode(const proto_telemetry_t *t, uint8_t *data, size_t cap);
int    proto_telemetry_decode(const uint8_t *data, size_t len, proto_telemetry_t *t);

/* ---- generic LE field helpers (shared by both firmwares) -------------------*/
static inline void proto_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static inline void proto_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline uint16_t proto_get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t proto_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#ifdef __cplusplus
}
#endif

#endif /* PROTO_FRAMES_H */
