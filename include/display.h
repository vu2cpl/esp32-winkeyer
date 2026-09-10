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

void begin();            // probe + splash; safe to call with no panel wired
bool present();          // a panel answered on the bus
uint8_t address();       // 7-bit address it answered on, 0 if none
void setEnabled(bool en);// blank the panel without unwiring it
bool enabled();

// "sh1106" (default) or "ssd1306". Takes effect immediately — no reflash,
// so a mis-set panel is fixed from the CLI or the web page.
bool        setController(const char* name);
const char* controller();

}  // namespace Display
