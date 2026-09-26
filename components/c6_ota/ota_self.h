/*
 * ota_self.h - C6 self-update: bundle stream -> A/B slot + assets (LLDD 4.7)
 *
 * The /ota/c6 upload sink is implemented here and registered by app_main
 * (composition root keeps c6_ota free of c6_http includes).
 */
#ifndef C6_OTA_SELF_H
#define C6_OTA_SELF_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* call once from app_main before httpd starts (creates mutex + reboot timer) */
esp_err_t ota_self_init(void);

/* ---- upload sink (called from httpd task; heavy work runs in ota_task) ---- */
esp_err_t ota_self_begin(int sd, size_t total);
esp_err_t ota_self_feed(int sd, const uint8_t *chunk, size_t n);
esp_err_t ota_self_finish(int sd, char *json, size_t cap);
void      ota_self_abort(int sd);

/* BOOT-time rollback handling: when the running image is in
 * ESP_OTA_IMG_PENDING_VERIFY, run the minimal self-check set and mark valid
 * (LLDD 4.1/4.10); leaves the decision to the caller's check result. */
esp_err_t ota_self_confirm_rollback(int self_check_ok);

/* 1 when an update session is active (page shows busy) */
int ota_self_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* C6_OTA_SELF_H */
