/*
 * test_sf.c - SF frame codec tests (tc275_car doc 22 §5, G2 gate):
 * round trip, padding/multi-frame segments, parser error branches,
 * SEQ window, 10^7 random-byte fuzz, and v2<->SF mapping round trips
 * for the representative commands (mirror of link.c's tables).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../components/c6_sf/sf_frame.h"
#include "../../components/c6_proto/proto_frames.h"
#include "minunit.h"

static int test_crc_and_roundtrip(void)
{
    sf_frame_t f, out;
    sf_parser_t p;
    uint8_t wire[SF_MAX_FRAME];
    size_t n;
    int i;

    memset(&f, 0, sizeof(f));
    f.type = SF_TYPE_CMD; f.cid = SF_CID_DRV; f.seq = 0x42; f.flags = 0;
    f.len = 5;
    f.data[0] = PROTO_CMD_DRIVE;
    f.data[1] = 0x58; f.data[2] = 0x02;              /* v = 600 */
    f.data[3] = 0x2C; f.data[4] = 0x01;              /* w = 300 */

    n = sf_encode(&f, wire, sizeof(wire));
    MU_CHECK_EQ(n, (long long)(SF_HEADER_LEN + 5 + 2));

    sf_parser_init(&p);
    for (i = 0; i < (int)n - 1; i++)
    {
        MU_CHECK_EQ(sf_parser_feed(&p, wire[i], &out), SF_RX_NONE);
    }
    MU_CHECK_EQ(sf_parser_feed(&p, wire[n - 1], &out), SF_RX_FRAME);
    MU_CHECK_EQ(out.type, SF_TYPE_CMD);
    MU_CHECK_EQ(out.cid, SF_CID_DRV);
    MU_CHECK_EQ(out.seq, 0x42);
    MU_CHECK_EQ(out.len, 5);
    MU_CHECK(memcmp(out.data, f.data, 5) == 0);
    return 0;
}

static int test_segment_padding_and_multiframe(void)
{
    sf_frame_t a, b;
    uint8_t seg[SF_MAX_FRAME * 2];
    uint8_t seqc = 0;
    sf_parser_t p;
    sf_frame_t out;
    size_t used = 0u;
    int i;
    int frames = 0;

    MU_CHECK_EQ(sf_build(SF_TYPE_CMD, SF_CID_PAIR, NULL, 0, &seqc, (uint8_t[]){0}, 0), 0); /* cap=0 */

    /* frame A: 3-byte payload -> frame len 13 -> padded segment grows */
    memset(&a, 0, sizeof(a));
    a.type = SF_TYPE_CMD; a.cid = SF_CID_PAIR; a.seq = seqc++; a.len = 3;
    a.data[0] = 0x00; a.data[1] = 0x3C; a.data[2] = 0x01;
    used = sf_segment_append(seg, sizeof(seg), used, &a);
    MU_CHECK(used == 13);
    used = sf_segment_pad(seg, used);
    MU_CHECK_EQ(used, 16);                           /* 13 -> 16 pad */

    /* frame B appended after the padding, multi-frame in one segment */
    memset(&b, 0, sizeof(b));
    b.type = SF_TYPE_OTA_C; b.cid = SF_CID_OTA_SWAP; b.seq = seqc++; b.len = 0;
    used = sf_segment_append(seg, sizeof(seg), used, &b);
    MU_CHECK_EQ(used, 26);                           /* 16 + 10 (empty frame) */
    used = sf_segment_pad(seg, used);
    MU_CHECK_EQ(used, 28);                           /* 26 -> 28 pad */

    sf_parser_init(&p);
    for (i = 0; i < (int)used; i++)
    {
        if (sf_parser_feed(&p, seg[i], &out) == SF_RX_FRAME)
        {
            frames++;
        }
    }
    MU_CHECK_EQ(frames, 2);
    MU_CHECK_EQ(out.cid, SF_CID_OTA_SWAP);
    return 0;
}

