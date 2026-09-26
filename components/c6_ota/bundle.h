/*
 * bundle.h - C6 firmware bundle framing (LLDD 4.7)
 *
 * Layout (all multi-byte fields little-endian):
 *   0   magic "C6FW"
 *   4   u8  fmt_ver = 1
 *   5   u8  flags    bit0 = has_assets
 *   6   u16 hdr_len  = 148
 *   8   u32 total_len (whole bundle)
 *   12  u32 c6_len
 *   16  32B c6_sha256
 *   48  u32 assets_len
 *   52  32B assets_sha256 (zeros when no assets)
 *   84  64B ed25519 signature over bytes [0, 84+32) i.e. the first 116 bytes
 *   148 c6.bin ... then assets.bin
 */
#ifndef C6_BUNDLE_H
#define C6_BUNDLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BUNDLE_HDR_LEN      148u
#define BUNDLE_SIGNED_LEN   84u   /* header minus the 64-byte signature */
#define BUNDLE_MAX_ASSETS   (512u * 1024u)
#define BUNDLE_MAX_C6       (3u * 1024u * 1024u)

typedef enum
{
    BUNDLE_IDLE = 0,
    BUNDLE_NEED_HDR,          /* accumulating the 148-byte header        */
    BUNDLE_C6_DATA,           /* header ok; streaming c6.bin             */
    BUNDLE_ASSETS_DATA,       /* streaming assets.bin                    */
    BUNDLE_DONE,
    BUNDLE_ERR_MAGIC,
    BUNDLE_ERR_VERSION,
    BUNDLE_ERR_SIG,           /* ed25519 signature invalid               */
    BUNDLE_ERR_SIZE,          /* declared sizes out of range             */
    BUNDLE_ERR_HASH,          /* payload digest mismatch                 */
    BUNDLE_ERR_TRUNCATED,     /* stream ended before total_len           */
} bundle_state_t;

typedef struct
{
    uint32_t c6_len;
    uint32_t assets_len;
    uint8_t  c6_sha[32];
    uint8_t  assets_sha[32];
} bundle_info_t;

/* opaque streaming context */
typedef struct bundle_ctx bundle_ctx_t;

/* pubkey = 32-byte ed25519 public key (embedded on target, test vectors on host) */
bundle_ctx_t *bundle_new(const uint8_t *pubkey, size_t pub_len);
void          bundle_free(bundle_ctx_t *ctx);

/* Feed one chunk; sink(cb_arg, off, data, len) is invoked for payload bytes
 * only (c6.bin first, then assets.bin) with GLOBAL stream offsets:
 * off is measured from the start of c6.bin, i.e. [0,c6_len) app,
 * [c6_len, c6_len+assets_len) asset blob. Returns state. */
bundle_state_t bundle_feed(bundle_ctx_t *ctx, const uint8_t *data, size_t len,
                           void *cb_arg,
                           int (*sink)(void *arg, uint32_t off, const uint8_t *d, size_t n));

/* Verify payload digests; only meaningful once state == BUNDLE_DONE. */
bundle_state_t bundle_finish(bundle_ctx_t *ctx);

void bundle_get_info(const bundle_ctx_t *ctx, bundle_info_t *out);
const char *bundle_state_str(bundle_state_t st);

#ifdef __cplusplus
}
#endif

#endif /* C6_BUNDLE_H */
