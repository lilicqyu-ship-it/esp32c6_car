/*
 * test_proto.c - proto v2 codec: CRC check value, encode/parse round trip,
 * every parser error branch, recovery after garbage, random fuzz (10^7).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "minunit.h"
#include "proto_frames.h"

static int test_crc_check_value(void)
{
    /* CRC-16/CCITT-FALSE check value (decision C5) */
    MU_CHECK_EQ(proto_crc16((const uint8_t *)"123456789", 9), 0x29B1);
    MU_CHECK_EQ(proto_crc16(NULL, 0), 0);
    return 0;
}

static int test_encode_parse_roundtrip(void)
{
    proto_frame_t f, out;
    proto_parser_t p;
    uint8_t wire[PROTO_MAX_FRAME];
    size_t n;
    int i;

    f.ver = PROTO_VER;
    f.cmd = PROTO_CMD_DRIVE;
    f.seq = 0x42;
    f.len = 4;
    f.data[0] = 0x34; f.data[1] = 0x12; f.data[2] = 0x00; f.data[3] = 0xF0;
    n = proto_encode(&f, wire, sizeof(wire));
    MU_CHECK_EQ(n, (long long)(PROTO_HEADER_LEN + 4 + 2));

    proto_parser_init(&p);
    for (i = 0; i < (int)n - 1; i++)
    {
        MU_CHECK_EQ(proto_parser_feed(&p, wire[i], &out), PROTO_RX_NONE);
    }
    MU_CHECK_EQ(proto_parser_feed(&p, wire[n - 1], &out), PROTO_RX_FRAME);
    MU_CHECK_EQ(out.cmd, PROTO_CMD_DRIVE);
    MU_CHECK_EQ(out.seq, 0x42);
    MU_CHECK_EQ(out.len, 4);
    MU_CHECK(memcmp(out.data, f.data, 4) == 0);
    return 0;
}

static int test_zero_length_frame(void)
{
    proto_frame_t out;
    proto_parser_t p;
    uint8_t wire[PROTO_MAX_FRAME];
    size_t n;
    int i;

    n = proto_build(PROTO_CMD_PING, 7, NULL, 0, wire, sizeof(wire));
    MU_CHECK_EQ(n, (long long)(PROTO_HEADER_LEN + 2));

    proto_parser_init(&p);
    for (i = 0; i < (int)n; i++)
    {
        proto_parser_feed(&p, wire[i], &out);
    }
    MU_CHECK_EQ(out.len, 0);
    MU_CHECK_EQ(out.cmd, PROTO_CMD_PING);
    return 0;
}

static int test_max_length_frame(void)
{
    uint8_t wire[PROTO_MAX_FRAME];
    uint8_t big[PROTO_MAX_PAYLOAD];
    size_t n;
    int i;

    for (i = 0; i < (int)PROTO_MAX_PAYLOAD; i++)
    {
        big[i] = (uint8_t)(i * 7 + 1);
    }
    n = proto_build(PROTO_CMD_DPT_SN_WRITE, 0x99, big, PROTO_MAX_PAYLOAD, wire, sizeof(wire));
    MU_CHECK(n > 0);

    proto_parser_t p;
    proto_parser_init(&p);
    proto_frame_t out;
    proto_rx_ev_t ev = PROTO_RX_NONE;
    for (i = 0; i < (int)n; i++)
    {
        ev = proto_parser_feed(&p, wire[i], &out);
    }
    MU_CHECK_EQ(ev, PROTO_RX_FRAME);
    MU_CHECK_EQ(out.len, PROTO_MAX_PAYLOAD);
    MU_CHECK(memcmp(out.data, big, PROTO_MAX_PAYLOAD) == 0);
    return 0;
}

static int test_oversize_len_rejected(void)
{
    proto_parser_t p;
    proto_frame_t out;

    proto_parser_init(&p);
    (void)proto_parser_feed(&p, PROTO_SYNC1, &out);
    (void)proto_parser_feed(&p, PROTO_SYNC2, &out);
    (void)proto_parser_feed(&p, PROTO_VER, &out);
    (void)proto_parser_feed(&p, 0x50, &out);
    (void)proto_parser_feed(&p, 0, &out);
    /* LEN = 65 > PROTO_MAX_PAYLOAD */
    MU_CHECK_EQ(proto_parser_feed(&p, PROTO_MAX_PAYLOAD + 1, &out), PROTO_RX_FMT_ERR);
    /* parser resynced: next valid frame must still parse */
    return 0;
}

