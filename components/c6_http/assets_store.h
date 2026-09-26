/*
 * assets_store.h - gzip web assets served from the "assets" partition (LLDD 4.3)
 */
#ifndef C6_ASSETS_STORE_H
#define C6_ASSETS_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    char     name[16];       /* e.g. "index.html" (gzipped on the wire) */
    uint32_t offset;         /* absolute offset inside assets.bin       */
    uint32_t gz_len;
    uint32_t raw_len;
    uint32_t crc32;
} assets_entry_t;

/* Locate + validate the partition header. Falls back to the embedded page. */
esp_err_t assets_store_init(void);

/* Find an entry by path ("/index.html"). false when missing. */
bool assets_find(const char *path, assets_entry_t *out);

/* Read up to len bytes at entry-relative offset; returns bytes read. */
int assets_read(const assets_entry_t *e, uint32_t off, uint8_t *buf, size_t len);

/* True when the built-in fallback page must be served (empty partition). */
bool assets_embedded_only(void);

/* Embedded fallback page metadata (plain HTML, not gzipped). */
const uint8_t *assets_embedded_html(size_t *len);

/* CRC32 of everything already consumed, for on-the-fly verification. */
void assets_crc_reset(uint32_t *ctx);
void assets_crc_feed(uint32_t *ctx, const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* C6_ASSETS_STORE_H */
