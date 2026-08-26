#pragma once

// ============================================================
//  ESP32 WinKeyer — keyer core API
//
//  Iambic keyer engine running as a 1 kHz FreeRTOS task pinned to
//  core 1 at high priority, so CW timing is independent of WiFi,
//  MQTT and the Arduino loop. Transport-agnostic: the serial CLI
//  today and the WK3 protocol engine later both drive this API.
// ============================================================

#include <Arduino.h>

enum KeyerMode : uint8_t { KEYER_IAMBIC_A = 0, KEYER_IAMBIC_B = 1 };

namespace Keyer {

void begin();   // GPIO + sidetone init, starts the keyer task. Call before WiFi.

// ── Settings ──────────────────────────────────────────────
void      setWpm(uint8_t wpm);          // clamped 5..60; overrides pot until pot moves
uint8_t   getWpm();
void      setMode(KeyerMode m);         // default IAMBIC_B
KeyerMode getMode();
void      setPaddleSwap(bool swapped);  // dit/dah exchange
bool      getPaddleSwap();
void      setSidetone(bool enabled);    // default on
void      setSidetoneHz(uint16_t hz);   // default 600
uint16_t  getSidetoneHz();
void      setPttEnabled(bool enabled);  // default on
void      setPttLeadMs(uint16_t ms);    // default 50
void      setPttTailMs(uint16_t ms);    // default 250
void      setPotEnabled(bool enabled);  // default OFF until a pot is wired (GPIO34 floats)
void      setPotRange(uint8_t minWpm, uint8_t range);  // default 10 + 25 → 10..35

// ── Sending ───────────────────────────────────────────────
bool sendChar(char c);   // queue ASCII (space = word gap); false if buffer full
void clearBuffer();      // abort buffered sending immediately (key up)
void tune(bool on);      // continuous key-down (with PTT)
bool tuning();
bool busy();             // element in progress or buffer non-empty
bool paddleBreakIn();    // true once if paddle press aborted a buffered send

}  // namespace Keyer
