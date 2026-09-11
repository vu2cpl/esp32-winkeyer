// ============================================================
//  ESP32 WinKeyer — 128x64 OLED status display (SH1106 / SSD1306)
//
//  One screen, refreshed at 5 Hz from a dedicated task:
//
//    WinKeyer              -52dBm     header: link quality
//    ──────────────────────────────
//     28 WPM   POT            [KEY]   speed, where it came from, activity
//    ──────────────────────────────
//    LOCAL B  HOST+NET               backend, iambic mode, host links
//    192.168.1.20                    address, or what to do about it
//
//  The panel is optional hardware. If the I²C probe finds nothing the
//  module disables itself and the rest of the firmware never notices.
// ============================================================

#include "display.h"
#include "log.h"
#include "pins.h"
#include "config.h"
#include "keyer.h"
#include "winkeyer.h"
#include "net.h"
#include "flex.h"
#include <Wire.h>
#include <U8g2lib.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>

namespace {

// Both controllers are 128x64 and share the whole drawing API, so the
// renderer is written once against the U8G2 base class and the concrete
// panel is chosen at run time. Two full-buffer objects cost 2 KB of the
// ESP32's 320 KB — cheap next to needing a reflash to swap panels.
U8G2_SH1106_128X64_NONAME_F_HW_I2C  panelSh1106(U8G2_R0, U8X8_PIN_NONE,
                                                PIN_I2C_SCL, PIN_I2C_SDA);
U8G2_SSD1306_128X64_NONAME_F_HW_I2C panelSsd1306(U8G2_R0, U8X8_PIN_NONE,
                                                 PIN_I2C_SCL, PIN_I2C_SDA);

// Default SH1106: the 1.3" panels in the shack are SH1106, and an SSD1306
// driven as SH1106 is just as visibly wrong as the reverse, so there is no
// "safe" default — only the likely one.
bool          useSh1106  = true;
U8G2*         oled       = &panelSh1106;

// Which family is actually fitted. OLEDs answer at 0x3C/0x3D and HD44780
// backpacks at 0x27/0x3F, so the FAMILY is detectable — one firmware runs
// whichever panel is plugged in. Geometry is not: a 16x2 and a 20x4 are
// the same chip at the same address, so that stays a setting, exactly like
// SH1106 vs SSD1306.
enum Kind : uint8_t { KIND_NONE, KIND_OLED, KIND_LCD };
Kind               kind    = KIND_NONE;
LiquidCrystal_I2C* lcd     = nullptr;
uint8_t            lcdCols = 20, lcdRows = 4;
// 16x2 row 2 alternates the IP with a slice warning, 2 s each.
inline uint32_t lcdPhase() { return (millis() / 2000) & 1; }

uint8_t       i2cAddr    = 0;
bool          taskStarted = false;
volatile bool cfgEnabled = true;
bool          blanked    = false;
unsigned long splashUntil  = 0;
unsigned long lastProbeMs  = 0;
bool          probedOnce   = false;
// Settings::begin() runs in setup() and calls setController()/setEnabled().
// Those must not touch I²C there — that is the hang this whole change is
// about — so they set this and the task does the work.
volatile bool needReinit   = false;

// Everything the screen shows, folded into one value. A 128x64 frame is
// 1 KB and ~100 ms of blocking I²C at 100 kHz, so the win is not a faster
// bus — it is not sending a frame at all when nothing has changed. The
// panel then tracks a knob turn within a tick instead of averaging 175 ms
// behind it, and sits silent on the bus while idle.
uint32_t lastSig = 0xFFFFFFFF;

uint32_t stateSig() {
  uint32_t h = 2166136261u;
  auto mix = [&](uint32_t v) { h = (h ^ v) * 16777619u; };
  mix(Keyer::getWpm());
  mix(Keyer::getPotEnabled());
  mix(Keyer::getMode());
  mix(Keyer::msSinceKey() < 150);   // latched activity, not the live edge
  mix(Keyer::pttIsOn());
  mix(Keyer::tuning());
  mix(Keyer::getRadio());
  mix(Keyer::getPttTailMs());
  mix(WinKeyer::getBackend());
  mix(WinKeyer::hostOpen());
  mix(Net::clientConnected());
  mix(Flex::connected());
  mix(Flex::sliceReady());
  { char w[24]; Flex::sliceWarning(w, sizeof w, Flex::WARN_SHORT);  // mode can change text
    for (const char* p = w; *p; p++) mix((uint8_t)*p);
    // The 16x2 alternates the warning with the IP, so it must redraw on
    // each swap. Only there: the OLED would resend 1 KB for nothing.
    if (*w && kind == KIND_LCD && lcdRows < 4) mix(lcdPhase()); }
  mix((uint32_t)WiFi.status());
  mix((uint32_t)WiFi.localIP());
  mix((uint32_t)((int)WiFi.RSSI() / 3));   // bucketed: RSSI jitters constantly
  return h;
}

uint32_t busHz = 400000;
// 0 = pick automatically (try 400 kHz, fall back if the panel does not
// answer there). Otherwise force this rate. Needed because answering the
// address probe at 400 kHz does NOT prove a panel can take 1 KB frames at
// that rate — marginal wiring passes detection and renders nothing.
uint32_t busForce = 0;

bool busUp = false;

// Bring the bus up ONCE. Calling Wire.begin() before every probe re-inits
// the driver mid-scan, and the results after the first are then unreliable
// — that made the OLED at 0x3C miss and a phantom appear at 0x27, i.e. the
// firmware confidently drove a 20x4 LCD that was not there.
void ensureBus(uint32_t hz) {
  if (!busUp) { Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, hz); busUp = true; }
  Wire.setClock(hz);
  // A stuck line — one loose jumper is enough — makes an I²C transaction
  // block forever. Without this the board hung inside display init and
  // never finished booting: no web server, no host link, a keyer taken
  // down by an ornament. The keyer must outlive its display.
  Wire.setTimeOut(50);
}

