/*
 * ota_self.c - C6 self-update orchestration (LLDD 4.7)
 *
 * httpd task : ota_self_begin/feed/finish - only enqueue chunks
 * ota_task   : bundle parse + ed25519 + esp_ota_write + assets write
 *              (signature verification off the httpd path, LLDD 4.3)
 */
#include "ota_self.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bundle.h"

static const char *TAG = "c6_ota";

/* public key embedded at build time (EMBED_FILES, 32 bytes) */
extern const uint8_t ota_pub_start[] asm("_binary_pub_ed25519_dev_bin_start");
extern const uint8_t ota_pub_end[]   asm("_binary_pub_ed25519_dev_bin_end");

#define OTA_CHUNK_SIZE     512u
#define OTA_QUEUE_LEN      24u
#define OTA_TASK_STACK     6144u
#define OTA_TASK_PRIO      8u
#define OTA_FEED_TMO_MS    2000u
#define OTA_FINISH_TMO_MS  30000u
#define OTA_REBOOT_DELAY_S 5u

typedef struct
{
    uint8_t data[OTA_CHUNK_SIZE];
    uint16_t len;
} ota_chunk_t;

typedef struct
{
    SemaphoreHandle_t mtx;
    QueueHandle_t     q;
    SemaphoreHandle_t done;
    volatile bool     active;
    volatile bool     abort;
    volatile bool     task_exited;            /* ota_task ran to completion   */
    int               sd;
    bundle_ctx_t     *bundle;
    const esp_partition_t *target;
    const esp_partition_t *assets_part;
    esp_ota_handle_t  ota;
    bool              assets_erased;
    size_t            total;
    bundle_state_t    result;
    esp_timer_handle_t reboot_timer;
} ota_self_ctx_t;

static ota_self_ctx_t s_ota;

static void ota_reboot_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "rebooting into new image");
    esp_restart();
}

esp_err_t ota_self_init(void)
{
    const esp_timer_create_args_t targs = {
        .callback = ota_reboot_cb,
        .name     = "ota_reboot",
    };

    if (s_ota.mtx != NULL)
    {
        return ESP_OK;
    }
    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.mtx = xSemaphoreCreateMutex();
    if (s_ota.mtx == NULL)
    {
        return ESP_ERR_NO_MEM;
    }
    return esp_timer_create(&targs, &s_ota.reboot_timer);
}

/* payload router: c6.bin -> inactive app slot, assets.bin -> assets partition */
static int ota_sink(void *arg, uint32_t off, const uint8_t *d, size_t n)
{
    ota_self_ctx_t *o = (ota_self_ctx_t *)arg;
    bundle_info_t info;

    if ((o == NULL) || o->abort || (o->bundle == NULL))
    {
        return -1;
    }
    bundle_get_info(o->bundle, &info);

    if (off < info.c6_len)
    {
        if (o->ota == 0)
        {
            /* lazy begin: signature already verified at header parse, so size
             * the write to the signed c6_len, not the untrusted total field */
            if (esp_ota_begin(o->target, info.c6_len, &o->ota) != ESP_OK)
            {
                o->ota = 0;
                return -1;
            }
        }
        return (esp_ota_write(o->ota, d, n) == ESP_OK) ? 0 : -1;
    }

    if (o->assets_part == NULL)
    {
        return (n == 0u) ? 0 : -1;
    }
    if (!o->assets_erased)
    {
        uint32_t span = (info.assets_len + 4095u) & ~4095u;
        if (esp_partition_erase_range(o->assets_part, 0u, span) != ESP_OK)
        {
            return -1;
        }
        o->assets_erased = true;
    }
    return (esp_partition_write(o->assets_part, off - info.c6_len, d, n) == ESP_OK) ? 0 : -1;
}

