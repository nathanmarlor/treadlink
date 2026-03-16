#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "config_store.h"

// Input from FTMS treadmill
typedef struct {
    uint16_t speed_001kmh;       // Speed in 0.01 km/h (or mph if misconfigured)
    int16_t  incline_01pct;      // Incline in 0.1% (signed)
    uint32_t total_distance_m;   // Total distance in meters
    bool     has_incline;
    bool     has_distance;
} ftms_treadmill_data_t;

// Output for RSC (footpod)
typedef struct {
    uint16_t speed_256ths_mps;   // Speed in m/s * 256
    uint8_t  cadence_spm;        // Cadence in strides/min
    uint16_t stride_length_cm;   // Stride length in cm
    uint32_t total_distance_dm;  // Total distance in decimeters
    bool     is_running;         // Walking vs running flag
} rsc_data_t;

void data_bridge_init(const treadlink_config_t *config);
void data_bridge_update_config(const treadlink_config_t *config);
rsc_data_t data_bridge_convert(const ftms_treadmill_data_t *ftms);

// Get the last converted speed in km/h (post mph correction) for UI display
float data_bridge_get_display_speed_kmh(void);
bool data_bridge_is_mph_mode(void);
