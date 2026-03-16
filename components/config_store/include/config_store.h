#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char treadmill_addr[18];    // "AA:BB:CC:DD:EE:FF"
    uint8_t treadmill_addr_type;
    char treadmill_name[32];
    bool auto_connect;
    float cadence_factor;       // speed-to-cadence multiplier
    float cadence_offset;       // cadence base offset
    bool speed_is_mph;          // treadmill reports mph instead of km/h
    uint8_t wifi_mode;          // 0=AP, 1=STA
    char wifi_ssid[33];
    char wifi_pass[65];
} treadlink_config_t;

esp_err_t config_store_init(void);
esp_err_t config_store_load(treadlink_config_t *config);
esp_err_t config_store_save(const treadlink_config_t *config);
void config_store_defaults(treadlink_config_t *config);
