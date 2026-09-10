#pragma once

// ============================================================
//  ESP32 WinKeyer — RTTY FSK keying line
//
//  Drives a rig's FSK input with Baudot (ITA2) at 45.45 baud:
//  1 start bit (space), 5 data bits LSB first, 1.5 stop bits
//  (mark), mark when idle.
//
//  Bit timing comes from an esp_timer running at the HALF-bit
//  period, which is what makes the 1.5-bit stop exact — it is
//  three half-bits, not one and a half of anything. A 22 ms bit
//  cannot be timed from the 1 ms FreeRTOS tick without 4% jitter,
//  and RTTY decoders notice.
// ============================================================

#include <Arduino.h>

namespace Fsk {

void begin();

bool send(const char* text);   // queue text; false if the buffer is full
void abort();                  // stop at once, drop the queue, release PTT
bool busy();
size_t pending();

// 45.45 (standard RTTY), 50, 75 or 100 baud.
bool  setBaud(float baud);
float baud();

// Some rigs want the line inverted — mark low rather than high. Wrong
// polarity prints as reversed-case gibberish at the far end, not silence.
void setInvert(bool on);
bool invert();

// Send LTRS continuously while the transmitter is up but no text is
// waiting, so the far end stays synchronised between overs.
void setDiddle(bool on);
bool diddle();

}  // namespace Fsk
