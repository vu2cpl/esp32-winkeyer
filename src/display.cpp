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

uint8_t       i2cAddr    = 0;
bool          taskStarted = false;
volatile bool cfgEnabled = true;
bool          blanked    = false;
unsigned long splashUntil = 0;

uint32_t busHz = 400000;

bool busUp = false;

// Bring the bus up ONCE. Calling Wire.begin() before every probe re-inits
// the driver mid-scan, and the results after the first are then unreliable
// — that made the OLED at 0x3C miss and a phantom appear at 0x27, i.e. the
// firmware confidently drove a 20x4 LCD that was not there.
void ensureBus(uint32_t hz) {
  if (!busUp) { Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, hz); busUp = true; }
  Wire.setClock(hz);
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
void pickBusSpeed() {
  if (answersAt(i2cAddr, 400000)) { busHz = 400000; return; }
  busHz = 100000;
  Wire.setClock(busHz);
  Log::println("[DISP] panel does not answer at 400 kHz — running the bus at "
                 "100 kHz. Works, but add 4.7k pull-ups to 3V3 or shorten the "
                 "leads if the panel ever goes missing at boot.");
}

void startTask();

// Bring up whatever is on the bus now. Called at boot and again whenever the
// operator hints a panel may have appeared (/i2c, /disp on) — the probe used
// to run only once at boot, so a panel wired to a running board stayed dark
// until the next reset with no clue as to why.
bool tryAdopt();

void startPanel() {
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

void drawMainLcd() {
  char l[24];
  const char* be = "LOCAL";
  if (WinKeyer::getBackend() == WK_BACKEND_FLEX)
    be = !Flex::connected() ? "FLEX?" : (Flex::sliceReady() ? "FLEX" : "FLEX!");
  const char* act = Keyer::tuning() ? "TUNE" : (Keyer::keyIsDown() ? "KEY" : "");

  if (lcdRows >= 4) {
    snprintf(l, sizeof l, "%2u WPM %s %4s", Keyer::getWpm(),
             Keyer::getPotEnabled() ? "POT" : "FIX", act);
    lcdLine(0, l);
    snprintf(l, sizeof l, "%-5s %c %s%s", be,
             Keyer::getMode() == KEYER_IAMBIC_A ? 'A' : 'B',
             WinKeyer::hostOpen() ? "HOST" : "----",
             Net::clientConnected() ? "+NET" : "");
    lcdLine(1, l);
    if (WiFi.status() == WL_CONNECTED)
      snprintf(l, sizeof l, "%s", WiFi.localIP().toString().c_str());
    else
      snprintf(l, sizeof l, "join %s", WIFI_AP_NAME);
    lcdLine(2, l);
    snprintf(l, sizeof l, "%ddBm  tail %ums", (int)WiFi.RSSI(),
             Keyer::getPttTailMs());
    lcdLine(3, l);
  } else {
    // 16x2: speed and backend on top, address below — replaced by the
    // activity flag while keying, which matters more in that moment.
    snprintf(l, sizeof l, "%2uWPM %s %s", Keyer::getWpm(),
             Keyer::getPotEnabled() ? "POT" : "FIX", be);
    lcdLine(0, l);
    if (*act)                              snprintf(l, sizeof l, "%s", act);
    else if (WiFi.status() == WL_CONNECTED) snprintf(l, sizeof l, "%s",
                                                    WiFi.localIP().toString().c_str());
    else                                    snprintf(l, sizeof l, "no wifi");
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
  oled->drawStr(0, 6, "WinKeyer");
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
  if (Keyer::tuning()) {
    oled->drawBox(96, 15, 32, 13);
    oled->setDrawColor(0);
    oled->drawStr(101, 25, "TUNE");
    oled->setDrawColor(1);
  } else if (Keyer::keyIsDown()) {
    oled->drawBox(96, 15, 32, 13);
    oled->setDrawColor(0);
    oled->drawStr(105, 25, "KEY");
    oled->setDrawColor(1);
  } else {
    oled->drawFrame(96, 15, 32, 13);
  }
  oled->drawHLine(0, 40, 128);

  // ── backend + host links ──
  const char* be = "LOCAL";
  if (WinKeyer::getBackend() == WK_BACKEND_FLEX) {
    // One glyph carries the whole Flex story: '?' not connected,
    // '!' connected but the radio has no CW slice to key.
    be = !Flex::connected() ? "FLEX?" : (Flex::sliceReady() ? "FLEX" : "FLEX!");
  }
  snprintf(buf, sizeof buf, "%-5s %c %s%s", be,
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
    if (cfgEnabled) {
      if (blanked) {
        if (kind == KIND_LCD) lcd->backlight(); else oled->setPowerSave(0);
        blanked = false;
      }
      if (millis() >= splashUntil) drawMain();
    } else if (!blanked) {
      if (kind == KIND_LCD) { lcd->clear(); lcd->noBacklight(); }
      else { oled->clearBuffer(); oled->sendBuffer(); oled->setPowerSave(1); }
      blanked = true;   // stop burning the panel in when it is not wanted
    }
    vTaskDelay(pdMS_TO_TICKS(200));
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
  startTask();
  Log::printf("[DISP] %s at 0x%02X on I2C %d/%d @ %u kHz\n",
                Display::controller(), i2cAddr, PIN_I2C_SDA, PIN_I2C_SCL,
                (unsigned)(busHz / 1000));
  return true;
}

}  // namespace

namespace Display {

void begin() {
  if (!tryAdopt())
    Log::printf("[DISP] no OLED at 0x3C/0x3D on I2C %d/%d — display off "
                  "(wire one and run /i2c, no reboot needed)\n",
                  PIN_I2C_SDA, PIN_I2C_SCL);
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
    i2cAddr = 0;
    if (!tryAdopt()) {
      Log::println("[DISP] auto: nothing on the bus");
      return true;
    }
    return true;
  }

  if (!strcasecmp(name, "sh1106") || !strcasecmp(name, "ssd1306")) {
    useSh1106 = !strcasecmp(name, "sh1106");
    oled = useSh1106 ? (U8G2*)&panelSh1106 : (U8G2*)&panelSsd1306;
    if (kind == KIND_OLED && i2cAddr) startPanel();
    return true;
  }

  if (!strcasecmp(name, "lcd16x2") || !strcasecmp(name, "lcd20x4")) {
    bool big = !strcasecmp(name, "lcd20x4");
    lcdCols = big ? 20 : 16;
    lcdRows = big ? 4  : 2;
    if (kind == KIND_LCD && i2cAddr) startPanel();
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
  cfgEnabled = en;
  if (en) tryAdopt();   // panel may have been wired since boot
}
bool    enabled()  { return cfgEnabled && i2cAddr != 0; }

}  // namespace Display
