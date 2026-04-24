/*
 * JJY-SIM R3
 *
 * JJY signal simulator using ESP32 / ESP8684.
 *
 * - Generates 40kHz / 60kHz JJY signal
 * - Synchronizes time via NTP over Wi-Fi
 * - Provides OLED display for status and time
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
#include <math.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "driver/i2c_master.h"
#include "esp_app_desc.h"

#include "jjy-sim-r3.h"
#include "jjy_pwm.h"
#include "led.h"
#include "settings.h"
#include "wifi_mng.h"

#include "OLEDDisplay.h"
#include "resource/shachi-lab_logo.h"
#include "resource/icon_image.h"
#include "resource/DejaVu_Sans_Condensed_Bold_16.h"
#include "resource/DejaVu_Sans_Condensed_Bold_38.h"

static const char *TAG __attribute__((unused)) = "main";

static char JJY_char = JJY_CHAR_EAST;     // JJY signal character
static char tz_str[10];                   // timezone string
static int config_wait_remain;            // configuration remain
static char *config_str = "";             // configuration string
static char *time_str = NULL;             // time string

typedef enum {
  DISP_REBOOT      = -3,
  DISP_CONFIG_MODE = -2,
  DISP_WAIT_CONFIG = -1,
  DISP_INIT_LOGO   = 0,
  DISP_VERSION     = 1,
  DISP_CONNECTING  = 2,
  DISP_CONNECTED   = 3,
  DISP_WAIT_0SEC   = 4,
  DISP_NTP_SYNCING = 5,
  DISP_SENDING     = 6
} disp_screen_t;

static void disp_screen(disp_screen_t mode);

// SNTPを使って時刻を同期する
static bool wait_for_time_sync(int timeout_ms)
{
  ESP_LOGI(TAG, "Starting SNTP");

  // SNTP設定
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "ntp.nict.jp");  // 日本ならこれが安定
  esp_sntp_init();

  int retry = 0;
  int max_retry = timeout_ms > 0 ? timeout_ms / 500 : 0;

  time_t now = 0;
  struct tm timeinfo = {0};

  while (max_retry == 0 || retry < max_retry) {
    time(&now);
    localtime_r(&now, &timeinfo);

    // 年が2020未満なら未同期とみなす
    if (timeinfo.tm_year >= (2020 - 1900)) {
      sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
      ESP_LOGI(TAG, "Time synchronized");
      ESP_LOGI(TAG, "Current time: %04d-%02d-%02d %02d:%02d:%02d",
                timeinfo.tm_year + 1900,
                timeinfo.tm_mon + 1,
                timeinfo.tm_mday,
                timeinfo.tm_hour,
                timeinfo.tm_min,
                timeinfo.tm_sec);
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    retry++;
  }
  ESP_LOGW(TAG, "SNTP sync timeout");
  return false;
}

// 時刻同期待ち
static bool main_wait_for_config(void)
{
  gpio_config_t io_conf = {
    .pin_bit_mask = (1ULL << PIN_CONFIG),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE
  };
  gpio_config(&io_conf);

  config_wait_remain = CONFIG_WAIT_TIME / 1000;
  for (int i = CONFIG_WAIT_TIME/CONFIG_WAIT_TICK; i; i--) {
    disp_screen(DISP_WAIT_CONFIG);
    config_wait_remain = i / (1000/CONFIG_WAIT_TICK) + 1;
    vTaskDelay(pdMS_TO_TICKS(CONFIG_WAIT_TICK));
    if (gpio_get_level(PIN_CONFIG) == 0) {
      return true;
    }
  }
  return false;
}

// Wi-Fi接続状態に応じたアイコンを返して、LEDを制御
static const uint8_t *get_wifi_icon_led(wifi_status_t status, bool ntp_flag)
{ 
  static const uint8_t *icon = NULL;

  switch (status) {
  case WIFI_STATUS_DISCONNECTED :
    icon = wifi_12x12_off_bits;
    led_blink(PIN_LED_WIFI, LED_MODE_BLINK_SLOW);
    break;
  case WIFI_STATUS_CONNECTED :
    icon = wifi_12x12_on_bits;
    led_blink(PIN_LED_WIFI, ntp_flag ? LED_MODE_BLINK_FAST : LED_MODE_ON);
    break;
  case WIFI_STATUS_CONNECTING :
    icon = (icon == wifi_12x12_conn1_bits) ? wifi_12x12_conn2_bits : wifi_12x12_conn1_bits;
    led_blink(PIN_LED_WIFI, LED_MODE_BLINK_SLOW);
    break;
  default :
    icon = NULL;
    led_blink(PIN_LED_WIFI, LED_MODE_OFF);
    break;
  }
  return icon;
}

// I2CベースのOLEDの初期化
static i2c_master_bus_handle_t i2c_bus_init(void)
{
  i2c_master_bus_config_t bus_cfg = {
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .i2c_port = I2C_NUM_0,
    .sda_io_num = PIN_OLED_SDA,
    .scl_io_num = PIN_OLED_SCL,
    .glitch_ignore_cnt = 7,
    .flags.enable_internal_pullup = true,
  };
  i2c_master_bus_handle_t i2c_bus;
  esp_err_t ret = i2c_new_master_bus(&bus_cfg, &i2c_bus);
  if (ret != ESP_OK) {
    return NULL;
  }
  return i2c_bus;
}

static const uint8_t brightness_table[11] = {
    0,    // 0: 完全消灯
    5,    // 1: ほぼ消灯に近い（でも見える）
    10,   // 2
    20,   // 3
    40,   // 4
    80,   // 5: 普通
    120,  // 6
    160,  // 7
    200,  // 8
    230,  // 9
    255   // 10: 最大
};


void disp_brightness(void)
{
  if (s_settings.brightness < 1 || s_settings.brightness > 10) {
    s_settings.brightness = 10;
  }
  OLEDDisplay_setBrightness(brightness_table[s_settings.brightness]);
}

static void disp_fade(bool direction)
{
  for (uint8_t i = 0; i < sizeof(brightness_table); i++) {
    uint8_t idx = direction ? sizeof(brightness_table) - 1 - i : i;
    OLEDDisplay_setBrightness(brightness_table[idx]);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// OLEDの画面描画
static void disp_screen(disp_screen_t mode)
{ 
  char buff_str[40];
  static i2c_master_bus_handle_t i2c_bus = NULL;
  static bool sw_old = false;
  static uint8_t button_count = 0;
  static bool jjy_enable = true;

  if (i2c_bus == NULL) {
    i2c_bus = i2c_bus_init();
    if (i2c_bus == NULL) return;

    oled_config_t oled_cfg = OLED_CONFIG_DEFAULT();
    oled_cfg.i2c.bus = i2c_bus;
    oled_cfg.rst_pin = PIN_OLED_RST;
    OLEDDisplay_config(&oled_cfg);
    OLEDDisplay_init();
    OLEDDisplay_flipScreenVertically();
  }

  if (mode == DISP_INIT_LOGO) {
    OLEDDisplay_drawBuffer(shachilab_logo_bits, sizeof(shachilab_logo_bits));
    OLEDDisplay_display();
    return;
  }

  OLEDDisplay_clear();
  OLEDDisplay_setFont(ArialMT_Plain_10);

  OLEDDisplay_drawStringf( 0,  0, buff_str,
     JJYSIM_PRODUCT_NAME "      %c",
     jjy_enable ? JJY_char : 'X');

  const uint8_t *icon = get_wifi_icon_led(wifi_get_status(), mode == DISP_NTP_SYNCING); 
  if (icon)  OLEDDisplay_drawXbm(116, 2, 12, 12, icon);

  do {
    if (mode == DISP_VERSION || mode <= DISP_WAIT_CONFIG) {
      const esp_app_desc_t *app = esp_app_get_description();
      OLEDDisplay_drawStringf( 0, 14, buff_str, "Version %s - " JJYSIM_CHIP_FAMILY, (char*)app->version );
    
      if (mode == DISP_VERSION) {
        OLEDDisplay_drawString( 0, 30, JJYSIM_COPYRIGHT );
        OLEDDisplay_drawString( 0, 42, JJYSIM_VENDOR_NAME );
      } else
      if (mode == DISP_WAIT_CONFIG) {
        OLEDDisplay_drawString( 0, 30, "Waiting for CONFIG" );
        OLEDDisplay_drawStringf(0, 42, buff_str, "%d sec left", config_wait_remain );
      } else
      if (mode == DISP_CONFIG_MODE) {  
        OLEDDisplay_drawString( 0, 30, config_str );
        OLEDDisplay_drawString( 0, 42, "Enter to CONFIG mode" );
      } else
      if (mode == DISP_REBOOT) {
        OLEDDisplay_drawString( 0, 42, "Reboot now!!" );
      }
      break;
    }
    if (mode < DISP_CONNECTING) break;

    if (mode == DISP_SENDING) {
      if (gpio_get_level(PIN_CONFIG) == 0) {
        if (!sw_old) {
          s_settings.disp_mode = !s_settings.disp_mode;
        } else {
          if (++button_count > 4) {
              jjy_enable = !jjy_enable;
              jjy_set_signal_enable(jjy_enable);
              button_count = 0;
            }
        }
        sw_old = true;
      } else {
        sw_old = false;
        button_count = 0;
      }
      if (s_settings.disp_mode == 0) {
        if (time_str == NULL) break;
        if (s_settings.dst) {
          OLEDDisplay_drawString(108, 35, "DST");
        }
        time_str[10] = '\0';
        time_str[16] = '\0';
        OLEDDisplay_setFont((uint8_t*)DejaVu_Sans_Condensed_Bold_16);
        OLEDDisplay_drawString(  0, 14, time_str+ 2);
        OLEDDisplay_drawString(109, 49, time_str+17);
        jjy_get_week_str(buff_str);
        OLEDDisplay_setTextAlignment(TEXT_ALIGN_CENTER);
        OLEDDisplay_drawString(110, 14, buff_str+ 0);
        OLEDDisplay_setTextAlignment(TEXT_ALIGN_LEFT);

        OLEDDisplay_setFont((uint8_t*)DejaVu_Sans_Condensed_Bold_38);
        if (time_str[11] == '0') {
          OLEDDisplay_drawString( 23, 28, time_str+12);
        } else {
          OLEDDisplay_drawString( -1, 28, time_str+11);
        }
        break;
      }
    }

    OLEDDisplay_drawStringf( 0, 14, buff_str, "SSID : %s", s_settings.ssid );

    if (mode == DISP_CONNECTING) {
      OLEDDisplay_drawString( 0, 24, "Connecting..." );
    } else {
      OLEDDisplay_drawStringf( 0, 24, buff_str, "Connect : %s", get_wifi_addr_str() );
      OLEDDisplay_drawString( 78, 34, tz_str );

      if (mode == DISP_NTP_SYNCING) {
        OLEDDisplay_drawString( 0, 34, "NTP SYNCING...");
      } else
      if (mode == DISP_WAIT_0SEC) {
        OLEDDisplay_drawString( 0, 34, "WAITING 00s" );
      } else
      if (mode == DISP_SENDING) {
        if (jjy_enable) {
          OLEDDisplay_drawString( 0, 34, "SENDING...");
        } else {
          OLEDDisplay_drawString( 0, 34, "NOT SENDING");
        }
        if (time_str == NULL) break;
        OLEDDisplay_setFont(ArialMT_Plain_16);
        OLEDDisplay_drawString( 0, 46, time_str+2);
      }
    }
  } while (0);

  disp_brightness();
  OLEDDisplay_display();
}

// main
void app_main(void)
{
  led_init();
  led_blink(PIN_LED_WIFI, LED_MODE_ON);
  led_blink(PIN_LED_PPS , LED_MODE_ON);

  const esp_app_desc_t *app = esp_app_get_description();

  printf("\n%s", JJYSIM_PRODUCT_NAME);
  printf("\n%s", app->version);
  printf("\n%s", JJYSIM_COPYRIGHT);

  disp_screen(DISP_INIT_LOGO);
  disp_fade(false);

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  load_settings(&s_settings);

  if (s_settings.band == JJY_BAND_WEST) JJY_char = JJY_CHAR_WEST;
  jjy_get_tz_str(tz_str, s_settings.timezone, s_settings.dst);
  if (s_settings.dst) strcat(tz_str, "*");

  vTaskDelay(pdMS_TO_TICKS(2000));
  disp_fade(true);

  led_blink(PIN_LED_WIFI, LED_MODE_OFF);
  led_blink(PIN_LED_PPS , LED_MODE_OFF);

  disp_screen(DISP_VERSION);
  disp_fade(false);
  vTaskDelay(pdMS_TO_TICKS(2000));

  wifi_init_common();

  ESP_LOGI(TAG, "Wi-Fi Valid : %d", s_settings.wifi_valid);
  if (s_settings.wifi_valid) {

    ESP_LOGI(TAG, "Saved Wi-Fi found: %s", s_settings.ssid);
    ESP_LOGI(TAG, "JJY settings: band=%d dst=%d tz=%.2f",
            s_settings.band, s_settings.dst, s_settings.timezone);
  
    led_blink(PIN_LED_PPS , LED_MODE_BLINK_SLOW);
    if (main_wait_for_config() == false) {

      led_blink(PIN_LED_PPS , LED_MODE_OFF);
      wifi_start_sta_only(s_settings.ssid, s_settings.pass);

      for (uint8_t i = 0; i < CONNECT_TIMEOUT; i++) {
        disp_screen(DISP_CONNECTING);
        if (is_wifi_connected()) break;
      }

      if (wifi_get_status() == WIFI_STATUS_CONNECTED) {

        ESP_LOGI(TAG, "Connected to Wi-Fi");
        disp_screen(DISP_NTP_SYNCING);

        while (1) {
          if (wait_for_time_sync(10000)) {
            // 時刻OK
            jjy_pwm_init(s_settings.band, s_settings.dst, s_settings.timezone);
            while (1) {
              time_str = jjy_pwm_proc();
              disp_screen(DISP_SENDING);
            }
          } else {                
            // リトライ
            ESP_LOGW(TAG, "Time sync failed");
          }
        }  
      } else { 
        config_str = "WiFi connection timeout.";
      }
    } else {
      config_str = "[CONFIG] was pressed.";
    }
  } else {
    config_str = "SSID is not set.";
  }

  ESP_LOGI(TAG, "%s", config_str);
  ESP_LOGI(TAG, "Enter to CONFIG mode");
  disp_screen(DISP_CONFIG_MODE);
  led_blink(PIN_LED_WIFI, LED_MODE_OFF);
  led_blink(PIN_LED_PPS , LED_MODE_ON);
  enter_setup_mode();

  while (is_reboot_req() == false) vTaskDelay(pdMS_TO_TICKS(1000));

  disp_screen(DISP_REBOOT);
  led_blink(PIN_LED_WIFI, LED_MODE_BLINK_FAST);
  led_blink(PIN_LED_PPS , LED_MODE_BLINK_FAST);

  vTaskDelay(pdMS_TO_TICKS(2000));
  esp_restart();
}
