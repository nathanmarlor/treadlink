#pragma once

#include "esp_err.h"
#include "data_bridge.h"
#include <stdint.h>
#include <stdbool.h>

#define FTMS_MAX_SCAN_RESULTS 10

typedef enum {
    FTMS_STATE_IDLE = 0,
    FTMS_STATE_SCANNING,
    FTMS_STATE_CONNECTING,
    FTMS_STATE_DISCOVERING,
    FTMS_STATE_SUBSCRIBING,
    FTMS_STATE_STREAMING,
    FTMS_STATE_RECONNECTING,
} ftms_state_t;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    char name[32];
    int8_t rssi;
} ftms_scan_result_t;

typedef void (*ftms_data_cb_t)(const ftms_treadmill_data_t *data);
typedef void (*ftms_conn_cb_t)(bool connected);
typedef void (*ftms_scan_cb_t)(const ftms_scan_result_t *result);

esp_err_t ftms_client_init(ftms_data_cb_t data_cb, ftms_conn_cb_t conn_cb);
void ftms_client_set_log_cb(void (*fn)(char, const char *, ...));
void ftms_client_set_scan_cb(ftms_scan_cb_t cb);
esp_err_t ftms_client_scan_start(void);
esp_err_t ftms_client_scan_stop(void);
esp_err_t ftms_client_connect(const uint8_t addr[6], uint8_t addr_type);
esp_err_t ftms_client_disconnect(void);
bool ftms_client_is_connected(void);
ftms_state_t ftms_client_get_state(void);

const ftms_scan_result_t *ftms_client_get_scan_results(uint8_t *count);
void ftms_client_clear_scan_results(void);
uint16_t ftms_client_get_reconnect_attempts(void);

// FTMS Control Point — treadmill control
bool ftms_client_has_control(void);
esp_err_t ftms_client_set_target_speed(uint16_t speed_001kmh);
esp_err_t ftms_client_set_target_incline(int16_t incline_01pct);
esp_err_t ftms_client_start_treadmill(void);
esp_err_t ftms_client_stop_treadmill(void);
