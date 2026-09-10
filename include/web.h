#pragma once

// ============================================================
//  ESP32 WinKeyer — settings web server
//
//  A small synchronous HTTP server on port 80, served from the
//  same mDNS name as the keyer link: http://winkeyer.local/
//
//  Every setting it writes goes through Settings::apply(), so the
//  web page, the serial CLI and NVS can never disagree about what
//  a setting means.
//
//  Visual style borrowed from soft-MORCONI (~/projects/Morconi),
//  which is a browser UI + Node bridge rather than an embedded
//  server — the look carried over, none of the code did.
// ============================================================

#include <Arduino.h>

namespace Web {

void begin();   // start the server; call after WiFi and Net::begin()
void poll();    // service one request; call from loop()

}  // namespace Web
