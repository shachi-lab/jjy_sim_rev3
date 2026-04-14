/*
 * JJY-SIM R3 - JJY PWM Generator
 *
 * Generates JJY time signal (40kHz / 60kHz) using PWM (H-bridge).
 *
 * - Creates 1-minute JJY frame from system time
 * - Outputs JJY modulation synchronized to second boundary
 * - Uses ESP-IDF LEDC for PWM generation
 * - Uses esp_timer for precise pulse timing
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
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/ledc.h"

#include "jjy-sim-r3.h"
#include "led.h"
#include "jjy_pwm.h"

static const char *TAG __attribute__((unused)) = "jjy_pwm";

#define JJY_T_BIT0      800000        // 800ms
#define JJY_T_BIT1      500000        // 500ms
#define JJY_T_PM        200000        // 200ms

#define JJY_BIT_ZERO    0
#define JJY_BIT_OFF     -1
#define JJY_BIT_PMn     -2
#define JJY_BIT_M       -3

#define TICK_US         (1000000LL / configTICK_RATE_HZ)

#define PWM_BITS        LEDC_TIMER_9_BIT
#define PWM_CH_A        LEDC_CHANNEL_0
#define PWM_CH_B        LEDC_CHANNEL_1
#define PWM_TIMER       LEDC_TIMER_0
#define PWM_MODE        LEDC_LOW_SPEED_MODE

#define PWM_DUTY_MAX    ((1U << PWM_BITS) - 1)
#define PWM_DUTY_ON     (PWM_DUTY_MAX / 2)     // 必要に応じて調整
#define PWM_DEAD        0                      // 必要に応じて調整

static bool jjy_signal_enable = true;
static struct tm timeinfo;
static char timeNowStr[40];
static int jjy_frame[60];
static esp_timer_handle_t s_jjy_off_timer = NULL;

static void set_timezone(float timezone, int dst);
static void wait_for_next_second(void);
static void create_jjy_frame(struct tm *tm);
static void jjy_put_bit(int flag);
static void send_char_immediately(char c);

static esp_err_t pwm_setup(int freq);
static void pwm_on(void);
static void pwm_off(void);
static void pwm_stop(void);

// Initialize JJY PWM generator
bool jjy_pwm_init(int band,int dst, float timezone)
{
  ESP_LOGI(TAG, "JJY-SIM main start");

  set_timezone(timezone, dst);

  if (pwm_setup(band*1000) != ESP_OK) {
    ESP_LOGE(TAG, "PWM setup failed");   
    return false;
  }
  wait_for_next_second();
  return true;
}

// Process JJY signal output
char *jjy_pwm_proc(void)
{
  static int current_min = -1;
  time_t now;

  wait_for_next_second();

  time(&now);
  localtime_r(&now, &timeinfo);

  timeinfo.tm_year -= 100;
  sprintf(timeNowStr, "20%02d/%02d/%02d,%02d:%02d:%02d",
      timeinfo.tm_year, timeinfo.tm_mon+1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

  if (current_min != timeinfo.tm_min) {

    printf("\n%s : ", &timeNowStr[2]);
    for (int i = 0; i < timeinfo.tm_sec; i++) send_char_immediately(' ');

    create_jjy_frame(&timeinfo);
    current_min = timeinfo.tm_min;
  }
  jjy_put_bit(jjy_frame[timeinfo.tm_sec]);

  return timeNowStr;
}

// Set JJY signal enable
void jjy_set_signal_enable(bool enable)
{
  jjy_signal_enable = enable;
}

// Convert day of the week to string
char *jjy_get_week_str(char *buff)
{
  const char *week[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  strcpy(buff, week[timeinfo.tm_wday]);
  return buff;
}

// Convert timezone to TZ string
char *jjy_get_tz_str(char *tz, float timezone, int dst)
{
  if (timezone < -12.0 || timezone > 12.0) timezone = TIMEZONE_JAPAN;

  int hour = (int)timezone;
  int min  = (int)((timezone - hour) * 60 + 0.5);

  sprintf(tz, "UTC%+d:%02d", hour, abs(min));

  return tz;
}

// Set timezone
static void set_timezone(float timezone, int dst)
{
  char tz[32];

  // 符号反転（ここ重要）
  timezone = -timezone;
  jjy_get_tz_str(tz, timezone, dst);

  setenv("TZ", tz, 1);
  tzset();
}

// Wait for next second
static void wait_for_next_second(void)
{
  struct timeval tv;
  int last_sec = -1;
  const suseconds_t tick_us = TICK_US;

  while (1) {
    gettimeofday(&tv, NULL);

    if (last_sec == -1) {
      last_sec = tv.tv_sec;
    }

    // 秒が変わったら終了
    if (tv.tv_sec != last_sec) {
      break;
    }

    // 残り時間で待ち方を変える
    if (tv.tv_usec < tick_us) {
      // まだ余裕ある → 寝る
      vTaskDelay(1);
    } else {
      // 秒境界直前 → 詰める
      taskYIELD();
    }
  }
}

/*
 * intをBCDに変換
 */
