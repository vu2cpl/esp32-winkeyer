#pragma once

// ============================================================
//  ESP32 WinKeyer — WiFi transport
//
//  Raw TCP server carrying the WinKeyer byte stream, advertised
//  over mDNS as winkeyer.local. A host-side bridge turns the
//  socket into a serial port the logging software can open.
// ============================================================

#include <Arduino.h>

namespace Net {

void begin();          // start mDNS + the WinKeyer TCP server
void poll();           // accept/serve the client, feed the WK engine
bool clientConnected();

}  // namespace Net
