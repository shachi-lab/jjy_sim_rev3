/*
 * JJY-SIM R3 - Wi-Fi Management (API)
 *
 * Provides Wi-Fi connection control and status handling.
 *
 * - Initializes Wi-Fi subsystem
 * - Starts STA connection with given credentials
 * - Provides connection status and IP address
 * - Supports setup mode and reboot request flag
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 */
#ifndef _WIFI_MNG_H_
#define _WIFI_MNG_H_

typedef enum {
  WIFI_STATUS_UNKNOWN = -1,     // Status not initialized
  WIFI_STATUS_DISCONNECTED = 0, // Not connected
  WIFI_STATUS_CONNECTING,       // Connecting to AP
  WIFI_STATUS_CONNECTED         // Connected and IP acquired
} wifi_status_t;

// Initialize Wi-Fi subsystem (common setup)
void wifi_init_common(void);

// Start Wi-Fi in STA mode using SSID and password
void wifi_start_sta_only(const char *ssid, const char *pass);

// Enter Wi-Fi setup mode (e.g. AP/captive portal)
void enter_setup_mode(void);

// Returns true if connected to Wi-Fi (wait 1sec)
bool is_wifi_connected(void);

// Get current IP address as string
char *get_wifi_addr_str(void);

// Returns true if reboot is requested
bool is_reboot_req(void);

// Get current Wi-Fi connection status
wifi_status_t wifi_get_status(void);

#endif
