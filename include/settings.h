#pragma once

// ============================================================
//  ESP32 WinKeyer — persisted operator settings
//
//  One place that owns validation and NVS, so the serial CLI and
//  the web page cannot drift apart on what a setting is called,
//  what range it accepts, or whether it survives a power cycle.
//
//  The rule on persistence: the OPERATOR's panel settings stick,
//  the HOST's session settings do not. A pot enabled at the bench
//  is still enabled tomorrow; a speed N1MM sets for one contest is
//  gone at the next boot. So Settings::apply() persists, while the
//  WK protocol engine calls the Keyer API directly and does not.
// ============================================================

#include <Arduino.h>
#include <ArduinoJson.h>

namespace Settings {

void begin();   // restore everything from NVS. Call after the modules' begin().

// The serial link's baud rate, read straight from NVS. Needed before
// begin() because Serial.begin() has to happen first thing in setup() —
// a logger that opens the port at WinKeyer speed is already talking.
uint32_t hostBaud();
bool     quietBoot();   // true when the link is too slow for a chatty boot
bool     displayEnabled();   // needed before Display::begin()
uint32_t txPower();          // WiFi transmit power in dBm

// Why the board last restarted, as a short string. Captured once at boot
// and served from RAM: the reason is printed to serial exactly once, and
// that port is usually held by a logger, so a crash during real operation
// was otherwise undiagnosable.
void        setResetReason(const char* why);
const char* resetReason();

// Apply one setting by name and persist it. `msg` receives a human-readable
// result (an error explains the accepted range). Returns false if the key is
// unknown or the value is out of range — nothing is changed in that case.
bool apply(const char* key, const char* val, char* msg, size_t msgLen);

void toJson(JsonDocument& doc);   // full current state, for the web UI

// Put the operator's persisted keyer settings back. A WinKeyer host may
// override speed, mode, weighting, PTT lead/tail and so on for the length
// of its session — that is the protocol working as intended — but those
// values must not outlive the session, or a logger that sets lead/tail to
// zero leaves the keyer that way until the next reboot.
void restoreKeyer();

// Backend selection has three coupled side effects (which engine keys, whether
// the local key line is live, where the keyer's key events are routed), so it
// lives here rather than being repeated by every caller.
void applyBackend(bool useFlex, bool persist);
bool loadBackend();

}  // namespace Settings
