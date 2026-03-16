#include "led_status.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// XIAO ESP32-S3 has a single-color orange LED on GPIO 21 (active low)
#define LED_GPIO        21
#define TASK_STACK_SIZE 2048

// Short delay slice for responsive state transitions
#define TICK_MS 50

static const char *TAG = "led_status";

static volatile led_state_t s_state = LED_STATE_NO_CONNECTION;
static TaskHandle_t s_task_handle;

static void led_on(void)  { gpio_set_level(LED_GPIO, 0); }
static void led_off(void) { gpio_set_level(LED_GPIO, 1); }

// Sleep in small slices so state changes take effect quickly
static bool delay_check(int ms, led_state_t expected)
{
    while (ms > 0) {
        int slice = ms > TICK_MS ? TICK_MS : ms;
        vTaskDelay(pdMS_TO_TICKS(slice));
        ms -= slice;
        if (s_state != expected) return false; // state changed, abort pattern
    }
    return true;
}

static void led_task(void *arg)
{
    while (1) {
        led_state_t state = s_state;

        switch (state) {
        case LED_STATE_NO_CONNECTION:
            // Slow blink: 1s on / 1s off
            led_on();
            if (!delay_check(1000, state)) break;
            led_off();
            delay_check(1000, state);
            break;

        case LED_STATE_SCANNING:
            // Fast blink: 100ms on / 100ms off
            led_on();
            if (!delay_check(100, state)) break;
            led_off();
            delay_check(100, state);
            break;

        case LED_STATE_TREADMILL_ONLY:
            // Double blink: on-off-on then long off
            led_on();
            if (!delay_check(200, state)) break;
            led_off();
            if (!delay_check(200, state)) break;
            led_on();
            if (!delay_check(200, state)) break;
            led_off();
            delay_check(1000, state);
            break;

        case LED_STATE_BOTH_CONNECTED:
            // Solid on
            led_on();
            delay_check(500, state);
            break;

        case LED_STATE_WIFI_AP:
            // Triple quick blink
            for (int i = 0; i < 3; i++) {
                led_on();
                if (!delay_check(100, state)) break;
                led_off();
                if (!delay_check(100, state)) break;
            }
            delay_check(800, state);
            break;
        }
    }
}

esp_err_t led_status_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    led_off();

    xTaskCreate(led_task, "led_task", TASK_STACK_SIZE, NULL, 1, &s_task_handle);
    ESP_LOGI(TAG, "LED status initialized on GPIO %d", LED_GPIO);
    return ESP_OK;
}

void led_status_set(led_state_t state)
{
    if (s_state != state) {
        s_state = state;
        ESP_LOGD(TAG, "LED state -> %d", state);
    }
}

led_state_t led_status_get(void)
{
    return s_state;
}
