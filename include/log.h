#pragma once

// ============================================================
//  ESP32 WinKeyer — console logging
//
//  The console and the WinKeyer host protocol share one serial
//  port. Once a logger opens a host session, every byte on that
//  wire belongs to the protocol: a stray "[FLEX] ..." line is not
//  a log message to the host, it is protocol data, and it lands in
//  the logger's CW window as garbage text.
//
//  So all console output goes through here, and it is muted for
//  the duration of a host session. Nothing is lost that matters —
//  the web page and MQTT report the same state, and neither shares
//  the serial port.
// ============================================================

#include <Arduino.h>

namespace Log {

void setMuted(bool muted);   // true while serial carries the WK protocol
bool muted();

void printf(const char* fmt, ...);
void println(const char* s = "");

}  // namespace Log
