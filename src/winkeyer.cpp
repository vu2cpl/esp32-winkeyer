// ============================================================
//  ESP32 WinKeyer — K1EL WinKeyer protocol engine
//
//  Implements the WK2-compatible host command set that logging
//  software actually exercises (N1MM+, DXLog, RUMlogNG,
//  MacLoggerDX, SkookumLogger, fldigi): host open/close, speed,
//  weighting, PTT lead/tail, mode register, buffered text with
//  prosign merge and buffered speed change, immediate keying,
//  backspace, clear, plus unsolicited status and pot reports.
//
//  Commands outside that set are parsed and their parameters
//  consumed correctly, so an unknown command can never desync
//  the byte stream — it is simply ignored.
//
//  Protocol by Steve K1EL. Implementation original; K3ng keyer
//  (Anthony Good K3NG) consulted for host-mode behaviour.
// ============================================================

#include "winkeyer.h"
#include "keyer.h"
#include "flex.h"
#include "settings.h"

namespace {

// ── Version reported to the host ──────────────────────────
// 23 = WinKeyer 2.3. Every logger supports WK2; reporting WK3
// buys nothing here and narrows compatibility.
static const uint8_t WK_VERSION = 23;

// ── Status byte (0xC0 | flags) ────────────────────────────
static const uint8_t ST_BASE    = 0xC0;
static const uint8_t ST_XOFF    = 0x02;
static const uint8_t ST_BREAKIN = 0x04;
static const uint8_t ST_BUSY    = 0x08;
static const uint8_t ST_KEYDOWN = 0x10;
static const uint8_t ST_WAIT    = 0x20;

// ── Parameter counts ──────────────────────────────────────
// Index = immediate command byte. Keeps the parser in sync even
// for commands we do not act on.
static const uint8_t IMM_PARAMS[32] = {
  0,   // 0x00 admin — handled separately
  1,   // 0x01 sidetone frequency
  1,   // 0x02 set WPM
  1,   // 0x03 set weighting
  2,   // 0x04 PTT lead/tail
  3,   // 0x05 speed pot setup
  1,   // 0x06 pause
  0,   // 0x07 get speed pot
  0,   // 0x08 backspace
  1,   // 0x09 pin configuration
  0,   // 0x0A clear buffer
  1,   // 0x0B key immediate
  1,   // 0x0C HSCW speed
  1,   // 0x0D Farnsworth WPM
  1,   // 0x0E set mode register
  15,  // 0x0F load defaults
  1,   // 0x10 first extension
  1,   // 0x11 key compensation
  1,   // 0x12 paddle switchpoint
  0,   // 0x13 null
  1,   // 0x14 software paddle
  0,   // 0x15 request status
  1,   // 0x16 pointer command
  1,   // 0x17 dit/dah ratio
  1,   // 0x18 PTT on/off
  1,   // 0x19 key buffered (seconds)
  1,   // 0x1A wait
  2,   // 0x1B merge letters (prosign)
  1,   // 0x1C buffered speed change
  1,   // 0x1D buffered HSCW
  0,   // 0x1E cancel buffered speed
  0,   // 0x1F buffered NOP
};

static uint8_t adminParams(uint8_t sub) {
  switch (sub) {
    case 0x04: return 1;   // echo test
    case 0x0D: return 15;  // load EEPROM
    case 0x0E: return 1;   // send stored message
    case 0x0F: return 1;   // load X1MODE
    case 0x14: return 1;   // sidetone volume
    default:   return 0;
  }
}

// ── Send buffer ───────────────────────────────────────────
// Byte FIFO holding text plus in-band escapes, so a buffered
// speed change or prosign takes effect at the right point in
// the stream rather than immediately on arrival.
static const uint8_t ESC_SPEED = 0xFE;   // followed by WPM
static const uint8_t ESC_MERGE = 0xFD;   // merge the next two characters

static const size_t BUF_SIZE = 512;
uint8_t  buf[BUF_SIZE];
size_t   bufHead = 0, bufTail = 0;

size_t bufCount() { return (bufHead + BUF_SIZE - bufTail) % BUF_SIZE; }
bool   bufEmpty() { return bufHead == bufTail; }

bool bufPush(uint8_t b) {
  size_t next = (bufHead + 1) % BUF_SIZE;
  if (next == bufTail) return false;      // full
  buf[bufHead] = b;
  bufHead = next;
  return true;
}
bool bufPeek(uint8_t& b) {
  if (bufEmpty()) return false;
  b = buf[bufTail];
  return true;
}
void bufDrop()  { if (!bufEmpty()) bufTail = (bufTail + 1) % BUF_SIZE; }
void bufReset() { bufHead = bufTail = 0; }

// Remove the most recently queued character (WK backspace).
void bufBackspace() {
  if (bufEmpty()) return;
  bufHead = (bufHead + BUF_SIZE - 1) % BUF_SIZE;
}

// ── Engine state ──────────────────────────────────────────
WinKeyer::WriteFn sink = nullptr;
bool     hostIsOpen = false;
uint8_t  lastStatus = 0;
uint8_t  lastPot    = 0xFF;
bool     paused     = false;
bool     serialEcho = false;
// Paddle echo: mode register bit 6. RUMlogNG sets 0x07 — it asks for
// character echo but not this one — so an operator override is offered
// rather than leaving hand-sent text uncapturable. 0 off, 1 on, 2 follow
// the host's mode register.
bool     paddleEchoBit = false;
uint8_t  paddleEchoCfg = 2;
// Monitor buffered text locally on a network backend. The radio generates
// the actual CW, so the operator otherwise hears nothing at all while the
// rig is transmitting — the keyer is silent because it is not the thing
// keying. Running the same text through the local keyer gives sidetone at
// the same WPM; its key events are withheld from the hook by
// Keyer::setHookPaddleOnly() so the radio is not keyed twice.
bool     monitorLocal = true;

// Characters handed to the radio but not yet echoed to the host.
//
// WinKeyer echo exists so a logger can highlight the character being SENT.
// On the Flex backend the whole buffer is handed over in one batch, so
// echoing at queue time dumps the entire message instantly: the host's idea
// of progress runs ahead of the air, and after the first message its
// highlight is desynchronised and later echoes are discarded. The radio
// reports real progress with "cwx sent=", so pace the echo against that.
char     echoQ[256];
uint16_t echoHead = 0, echoTail = 0;
inline uint16_t echoCount() { return (uint16_t)(echoTail - echoHead); }
inline void echoPush(char c) {
  if (echoCount() < sizeof(echoQ)) echoQ[echoTail++ % sizeof(echoQ)] = c;
}
inline void echoReset() { echoHead = echoTail = 0; }

// The RADIO generates the CW, so it starts a few hundred ms after we hand
// the text over — network, then its own CW start — while a local monitor
// copy starts at once. The sidetone therefore runs AHEAD of the air, which
// Manoj hears as an echo that finishes early. Hold the copy by an operator-
// set delay so the two line up. 0 = straight through, as before.
// 0 = straight through, MON_DELAY_AUTO = follow the measured radio latency,
// anything else = that many milliseconds.
static const uint16_t MON_DELAY_AUTO = 0xFFFF;
uint16_t cfgMonDelayMs = MON_DELAY_AUTO;
inline uint16_t monDelayNow() {
  if (cfgMonDelayMs == MON_DELAY_AUTO) return Flex::startLatencyMs();
  return cfgMonDelayMs;
}
struct MonChar { char c; uint32_t due; };
MonChar  monQ[128];
uint16_t monHead = 0, monTail = 0;
inline uint16_t monCount() { return (uint16_t)(monTail - monHead); }
inline void monPush(char c) {
  const uint16_t d = monDelayNow();
  if (!d) { Keyer::sendChar(c); return; }
  if (monCount() >= sizeof(monQ) / sizeof(monQ[0])) return;   // sidetone only
  monQ[monTail % (sizeof(monQ) / sizeof(monQ[0]))] = { c, millis() + d };
  monTail++;
}
inline void monReset() { monHead = monTail = 0; }
void monPump() {
  const uint16_t N = sizeof(monQ) / sizeof(monQ[0]);
  while (monCount() && (int32_t)(millis() - monQ[monHead % N].due) >= 0)
    Keyer::sendChar(monQ[monHead++ % N].c);
}

uint8_t  modeReg    = 0x00;
WkBackend backend   = WK_BACKEND_LOCAL;

// Command parsing
uint8_t  pendingCmd    = 0;
uint8_t  pendingSub    = 0;
uint8_t  paramsNeeded  = 0;
uint8_t  paramBuf[16];
uint8_t  paramCount    = 0;
bool     inAdmin       = false;
bool     awaitingSub   = false;

// Settings mirrored so admin "get values" can answer.
uint8_t  cfgSpeed = 20, cfgWeight = 50, cfgLead = 5, cfgTail = 25;
uint8_t  cfgRatio = 50, cfgComp = 0, cfgFirstExt = 0, cfgSwitch = 50;
uint8_t  cfgPotMin = 10, cfgPotRange = 25, cfgFarns = 0;

// Text accumulated for the Flex backend before being flushed.
char     flexOut[64];
uint8_t  flexLen = 0;

void emit(uint8_t b) { if (sink) sink(&b, 1); }

void emitStatus(bool force) {
  if (!hostIsOpen) return;
  uint8_t s = ST_BASE;
  bool busy = (backend == WK_BACKEND_FLEX)
                ? (Flex::pending() > 0 || !bufEmpty() || flexLen > 0)
                : (Keyer::busy() || !bufEmpty());
  if (busy)                 s |= ST_BUSY;
  if (Keyer::keyIsDown())   s |= ST_KEYDOWN;
  if (paused)               s |= ST_WAIT;
  if (bufCount() > BUF_SIZE - 32) s |= ST_XOFF;
  if (Keyer::paddleBreakIn())     s |= ST_BREAKIN;
  if (force || s != lastStatus) {
    lastStatus = s;
    emit(s);
  }
}

void emitPot(bool force) {
  if (!hostIsOpen) return;
  uint8_t span = cfgPotRange ? cfgPotRange : 1;
  int v = ((int)Keyer::getWpm() - (int)cfgPotMin) * 31 / span;
  uint8_t pot = (uint8_t)constrain(v, 0, 31);
  if (force || pot != lastPot) {
    lastPot = pot;
    emit(0x80 | pot);
  }
}

void applyModeRegister(uint8_t m) {
  modeReg = m;
  // Bits 5:4 are the keyer mode (00 iambic B, 01 A, 10 ultimatic, 11 bug)
  // and bit 3 swaps the paddle. Both are RECORDED, NOT APPLIED: that is the
  // paddle in the operator's hand, and a swap set the wrong way round makes
  // it unusable mid-contest. modeReg still answers the host honestly.
  // The echo bits below ARE the host's — they are protocol behaviour the
  // logger needs, not anything to do with the fist.
  serialEcho    = (m & 0x04) != 0;
  paddleEchoBit = (m & 0x40) != 0;   // bit 6 — echo hand-sent characters
}

void resetToDefaults() {
  bufReset();
  echoReset();
  flexLen = 0;
  paused = false;
  Keyer::clearBuffer();
  Keyer::pttManual(false);
  Keyer::tune(false);
}

// ── Command execution ─────────────────────────────────────
void execAdmin(uint8_t sub, const uint8_t* p, uint8_t n) {
  switch (sub) {
    case 0x01:                        // reset
      resetToDefaults();
      hostIsOpen = false;
      break;
    case 0x02:                        // host open
      hostIsOpen = true;
      resetToDefaults();
      emit(WK_VERSION);
      lastStatus = 0; lastPot = 0xFF;
      emitStatus(true);
      break;
    case 0x03:                        // host close
      resetToDefaults();
      hostIsOpen = false;
      Settings::restoreKeyer();       // the host's overrides end with it
      break;
    case 0x04:                        // echo test
      if (n >= 1) emit(p[0]);
      break;
    case 0x05: emit(0); break;        // paddle A2D — no hardware
    case 0x06: emit(0); break;        // speed A2D — no hardware
    case 0x07: {                      // get values (15 bytes)
      uint8_t v[15] = {
        modeReg, cfgSpeed, cfgSwitch, cfgPotMin, cfgPotRange,
        cfgFarns, cfgWeight, cfgLead, cfgTail, 0 /* sample */,
        cfgRatio, cfgComp, cfgFirstExt, 0, 0
      };
      if (sink) sink(v, sizeof(v));
      break;
    }
    case 0x09: emit(0); break;        // get calibration
    case 0x0A: case 0x0B: case 0x13:  // WK1/WK2/WK3 mode select
      break;
    case 0x0C: {                      // dump EEPROM — 256 zero bytes
      uint8_t z[16] = {0};
      for (int i = 0; i < 16 && sink; i++) sink(z, sizeof(z));
      break;
    }
    default:
      break;                          // parsed, parameters consumed, ignored
  }
}

void execImmediate(uint8_t cmd, const uint8_t* p, uint8_t n) {
  switch (cmd) {
    case 0x01:                        // sidetone control — PARSED, NOT APPLIED
      // K1EL: the low nibble is N, tone = 4000/N; bit 7 means paddle-only.
      // RUMlogNG sends N=4 on every session open, which is a perfectly legal
      // 1000 Hz — and it overrode the operator's 600 Hz every time a logger
      // connected. The sidetone is the only setting here that nobody but the
      // operator hears: it is not timing, it does not reach the air, and the
      // logger never reads it back. So the byte is consumed to keep the
      // stream in sync and the operator's pitch is left alone.
      break;
    case 0x02:                        // set speed
      if (n && p[0]) { cfgSpeed = p[0]; Keyer::setWpm(p[0]); }
      break;
    case 0x03:                        // weighting — RECORDED, NOT APPLIED
      if (n) cfgWeight = p[0];        // fist, not protocol: see 0x0D
      break;
    case 0x04:                        // PTT lead / tail — RECORDED, NOT APPLIED
      // RUMlogNG sends 0,0 on every session open, which turns off both the
      // lead-in and the tail on a station where the KEYER is what sequences
      // PTT. Lead and tail belong to the rig and the amp in front of it, not
      // to whichever logger happens to be attached, so the values are kept
      // for the status dump and the keyer goes on using the operator's.
      // To hand PTT timing back to the host, restore the three setters here
      // (Keyer lead, Keyer tail, Flex tail — both tails, or the host moves
      // the local line while the radio keeps its own).
      if (n >= 2) { cfgLead = p[0]; cfgTail = p[1]; }
      break;
    case 0x05:                        // speed pot range
      if (n >= 2) {
        cfgPotMin = p[0]; cfgPotRange = p[1];
        Keyer::setPotRange(p[0], p[1]);
      }
      break;
    case 0x06:                        // pause / resume
      paused = (n && p[0]);
      break;
    case 0x07:                        // host asked for the pot value
      emitPot(true);
      break;
    case 0x08:                        // backspace
      bufBackspace();
      break;
    case 0x09:                        // pin configuration
      // Only bit 0 (PTT enable) is acted on — the remaining bits differ
      // between WK revisions and getting them wrong would silently
      // disable the operator's sidetone or key output.
      // Deliberately NOT in the operator-owned set of 0x0D: Manoj's call,
      // 2026-09-12 — "ptt and key, let it be settable from rumlog". A logger
      // turning the PTT line off for its session is a thing a logger is
      // entitled to do; it is restored from NVS when the session closes.
      if (n) Keyer::setPttEnabled((p[0] & 0x01) != 0);
      break;
    case 0x0A:                        // clear buffer
      bufReset();
      flexLen = 0;
      Keyer::clearBuffer();
      if (backend == WK_BACKEND_FLEX) { Flex::clear(); Keyer::clearBuffer(); echoReset(); monReset(); }
      break;
    case 0x0B:                        // key immediate
      if (n) Keyer::tune(p[0] != 0);
      break;
    case 0x0D:                        // Farnsworth — RECORDED, NOT APPLIED
      // RUMlogNG sets Farnsworth 20 on every session, which stretches the
      // spacing of everything sent and, with the monitor copy beside it,
      // sounded here like the radio unkeying early. Sending style is the
      // operator's; the byte is kept so the status dump stays honest.
      // Same for weighting (0x03), dit/dah ratio (0x17) and the keyer mode
      // and paddle swap in the mode register. Speed and the echo bits are
      // still the host's — a logger has to be able to drive those.
      // To hand any of them back, restore the setter next to the record.
      if (n) cfgFarns = p[0];
      break;
    case 0x0E:                        // mode register
      if (n) applyModeRegister(p[0]);
      break;
    case 0x0F:                        // load defaults (15 bytes)
      if (n >= 15) {
        applyModeRegister(p[0]);
        cfgSpeed = p[1]; Keyer::setWpm(p[1]);
        cfgSwitch = p[2];
        cfgPotMin = p[3]; cfgPotRange = p[4];
        Keyer::setPotRange(p[3], p[4]);
        // Speed and the pot range above are the host's; everything from
        // here down is the operator's and is recorded only — see 0x0D.
        cfgFarns = p[5];
        cfgWeight = p[6];
        cfgLead = p[7];
        cfgTail = p[8];
        cfgRatio = p[10];
        cfgComp = p[11];
        cfgFirstExt = p[12];
      }
      break;
    case 0x10: if (n) cfgFirstExt = p[0]; break;
    case 0x11: if (n) cfgComp = p[0];     break;
    case 0x12: if (n) cfgSwitch = p[0];   break;
    case 0x14:                        // software paddle
      break;
    case 0x15:                        // request status
      emitStatus(true);
      break;
    case 0x17:                        // dit/dah ratio — RECORDED, NOT APPLIED
      if (n) cfgRatio = p[0];         // fist, not protocol: see 0x0D
      break;
    case 0x18:                        // PTT on / off
      if (n) Keyer::pttManual(p[0] != 0);
      break;
    case 0x1B:                        // merge letters into a prosign
      if (n >= 2) { bufPush(ESC_MERGE); bufPush(p[0]); bufPush(p[1]); }
      break;
    case 0x1C:                        // buffered speed change
      if (n) { bufPush(ESC_SPEED); bufPush(p[0]); }
      break;
    case 0x1E:                        // cancel buffered speed
      bufPush(ESC_SPEED); bufPush(cfgSpeed);
      break;
    default:
      break;                          // consumed and ignored
  }
}

// ── Buffer pump ───────────────────────────────────────────
void flushFlex() {
  if (flexLen == 0) return;
  flexOut[flexLen] = '\0';
  Flex::send(flexOut);
  flexLen = 0;
}

void pump() {
  if (paused) return;

  if (backend == WK_BACKEND_FLEX) {
    // The radio owns element timing, so the buffer can be handed over in
    // batches; only escapes need to be applied in stream order.
    uint8_t b;
    while (bufPeek(b)) {
      if (b == ESC_SPEED) {
        if (bufCount() < 2) break;
        bufDrop();
        uint8_t wpm; bufPeek(wpm); bufDrop();
        flushFlex();
        Flex::setWpm(wpm);
        continue;
      }
      if (b == ESC_MERGE) { bufDrop(); continue; }   // no prosign concept in cwx
      if (flexLen >= sizeof(flexOut) - 1) break;
      bufDrop();
      flexOut[flexLen++] = (char)b;
      if (monitorLocal) monPush((char)b);           // sidetone only, delayed
      if (serialEcho) echoPush((char)b);            // echoed as the radio sends it
    }
    flushFlex();
    return;
  }

  // Local backend: stay only a couple of characters ahead of the keyer so
  // echo timing is honest and escapes land at the right point.
  uint8_t b;
  while (Keyer::queueDepth() < 3 && bufPeek(b)) {
    if (b == ESC_SPEED) {
      if (bufCount() < 2) break;
      if (Keyer::busy()) break;          // let the previous text drain first
      bufDrop();
      uint8_t wpm; bufPeek(wpm); bufDrop();
      Keyer::setWpm(wpm);
      continue;
    }
    if (b == ESC_MERGE) {
      bufDrop();
      Keyer::sendChar(KEYER_MERGE_MARK);
      continue;
    }
    bufDrop();
    Keyer::sendChar((char)b);
    if (serialEcho) emit(b);
  }
}

}  // namespace