static int test_parser_error_branches(void)
{
    sf_frame_t f, out;
    sf_parser_t p;
    uint8_t wire[SF_MAX_FRAME];
    size_t n;
    int i;

    memset(&f, 0, sizeof(f));
    f.type = SF_TYPE_TEL; f.cid = SF_CID_TELEMETRY; f.seq = 1; f.len = 38;

    /* CRC corruption */
    n = sf_encode(&f, wire, sizeof(wire));
    wire[10] ^= 0xFF;
    sf_parser_init(&p);
    for (i = 0; i < (int)n; i++)
    {
        sf_rx_ev_t ev = sf_parser_feed(&p, wire[i], &out);
        if (i == (int)n - 1) { MU_CHECK_EQ(ev, SF_RX_CRC_ERR); }
        else { MU_CHECK_EQ(ev, SF_RX_NONE); }
    }

    /* version error: must surface immediately at byte 2; the trailing bytes
     * then re-enter idle and raise FMT_ERR (garbage) - both are fine as long
     * as no frame is emitted */
    n = sf_encode(&f, wire, sizeof(wire));
    wire[1] = 0x02;
    sf_parser_init(&p);
    {
        sf_rx_ev_t ev = SF_RX_NONE;
        int saw_ver = 0;
        for (i = 0; i < (int)n; i++)
        {
            ev = sf_parser_feed(&p, wire[i], &out);
            if (ev == SF_RX_VER_ERR) { saw_ver = 1; }
            MU_CHECK(ev != SF_RX_FRAME);
        }
        MU_CHECK_EQ(saw_ver, 1);
    }

    /* oversize LEN */
    sf_parser_init(&p);
    memset(wire, 0, sizeof(wire));
    wire[0] = SF_MAGIC; wire[1] = SF_VER; wire[5] = 0xF9; wire[6] = 0x00;
    {
        sf_rx_ev_t ev = SF_RX_NONE;
        /* feed header; LEN>248 must raise FMT_ERR at header end */
        for (i = 0; i < 8; i++) { ev = sf_parser_feed(&p, wire[i], &out); }
        MU_CHECK_EQ(ev, SF_RX_FMT_ERR);
    }

    /* resync after garbage */
    n = sf_encode(&f, wire, sizeof(wire));
    sf_parser_init(&p);
    (void)sf_parser_feed(&p, 0x77, &out);
    (void)sf_parser_feed(&p, 0x00, &out);
    for (i = 0; i < (int)n; i++) { (void)sf_parser_feed(&p, wire[i], &out); }
    MU_CHECK_EQ(out.type, SF_TYPE_TEL);
    return 0;
}

static int test_seq_window(void)
{
    uint8_t last = 0;

    MU_CHECK_EQ(sf_seq_ok(1, &last), 1);
    MU_CHECK_EQ(sf_seq_ok(2, &last), 1);
    MU_CHECK_EQ(sf_seq_ok(2, &last), 0);             /* replay               */
    MU_CHECK_EQ(sf_seq_ok(200, &last), 0);           /* window jump > 32     */
    last = 250;
    MU_CHECK_EQ(sf_seq_ok(251, &last), 1);
    MU_CHECK_EQ(sf_seq_ok(3, &last), 1);             /* wrap 251->3 = 8      */
    MU_CHECK(sf_seq_ok(20, &last) == 1);
    MU_CHECK(sf_seq_ok(53, &last) == 0);             /* >32 across wrap      */
    return 0;
}

static int test_fuzz_10M(void)
{
    sf_parser_t p;
    sf_frame_t out;
    uint32_t rng = 0xDEADBEEFu;
    long i;

    sf_parser_init(&p);
    for (i = 0; i < 10000000L; i++)
    {
        rng = (rng * 1664525u) + 1013904223u;
        (void)sf_parser_feed(&p, (uint8_t)(rng >> 24), &out);
    }
    return 0;
}

/* ---- v2<->SF mapping round trips (link.c tables mirrored) ----------------- */

static size_t v2_to_sf_ref(const proto_frame_t *vf, sf_frame_t *sf, uint8_t *seq)
{
    /* mirror of link.c v2_to_sf for the covered subset */
    memset(sf, 0, sizeof(*sf));
    sf->seq = (*seq)++;
    if (vf->cmd == PROTO_CMD_DRIVE && vf->len >= 4)
    {
        sf->type = SF_TYPE_CMD; sf->cid = SF_CID_DRV; sf->len = 5;
        sf->data[0] = PROTO_CMD_DRIVE;
        memcpy(&sf->data[1], vf->data, 4);
        return 1;
    }
    if (vf->cmd == PROTO_CMD_PAIR)
    {
        sf->type = SF_TYPE_CMD; sf->cid = SF_CID_PAIR;
        sf->len = vf->len; memcpy(sf->data, vf->data, vf->len);
        return 1;
    }
    if (vf->cmd == PROTO_CMD_LINK_STATE)
    {
        sf->type = SF_TYPE_CMD; sf->cid = SF_CID_DIAG;
        sf->len = (uint16_t)(vf->len + 1); sf->data[0] = PROTO_CMD_LINK_STATE;
        memcpy(&sf->data[1], vf->data, vf->len);
        return 1;
    }
    if (vf->cmd >= PROTO_CMD_DPT_ENTER && vf->cmd <= PROTO_CMD_DPT_SELFTEST)
    {
        /* mirror of link.c L285: DPT family -> SF CMD/DPT, payload[0]=op */
        sf->type = SF_TYPE_CMD; sf->cid = SF_CID_DPT;
        sf->len  = (uint16_t)(vf->len + 1);
        sf->data[0] = vf->cmd;
        memcpy(&sf->data[1], vf->data, vf->len);
        return 1;
    }
    return 0;
}

