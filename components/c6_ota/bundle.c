/*
 * bundle.c - streaming bundle parser with signature + digest verification
 */
#include "bundle.h"

#include <stdlib.h>
#include <string.h>

#include "ed25519v.h"
#include "proto_frames.h"       /* proto_get_u16/u32 LE helpers */
#include "sha512.h"

struct bundle_ctx
{
    bundle_state_t state;
    bundle_info_t  info;

    const uint8_t *pubkey;
    size_t         pub_len;

    uint8_t  hdr[BUNDLE_HDR_LEN];
    size_t   hdr_got;

    size_t   total_got;
    size_t   blob_got;                       /* offset inside current blob */

    c6_sha512_ctx_t c6_hash;
    c6_sha512_ctx_t assets_hash;
};

bundle_ctx_t *bundle_new(const uint8_t *pubkey, size_t pub_len)
{
    bundle_ctx_t *b = (bundle_ctx_t *)calloc(1u, sizeof(bundle_ctx_t));
    if (b != NULL)
    {
        b->state   = BUNDLE_NEED_HDR;
        b->pubkey  = pubkey;
        b->pub_len = pub_len;
    }
    return b;
}

void bundle_free(bundle_ctx_t *ctx)
{
    free(ctx);
}

void bundle_get_info(const bundle_ctx_t *ctx, bundle_info_t *out)
{
    if ((ctx != NULL) && (out != NULL))
    {
        *out = ctx->info;
    }
}

const char *bundle_state_str(bundle_state_t st)
{
    switch (st)
    {
        case BUNDLE_DONE:       return "done";
        case BUNDLE_ERR_MAGIC:  return "magic";
        case BUNDLE_ERR_VERSION:return "version";
        case BUNDLE_ERR_SIG:    return "signature";
        case BUNDLE_ERR_SIZE:   return "size";
        case BUNDLE_ERR_HASH:   return "hash";
        case BUNDLE_ERR_TRUNCATED: return "truncated";
        default:                return "incomplete";
    }
}

static bundle_state_t bundle_parse_header(bundle_ctx_t *b)
{
    const uint8_t *h = b->hdr;
    uint32_t total, c6_len, assets_len;
    uint16_t hdr_len;

    if (memcmp(h, "C6FW", 4u) != 0)
    {
        return BUNDLE_ERR_MAGIC;
    }
    if (h[4] != 1u)
    {
        return BUNDLE_ERR_VERSION;
    }
    hdr_len = proto_get_u16(&h[6]);
    if (hdr_len != BUNDLE_HDR_LEN)
    {
        return BUNDLE_ERR_VERSION;
    }
    total      = proto_get_u32(&h[8]);
    c6_len     = proto_get_u32(&h[12]);
    assets_len = proto_get_u32(&h[48]);

    if ((total != (BUNDLE_HDR_LEN + c6_len + assets_len)) ||
        (c6_len == 0u) || (c6_len > BUNDLE_MAX_C6) ||
        (assets_len > BUNDLE_MAX_ASSETS) ||
        ((assets_len > 0u) && ((h[5] & 1u) == 0u)))
    {
        return BUNDLE_ERR_SIZE;
    }

    /* ed25519 over the first 84 bytes */
    uint8_t zeros[32] = { 0 };

    if ((b->pub_len != 32u) || (b->pubkey == NULL) ||
        (memcmp(b->pubkey, zeros, 32u) == 0))
    {
        return BUNDLE_ERR_SIG;                /* no key provisioned */
    }
    if (c6_ed25519_verify(b->pubkey, &h[84], h, BUNDLE_SIGNED_LEN) != 0)
    {
        return BUNDLE_ERR_SIG;
    }

    b->info.c6_len      = c6_len;
    b->info.assets_len  = assets_len;
    memcpy(b->info.c6_sha, &h[16], 32u);
    memcpy(b->info.assets_sha, &h[52], 32u);
    c6_sha512_init(&b->c6_hash);
    c6_sha512_init(&b->assets_hash);
    return BUNDLE_C6_DATA;
}

bundle_state_t bundle_feed(bundle_ctx_t *b, const uint8_t *data, size_t len,
                           void *cb_arg,
                           int (*sink)(void *arg, uint32_t off, const uint8_t *d, size_t n))
{
    if ((b == NULL) || ((data == NULL) && (len != 0u)))
    {
        return (b != NULL) ? b->state : BUNDLE_ERR_SIZE;
    }
    if (b->state >= BUNDLE_DONE)
    {
        return b->state;                      /* terminal, incl. errors */
    }

    while (len > 0u)
    {
        if (b->state == BUNDLE_NEED_HDR)
        {
            size_t n = BUNDLE_HDR_LEN - b->hdr_got;
            if (n > len)
            {
                n = len;
            }
            memcpy(&b->hdr[b->hdr_got], data, n);
            b->hdr_got += n;
            data += n;
            len  -= n;
            if (b->hdr_got == BUNDLE_HDR_LEN)
            {
                b->state = bundle_parse_header(b);
                if (b->state >= BUNDLE_DONE)
                {
                    return b->state;
                }
            }
            continue;
        }

        if (b->state == BUNDLE_C6_DATA)
        {
            size_t remain = b->info.c6_len - b->blob_got;
            size_t n = (len < remain) ? len : remain;
            c6_sha512_update(&b->c6_hash, data, n);
            /* sink offset is GLOBAL (measured from start of c6.bin) */
            if ((sink != NULL) && (sink(cb_arg, (uint32_t)b->total_got, data, n) != 0))
            {
                return (b->state = BUNDLE_ERR_SIZE);
            }
            b->blob_got += n;
            b->total_got += n;
            data += n;
            len  -= n;
            if (b->blob_got == (size_t)b->info.c6_len)
            {
                if (b->info.assets_len > 0u)
                {
                    b->state    = BUNDLE_ASSETS_DATA;
                    b->blob_got = 0u;
                }
                else
                {
                    b->state = BUNDLE_DONE;
                }
            }
            continue;
        }

        if (b->state == BUNDLE_ASSETS_DATA)
        {
            size_t remain = b->info.assets_len - b->blob_got;
            size_t n = (len < remain) ? len : remain;
            c6_sha512_update(&b->assets_hash, data, n);
            /* global offset: c6.bin has already streamed, blob_got restarted at 0 */
            if ((sink != NULL) && (sink(cb_arg, (uint32_t)b->total_got, data, n) != 0))
            {
                return (b->state = BUNDLE_ERR_SIZE);
            }
            b->blob_got += n;
            b->total_got += n;
            data += n;
            len  -= n;
            if (b->blob_got == (size_t)b->info.assets_len)
            {
                b->state = BUNDLE_DONE;
            }
            continue;
        }

        break;
    }
    return b->state;
}

bundle_state_t bundle_finish(bundle_ctx_t *b)
{
    uint8_t digest[C6_SHA512_DIGEST_LEN];   /* payload hash = SHA-512[:32] */

    if (b == NULL)
    {
        return BUNDLE_ERR_SIZE;
    }
    if (b->state != BUNDLE_DONE)
    {
        return (b->state = BUNDLE_ERR_TRUNCATED);
    }
    c6_sha512_final(&b->c6_hash, digest);
    if (memcmp(digest, b->info.c6_sha, 32u) != 0)
    {
        return (b->state = BUNDLE_ERR_HASH);
    }
    if (b->info.assets_len > 0u)
    {
        c6_sha512_final(&b->assets_hash, digest);
        if (memcmp(digest, b->info.assets_sha, 32u) != 0)
        {
            return (b->state = BUNDLE_ERR_HASH);
        }
    }
    return b->state;
}
