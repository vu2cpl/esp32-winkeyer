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
bool      getSidetone();
void      setSidetoneHz(uint16_t hz);   // default 600
uint16_t  getSidetoneHz();
void      setPttEnabled(bool enabled);  // default on
bool      getPttEnabled();
void      setPttLeadMs(uint16_t ms);    // default 50
uint16_t  getPttLeadMs();
void      setPttTailMs(uint16_t ms);    // default 250
uint16_t  getPttTailMs();
void      setPotEnabled(bool enabled);  // default OFF until a pot is wired (GPIO34 floats)
bool      getPotEnabled();
void      setPotRange(uint8_t minWpm, uint8_t range);  // default 10 + 25 → 10..35
uint8_t   getPotMin();
uint8_t   getPotRange();
void      setKeyOutEnabled(bool en);    // false = sidetone only (Flex backend owns the rig)

// Which radio the KEY/PTT lines drive: 1 = radio 1 (GPIO33/32), 2 = radio 2
// (GPIO18/19), 3 = both. Both is deliberate — a rig plus an amp or monitor —
// but it can key two transmitters at once, so it is never the default.
void      setRadio(uint8_t sel);
uint8_t   getRadio();

// WK-style refinements. Nominal values reproduce standard timing.
void      setWeighting(uint8_t w);      // 10..90, nominal 50 (mark/space balance)
uint8_t   getWeighting();
void      setRatio(uint8_t r);          // 33..66, nominal 50 (dah = 3 dits)
uint8_t   getRatio();
void      setFarnsworth(uint8_t wpm);   // 0 = off; else gaps stretched to this WPM
uint8_t   getFarnsworth();

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

// Called from the keyer task on every key transition, so a network backend
// can mirror the element timing to a radio. Must not block: it runs inside
// the 1 kHz task. Pass nullptr to detach.
void setKeyEventHook(void (*fn)(bool down));

// Restrict the key hook to PADDLE-generated elements. On a network backend
// the radio generates buffered text itself, so buffered text may be run
// through this keyer purely to make sidetone — but its key events must not
// reach the hook, or the radio would be keyed twice for the same text.
void setHookPaddleOnly(bool on);
bool   paddleBreakIn();    // true once if paddle press aborted a buffered send

// Characters the OPERATOR sent on the paddle, decoded from the elements
// this keyer generated (so exact, not a signal decoder). Drives WinKeyer
// paddle echo. Returns false when nothing is waiting.
bool   decodedRead(char& c);

}  // namespace Keyer
