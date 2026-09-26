/*
 * test_sha512.c - known-answer vectors (digests generated with Python
 * hashlib at coding time; "abc" is also FIPS 180-4's own example).
 */
#include <stdio.h>
#include <string.h>

#include "../../components/c6_ota/sha512.h"
#include "minunit.h"

static int hexeq(const uint8_t *d, const char *hex)
{
    char out[C6_SHA512_DIGEST_LEN * 2 + 1];
    static const char hx[] = "0123456789abcdef";
    int i;

    for (i = 0; i < C6_SHA512_DIGEST_LEN; i++)
    {
        out[i * 2]     = hx[d[i] >> 4];
        out[i * 2 + 1] = hx[d[i] & 0xF];
    }
    out[C6_SHA512_DIGEST_LEN * 2] = '\0';
    if (strcmp(out, hex) != 0)
    {
        printf("  digest mismatch:\n    got      %s\n    expected %s\n", out, hex);
        return 0;
    }
    return 1;
}

static int test_abc(void)
{
    uint8_t d[C6_SHA512_DIGEST_LEN];
    c6_sha512((const uint8_t *)"abc", 3, d);
    MU_CHECK(hexeq(d,
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
    return 0;
}

static int test_empty(void)
{
    uint8_t d[C6_SHA512_DIGEST_LEN];
    c6_sha512((const uint8_t *)"", 0, d);
    MU_CHECK(hexeq(d,
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"));
    return 0;
}

static int test_long_message(void)
{
    /* "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
     * "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu" (FIPS 180-4) */
    static const char msg[] =
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
        "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    uint8_t d[C6_SHA512_DIGEST_LEN];
    c6_sha512((const uint8_t *)msg, strlen(msg), d);
    MU_CHECK(hexeq(d,
        "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
        "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909"));
    return 0;
}

static int test_streaming_matches_oneshot(void)
{
    uint8_t buf[1000];
    uint8_t a[C6_SHA512_DIGEST_LEN], b[C6_SHA512_DIGEST_LEN];
    c6_sha512_ctx_t ctx;
    size_t off = 0;
    size_t chunk = 37;

    for (int i = 0; i < (int)sizeof(buf); i++)
    {
        buf[i] = (uint8_t)(i * 13 + 5);
    }
    c6_sha512(buf, sizeof(buf), a);
    c6_sha512_init(&ctx);
    while (off < sizeof(buf))
    {
        size_t n = (sizeof(buf) - off < chunk) ? (sizeof(buf) - off) : chunk;
        c6_sha512_update(&ctx, buf + off, n);
        off += n;
    }
    c6_sha512_final(&ctx, b);
    MU_CHECK(memcmp(a, b, C6_SHA512_DIGEST_LEN) == 0);
    return 0;
}

int main(void)
{
    MU_RUN(test_abc);
    MU_RUN(test_empty);
    MU_RUN(test_long_message);
    MU_RUN(test_streaming_matches_oneshot);
    MU_REPORT("sha512");
    return (mu_failed != 0) ? 1 : 0;
}
