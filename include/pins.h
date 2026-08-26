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
#define PIN_SIDETONE      4   // LEDC PWM → piezo / small speaker
#define PIN_SPEED_POT    34   // ADC1_CH6, input-only; pot wiper across 3V3–GND
#define PIN_STATUS_LED    2   // onboard LED

// Reserved for the future OTRSP/SO2R phase — do not assign:
//   16, 17 (UART2 RX/TX), 18, 19, 21, 22, 23
