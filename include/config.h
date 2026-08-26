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
