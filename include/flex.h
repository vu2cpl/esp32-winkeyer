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

// How long to hold the transmitter after the last element before "xmit 0".
// This is the Flex path's own tail, and it is a SEPARATE transmitter from
// the keyer's: the keyer's tail releases the local PTT line (GPIO32, still
// live on this backend for an amp or sequencer), this one releases the
// radio. Both must be set together or the control silently does nothing to
// whichever one you are listening to. Settings::apply() keeps them in step.
void     setPttTailMs(uint16_t ms);
uint16_t pttTailMs();

// True while we are holding the radio's transmitter (xmit 1 sent, xmit 0
// not yet). Distinct from the local PTT line, which is the keyer's.
bool     transmitting();

// Whether to assert PTT (xmit 1) around keying. With break-in/QSK the
// radio can switch T/R off the key edge alone, in which case asserting
// PTT ourselves may suppress the carrier. Runtime-switchable so this can
// be settled by ear rather than by reflashing.
void setUseXmit(bool on);
bool useXmit();

// Whether to issue "client bind" to the GUI client. Binding is documented
// as required, but it also makes us a distinct client the radio may treat
// as competing for the transmitter — switchable so it can be tested.
void setBind(bool on);
bool bindEnabled();

// "ptt" (FlexRadio wiki) or "key" (MORCONI). Both are accepted by the
// radio; only a power meter can say which one keys.
void        setKeyVerb(const char* verb);
const char* keyVerb();

// True when the radio has a slice in use and in CW mode. Without both the
// radio transmits nothing and reports no error at all.
bool sliceReady();

// Measured delay between handing the radio text and it starting to
// transmit — the network hop plus the radio's own CW start. 0 until a
// transmission has been timed. Used to align the local sidetone copy.
uint16_t startLatencyMs();

// Operator-facing reason the radio will not transmit CW, or "" when it
// will (or when not connected — that has its own indicator). Caller's
// buffer, as the display task and the web handler run on different cores.
enum WarnForm : uint8_t {
  WARN_LONG,    // web page sentence
  WARN_SHORT,   // <= 18 chars: OLED title, 20x4 LCD row
  WARN_TINY,    // <= 16 chars: 16x2 LCD row
};
void sliceWarning(char* out, size_t n, WarnForm form);

// LAN scan. Discovery is a UDP broadcast and never leaves the keyer's own
// subnet, so a radio on another segment is invisible to it. The command API
// is plain TCP, which a router forwards like anything else: scanStart()
// sweeps one /24 for port 4992 in a background task and asks whatever
// answers what it is. Read-only on the radio — it opens a connection,
// reads the greeting, asks "info", and closes.
struct ScanHit {
  char ip[16];
  char model[24];
  char name[32];     // radio nickname, else callsign
};
bool    scanStart(const char* prefix);   // "192.168.1"; "" = keyer's own /24
bool    scanRunning();
uint8_t scanTried();                     // hosts done, 0..254
String  scanNet();                       // the /24 being / last scanned
String  scanError();                     // "" unless the sweep gave up
uint8_t scanHits(ScanHit* out, uint8_t max);

void send(const char* text);   // queue text for transmission (cwx send)
void clear();                  // cwx clear
void setWpm(uint8_t wpm);      // cwx wpm
int  pending();                // characters queued but not yet keyed

}  // namespace Flex
