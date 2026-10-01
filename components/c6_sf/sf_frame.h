/*
 * sf_frame.h - SPI-Link SF frame codec (pure C, host-testable)
 *
 * Implements the SF frame defined in tc275_car doc/20-design/22-link-spi-design.md
 * §5 (SDD V1.2 §6.1a).  The SF frame lives ONLY on the SPI link between the
 * C6 (slave, spi_slave_hd) and the TC275 (QSPI3 master); the phone/WS side
 * keeps proto v2 frames - c6_link performs the v2<->SF field mapping
 * (22 §5.5) so bridge/pair/ota_relay never see the wire container.
 *
 * Hard rules (same as c6_proto): pure C99, no OS headers, no dynamic memory,
 * explicit little-endian wire fields, host-compilable.
 *
 * Frame:
 *   | 5A | 01 | TYPE | SEQ | FLAGS | LEN u16LE | CID | DATA[LEN] | CRC16 |
 *   LEN <= 248, CRC = CRC16-CCITT-FALSE over the first 8+LEN bytes,
 *   segments pad frames with 0x00 up to a 4-byte multiple (parser skips 0x00
 *   while idle).  Several frames may be concatenated in one SPI segment.
 */
#ifndef SF_FRAME_H
#define SF_FRAME_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- frame constants -----------------------------------------------------*/
#define SF_MAGIC             0x5Au
#define SF_VER               0x01u
#define SF_MAX_PAYLOAD       248u
#define SF_HEADER_LEN        8u                        /* magic..len      */
#define SF_MIN_FRAME         (SF_HEADER_LEN + 2u)      /* empty payload   */
#define SF_MAX_FRAME         (SF_HEADER_LEN + SF_MAX_PAYLOAD + 2u)

/* FLAGS (22 §5.1: bit0-2 reserved for segmentation; V1.0 sends unfragmented) */
#define SF_FLAGS_FRAG        0x01u                     /* continuation    */
#define SF_FLAGS_FRAG_END    0x02u                     /* last fragment   */

/* ---- TYPE (22 §5.2) -------------------------------------------------------*/
#define SF_TYPE_CMD          0x01u   /* C6 -> TC275, commands (never dropped) */
#define SF_TYPE_TEL          0x02u   /* TC275 -> C6, telemetry (drop-old)     */
#define SF_TYPE_ACK          0x03u   /* both, payload = acked TYPE+SEQ        */
#define SF_TYPE_HBT          0x04u   /* both, heartbeat / RTT marker          */
#define SF_TYPE_EVT          0x05u   /* TC275 -> C6, error / state change     */
#define SF_TYPE_OTA_D        0x06u   /* both, OTA data (BEGIN/CHUNK/ABORT)    */
#define SF_TYPE_OTA_C        0x07u   /* both, OTA control (ACK/STATUS/SWAP)   */
#define SF_TYPE_DBG          0x08u   /* TC275 -> C6, log ring dump (droppable)*/
#define SF_TYPE_VND          0x0Fu   /* vendor / reserved                     */

/* ---- CID channels ---------------------------------------------------------*/
/* CMD (0x01) */
#define SF_CID_DRV           0x01u   /* driving: {u8 op, i16 v, i16 w}        */
#define SF_CID_CFG           0x02u   /* config  = v2 0x52 data as-is          */
#define SF_CID_DIAG          0x03u   /* diag/status: {u8 op, ...}; op=0x42 is
                                        the v2 LINK_STATE, op=0x53 = v2 DIAG  */
#define SF_CID_DPT           0x04u   /* production test = v2 0x70..0x7F data  */
#define SF_CID_PAIR          0x05u   /* pairing = v2 0x51 data as-is          */

/* TEL (0x02) */
#define SF_CID_TELEMETRY     0x10u   /* payload = proto v2 0x41 38B layout    */

/* EVT (0x05) */
#define SF_CID_EVT_ERROR     0x20u   /* {u16 errcode, ...}                    */
#define SF_CID_EVT_STATE     0x21u   /* {u8 kind, ...}: kind 1 = PAIR reply,
                                        payload then = v2 0x51 data           */
#define SF_CID_DPT_RESULT    0x22u   /* DPT result (doc/17 §4.1/§8.4):
                                        {op u8, status u8, invert i8x4,
                                         delta i32x4 LE, saved u8}           */
#define SF_CID_DPT_REC       0x23u   /* DPT record (doc/17 §8.4), 15B:
                                        {ver u8, src u8, pos u8x4,
                                         invert i8x4, fullScale i16,
                                         wheelDia i16, crcOk u8} LE          */

/* OTA (0x06 OTA_D / 0x07 OTA_C) */
#define SF_CID_OTA_BEGIN     0x30u   /* {u32 total, u32 crc32}                */
#define SF_CID_OTA_CHUNK     0x31u   /* {u16 idx, data[<=240]}                */
#define SF_CID_OTA_ACK       0x32u   /* {u16 idx, u8 result}                  */
#define SF_CID_OTA_STATUS    0x33u   /* {u8 state, u8 pct}                    */
#define SF_CID_OTA_SWAP      0x34u   /* TC275 reboots into new slot           */
#define SF_CID_OTA_ABORT     0x35u   /* C6 extension to 22 §5.5 (write-back)  */

