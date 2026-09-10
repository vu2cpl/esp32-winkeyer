#pragma once

// ============================================================
//  ESP32 WinKeyer — config
//  Non-secret build config. Real credentials live in secrets.h
//  (git-ignored) — see secrets.h.example.
// ============================================================

// ── WiFi onboarding (WiFiManager captive portal) ──────────
// No compile-time SSID/password. First boot — or whenever the saved network is
// unreachable — the node opens AP vu2cpl-esp32-winkeyer-setup. Join it, pick your network,
// enter its password; creds persist in NVS.
#define WIFI_AP_NAME           "vu2cpl-esp32-winkeyer-setup"
#define WIFI_AP_PASS           "vu2cpl1234"
#define WIFI_PORTAL_TIMEOUT_S  180

// ── WinKeyer transport ────────────────────────────────────
// Raw WinKeyer byte stream over TCP; the host-side bridge in tools/
// maps it to a serial port. Advertised as _winkeyer._tcp over mDNS.
#define MDNS_HOSTNAME   "winkeyer"
#define WK_TCP_PORT     8088

// ── FlexRadio (SmartSDR) backend ──────────────────────────
// Command API is TCP 4992. Discovery broadcasts: VITA-49 on UDP 4991
// (firmware > v1.1.3), legacy proprietary format on UDP 4992.
#define FLEX_API_PORT            4992
#define FLEX_DISCOVERY_PORT_NEW  4991
#define FLEX_DISCOVERY_PORT_OLD  4992

// ── MQTT broker (shack) ───────────────────────────────────
// Broker requires auth (since 2026-08-21). Role account + password come from
// secrets.h — use the account scoped to this device (iot for sensors/Tasmota,
// svc for Pi/host publishers). Passwords are in the shack password manager.
#define MQTT_HOST       "192.168.1.10"
#define MQTT_PORT       1883
#define MQTT_CLIENT_ID  "esp32-winkeyer"

// ── Topics (shack/<service>/...) ──────────────────────────
#define T_STATUS   "shack/esp32-winkeyer/status"   // retained; LWT publishes {"event":"offline"}

#include "secrets.h"
