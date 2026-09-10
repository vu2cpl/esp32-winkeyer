#pragma once

// ============================================================
//  ESP32 WinKeyer — keyer core API
//
//  Iambic keyer engine running as a 1 kHz FreeRTOS task pinned to
//  core 1 at high priority, so CW timing is independent of WiFi,
//  MQTT and the Arduino loop. Transport-agnostic: the serial CLI
//  and the WK3 protocol engine both drive this API.
// ============================================================

#include <Arduino.h>

enum KeyerMode : uint8_t { KEYER_IAMBIC_A = 0, KEYER_IAMBIC_B = 1 };

// Queued alongside characters to suppress the inter-character gap after
// the next character — this is how WK's "merge letters" prosigns work.
static const char KEYER_MERGE_MARK = 0x01;

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
void      setKeyOutEnabled(bool en);    // false = sidetone only (Flex backend owns the rig)

// WK-style refinements. Nominal values reproduce standard timing.
void      setWeighting(uint8_t w);      // 10..90, nominal 50 (mark/space balance)
void      setRatio(uint8_t r);          // 33..66, nominal 50 (dah = 3 dits)
void      setFarnsworth(uint8_t wpm);   // 0 = off; else gaps stretched to this WPM

// ── Sending ───────────────────────────────────────────────
bool   sendChar(char c);   // queue ASCII (space = word gap); false if buffer full
size_t queueDepth();       // characters still queued in the keyer
void   clearBuffer();      // abort buffered sending immediately (key up)
void   tune(bool on);      // continuous key-down (with PTT)
bool   tuning();
void   pttManual(bool on); // host-forced PTT (WK 0x18), independent of send activity
bool   busy();             // element in progress or buffer non-empty
bool   keyIsDown();        // for WK status reporting
bool   paddleActive();     // either paddle currently closed
bool   paddleDit();        // debounced dit lever (after any swap)
bool   paddleDah();        // debounced dah lever (after any swap)
bool   paddleBreakIn();    // true once if paddle press aborted a buffered send

}  // namespace Keyer
