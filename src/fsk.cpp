// ============================================================
//  ESP32 WinKeyer — RTTY FSK keying line
//
//  Baudot/ITA2 on GPIO27. The shift state (LTRS vs FIGS) is the
//  part that bites: the code for "Q" and the code for "1" are the
//  same five bits, and which one prints depends on a shift
//  character sent earlier. Losing track prints digits as letters
//  for the rest of the over, so the shift is tracked explicitly
//  and re-sent whenever a character needs the other one.
// ============================================================

#include "fsk.h"
#include "pins.h"
#include "keyer.h"
#include "log.h"
#include <esp_timer.h>

namespace {

// ── ITA2 ──────────────────────────────────────────────────
// Five-bit codes, LSB sent first. Letters and figures share the
// codes; FIGS_TAB[i] is what LTRS_TAB[i]'s key prints when shifted.
const char LTRS_TAB[32] = {
  0,   'E', '\n','A', ' ', 'S', 'I', 'U',
  '\r','D', 'R', 'J', 'N', 'F', 'C', 'K',
  'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q',
  'O', 'B', 'G',  0,  'M', 'X', 'V',  0
};
const char FIGS_TAB[32] = {
  0,   '3', '\n','-', ' ', '\a','8', '7',
  '\r','$', '4', '\'',',', '!', ':', '(',
  '5', '"', ')', '2', '#', '6', '0', '1',
  '9', '?', '&',  0,  '.', '/', ';',  0
};

const uint8_t CODE_LTRS = 0x1F;   // 11111
const uint8_t CODE_FIGS = 0x1B;   // 11011

// Find a character's code, and whether it needs the figures shift.
bool encode(char c, uint8_t& code, bool& figs) {
  if (!c) return false;
  c = toupper((unsigned char)c);
  for (uint8_t i = 0; i < 32; i++) {
    if (LTRS_TAB[i] == c) { code = i; figs = false; return true; }
  }
  for (uint8_t i = 0; i < 32; i++) {
    if (FIGS_TAB[i] == c) { code = i; figs = true;  return true; }
  }
  return false;
}

// ── Line state ────────────────────────────────────────────
volatile bool cfgInvert = false;
volatile bool cfgDiddle = false;
float         cfgBaud   = 45.45f;

esp_timer_handle_t timer = nullptr;

// Character ring. Written by loop()/web, drained in the timer callback.
char     txBuf[256];
volatile uint16_t txHead = 0, txTail = 0;
inline uint16_t txCount() { return (uint16_t)(txTail - txHead); }

// Transmit state machine, stepped every HALF bit.
enum Phase : uint8_t { PH_IDLE, PH_START, PH_DATA, PH_STOP };
volatile Phase   phase     = PH_IDLE;
volatile uint8_t curCode   = 0;
volatile uint8_t bitIdx    = 0;
volatile uint8_t halfLeft  = 0;      // half-bits remaining in this phase
volatile bool    figsShift = false;  // current shift state of the FAR end
volatile uint8_t pendCode  = 0;      // shift char queued ahead of a character
volatile bool    havePend  = false;
volatile bool    pttUp     = false;

inline void mark()  { digitalWrite(PIN_FSK_OUT, cfgInvert ? LOW  : HIGH); }
inline void space() { digitalWrite(PIN_FSK_OUT, cfgInvert ? HIGH : LOW); }
inline void setBit(bool one) { one ? mark() : space(); }

void startChar(uint8_t code) {
  // RTTY holds PTT for the whole over with no CW elements behind it, so
  // without this the keyer's 10 s no-keying backstop would drop the line
  // mid-message.
  Keyer::pttKeepAlive();
  curCode  = code;
  phase    = PH_START;
  halfLeft = 2;          // start bit = one whole bit
  bitIdx   = 0;
  space();
}

// Pull the next thing to send: a queued shift character, then text.
bool nextChar() {
  if (havePend) { havePend = false; startChar(pendCode); return true; }

  while (txCount() > 0) {
    char c = txBuf[txHead++ % sizeof(txBuf)];
    uint8_t code; bool figs;
    if (!encode(c, code, figs)) continue;         // unprintable in Baudot
    // Space and CR/LF exist in both shifts, so they never force a change.
    bool neutral = (code == 4 || code == 8 || code == 2);
    if (!neutral && figs != figsShift) {
      figsShift = figs;
      pendCode  = code;                            // the character follows
      havePend  = true;
      startChar(figs ? CODE_FIGS : CODE_LTRS);
      return true;
    }
    startChar(code);
    return true;
  }

  if (cfgDiddle && pttUp) { startChar(CODE_LTRS); return true; }
  return false;
}

void onTick(void*) {
  if (halfLeft > 0 && --halfLeft > 0) return;    // still inside this bit

  switch (phase) {
    case PH_START:
      phase = PH_DATA; bitIdx = 0;
      setBit(curCode & 0x01);
      halfLeft = 2;
      break;

    case PH_DATA:
      if (++bitIdx < 5) {
        setBit((curCode >> bitIdx) & 0x01);
        halfLeft = 2;
      } else {
        phase = PH_STOP;
        mark();
        halfLeft = 3;        // 1.5 bits — the reason this runs on half-bits
      }
      break;

    case PH_STOP:
      if (!nextChar()) {
        phase = PH_IDLE;
        mark();
        if (pttUp) { Keyer::pttManual(false); pttUp = false; }
      }
      break;

    case PH_IDLE:
      mark();
      break;
  }
}

}  // namespace

namespace Fsk {

void begin() {
  pinMode(PIN_FSK_OUT, OUTPUT);
  mark();
  const esp_timer_create_args_t args = {
    .callback = &onTick, .arg = nullptr,
    .dispatch_method = ESP_TIMER_TASK, .name = "fsk", .skip_unhandled_events = false
  };
  esp_timer_create(&args, &timer);
  setBaud(cfgBaud);
  Log::printf("[FSK] line on GPIO%d, %.2f baud, mark=%s\n",
              PIN_FSK_OUT, cfgBaud, cfgInvert ? "low" : "high");
}

bool setBaud(float b) {
  if (b < 10.0f || b > 300.0f) return false;
  cfgBaud = b;
  uint64_t halfUs = (uint64_t)(500000.0 / b + 0.5);   // half a bit
  if (timer) {
    esp_timer_stop(timer);
    esp_timer_start_periodic(timer, halfUs);
  }
  return true;
}
float baud() { return cfgBaud; }

bool send(const char* text) {
  if (!text || !*text) return true;
  for (const char* p = text; *p; p++) {
    if (txCount() >= sizeof(txBuf) - 1) return false;
    txBuf[txTail++ % sizeof(txBuf)] = *p;
  }
  if (phase == PH_IDLE) {
    // RTTY holds the transmitter for the whole over, so PTT goes up here
    // and comes down when the queue empties — not per character.
    if (!pttUp) { Keyer::pttManual(true); pttUp = true; }
    figsShift = false;
    havePend  = false;
    pendCode  = 0;
    startChar(CODE_LTRS);        // put the far end in a known shift
  }
  return true;
}

void abort() {
  txHead = txTail = 0;
  havePend = false;
  phase = PH_IDLE;
  mark();
  if (pttUp) { Keyer::pttManual(false); pttUp = false; }
}

bool   busy()    { return phase != PH_IDLE || txCount() > 0; }
size_t pending() { return txCount(); }

void setInvert(bool on) { cfgInvert = on; if (phase == PH_IDLE) mark(); }
bool invert()           { return cfgInvert; }
void setDiddle(bool on) { cfgDiddle = on; }
bool diddle()           { return cfgDiddle; }

}  // namespace Fsk
