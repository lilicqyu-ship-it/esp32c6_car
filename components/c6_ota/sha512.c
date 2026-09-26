/*
 * sha512.c - self-contained SHA-512 (FIPS 180-4)
 */
#include "sha512.h"

#include <string.h>

#include "c6_consts.h"

#define ROR64(x, n) (((x) >> (n)) | ((x) << (64u - (n))))

void c6_sha512_init(c6_sha512_ctx_t *ctx)
{
    memcpy(ctx->h, C6_SHA512_IV, sizeof(ctx->h));
    ctx->len_bits = 0u;
    ctx->buf_len  = 0u;
}

static void sha512_block(c6_sha512_ctx_t *ctx, const uint8_t *p)
{
    uint64_t w[80];
    uint64_t a, b, c, d, e, f, g, h;
    int i;

    for (i = 0; i < 16; i++)
    {
        w[i] = ((uint64_t)p[i * 8u] << 56) | ((uint64_t)p[i * 8u + 1u] << 48) |
               ((uint64_t)p[i * 8u + 2u] << 40) | ((uint64_t)p[i * 8u + 3u] << 32) |
               ((uint64_t)p[i * 8u + 4u] << 24) | ((uint64_t)p[i * 8u + 5u] << 16) |
               ((uint64_t)p[i * 8u + 6u] << 8) | ((uint64_t)p[i * 8u + 7u]);
    }
    for (i = 16; i < 80; i++)
    {
        uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
    e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];

    for (i = 0; i < 80; i++)
    {
        uint64_t S1 = ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41);
        uint64_t ch = (e & f) ^ ((~e) & g);
        uint64_t t1 = h + S1 + ch + C6_SHA512_K[i] + w[i];
        uint64_t S0 = ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + maj;

        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;
}

void c6_sha512_update(c6_sha512_ctx_t *ctx, const uint8_t *data, size_t len)
{
    ctx->len_bits += (uint64_t)len * 8u;

    while (len > 0u)
    {
        size_t n = C6_SHA512_BLOCK_LEN - ctx->buf_len;
        if (n > len)
        {
            n = len;
        }
        memcpy(&ctx->buf[ctx->buf_len], data, n);
        ctx->buf_len += n;
        data += n;
        len -= n;
        if (ctx->buf_len == C6_SHA512_BLOCK_LEN)
        {
            sha512_block(ctx, ctx->buf);
            ctx->buf_len = 0u;
        }
    }
}

void c6_sha512_final(c6_sha512_ctx_t *ctx, uint8_t out[C6_SHA512_DIGEST_LEN])
{
    uint64_t bits = ctx->len_bits;
    uint8_t pad = 0x80u;
    uint8_t len_field[16] = { 0 };
    int i;

    c6_sha512_update(ctx, &pad, 1u);
    pad = 0x00u;
    while (ctx->buf_len != 112u)              /* 128 - 16-byte length field */
    {
        c6_sha512_update(ctx, &pad, 1u);
    }
    for (i = 0; i < 8; i++)
    {
        len_field[8 + i] = (uint8_t)(bits >> (56 - i * 8));
    }
    c6_sha512_update(ctx, len_field, 16u);    /* fills the block and flushes */

    for (i = 0; i < 8; i++)
    {
        for (int j = 7; j >= 0; j--)
        {
            out[i * 8u + (uint32_t)(7 - j)] = (uint8_t)(ctx->h[i] >> (j * 8));
        }
    }
}

void c6_sha512(const uint8_t *data, size_t len, uint8_t out[C6_SHA512_DIGEST_LEN])
{
    c6_sha512_ctx_t ctx;

    c6_sha512_init(&ctx);
    c6_sha512_update(&ctx, data, len);
    c6_sha512_final(&ctx, out);
}
