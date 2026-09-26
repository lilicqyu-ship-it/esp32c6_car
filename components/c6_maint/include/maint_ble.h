/*
 * maint_ble.h - BLE production-test channel (LLDD 4.9, CONFIG_C6_MAINT_BLE)
 *
 * NimBLE GATT service: CMD (write) + IND (notify response).  DPT commands
 * carrying the fixture token are relayed verbatim to the LINK as 0x70-0x7F;
 * C6-local items (version/self-test/NVS write) are answered locally.
 */
#ifndef C6_MAINT_BLE_H
#define C6_MAINT_BLE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t maint_ble_start(void);

#ifdef __cplusplus
}
#endif

#endif /* C6_MAINT_BLE_H */
