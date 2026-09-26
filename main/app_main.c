/*
 * app_main.c - ESP32-C6 network coprocessor: boot orchestration (LLDD 4.1)
 *
 * Composition root: this file is the only place that wires components
 * together (keeps the star dependency rule of LLDD 2.2 true).
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_state.h"
#include "bridge.h"
#include "factory.h"
#include "http_server.h"
#include "link.h"
#include "net.h"
#include "ota_self.h"
#include "pair.h"
#include "proto_frames.h"

#ifdef CONFIG_C6_MAINT_BLE
#include "maint_ble.h"
#endif
#ifdef CONFIG_C6_LEGACY_TCP
#include "legacy_tcp.h"
#endif

static const char *TAG = "c6_main";

/* ---- WS command entry: c6_http already enforced role+SEQ (first gate) ------- */
static void on_ws_binary(const proto_frame_t *f, int sd)
{
    if (bridge_post_cmd(f, sd) != ESP_OK)
    {
        (void)ws_send_ctl(sd, "{\"t\":\"err\",\"e\":\"busy\"}");
    }
}

/* ---- upload sinks ------------------------------------------------------------ */

static const http_upload_sink_t SINK_SELF = {
    .begin  = ota_self_begin,
    .feed   = ota_self_feed,
    .finish = ota_self_finish,
    .abort  = ota_self_abort,
};

static const http_upload_sink_t SINK_RELAY = {
    .begin  = ota_relay_begin,
    .feed   = ota_relay_feed,
    .finish = ota_relay_finish,
    .abort  = ota_relay_abort,
};

/* net counts AP stations, bridge only cares that the set changed */
static void on_ap_clients(int count)
{
    (void)count;
    bridge_notify_clients();
}

void app_main(void)
{
    factory_data_t fact;
    bool have_factory = true;
    char ssid[32];

    /* 1. NVS + factory data (BOOT self-check, LLDD 4.1) */
    if (factory_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS init failed");
    }
    if ((factory_load(&fact) != ESP_OK) || !fact.have_sn || !fact.have_pass)
    {
#if CONFIG_C6_FACTORY_DEV_OVERRIDE
        ESP_LOGW(TAG, "factory data missing - DEV OVERRIDE active "
                      "(CONFIG_C6_FACTORY_DEV_OVERRIDE, production must disable)");
        strcpy(fact.sn, "DEV000");
        strcpy(fact.wifi_pass, "sddev123456");
        fact.have_sn   = true;
        fact.have_pass = true;
        have_factory   = false;
#else
        have_factory = false;
#endif
    }

    (void)ota_self_init();
    app_state_init(have_factory);

    if (!have_factory)
    {
        app_state_enter(APP_FACTORY_WAIT);
        /* DPT channel provisioning happens over BLE (or bench override);
         * the box intentionally never becomes a controller in this state */
    }

    /* 2. LINK to TC275 (brings its own tasks + health timer) */
    if (link_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "link init failed - continuing offline");
    }

    /* 3. application services */
    (void)pair_init();
    pair_set_output(bridge_send_frame);
    if (bridge_start() != ESP_OK)
    {
        ESP_LOGE(TAG, "bridge start failed");
    }

    /* 4. network (softAP + captive DNS + mDNS) */
    factory_ap_ssid(&fact, ssid, sizeof(ssid));
    net_cfg_t ncfg = {
        .ssid     = ssid,
        .password = fact.wifi_pass,
        .channel  = (fact.channel != 0u) ? fact.channel : 6u,
        .max_conn = 4u,
    };
    if (net_start(&ncfg) == ESP_OK)
    {
        app_state_enter(APP_ONLINE);
    }
    else
    {
        ESP_LOGE(TAG, "net start failed");
        app_state_enter(APP_NET_START);
    }

    /* 5. web plane */
    if (http_start() == ESP_OK)
    {
        (void)http_register_upload_sink("/ota/c6", &SINK_SELF);
        (void)http_register_upload_sink("/ota/tc275", &SINK_RELAY);
        ws_on_binary(on_ws_binary);
        http_on_session_change(bridge_notify_clients);
        http_set_diag_provider(app_diag_render);
    }

    /* 6. AP station counting also feeds 0x42 LINK_STATE */
    net_on_ap_clients(on_ap_clients);

#ifdef CONFIG_C6_MAINT_BLE
    (void)maint_ble_start();
#endif
#ifdef CONFIG_C6_LEGACY_TCP
    (void)legacy_tcp_start();
#endif

    ESP_LOGI(TAG, "c6_car up: ssid=%s state=%s", ssid, app_state_name());

    /* app_main task idles; all work lives in the component tasks */
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
