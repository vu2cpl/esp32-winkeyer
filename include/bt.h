#pragma once

// ============================================================
//  ESP32 WinKeyer — Bluetooth LE keyboard
//
//  Type CW on a BLE keyboard: letters, digits and punctuation go
//  out as they are typed, F1-F6 play the memories, Esc stops
//  everything, PgUp/PgDn (or Up/Down) change speed.
//
//  OFF by default, and switching it takes a restart. Two reasons:
//  - initArduino() frees the Bluetooth controller's memory at boot
//    unless something claims it, and claiming it costs ~90 KB of
//    heap whether a keyboard ever connects or not. The switch is
//    read from NVS at that moment, so "off" costs nothing.
//  - While Bluetooth runs, ESP-IDF requires WiFi modem sleep ON,
//    which main.cpp otherwise turns OFF to kill ~300 ms of packet
//    latency. Measured with BT up: ~85 ms average ping, spikes past
//    200 ms (HANDOVER, 2026-09-13). Whether paddle keying to the
//    Flex survives that is exactly what the switch lets you try.
//
//  BLE only: the precompiled core has no Classic HID host. The S3
//  core ships NimBLE rather than Bluedroid, so there this module
//  compiles to stubs that report "unsupported".
//
//  One keyboard is kept. Pairing another replaces it — keyboards
//  like the Amkette Optimus take a new address every time they
//  enter pairing mode, so without pruning the bond list fills up.
// ============================================================

#include <Arduino.h>
#include <ArduinoJson.h>

namespace Bt {

bool available();          // this build has a usable BLE HID host
void begin(bool enabled);  // call once from setup(), after Settings::begin()
void poll();               // call from loop(): key actions, reconnects

bool setting();            // the saved switch (applies at the next boot)
void setSetting(bool on);  // record a change; Settings owns persistence
bool active();             // the stack is running this boot

bool scanStart();          // 10 s scan for keyboards; false if not running
bool connect(const char* addr, uint8_t addrType);   // "aa:bb:cc:dd:ee:ff"
void forget();             // drop the paired keyboard and every bond

// full=false: the small block /api/state carries every second.
// full=true:  plus address, key count, heap and the scan list (/api/bt).
void toJson(JsonObject o, bool full);

}  // namespace Bt
