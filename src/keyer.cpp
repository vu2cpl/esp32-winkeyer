// ============================================================
//  ESP32 WinKeyer — keyer core
//
//  Iambic A/B state machine, paddle inputs, key/PTT outputs,
//  LEDC sidetone, speed pot. Ticks at 1 kHz in its own FreeRTOS
//  task on core 1, priority above the Arduino loop, so element
//  timing is unaffected by WiFi/MQTT work.
//
//  Timing: PARIS standard — dit = 1200/WPM ms, dah = 3 dits,
//  inter-element = 1 dit, inter-char = 3 dits, word = 7 dits.
//  Weighting shifts the mark/space balance without changing WPM;
//  ratio changes dah length; Farnsworth stretches only the gaps.
//
//  Iambic semantics (Curtis): both modes latch the opposite
//  paddle during an element; mode B also latches during the
//  inter-element space, mode A decides from the live paddles.
//
//  Reference: K3ng CW keyer (Anthony Good K3NG) for behaviour;
//  implementation is original.
// ============================================================

#include "keyer.h"
#include "pins.h"

// ── Morse table ───────────────────────────────────────────
static const struct { char c; const char* p; } MORSE[] = {
  {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},  {'D', "-.."},
  {'E', "."},     {'F', "..-."},  {'G', "--."},   {'H', "...."},
  {'I', ".."},    {'J', ".---"},  {'K', "-.-"},   {'L', ".-.."},
  {'M', "--"},    {'N', "-."},    {'O', "---"},   {'P', ".--."},
  {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},   {'T', "-"},
  {'U', "..-"},   {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},
  {'Y', "-.--"},  {'Z', "--.."},
  {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"},
  {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
  {'8', "---.."}, {'9', "----."},
  {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'/', "-..-."},
  {'=', "-...-"},  {'+', ".-.-."},  {'-', "-....-"}, {'@', ".--.-."},
  {':', "---..."}, {';', "-.-.-."}, {'\'', ".----."}, {'"', ".-..-."},
  {'(', "-.--."},  {')', "-.--.-"}, {'!', "-.-.--"},  {'&', ".-..."},
  {'$', "...-..-"}, {'_', "..--.-"},
};

static const char* morseFor(char c) {
  for (auto& e : MORSE)
    if (e.c == c) return e.p;
  return nullptr;
}

namespace {

// ── Configuration (written from loop task, read by keyer task) ──
volatile uint8_t   cfgWpm       = 20;
volatile uint8_t   cfgWeight    = 50;
volatile uint8_t   cfgRatio     = 50;
volatile uint8_t   cfgFarns     = 0;
volatile KeyerMode cfgMode      = KEYER_IAMBIC_B;
volatile bool      cfgSwap      = false;
volatile bool      cfgSidetone  = true;
volatile uint16_t  cfgToneHz    = 600;
volatile bool      cfgPtt       = true;
volatile bool      cfgKeyOut    = true;
volatile uint16_t  cfgLeadMs    = 50;
volatile uint16_t  cfgTailMs    = 250;
volatile bool      cfgPotEn     = false;   // off until a pot is wired — GPIO34 floats
volatile uint8_t   cfgPotMin    = 10;
volatile uint8_t   cfgPotRange  = 25;

// ── Derived timing, recomputed whenever the above change ──
volatile uint16_t tMarkDit = 60, tMarkDah = 180;   // key-down durations
volatile uint16_t tGapElem = 60;                   // inter-element space
volatile uint16_t tGapChar = 120;                  // *additional* after a character
volatile uint16_t tGapWord = 240;                  // *additional* for a space

void recalc() {
  uint16_t unit = 1200 / (cfgWpm ? cfgWpm : 20);
  // Weighting moves the mark/space boundary without changing overall WPM.
  uint32_t w = cfgWeight ? cfgWeight : 50;
  tMarkDit = (uint16_t)((unit * 2 * w) / 100);
  tGapElem = (uint16_t)(unit * 2 - tMarkDit);
  if (tMarkDit < 5) tMarkDit = 5;
  if (tGapElem < 5) tGapElem = 5;
  // Ratio: nominal 50 → dah is 3 dits.
  tMarkDah = (uint16_t)((uint32_t)tMarkDit * 3 * (cfgRatio ? cfgRatio : 50) / 50);
  // Farnsworth stretches only the inter-character and word gaps.
  uint16_t gapUnit = unit;
  if (cfgFarns >= 5 && cfgFarns < cfgWpm) gapUnit = 1200 / cfgFarns;
  tGapChar = gapUnit * 2;
  tGapWord = gapUnit * 4;
}

// ── Cross-task signalling ──
QueueHandle_t  charQ;
volatile bool  flagClear    = false;
volatile bool  flagTune     = false;
volatile bool  flagPttHold  = false;
volatile bool  flagBreakIn  = false;   // sticky until read via paddleBreakIn()

// ── Keyer-task state ──
enum State : uint8_t { ST_IDLE, ST_LEAD, ST_KEYDOWN, ST_GAP, ST_TUNE };
volatile State state = ST_IDLE;
uint32_t timerMs     = 0;
bool     curIsDah    = false;
bool     curIsAuto   = false;   // current element from buffer (vs paddles)
bool     lastWasDah  = false;
bool     memDit      = false, memDah = false;
bool     mergeNext   = false;   // suppress the gap after the next character
const char* pattern  = nullptr; // remaining elements of the char being sent
bool     pttOn       = false;
uint32_t tailTimer   = 0;
volatile bool keyDownFlag = false;

// Debounced paddles
uint8_t ditCnt = 0, dahCnt = 0;
volatile bool dit = false, dah = false;
bool    prevDit = false, prevDah = false;
static const uint8_t DEBOUNCE_TICKS = 3;

// Speed pot
uint32_t potAccum = 0;
uint16_t potTick  = 0;
int      potLastWpm = -1;

static const int SIDETONE_CH = 0;

void (*keyHook)(bool) = nullptr;

// ── Low-level outputs ─────────────────────────────────────
void toneOn()  { if (cfgSidetone) ledcWriteTone(SIDETONE_CH, cfgToneHz); }
void toneOff() { ledcWriteTone(SIDETONE_CH, 0); }
void keyDown() {
  if (cfgKeyOut) digitalWrite(PIN_KEY_OUT, HIGH);
  bool was = keyDownFlag;
  keyDownFlag = true;
  toneOn();
  if (!was && keyHook) keyHook(true);
}
void keyUp() {
  digitalWrite(PIN_KEY_OUT, LOW);
  bool was = keyDownFlag;
  keyDownFlag = false;
  toneOff();
  if (was && keyHook) keyHook(false);
}
void pttAssert()  { if (cfgPtt) digitalWrite(PIN_PTT_OUT, HIGH); pttOn = true; }
void pttRelease() {
  if (flagPttHold) return;          // host is holding PTT down explicitly
  digitalWrite(PIN_PTT_OUT, LOW);
  pttOn = false;
}

// ── State machine helpers ─────────────────────────────────
void startElement(bool isDah, bool isAuto) {
  curIsDah   = isDah;
  curIsAuto  = isAuto;
  lastWasDah = isDah;
  if (isDah) memDah = false; else memDit = false;
  keyDown();
  state   = ST_KEYDOWN;
  timerMs = isDah ? tMarkDah : tMarkDit;
}

void goIdle() {
  pattern = nullptr;
  memDit = memDah = false;
  mergeNext = false;
  state = ST_IDLE;
  tailTimer = cfgTailMs;
}

// Decide what happens after a gap expires (also entered from IDLE/LEAD
// when work appears).
void decideNext() {
  // Paddles first. Mode B considers latched memory; mode A only live paddles.
  bool wantDit = dit || (cfgMode == KEYER_IAMBIC_B && memDit);
  bool wantDah = dah || (cfgMode == KEYER_IAMBIC_B && memDah);
  if (wantDit && wantDah) { startElement(!lastWasDah, false); return; }  // squeeze alternates
  if (wantDit)            { startElement(false, false);       return; }
  if (wantDah)            { startElement(true,  false);       return; }

  // Continue the character in progress.
  if (pattern && *pattern) { startElement(*pattern++ == '-', true); return; }
  if (pattern) {
    // Character finished. One element gap has already elapsed; add the rest
    // of the inter-character space unless a merge (prosign) suppressed it.
    pattern = nullptr;
    if (mergeNext) { mergeNext = false; decideNext(); return; }
    state = ST_GAP;
    timerMs = tGapChar;
    return;
  }

  // Pop the next buffered item.
  char c;
  while (xQueueReceive(charQ, &c, 0) == pdTRUE) {
    if (c == KEYER_MERGE_MARK) { mergeNext = true; continue; }
    if (c == ' ') {                    // word gap, on top of the character gap
      state = ST_GAP;
      timerMs = tGapWord;
      return;
    }
    pattern = morseFor(c);
    if (pattern && *pattern) { startElement(*pattern++ == '-', true); return; }
    pattern = nullptr;                 // unknown char — skip it
  }

  goIdle();
}

// Begin activity out of idle: honour PTT lead-in before the first element.
void startActivity() {
  if (cfgPtt && !pttOn && cfgLeadMs) {
    pttAssert();
    state = ST_LEAD;
    timerMs = cfgLeadMs;
  } else {
    pttAssert();
    decideNext();
  }
}

// ── Paddle sampling ───────────────────────────────────────
void samplePaddles() {
  bool rawDit = !digitalRead(cfgSwap ? PIN_PADDLE_DAH : PIN_PADDLE_DIT);
  bool rawDah = !digitalRead(cfgSwap ? PIN_PADDLE_DIT : PIN_PADDLE_DAH);
  ditCnt = rawDit ? min<uint8_t>(ditCnt + 1, DEBOUNCE_TICKS) : 0;
  dahCnt = rawDah ? min<uint8_t>(dahCnt + 1, DEBOUNCE_TICKS) : 0;
  prevDit = dit; prevDah = dah;
  dit = ditCnt >= DEBOUNCE_TICKS;
  dah = dahCnt >= DEBOUNCE_TICKS;
}

// ── Speed pot ─────────────────────────────────────────────
void samplePot() {
  if (!cfgPotEn) return;
  if (++potTick < 50) return;          // one reading per 50 ms
  potTick = 0;
  potAccum = (potAccum * 3 + (uint32_t)analogRead(PIN_SPEED_POT)) / 4;  // IIR smooth
  int wpm = cfgPotMin + (int)((potAccum * cfgPotRange) / 4095);
  if (potLastWpm < 0) { potLastWpm = wpm; return; }   // first reading: don't stomp boot speed
  if (wpm != potLastWpm) {
    potLastWpm = wpm;
    cfgWpm = wpm;
    recalc();
  }
}

// ── The 1 kHz keyer task ──────────────────────────────────
void keyerTask(void*) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(1));
    samplePaddles();
    samplePot();

    // Host abort (WK "clear buffer"): stop buffered sending at once.
    if (flagClear) {
      flagClear = false;
      xQueueReset(charQ);
      pattern = nullptr;
      mergeNext = false;
      if (curIsAuto && state == ST_KEYDOWN) { keyUp(); goIdle(); }
      else if (state == ST_GAP)             { goIdle(); }
    }

    // Paddle break-in: a fresh paddle press aborts buffered sending.
    bool paddleEdge = (dit && !prevDit) || (dah && !prevDah);
    if (paddleEdge && (pattern || uxQueueMessagesWaiting(charQ) > 0)) {
      xQueueReset(charQ);
      pattern = nullptr;
      mergeNext = false;
      flagBreakIn = true;
      if (curIsAuto && state == ST_KEYDOWN) { keyUp(); state = ST_GAP; timerMs = tGapElem; }
    }

    // Tune mode overrides everything.
    if (flagTune && state != ST_TUNE) {
      keyUp();
      pattern = nullptr;
      pttAssert();
      keyDown();
      state = ST_TUNE;
    }

    switch (state) {
      case ST_TUNE:
        if (!flagTune) { keyUp(); goIdle(); }
        break;

      case ST_IDLE:
        if (dit || dah || uxQueueMessagesWaiting(charQ) > 0) {
          // Latch the triggering paddle so a quick tap survives the PTT lead.
          if (dit) memDit = true;
          if (dah) memDah = true;
          startActivity();
        } else if (pttOn) {
          if (tailTimer > 0) tailTimer--;
          if (tailTimer == 0) pttRelease();
        }
        break;

      case ST_LEAD:
        if (dit) memDit = true;        // keep latching through the lead-in
        if (dah) memDah = true;
        if (timerMs > 0) timerMs--;
        if (timerMs == 0) decideNext();
        break;

      case ST_KEYDOWN:
        // Both iambic modes latch the opposite paddle during an element.
        if (curIsDah && dit) memDit = true;
        if (!curIsDah && dah) memDah = true;
        if (timerMs > 0) timerMs--;
        if (timerMs == 0) {
          keyUp();
          state = ST_GAP;
          timerMs = tGapElem;          // inter-element space
        }
        break;

      case ST_GAP:
        // Mode B also latches during the space; mode A decides live.
        if (cfgMode == KEYER_IAMBIC_B) {
          if (dit) memDit = true;
          if (dah) memDah = true;
        }
        if (timerMs > 0) timerMs--;
        if (timerMs == 0) decideNext();
        break;
    }
  }
}

}  // namespace

