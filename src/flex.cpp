// ============================================================
//  ESP32 WinKeyer — FlexRadio (SmartSDR) backend
//
//  Discovery: the radio broadcasts an ASCII key=value payload.
//  Firmware after v1.1.3 wraps it in a VITA-49 packet on UDP
//  4991; older firmware uses a proprietary format on UDP 4992.
//  Both carry the same "key=value" text, so we listen on both
//  ports and scan the datagram for the fields we need rather
//  than parsing two different headers.
//
//  Command API: TCP 4992, line oriented. We send
//  "C<seq>|<command>"; the radio answers "R<seq>|<hex>|<msg>"
//  and pushes "S<handle>|..." status lines we subscribe to.
//
//  Keying uses "cwx send <text>" — a literal space does not
//  survive the command parser, so spaces go as ASCII 0x7F and
//  the radio translates them back.
//
//  Deliberately NOT implemented: real-time paddle keying over
//  the network. Element timing sent packet-by-packet inherits
//  WiFi jitter; paddles stay on the local key output.
// ============================================================

#include "flex.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include "config.h"

namespace {

WiFiUDP    udpNew, udpOld;
WiFiClient tcp;
Preferences prefs;

bool     cfgEnabled = false;
String   cfgManualIp;

String   foundIp, foundModel, foundNick;
uint32_t lastDiscovery = 0;
uint32_t lastConnectTry = 0;
uint32_t seq = 1;

String   rxLine;
String   radioHandle;
bool     subscribed = false;

long     queuedIdx = 0;      // index returned by the last "cwx send"
long     sentIdx   = 0;      // index reported by "cwx sent="
uint8_t  cfgWpm    = 20;
uint32_t busyUntil = 0;      // backstop: see pending()

// Rough time for the radio to key `n` characters, used only as an upper
// bound. ~12 dit-units per character is generous for plain text; the
// point is never to expire early, only to guarantee we expire at all.
uint32_t estimateMs(size_t n) {
  uint32_t unit = 1200 / (cfgWpm ? cfgWpm : 20);
  return (uint32_t)n * 12 * unit;
}

// Extract "key=value" from a discovery datagram, honouring token
// boundaries so "ip=" does not match inside "serial_ip=".
String field(const String& s, const char* key) {
  String want = String(key) + "=";
  int from = 0;
  while (true) {
    int i = s.indexOf(want, from);
    if (i < 0) return "";
    if (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\0') {
      int j = i + want.length();
      int e = j;
      while (e < (int)s.length() && s[e] != ' ' && s[e] >= 0x20) e++;
      return s.substring(j, e);
    }
    from = i + 1;
  }
}

void handleDiscovery(WiFiUDP& udp) {
  int len = udp.parsePacket();
  if (len <= 0) return;
  char pkt[600];
  int n = udp.read(pkt, sizeof(pkt) - 1);
  if (n <= 0) return;
  pkt[n] = '\0';
  // Header bytes are binary in the VITA-49 form; replace them so the
  // ASCII payload can be scanned as one string.
  for (int i = 0; i < n; i++) if (pkt[i] < 0x20 && pkt[i] != '\0') pkt[i] = ' ';
  String s(pkt);
  String ip = field(s, "ip");
  if (ip.length() < 7) return;
  if (ip != foundIp) {
    foundIp    = ip;
    foundModel = field(s, "model");
    foundNick  = field(s, "nickname");
    Serial.printf("[FLEX] discovered %s at %s (%s)\n",
                  foundModel.c_str(), foundIp.c_str(), foundNick.c_str());
  }
  lastDiscovery = millis();
}

void sendCmd(const String& cmd) {
  if (!tcp.connected()) return;
  tcp.printf("C%lu|%s\n", (unsigned long)seq++, cmd.c_str());
}

void onLine(const String& line) {
  if (line.length() < 2) return;
  char t = line[0];

  if (t == 'H') {                      // handle assigned on connect
    radioHandle = line.substring(1);
    radioHandle.trim();
    return;
  }
  if (t == 'V') {                      // API version banner
    Serial.printf("[FLEX] api %s\n", line.substring(1).c_str());
    return;
  }
  if (t == 'R') {                      // reply: R<seq>|<hex status>|<message>
    int p1 = line.indexOf('|');
    int p2 = line.indexOf('|', p1 + 1);
    if (p1 < 0) return;
    String status = (p2 > 0) ? line.substring(p1 + 1, p2) : line.substring(p1 + 1);
    String msg    = (p2 > 0) ? line.substring(p2 + 1) : "";
    if (strtoul(status.c_str(), nullptr, 16) != 0) {
      Serial.printf("[FLEX] command error %s (%s)\n", status.c_str(), msg.c_str());
      // A refused command will never be acknowledged, so anything we were
      // waiting on is never going to complete. Drop it rather than leaving
      // the host stuck reading BUSY forever.
      if (queuedIdx > sentIdx) {
        Serial.println("[FLEX] send refused — clearing pending");
        queuedIdx = sentIdx = 0;
        busyUntil = 0;
      }
      return;
    }
    // A successful "cwx send" answers with the buffer index it landed at.
    long v = msg.toInt();
    if (v > 0) queuedIdx = v;
    return;
  }
  if (t == 'S') {                      // status: S<handle>|cwx sent=<n> ...
    int bar = line.indexOf('|');
    if (bar < 0) return;
    String body = line.substring(bar + 1);
    int k = body.indexOf("sent=");
    if (k >= 0) sentIdx = body.substring(k + 5).toInt();
    int e = body.indexOf("erase_stop=");
    if (e >= 0) sentIdx = body.substring(e + 11).toInt();
  }
}

void pollSocket() {
  while (tcp.available()) {
    char c = tcp.read();
    if (c == '\n') { onLine(rxLine); rxLine = ""; }
    else if (c != '\r' && rxLine.length() < 400) rxLine += c;
  }
}

void tryConnect() {
  String ip = cfgManualIp.length() ? cfgManualIp : foundIp;
  if (ip.length() < 7) return;
  if (millis() - lastConnectTry < 5000) return;
  lastConnectTry = millis();

  Serial.printf("[FLEX] connecting to %s:%d… ", ip.c_str(), FLEX_API_PORT);
  if (!tcp.connect(ip.c_str(), FLEX_API_PORT)) {
    Serial.println("failed");
    return;
  }
  Serial.println("ok");
  tcp.setNoDelay(true);
  rxLine = "";
  subscribed = false;
  queuedIdx = sentIdx = 0;
}

}  // namespace

