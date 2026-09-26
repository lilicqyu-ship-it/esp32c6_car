/*
 * factory.c - factory data in encrypted NVS (LLDD 4.8)
 */
#include "factory.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#define NS            "c6fact"
#define KEY_SN        "sn"
#define KEY_PASS      "wifi_pass"
#define KEY_SALT      "pair_salt"
#define KEY_CH        "ch"
#define KEY_LOG       "log_level"
#define KEY_UP_SSID   "uplink_ssid"
#define KEY_UP_PASS   "uplink_pass"
#define KEY_SESS_HASH "sess_tok"
#define KEY_SESS_EXP  "sess_exp"

esp_err_t factory_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err;
}

static const char *ppt_key(int i)
{
    static const char *keys[FACTORY_PPT_COUNT] = { "ppt0", "ppt1", "ppt2", "ppt3" };
    return ((i >= 0) && (i < FACTORY_PPT_COUNT)) ? keys[i] : keys[0];
}

esp_err_t factory_load(factory_data_t *out)
{
    nvs_handle_t h;
    esp_err_t err;
    size_t n;

    if (out == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->channel = 6;                                   /* LLDD 4.2 default */

    err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        return err;                                     /* caller decides FACTORY_WAIT */
    }

    n = sizeof(out->sn);
    if (nvs_get_str(h, KEY_SN, out->sn, &n) == ESP_OK && n > 1u)
    {
        out->have_sn = true;
    }
    n = sizeof(out->wifi_pass);
    if (nvs_get_str(h, KEY_PASS, out->wifi_pass, &n) == ESP_OK && n > 1u)
    {
        out->have_pass = true;
    }
    n = FACTORY_SALT_LEN;
    (void)nvs_get_blob(h, KEY_SALT, out->salt, &n);
    (void)nvs_get_u8(h, KEY_CH, &out->channel);
    (void)nvs_get_u8(h, KEY_LOG, &out->log_level);

    n = sizeof(out->uplink_ssid);
    (void)nvs_get_str(h, KEY_UP_SSID, out->uplink_ssid, &n);
    n = sizeof(out->uplink_pass);
    (void)nvs_get_str(h, KEY_UP_PASS, out->uplink_pass, &n);

    for (int i = 0; i < FACTORY_PPT_COUNT; i++)
    {
        n = FACTORY_TOKEN_HASH_LEN;
        (void)nvs_get_blob(h, ppt_key(i), out->ppt[i], &n);
    }

    nvs_close(h);
    return ESP_OK;
}

void factory_ap_ssid(const factory_data_t *f, char *out, size_t cap)
{
    const char *sn = (f != NULL && f->have_sn) ? f->sn : "";
    size_t sn_len = strlen(sn);

    if ((out == NULL) || (cap == 0u))
    {
        return;
    }
    if (sn_len >= 6u)
    {
        (void)snprintf(out, cap, "SD-%.6s", &sn[sn_len - 6u]);
    }
    else
    {
        (void)snprintf(out, cap, "SD-%.6s", sn);
    }
}

esp_err_t factory_write_sn(const char *sn)
{
    nvs_handle_t h;
    esp_err_t err;

    if ((sn == NULL) || (strlen(sn) == 0u) || (strlen(sn) > FACTORY_SN_LEN))
    {
        return ESP_ERR_INVALID_ARG;
    }
    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_str(h, KEY_SN, sn);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t factory_write_wifi_pass(const char *pass)
{
    nvs_handle_t h;
    esp_err_t err;
    size_t n = (pass != NULL) ? strlen(pass) : 0u;

    if ((n < 8u) || (n >= FACTORY_WIFI_PASS_MAX))       /* WPA2 needs >= 8 */
    {
        return ESP_ERR_INVALID_ARG;
    }
    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_str(h, KEY_PASS, pass);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t factory_write_channel(uint8_t ch)
{
    nvs_handle_t h;
    esp_err_t err;

    if ((ch != 1u) && (ch != 6u) && (ch != 11u))
    {
        return ESP_ERR_INVALID_ARG;
    }
    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_u8(h, KEY_CH, ch);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t factory_write_log_level(uint8_t level)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);

    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_u8(h, KEY_LOG, level);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t factory_write_ppt(uint8_t slot, const uint8_t token_hash[FACTORY_TOKEN_HASH_LEN])
{
    nvs_handle_t h;
    esp_err_t err;

    if ((slot >= FACTORY_PPT_COUNT) || (token_hash == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_blob(h, ppt_key((int)slot), token_hash, FACTORY_TOKEN_HASH_LEN);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t factory_save_session(const uint8_t hash[FACTORY_TOKEN_HASH_LEN], int64_t expiry_us)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);

    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_set_blob(h, KEY_SESS_HASH, hash, FACTORY_TOKEN_HASH_LEN);
    if (err == ESP_OK)
    {
        err = nvs_set_i64(h, KEY_SESS_EXP, expiry_us);
    }
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t factory_load_session(uint8_t hash[FACTORY_TOKEN_HASH_LEN], int64_t *expiry_us)
{
    nvs_handle_t h;
    esp_err_t err;
    size_t n = FACTORY_TOKEN_HASH_LEN;

    if ((hash == NULL) || (expiry_us == NULL))
    {
        return ESP_ERR_INVALID_ARG;
    }
    err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        return err;
    }
    err = nvs_get_blob(h, KEY_SESS_HASH, hash, &n);
    if (err == ESP_OK)
    {
        err = nvs_get_i64(h, KEY_SESS_EXP, expiry_us);
    }
    nvs_close(h);
    return err;
}

esp_err_t factory_clear_session(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);

    if (err != ESP_OK)
    {
        return err;
    }
    (void)nvs_erase_key(h, KEY_SESS_HASH);
    (void)nvs_erase_key(h, KEY_SESS_EXP);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