bool answersAt(uint8_t addr, uint32_t hz) {
  ensureBus(hz);
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

// Probe both addresses the common breakouts use. A module with its
// address jumper moved answers on 0x3D instead of 0x3C.
//
// Always probe at 100 kHz. Panels on breadboard leads, or relying on weak
// on-module pull-ups, answer reliably at 100 kHz but only intermittently at
// 400 kHz — which showed up here as a display that was found on some boots
// and not others. Detection must not be the thing that is marginal.
uint8_t probe() {
  const uint8_t oleds[] = {0x3C, 0x3D};
  for (uint8_t a : oleds)
    if (answersAt(a, 100000)) { kind = KIND_OLED; return a; }
  const uint8_t lcds[] = {0x27, 0x3F};      // PCF8574 backpacks
  for (uint8_t a : lcds)
    if (answersAt(a, 100000)) { kind = KIND_LCD; return a; }
  kind = KIND_NONE;
  return 0;
}

// Rendering is a different question from detection: 128x64 is 1 KB per
// frame, ~25 ms at 400 kHz but ~100 ms at 100 kHz, and that whole time is
// spent inside a blocking I²C transaction. So try for 400 kHz, prove the
// panel still answers there, and fall back honestly if it does not.
// 400 kHz for rendering by default; DETECTION still probes at 100 kHz,
// which is a separate question and stays slow.
//
// Note what the earlier 100 kHz default did and did not fix. Frames stopped
// failing for a while, so it looked like the answer — but the panel later
// went dark at 100 kHz too, and the boot then hung inside an I²C
// transaction, which only happens when a line is held low. The real fault
// was wiring; the bus rate was treading on the symptom. With rendering now
// skipped unless something changed, frame cost matters far less either way.
//
// /disp slow forces 100 kHz and persists, for wiring that needs it.
void pickBusSpeed() {
  busHz = busForce ? busForce : 400000;
  Wire.setClock(busHz);
}

void startTask();

// Bring up whatever is on the bus now. Called at boot and again whenever the
// operator hints a panel may have appeared (/i2c, /disp on) — the probe used
// to run only once at boot, so a panel wired to a running board stayed dark
// until the next reset with no clue as to why.
bool tryAdopt();

void startPanel() {
  lastSig = 0xFFFFFFFF;      // whatever was on the glass is gone
  if (kind == KIND_LCD) {
    // HD44780 backpacks are 100 kHz parts and only push ~80 bytes a frame,
    // so there is nothing to gain from probing for 400 kHz here.
    busHz = 100000;
    Wire.setClock(busHz);
    delete lcd;
    lcd = new LiquidCrystal_I2C(i2cAddr, lcdCols, lcdRows);
    lcd->init();
    lcd->backlight();
    lcd->clear();
    return;
  }
  pickBusSpeed();
  oled->setI2CAddress(i2cAddr << 1);
  oled->begin();
  oled->setBusClock(busHz);
}

// Text panels get their own layout rather than a squeezed graphic one:
// 20x4 carries speed, backend, address and activity; 16x2 has room for
// speed and one more line, so the address shares row 2 with the activity
// flag only when something is actually happening.
void lcdLine(uint8_t row, const char* text) {
  if (row >= lcdRows) return;
  char pad[21];
  snprintf(pad, lcdCols + 1, "%-*s", (int)lcdCols, text);
  lcd->setCursor(0, row);
  lcd->print(pad);
}

// Which radio the key line drives, shown as a suffix on the backend so it
// reads as one token: LOCAL1, FLEX2, FLEXB. Attached rather than spaced
// because the "both" letter B would otherwise sit next to the iambic mode
// letter, which is also A or B — "FLEX B B" is not a readable status line.
const char* radioTag() {
  switch (Keyer::getRadio()) {
    case 2:  return "2";
    case 3:  return "B";
    default: return "1";
  }
}

void drawMainLcd() {
  char l[24];
  const char* be = "LOCAL";
  if (WinKeyer::getBackend() == WK_BACKEND_FLEX)
    be = !Flex::connected() ? "FLX?" : (Flex::sliceReady() ? "FLX" : "FLX!");
  // PTT is held for the whole over; the key only during elements. Showing
  // both makes the lead-in and tail visible as PTT-without-KEY either side
  // of the sending, which is exactly what those two settings control.
  bool tune = Keyer::tuning();
  bool ptt  = Keyer::pttIsOn();
  bool key  = Keyer::msSinceKey() < 150;
  const char* act = tune ? "TUNE"
                  : (ptt && key) ? "PTT KEY"
                  : ptt          ? "PTT"
                  : key          ? "KEY" : "";

  if (lcdRows >= 4) {
    snprintf(l, sizeof l, "%2u WPM %s %4s", Keyer::getWpm(),
             Keyer::getPotEnabled() ? "POT" : "FIX", act);
    lcdLine(0, l);
    char bere[10];
    snprintf(bere, sizeof bere, "%s%s", be, radioTag());
    snprintf(l, sizeof l, "%-6s %c %s%s", bere,
             Keyer::getMode() == KEYER_IAMBIC_A ? 'A' : 'B',
             WinKeyer::hostOpen() ? "HOST" : "----",
             Net::clientConnected() ? "+NET" : "");
    lcdLine(1, l);
    if (WiFi.status() == WL_CONNECTED)
      snprintf(l, sizeof l, "%s", WiFi.localIP().toString().c_str());
    else
      snprintf(l, sizeof l, "join %s", WIFI_AP_NAME);
    lcdLine(2, l);
    // Row 4 is the least-needed line, so a slice warning takes it over.
    char warn[24];
    Flex::sliceWarning(warn, sizeof warn, Flex::WARN_SHORT);
    if (*warn)
      snprintf(l, sizeof l, "%s", warn);
    else
      snprintf(l, sizeof l, "%ddBm  tail %ums", (int)WiFi.RSSI(),
               Keyer::getPttTailMs());
    lcdLine(3, l);
  } else {
    // 16x2: row 0 packs speed, mode, speed-source and backend into all 16
    // columns. Row 1 is the address, because that is what you need in order
    // to reach the web page — replaced by KEY/TUNE only while sending.
    // 16 columns exactly: "28WPM POT FLEX!1". The iambic mode letter is
    // the one thing dropped here — it changes once a year, whereas which
    // radio is live can change between overs.
    snprintf(l, sizeof l, "%2uWPM %s %s%s", Keyer::getWpm(),
             Keyer::getPotEnabled() ? "POT" : "FIX", be, radioTag());
    lcdLine(0, l);

    // A slice warning alternates with the address rather than replacing it.
    char warn[24];
    Flex::sliceWarning(warn, sizeof warn, Flex::WARN_TINY);
    if (*act)
      snprintf(l, sizeof l, "%s", act);
    else if (*warn && lcdPhase())
      snprintf(l, sizeof l, "%s", warn);
    else if (WiFi.status() == WL_CONNECTED)
      snprintf(l, sizeof l, "%s", WiFi.localIP().toString().c_str());
    else
      snprintf(l, sizeof l, "no wifi");
    lcdLine(1, l);
  }
}

void drawSplash() {
  if (kind == KIND_LCD) {
    lcd->clear();
    lcdLine(0, "VU2CPL WinKeyer");
    lcdLine(1, "K1EL WK3 protocol");
    return;
  }
  oled->clearBuffer();
  oled->setFont(u8g2_font_ncenB14_tr);
  oled->drawStr(24, 26, "VU2CPL");
  oled->setFont(u8g2_font_6x10_tf);
  oled->drawStr(16, 44, "ESP32 WinKeyer");
  oled->setFont(u8g2_font_5x7_tf);
  oled->drawStr(20, 58, "K1EL WK3 protocol");
  oled->sendBuffer();
}

void drawMain() {
  if (kind == KIND_LCD) { drawMainLcd(); return; }
  char buf[24];
  oled->clearBuffer();

  // ── header: who we are, and how good the link is ──
  oled->setFont(u8g2_font_5x7_tf);
  // The title gives way to a slice warning: without a CW slice the radio
  // sends nothing and says nothing, and FLX! below is easy to miss.
  char warn[24];
  Flex::sliceWarning(warn, sizeof warn, Flex::WARN_SHORT);
  oled->drawStr(0, 6, warn[0] ? warn : "WinKeyer");
  if (WiFi.status() == WL_CONNECTED) snprintf(buf, sizeof buf, "%ddBm", (int)WiFi.RSSI());
  else                               snprintf(buf, sizeof buf, "no wifi");
  oled->drawStr(128 - oled->getStrWidth(buf), 6, buf);
  oled->drawHLine(0, 9, 128);

  // ── speed, the one number worth reading across the shack ──
  snprintf(buf, sizeof buf, "%u", Keyer::getWpm());
  oled->setFont(u8g2_font_fub20_tn);
  oled->drawStr(2, 36, buf);
  int x = 2 + oled->getStrWidth(buf) + 5;
  oled->setFont(u8g2_font_6x10_tf);
  oled->drawStr(x, 25, "WPM");
  // Where that number came from: the knob, or whoever set it last.
  oled->drawStr(x, 36, Keyer::getPotEnabled() ? "POT" : "FIX");

  // ── activity: tune latches, key follows the element ──
  // Empty box = PTT up. Filled box with KEY = sending. Nothing when idle.
  {
    const bool key = Keyer::tuning() || Keyer::msSinceKey() < 150;
    const bool ptt = Keyer::pttIsOn();
    if (key) {
      const char* t = Keyer::tuning() ? "TUNE" : "KEY";
      oled->drawBox(96, 15, 32, 13);
      oled->setDrawColor(0);
      oled->drawStr(96 + (32 - oled->getStrWidth(t)) / 2, 25, t);
      oled->setDrawColor(1);
    } else if (ptt) {
      oled->drawFrame(96, 15, 32, 13);
    }
  }
  oled->drawHLine(0, 40, 128);

  // ── backend + host links ──
  const char* be = "LOCAL";
  if (WinKeyer::getBackend() == WK_BACKEND_FLEX) {
    // One glyph carries the whole Flex story: '?' not connected,
    // '!' connected but the radio has no CW slice to key.
    be = !Flex::connected() ? "FLX?" : (Flex::sliceReady() ? "FLX" : "FLX!");
  }
  char bere[10];
  snprintf(bere, sizeof bere, "%s%s", be, radioTag());
  snprintf(buf, sizeof buf, "%-6s %c %s%s", bere,
           Keyer::getMode() == KEYER_IAMBIC_A ? 'A' : 'B',
           WinKeyer::hostOpen() ? "HOST" : "----",
           Net::clientConnected() ? "+NET" : "");
  oled->drawStr(0, 51, buf);

  // ── address, or what to do about not having one ──
  if (WiFi.status() == WL_CONNECTED)
    snprintf(buf, sizeof buf, "%s", WiFi.localIP().toString().c_str());
  else
    snprintf(buf, sizeof buf, "join %s", WIFI_AP_NAME);
  oled->drawStr(0, 62, buf);

  oled->sendBuffer();
}

void task(void*) {
  for (;;) {
    // Adoption happens HERE, not in setup(). A held-low I²C line blocks a
    // transaction forever, and when this ran during setup() that took the
    // whole board down — no keyer, no host link, no web page, needing a
    // power cycle. Inside its own task a stuck bus costs the panel and
    // nothing else. Retried every few seconds so a display wired up later
    // is picked up without a reboot.
    if (cfgEnabled && !i2cAddr && millis() - lastProbeMs > 3000) {
      lastProbeMs = millis();
      if (!tryAdopt() && !probedOnce) {
        probedOnce = true;
        Log::printf("[DISP] no panel on I2C %d/%d — will keep looking\n",
                    PIN_I2C_SDA, PIN_I2C_SCL);
      }
    }
    if (needReinit && i2cAddr) { needReinit = false; startPanel(); }

    if (cfgEnabled && i2cAddr) {
      if (blanked) {
        if (kind == KIND_LCD) lcd->backlight(); else oled->setPowerSave(0);
        blanked = false;
        lastSig = 0xFFFFFFFF;
      }
      if (millis() >= splashUntil) {
        uint32_t sig = stateSig();
        if (sig != lastSig) { lastSig = sig; drawMain(); }
      }
    } else if (!blanked && i2cAddr) {
      if (kind == KIND_LCD) { lcd->clear(); lcd->noBacklight(); }
      else { oled->clearBuffer(); oled->sendBuffer(); oled->setPowerSave(1); }
      blanked = true;   // stop burning the panel in when it is not wanted
    }
    vTaskDelay(pdMS_TO_TICKS(60));
  }
}

// Priority 1 (same as loopTask) on core 0: a ~25 ms I²C frame must not sit
// in front of the keyer task on core 1, nor in front of the host link that
// loop() services.
void startTask() {
  if (taskStarted) return;
  xTaskCreatePinnedToCore(task, "display", 4096, nullptr, 1, nullptr, 0);
  taskStarted = true;
}

bool tryAdopt() {
  if (i2cAddr) return true;
  i2cAddr = probe();
  if (!i2cAddr) return false;
  startPanel();
  drawSplash();
  splashUntil = millis() + 1500;
  Log::printf("[DISP] %s at 0x%02X on I2C %d/%d @ %u kHz\n",
                Display::controller(), i2cAddr, PIN_I2C_SDA, PIN_I2C_SCL,
                (unsigned)(busHz / 1000));
  return true;
}

}  // namespace