// ── Public API ────────────────────────────────────────────
namespace Flex {

void begin() {
  prefs.begin("flex", false);
  cfgEnabled = prefs.getBool("en", false);
  // isKey() first: getString on a missing key logs an error at E level,
  // which looks like a fault on a fresh board when it is just "unset".
  cfgManualIp = prefs.isKey("ip") ? prefs.getString("ip", "") : String("");
  prefs.end();
  Serial.printf("[FLEX] backend %s%s\n",
                cfgEnabled ? "enabled" : "disabled",
                cfgManualIp.length() ? (", fixed IP " + cfgManualIp).c_str() : "");
}

void poll() {
  if (!cfgEnabled || WiFi.status() != WL_CONNECTED) return;

  static bool listening = false;
  if (!listening) {
    udpNew.begin(FLEX_DISCOVERY_PORT_NEW);
    udpOld.begin(FLEX_DISCOVERY_PORT_OLD);
    listening = true;
  }
  handleDiscovery(udpNew);
  handleDiscovery(udpOld);

  if (!tcp.connected()) { tryConnect(); return; }

  pollSocket();

  if (!subscribed && radioHandle.length()) {
    // No "client program" here: SmartSDR 1.4.0.0 rejects it with
    // 10000002 "unknown client program", and it buys us nothing —
    // the subscription is what actually matters.
    sendCmd("sub cwx all");
    subscribed = true;
    Serial.printf("[FLEX] subscribed (handle %s)\n", radioHandle.c_str());
  }
}

void setEnabled(bool on) {
  cfgEnabled = on;
  prefs.begin("flex", false);
  prefs.putBool("en", on);
  prefs.end();
  if (!on && tcp.connected()) tcp.stop();
}
bool enabled()   { return cfgEnabled; }
bool connected() { return tcp.connected() && subscribed; }

void setManualIp(const char* ip) {
  cfgManualIp = ip;
  prefs.begin("flex", false);
  prefs.putString("ip", cfgManualIp);
  prefs.end();
  if (tcp.connected()) tcp.stop();
}
String manualIp()  { return cfgManualIp; }
String radioIp()   { return cfgManualIp.length() ? cfgManualIp : foundIp; }
String radioModel(){ return foundModel; }

void send(const char* text) {
  if (!connected() || !text || !*text) return;
  String out;
  for (const char* p = text; *p; p++) out += (*p == ' ') ? (char)0x7F : *p;
  sendCmd("cwx send " + out);
  queuedIdx += strlen(text);          // provisional until the reply lands
  busyUntil = millis() + estimateMs(strlen(text)) + 5000;
}

void clear() {
  if (!connected()) return;
  sendCmd("cwx clear");
  queuedIdx = sentIdx = 0;
  busyUntil = 0;
}

void setWpm(uint8_t wpm) {
  cfgWpm = wpm;
  if (!connected()) return;
  sendCmd("cwx wpm " + String(wpm));
}

int pending() {
  long d = queuedIdx - sentIdx;
  if (d <= 0) return 0;
  // The radio reports progress with "cwx sent=", but it will not report
  // anything if it cannot transmit at all — a slice in the wrong mode, an
  // interlock, another client holding the transmitter. Without a backstop
  // the host reads BUSY forever and a logger hangs waiting for the keyer.
  if (busyUntil && (int32_t)(millis() - busyUntil) > 0) {
    Serial.println("[FLEX] no progress from radio — clearing pending "
                   "(slice not in CW mode? another client transmitting?)");
    queuedIdx = sentIdx = 0;
    busyUntil = 0;
    return 0;
  }
  return (int)d;
}

}  // namespace Flex
