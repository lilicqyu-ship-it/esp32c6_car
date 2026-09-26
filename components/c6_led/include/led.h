/*
 * led.h - WS2812 status indicator (one GPIO, RMT-driven, no external deps)
 *
 * Patterns are semantic, not free-form colors: the app state machine maps
 * its transitions onto them so the LED doubles as a bring-up aid.
 */
#ifndef C6_LED_H
#define C6_LED_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    LED_PAT_OFF = 0,      /* dark                                         */
    LED_PAT_BOOT,         /* white slow blink: boot, before first state   */
    LED_PAT_FACTORY_WAIT, /* yellow double-blink: waiting provisioning    */
    LED_PAT_NET_START,    /* blue blink 2 Hz: Wi-Fi coming up             */
    LED_PAT_ONLINE,       /* green heartbeat: normal operation            */
    LED_PAT_FAULT,        /* red blink 4 Hz: net start failed             */
} led_pattern_t;

/* call once from app_main; no-op when CONFIG_C6_LED_ENABLE=n */
esp_err_t led_init(void);

/* thread-safe; may be called from any task/timer context */
void led_pattern(led_pattern_t pat);

#ifdef __cplusplus
}
#endif

#endif /* C6_LED_H */