#define SF_OTA_CHUNK_MAX     240u    /* 22 §5.5: 62 -> 240                    */

/* DBG (0x08) */
#define SF_CID_DBG_LOG       0x40u

/* ---- shared registers (22 §4.3, slave publishes, master reads) -------------*/
#define SF_REG_READY         0u      /* magic 0x5F534601 ("_SF1")             */
#define SF_REG_TX_PENDING    4u      /* bytes host may read now               */
#define SF_REG_RX_ROOM       8u      /* bytes host may write now              */
#define SF_REG_ALIVE         12u     /* +1 every 10 ms                        */
#define SF_REG_ERRSTAT       16u     /* error bitmap (see bits below)         */
#define SF_REG_CMDRSP        20u     /* GEN receipt {u8 cmd, u8 result, ...}  */
#define SF_REG_GEN           24u     /* host GEN write slot (master->slave)   */
#define SF_REG_COUNT         28u     /* bytes published by the slave          */

#define SF_READY_MAGIC       0x5F534601u

/* SF_ERRSTAT bits (each sticky, saturating counters behind) */
#define SF_ERR_CRC           0x00000001u  /* frame CRC failed                 */
#define SF_ERR_FMT           0x00000002u  /* frame format/ver error           */
#define SF_ERR_SEQ           0x00000004u  /* SEQ window violation             */
#define SF_ERR_RXOVFL        0x00000008u  /* RX queue overflow / frame dropped*/
#define SF_ERR_TXOVFL        0x00000010u  /* TX queue overflow (BUSY)         */
#define SF_ERR_TRUN          0x00000020u  /* residual frame across segments   */
#define SF_ERR_LINKLOST      0x00000040u  /* 5 consecutive CRC failures       */

/* GEN commands (master -> slave, 4-byte write to SF_REG_GEN) */
#define SF_GEN_NOP           0u
#define SF_GEN_RESET_LINK    1u    /* reset slave link state machines        */
#define SF_GEN_SILENCE_ON    2u    /* stop asserting IRQ (bench injections)  */
#define SF_GEN_SILENCE_OFF   3u
#define SF_GEN_CLOCK_SET     4u    /* payload u32 = new clock (master-side)  */

#define SF_GEN_RSP_OK        0u
#define SF_GEN_RSP_UNKNOWN   1u

/* ---- frame object ---------------------------------------------------------*/
typedef struct
{
    uint8_t  type;
    uint8_t  seq;
    uint8_t  flags;
    uint8_t  cid;
    uint16_t len;
    uint8_t  data[SF_MAX_PAYLOAD];
} sf_frame_t;

/* ---- encode ----------------------------------------------------------------*/
/*
 * Serialize f into out (frame only, no padding).  Returns the frame length
 * (8 + len + 2) or 0 on error (null / oversize / cap too small).
 */
size_t sf_encode(const sf_frame_t *f, uint8_t *out, size_t cap);

/* Convenience build+encode.  seq = *seq_counter, auto-incremented (mod 256). */
size_t sf_build(uint8_t type, uint8_t cid, const uint8_t *data, size_t len,
                uint8_t *seq_counter, uint8_t *out, size_t cap);

/* ---- segment assembly (22 §5.1: multi-frame + 4-byte padding) --------------*/
/*
 * Append frame to a segment buffer.  Returns bytes written, 0 when the frame
 * does not fit (caller must flush the segment first).
 */
size_t sf_segment_append(uint8_t *seg, size_t seg_cap, size_t seg_used,
                         const sf_frame_t *f);

/* Pad the segment tail with 0x00 up to a 4-byte multiple. Returns new length. */
size_t sf_segment_pad(uint8_t *seg, size_t used);

/* ---- byte-wise parser -------------------------------------------------------*/
typedef enum
{
    SF_RX_NONE = 0,
    SF_RX_FRAME,          /* *out holds a CRC-valid frame                    */
    SF_RX_CRC_ERR,
    SF_RX_FMT_ERR,        /* bad LEN / frame too long                        */
    SF_RX_VER_ERR,
    SF_RX_RESIDUAL,       /* segment ended mid-frame (caller decides policy) */
} sf_rx_ev_t;

typedef struct
{
    int      state;                       /* opaque                       */
    uint8_t  buf[SF_MAX_FRAME];
    uint16_t pos;
    uint16_t need;
} sf_parser_t;

void         sf_parser_init(sf_parser_t *p);
sf_rx_ev_t   sf_parser_feed(sf_parser_t *p, uint8_t byte, sf_frame_t *out);

/* Strict forward window (22 §5.3): valid when 1 <= (seq - last) mod 256 <= 32.
 * On success *last is advanced.  Host->slave and slave->host directions keep
 * their own *last. */
int sf_seq_ok(uint8_t seq, uint8_t *last);

#ifdef __cplusplus
}
#endif

#endif /* SF_FRAME_H */
