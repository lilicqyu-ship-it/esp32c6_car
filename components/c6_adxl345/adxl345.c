/*
 * adxl345.c - ADXL345 3-axis accelerometer, 4-wire SPI (bit-banged)
 *
 * The C6 has exactly one general-purpose SPI controller and it is owned by
 * the TC275 LINK slave (spi_slave_hd), so the sensor bus is clocked by
 * software: SPI mode 3 (CPOL=1/CPHA=1, datasheet Fig. 43) at ~500 kHz.
 * 6 data bytes at a 50 Hz poll cost ~0.1 ms CPU per second of runtime.
 *
 * Chip wiring (GY-291-style module): CS/SCL/SDA/SDO driven as CS/SCLK/
 * MOSI/MISO; INT1/INT2 unused.  A software bus has no protocol-level
 * failure signal, so liveness is checked by re-reading DEVID every few
 * seconds (covered states: absent at boot -> start() fails with no task;
 * disappearing later -> ok=false in the diag object).
 *
 * Configuration: standby -> BW_RATE -> DATA_FORMAT (full-res, Kconfig
 * range) -> POWER_CTL.Measure.  Data is read as one 6-byte frame from
 * DATAX0 with the MB bit set (CS must stay low for the whole frame).
 * Full-resolution scale is 3.9 mg/LSB, handled in integer math so the
 * poll task never touches the soft-float library.
 */
#include "adxl345.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ---- ADXL345 registers (SPI addressing: bit7 R/Wn, bit6 MB) ----------------- */
#define REG_DEVID        0x00u
#define REG_BW_RATE      0x2Cu
#define REG_POWER_CTL    0x2Du
#define REG_DATA_FORMAT  0x31u
#define REG_DATAX0       0x32u

#define DEVID_ADXL345    0xE5u
#define PCTL_MEASURE     0x08u
#define FMT_FULL_RES     0x08u

/* DEVID liveness check cadence, in poll periods (20 ms * 250 = 5 s) */
#define LIVENESS_EVERY   250u

#if CONFIG_C6_ADXL345_ENABLE

static const char *TAG = "c6_adxl";

/* last DEVID seen on the bus; exposed through diag so a phone can tell
 * "no chip" (0xff = MISO floating) from "miswired" (garbage) at a glance */
static uint8_t s_probe_id = 0xFFu;

static const struct
{
    uint16_t hz;
    uint8_t  code;
} RATES[] = {
    { 13,   0x08u },   /* 12.5 Hz */
    { 25,   0x09u },
    { 50,   0x0Au },
    { 100,  0x0Bu },
    { 200,  0x0Cu },
    { 400,  0x0Du },
    { 800,  0x0Eu },
    { 1600, 0x0Fu },
};

static void adxl_task(void *arg);

/* snapshot shared between the poll task and every diag reader */
static portMUX_TYPE s_spin = portMUX_INITIALIZER_UNLOCKED;
static adxl345_data_t s_data;
static bool s_running;

/* ---- bit-bang SPI (mode 3) ---------------------------------------------------
 * C6 gpio_dev_t registers are typed unions, so the raw-register shortcut of
 * older targets does not apply - use the driver API like c6_link does. */

#define PIN_SCLK   ((gpio_num_t)CONFIG_C6_ADXL345_SCLK_GPIO)
#define PIN_MOSI   ((gpio_num_t)CONFIG_C6_ADXL345_MOSI_GPIO)
#define PIN_MISO   ((gpio_num_t)CONFIG_C6_ADXL345_MISO_GPIO)
#define PIN_CS     ((gpio_num_t)CONFIG_C6_ADXL345_CS_GPIO)
#define HALF_BIT   CONFIG_C6_ADXL345_HALF_BIT_US

static inline void sclk_hi(void) { (void)gpio_set_level(PIN_SCLK, 1); }
static inline void sclk_lo(void) { (void)gpio_set_level(PIN_SCLK, 0); }
static inline void mosi_set(bool v)
{
    (void)gpio_set_level(PIN_MOSI, v ? 1 : 0);
}
static inline void cs_set(bool high)
{
    (void)gpio_set_level(PIN_CS, high ? 1 : 0);
}
static inline bool miso_get(void)
{
    return gpio_get_level(PIN_MISO) != 0;
}

