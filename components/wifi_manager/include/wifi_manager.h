#pragma once

#include "esp_err.h"
#include "config_store.h"
#include <stdbool.h>

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start_ap(void);
esp_err_t wifi_manager_start_sta(const char *ssid, const char *pass);
bool wifi_manager_is_connected(void);
esp_err_t wifi_manager_get_ip(char *buf, size_t len);
