/*
 * assets_store.c - gzip web assets from the "assets" data partition
 *
 * assets.bin layout (tools/build_assets.py):
 *   "ASSETS" | u8 ver=1 | u8 rsv | u16 count | u32 total_len
 *   entry[count]: name[16] u32 off u32 gz_len u32 raw_len u32 crc32
 *   payload blobs (gzip streams, 4-byte aligned)
 */
#include "assets_store.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

static const char *TAG = "c6_assets";

#define ASSETS_HDR_MAGIC   "ASSETS"
#define ASSETS_VER         1u
#define ASSETS_MAX_ENTRIES 16u

/* fallback page: functional minimal controller, used until a real bundle is
 * written into the assets partition (coding-plan decision C10) */
static const char EMBEDDED_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>SmartDrive</title></head>"
"<body style='font-family:sans-serif;text-align:center;margin-top:3em'>"
"<h2>SmartDrive (embedded page)</h2>"
"<p>assets partition empty - full UI ships in the firmware bundle.</p>"
"<p><a href='/api/health'>/api/health</a> <a href='/api/diag'>/api/diag</a></p>"
"</body></html>";

typedef struct
{
    const esp_partition_t *part;
    assets_entry_t entries[ASSETS_MAX_ENTRIES];
    uint16_t count;
} assets_ctx_t;

static assets_ctx_t s_assets;

esp_err_t assets_store_init(void)
{
    s_assets.part  = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                              (esp_partition_type_t)0x40, "assets");
    s_assets.count = 0u;
    if (s_assets.part == NULL)
    {
        ESP_LOGW(TAG, "assets partition missing - embedded page only");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t hdr[16];
    esp_err_t err = esp_partition_read(s_assets.part, 0u, hdr, sizeof(hdr));
    if (err != ESP_OK)
    {
        return err;
    }
    if (memcmp(hdr, ASSETS_HDR_MAGIC, 6u) != 0)
    {
        ESP_LOGW(TAG, "assets partition empty - embedded page only");
        return ESP_ERR_INVALID_STATE;
    }
    if (hdr[6] != ASSETS_VER)
    {
        return ESP_ERR_INVALID_VERSION;
    }
    uint16_t count = (uint16_t)(hdr[8] | (hdr[9] << 8));
    if (count > ASSETS_MAX_ENTRIES)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t buf[ASSETS_MAX_ENTRIES * 32u];
    size_t need = (size_t)count * 32u;
    err = esp_partition_read(s_assets.part, 16u, buf, need);
    if (err != ESP_OK)
    {
        return err;
    }
    for (uint16_t i = 0u; i < count; i++)
    {
        const uint8_t *e = &buf[i * 32u];
        memcpy(s_assets.entries[i].name, e, 15u);
        s_assets.entries[i].name[15] = '\0';
        s_assets.entries[i].offset  = (uint32_t)e[16] | ((uint32_t)e[17] << 8) |
                                      ((uint32_t)e[18] << 16) | ((uint32_t)e[19] << 24);
        s_assets.entries[i].gz_len  = (uint32_t)e[20] | ((uint32_t)e[21] << 8) |
                                      ((uint32_t)e[22] << 16) | ((uint32_t)e[23] << 24);
        s_assets.entries[i].raw_len = (uint32_t)e[24] | ((uint32_t)e[25] << 8) |
                                      ((uint32_t)e[26] << 16) | ((uint32_t)e[27] << 24);
        s_assets.entries[i].crc32   = (uint32_t)e[28] | ((uint32_t)e[29] << 8) |
                                      ((uint32_t)e[30] << 16) | ((uint32_t)e[31] << 24);
    }
    s_assets.count = count;
    ESP_LOGI(TAG, "assets partition: %u entries", count);
    return ESP_OK;
}

bool assets_find(const char *path, assets_entry_t *out)
{
    if (path == NULL || path[0] == '/')
    {
        path = (path != NULL) ? &path[1] : path;         /* strip leading '/' */
    }
    for (uint16_t i = 0u; i < s_assets.count; i++)
    {
        if (strcmp(s_assets.entries[i].name, path) == 0)
        {
            if (out != NULL)
            {
                *out = s_assets.entries[i];
            }
            return true;
        }
    }
    return false;
}

int assets_read(const assets_entry_t *e, uint32_t off, uint8_t *buf, size_t len)
{
    if ((s_assets.part == NULL) || (e == NULL) || (buf == NULL) || (off > e->gz_len))
    {
        return -1;
    }
    size_t n = e->gz_len - off;
    if (n > len)
    {
        n = len;
    }
    esp_err_t err = esp_partition_read(s_assets.part, e->offset + off, buf, n);
    return (err == ESP_OK) ? (int)n : -1;
}

bool assets_embedded_only(void)
{
    return (s_assets.count == 0u);
}

const uint8_t *assets_embedded_html(size_t *len)
{
    if (len != NULL)
    {
        *len = sizeof(EMBEDDED_HTML) - 1u;
    }
    return (const uint8_t *)EMBEDDED_HTML;
}

void assets_crc_reset(uint32_t *ctx)
{
    if (ctx != NULL)
    {
        *ctx = 0u;
    }
}

void assets_crc_feed(uint32_t *ctx, const uint8_t *data, size_t len)
{
    if (ctx != NULL)
    {
        *ctx = esp_rom_crc32_le(*ctx, data, (size_t)len);
    }
}