static void ota_task(void *arg)
{
    ota_self_ctx_t *o = &s_ota;
    ota_chunk_t *chunk;

    (void)arg;
    chunk = malloc(sizeof(ota_chunk_t));
    if (chunk == NULL)
    {
        o->result = BUNDLE_ERR_SIZE;
        o->task_exited = true;
        (void)xSemaphoreGive(o->done);
        vTaskDelete(NULL);
        return;
    }

    for (;;)
    {
        if (xQueueReceive(o->q, chunk, pdMS_TO_TICKS(200)) != pdTRUE)
        {
            if (o->abort || !o->active)
            {
                break;
            }
            continue;
        }
        bundle_state_t st = bundle_feed(o->bundle, chunk->data, chunk->len, o, ota_sink);
        if (st >= BUNDLE_DONE)
        {
            o->result = st;
            break;
        }
    }
    free(chunk);

    if (o->result == BUNDLE_DONE)
    {
        o->result = bundle_finish(o->bundle);
    }

    if (o->ota != 0)
    {
        if ((o->result != BUNDLE_DONE) || o->abort)
        {
            (void)esp_ota_abort(o->ota);
            o->ota = 0;
            if (o->result == BUNDLE_DONE)
            {
                o->result = BUNDLE_ERR_TRUNCATED;
            }
        }
        else
        {
            if (esp_ota_end(o->ota) != ESP_OK)
            {
                o->result = BUNDLE_ERR_HASH;
            }
            else if (esp_ota_set_boot_partition(o->target) != ESP_OK)
            {
                o->result = BUNDLE_ERR_SIZE;
            }
            else
            {
                ESP_LOGI(TAG, "bundle installed -> %s, reboot in %us",
                         o->target->label, (unsigned)OTA_REBOOT_DELAY_S);
            }
            o->ota = 0;
        }
    }
    /* publish exit BEFORE the handoff flag so a timed-out finish() may
     * reclaim q/done/bundle only after this task can no longer touch them */
    o->task_exited = true;
    (void)xSemaphoreGive(o->done);
    vTaskDelete(NULL);
}

/* ---- sink API (httpd context) ------------------------------------------------*/

