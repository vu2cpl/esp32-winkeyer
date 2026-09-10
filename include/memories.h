#pragma once

// ============================================================
//  ESP32 WinKeyer — message memories
//
//  Six slots of canned text in NVS, played through whichever
//  backend is current. No GPIO cost: they are triggered from the
//  web page, the CLI or the host, and front-panel buttons can be
//  wired to them later without changing any of this.
//
//  %C expands to your callsign so the same memory works when the
//  call changes for a contest or a portable operation.
// ============================================================

#include <Arduino.h>

namespace Memories {

static const uint8_t COUNT   = 6;
static const size_t  MAX_LEN = 100;

void   begin();
bool   set(uint8_t slot, const char* text);   // 1..COUNT; "" clears
String get(uint8_t slot);
bool   play(uint8_t slot);                    // false if empty or out of range

void   setCall(const char* call);
String call();

}  // namespace Memories
