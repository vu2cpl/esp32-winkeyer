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
String   boundClientId;      // GUI client we transmit on behalf of
String   guiHandle;          // ...and its handle, required on every cw key

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

// ── Direct (element-level) keying ─────────────────────────
// The keyer task produces key transitions; it must never block on the
// network, so it only enqueues here and poll() does the socket write.
// This is how Maestro and MORCONI key a Flex over the network.
bool          cfgDirect = false;
bool          logKeying = true;   // chatty during bring-up
QueueHandle_t keyQ = nullptr;
bool          xmitOn = false;     // do we currently hold the transmitter?
bool          keyIsDown = false;
bool          cfgUseXmit = true;  // assert PTT around keying (see setUseXmit)
uint16_t      keyIndex = 0;       // 16-bit sequence counter for cw key
bool          cfgBind = true;     // issue "client bind" to the GUI client
// "key" is what actually produces RF on a 6600 running SmartSDR 4.2.20.
// The FlexRadio wiki documents "cw ptt" for keying and the radio accepts
// it without error, but it did not key here. Switchable via /flex cmd.
const char*   cfgKeyVerb = "key";
bool          sliceInUse = false;
bool          sliceIsCw  = false;
uint32_t      lastWarnMs = 0;
uint32_t      lastKeyMs = 0;
uint32_t      cfgTailMs = 400;    // hold TX this long after the last element

struct KeyEvt { bool down; uint32_t at; };

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
    uint32_t st = strtoul(status.c_str(), nullptr, 16);
    // 0x50001000 is NOT a failure. Per FlexRadio: the command ran fine but
    // the handler neglected to set a result, so the command processor
    // substitutes this code. "cw key" answers this way — treating it as an
    // error is what made direct keying look unsupported.
    if (st == 0x50001000) return;
    if (st != 0) {
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
  if (t == 'S') {                      // status: S<handle>|<object> ...
    int bar = line.indexOf('|');
    if (bar < 0) return;
    String body = line.substring(bar + 1);
    int k = body.indexOf("sent=");
    if (k >= 0) sentIdx = body.substring(k + 5).toInt();
    int e = body.indexOf("erase_stop=");
    if (e >= 0) sentIdx = body.substring(e + 11).toInt();

    // Track whether there is a slice to key on at all. A radio with no
    // slice in use transmits nothing and reports no error, which is an
    // hour of debugging if the keyer stays silent about it.
    if (body.startsWith("slice ")) {
      if (body.indexOf("in_use=0") >= 0) {
        sliceInUse = false;
      } else if (body.indexOf("in_use=1") >= 0) {
        sliceInUse = true;
      }
      int m = body.indexOf("mode=");
      if (m >= 0) {
        int me = m + 5;
        while (me < (int)body.length() && body[me] > ' ') me++;
        sliceIsCw = body.substring(m + 5, me) == "CW";
      }
    }

    // A non-GUI client cannot transmit in its own right — the radio only
    // allows TX in a GUI client's context (with none connected it reports
    // tx_allowed=0, and CWX from an unbound client is refused as though
    // someone else held the transmitter). So bind to the first GUI client
    // we see and send CW on its behalf.
    if (boundClientId.length() == 0 && body.startsWith("client ")) {
      int idPos = body.indexOf("client_id=");
      if (idPos >= 0 && body.indexOf("connected") >= 0) {
        int s = idPos + 10, e2 = s;
        while (e2 < (int)body.length() && body[e2] > ' ') e2++;
        String id = body.substring(s, e2);
        // Skip ourselves: our own handle came back on connect.
        if (id.length() > 8 && !body.startsWith("client 0x" + radioHandle)) {
          boundClientId = id;
          // The handle sits right after "client " and is needed verbatim on
          // every keying command — binding alone is not enough, and without
          // it the radio accepts cw key but produces no RF.
          int hs = body.indexOf("0x");
          if (hs >= 0) {
            int he = hs;
            while (he < (int)body.length() && body[he] > ' ') he++;
            guiHandle = body.substring(hs, he);
          }
          if (cfgBind) {
            sendCmd("client bind client_id=" + id);
            Serial.printf("[FLEX] bound to GUI client %s (handle %s)\n",
                          id.c_str(), guiHandle.c_str());
          } else {
            Serial.printf("[FLEX] GUI client %s (handle %s) — not binding\n",
                          id.c_str(), guiHandle.c_str());
          }
        }
      }
    }
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
  boundClientId = "";
  queuedIdx = sentIdx = 0;
}

}  // namespace

