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
// 0 = the portal never times out. The portal is non-blocking here, so the
// keyer keeps keying while it is up and there is nothing to reclaim by
// closing it — whereas a timeout strands an un-onboarded board with no AP
// to join, which is exactly when you need it most.
#define WIFI_PORTAL_TIMEOUT_S  0

// ── Serial host link ──────────────────────────────────────
// A real K1EL WinKeyer runs its serial link at 1200 baud, 8 data bits, TWO
// stop bits, and loggers open the port that way without asking. RUMlogNG was
// observed doing exactly this (stty reported "speed 1200 baud; cs8 cstopb"),
// so the firmware must match or the handshake arrives as noise.
//
// The cost is that the console shares this port: at 1200 baud a chatty boot
// takes seconds, and the host cannot be answered until it finishes. So when
// the link is running at WinKeyer speed the boot log is trimmed to one line
// and the WiFiManager debug chatter is silenced — use the web page or
// /status for detail, both of which are unaffected.
#define WK_HOST_BAUD_DEFAULT  1200
#define WK_CONSOLE_BAUD       115200   // pick this with /baud for a readable log

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
