/*
 * JJY-SIM R3 - Settings (API)
 *
 * Provides application settings management.
 *
 * - Defines configuration structure (Wi-Fi, band, timezone, etc.)
 * - Loads and saves settings using NVS
 * - Provides default initialization and partial reset
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 */
#ifndef _SETTINGS_H_
#define _SETTINGS_H_

#include <stdint.h>
#include "esp_err.h"

typedef struct {
  char ssid[64];
  char pass[64];
  int band;         // JJY band (40 or 60 kHz)
  bool dst;         // Daylight saving (0:off, 1:on)
  bool hourly_mode; // True if sync assist is enabled
  float timezone;   // Timezone (e.g. 9.0, 5.5, 5.75)
  int disp_mode;    // Display mode (0:Normal, 1:Large)
  int brightness;   // Display brightness
  bool night_mode;  // True if night mode is enabled
  bool wifi_valid;  // True if valid Wi-Fi settings are loaded
} app_settings_t;

// Global settings instance
extern app_settings_t s_settings;

#define WIFI_NAMESPACE          "wifi_cfg"
#define WIFI_KEY_SSID           "ssid"
#define WIFI_KEY_PASS           "pass"
#define WIFI_KEY_BAND           "band"
#define WIFI_KEY_DST            "dst"
#define WIFI_KEY_TZ             "tz"
#define WIFI_KEY_DISP           "disp"
#define WIFI_KEY_BRIGHT         "bright"
#define WIFI_KEY_NIGHT          "night"
#define WIFI_KEY_HOURLY         "hourly"

// Set default configuration values
void settings_set_defaults(app_settings_t *cfg);

// Load settings from NVS
bool load_settings(app_settings_t *cfg);

// Save settings to NVS
esp_err_t save_settings(const app_settings_t *cfg);

// Clear only Wi-Fi related settings
void clear_wifi_settings_only(void);

#endif
