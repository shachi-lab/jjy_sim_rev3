/*
 * JJY-SIM R3 - Settings Management
 *
 * Handles application settings stored in NVS.
 *
 * - Loads and saves configuration (Wi-Fi, band, timezone, etc.)
 * - Encrypts SSID and password before storage
 * - Provides default initialization and partial reset functions
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 *
 * Developed by Shachi-lab
 * https://shachi-lab.com
 *   ____  _                _     _     _       _
 *  / ___)| |__   __ _  ___| |__ |_|   | | __ _| |__
 *  \___ \| '_ \ / _` |/ __) '_ \ _  _ | |/ _` | '_ \
 *   ___) | | | | (_| | (__| | | | ||_|| | (_| | |_) |
 *  (____/|_| |_|\__,_|\___)_| |_|_|   |_|\__,_|_.__/
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "settings.h"
#include "storage_crypto.h"

static const char *TAG __attribute__((unused)) = "settings";

app_settings_t s_settings = {
  .ssid = "",
  .pass = "",
  .band = 40,
  .timezone = 9.0f,
  .dst = 0,
  .hourly_mode = false,
  .wifi_valid = false,
  .disp_mode = 0,
  .brightness = 100,
  .night_mode = false
};

void settings_set_defaults(app_settings_t *cfg)
{
  memset(cfg, 0, sizeof(*cfg));
  cfg->band = 40;
  cfg->timezone = 9.0f;
  cfg->dst = 0;
  cfg->hourly_mode = false,
  cfg->wifi_valid = false;
  cfg->disp_mode = 0;
  cfg->brightness = 10;
  cfg->night_mode = false;
}

bool load_settings(app_settings_t *cfg)
{
  settings_set_defaults(cfg);

  nvs_handle_t nvs;
  esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    return false;
  }

  storage_crypto_blob_t ssid_blob;
  size_t len = sizeof(ssid_blob);
  err = nvs_get_blob(nvs, WIFI_KEY_SSID, &ssid_blob, &len);
  storage_decrypt(&ssid_blob, cfg->ssid, sizeof(cfg->ssid));
  if (err == ESP_OK && cfg->ssid[0] != '\0') {
    cfg->wifi_valid = true;
  }

  storage_crypto_blob_t pass_blob;
  len = sizeof(pass_blob);
  err = nvs_get_blob(nvs, WIFI_KEY_PASS, &pass_blob, &len);
  storage_decrypt(&pass_blob, cfg->pass, sizeof(cfg->pass));
  if (err != ESP_OK) {
    cfg->pass[0] = '\0';
  }

  int32_t v32;
  if (nvs_get_i32(nvs, WIFI_KEY_BAND, &v32) == ESP_OK) {
    cfg->band = (int)v32;
  }
  if (nvs_get_i32(nvs, WIFI_KEY_DST, &v32) == ESP_OK) {
    cfg->dst = (int)v32;
  }
  if (nvs_get_i32(nvs, WIFI_KEY_HOURLY, &v32) == ESP_OK) {
    cfg->hourly_mode = (int)v32;
  }

  char tzbuf[16];
  len = sizeof(tzbuf);
  if (nvs_get_str(nvs, WIFI_KEY_TZ, tzbuf, &len) == ESP_OK) {
    cfg->timezone = strtof(tzbuf, NULL);
  }
  if (nvs_get_i32(nvs, WIFI_KEY_DISP, &v32) == ESP_OK) {
    cfg->disp_mode = (int)v32;
  }
  if (nvs_get_i32(nvs, WIFI_KEY_BRIGHT, &v32) == ESP_OK) {
    cfg->brightness = (int)v32;
  }
  if (nvs_get_i32(nvs, WIFI_KEY_NIGHT, &v32) == ESP_OK) {
    cfg->night_mode = (int)v32;
  }

  nvs_close(nvs);
  return true;
}

esp_err_t save_settings(const app_settings_t *cfg)
{
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    return err;
  }

  storage_crypto_blob_t ssid_blob;
  storage_crypto_blob_t pass_blob;
  storage_encrypt(cfg->ssid, &ssid_blob);
  storage_encrypt(cfg->pass, &pass_blob);

  err = nvs_set_blob(nvs, WIFI_KEY_SSID, &ssid_blob, sizeof(ssid_blob));
  if (err == ESP_OK) err = nvs_set_blob(nvs, WIFI_KEY_PASS, &pass_blob, sizeof(pass_blob));
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_BAND, cfg->band);
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_DST, cfg->dst);
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_HOURLY, cfg->hourly_mode);
  if (err == ESP_OK) {
    char tzbuf[16];
    snprintf(tzbuf, sizeof(tzbuf), "%.2f", cfg->timezone);
    err = nvs_set_str(nvs, WIFI_KEY_TZ, tzbuf);
  }
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_DISP, cfg->disp_mode);
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_BRIGHT, cfg->brightness);
  if (err == ESP_OK) err = nvs_set_i32(nvs, WIFI_KEY_NIGHT, cfg->night_mode);
  if (err == ESP_OK) err = nvs_commit(nvs);
  
  nvs_close(nvs);
  return err;
}

void clear_wifi_settings_only(void)
{
  nvs_handle_t nvs;
  if (nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_erase_key(nvs, WIFI_KEY_SSID);
    nvs_erase_key(nvs, WIFI_KEY_PASS);
    nvs_commit(nvs);
    nvs_close(nvs);
  }
}