namespace Display {

void begin(bool enabled) {
  cfgEnabled = enabled;
  if (!enabled) {
    Log::println("[DISP] disabled — I2C bus not touched");
    return;
  }
  // Start the task and return immediately. Everything that touches I²C —
  // probe, init, splash, rendering — happens inside it, so the boot cannot
  // be held up by the display no matter what the bus is doing.
  startTask();
}

uint8_t scan() {
  ensureBus(100000);       // scan slow so a marginal bus still shows up
  Log::printf("[I2C] scanning bus on SDA=%d SCL=%d @ 100 kHz\n",
                PIN_I2C_SDA, PIN_I2C_SCL);
  uint8_t n = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Log::printf("[I2C]   0x%02X responds%s\n", a,
                    (a == 0x3C || a == 0x3D) ? "   <- OLED address" : "");
      n++;
    }
  }
  if (!n) {
    // A dead bus is nearly always physical. Name the four things that
    // actually cause it, in the order they are worth checking.
    Log::println("[I2C] nothing responded. In order of likelihood:");
    Log::println("[I2C]   1. SDA/SCL swapped — SDA must be GPIO21, SCL GPIO22");
    Log::println("[I2C]   2. no power — check 3V3 and GND at the panel itself");
    Log::println("[I2C]   3. missing pull-ups — 4.7k from each line to 3V3");
    Log::println("[I2C]   4. a broken jumper or dry joint on one of the four wires");
  }
  tryAdopt();   // restores the panel's own bus speed if one is found
  return n;
}

