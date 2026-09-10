#pragma once

// ============================================================
//  ESP32 WinKeyer — GPIO map (classic ESP32 devkit, esp32dev)
//
//  Chosen to avoid strapping pins (0,2,5,12,15) and boot-glitch
//  outputs. Pot must be on ADC1 (GPIO32-39): WiFi disables ADC2.
//  Paddle jack convention: tip = dit, ring = dah (software-swappable).
// ============================================================

#define PIN_PADDLE_DIT   25   // input, internal pullup, paddle closes to GND
#define PIN_PADDLE_DAH   26   // input, internal pullup, paddle closes to GND
#define PIN_KEY_OUT      33   // active high → NPN/optocoupler to rig KEY
#define PIN_PTT_OUT      32   // active high → NPN/optocoupler to rig PTT
#define PIN_SIDETONE      4   // LEDC PWM → passive piezo
#define PIN_SPEED_POT    34   // ADC1_CH6, input-only; 10 k pot wiper, 3V3–GND
#define PIN_STATUS_LED    2   // onboard LED

// I²C — SSD1306 OLED. These are the ESP32's default I²C pins and every
// OLED library assumes them, which is why the OTRSP block was moved off
// them (2026-09-10) rather than the display being asked to move.
#define PIN_I2C_SDA      21
#define PIN_I2C_SCL      22

#define PIN_KEY_OUT2     18   // radio 2 KEY — same drive as radio 1
#define PIN_PTT_OUT2     19   // radio 2 PTT
#define PIN_FSK_OUT      27   // RTTY FSK keying line; mark = idle (invertible)

// The OTRSP/SO2R reservation that used to hold 16, 17, 27, 14, 13 and 5 is
// GONE (2026-09-11): there is no SO2R plan for this box, and holding six
// pins for work that was never going to start had begun to squeeze real
// features onto a resistor ladder. SO2R lives in ~/projects/SO2R box.
//
// Free: 13, 14, 16, 17, 23. Input-only spares: 35, 36, 39 (no
// internal pull-ups on those — a button there needs an external one).
//
// Avoid GPIO 5, 12 and 15: strapping pins, sampled at boot and pulsed on
// reset, so they make unreliable outputs and can stop the board booting.
// GPIO 6-11 are the SPI flash and are unusable. On a WROVER module 16/17
// are the PSRAM lines — fine on this WROOM-based D0WD-V3 devkit, but check
// before reusing them on a different board.