static int get_int_to_bcd(int n)
{
  int res = n % 10;
  res += ((n/ 10)%10)<<4;
  res += ((n/100)%10)<<8;
  return res;
}

/*
 * 偶数パリティを計算
 */
static int get_even_parity(int n)
{
  n ^= n >> 8;
  n ^= n >> 4;
  n ^= n >> 2;
  n ^= n >> 1;
  return n & 1;
}

// Create JJY frame
static void create_jjy_frame(struct tm *tm)
{
  const int totalDaysOfMonth[] = {0,31,59,90,120,151,181,212,243,273,304,334 };

  int totalDays = totalDaysOfMonth[tm->tm_mon];
  totalDays += tm->tm_mday;
  if (((tm->tm_year & 0x03)==0) && (tm->tm_mon > 2)) totalDays++;

  int mm = get_int_to_bcd(tm->tm_min);
  int hh = get_int_to_bcd(tm->tm_hour);
  int dd = get_int_to_bcd(totalDays);
  int yy = get_int_to_bcd(tm->tm_year % 100);
  int pa1= get_even_parity(hh);
  int pa2= get_even_parity(mm);
  int ww = tm->tm_wday;

  jjy_frame[ 0] =   JJY_BIT_M;     // :00 M
  jjy_frame[ 1] =   mm & 0x40;     // :01
  jjy_frame[ 2] =   mm & 0x20;     // :02
  jjy_frame[ 3] =   mm & 0x10;     // :03
  jjy_frame[ 4] =   0;             // :04
  jjy_frame[ 5] =   mm & 0x08;     // :05
  jjy_frame[ 6] =   mm & 0x04;     // :06
  jjy_frame[ 7] =   mm & 0x02;     // :07
  jjy_frame[ 8] =   mm & 0x01;     // :08
  jjy_frame[ 9] =   JJY_BIT_PMn;   // :09 P1
  jjy_frame[10] =   0;             // :10
  jjy_frame[11] =   0;             // :11
  jjy_frame[12] =   hh & 0x20;     // :12
  jjy_frame[13] =   hh & 0x10;     // :13
  jjy_frame[14] =   0;             // :14
  jjy_frame[15] =   hh & 0x08;     // :15
  jjy_frame[16] =   hh & 0x04;     // :16
  jjy_frame[17] =   hh & 0x02;     // :17
  jjy_frame[18] =   hh & 0x01;     // :18
  jjy_frame[19] =   JJY_BIT_PMn;   // :19 P2
  jjy_frame[20] =   0;             // :20
  jjy_frame[21] =   0;             // :21
  jjy_frame[22] =   dd & 0x200;    // :22
  jjy_frame[23] =   dd & 0x100;    // :23
  jjy_frame[24] =   0;             // :24
  jjy_frame[25] =   dd & 0x80;     // :25
  jjy_frame[26] =   dd & 0x40;     // :26
  jjy_frame[27] =   dd & 0x20;     // :27
  jjy_frame[28] =   dd & 0x10;     // :28
  jjy_frame[29] =   JJY_BIT_PMn;   // :29 P3
  jjy_frame[30] =   dd & 0x08;     // :30
  jjy_frame[31] =   dd & 0x04;     // :31
  jjy_frame[32] =   dd & 0x02;     // :32
  jjy_frame[33] =   dd & 0x01;     // :33
  jjy_frame[34] =   0;             // :34
  jjy_frame[35] =   0;             // :35
  jjy_frame[36] =   pa1;           // :36 PA1
  jjy_frame[37] =   pa2;           // :37 PA2
  jjy_frame[38] =   0;             // :38 SU1
  jjy_frame[39] =   JJY_BIT_PMn;   // :39 P4
  jjy_frame[40] =   0;             // :40 SU2
  jjy_frame[41] =   yy & 0x80;     // :41
  jjy_frame[42] =   yy & 0x40;     // :42
  jjy_frame[43] =   yy & 0x20;     // :43
  jjy_frame[44] =   yy & 0x10;     // :44
  jjy_frame[45] =   yy & 0x08;     // :45
  jjy_frame[46] =   yy & 0x04;     // :46
  jjy_frame[47] =   yy & 0x02;     // :47
  jjy_frame[48] =   yy & 0x01;     // :48
  jjy_frame[49] =   JJY_BIT_PMn;   // :49 P5
  jjy_frame[50] =   ww & 0x04;     // :50
  jjy_frame[51] =   ww & 0x02;     // :51
  jjy_frame[52] =   ww & 0x01;     // :52
  jjy_frame[53] =   0;             // :53 LS1
  jjy_frame[54] =   0;             // :54 LS2
  jjy_frame[55] =   0;             // :55
  jjy_frame[56] =   0;             // :56
  jjy_frame[57] =   0;             // :57
  jjy_frame[58] =   0;             // :58
  jjy_frame[59] =   JJY_BIT_PMn;   // :59 P0
}