esp_err_t ota_self_begin(int sd, size_t total)
{
    ota_self_ctx_t *o = &s_ota;

    if ((o->mtx == NULL) || (xSemaphoreTake(o->mtx, pdMS_TO_TICKS(100)) != pdTRUE))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (o->active)
    {
        /* Previous session still marked active: only recycle once its task
         * provably exited, otherwise a running ota_task would UAF. */
        if (!o->task_exited)
        {
            (void)xSemaphoreGive(o->mtx);
            return ESP_ERR_INVALID_STATE;
        }
        if (o->q != NULL) { vQueueDelete(o->q); o->q = NULL; }
        if (o->done != NULL) { vSemaphoreDelete(o->done); o->done = NULL; }
        if (o->bundle != NULL) { bundle_free(o->bundle); o->bundle = NULL; }
        o->active = false;
    }

    const esp_partition_t *run  = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if ((next == NULL) || (next == run))
    {
        (void)xSemaphoreGive(o->mtx);
        return ESP_ERR_NOT_FOUND;
    }

    o->target      = next;
    o->assets_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                              (esp_partition_subtype_t)0x40, "assets");
    o->total       = total;
    o->sd          = sd;
    o->result      = BUNDLE_IDLE;
    o->abort       = false;
    o->ota         = 0;
    o->assets_erased = false;

    size_t pub_len = (size_t)(ota_pub_end - ota_pub_start);
    o->bundle = (pub_len == 32u) ? bundle_new(ota_pub_start, pub_len) : NULL;
    if (o->bundle == NULL)
    {
        (void)xSemaphoreGive(o->mtx);
        return ESP_ERR_INVALID_STATE;         /* no signing key provisioned */
    }

    o->q    = xQueueCreate(OTA_QUEUE_LEN, sizeof(ota_chunk_t));
    o->done = xSemaphoreCreateBinary();
    if ((o->q == NULL) || (o->done == NULL))
    {
        if (o->q != NULL) { vQueueDelete(o->q); o->q = NULL; }
        if (o->done != NULL) { vSemaphoreDelete(o->done); o->done = NULL; }
        bundle_free(o->bundle);
        o->bundle = NULL;
        (void)xSemaphoreGive(o->mtx);
        return ESP_ERR_NO_MEM;
    }

    o->active = true;
    o->task_exited = false;
    (void)xSemaphoreGive(o->mtx);

    if (xTaskCreate(ota_task, "ota_task", OTA_TASK_STACK, NULL, OTA_TASK_PRIO, NULL) != pdPASS)
    {
        /* task never started, so reclaiming here is race-free */
        (void)xSemaphoreTake(o->mtx, portMAX_DELAY);
        o->active = false;
        vQueueDelete(o->q); o->q = NULL;
        vSemaphoreDelete(o->done); o->done = NULL;
        bundle_free(o->bundle); o->bundle = NULL;
        (void)xSemaphoreGive(o->mtx);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ota_self_feed(int sd, const uint8_t *chunk, size_t n)
{
    ota_self_ctx_t *o = &s_ota;

    if (!o->active || (sd != o->sd))
    {
        return ESP_ERR_INVALID_STATE;
    }
    while (n > 0u)
    {
        ota_chunk_t msg;
        size_t want = (n < OTA_CHUNK_SIZE) ? n : OTA_CHUNK_SIZE;

        memcpy(msg.data, chunk, want);
        msg.len = (uint16_t)want;
        if (xQueueSend(o->q, &msg, pdMS_TO_TICKS(OTA_FEED_TMO_MS)) != pdTRUE)
        {
            return ESP_ERR_TIMEOUT;           /* ota_task backpressured */
        }
        chunk += want;
        n     -= want;
    }
    return ESP_OK;
}

esp_err_t ota_self_finish(int sd, char *json, size_t cap)
{
    ota_self_ctx_t *o = &s_ota;
    bundle_state_t st;
    const char *err = "ok";
    bool ok;

    if (!o->active || (sd != o->sd))
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(o->done, pdMS_TO_TICKS(OTA_FINISH_TMO_MS)) != pdTRUE)
    {
        o->abort = true;
        if (xSemaphoreTake(o->done, pdMS_TO_TICKS(5000)) != pdTRUE)
        {
            (void)xSemaphoreTake(o->mtx, portMAX_DELAY);
            if (!o->task_exited)
            {
                /* ota_task may still touch q/done/bundle: leave the session
                 * alive for the next begin() to reclaim, never free here */
                (void)xSemaphoreGive(o->mtx);
                return ESP_FAIL;
            }
            (void)xSemaphoreGive(o->mtx);
        }
    }
    st  = o->result;
    ok  = (st == BUNDLE_DONE) && !o->abort;
    err = ok ? "ok" : bundle_state_str(st);

    if ((json != NULL) && (cap > 0u))
    {
        (void)snprintf(json, cap,
                       "{\"ok\":%s,\"e\":\"%s\",\"slot\":\"%s\",\"reboot_s\":%u}",
                       ok ? "true" : "false",
                       err,
                       (o->target != NULL) ? o->target->label : "?",
                       (unsigned)OTA_REBOOT_DELAY_S);
    }

    (void)xSemaphoreTake(o->mtx, portMAX_DELAY);
    o->active = false;
    if (o->q != NULL) { vQueueDelete(o->q); o->q = NULL; }
    if (o->done != NULL) { vSemaphoreDelete(o->done); o->done = NULL; }
    if (o->bundle != NULL) { bundle_free(o->bundle); o->bundle = NULL; }
    (void)xSemaphoreGive(o->mtx);

    if (ok)
    {
        /* LLDD 4.7: reply first, then self-restart */
        (void)esp_timer_start_once(s_ota.reboot_timer, OTA_REBOOT_DELAY_S * 1000000ull);
    }
    return ok ? ESP_OK : ESP_FAIL;
}

void ota_self_abort(int sd)
{
    ota_self_ctx_t *o = &s_ota;

    if (o->active && (sd == o->sd))
    {
        o->abort = true;
    }
}

int ota_self_busy(void)
{
    return s_ota.active ? 1 : 0;
}

esp_err_t ota_self_confirm_rollback(int self_check_ok)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    if (esp_ota_get_state_partition(run, &state) != ESP_OK)
    {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY)
    {
        return ESP_OK;
    }
    if (self_check_ok != 0)
    {
        ESP_LOGW(TAG, "new image confirmed (rollback cancelled)");
        return esp_ota_mark_app_valid_cancel_rollback();
    }
    ESP_LOGE(TAG, "self-check failed - rebooting for rollback");
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}
