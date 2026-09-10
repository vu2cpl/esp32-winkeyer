#pragma once

// ============================================================
//  ESP32 WinKeyer — K1EL WinKeyer protocol engine
//
//  Transport-agnostic: bytes arrive via feed() together with the
//  sink to answer on, so the same engine serves the WiFi TCP
//  bridge and the wired USB serial port. Protocol by Steve K1EL.
// ============================================================

#include <Arduino.h>

// Where buffered text is actually keyed.
enum WkBackend : uint8_t {
  WK_BACKEND_LOCAL = 0,   // local keyer core → GPIO key/PTT
  WK_BACKEND_FLEX  = 1,   // FlexRadio over the network, via "cwx send"
};

namespace WinKeyer {

typedef void (*WriteFn)(const uint8_t* data, size_t len);

void begin();

// Feed one received byte. `sink` is how replies for this transport are
// written; the most recent caller owns the engine's output.
void feed(uint8_t b, WriteFn sink);

void poll();          // pump the send buffer, emit status/pot changes

bool hostOpen();

// What the host asked for. Echo is mode-register bit 2: if a logger never
// sets it there is no echo, and that is the spec, not a fault — so make it
// visible rather than a guess.
// Sidetone for buffered text on a network backend, where the radio — not
// this keyer — generates the CW. Off makes the keyer silent while the rig
// transmits, which is what a bare "cwx send" path does.
void setMonitor(bool on);
bool monitor();

uint8_t modeRegister();
bool    echoEnabled();
void closeHost();     // drop host mode (transport disconnected)

void      setBackend(WkBackend b);
WkBackend getBackend();

}  // namespace WinKeyer
