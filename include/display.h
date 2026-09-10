#pragma once

// ============================================================
//  ESP32 WinKeyer — 128x64 OLED status display (optional hardware)
//
//  128x64 OLED on I²C (GPIO21/22). The panel is optional: begin()
//  probes the bus and quietly stays off if nothing answers, so the
//  same firmware runs on a board with no display wired.
//
//  Two controllers, same geometry: SH1106 (nearly all 1.3" panels)
//  and SSD1306 (nearly all 0.96"). Both answer on the same I²C
//  address, so which one is fitted CANNOT be probed — it is a
//  setting. Guessing wrong is harmless and obvious: an SH1106 driven
//  as an SSD1306 draws shifted 2 px right with a sliver of garbage
//  down the left edge, because its RAM is 132 columns wide.
//
//  Rendering runs in its own low-priority task on core 0. I²C
//  transactions block for ~25 ms per frame, which must never happen
//  inside the 1 kHz keyer task (element timing) or the Arduino loop
//  (the WinKeyer host link).
// ============================================================

#include <Arduino.h>

namespace Display {

// Probe + splash, unless the panel has been disabled — in which case the
// I²C bus is not touched at all. That matters because the probe and init
// run before the saved settings are loaded, so "disabled" previously still
// meant "poke the bus at every boot", which is where a stuck line hangs.
void begin(bool enabled);
bool present();          // a panel answered on the bus
uint8_t address();       // 7-bit address it answered on, 0 if none
void setEnabled(bool en);// blank the panel without unwiring it
bool enabled();

// Scan the whole bus and report every address that answers. Also adopts a
// panel wired up after boot, so a display added to a running board works
// without a reset. Returns the number of devices found.
uint8_t scan();

// "sh1106", "ssd1306", "lcd16x2", "lcd20x4", or "auto" to re-probe the bus.
// Takes effect immediately — no reflash — so a mis-set panel is fixed from
// the CLI or the web page, and swapping an LCD back for the OLED is just
// "auto". The FAMILY is detectable (OLEDs at 0x3C/0x3D, HD44780 backpacks
// at 0x27/0x3F); the geometry of a text panel is not, since a 16x2 and a
// 20x4 are the same chip at the same address.
bool        setController(const char* name);
const char* controller();

}  // namespace Display
