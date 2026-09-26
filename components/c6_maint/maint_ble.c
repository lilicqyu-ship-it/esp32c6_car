/*
 * maint_ble.c - NimBLE GATT DPT service (compiled only with CONFIG_C6_MAINT_BLE)
 */
#include "maint_ble.h"

#include <string.h>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "factory.h"
#include "link.h"

static const char *TAG = "c6_maint";

/* production-test service / characteristics (vendor range, DPT only) */
static const ble_uuid128_t SVC_DPT_UUID =
    BLE_UUID128_INIT(0x11, 0x07, 0xd1, 0xfd, 0x43, 0x1e, 0x8e, 0xb4,
                     0x5b, 0x48, 0x8f, 0xa1, 0x23, 0xd4, 0x6c, 0xfd);
static const ble_uuid128_t CHR_CMD_UUID =
    BLE_UUID128_INIT(0x12, 0x07, 0xd1, 0xfd, 0x43, 0x1e, 0x8e, 0xb4,
                     0x5b, 0x48, 0x8f, 0xa1, 0x23, 0xd4, 0x6c, 0xfd);
static const ble_uuid128_t CHR_IND_UUID =
    BLE_UUID128_INIT(0x13, 0x07, 0xd1, 0xfd, 0x43, 0x1e, 0x8e, 0xb4,
                     0x5b, 0x48, 0x8f, 0xa1, 0x23, 0xd4, 0x6c, 0xfd);

static uint16_t s_ind_attr;

static int chr_ind_access(uint16_t conn, uint16_t attr,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);
static int chr_cmd_access(uint16_t conn, uint16_t attr,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_DPT_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = &CHR_CMD_UUID.u,
                .access_cb  = chr_cmd_access,
                .flags      = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid       = &CHR_IND_UUID.u,
                .access_cb  = chr_ind_access,
                .val_handle = &s_ind_attr,
                .flags      = BLE_GATT_CHR_F_INDICATE,
            },
            { 0 },
        },
    },
    { 0 },
};

/* fixture token check: first frame after connect must be DPT_ENTER + token */
static bool s_dpt_open;
static uint8_t s_fixture_token[16];

static int chr_cmd_access(uint16_t conn, uint16_t attr,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const struct os_mbuf *om = ctxt->om;
    uint16_t len = OS_MBUF_PKTLEN(om);
    uint8_t buf[256];

    if (len > sizeof(buf))
    {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    (void)os_mbuf_copydata(om, 0, len, buf);

    if (!s_dpt_open)
    {
        /* expect DPT_ENTER (0x70) frame carrying the fixture token (payload) */
        if ((len >= (uint16_t)(PROTO_HEADER_LEN + 16u)) &&
            (buf[0] == 0xAAu) && (buf[1] == 0x55u) &&
            (buf[2] == PROTO_VER) &&
            (buf[3] == PROTO_CMD_DPT_ENTER) && (buf[5] == 16u))
        {
            memcpy(s_fixture_token, &buf[6], 16u);
            s_dpt_open = true;
            ESP_LOGI(TAG, "DPT session opened");
            return 0;
        }
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    /* relay everything else to LINK verbatim (motion 0x7x, aging, ...) */
    {
        proto_frame_t f;
        if (len < PROTO_HEADER_LEN)
        {
            return BLE_ATT_ERR_UNLIKELY;
        }
        f.ver = buf[2];
        f.cmd = buf[3];
        f.seq = buf[4];
        f.len = buf[5];
        if ((f.len <= PROTO_MAX_PAYLOAD) && (len >= (uint16_t)(6u + f.len)))
        {
            memcpy(f.data, &buf[6], f.len);
            (void)link_send(&f);
        }
    }
    return 0;
}

static int chr_ind_access(uint16_t conn, uint16_t attr,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    ctxt->om = ble_hs_mbuf_from_flat("ok", 2);
    return 0;
}

static void maint_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0)
    {
        struct ble_gap_adv_params ap = { 0 };
        uint8_t own[6];
        ble_hs_id_infer_auto(0, &own[0]);
        (void)own;
        ap.itvl_min = BLE_GAP_ADV_ITVL_MS(120);
        ap.itvl_max = BLE_GAP_ADV_ITVL_MS(180);
        (void)ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                                &ap, NULL, NULL);
        ESP_LOGI(TAG, "DPT BLE advertising");
    }
}

static void maint_on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
}

static void maint_task(void *param)
{
    (void)param;
    nimble_port_run();                        /* returns on termination */
    nimble_port_freertos_deinit();
}

esp_err_t maint_ble_start(void)
{
    esp_err_t err;

    err = nimble_port_init();
    if (err != ESP_OK)
    {
        return err;
    }
    (void)ble_gatts_count_cfg(svcs);
    (void)ble_gatts_add_svcs(svcs);
    (void)ble_svc_gap_device_name_set("SD-DPT");

    ble_hs_cfg.sync_cb = maint_on_sync;
    ble_hs_cfg.reset_cb = maint_on_reset;

    nimble_port_freertos_init(maint_task);
    return ESP_OK;
}