// JJYの出力を止める
static void jjy_off_timer_cb(void *arg)
{
    pwm_off();
}

// JJYの出力を止める
static void jjy_off_init_once(void)
{
  if (s_jjy_off_timer != NULL)    return;

  esp_timer_create_args_t timer_args = {
    .callback = jjy_off_timer_cb,
    .arg = NULL,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "jjy_off"
  };

  esp_timer_create(&timer_args, &s_jjy_off_timer);
}

// JJYの出力
static void jjy_on_us(uint32_t on_time_us)
{
  if (jjy_signal_enable == false) return;
  pwm_on();
  jjy_off_init_once();
  esp_timer_stop(s_jjy_off_timer);  // 動いてたら止める
  esp_timer_start_once(s_jjy_off_timer, on_time_us);
}

/*
 * JJYの1ビットを出力 
 */
static void jjy_put_bit(int flag)
{
  char ch;

  switch (flag) {
  case JJY_BIT_OFF:
    ch = '.';
    break;

  case JJY_BIT_M:
  case JJY_BIT_PMn:
    jjy_on_us(JJY_T_PM);
    ch = '-';
    break;

  case JJY_BIT_ZERO:
    jjy_on_us(JJY_T_BIT0);
    ch = '0';
    break;

  default:
    pwm_on();
    jjy_on_us(JJY_T_BIT1);
    ch = '1';
    break;
  } 
  led_mode_t mode = (flag == JJY_BIT_M) ? LED_MODE_PULSE_LONG : LED_MODE_PULSE_SHORT;
  led_blink(PIN_LED_PPS, mode);
  send_char_immediately(ch);
}

#ifdef CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED

