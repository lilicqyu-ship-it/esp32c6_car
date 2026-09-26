/*
 * net.c - softAP bring-up, Wi-Fi events, captive DNS + mDNS (LLDD 4.2)
 */
#include "net.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "captive_dns.h"
#include "mdns_lite.h"

static const char *TAG = "c6_net";

/* stored exclusively here; written by DPT (LLDD 4.8) */
#define NS_UP        "c6user"
#define KEY_UP_SSID  "uplink_ssid"
#define KEY_UP_PASS  "uplink_pass"

typedef struct
{
    net_clients_cb_t cb;
    int sta_count;
    char ip[16];
    bool ap_up;
} net_ctx_t;

static net_ctx_t s_net = { .ip = "192.168.4.1" };   /* default AP addr */

/* ---- reserved STA uplink (route B, LLDD 4.2) -------------------------------- */
esp_err_t net_load_uplink(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    nvs_handle_t h;
    esp_err_t err;
    size_t n;

    if (ssid == NULL || pass == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    err = nvs_open(NS_UP, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        return err;
    }
    n = ssid_cap;
    err = nvs_get_str(h, KEY_UP_SSID, ssid, &n);
    if (err == ESP_OK)
    {
        n = pass_cap;
        err = nvs_get_str(h, KEY_UP_PASS, pass, &n);
    }
    nvs_close(h);
    return err;
}

static void net_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT)
    {
        switch (id)
        {
            case WIFI_EVENT_AP_START:
                s_net.ap_up = true;
                ESP_LOGI(TAG, "AP up SSID clients cap ok");
                break;
            case WIFI_EVENT_AP_STOP:
                s_net.ap_up = false;
                break;
            case WIFI_EVENT_AP_STACONNECTED:
                s_net.sta_count++;
                ESP_LOGI(TAG, "STA joined (count=%d)", s_net.sta_count);
                if (s_net.cb != NULL)
                {
                    s_net.cb(s_net.sta_count);
                }
                break;
            case WIFI_EVENT_AP_STADISCONNECTED:
                if (s_net.sta_count > 0)
                {
                    s_net.sta_count--;
                }
                ESP_LOGI(TAG, "STA left (count=%d)", s_net.sta_count);
                if (s_net.cb != NULL)
                {
                    s_net.cb(s_net.sta_count);
                }
                break;
            default:
                break;      /* event callbacks only post state, no business (LLDD 4.2) */
        }
    }
    /*
     * AP address is static (192.168.4.1 default, kept in s_net.ip) - IDF 6.x
     * no longer emits IP_EVENT_AP_GOT_IP for the softAP interface.
     */
}

const char *net_ip_str(void)
{
    return s_net.ip;
}

int net_sta_count(void)
{
    return s_net.sta_count;
}

void net_on_ap_clients(net_clients_cb_t cb)
{
    s_net.cb = cb;
}

esp_err_t net_start(const net_cfg_t *cfg)
{
    esp_err_t err;
    wifi_init_config_t wicfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t ap_cfg = { 0 };
    esp_netif_t *ap_netif;

    err = esp_netif_init();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE))
    {
        return err;
    }
    err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE))
    {
        return err;
    }
    ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL)
    {
        return ESP_FAIL;
    }
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, net_event_handler, NULL);
    if (err != ESP_OK)
    {
        return err;
    }
    err = esp_wifi_init(&wicfg);
    if (err != ESP_OK)
    {
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_AP);      /* APSTA reserved for route B */
    if (err != ESP_OK)
    {
        return err;
    }

    strncpy((char *)ap_cfg.ap.ssid, cfg->ssid, sizeof(ap_cfg.ap.ssid) - 1u);
    strncpy((char *)ap_cfg.ap.password, cfg->password, sizeof(ap_cfg.ap.password) - 1u);
    ap_cfg.ap.ssid_len        = (uint8_t)strlen(cfg->ssid);
    ap_cfg.ap.channel         = cfg->channel;
    ap_cfg.ap.max_connection  = cfg->max_conn;
    ap_cfg.ap.authmode        = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.pmf_cfg.required = false;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK)
    {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK)
    {
        return err;
    }

    err = captive_dns_start(net_ip_str());
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "captive DNS failed: %s", esp_err_to_name(err));
    }
    err = mdns_lite_start("mycar", cfg->ssid, net_ip_str());
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "mDNS failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}
