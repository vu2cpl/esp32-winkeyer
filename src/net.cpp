// ============================================================
//  ESP32 WinKeyer — WiFi transport
//
//  One client at a time: a keyer with two hosts sending CW at
//  once has no sane behaviour, so a new connection displaces
//  the old one and the WinKeyer host session is reset with it.
// ============================================================

#include "net.h"
#include "log.h"
#include "winkeyer.h"
#include "config.h"
#include <WiFi.h>
#include <ESPmDNS.h>

namespace {

WiFiServer server(WK_TCP_PORT);
WiFiClient client;
bool started = false;
bool mdnsUp  = false;

void tcpSink(const uint8_t* data, size_t len) {
  if (client && client.connected()) client.write(data, len);
}

}  // namespace

namespace Net {

void begin() { /* deferred until WiFi is actually up */ }

void poll() {
  if (WiFi.status() != WL_CONNECTED) {
    if (started) {                      // network dropped — tear the session down
      if (client) { client.stop(); }
      WinKeyer::closeHost();
      server.end();
      started = false;
      mdnsUp = false;
    }
    return;
  }

  if (!started) {
    server.begin();
    server.setNoDelay(true);
    started = true;
    Log::printf("[NET] WinKeyer TCP server on port %d\n", WK_TCP_PORT);
  }

  if (!mdnsUp) {
    if (MDNS.begin(MDNS_HOSTNAME)) {
      MDNS.addService("winkeyer", "tcp", WK_TCP_PORT);
      MDNS.addService("http", "tcp", 80);   // the settings page (src/web.cpp)
      mdnsUp = true;
      Log::printf("[NET] mDNS: %s.local\n", MDNS_HOSTNAME);
    }
  }

  if (server.hasClient()) {
    WiFiClient incoming = server.available();
    if (client && client.connected()) {
      Log::println("[NET] new client — dropping the previous one");
      client.stop();
      WinKeyer::closeHost();
    }
    client = incoming;
    client.setNoDelay(true);
    Log::printf("[NET] client %s connected\n", client.remoteIP().toString().c_str());
  }

  if (client && !client.connected()) {
    Log::println("[NET] client disconnected");
    client.stop();
    WinKeyer::closeHost();
  }

  // Block reads, not byte-at-a-time: see the comment on Flex::pollSocket().
  // Same library, same pbuf double-free if the socket goes away mid-read.
  uint8_t buf[128];
  while (client && client.connected()) {
    int avail = client.available();
    if (avail <= 0) break;
    int n = client.read(buf, avail < (int)sizeof buf ? avail : (int)sizeof buf);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) WinKeyer::feed(buf[i], tcpSink);
  }
}

bool clientConnected() { return client && client.connected(); }

}  // namespace Net