// ── Public API ────────────────────────────────────────────
namespace WinKeyer {

void begin() {
  bufReset();
  hostIsOpen = false;
}

void feed(uint8_t b, WriteFn s) {
  sink = s;

  // Collecting parameters for a command already in progress.
  if (paramsNeeded > 0) {
    if (paramCount < sizeof(paramBuf)) paramBuf[paramCount] = b;
    paramCount++;
    if (--paramsNeeded == 0) {
      if (inAdmin) execAdmin(pendingSub, paramBuf, paramCount);
      else         execImmediate(pendingCmd, paramBuf, paramCount);
      inAdmin = false;
    }
    return;
  }

  // Second byte of an admin command is its sub-command.
  if (awaitingSub) {
    awaitingSub = false;
    pendingSub  = b;
    paramCount  = 0;
    paramsNeeded = adminParams(b);
    if (paramsNeeded == 0) { execAdmin(b, nullptr, 0); inAdmin = false; }
    return;
  }

  if (b == 0x00) {                    // admin prefix
    inAdmin = true;
    awaitingSub = true;
    return;
  }

  if (b < 0x20) {                     // immediate command
    pendingCmd   = b;
    paramCount   = 0;
    paramsNeeded = IMM_PARAMS[b];
    if (paramsNeeded == 0) execImmediate(b, nullptr, 0);
    return;
  }

  // Printable: text to send. WK uses 0x7F-style high codes for control,
  // everything from 0x20 up is buffered as CW.
  if (!hostIsOpen) return;            // ignore text until the host opens
  bufPush(b);
}

// Emit as many as the radio has actually sent. Self-correcting: if the
// Flex backstop gives up on a stalled radio and zeroes pending, everything
// outstanding is flushed rather than stranded.
void pumpEcho() {
  if (backend != WK_BACKEND_FLEX || !serialEcho) return;
  int outstanding = (int)echoCount() - Flex::pending();
  while (outstanding-- > 0 && echoCount() > 0)
    emit((uint8_t)echoQ[echoHead++ % sizeof(echoQ)]);
}

// Hand-sent characters go back to the host so a logger can capture what was
// keyed by hand. Always drained, echoed or not, or the queue would fill and
// stall the decoder.
void pumpPaddleEcho() {
  char c;
  bool on = (paddleEchoCfg == 2) ? paddleEchoBit : (paddleEchoCfg == 1);
  while (Keyer::decodedRead(c))
    if (on && hostIsOpen) emit((uint8_t)c);
}

void poll() {
  pump();
  monPump();
  pumpEcho();
  pumpPaddleEcho();
  emitStatus(false);
  emitPot(false);
}

void abort() {
  bufReset();
  flexLen = 0;
  Keyer::clearBuffer();
  if (backend == WK_BACKEND_FLEX) { Flex::clear(); echoReset(); monReset(); }
}

void sendText(const char* text) {
  if (!text || !*text) return;
  if (backend == WK_BACKEND_FLEX) {
    Flex::send(text);                       // the radio generates the CW
    if (monitorLocal)                       // ...and we make the sidetone
      for (const char* p = text; *p; p++) monPush(*p);
  } else {
    for (const char* p = text; *p; p++) Keyer::sendChar(*p);
    Keyer::sendChar(' ');
  }
}

void setMonitor(bool on) {
  monitorLocal = on;
  if (!on) Keyer::clearBuffer();
}
bool monitor() { return monitorLocal; }

void    setPaddleEcho(uint8_t mode) { paddleEchoCfg = mode; }
uint8_t paddleEcho() { return paddleEchoCfg; }
bool    paddleEchoActive() {
  return (paddleEchoCfg == 2) ? paddleEchoBit : (paddleEchoCfg == 1);
}

uint8_t modeRegister() { return modeReg; }
bool    echoEnabled()  { return serialEcho; }

void     setMonitorDelayMs(uint16_t ms) { cfgMonDelayMs = ms; if (!ms) monReset(); }
uint16_t monitorDelayMs()               { return cfgMonDelayMs; }
uint16_t monitorDelayNowMs()            { return monDelayNow(); }

bool hostOpen() { return hostIsOpen; }

void closeHost() {
  if (hostIsOpen) Settings::restoreKeyer();
  resetToDefaults();
  hostIsOpen = false;
  sink = nullptr;
}

void      setBackend(WkBackend b) { flushFlex(); backend = b; }
WkBackend getBackend()            { return backend; }

}  // namespace WinKeyer