static uint8_t xfer_byte(uint8_t tx)
{
    uint8_t rx = 0u;

    /* SPI mode 3, CPOL=1/CPHA=1: SCLK rests HIGH, the slave shifts MISO out
     * on the FALLING edge and samples MOSI on the RISING edge - so every bit
     * is lo -> (launch MOSI) -> hi (sample).  The earlier version launched
     * MOSI first and called sclk_hi() on a pin already resting high: no edge,
     * the whole byte sampled one position early.  DEVID read as 0xF2 = 0xE5
     * shifted by exactly one bit - the sensor answered all along. */
    for (int i = 7; i >= 0; i--)
    {
        sclk_lo();                  /* falling edge: slave launches MISO(i)  */
        mosi_set((tx >> i) & 1u);   /* launch MOSI ahead of the rising edge  */
        esp_rom_delay_us(HALF_BIT);
        sclk_hi();                  /* rising edge: slave samples, we sample */
        rx = (uint8_t)((rx << 1) | (miso_get() ? 1u : 0u));
        esp_rom_delay_us(HALF_BIT); /* clock rests HIGH between bits (CPOL=1)*/
    }
    return rx;
}

static void frame_begin(void)
{
    cs_set(false);
    esp_rom_delay_us(2);            /* CS setup before the first edge        */
}

static void frame_end(void)
{
    cs_set(true);
    esp_rom_delay_us(5);            /* CS high time between transfers        */
}

static void reg_write(uint8_t reg, uint8_t val)
{
    frame_begin();
    (void)xfer_byte(reg & 0x3Fu);   /* bit7=0 write; MB only for data reads  */
    (void)xfer_byte(val);
    frame_end();
}

static void reg_read(uint8_t reg, uint8_t *buf, uint8_t len)
{
    frame_begin();
    (void)xfer_byte(0x80u | ((len > 1u) ? 0x40u : 0x00u) | (reg & 0x3Fu));
    for (uint8_t i = 0u; i < len; i++)
    {
        buf[i] = xfer_byte(0x00u);
    }
    frame_end();
}

/* ---- pure helpers ------------------------------------------------------------ */

static inline int32_t raw_to_mg(const uint8_t *p)
{
    /* full-resolution: 3.9 mg/LSB; 39/10 keeps the poll task float-free */
    const int16_t counts = (int16_t)(p[0] | ((uint16_t)p[1] << 8));
    return ((int32_t)counts * 39) / 10;
}

