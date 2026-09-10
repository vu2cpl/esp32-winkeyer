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
#include "pins.h"
#include "config.h"
#include "keyer.h"
#include "winkeyer.h"
#include "net.h"
#include "flex.h"
#include <Wire.h>
#include <U8g2lib.h>
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

uint8_t       i2cAddr    = 0;
volatile bool cfgEnabled = true;
bool          blanked    = false;
unsigned long splashUntil = 0;

// Probe both addresses the common breakouts use. A module with its
// address jumper moved answers on 0x3D instead of 0x3C.
uint8_t probe() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  const uint8_t candidates[] = {0x3C, 0x3D};
  for (uint8_t a : candidates) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) return a;
  }
  return 0;
}

void startPanel() {
  oled->setI2CAddress(i2cAddr << 1);
  oled->begin();
  oled->setBusClock(400000);
}

void drawSplash() {
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
      if (blanked) { oled->setPowerSave(0); blanked = false; }
      if (millis() >= splashUntil) drawMain();
    } else if (!blanked) {
      oled->clearBuffer();
      oled->sendBuffer();
      oled->setPowerSave(1);   // stop burning the panel in when it is not wanted
      blanked = true;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

}  // namespace

namespace Display {

void begin() {
  i2cAddr = probe();
  if (!i2cAddr) {
    Serial.printf("[DISP] no OLED at 0x3C/0x3D on I2C %d/%d — display off\n",
                  PIN_I2C_SDA, PIN_I2C_SCL);
    return;
  }
  startPanel();
  drawSplash();
  splashUntil = millis() + 1500;
  Serial.printf("[DISP] %s at 0x%02X on I2C %d/%d\n",
                controller(), i2cAddr, PIN_I2C_SDA, PIN_I2C_SCL);

  // Priority 1 (same as loopTask) on core 0: a ~25 ms I²C frame must not
  // sit in front of the keyer task on core 1, nor in front of the host
  // link that loop() services.
  xTaskCreatePinnedToCore(task, "display", 4096, nullptr, 1, nullptr, 0);
}

bool setController(const char* name) {
  bool sh;
  if      (!strcasecmp(name, "sh1106"))  sh = true;
  else if (!strcasecmp(name, "ssd1306")) sh = false;
  else return false;
  if (sh == useSh1106) return true;
  useSh1106 = sh;
  oled = sh ? (U8G2*)&panelSh1106 : (U8G2*)&panelSsd1306;
  if (i2cAddr) startPanel();   // re-init the newly selected driver
  return true;
}

const char* controller() { return useSh1106 ? "sh1106" : "ssd1306"; }

bool    present()  { return i2cAddr != 0; }
uint8_t address()  { return i2cAddr; }
void    setEnabled(bool en) { cfgEnabled = en; }
bool    enabled()  { return cfgEnabled && i2cAddr != 0; }

}  // namespace Display