// ── Public API ────────────────────────────────────────────
namespace Keyer {

void begin() {
  pinMode(PIN_PADDLE_DIT, INPUT_PULLUP);
  pinMode(PIN_PADDLE_DAH, INPUT_PULLUP);
  pinMode(PIN_KEY_OUT, OUTPUT);  digitalWrite(PIN_KEY_OUT, LOW);
  pinMode(PIN_PTT_OUT, OUTPUT);  digitalWrite(PIN_PTT_OUT, LOW);

  ledcSetup(SIDETONE_CH, cfgToneHz, 10);
  ledcAttachPin(PIN_SIDETONE, SIDETONE_CH);
  ledcWriteTone(SIDETONE_CH, 0);

  analogSetPinAttenuation(PIN_SPEED_POT, ADC_11db);   // full 0–3.3 V range

  recalc();
  charQ = xQueueCreate(256, sizeof(char));

  // Priority well above loopTask (1); pinned to core 1 alongside it —
  // WiFi/BT stacks live on core 0 and never preempt element timing.
  xTaskCreatePinnedToCore(keyerTask, "keyer", 4096, nullptr, 10, nullptr, 1);
}

void setWpm(uint8_t wpm) {
  cfgWpm = constrain(wpm, (uint8_t)5, (uint8_t)60);
  recalc();
  potLastWpm = -1;   // host speed rules until the pot moves again
}
uint8_t   getWpm()  { return cfgWpm; }
void      setMode(KeyerMode m) { cfgMode = m; }
KeyerMode getMode() { return cfgMode; }
void      setPaddleSwap(bool s) { cfgSwap = s; }
bool      getPaddleSwap() { return cfgSwap; }
void      setSidetone(bool en) { cfgSidetone = en; if (!en) toneOff(); }
void      setSidetoneHz(uint16_t hz) { cfgToneHz = constrain(hz, (uint16_t)300, (uint16_t)2000); }
uint16_t  getSidetoneHz() { return cfgToneHz; }
void      setPttEnabled(bool en) { cfgPtt = en; if (!en) digitalWrite(PIN_PTT_OUT, LOW); }
void      setPttLeadMs(uint16_t ms) { cfgLeadMs = ms; }
void      setPttTailMs(uint16_t ms) { cfgTailMs = ms; }
void      setPotEnabled(bool en) { cfgPotEn = en; potLastWpm = -1; }
void      setPotRange(uint8_t minWpm, uint8_t range) { cfgPotMin = minWpm; cfgPotRange = range; }
void      setKeyOutEnabled(bool en) { cfgKeyOut = en; if (!en) digitalWrite(PIN_KEY_OUT, LOW); }
void      setWeighting(uint8_t w) { cfgWeight = constrain(w, (uint8_t)10, (uint8_t)90); recalc(); }
void      setRatio(uint8_t r)     { cfgRatio  = constrain(r, (uint8_t)33, (uint8_t)66); recalc(); }
void      setFarnsworth(uint8_t w){ cfgFarns  = w; recalc(); }

bool sendChar(char c) {
  if (c != KEYER_MERGE_MARK) {
    c = toupper((unsigned char)c);
    if (c != ' ' && !morseFor(c)) return true;   // silently skip unknown chars
  }
  return xQueueSend(charQ, &c, 0) == pdTRUE;
}

size_t queueDepth() { return charQ ? uxQueueMessagesWaiting(charQ) : 0; }

void clearBuffer() { flagClear = true; }
void tune(bool on) { flagTune = on; }
bool tuning()      { return flagTune; }

void pttManual(bool on) {
  flagPttHold = on;
  if (on) { if (cfgPtt) digitalWrite(PIN_PTT_OUT, HIGH); pttOn = true; }
  else if (state == ST_IDLE) { digitalWrite(PIN_PTT_OUT, LOW); pttOn = false; }
}

bool busy()         { return state != ST_IDLE || queueDepth() > 0; }
bool keyIsDown()    { return keyDownFlag; }
bool paddleActive() { return dit || dah; }
bool paddleDit()    { return dit; }
bool paddleDah()    { return dah; }

void setKeyEventHook(void (*fn)(bool)) { keyHook = fn; }

bool paddleBreakIn() {
  bool b = flagBreakIn;
  flagBreakIn = false;
  return b;
}

}  // namespace Keyer
