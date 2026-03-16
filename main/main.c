#include <string.h>
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config_store.h"
#include "led_status.h"
#include "ble_common.h"
#include "ble_ftms_client.h"
#include "ble_rsc_server.h"
#include "data_bridge.h"
#include "wifi_manager.h"
#include "web_server.h"

static const char *TAG = "treadlink";

static treadlink_config_t s_config;

// --- Callbacks ---

static void rsc_log_handler(char level, const char *msg)
{
    web_log(level, "%s", msg);
}

static void update_led_state(void)
{
    bool tm = ftms_client_is_connected();
    bool gm = rsc_server_is_connected();
    ftms_state_t ftms_state = ftms_client_get_state();

    if (tm && gm) {
        led_status_set(LED_STATE_BOTH_CONNECTED);
    } else if (tm) {
        led_status_set(LED_STATE_TREADMILL_ONLY);
    } else if (ftms_state == FTMS_STATE_SCANNING) {
        led_status_set(LED_STATE_SCANNING);
    } else if (ftms_state == FTMS_STATE_CONNECTING || ftms_state == FTMS_STATE_RECONNECTING) {
        led_status_set(LED_STATE_SCANNING); // fast blink while connecting
    } else {
        led_status_set(LED_STATE_NO_CONNECTION);
    }
}

static void on_treadmill_data(const ftms_treadmill_data_t *ftms)
{
    rsc_data_t rsc = data_bridge_convert(ftms);
    rsc_server_notify(&rsc);
    web_server_update_data(ftms, &rsc,
                           ftms_client_is_connected(),
                           rsc_server_is_connected());
}

static void restart_rsc_adv_if_needed(void)
{
    // FTMS operations (scan, connect, reconnect) stop BLE advertising.
    // Restart RSC advertising so Garmin can still discover/connect.
    // Don't restart during active scan/connect — it would cancel the GAP procedure.
    ftms_state_t state = ftms_client_get_state();
    if (!rsc_server_is_connected() &&
        state != FTMS_STATE_SCANNING &&
        state != FTMS_STATE_CONNECTING) {
        rsc_server_start_advertising();
    }
}

static void on_treadmill_connection(bool connected)
{
    ESP_LOGI(TAG, "Treadmill %s", connected ? "connected" : "disconnected");
    web_log(connected ? 'I' : 'W', "Treadmill %s", connected ? "connected" : "disconnected");
    update_led_state();
    web_server_set_connection_status(ftms_client_is_connected(), rsc_server_is_connected());
    restart_rsc_adv_if_needed();
}

static void on_garmin_connection(bool connected)
{
    update_led_state();
    web_server_set_connection_status(ftms_client_is_connected(), rsc_server_is_connected());
}

// --- Watchdog task ---
// Periodically feeds the task watchdog and updates LED state.
// If any task hangs, the hardware watchdog triggers a clean reboot.

static void watchdog_task(void *arg)
{
    esp_task_wdt_add(NULL);

    while (1) {
        esp_task_wdt_reset();
        update_led_state();
        restart_rsc_adv_if_needed();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// --- Main ---

void app_main(void)
{
    ESP_LOGI(TAG, "TreadLink starting...");

    // NVS + config
    ESP_ERROR_CHECK(config_store_init());
    config_store_load(&s_config);

    // LED
    ESP_ERROR_CHECK(led_status_init());
    led_status_set(LED_STATE_NO_CONNECTION);

    // WiFi
    wifi_manager_init();
    if (s_config.wifi_mode == 0) {
        wifi_manager_start_ap();
    } else {
        wifi_manager_start_sta(s_config.wifi_ssid, s_config.wifi_pass);
    }
    vTaskDelay(pdMS_TO_TICKS(100)); // brief settle for WiFi AP

    // Web server
    ESP_ERROR_CHECK(web_server_init());
    web_server_set_simulate_callback(on_treadmill_data);
    ESP_ERROR_CHECK(web_server_start());

    // BLE: configure stack, register all services, then start host
    ESP_ERROR_CHECK(ble_common_init());
    data_bridge_init(&s_config);
    ESP_ERROR_CHECK(rsc_server_init(on_garmin_connection, rsc_log_handler));
    ESP_ERROR_CHECK(ftms_client_init(on_treadmill_data, on_treadmill_connection));
    ble_common_start();
    vTaskDelay(pdMS_TO_TICKS(200)); // wait for NimBLE host sync
    rsc_server_start_advertising();

    // Auto-connect to saved treadmill
    if (s_config.auto_connect && s_config.treadmill_addr[0] != '\0') {
        uint8_t addr[6];
        unsigned int a[6];
        if (sscanf(s_config.treadmill_addr, "%02X:%02X:%02X:%02X:%02X:%02X",
                   &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
            for (int i = 0; i < 6; i++) addr[i] = (uint8_t)a[5 - i];
            web_log('I', "Auto-connecting to %s", s_config.treadmill_addr);
            ftms_client_connect(addr, s_config.treadmill_addr_type);
        }
    }

    // Watchdog task — monitors system health
    xTaskCreate(watchdog_task, "watchdog", 2048, NULL, 1, NULL);

    web_log('I', "TreadLink ready");
    ESP_LOGI(TAG, "TreadLink ready!");
}
