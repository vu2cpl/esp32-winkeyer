#pragma once

// ============================================================
//  ESP32 WinKeyer — FlexRadio (SmartSDR) backend
//
//  Finds a 6000/8000-series radio on the LAN via its discovery
//  broadcast, opens the SmartSDR command API on TCP 4992, and
//  keys CW with "cwx send" — the radio generates the element
//  timing, so network jitter never reaches the air.
// ============================================================

#include <Arduino.h>

namespace Flex {

void begin();
void poll();

void setEnabled(bool on);      // persisted in NVS
bool enabled();
bool connected();

void   setManualIp(const char* ip);   // "" = use discovery
String manualIp();
String radioIp();
String radioModel();

// Real-time keying. keyEvent() is safe to call from the keyer task; it only
// queues, and poll() does the network write.
void keyEvent(bool down);
void setDirectKeying(bool on);
bool directKeying();

void send(const char* text);   // queue text for transmission (cwx send)
void clear();                  // cwx clear
void setWpm(uint8_t wpm);      // cwx wpm
int  pending();                // characters queued but not yet keyed

}  // namespace Flex
