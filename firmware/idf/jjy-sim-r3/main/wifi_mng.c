/*
* JJY-SIM R3
*
* Wi-Fi management module
* - Handles connection and reconnection
* - Provides status for UI
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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"

#include "jjy-sim-r3.h"
#include "settings.h"
#include "wifi_mng.h"
#include "wifi_mng_def.h"

static const char *TAG __attribute__((unused)) = "wifi_mng";

#define MAX_AP_RECORDS          20
#define DNS_PORT                53
#define DNS_MAX_LEN             512

#define STA_CONNECT_TIMEOUT_MS  1000

static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static httpd_handle_t s_httpd = NULL;
static esp_ip4_addr_t s_ap_ip;
static wifi_ap_record_t s_scan_records[MAX_AP_RECORDS];
static uint16_t s_scan_count = 0;
static char ip_str[16] = {0};
static bool setup_mode = false;
static bool reboot_req = false;
static wifi_status_t wifi_status = WIFI_STATUS_UNKNOWN;

void disp_brightness(int hour);

static void reboot_req_handler(void)
{
  reboot_req = true;
  vTaskDelay(pdMS_TO_TICKS(5000));
  esp_restart();
}

static void html_begin(httpd_req_t *req)
{
  httpd_resp_set_type(req, "text/html; charset=utf-8");
}

static void redirect_root(httpd_req_t *req)
{
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_send(req, "Redirect", HTTPD_RESP_USE_STRLEN);
}

static void url_decode(char *dst, const char *src, size_t dst_len)
{
  size_t di = 0;
  for (size_t i = 0; src[i] != '\0' && di + 1 < dst_len; i++) {
    if (src[i] == '+') {
      dst[di++] = ' ';
    } else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
      char hex[3] = { src[i + 1], src[i + 2], 0 };
      dst[di++] = (char)strtol(hex, NULL, 16);
      i += 2;
    } else {
      dst[di++] = src[i];
    }
  }
  dst[di] = '\0';
}

static void form_get_value(const char *body, const char *key, char *out, size_t out_len)
{
  out[0] = '\0';
  const char *p = strstr(body, key);
  if (!p) return;
  p += strlen(key);
  const char *e = strchr(p, '&');
  size_t len = e ? (size_t)(e - p) : strlen(p);

  char temp[256];
  if (len >= sizeof(temp)) len = sizeof(temp) - 1;
  memcpy(temp, p, len);
  temp[len] = '\0';
  url_decode(out, temp, out_len);
}

static void make_ap_ssid(char *out, size_t out_len)
{
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(out, out_len, WIFI_AP_SSID_PREFIX "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
  static bool is_started = false;

  if (event_base == WIFI_EVENT) {
    switch (event_id) {
    case WIFI_EVENT_STA_START:
      if (setup_mode) return;   
      is_started = true;
      wifi_status = WIFI_STATUS_CONNECTING;
      ESP_LOGI(TAG, "STA start");
      esp_wifi_connect();
      break;

    case WIFI_EVENT_STA_DISCONNECTED:
      if (setup_mode) return;  
      ESP_LOGW(TAG, "STA disconnected");
      xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
      ESP_LOGI(TAG, "STA retry");
      esp_wifi_connect();
      if (is_started == false) wifi_status = WIFI_STATUS_DISCONNECTED;
      break;

    case WIFI_EVENT_AP_START:
      setup_mode = true;
      is_started = false;
      wifi_status = WIFI_STATUS_UNKNOWN;
      ESP_LOGI(TAG, "SoftAP start");
      break;

    default:
      break;
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
    is_started = false;
    wifi_status = WIFI_STATUS_CONNECTED;
    ESP_LOGI(TAG, "STA got IP");
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

void wifi_init_common(void)
{
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  s_wifi_event_group = xEventGroupCreate();

  if (s_sta_netif == NULL) {
    s_sta_netif = esp_netif_create_default_wifi_sta();
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(
    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &wifi_event_handler,
                                        NULL,
                                        NULL));

  ESP_ERROR_CHECK(
    esp_event_handler_instance_register(IP_EVENT,
                                        IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler,
                                        NULL,
                                        NULL));
}

static esp_err_t start_wifi_scan(void)
{
  wifi_scan_config_t scan_config = {
    .ssid = NULL,
    .bssid = NULL,
    .channel = 0,
    .show_hidden = false,
    .scan_type = WIFI_SCAN_TYPE_ACTIVE,
  };

  ESP_LOGI(TAG, "Starting Wi-Fi scan...");
  esp_err_t err = esp_wifi_scan_start(&scan_config, true);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "scan start failed: %s", esp_err_to_name(err));
    return err;
  }

  s_scan_count = MAX_AP_RECORDS;
  err = esp_wifi_scan_get_ap_records(&s_scan_count, s_scan_records);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "get ap records failed: %s", esp_err_to_name(err));
    return err;
  }

  ESP_LOGI(TAG, "Scan done: %u APs", s_scan_count);
  return ESP_OK;
}

#if 0
static void wifi_stop_all(void)
{
  esp_wifi_stop();
}
#endif

void wifi_start_sta_only(const char *ssid, const char *pass)
{
  if (s_sta_netif == NULL) {
    s_sta_netif = esp_netif_create_default_wifi_sta();
  }
  setup_mode = false;

  wifi_config_t wifi_config = {0};
  strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
  strlcpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
  wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "Trying STA connect to SSID=%s", ssid);
}

static void dhcps_set_option114(esp_netif_t *netif)
{
  const char *uri = "http://192.168.4.1/";
  esp_err_t err = esp_netif_dhcps_option(netif,
                                          ESP_NETIF_OP_SET,
                                          ESP_NETIF_CAPTIVEPORTAL_URI,
                                          (void *)uri,
                                          strlen(uri));
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "DHCP option 114 set: %s", uri);
  } else {
    ESP_LOGW(TAG, "Failed to set DHCP option 114: %s", esp_err_to_name(err));
  }
}

static void wifi_start_apsta_for_setup(void)
{
  wifi_status = WIFI_STATUS_UNKNOWN;

  if (s_ap_netif == NULL) {
    s_ap_netif = esp_netif_create_default_wifi_ap();
  }
  if (s_sta_netif == NULL) {
    s_sta_netif = esp_netif_create_default_wifi_sta();
  }

  char ap_ssid[32];
  make_ap_ssid(ap_ssid, sizeof(ap_ssid));

  wifi_config_t ap_config = {0};
  strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
  ap_config.ap.ssid_len = strlen(ap_ssid);
  ap_config.ap.channel = 1;
  ap_config.ap.max_connection = 4;
  ap_config.ap.authmode = WIFI_AUTH_OPEN;

  if (strlen(WIFI_AP_PASS) > 0) {
    strlcpy((char *)ap_config.ap.password, WIFI_AP_PASS, sizeof(ap_config.ap.password));
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  esp_netif_ip_info_t ip_info;
  ESP_ERROR_CHECK(esp_netif_get_ip_info(s_ap_netif, &ip_info));
  s_ap_ip = ip_info.ip;

  dhcps_set_option114(s_ap_netif);

  ESP_LOGI(TAG, "Setup AP started: %s", ap_ssid);
  ESP_LOGI(TAG, "AP IP: " IPSTR, IP2STR(&s_ap_ip));

  start_wifi_scan();
}

static int dns_skip_name(const uint8_t *buf, int len, int off)
{
  while (off < len) {
    uint8_t c = buf[off];
    if (c == 0) return off + 1;
    if ((c & 0xC0) == 0xC0) return off + 2;
    off += c + 1;
  }
  return -1;
}

static int dns_build_answer(uint8_t *resp, int req_len, uint32_t ip_be)
{
  if (req_len < 12) return -1;

  resp[2] = 0x81;
  resp[3] = 0x80;
  resp[4] = 0x00; resp[5] = 0x01;
  resp[6] = 0x00; resp[7] = 0x01;
  resp[8] = 0x00; resp[9] = 0x00;
  resp[10] = 0x00; resp[11] = 0x00;

  int qname_end = dns_skip_name(resp, req_len, 12);
  if (qname_end < 0 || qname_end + 4 > req_len) return -1;

  int pos = qname_end + 4;

  resp[pos++] = 0xC0;
  resp[pos++] = 0x0C;
  resp[pos++] = 0x00;
  resp[pos++] = 0x01;
  resp[pos++] = 0x00;
  resp[pos++] = 0x01;
  resp[pos++] = 0x00;
  resp[pos++] = 0x00;
  resp[pos++] = 0x00;
  resp[pos++] = 0x3C;
  resp[pos++] = 0x00;
  resp[pos++] = 0x04;

  memcpy(&resp[pos], &ip_be, 4);
  pos += 4;

  return pos;
}

static void dns_server_task(void *arg)
{
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    ESP_LOGE(TAG, "DNS socket create failed");
    vTaskDelete(NULL);
    return;
  }

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(DNS_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "DNS bind failed");
    close(sock);
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "DNS server started");

  while (1) {
    uint8_t req[DNS_MAX_LEN];
    struct sockaddr_in from = {0};
    socklen_t from_len = sizeof(from);

    int len = recvfrom(sock, req, sizeof(req), 0,
                        (struct sockaddr *)&from, &from_len);
    if (len < 12) continue;

    uint16_t qdcount = ((uint16_t)req[4] << 8) | req[5];
    if (qdcount == 0) continue;

    uint8_t resp[DNS_MAX_LEN];
    memcpy(resp, req, len);

    int resp_len = dns_build_answer(resp, len, s_ap_ip.addr);
    if (resp_len > 0) {
      sendto(sock, resp, resp_len, 0,
            (struct sockaddr *)&from, from_len);
    }
  }
}

static void html_tz_option(httpd_req_t *req, float val, const char *label)
{
  char line[256];
  snprintf(line, sizeof(line),
            "<option value='%.2f'%s>%s</option>",
            val,
            (fabsf(val - s_settings.timezone) < 0.01f) ? " selected" : "",
            label);
  httpd_resp_sendstr_chunk(req, line);
}

static void httpd_resp_send_header(httpd_req_t *req)
{
  httpd_resp_sendstr_chunk(req,
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>" WIFI_AP_TITLE "</title>"
    "<style>" WIFI_AP_CSS_STYLE "</style>"
    "</head><body>");
}

static void httpd_resp_send_footer(httpd_req_t *req)
{
  httpd_resp_sendstr_chunk(req, "</body></html>");
  httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
  html_begin(req);
  httpd_resp_send_header(req);

  httpd_resp_sendstr_chunk(req, "<h1>" WIFI_AP_TITLE "</h1>");
  httpd_resp_sendstr_chunk(req, "<form method='POST' action='/save'>");

  httpd_resp_sendstr_chunk(req, "<fieldset><legend>Wi-Fi</legend>");
  httpd_resp_sendstr_chunk(req, "<label>SSID</label><select name='ssid'>");

  char line[512];
  for (int i = 0; i < s_scan_count; i++) {
    const char *ssid = (const char *)s_scan_records[i].ssid;
    if (ssid[0] == '\0') continue;

    snprintf(line, sizeof(line),
        "<option value='%s'%s>%s (RSSI=%d)</option>",
        ssid,
        (strcmp(ssid, s_settings.ssid) == 0) ? " selected" : "",
        ssid,
        s_scan_records[i].rssi);
    httpd_resp_sendstr_chunk(req, line);
  }

  httpd_resp_sendstr_chunk(req, "</select>");
  httpd_resp_sendstr_chunk(req, "<label>Password</label>");

  snprintf(line, sizeof(line),
      "<input type='password' name='pass' value='%s' style='width:100%%'>",
      s_settings.pass);
  httpd_resp_sendstr_chunk(req, line);
  httpd_resp_sendstr_chunk(req, "</fieldset>");

  httpd_resp_sendstr_chunk(req, "<fieldset><legend>JJY Settings</legend>");

  httpd_resp_sendstr_chunk(req, "<label>BAND</label><select name='band'>");
  httpd_resp_sendstr_chunk(req, s_settings.band == 40 ?
      "<option value='40' selected>40kHz</option><option value='60'>60kHz</option>" :
      "<option value='40'>40kHz</option><option value='60' selected>60kHz</option>");
  httpd_resp_sendstr_chunk(req, "</select>");

  httpd_resp_sendstr_chunk(req, "<label>Timezone</label><select name='tz'>");

  html_tz_option(req, -12.0f, "(UTC-12:00) Baker Island");
  html_tz_option(req, -11.0f, "(UTC-11:00) American Samoa");
  html_tz_option(req, -10.0f, "(UTC-10:00) Hawaii");
  html_tz_option(req,  -9.0f, "(UTC-09:00) Alaska");
  html_tz_option(req,  -8.0f, "(UTC-08:00) Los Angeles / Pacific Time");
  html_tz_option(req,  -7.0f, "(UTC-07:00) Denver / Mountain Time");
  html_tz_option(req,  -6.0f, "(UTC-06:00) Chicago / Central Time");
  html_tz_option(req,  -5.0f, "(UTC-05:00) New York / Eastern Time");
  html_tz_option(req,  -4.0f, "(UTC-04:00) Santiago / Atlantic Time");
  html_tz_option(req,  -3.5f, "(UTC-03:30) Newfoundland");
  html_tz_option(req,  -3.0f, "(UTC-03:00) Buenos Aires / Sao Paulo");
  html_tz_option(req,  -2.0f, "(UTC-02:00) South Georgia");
  html_tz_option(req,  -1.0f, "(UTC-01:00) Azores");
  html_tz_option(req,   0.0f, "(UTC+00:00) London / Lisbon");
  html_tz_option(req,   1.0f, "(UTC+01:00) Berlin / Paris");
  html_tz_option(req,   2.0f, "(UTC+02:00) Cairo / Athens");
  html_tz_option(req,   3.0f, "(UTC+03:00) Moscow / Nairobi");
  html_tz_option(req,   3.5f, "(UTC+03:30) Tehran");
  html_tz_option(req,   4.0f, "(UTC+04:00) Dubai / Baku");
  html_tz_option(req,   4.5f, "(UTC+04:30) Kabul");
  html_tz_option(req,   5.0f, "(UTC+05:00) Karachi / Tashkent");
  html_tz_option(req,   5.5f, "(UTC+05:30) India / Colombo");
  html_tz_option(req,   5.75f,"(UTC+05:45) Nepal");
  html_tz_option(req,   6.0f, "(UTC+06:00) Dhaka / Almaty");
  html_tz_option(req,   6.5f, "(UTC+06:30) Yangon / Cocos Islands");
  html_tz_option(req,   7.0f, "(UTC+07:00) Bangkok / Jakarta");
  html_tz_option(req,   8.0f, "(UTC+08:00) Beijing / Singapore");
  html_tz_option(req,   9.0f, "(UTC+09:00) Tokyo / Seoul");
  html_tz_option(req,   9.5f, "(UTC+09:30) Darwin / Adelaide");
  html_tz_option(req,  10.0f, "(UTC+10:00) Sydney / Guam");
  html_tz_option(req,  11.0f, "(UTC+11:00) Solomon Islands / Magadan");
  html_tz_option(req,  12.0f, "(UTC+12:00) Fiji / Auckland");
  html_tz_option(req,  13.0f, "(UTC+13:00) Tonga / Samoa");

  httpd_resp_sendstr_chunk(req, "</select>");

  snprintf(line, sizeof(line),
    "<label class='checkrow'>"
    "<input type='checkbox' name='dst' value='1' %s>"
    "DST (+1 hour)"
    "</label>",
    s_settings.dst ? "checked" : ""
  );
  httpd_resp_sendstr_chunk(req, line);

  snprintf(line, sizeof(line),
    "<label class='checkrow'>"
    "<input type='checkbox' name='hourly' value='1' %s>"
    "Hourly mode (-5/+6 minutes)"
    "</label>",
    s_settings.hourly_mode ? "checked" : ""
  );
  httpd_resp_sendstr_chunk(req, line);

  httpd_resp_sendstr_chunk(req, "</fieldset>");

  httpd_resp_sendstr_chunk(req, "<fieldset><legend>Display Settings</legend>");

  httpd_resp_sendstr_chunk(req, "<label>Initial display mode</label><select name='disp'>");
  httpd_resp_sendstr_chunk(req, s_settings.disp_mode ?
      "<option value='0'>Clock</option><option value='1' selected>Status</option>" :
      "<option value='0' selected>Clock</option><option value='1'>Status</option>");
  httpd_resp_sendstr_chunk(req, "</select>");

  httpd_resp_sendstr_chunk(req,
    "<label for='bright'>OLED brightness</label>"
    "<div style='display:flex;align-items:center;gap:6px;'>"
    "<span>🌙</span>");

  snprintf(line, sizeof(line),
    "<input type='range' id='bright' name='bright' min='0' max='10' value='"
    "%d' oninput='setBrightness(this.value)'>"
    "<span>☀️</span>"
    "</div>",
    s_settings.brightness
  );
  httpd_resp_sendstr_chunk(req, line);

  snprintf(line, sizeof(line),
    "<label class='checkrow'>"
    "<input type='checkbox' name='night' value='1' %s>"
    "Night mode (22:00–7:00)"
    "</label>",
    s_settings.night_mode ? "checked" : ""
  );
  httpd_resp_sendstr_chunk(req, line);

  httpd_resp_sendstr_chunk(req, "</fieldset>");

  httpd_resp_sendstr_chunk(req,
      "<button type='submit'>保存して再起動</button></form>");

  httpd_resp_sendstr_chunk(req, "<div class='actions'>");

  httpd_resp_sendstr_chunk(req,
      "<hr><form method='POST' action='/scan'>"
      "<button type='submit'>Wi-Fi再スキャン</button></form>");

  httpd_resp_sendstr_chunk(req,
      "<form method='POST' action='/clear_wifi'>"
      "<button type='submit'>Wi-Fi設定クリア</button></form>");

  httpd_resp_sendstr_chunk(req,
      "<form method='POST' action='/reboot'>"
      "<button type='submit'>再起動</button></form>");

  httpd_resp_sendstr_chunk(req, "</div>");

  httpd_resp_sendstr_chunk(req,
      "<div class='nav'>"
      "<a href='/info'>本体情報</a>"
      "</div>");

  httpd_resp_sendstr_chunk(req,
    "<script>"
    "let brightTimer;"
    "function setBrightness(v){"
      "clearTimeout(brightTimer);"
      "brightTimer = setTimeout(()=>{"
        "fetch('/set_brightness', {"
          "method:'POST',"
          "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
          "body:'bright=' + encodeURIComponent(v)"
        "}).catch(()=>{});"
      "}, 150);"
    "}"
    "</script>");

    httpd_resp_send_footer(req);
  return ESP_OK;
}

static esp_err_t httpd_resp_send_text(httpd_req_t *req, char *text)
{
  html_begin(req);
  httpd_resp_send_header(req);
  httpd_resp_sendstr_chunk(req, text);
  httpd_resp_send_footer(req);
  return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
  char buf[768];
  int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (len <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    return ESP_FAIL;
  }
  buf[len] = '\0';

  ESP_LOGI(TAG, "POST: %s", buf);

  app_settings_t cfg = s_settings;
  char temp[64];

  form_get_value(buf, "ssid=", cfg.ssid, sizeof(cfg.ssid));
  form_get_value(buf, "pass=", cfg.pass, sizeof(cfg.pass));
  cfg.wifi_valid = (cfg.ssid[0] != '\0');

  form_get_value(buf, "band=", temp, sizeof(temp));
  if (temp[0]) cfg.band = atoi(temp);

  form_get_value(buf, "tz=", temp, sizeof(temp));
  if (temp[0]) cfg.timezone = strtof(temp, NULL);

  form_get_value(buf, "dst=", temp, sizeof(temp));
  cfg.dst = temp[0];

  form_get_value(buf, "hourly=", temp, sizeof(temp));
  cfg.hourly_mode = temp[0];

  form_get_value(buf, "disp=", temp, sizeof(temp));
  if (temp[0]) cfg.disp_mode = atoi(temp);

  form_get_value(buf, "bright=", temp, sizeof(temp));
  if (temp[0]) cfg.brightness = atoi(temp);

  form_get_value(buf, "night=", temp, sizeof(temp));
  cfg.night_mode = temp[0];

  esp_err_t err = save_settings(&cfg);
  if (err != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
    return ESP_FAIL;
  }

  s_settings = cfg;

  httpd_resp_send_text(req,
      "<h2>保存しました</h2>"
      "<h3>再起動します...</h3>");
  reboot_req_handler();
  return ESP_OK;
}

static esp_err_t scan_post_handler(httpd_req_t *req)
{
  start_wifi_scan();
  httpd_resp_set_status(req, "303 See Other");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

static esp_err_t clear_wifi_post_handler(httpd_req_t *req)
{
  clear_wifi_settings_only();
  httpd_resp_send_text(req,
      "<h2>Wi-Fi設定をクリアしました</h2>"
      "<h3>再起動します...</h3>");
  reboot_req_handler();
  return ESP_OK;
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
  httpd_resp_send_text(req,
      "<h2>再起動します...</h2>");
  reboot_req_handler();
  return ESP_OK;
}

static esp_err_t captive_get_handler(httpd_req_t *req)
{
  redirect_root(req);
  return ESP_OK;
}

static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
  redirect_root(req);
  return ESP_OK;
}

static void format_mac_addr(char *out, size_t out_len, const uint8_t mac[6])
{
  snprintf(out, out_len,
           "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void html_send_kv_ex(httpd_req_t *req, const char *key, const char *value, bool mono)
{
  char line[384];
  snprintf(line, sizeof(line),
           "<tr><th>%s</th><td%s>%s</td></tr>",
           key,
           mono ? " class='mono'" : "",
           value ? value : "");
  httpd_resp_sendstr_chunk(req, line);
}

static void html_send_kv(httpd_req_t *req, const char *key, const char *value)
{
  html_send_kv_ex(req, key, value, false);
}

static void html_send_kv_mono(httpd_req_t *req, const char *key, const char *value)
{
  html_send_kv_ex(req, key, value, true);
}

static void html_send_fieldset_open(httpd_req_t *req, const char *title)
{
    char line[128];

    snprintf(line, sizeof(line),
      "<fieldset><legend>%s</legend><table>",
      title);
    httpd_resp_sendstr_chunk(req, line);
}

static void html_send_fieldset_close(httpd_req_t *req)
{
  httpd_resp_sendstr_chunk(req, "</table></fieldset>");
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
  html_begin(req);
  httpd_resp_send_header(req);

  httpd_resp_sendstr_chunk(req, "<h1>JJY-SIM R3 本体情報</h1>");

  html_send_fieldset_open(req, "Product");
  html_send_kv(req, "Name", JJYSIM_PRODUCT_NAME);
  html_send_kv(req, "Vendor", JJYSIM_VENDOR_NAME);
  html_send_fieldset_close(req);

  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);

  const esp_app_desc_t *app = esp_app_get_description();

  uint8_t sta_mac[6] = {0};
  uint8_t ap_mac[6]  = {0};
  char sta_mac_str[32];
  char ap_mac_str[32];
  char temp[128];

  esp_read_mac(sta_mac, ESP_MAC_WIFI_STA);
  esp_read_mac(ap_mac, ESP_MAC_WIFI_SOFTAP);

  format_mac_addr(sta_mac_str, sizeof(sta_mac_str), sta_mac);
  format_mac_addr(ap_mac_str, sizeof(ap_mac_str), ap_mac);

  html_send_fieldset_open(req, "Device");
  html_send_kv(req, "Module", JJYSIM_MODULE_NAME);
  html_send_kv(req, "Chip", JJYSIM_CHIP_NAME);

  snprintf(temp, sizeof(temp), "%d", chip_info.revision);
  html_send_kv(req, "Chip revision", temp);

  uint32_t flash_size = 0;
  if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
    snprintf(temp, sizeof(temp), "%u MB",
             (unsigned int)(flash_size / (1024 * 1024)));
  } else {
    snprintf(temp, sizeof(temp), "unknown");
  }
  html_send_kv(req, "Flash size", temp);

  html_send_kv_mono(req, "STA MAC", sta_mac_str);
  html_send_kv_mono(req, "AP MAC", ap_mac_str);
  html_send_fieldset_close(req);

  snprintf(temp, sizeof(temp), "%s %s", app->date, app->time);

  html_send_fieldset_open(req, "Software");
  html_send_kv_mono(req, "Project", app->project_name);
  html_send_kv_mono(req, "Version", app->version);
  html_send_kv_mono(req, "ESP-IDF", app->idf_ver);
  html_send_kv_mono(req, "Build"  , temp);
  html_send_kv_mono(req, "Target" , CONFIG_IDF_TARGET);
  html_send_fieldset_close(req);

	snprintf(temp, sizeof(temp),"%s%s%s\n", 
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT " : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE " : "");

  html_send_fieldset_open(req, "Radio certification");
  html_send_kv(req, "Radio", temp);
  html_send_kv(req, "Module", JJYSIM_MODULE_NAME);
  html_send_kv_mono(req, "Certification ID", JJYSIM_CERTIFICATION);
  html_send_fieldset_close(req);

  html_send_fieldset_open(req, "License / Copyright");
  html_send_kv(req, "Copyright", JJYSIM_COPYRIGHT " " JJYSIM_VENDOR_NAME);
  html_send_kv(req, "License", JJYSIM_LICENSE_NAME);
  httpd_resp_sendstr_chunk(req,
    "<tr><th></th><td><a href='/license'>View License</a></td></tr>");
  html_send_fieldset_close(req);

  httpd_resp_sendstr_chunk(req,
    "<div class='nav'>"
    "<a href='/'>設定画面へ戻る</a>"
    "</div>");

  httpd_resp_send_footer(req);
  return ESP_OK;
}

static esp_err_t license_get_handler(httpd_req_t *req)
{
  html_begin(req);
  httpd_resp_send_header(req);

  httpd_resp_sendstr_chunk(req, "<h1>MIT License</h1>");

  httpd_resp_sendstr_chunk(req,
    "<pre style='white-space:pre-wrap;font-family:Consolas,monospace;'>"
    WIFI_AP_MIT_LICENSE
    "</pre>");

  httpd_resp_sendstr_chunk(req,
    "<div class='nav'><a href='/info'>戻る</a></div>");

  httpd_resp_send_footer(req);
  return ESP_OK;
}

static esp_err_t set_brightness_post_handler(httpd_req_t *req)
{
  char buf[128];
  int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (len <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    return ESP_FAIL;
  }
  buf[len] = '\0';

  char temp[32];
  form_get_value(buf, "bright=", temp, sizeof(temp));
  if (temp[0] == '\0') {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No bright");
    return ESP_FAIL;
  }

  s_settings.brightness = atoi(temp);
  disp_brightness(-1);

  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

static void start_web_server(void)
{
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 13;

  if (httpd_start(&s_httpd, &config) == ESP_OK) {
    httpd_uri_t root = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = root_get_handler
    };
    httpd_uri_t info = {
      .uri = "/info",
      .method = HTTP_GET,
      .handler = info_get_handler
    };
    httpd_uri_t save = {
      .uri = "/save",
      .method = HTTP_POST,
      .handler = save_post_handler
    };
    httpd_uri_t scan = {
      .uri = "/scan",
      .method = HTTP_POST,
      .handler = scan_post_handler
    };
    httpd_uri_t clear_wifi = {
      .uri = "/clear_wifi",
      .method = HTTP_POST,
      .handler = clear_wifi_post_handler
    };
    httpd_uri_t reboot = {
      .uri = "/reboot",
      .method = HTTP_POST,
      .handler = reboot_post_handler
    };
    httpd_uri_t generate_204 = {
      .uri = "/generate_204",
      .method = HTTP_GET,
      .handler = captive_get_handler
    };
    httpd_uri_t hotspot_detect = {
      .uri = "/hotspot-detect.html",
      .method = HTTP_GET,
      .handler = captive_get_handler
    };
    httpd_uri_t ncsi = {
      .uri = "/ncsi.txt",
      .method = HTTP_GET,
      .handler = captive_get_handler
    };
    httpd_uri_t connecttest = {
      .uri = "/connecttest.txt",
      .method = HTTP_GET,
      .handler = captive_get_handler
    };
    httpd_uri_t fwlink = {
      .uri = "/fwlink",
      .method = HTTP_GET,
      .handler = captive_get_handler
    };
    httpd_uri_t license = {
      .uri = "/license",
      .method = HTTP_GET,
      .handler = license_get_handler
    };
    httpd_uri_t set_brightness_uri = {
      .uri      = "/set_brightness",
      .method   = HTTP_POST,
      .handler  = set_brightness_post_handler,
      .user_ctx = NULL
    };

    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &info);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &scan);
    httpd_register_uri_handler(s_httpd, &clear_wifi);
    httpd_register_uri_handler(s_httpd, &reboot);
    httpd_register_uri_handler(s_httpd, &generate_204);
    httpd_register_uri_handler(s_httpd, &hotspot_detect);
    httpd_register_uri_handler(s_httpd, &ncsi);
    httpd_register_uri_handler(s_httpd, &connecttest);
    httpd_register_uri_handler(s_httpd, &fwlink);
    httpd_register_uri_handler(s_httpd, &license);
    httpd_register_uri_handler(s_httpd, &set_brightness_uri);

    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, http_404_error_handler);

    ESP_LOGI(TAG, "HTTP server started");
  }
}

void enter_setup_mode(void)
{
  ESP_LOGW(TAG, "Entering setup portal mode");

  setup_mode = true;
  esp_wifi_disconnect();   // まず接続試行を止める
  esp_wifi_stop();
  vTaskDelay(pdMS_TO_TICKS(300));

  wifi_start_apsta_for_setup();
  start_web_server();

  xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 4, NULL);
}

bool is_wifi_connected(void)
{
  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group,
      WIFI_CONNECTED_BIT,
      pdFALSE,
      pdFALSE,
      pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS)
  );
  return (bits & WIFI_CONNECTED_BIT) != 0;
}

char *get_wifi_addr_str(void)
{
  return ip_str;
}

bool is_reboot_req(void)
{
  return reboot_req;
}

wifi_status_t wifi_get_status(void)
{
  return wifi_status;
}
