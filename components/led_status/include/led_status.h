#pragma once

#include "esp_err.h"

typedef enum {
    LED_STATE_NO_CONNECTION = 0,   // Slow blink (1s on / 1s off)
    LED_STATE_SCANNING,            // Fast blink (100ms on / 100ms off)
    LED_STATE_TREADMILL_ONLY,      // Double blink (200/200/200/1000)
    LED_STATE_BOTH_CONNECTED,      // Solid on
    LED_STATE_WIFI_AP,             // Triple quick blink
} led_state_t;

esp_err_t led_status_init(void);
void led_status_set(led_state_t state);
led_state_t led_status_get(void);
