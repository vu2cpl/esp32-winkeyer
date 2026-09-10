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

// Reserved for the future OTRSP/SO2R phase — do not assign:
//   16, 17 (UART2 RX/TX), 27, 14, 13, 5 (relay outputs)
// Free: 18, 19, 23. Input-only spares: 35, 36, 39 (straight-key jack,
// command button).
