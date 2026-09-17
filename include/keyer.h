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
// Take the speed from the knob now, without waiting for it to move (WinKeyer
// "set speed 0"). Applied on the next pot reading, within 50 ms. No effect
// without a pot.
void      usePotSpeed();
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
// Dit units one character takes on the air, including the 3-unit gap after
// it: 'E' = 4, 'T' = 6. 0 if it has no Morse. (A space adds 4: 7 in all.)
uint8_t charUnits(char c);
// Extra microseconds per dit unit for the sidetone copy of text a network
// radio is generating, so it keeps pace with a radio that sends slow: 700
// plays each unit 0.7 ms longer than 1200/WPM. Paddle elements and the local
// backend are never changed.
void    setMonitorExtraUs(int16_t us);
int16_t monitorExtraUs();
size_t queueDepth();       // characters still queued in the keyer
void   clearBuffer();      // abort buffered sending immediately (key up)
// Practice: the keyer makes sidetone for paddle, text and tune, but drives no
// KEY or PTT line and sends nothing to a network radio. Switching either way
// stops whatever is being sent. Not saved: every boot is ready to transmit.
void   setPractice(bool on);
bool   practice();
void   tune(bool on);      // continuous key-down (with PTT)
bool   tuning();
void   pttManual(bool on); // host-forced PTT (WK 0x18), independent of send activity
bool   busy();             // element in progress or buffer non-empty
bool   keyIsDown();        // for WK status reporting
// Milliseconds since the key last went down, stamped at 1 kHz so no element
// is missed. Use this for an activity indicator: sampling keyIsDown() from
// a slower loop aliases against the element length and looks random.
uint32_t msSinceKey();
// Is the PTT line asserted right now? Unlike the key it is held for the
// whole over, so it shows the lead-in and the tail either side of the
// elements — which is what makes those two settings visible.
bool   pttIsOn();
// Tell the backstop the line is still doing something useful. Needed by
// anything that holds PTT without keying elements — RTTY, where a single
// over is far longer than the 10 s idle limit.
void   pttKeepAlive();
// True once if the safety backstop had to drop a PTT line that nothing
// else released. Worth logging: it means a transition was lost upstream.
bool   pttStuckWasCleared();

// Play one character on the SIDETONE only — no key line, no PTT, no key
// hook. Blocking, for a startup chirp: a boot that keys the transmitter
// every time it powers up would be worse than no indication at all.
void   chirp(char c);

// false = the keyer stops sequencing PTT and the caller owns the line via
// pttManual(). Set on network backends, where the radio generates the CW
// and the local keyer's copy is only a monitor.
void   setPttAuto(bool on);
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
// The operator is on the paddle: true from the first element until the paddle
// hang time (one word space + a dit) has passed. Drives WinKeyer BREAKIN,
// which is a level for the whole session, not a pulse.
bool   paddleSession();
// What opened the paddle session: 'd' dit, 'a' dah, 'e' paddle element.
char   paddleSessionCause();
// The speed pot's step above its minimum (0..range), or -1 when no pot is
// enabled. What a WinKeyer pot byte reports — the knob, not the speed.
int8_t potStep();

// Characters the OPERATOR sent on the paddle, decoded from the elements
// this keyer generated (so exact, not a signal decoder). Drives WinKeyer
// paddle echo. Returns false when nothing is waiting.
bool   decodedRead(char& c);
// Buffered characters in the order they FINISH being keyed (a space when a
// word gap ends). Drives WinKeyer serial echo on the local backend, which a
// real WinKeyer sends only after the letter is completely sent.
bool   sentRead(char& c);

}  // namespace Keyer