#include "driver/usb_serial_jtag.h"
#include "hal/usb_serial_jtag_ll.h"
// 1文字送った後に、ハードウェアを直接突っつく
static void send_char_immediately(char c)
{
    // 標準の書き込み（これでハードウェアFIFOに1バイト入る）
    fwrite(&c, 1, 1, stdout);
    fflush(stdout);
    // ハードウェアFIFOに「パケットをフラッシュしろ」と命令を出す
    // これが物理レベルでの強制送出命令だよ
    usb_serial_jtag_ll_txfifo_flush();
}
#else
static void send_char_immediately(char c)
{
  fwrite(&c, 1, 1, stdout);
  fflush(stdout);
}
#endif

/*
 * PWM(Hブリッジ)の初期設定
 */
static esp_err_t pwm_setup(int freq)
{
  esp_err_t err;

  ledc_timer_config_t timer_conf = {
    .speed_mode      = PWM_MODE,
    .duty_resolution = PWM_BITS,
    .timer_num       = PWM_TIMER,
    .freq_hz         = freq,
    .clk_cfg         = LEDC_USE_XTAL_CLK,
  };
  err = ledc_timer_config(&timer_conf);
  if (err != ESP_OK) {
    return err;
  }

  ledc_channel_config_t ch_a = {
    .gpio_num   = PIN_PWM_A,
    .speed_mode = PWM_MODE,
    .channel    = PWM_CH_A,
    .intr_type  = LEDC_INTR_DISABLE,
    .timer_sel  = PWM_TIMER,
    .duty       = 0,
    .hpoint     = 0,
    .flags.output_invert = 0,
  };
  err = ledc_channel_config(&ch_a);
  if (err != ESP_OK) {
    return err;
  }

  ledc_channel_config_t ch_b = {
    .gpio_num   = PIN_PWM_B,
    .speed_mode = PWM_MODE,
    .channel    = PWM_CH_B,
    .intr_type  = LEDC_INTR_DISABLE,
    .timer_sel  = PWM_TIMER,
    .duty       = 0,
    .hpoint     = 0,
    .flags.output_invert = 0,
  };
  err = ledc_channel_config(&ch_b);
  if (err != ESP_OK) {
    return err;
  }

  pwm_stop();

  return ESP_OK;
}

/*
 * PWM出力
 */
static void pwm_on(void)
{
  uint32_t duty_a   = PWM_DUTY_ON - PWM_DEAD;
  uint32_t hpoint_a = PWM_DEAD;

  uint32_t duty_b   = PWM_DUTY_ON - PWM_DEAD;
  uint32_t hpoint_b = PWM_DUTY_ON + PWM_DEAD;

  ledc_set_duty_with_hpoint(PWM_MODE, PWM_CH_A, duty_a, hpoint_a);
  ledc_set_duty_with_hpoint(PWM_MODE, PWM_CH_B, duty_b, hpoint_b);

  ledc_update_duty(PWM_MODE, PWM_CH_A);
  ledc_update_duty(PWM_MODE, PWM_CH_B);
}

/*
 * 出力をブレーキするとき（Hブリッジブレーキ）
 * 両側Hiにしたいなら duty=MAX
 */
static void pwm_off(void)
{
  ledc_set_duty(PWM_MODE, PWM_CH_A, PWM_DUTY_MAX);
  ledc_set_duty(PWM_MODE, PWM_CH_B, PWM_DUTY_MAX);

  ledc_update_duty(PWM_MODE, PWM_CH_A);
  ledc_update_duty(PWM_MODE, PWM_CH_B);
}

/*
 * 出力を止めるとき（Hブリッジ休止）
 * 0固定にする
 */
static void pwm_stop(void)
{
  ledc_set_duty(PWM_MODE, PWM_CH_A, 0);
  ledc_set_duty(PWM_MODE, PWM_CH_B, 0);

  ledc_update_duty(PWM_MODE, PWM_CH_A);
  ledc_update_duty(PWM_MODE, PWM_CH_B);
}
