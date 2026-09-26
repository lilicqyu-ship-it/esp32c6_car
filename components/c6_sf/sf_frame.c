/*
 * sf_frame.c - SPI-Link SF frame codec (see sf_frame.h for the format)
 */
#include "sf_frame.h"

#include <string.h>

#include "proto_frames.h"       /* shared CRC16-CCITT-FALSE */

/* ---- parser states -----------------------------------------------------------*/
#define P_IDLE      0
#define P_HDR       1    /* collecting the 8 header bytes                       */
#define P_DATA      2
#define P_CRC_HI    3
#define P_CRC_LO    4

void sf_parser_init(sf_parser_t *p)
{
    if (p != NULL)
    {
        p->state = P_IDLE;
        p->pos   = 0u;
        p->need  = 0u;
    }
}

sf_rx_ev_t sf_parser_feed(sf_parser_t *p, uint8_t byte, sf_frame_t *out)
{
    if ((p == NULL) || (out == NULL))
    {
        return SF_RX_FMT_ERR;
    }

    switch (p->state)
    {
        case P_IDLE:
            if (byte == 0x00u)
            {
                break;                              /* segment padding      */
            }
            if (byte != SF_MAGIC)
            {
                return SF_RX_FMT_ERR;               /* resync on garbage    */
            }
            p->buf[0] = byte;
            p->pos    = 1u;
            p->state  = P_HDR;
            break;

        case P_HDR:
            p->buf[p->pos++] = byte;
            if (p->pos == 2u)
            {
                if (byte != SF_VER)
                {
                    p->state = P_IDLE;
                    return SF_RX_VER_ERR;
                }
            }
            else if (p->pos == SF_HEADER_LEN)
            {
                uint16_t len = (uint16_t)(p->buf[5] | (p->buf[6] << 8));
                if (len > SF_MAX_PAYLOAD)
                {
                    p->state = P_IDLE;
                    return SF_RX_FMT_ERR;
                }
                p->need  = len;
                p->state = (len == 0u) ? P_CRC_HI : P_DATA;
            }
            break;

        case P_DATA:
            /* data bytes live after the 8-byte header; p->pos is already
             * absolute (>= SF_HEADER_LEN when entering P_DATA)            */
            p->buf[p->pos] = byte;
            p->pos++;
            if ((uint16_t)(p->pos - SF_HEADER_LEN) >= p->need)
            {
                p->state = P_CRC_HI;
            }
            break;

        case P_CRC_HI:
            p->buf[SF_HEADER_LEN + p->need] = byte;
            p->state = P_CRC_LO;
            break;

        case P_CRC_LO:
        {
            uint16_t rx_crc;
            uint16_t calc;
            size_t   total;

            p->buf[SF_HEADER_LEN + p->need + 1u] = byte;
            p->state = P_IDLE;

            rx_crc = (uint16_t)((uint16_t)byte |
                                ((uint16_t)p->buf[SF_HEADER_LEN + p->need] << 8));
            total  = (size_t)SF_HEADER_LEN + p->need;
            calc   = proto_crc16(p->buf, total);
            if (rx_crc != calc)
            {
                return SF_RX_CRC_ERR;
            }

            out->type  = p->buf[2];
            out->seq   = p->buf[3];
            out->flags = p->buf[4];
            out->cid   = p->buf[7];
            out->len   = p->need;
            memcpy(out->data, &p->buf[SF_HEADER_LEN], p->need);
            return SF_RX_FRAME;
        }

        default:
            sf_parser_init(p);
            return SF_RX_FMT_ERR;
    }
    return SF_RX_NONE;
}

/* ---- encode --------------------------------------------------------------------*/
size_t sf_encode(const sf_frame_t *f, uint8_t *out, size_t cap)
{
    uint16_t crc;
    size_t   n;

    if ((f == NULL) || (out == NULL) || (f->len > SF_MAX_PAYLOAD))
    {
        return 0u;
    }
    n = (size_t)SF_HEADER_LEN + f->len + 2u;
    if (cap < n)
    {
        return 0u;
    }

    out[0] = SF_MAGIC;
    out[1] = SF_VER;
    out[2] = f->type;
    out[3] = f->seq;
    out[4] = f->flags;
    out[5] = (uint8_t)(f->len & 0xFFu);
    out[6] = (uint8_t)(f->len >> 8);
    out[7] = f->cid;
    memcpy(&out[SF_HEADER_LEN], f->data, f->len);

    crc = proto_crc16(out, (size_t)SF_HEADER_LEN + f->len);
    out[SF_HEADER_LEN + f->len]      = (uint8_t)(crc >> 8);
    out[SF_HEADER_LEN + f->len + 1u] = (uint8_t)(crc & 0xFFu);
    return n;
}

size_t sf_build(uint8_t type, uint8_t cid, const uint8_t *data, size_t len,
                uint8_t *seq_counter, uint8_t *out, size_t cap)
{
    sf_frame_t f;

    if ((len > SF_MAX_PAYLOAD) || ((data == NULL) && (len != 0u)))
    {
        return 0u;
    }
    f.type  = type;
    f.seq   = (*seq_counter)++;
    f.flags = 0u;
    f.cid   = cid;
    f.len   = (uint16_t)len;
    if (len != 0u)
    {
        memcpy(f.data, data, len);
    }
    return sf_encode(&f, out, cap);
}

/* ---- segment assembly ------------------------------------------------------------*/
size_t sf_segment_append(uint8_t *seg, size_t seg_cap, size_t seg_used,
                         const sf_frame_t *f)
{
    size_t n = sf_encode(f, &seg[seg_used], seg_cap - seg_used);

    if (n == 0u)
    {
        return 0u;                                  /* does not fit         */
    }
    return seg_used + n;
}

size_t sf_segment_pad(uint8_t *seg, size_t used)
{
    size_t padded = (used + 3u) & ~3u;

    if (seg != NULL)
    {
        while (used < padded)
        {
            seg[used++] = 0x00u;
        }
    }
    return padded;
}

/* ---- SEQ window (22 §5.3) ----------------------------------------------------------*/
int sf_seq_ok(uint8_t seq, uint8_t *last)
{
    uint8_t delta = (uint8_t)(seq - *last);

    if ((delta >= 1u) && (delta <= 32u))
    {
        *last = seq;
        return 1;
    }
    return 0;
}
