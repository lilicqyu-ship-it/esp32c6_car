/*
 * factory.h - factory data in encrypted NVS (LLDD 4.8)
 *
 * Single read/write gateway for SN, Wi-Fi credentials, pairing salt, RF
 * channel, log level and the pre-paired token table.  No other component may
 * touch the "c6fact" namespace.
 */
#ifndef C6_FACTORY_H
#define C6_FACTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACTORY_SN_LEN          16u
#define FACTORY_WIFI_PASS_MAX   64u
#define FACTORY_SALT_LEN        16u
#define FACTORY_PPT_COUNT       4u                       /* pre-paired tokens */
#define FACTORY_TOKEN_HASH_LEN  16u                      /* truncated sha256  */

typedef struct
{
    char     sn[FACTORY_SN_LEN + 1u];                    /* NUL-terminated */
    char     wifi_pass[FACTORY_WIFI_PASS_MAX];           /* NUL-terminated */
    uint8_t  salt[FACTORY_SALT_LEN];
    uint8_t  channel;                                    /* 1/6/11, def 6  */
    uint8_t  log_level;
    char     uplink_ssid[33u];                           /* reserved (B路线) */
    char     uplink_pass[FACTORY_WIFI_PASS_MAX];
    uint8_t  ppt[FACTORY_PPT_COUNT][FACTORY_TOKEN_HASH_LEN]; /* pre-paired */
    bool     have_sn;
    bool     have_pass;
} factory_data_t;

/* Open/init NVS namespace. Call once from app_main after nvs_flash_init. */
esp_err_t factory_init(void);

/* Load a snapshot into *out (always fully populated, defaults where absent). */
esp_err_t factory_load(factory_data_t *out);

/* SSID derived from SN: "SD-" + last 6 chars (or "SD-000000" w/o digits). */
void factory_ap_ssid(const factory_data_t *f, char *out, size_t cap);

/* Individual writers (production-test path only, LLDD 4.8). */
esp_err_t factory_write_sn(const char *sn);
esp_err_t factory_write_wifi_pass(const char *pass);
esp_err_t factory_write_channel(uint8_t ch);
esp_err_t factory_write_log_level(uint8_t level);
esp_err_t factory_write_ppt(uint8_t slot, const uint8_t token_hash[FACTORY_TOKEN_HASH_LEN]);

/* Session token persistence (pair grace, LLDD 4.4): hash + absolute expiry. */
esp_err_t factory_save_session(const uint8_t hash[FACTORY_TOKEN_HASH_LEN], int64_t expiry_us);
esp_err_t factory_load_session(uint8_t hash[FACTORY_TOKEN_HASH_LEN], int64_t *expiry_us);
esp_err_t factory_clear_session(void);

#ifdef __cplusplus
}
#endif

#endif /* C6_FACTORY_H */
