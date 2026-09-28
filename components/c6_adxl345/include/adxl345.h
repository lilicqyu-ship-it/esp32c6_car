/*
 * adxl345.h - ADXL345 3-axis accelerometer over 4-wire SPI (bench diag sensor)
 *
 * C6-local only: readings surface in /api/diag via adxl345_diag_json().
 * They never enter the v2/SF link protocol - motion safety stays on the
 * TC275, this is a bring-up/diagnostic instrument.
 */
#ifndef C6_ADXL345_H
#define C6_ADXL345_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    bool     ok;                 /* last sample read succeeded                */
    int32_t  x_mg, y_mg, z_mg;   /* milli-g, full-res 3.9 mg/LSB              */
    int32_t  mag_mg;             /* isqrt(x^2+y^2+z^2): ~1000 at rest, 1g     */
    uint32_t updates;            /* lifetime successful samples               */
    uint32_t errors;             /* lifetime transfer failures                */
} adxl345_data_t;

/* probe (DEVID 0xE5), configure and spawn the poll task; safe to leave
 * un-started - every other call then degrades to a no-op / "null" JSON */
esp_err_t adxl345_start(void);

/* latest sample snapshot (lock-protected, safe from any task) */
void adxl345_get(adxl345_data_t *out);

/* append the imu diag object into buf: {"ok":true,...} or the literal null;
 * returns bytes written (excluding the terminator), -1 if buf/cap invalid */
int adxl345_diag_json(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* C6_ADXL345_H */
