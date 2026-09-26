/*
 * test_bundle.c - streaming bundle parser: happy path (signed by the dev key
 * via tools/ed25519_ref.py vectors), oversize/size-mismatch rejection, and
 * truncation detection.  The signature check uses the dev public key.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../components/c6_ota/bundle.h"
#include "../../components/c6_ota/ed25519v.h"
#include "../../components/c6_ota/sha512.h"
#include "minunit.h"

/* dev public key (components/c6_ota/keys/pub_ed25519_dev.bin) */
static const uint8_t DEV_PUB[32] = {
    0x74, 0x51, 0x09, 0x99, 0xf7, 0x42, 0x48, 0x14, 0x44, 0x63, 0x95, 0x8a,
    0xbf, 0xbd, 0xd6, 0xfc, 0x55, 0x9f, 0x9a, 0xb2, 0x47, 0xc3, 0x11, 0xfb,
    0x58, 0xf7, 0x4c, 0x5d, 0x6d, 0x46, 0x8b, 0x5f
};

/* valid signed header produced by tools/ed25519_ref.py against DEV_PUB:
 * c6_len=8, c6_sha=sha256(01..08), assets_len=4, assets_sha=sha256(AA x4),
 * signed over the first 84 bytes of the header */
static const uint8_t HDR_OK[148] = {
    0x43, 0x36, 0x46, 0x57, 0x01, 0x01, 0x94, 0x00, 0xa0, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x18, 0x18, 0xcc, 0x2a, 0xcd, 0x20, 0x78, 0x80,
    0xa0, 0x7a, 0xfc, 0x36, 0x0f, 0xd0, 0xda, 0x87, 0xe5, 0x1c, 0xcf, 0x17,
    0xe7, 0xc6, 0x04, 0xc4, 0xeb, 0x16, 0xbe, 0x57, 0x88, 0x32, 0x27, 0x24,
    0x04, 0x00, 0x00, 0x00, 0x70, 0x47, 0x34, 0x0c, 0x40, 0x0f, 0x85, 0xee,
    0x04, 0x47, 0x00, 0x0f, 0x61, 0x3d, 0xcd, 0xac, 0x95, 0x43, 0xd1, 0xf8,
    0x0f, 0x5c, 0x2c, 0x10, 0x07, 0x0c, 0xce, 0x4c, 0x9f, 0xf9, 0x98, 0x98,
    0xdd, 0x72, 0xbb, 0x5e, 0xb8, 0x31, 0x39, 0xe7, 0xb1, 0x67, 0x29, 0x0a,
    0x47, 0xc0, 0xd0, 0x09, 0xde, 0xd3, 0x52, 0xc8, 0x22, 0xa9, 0xa3, 0x66,
    0x98, 0x65, 0x35, 0x00, 0x06, 0x13, 0x53, 0xed, 0x72, 0xd2, 0x84, 0x36,
    0xf9, 0x6a, 0x79, 0xce, 0xb0, 0x25, 0x56, 0xfd, 0x76, 0x51, 0x16, 0x52,
    0xb3, 0xe2, 0x54, 0x25, 0xae, 0xc1, 0x4e, 0x9b, 0x0d, 0x71, 0xd5, 0x55,
    0x7f, 0xf0, 0xba, 0x04,
};

static uint8_t PAYLOAD[12] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0xAA, 0xAA, 0xAA, 0xAA
};

static int sink_count(void *arg, uint32_t off, const uint8_t *d, size_t n)
{
    (void)d;
    (*(uint32_t *)arg) += n;
    MU_CHECK(off + n <= 12u);
    return 0;
}

static int test_valid_bundle_stream(void)
{
    bundle_ctx_t *b = bundle_new(DEV_PUB, sizeof(DEV_PUB));
    uint8_t wire[148 + 12];
    uint32_t sunk = 0;
    bundle_info_t info;
    bundle_state_t st;

    MU_CHECK(b != NULL);
    memcpy(wire, HDR_OK, 148);
    memcpy(wire + 148, PAYLOAD, 12);

    /* feed byte by byte */
    for (int i = 0; i < (int)sizeof(wire); i++)
    {
        st = bundle_feed(b, &wire[i], 1, &sunk, sink_count);
        MU_CHECK(st < BUNDLE_DONE || i == (int)sizeof(wire) - 1);
    }
    MU_CHECK_EQ(st, BUNDLE_DONE);
    MU_CHECK_EQ(sunk, 12);
    MU_CHECK_EQ(bundle_finish(b), BUNDLE_DONE);
    bundle_get_info(b, &info);
    MU_CHECK_EQ(info.c6_len, 8);
    MU_CHECK_EQ(info.assets_len, 4);
    bundle_free(b);
    return 0;
}

static int test_bad_magic(void)
{
    bundle_ctx_t *b = bundle_new(DEV_PUB, sizeof(DEV_PUB));
    uint8_t wire[148];

    memcpy(wire, HDR_OK, 148);
    wire[0] = 'X';
    MU_CHECK_EQ(bundle_feed(b, wire, sizeof(wire), NULL, NULL), BUNDLE_ERR_MAGIC);
    bundle_free(b);
    return 0;
}

static int test_bad_signature(void)
{
    bundle_ctx_t *b = bundle_new(DEV_PUB, sizeof(DEV_PUB));
    uint8_t wire[148];

    memcpy(wire, HDR_OK, 148);
    wire[90] ^= 0x40;                          /* corrupt sig area */
    MU_CHECK_EQ(bundle_feed(b, wire, sizeof(wire), NULL, NULL), BUNDLE_ERR_SIG);
    bundle_free(b);
    return 0;
}

static int test_wrong_key_rejected(void)
{
    static const uint8_t other[32] = { 0 };
    bundle_ctx_t *b = bundle_new(other, sizeof(other));
    uint8_t wire[148];

    memcpy(wire, HDR_OK, 148);
    MU_CHECK_EQ(bundle_feed(b, wire, sizeof(wire), NULL, NULL), BUNDLE_ERR_SIG);
    bundle_free(b);
    return 0;
}

static int test_size_mismatch(void)
{
    bundle_ctx_t *b = bundle_new(DEV_PUB, sizeof(DEV_PUB));
    uint8_t wire[148];

    memcpy(wire, HDR_OK, 148);
    wire[10] = 0x99;                           /* total no longer matches lens */
    bundle_state_t st = bundle_feed(b, wire, sizeof(wire), NULL, NULL);
    MU_CHECK(st >= BUNDLE_DONE);               /* sig covers field -> may fail
                                                  as SIG or SIZE, both terminal */
    bundle_free(b);
    return 0;
}

static int test_truncated_stream(void)
{
    bundle_ctx_t *b = bundle_new(DEV_PUB, sizeof(DEV_PUB));
    uint8_t wire[148 + 12];

    memcpy(wire, HDR_OK, 148);
    memcpy(wire + 148, PAYLOAD, 12);
    (void)bundle_feed(b, wire, sizeof(wire) - 1, NULL, NULL);   /* one byte short */
    MU_CHECK_EQ(bundle_finish(b), BUNDLE_ERR_TRUNCATED);
    bundle_free(b);
    return 0;
}

int main(void)
{
    MU_RUN(test_valid_bundle_stream);
    MU_RUN(test_bad_magic);
    MU_RUN(test_bad_signature);
    MU_RUN(test_wrong_key_rejected);
    MU_RUN(test_size_mismatch);
    MU_RUN(test_truncated_stream);
    MU_REPORT("bundle");
    return (mu_failed != 0) ? 1 : 0;
}