static int test_mapping_roundtrip(void)
{
    proto_frame_t v;
    sf_frame_t s, out;
    uint8_t wire[SF_MAX_FRAME];
    uint8_t seq = 0;
    sf_parser_t p;
    size_t n, w;
    int i;

    /* DRIVE 0x50 {v=600, w=-300} -> SF CMD/DRV {op, v, w} */
    v.ver = PROTO_VER; v.cmd = PROTO_CMD_DRIVE; v.seq = 9; v.len = 4;
    proto_put_u16(&v.data[0], 600);
    proto_put_u16(&v.data[2], (uint16_t)-300);
    MU_CHECK_EQ(v2_to_sf_ref(&v, &s, &seq), 1);
    MU_CHECK_EQ(s.cid, SF_CID_DRV);
    MU_CHECK_EQ(s.data[0], PROTO_CMD_DRIVE);
    MU_CHECK_EQ(proto_get_u16(&s.data[1]), 600);
    MU_CHECK_EQ((int16_t)proto_get_u16(&s.data[3]), -300);

    /* wire round trip */
    n = sf_encode(&s, wire, sizeof(wire));
    sf_parser_init(&p);
    for (i = 0; i < (int)n; i++) { (void)sf_parser_feed(&p, wire[i], &out); }
    MU_CHECK_EQ(out.cid, SF_CID_DRV);
    MU_CHECK_EQ(proto_get_u16(&out.data[1]), 600);

    /* LINK_STATE 0x42 -> CMD/DIAG {op=0x42, state} */
    v.cmd = PROTO_CMD_LINK_STATE; v.len = 1; v.data[0] = 2;
    MU_CHECK_EQ(v2_to_sf_ref(&v, &s, &seq), 1);
    MU_CHECK_EQ(s.cid, SF_CID_DIAG);
    MU_CHECK_EQ(s.data[0], PROTO_CMD_LINK_STATE);
    MU_CHECK_EQ(s.data[1], 2);

    /* unmapped v2 command (0x61 CHUNK goes via the chunk fast path) */
    v.cmd = PROTO_CMD_OTA_CHUNK; v.len = 3;
    MU_CHECK_EQ(v2_to_sf_ref(&v, &s, &seq), 0);
    (void)w;
    return 0;
}

static int test_ota_chunk_layout(void)
{
    sf_frame_t f;
    uint8_t seq = 0;
    uint8_t data[SF_OTA_CHUNK_MAX];
    uint8_t wire[SF_MAX_FRAME];
    size_t n;
    int i;

    for (i = 0; i < SF_OTA_CHUNK_MAX; i++) { data[i] = (uint8_t)i; }
    f.type = SF_TYPE_OTA_D; f.cid = SF_CID_OTA_CHUNK; f.seq = seq++; f.flags = 0;
    f.len = (uint16_t)(2 + SF_OTA_CHUNK_MAX);
    proto_put_u16(&f.data[0], 1234);
    memcpy(&f.data[2], data, SF_OTA_CHUNK_MAX);

    n = sf_encode(&f, wire, sizeof(wire));
    MU_CHECK_EQ(n, (long long)(SF_HEADER_LEN + 242 + 2));
    MU_CHECK((n & 3u) == 0u);                        /* full chunk stays 4-aligned */
    return 0;
}