static uint32_t isqrt64(uint64_t v)
{
    uint64_t res = 0;
    uint64_t bit = 1ULL << 62;

    while (bit > v)
    {
        bit >>= 2;
    }
    while (bit != 0u)
    {
        const uint64_t sum = res + bit;
        if (v >= sum)
        {
            v -= sum;
            res = (res >> 1) + bit;
        }
        else
        {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)res;
}

static uint16_t nearest_rate_code(int cfg_hz, int *programmed_hz)
{
    uint8_t code = RATES[3].code;   /* 100 Hz fallback                        */
    int best = RATES[3].hz;

    for (size_t i = 0u; i < sizeof(RATES) / sizeof(RATES[0]); i++)
    {
        const int d = (RATES[i].hz > cfg_hz) ? (RATES[i].hz - cfg_hz)
                                             : (cfg_hz - RATES[i].hz);
        const int db = (best > cfg_hz) ? (best - cfg_hz) : (cfg_hz - best);
        if (d < db)
        {
            best = RATES[i].hz;
            code = RATES[i].code;
        }
    }
    *programmed_hz = best;
    return code;
}

/* one-shot deep diagnostic on the first failed probe: MISO idle level, four
 * consecutive DEVID reads (floating bus reads 0xff, a stable wrong value is
 * a clocking/wiring signature, flapping values mean a marginal connection) */
static void probe_dump(void)
{
    uint8_t v[4] = { 0u, 0u, 0u, 0u };

    for (int i = 0; i < 4; i++)
    {
        reg_read(REG_DEVID, &v[i], 1u);
    }
    ESP_LOGI(TAG, "probe: miso_idle=%d devid x4 = %02x %02x %02x %02x (want e5)",
             gpio_get_level(PIN_MISO), v[0], v[1], v[2], v[3]);
}

/* ---- bring-up ----------------------------------------------------------------- */

static void adxl_configure(void)
{
    /* standby first: rate/format changes are only safe while Measure=0 */
    reg_write(REG_POWER_CTL, 0x00u);

    int programmed = 0;
    reg_write(REG_BW_RATE, nearest_rate_code(CONFIG_C6_ADXL345_ODR_HZ,
                                             &programmed));

    int r = CONFIG_C6_ADXL345_RANGE_G;          /* snap to 2/4/8/16           */
    r = (r <= 2) ? 2 : (r <= 4) ? 4 : (r <= 8) ? 8 : 16;
    reg_write(REG_DATA_FORMAT, (uint8_t)(FMT_FULL_RES | ((r / 2) - 1)));

    reg_write(REG_POWER_CTL, PCTL_MEASURE);

    ESP_LOGI(TAG, "odr=%d Hz range=%d g sclk~%lu kHz",
             programmed, r, (unsigned long)(500u / HALF_BIT));
}

/* hot-plug: bench sensors get (un)plugged freely - a failed boot probe arms a
 * 5 s retry instead of requiring a reboot to appear in /diag */
static esp_timer_handle_t s_retry_timer;

static void retry_cb(void *arg)
{
    (void)arg;
    if (adxl345_start() == ESP_OK)
    {
        /* adxl_configure() logged the live config; drop the one-shot */
        esp_timer_handle_t t = s_retry_timer;
        s_retry_timer = NULL;
        if (t != NULL)
        {
            (void)esp_timer_delete(t);
        }
    }
}

static void arm_retry(void)
{
    if (s_retry_timer != NULL)
    {
        (void)esp_timer_start_once(s_retry_timer, 5u * 1000000u);
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = retry_cb,
        .name     = "adxl_retry",
    };
    if ((esp_timer_create(&args, &s_retry_timer) == ESP_OK) &&
        (esp_timer_start_once(s_retry_timer, 5u * 1000000u) != ESP_OK))
    {
        (void)esp_timer_delete(s_retry_timer);
        s_retry_timer = NULL;
    }
}

esp_err_t adxl345_start(void)
{
    if (s_running)
    {
        return ESP_OK;
    }

    const gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_C6_ADXL345_SCLK_GPIO) |
                        (1ULL << CONFIG_C6_ADXL345_MOSI_GPIO) |
                        (1ULL << CONFIG_C6_ADXL345_CS_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&out_cfg) != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }
    const gpio_config_t in_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_C6_ADXL345_MISO_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* idle MISO between frames    */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&in_cfg) != ESP_OK)
    {
        return ESP_ERR_INVALID_STATE;
    }

    cs_set(true);                   /* SPI mode 3 idle + module deselect     */
    sclk_hi();
    mosi_set(false);
    esp_rom_delay_us(10);

    uint8_t id = 0u;
    reg_read(REG_DEVID, &id, 1u);
    s_probe_id = id;
    if (id != DEVID_ADXL345)
    {
        static bool warned;
        if (!warned)
        {
            warned = true;
            ESP_LOGW(TAG, "DEVID 0x%02x != 0xE5 - sensor absent or wiring wrong",
                     id);
            probe_dump();
        }
        arm_retry();
        return ESP_ERR_NOT_FOUND;
    }

    adxl_configure();

    if (xTaskCreate(adxl_task, "c6_adxl", 3072, NULL, 3, NULL) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    s_running = true;
    return ESP_OK;
}

/* ---- poll task ---------------------------------------------------------------- */

/* display EMA (1/16): raw 50 Hz samples flicker the last digit on the diag
 * page; integer math keeps the poll task float-free */
static adxl345_data_t s_ema;
static bool s_ema_seeded;