static int test_bad_version_rejected(void)
{
    proto_parser_t p;
    proto_frame_t out;
    uint8_t wire[16];
    size_t n;
    int i;

    n = proto_build(PROTO_CMD_PING, 1, NULL, 0, wire, sizeof(wire));
    MU_CHECK(n > 0);
    wire[2] = 0x03;                            /* corrupt VER */

    proto_parser_init(&p);
    for (i = 0; i < (int)n; i++)
    {
        proto_rx_ev_t ev = proto_parser_feed(&p, wire[i], &out);
        MU_CHECK(ev != PROTO_RX_FRAME);
    }
    return 0;
}

static int test_crc_corruption_detected(void)
{
    proto_frame_t f, out;
    proto_parser_t p;
    uint8_t wire[PROTO_MAX_FRAME];
    size_t n;
    int i;

    f.ver = PROTO_VER; f.cmd = 0x50; f.seq = 1; f.len = 2;
    f.data[0] = 0x11; f.data[1] = 0x22;
    n = proto_encode(&f, wire, sizeof(wire));
    wire[6] ^= 0xFF;                            /* flip payload bit */

    proto_parser_init(&p);
    for (i = 0; i < (int)n; i++)
    {
        proto_rx_ev_t ev = proto_parser_feed(&p, wire[i], &out);
        if (i == (int)n - 1)
        {
            MU_CHECK_EQ(ev, PROTO_RX_CRC_ERR);
        }
        else
        {
            MU_CHECK_EQ(ev, PROTO_RX_NONE);
        }
    }
    return 0;
}

static int test_resync_after_garbage(void)
{
    proto_frame_t out;
    proto_parser_t p;
    uint8_t wire[PROTO_MAX_FRAME];
    size_t n;
    int i;
    const uint8_t junk[] = { 0x00, 0xFF, 0xAA, 0x12, 0x55, 0x55, 0x77 };

    n = proto_build(PROTO_CMD_PING, 9, NULL, 0, wire, sizeof(wire));
    MU_CHECK(n > 0);

    proto_parser_init(&p);
    for (i = 0; i < (int)(sizeof(junk)); i++)
    {
        (void)proto_parser_feed(&p, junk[i], &out);
    }
    for (i = 0; i < (int)n; i++)
    {
        (void)proto_parser_feed(&p, wire[i], &out);
    }
    MU_CHECK_EQ(out.cmd, PROTO_CMD_PING);
    MU_CHECK_EQ(out.seq, 9);
    return 0;
}

static int test_encode_arg_validation(void)
{
    proto_frame_t f;
    uint8_t small[8];
    uint8_t wire[PROTO_MAX_FRAME];

    f.ver = PROTO_VER; f.cmd = 1; f.seq = 0; f.len = 10;
    MU_CHECK_EQ(proto_encode(NULL, wire, sizeof(wire)), 0);
    MU_CHECK_EQ(proto_encode(&f, NULL, sizeof(wire)), 0);
    MU_CHECK_EQ(proto_encode(&f, wire, 5), 0);            /* cap too small */
    f.len = PROTO_MAX_PAYLOAD + 1;                        /* illegal len  */
    MU_CHECK_EQ(proto_encode(&f, wire, sizeof(wire)), 0);
    f.len = 2;
    MU_CHECK_EQ(proto_build(1, 0, NULL, 3, wire, sizeof(wire)), 0); /* NULL data */
    MU_CHECK_EQ(proto_build(1, 0, NULL, 0, small, 4), 0); /* cap too small */
    return 0;
}

