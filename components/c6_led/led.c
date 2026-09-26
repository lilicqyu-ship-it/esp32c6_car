/*
 * led.c - WS2812 status indicator (one GPIO, RMT-driven, no external deps)
 *
 * A 50 ms tick task renders the current pattern; a frame is pushed to the
 * strip only when the 24-bit color actually changes, so the RMT bus is idle
 * while the LED holds its state.  Colors are pre-scaled to ~19% to keep the
 * indicator comfortable at night and the current peak small.
 */
#include "led.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "c6_led";

#define LED_TICK_MS        50u
#define LED_BRIGHT_MAX     48u   /* 0..255 per channel (~19%)               */
#define LED_FRAME_BITS     24u   /* GRB, MSB first                          */

/* RMT resolution 10 MHz -> 100 ns per tick; WS2812 spec windows:
 * T0H 220-380 ns, T0L 580-1000 ns, T1H 580-1000 ns, T1L 220-420 ns */
#define LED_RES_HZ         (10u * 1000u * 1000u)
#define T0H_TICKS          3u    /* 300 ns                                  */
#define T0L_TICKS          8u    /* 800 ns                                  */
#define T1H_TICKS          8u    /* 800 ns                                  */
#define T1L_TICKS          3u    /* 300 ns                                  */
#define RESET_TICKS        300u  /* 30 us low (spec wants > 50 us total idle;
                                  * the rest comes from inter-frame delay)  */

typedef struct
{
    uint8_t r, g, b;
} led_color_t;

typedef struct
{
    led_pattern_t pat;
    led_color_t   on;      /* "lit" color; off is always black           */
    uint16_t      period_ms;
    uint16_t      on0_ms;  /* lit 0: [0, on0)                            */
    uint16_t      on1_ms;  /* lit 1: [on0+gap, on0+gap+on1), 0 = single  */
    uint16_t      gap_ms;
} led_step_t;

/* timings must fit led_step_t fields; period includes the dark tail */
static const led_step_t STEPS[] = {
    [LED_PAT_OFF]          = { .pat = LED_PAT_OFF, .on = {0, 0, 0} },
    [LED_PAT_BOOT]         = { .pat = LED_PAT_BOOT, .on = {44, 44, 44},
                               .period_ms = 1000, .on0_ms = 500 },
    [LED_PAT_FACTORY_WAIT] = { .pat = LED_PAT_FACTORY_WAIT, .on = {44, 44, 0},
                               .period_ms = 1000, .on0_ms = 100,
                               .gap_ms = 100, .on1_ms = 100 },
    [LED_PAT_NET_START]    = { .pat = LED_PAT_NET_START, .on = {0, 0, 44},
                               .period_ms = 500, .on0_ms = 250 },
    [LED_PAT_ONLINE]       = { .pat = LED_PAT_ONLINE, .on = {0, 44, 0},
                               .period_ms = 2000, .on0_ms = 100,
                               .gap_ms = 150, .on1_ms = 100 },
    [LED_PAT_FAULT]        = { .pat = LED_PAT_FAULT, .on = {44, 0, 0},
                               .period_ms = 250, .on0_ms = 125 },
};

#if CONFIG_C6_LED_ENABLE

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_copy;
/* v6.1 rmt_transmit rejects NULL config; zero = loop once, idle low (eot 0) */
static const rmt_transmit_config_t TX_CFG = { 0 };
static volatile led_pattern_t s_pat = LED_PAT_BOOT;
static led_color_t s_last = { 0xFF, 0xFF, 0xFF };   /* force first frame */

static esp_err_t led_show(const led_color_t *c)
{
    if (c->r == s_last.r && c->g == s_last.g && c->b == s_last.b)
    {
        return ESP_OK;
    }

    /* wire order is GRB */
    const uint8_t bytes[3] = { c->g, c->r, c->b };
    rmt_symbol_word_t syms[LED_FRAME_BITS + 1u];
    for (size_t i = 0u; i < LED_FRAME_BITS; i++)
    {
        const bool one = (bytes[i / 8u] >> (7u - (i % 8u))) & 1u;
        syms[i].level0    = 1;
        syms[i].duration0 = one ? T1H_TICKS : T0H_TICKS;
        syms[i].level1    = 0;
        syms[i].duration1 = one ? T1L_TICKS : T0L_TICKS;
    }
    syms[LED_FRAME_BITS].level0    = 0;
    syms[LED_FRAME_BITS].duration0 = RESET_TICKS;
    syms[LED_FRAME_BITS].level1    = 0;
    syms[LED_FRAME_BITS].duration1 = RESET_TICKS;

    esp_err_t err = rmt_transmit(s_chan, s_copy, syms, sizeof(syms), &TX_CFG);
    if (err == ESP_OK)
    {
        err = rmt_tx_wait_all_done(s_chan, 20 /* ms */);
        s_last = *c;
    }
    return err;
}

static void led_render(uint32_t t_ms)
{
    const led_step_t *st = &STEPS[s_pat];

    if (st->period_ms == 0u)
    {
        (void)led_show(&st->on);    /* OFF or solid color            */
        return;
    }
    const uint32_t ph = t_ms % st->period_ms;
    const uint32_t end0 = st->on0_ms;
    const uint32_t end1 = end0 + st->gap_ms + st->on1_ms;
    bool lit;
    if (st->on1_ms != 0u)
    {
        lit = (ph < end0) || (ph >= end0 + st->gap_ms && ph < end1);
    }
    else
    {
        lit = ph < end0;
    }
    (void)led_show(lit ? &st->on : &STEPS[LED_PAT_OFF].on);
}

static void led_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t t_ms = 0u;
    for (;;)
    {
        led_render(t_ms);
        t_ms += LED_TICK_MS;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(LED_TICK_MS));
    }
}

esp_err_t led_init(void)
{
    const rmt_tx_channel_config_t tx = {
        .gpio_num        = CONFIG_C6_LED_GPIO,
        .clk_src         = RMT_CLK_SRC_DEFAULT,
        .resolution_hz   = LED_RES_HZ,
        .mem_block_symbols = 64u,
        .trans_queue_depth = 1u,
    };
    esp_err_t err = rmt_new_tx_channel(&tx, &s_chan);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "rmt tx channel: %s", esp_err_to_name(err));
        return err;
    }
    const rmt_copy_encoder_config_t ce = {};
    err = rmt_new_copy_encoder(&ce, &s_copy);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "copy encoder: %s", esp_err_to_name(err));
        return err;
    }
    err = rmt_enable(s_chan);
    if (err != ESP_OK)
    {
        return err;
    }
    if (xTaskCreate(led_task, "c6_led", 2048, NULL, 3, NULL) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ws2812 on GPIO%d", CONFIG_C6_LED_GPIO);
    return ESP_OK;
}

void led_pattern(led_pattern_t pat)
{
    if ((unsigned)pat <
        sizeof(STEPS) / sizeof(STEPS[0]))
    {
        s_pat = pat;    /* aligned 32-bit store, atomic on RISC-V */
    }
}

#else /* !CONFIG_C6_LED_ENABLE: keep call sites buildable, do nothing */

esp_err_t led_init(void)
{
    return ESP_OK;
}

void led_pattern(led_pattern_t pat)
{
    (void)pat;
}

#endif /* CONFIG_C6_LED_ENABLE */