static int test_dpt_calib_frames(void)
{
    /* doc/17 V1.1 §8.4: DPT trigger family (page -> TC275) and result
     * events (TC275 -> page) must survive the SF wire container byte-exactly */
    proto_frame_t v;
    sf_frame_t s, out;
    uint8_t seq = 0;
    uint8_t wire[SF_MAX_FRAME];
    sf_parser_t p;
    size_t n;
    int i;

    /* 0x70 CAL_DIR, empty payload -> SF CMD/DPT {op=0x70} len=1 */
    v.ver = PROTO_VER; v.cmd = PROTO_CMD_DPT_ENTER; v.seq = 1; v.len = 0;
    MU_CHECK_EQ(v2_to_sf_ref(&v, &s, &seq), 1);
    MU_CHECK_EQ(s.cid, SF_CID_DPT);
    MU_CHECK_EQ(s.len, 1);
    MU_CHECK_EQ(s.data[0], PROTO_CMD_DPT_ENTER);

    /* 0x71 MOTOR_JOG {motor=2, duty=-500} -> DPT {0x71,2,0x0C,0xFE} len=4 */
    v.cmd = 0x71; v.len = 3;
    v.data[0] = 2; proto_put_u16(&v.data[1], (uint16_t)(int16_t)-500);
    MU_CHECK_EQ(v2_to_sf_ref(&v, &s, &seq), 1);
    MU_CHECK_EQ(s.len, 4);
    MU_CHECK_EQ(s.data[0], 0x71);
    MU_CHECK_EQ(s.data[1], 2);
    MU_CHECK_EQ((int16_t)proto_get_u16(&s.data[2]), -500);
    n = sf_encode(&s, wire, sizeof(wire));
    sf_parser_init(&p);
    for (i = 0; i < (int)n; i++) { (void)sf_parser_feed(&p, wire[i], &out); }
    MU_CHECK_EQ(out.type, SF_TYPE_CMD);
    MU_CHECK_EQ(out.cid, SF_CID_DPT);
    MU_CHECK_EQ(out.len, 4);
    MU_CHECK(memcmp(out.data, s.data, 4) == 0);

    /* 0x73 REC_SET 12B -> DPT len=13 (pos x4, invert x4, fullScale, wheelDia) */
    v.cmd = 0x73; v.len = 12;
    for (i = 0; i < 12; i++) { v.data[i] = (uint8_t)(i + 1); }
    MU_CHECK_EQ(v2_to_sf_ref(&v, &s, &seq), 1);
    MU_CHECK_EQ(s.len, 13);
    MU_CHECK(memcmp(s.data, "\x73", 1) == 0);

    /* EVT 0x22 cal result: {op,status,invert i8x4,delta i32x4 LE,saved} 23B */
    s.type = SF_TYPE_EVT; s.cid = SF_CID_DPT_RESULT; s.seq = seq++; s.flags = 0;
    s.len = 23; memset(s.data, 0, sizeof(s.data));
    s.data[0] = PROTO_CMD_DPT_ENTER; s.data[1] = 0;      /* op, status=done */
    s.data[2] = (uint8_t)-1;                              /* invert[0]       */
    proto_put_u32(&s.data[6],  (uint32_t)(int32_t)-1204); /* delta[0]        */
    proto_put_u32(&s.data[18], (uint32_t)(int32_t)987);   /* delta[3]        */
    s.data[22] = 1;                                       /* saved=DFlash    */
    n = sf_encode(&s, wire, sizeof(wire));
    sf_parser_init(&p);
    for (i = 0; i < (int)n; i++) { (void)sf_parser_feed(&p, wire[i], &out); }
    MU_CHECK_EQ(out.type, SF_TYPE_EVT);
    MU_CHECK_EQ(out.cid, SF_CID_DPT_RESULT);
    MU_CHECK_EQ(out.len, 23);
    MU_CHECK_EQ((int8_t)out.data[2], -1);
    MU_CHECK_EQ((int32_t)proto_get_u32(&out.data[6]), -1204);
    MU_CHECK_EQ((int32_t)proto_get_u32(&out.data[18]), 987);
    MU_CHECK_EQ(out.data[22], 1);

    /* EVT 0x23 record: {ver,src,pos x4,invert x4,fullScale,wheelDia,crcOk} 15B */
    s.type = SF_TYPE_EVT; s.cid = SF_CID_DPT_REC; s.seq = seq++; s.flags = 0;
    s.len = 15; memset(s.data, 0, sizeof(s.data));
    s.data[0] = 1; s.data[1] = 1;                         /* ver, src=DFlash  */
    s.data[2] = 0; s.data[3] = 2; s.data[4] = 3; s.data[5] = 1; /* A/B/C/D    */
    s.data[6] = (uint8_t)-1;
    proto_put_u16(&s.data[10], 3250);                     /* fullScale        */
    proto_put_u16(&s.data[12], 125);                      /* wheelDia         */
    s.data[14] = 1;                                       /* crcOk            */
    n = sf_encode(&s, wire, sizeof(wire));
    sf_parser_init(&p);
    for (i = 0; i < (int)n; i++) { (void)sf_parser_feed(&p, wire[i], &out); }
    MU_CHECK_EQ(out.cid, SF_CID_DPT_REC);
    MU_CHECK_EQ(out.len, 15);
    MU_CHECK_EQ(proto_get_u16(&out.data[10]), 3250);
    MU_CHECK_EQ(proto_get_u16(&out.data[12]), 125);
    MU_CHECK_EQ((int8_t)out.data[6], -1);
    return 0;
}

int main(void)
{
    MU_RUN(test_crc_and_roundtrip);
    MU_RUN(test_segment_padding_and_multiframe);
    MU_RUN(test_parser_error_branches);
    MU_RUN(test_seq_window);
    printf("fuzzing 10M random bytes...\n");
    MU_RUN(test_fuzz_10M);
    MU_RUN(test_mapping_roundtrip);
    MU_RUN(test_ota_chunk_layout);
    MU_RUN(test_dpt_calib_frames);
    MU_REPORT("sf");
    return (mu_failed != 0) ? 1 : 0;
}