static int test_telemetry_roundtrip(void)
{
    proto_telemetry_t t, d;
    uint8_t buf[PROTO_TELEMETRY_LEN];
    size_t n;

    memset(&t, 0, sizeof(t));
    t.seq = 0xDEADBEEFu;
    t.uptime_ms = 123456789u;
    t.state = 0x0A;
    t.fault_code = 0x8001;
    t.v_target_l = -321;
    t.v_target_r = 400;
    t.v_meas_l = -300;
    t.v_meas_r = 390;
    t.battery_mv = 7400;
    t.battery_pct = 88;
    t.odo_session_mm = 1000000u;
    t.odo_total_mm = 0xFFFFFFFFu;
    t.link_rtt_ms = 12;
    t.link_err_rate = 1;
    t.fw_ver = 0x00010002u;
    t.hw_rev = 3;

    n = proto_telemetry_encode(&t, buf, sizeof(buf));
    MU_CHECK_EQ(n, PROTO_TELEMETRY_LEN);
    MU_CHECK_EQ(proto_telemetry_decode(buf, sizeof(buf), &d), 0);
    MU_CHECK_EQ(d.seq, (long long)t.seq);
    MU_CHECK_EQ(d.v_target_l, t.v_target_l);
    MU_CHECK_EQ(d.v_meas_r, t.v_meas_r);
    MU_CHECK_EQ(d.battery_mv, t.battery_mv);
    MU_CHECK_EQ(d.odo_total_mm, (long long)t.odo_total_mm);
    MU_CHECK_EQ(d.fw_ver, (long long)t.fw_ver);
    MU_CHECK_EQ(proto_telemetry_decode(buf, PROTO_TELEMETRY_LEN - 1, &d), -1);
    MU_CHECK_EQ(proto_telemetry_encode(&t, buf, PROTO_TELEMETRY_LEN - 1), 0);
    return 0;
}

/* random-frame fuzz: 10^7 feeds must never crash or yield a bad frame */
static int test_fuzz_10M(void)
{
    proto_parser_t p;
    proto_frame_t out;
    uint32_t rng = 0x12345678u;
    long i;

    proto_parser_init(&p);
    for (i = 0; i < 10000000L; i++)
    {
        rng = (rng * 1664525u) + 1013904223u;
        uint8_t byte = (uint8_t)(rng >> 24);
        (void)proto_parser_feed(&p, byte, &out);
    }
    return 0;
}

/* structured fuzz: random valid frames with random single-bit corruption */
static int test_fuzz_corruption(void)
{
    proto_frame_t f, out;
    proto_parser_t p;
    uint8_t wire[PROTO_MAX_FRAME];
    uint32_t rng = 0xCAFEBABEu;
    int round;
    long good = 0;

    for (round = 0; round < 50000; round++)
    {
        size_t len = (rng >> 8) % (PROTO_MAX_PAYLOAD + 1u);
        size_t n;
        int i;

        rng = (rng * 1103515245u) + 12345u;
        for (i = 0; i < (int)len; i++)
        {
            rng = (rng * 1664525u) + 1013904223u;
            f.data[i] = (uint8_t)(rng >> 24);
        }
        f.ver = PROTO_VER;
        f.cmd = (uint8_t)(rng >> 16);
        f.seq = (uint8_t)(rng >> 24);
        f.len = (uint8_t)len;
        n = proto_encode(&f, wire, sizeof(wire));
        if (n == 0u)
        {
            return -1;
        }
        /* corrupt one random byte in one of three rounds */
        rng = (rng * 1664525u) + 1013904223u;
        if ((round % 3) == 0)
        {
            wire[rng % n] ^= (uint8_t)(1u << (rng >> 28));
        }
        proto_parser_init(&p);
        proto_rx_ev_t ev = PROTO_RX_NONE;
        for (i = 0; i < (int)n; i++)
        {
            ev = proto_parser_feed(&p, wire[i], &out);
        }
        if (ev == PROTO_RX_FRAME)
        {
            good++;
            if (memcmp(out.data, f.data, f.len) != 0)
            {
                return -1;                     /* false accept: never allowed */
            }
        }
    }
    if (good == 0)
    {
        return -1;                             /* sanity: some must survive */
    }
    return 0;
}

int main(void)
{
    MU_RUN(test_crc_check_value);
    MU_RUN(test_encode_parse_roundtrip);
    MU_RUN(test_zero_length_frame);
    MU_RUN(test_max_length_frame);
    MU_RUN(test_oversize_len_rejected);
    MU_RUN(test_bad_version_rejected);
    MU_RUN(test_crc_corruption_detected);
    MU_RUN(test_resync_after_garbage);
    MU_RUN(test_encode_arg_validation);
    MU_RUN(test_telemetry_roundtrip);
    printf("fuzzing 10M random bytes...\n");
    MU_RUN(test_fuzz_10M);
    MU_RUN(test_fuzz_corruption);
    MU_REPORT("proto");
    return (mu_failed != 0) ? 1 : 0;
}