// ── Public API ────────────────────────────────────────────
namespace Flex {

void begin() {
  keyQ = xQueueCreate(64, sizeof(KeyEvt));
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

void keyEvent(bool down) {
  if (!cfgDirect || !keyQ) return;
  KeyEvt e{down, millis()};
  // Called from the keyer task (a task, not an ISR) — zero-tick send so it
  // never blocks; drop rather than stall element timing if it backs up.
  xQueueSend(keyQ, &e, 0);
}

void     setPttTailMs(uint16_t ms) { cfgTailMs = ms; }
uint16_t pttTailMs() { return cfgTailMs; }

void setDirectKeying(bool on) {
  cfgDirect = on;
  if (!on && tcp.connected()) {                      // never leave it keyed
    sendCmd("cw key 0");
    if (xmitOn) { sendCmd("xmit 0"); xmitOn = false; }
  }
}
bool directKeying() { return cfgDirect; }

void setUseXmit(bool on) {
  cfgUseXmit = on;
  if (!on && xmitOn && tcp.connected()) {
    sendCmd("xmit 0");
    xmitOn = false;
  }
}
bool useXmit() { return cfgUseXmit; }

void setKeyVerb(const char* verb) {
  cfgKeyVerb = (verb && !strcasecmp(verb, "key")) ? "key" : "ptt";
}
const char* keyVerb()  { return cfgKeyVerb; }
bool        sliceReady() { return sliceInUse && sliceIsCw; }

void setBind(bool on) {
  cfgBind = on;
  boundClientId = "";        // force re-evaluation on the next client status
  if (tcp.connected()) tcp.stop();
}
bool bindEnabled() { return cfgBind; }

// Drain queued key transitions onto the socket. Called every loop pass.
//
// A bare "cw key" does nothing: the radio only keys for whichever client
// holds the transmitter, and interlock.tx_client_handle stays 0 until one
// asks for it. So this mirrors a hardware keyer — assert PTT (xmit 1) on
// the first element, key the elements, and drop PTT after a tail so the
// transmitter is not held between letters.
void pumpKeying() {
  if (!keyQ || !tcp.connected()) return;

  KeyEvt e;
  while (xQueueReceive(keyQ, &e, 0) == pdTRUE) {
    // Say why nothing will happen, rather than keying into the void. The
    // radio reports no error for either of these — it simply transmits
    // nothing, which is indistinguishable from a broken keyer.
    if (e.down && (!sliceInUse || !sliceIsCw) && millis() - lastWarnMs > 5000) {
      lastWarnMs = millis();
      Serial.printf("[FLEX] warning: %s — the radio will not transmit\n",
                    !sliceInUse ? "no slice in use in SmartSDR"
                                : "the slice is not in CW mode");
    }
    if (e.down && !xmitOn && cfgUseXmit) {
      tcp.printf("C%lu|xmit 1\n", (unsigned long)seq++);
      xmitOn = true;
      if (logKeying) Serial.println("[FLEX] xmit 1 (PTT)");
    }
    // Form taken from MORCONI:
    //   cw key 1 time=0xB85A index=225 client_handle=0x6A2C5ABC
    // time is milliseconds as 16-bit hex, index a decimal counter.
    //
    // client_handle is the GUI client's handle, not ours: the keying is
    // performed in that client's transmit context. (Tried our own handle
    // too — same refusal, and the documentation is explicit that the GUI
    // client's handle is what CWKey wants.)
    //
    // The timestamps let the radio reconstruct element timing rather than
    // keying on packet arrival, so the fist survives the link.
    // Sub-command spelling is contested: FlexRadio's own wiki documents
    // "cw ptt [1|0] time= index=" ("will transition radio between PTT and
    // MOX or key on/off"), while MORCONI's author shows "cw key". Both are
    // accepted by the radio, so which one actually keys is a question for
    // the meter — hence the runtime switch.
    tcp.printf("C%lu|cw %s %d time=0x%04X index=%u client_handle=%s\n",
               (unsigned long)seq++, cfgKeyVerb,
               e.down ? 1 : 0,
               (unsigned)(e.at & 0xFFFF), (unsigned)(keyIndex++ & 0xFFFF),
               guiHandle.length() ? guiHandle.c_str() : "0x0");
    if (logKeying) Serial.printf("[FLEX] cw %s %d\n", cfgKeyVerb, e.down ? 1 : 0);
    keyIsDown = e.down;
    lastKeyMs = millis();
  }

  // Release the transmitter once the operator has stopped sending — but
  // never while the key is still down, or a long element (or tune) would
  // drop PTT out from under itself.
  if (xmitOn && !keyIsDown && lastKeyMs && millis() - lastKeyMs > cfgTailMs) {
    tcp.printf("C%lu|xmit 0\n", (unsigned long)seq++);
    xmitOn = false;
    if (logKeying) Serial.println("[FLEX] xmit 0 (PTT release)");
  }
}

void poll() {
  if (!cfgEnabled || WiFi.status() != WL_CONNECTED) return;
  pumpKeying();

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
    sendCmd("sub client all");     // so we can find a GUI client to bind to
    sendCmd("sub slice all");      // to warn when there is nothing to key on
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