static int32_t ema16(int32_t avg, int32_t raw)
{
    return avg + (raw - avg) / 16;
}

static void adxl_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint8_t raw[6];
    uint32_t cycles = 0u;

    for (;;)
    {
        bool ok;
        int32_t x = 0, y = 0, z = 0;
        uint32_t mag = 0u;

        if ((cycles % LIVENESS_EVERY) == 0u)
        {
            uint8_t id = 0u;
            reg_read(REG_DEVID, &id, 1u);
            if (id != DEVID_ADXL345)
            {
                portENTER_CRITICAL(&s_spin);
                s_data.ok = false;
                s_data.errors++;
                portEXIT_CRITICAL(&s_spin);
                ESP_LOGW(TAG, "DEVID lost (0x%02x)", id);
                goto next;
            }
        }

        reg_read(REG_DATAX0, raw, sizeof(raw));
        x = raw_to_mg(&raw[0]);
        y = raw_to_mg(&raw[2]);
        z = raw_to_mg(&raw[4]);
        if (!s_ema_seeded)
        {
            s_ema.x_mg = x; s_ema.y_mg = y; s_ema.z_mg = z;
            s_ema_seeded = true;
        }
        else
        {
            s_ema.x_mg = ema16(s_ema.x_mg, x);
            s_ema.y_mg = ema16(s_ema.y_mg, y);
            s_ema.z_mg = ema16(s_ema.z_mg, z);
        }
        x = s_ema.x_mg; y = s_ema.y_mg; z = s_ema.z_mg;
        mag = isqrt64((uint64_t)((int64_t)x * x) +
                      (uint64_t)((int64_t)y * y) +
                      (uint64_t)((int64_t)z * z));
        ok = true;

        portENTER_CRITICAL(&s_spin);
        s_data.x_mg = x;
        s_data.y_mg = y;
        s_data.z_mg = z;
        s_data.mag_mg = (int32_t)mag;
        s_data.updates++;
        s_data.ok = ok;
        portEXIT_CRITICAL(&s_spin);

    next:
        cycles++;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(CONFIG_C6_ADXL345_POLL_MS));
    }
}

/* ---- snapshot API -------------------------------------------------------------- */

void adxl345_get(adxl345_data_t *out)
{
    if (out == NULL)
    {
        return;
    }
    portENTER_CRITICAL(&s_spin);
    *out = s_data;
    portEXIT_CRITICAL(&s_spin);
}

int adxl345_diag_json(char *buf, size_t cap)
{
    if ((buf == NULL) || (cap == 0u))
    {
        return -1;
    }
    if (!s_running)
    {
        /* expose the raw probe value: 0xff = MISO floating (no chip / SDO
         * open), a stable wrong byte = clocking/wiring signature */
        (void)snprintf(buf, cap, "{\"probe\":\"0x%02x\"}", s_probe_id);
        return (int)strlen(buf);
    }

    adxl345_data_t d;
    adxl345_get(&d);
    (void)snprintf(buf, cap,
                   "{\"ok\":%s,\"mg\":[%ld,%ld,%ld],\"mag\":%ld,"
                   "\"upd\":%lu,\"err\":%lu}",
                   d.ok ? "true" : "false",
                   (long)d.x_mg, (long)d.y_mg, (long)d.z_mg,
                   (long)d.mag_mg,
                   (unsigned long)d.updates,
                   (unsigned long)d.errors);
    return (int)strlen(buf);
}

#else /* !CONFIG_C6_ADXL345_ENABLE: keep call sites buildable, do nothing */

esp_err_t adxl345_start(void)
{
    return ESP_OK;
}

void adxl345_get(adxl345_data_t *out)
{
    if (out != NULL)
    {
        memset(out, 0, sizeof(*out));
    }
}

int adxl345_diag_json(char *buf, size_t cap)
{
    if ((buf == NULL) || (cap == 0u))
    {
        return -1;
    }
    (void)snprintf(buf, cap, "null");
    return (int)strlen(buf);
}

#endif /* CONFIG_C6_ADXL345_ENABLE */
