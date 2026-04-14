/*
 * JJY-SIM R3 - JJY PWM (API)
 *
 * Provides JJY signal generation interface.
 *
 * - Initializes PWM-based JJY output
 * - Generates and transmits time signal per second
 * - Provides timezone string utility
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 */
#ifndef JJY_PWM_H
#define JJY_PWM_H

#include <stdbool.h>
#include <stdint.h>

// Initialize JJY PWM generator
bool jjy_pwm_init(int band, int dst, float timezone);

// Process JJY signal output (call once per second)
char *jjy_pwm_proc(void);

// Convert timezone to TZ string
char *jjy_get_tz_str(char *tz, float timezone, int dst);

// Convert day of the week to string
char *jjy_get_week_str(char *buff);

// Set JJY signal output enable
void jjy_set_signal_enable(bool enable);

#endif