bool setController(const char* name) {
  // "auto" re-probes, which is how you get back to the OLED after trying an
  // LCD (or the reverse) without a reflash — the family is detectable even
  // though the geometry is not.
  if (!strcasecmp(name, "auto")) {
    i2cAddr = 0;          // the task re-probes within a few seconds
    probedOnce = false;
    return true;
  }

  if (!strcasecmp(name, "sh1106") || !strcasecmp(name, "ssd1306")) {
    useSh1106 = !strcasecmp(name, "sh1106");
    oled = useSh1106 ? (U8G2*)&panelSh1106 : (U8G2*)&panelSsd1306;
    needReinit = true;
    return true;
  }

  if (!strcasecmp(name, "slow") || !strcasecmp(name, "fast")) {
    busForce = !strcasecmp(name, "slow") ? 100000 : 400000;
    needReinit = true;
    Log::printf("[DISP] bus forced to %u kHz\n", (unsigned)(busForce / 1000));
    return true;
  }

  if (!strcasecmp(name, "lcd16x2") || !strcasecmp(name, "lcd20x4")) {
    bool big = !strcasecmp(name, "lcd20x4");
    lcdCols = big ? 20 : 16;
    lcdRows = big ? 4  : 2;
    needReinit = true;
    return true;
  }
  return false;
}

const char* controller() {
  if (kind == KIND_LCD) return lcdRows >= 4 ? "lcd20x4" : "lcd16x2";
  return useSh1106 ? "sh1106" : "ssd1306";
}

bool    present()  { return i2cAddr != 0; }
uint8_t address()  { return i2cAddr; }
void    setEnabled(bool en) {
  cfgEnabled = en;      // the task adopts, so this never blocks the caller
}
bool    enabled()  { return cfgEnabled && i2cAddr != 0; }

}  // namespace Display
