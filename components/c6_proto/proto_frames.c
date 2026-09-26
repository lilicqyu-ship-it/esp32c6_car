/*
 * proto_frames.c - SmartDrive LINK protocol v2 codec (single shared implementation)
 *
 * See proto_frames.h for the frame format and hard rules.  This file must stay
 * free of any target-specific include so both TriCore and RISC-V toolchains and
 * the host unit tests compile it verbatim.
 */
#include "proto_frames.h"

/* ---- CRC16-CCITT-FALSE (poly 0x1021, init 0xFFFF, MSB first) --------------*/
uint16_t proto_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    size_t i;
    int b;

    if (data == NULL)
    {
        return 0u;
    }

    for (i = 0u; i < len; i++)
    {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (b = 0; b < 8; b++)
        {
            if ((crc & 0x8000u) != 0u)
            {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            }
            else
            {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* ---- parser states ---------------------------------------------------------*/
#define P_SYNC1     0
#define P_SYNC2     1
#define P_VER       2
#define P_CMD       3
#define P_SEQ       4
#define P_LEN       5
#define P_DATA      6
#define P_CRC_HI    7
#define P_CRC_LO    8

void proto_parser_init(proto_parser_t *p)
{
    if (p != NULL)
    {
        p->state = P_SYNC1;
        p->pos   = 0u;
        p->need  = 0u;
    }
}

proto_rx_ev_t proto_parser_feed(proto_parser_t *p, uint8_t byte, proto_frame_t *out)
{
    proto_rx_ev_t ev = PROTO_RX_NONE;

    if ((p == NULL) || (out == NULL))
    {
        return PROTO_RX_FMT_ERR;
    }

    switch (p->state)
    {
        case P_SYNC1:
            if (byte == PROTO_SYNC1)
            {
                p->buf[0] = byte;
                p->state  = P_SYNC2;
            }
            break;

        case P_SYNC2:
            if (byte == PROTO_SYNC2)
            {
                p->buf[1] = byte;
                p->state  = P_VER;
            }
            else if (byte == PROTO_SYNC1)
            {
                p->buf[0] = byte;       /* stay resynced on AA AA 55 */
            }
            else
            {
                p->state = P_SYNC1;
            }
            break;

        case P_VER:
            if (byte == PROTO_VER)
            {
                p->buf[2] = byte;
                p->state  = P_CMD;
            }
            else
            {
                p->state = P_SYNC1;
                ev       = PROTO_RX_VER_ERR;
            }
            break;

        case P_CMD:
            p->buf[3] = byte;
            p->state  = P_SEQ;
            break;

        case P_SEQ:
            p->buf[4] = byte;
            p->state  = P_LEN;
            break;

        case P_LEN:
            if (byte > PROTO_MAX_PAYLOAD)
            {
                p->state = P_SYNC1;
                ev       = PROTO_RX_FMT_ERR;
            }
            else
            {
                p->buf[5] = byte;
                p->pos    = 0u;
                p->need   = (uint16_t)byte;
                p->state  = (byte == 0u) ? P_CRC_HI : P_DATA;
            }
            break;

        case P_DATA:
            p->buf[PROTO_HEADER_LEN + p->pos] = byte;
            p->pos++;
            if (p->pos >= p->need)
            {
                p->state = P_CRC_HI;
            }
            break;

        case P_CRC_HI:
            p->buf[PROTO_HEADER_LEN + p->need] = byte;
            p->state = P_CRC_LO;
            break;

        case P_CRC_LO:
        {
            uint16_t rx_crc;
            uint16_t calc_crc;

            p->buf[PROTO_HEADER_LEN + p->need + 1u] = byte;
            p->state = P_SYNC1;

            rx_crc   = (uint16_t)((uint16_t)byte |
                                  ((uint16_t)p->buf[PROTO_HEADER_LEN + p->need] << 8));
            calc_crc = proto_crc16(p->buf, (size_t)(PROTO_HEADER_LEN + p->need));

            if (rx_crc != calc_crc)
            {
                ev = PROTO_RX_CRC_ERR;
            }
            else
            {
                out->ver = p->buf[2];
                out->cmd = p->buf[3];
                out->seq = p->buf[4];
                out->len = p->buf[5];
                for (uint16_t i = 0u; i < p->need; i++)
                {
                    out->data[i] = p->buf[PROTO_HEADER_LEN + i];
                }
                ev = PROTO_RX_FRAME;
            }
            break;
        }

        default:
            proto_parser_init(p);
            ev = PROTO_RX_FMT_ERR;
            break;
    }

    return ev;
}

/* ---- encode ----------------------------------------------------------------*/
size_t proto_encode(const proto_frame_t *f, uint8_t *out, size_t cap)
{
    uint16_t crc;
    size_t n;

    if ((f == NULL) || (out == NULL) || (f->len > PROTO_MAX_PAYLOAD))
    {
        return 0u;
    }

    n = (size_t)PROTO_HEADER_LEN + f->len + 2u;
    if (cap < n)
    {
        return 0u;
    }

    out[0] = PROTO_SYNC1;
    out[1] = PROTO_SYNC2;
    out[2] = PROTO_VER;
    out[3] = f->cmd;
    out[4] = f->seq;
    out[5] = f->len;
    for (uint8_t i = 0u; i < f->len; i++)
    {
        out[PROTO_HEADER_LEN + i] = f->data[i];
    }

    crc = proto_crc16(out, (size_t)PROTO_HEADER_LEN + f->len);
    out[PROTO_HEADER_LEN + f->len]      = (uint8_t)(crc >> 8);
    out[PROTO_HEADER_LEN + f->len + 1u] = (uint8_t)(crc & 0xFFu);
    return n;
}

size_t proto_build(uint8_t cmd, uint8_t seq, const uint8_t *data, size_t len,
                   uint8_t *out, size_t cap)
{
    proto_frame_t f;

    if ((len > PROTO_MAX_PAYLOAD) || ((data == NULL) && (len != 0u)))
    {
        return 0u;
    }

    f.ver = PROTO_VER;
    f.cmd = cmd;
    f.seq = seq;
    f.len = (uint8_t)len;
    for (size_t i = 0u; i < len; i++)
    {
        f.data[i] = data[i];
    }
    return proto_encode(&f, out, cap);
}

/* ---- telemetry --------------------------------------------------------------*/
size_t proto_telemetry_encode(const proto_telemetry_t *t, uint8_t *data, size_t cap)
{
    if ((t == NULL) || (data == NULL) || (cap < PROTO_TELEMETRY_LEN))
    {
        return 0u;
    }

    proto_put_u32(&data[0],  t->seq);
    proto_put_u32(&data[4],  t->uptime_ms);
    data[8]  = t->state;
    proto_put_u16(&data[9],  t->fault_code);
    proto_put_u16(&data[11], (uint16_t)t->v_target_l);
    proto_put_u16(&data[13], (uint16_t)t->v_target_r);
    proto_put_u16(&data[15], (uint16_t)t->v_meas_l);
    proto_put_u16(&data[17], (uint16_t)t->v_meas_r);
    proto_put_u16(&data[19], t->battery_mv);
    data[21] = t->battery_pct;
    proto_put_u32(&data[22], t->odo_session_mm);
    proto_put_u32(&data[26], t->odo_total_mm);
    proto_put_u16(&data[30], t->link_rtt_ms);
    data[32] = t->link_err_rate;
    proto_put_u32(&data[33], t->fw_ver);
    data[37] = t->hw_rev;
    return (size_t)PROTO_TELEMETRY_LEN;
}

int proto_telemetry_decode(const uint8_t *data, size_t len, proto_telemetry_t *t)
{
    if ((data == NULL) || (t == NULL) || (len < (size_t)PROTO_TELEMETRY_LEN))
    {
        return -1;
    }

    t->seq           = proto_get_u32(&data[0]);
    t->uptime_ms     = proto_get_u32(&data[4]);
    t->state         = data[8];
    t->fault_code    = proto_get_u16(&data[9]);
    t->v_target_l    = (int16_t)proto_get_u16(&data[11]);
    t->v_target_r    = (int16_t)proto_get_u16(&data[13]);
    t->v_meas_l      = (int16_t)proto_get_u16(&data[15]);
    t->v_meas_r      = (int16_t)proto_get_u16(&data[17]);
    t->battery_mv    = proto_get_u16(&data[19]);
    t->battery_pct   = data[21];
    t->odo_session_mm = proto_get_u32(&data[22]);
    t->odo_total_mm  = proto_get_u32(&data[26]);
    t->link_rtt_ms   = proto_get_u16(&data[30]);
    t->link_err_rate = data[32];
    t->fw_ver        = proto_get_u32(&data[33]);
    t->hw_rev        = data[37];
    return 0;
}
