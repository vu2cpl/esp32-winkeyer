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
// Send text the way the CURRENT backend requires: to the radio on the Flex
// path (with local sidetone if monitoring), to the keyer on the local path.
// Anything originating text — the CLI, the web page, message memories —
// must go through here. Pushing straight to Keyer::sendChar() makes
// sidetone and no RF on the Flex backend, because those elements are
// withheld from the key hook to stop the radio being keyed twice.
void sendText(const char* text);

// Stop everything queued and playing, here and on the radio — the page's
// STOP. Same internals as the host's clear-buffer command (0x0A).
void abort();
void setMonitor(bool on);
bool monitor();

// Hold the local sidetone copy of radio-generated text by this many ms, so
// it lines up with the air instead of running ahead of it. 0 = no delay.
// 0xFFFF means "auto": follow Flex::startLatencyMs().
void     setMonitorDelayMs(uint16_t ms);
uint16_t monitorDelayMs();      // the setting (0xFFFF = auto)
uint16_t monitorDelayNowMs();   // what is actually being applied

// Echo of characters sent on the PADDLE (mode register bit 6), so a logger
// captures hand-sent text. 0 off, 1 forced on, 2 follow the host — RUMlogNG
// never sets the bit, hence the override.
void    setPaddleEcho(uint8_t mode);
uint8_t paddleEcho();
bool    paddleEchoActive();

uint8_t modeRegister();
// Diagnostics for the two commands whose bit layout differs between
// WinKeyer revisions: the last pin-configuration byte (-1 if a host has
// never sent one) and the last load-defaults payload as hex. Both surface
// in /api/state, because the console cannot be read while the session that
// sends them owns the wire.
int16_t     lastPinCfg();
const char* lastDefaults();
bool    echoEnabled();
void closeHost();     // drop host mode (transport disconnected)

void      setBackend(WkBackend b);
WkBackend getBackend();

}  // namespace WinKeyer
