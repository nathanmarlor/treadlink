#pragma once

#include "esp_err.h"
#include "data_bridge.h"
#include "ble_ftms_client.h"

typedef void (*web_simulate_cb_t)(const ftms_treadmill_data_t *ftms);

esp_err_t web_server_init(void);
esp_err_t web_server_start(void);
void web_server_set_simulate_callback(web_simulate_cb_t cb);
void web_server_update_data(const ftms_treadmill_data_t *ftms, const rsc_data_t *rsc,
                            bool treadmill_connected, bool garmin_connected);
void web_server_set_connection_status(bool treadmill_connected, bool garmin_connected);
void web_log(char level, const char *fmt, ...);
