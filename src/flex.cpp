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
#include "log.h"
#include "keyer.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <cstdarg>
#include <Preferences.h>
#include "config.h"

namespace Flex { void sendKeyUp(); }   // defined with the keying code below

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

long     queuedIdx = 0;      // radio buffer index of the LAST character queued
long     sentIdx   = 0;      // index reported by "cwx sent="
uint8_t  cfgWpm    = 20;
// What the radio says its cwx speed is, from "cwx wpm=" status. Not the same
// thing as cfgWpm: SmartSDR's CWX panel (or anything else on the API) can
// change it behind our back, and then the radio keys at one speed while the
// local sidetone copy runs at another. Seen 2026-09-13: radio 30, keyer 25 —
// PTT dropped with the radio while the sidetone ran on ~3 s.
uint8_t  radioWpmVal = 0;
uint32_t busyUntil = 0;      // backstop: see pending()

// Replies to "cwx send" are matched to the send that caused them by sequence
// number. The reply carries the buffer index of the block's FIRST character,
// and "cwx sent=" then counts up one per character (seen 2026-09-13:
// "TEST " -> sent=6799..6803), so the block ends at index + length - 1.
// Taking the bare index as the end made pending() collapse to ~0 as soon as
// the reply landed, and every echo went to the host at once, long before the
// radio had keyed the text. A reply to a send made before a "cwx clear" is
// stale and must not re-arm pending() — that was the ~1 s BUSY a host saw
// after Clear Buffer or a paddle break-in.
// Room for a whole memory's worth: a logger hands text over a character per
// command, faster than the radio answers, and 11 were outstanding at once on
// 2026-09-17. With 8, the oldest were overwritten before their replies came,
// those characters had no known text, and the CW timing learner threw the
// whole message away — so it learned almost only from slowly typed text.
struct CwxSend { uint32_t seq; uint16_t len; char text[64]; };
CwxSend  cwxSends[32];
uint8_t  cwxSendNext = 0;
uint32_t clearSeq = 0;       // replies to commands before this are stale

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
bool     logKeying = false;   // chatty during bring-up
QueueHandle_t keyQ = nullptr;
bool          xmitOn = false;     // do we currently hold the transmitter?
bool          keyIsDown = false;
bool          cfgUseXmit = true;  // assert PTT around keying (see setUseXmit)
uint16_t      keyIndex = 0;       // 16-bit sequence counter for cw key
bool          cfgBind = false;    // issue "client bind" to the GUI client
// "key" is what actually produces RF on a 6600 running SmartSDR 4.2.20.
// The FlexRadio wiki documents "cw ptt" for keying and the radio accepts
// it without error, but it did not key here. Switchable via /flex cmd.
const char*   cfgKeyVerb = "key";
bool          sliceInUse = false;
bool          sliceIsCw  = false;
char          sliceMode[8] = "";  // as the radio names it: "LSB", "DIGU", ...
uint32_t      lastWarnMs = 0;
uint32_t      lastKeyMs = 0;
// What the RADIO says it is doing. Our own bookkeeping can be right while
// the radio is still transmitting — a key-up that never arrived leaves no
// trace on this side — so the interlock is the only honest witness.
bool          radioTx      = false;   // interlock state=TRANSMITTING
bool          radioTxIsCw  = false;   // ...with source=SWCW, i.e. our keying
uint32_t      radioTxSince = 0;
// Last sign of life from the radio's own CW sender: a "cwx sent=" report,
// or the moment we handed it text. While this is fresh the radio IS
// sending — a memory plays with nothing keyed on our side, which must
// never be mistaken for a stuck transmitter.
uint32_t      lastCwxMs    = 0;
// How long the radio takes to START sending after we hand it text: the
// network hop plus its own CW start. Measured from "cwx send" to the
// interlock going TRANSMITTING, and only when the radio was idle first, so
// back-to-back messages do not measure zero. The sidetone copy is held by
// this so it stops running ahead of the air.
uint32_t      cwxSendAt    = 0;
uint16_t      startLatency = 0;

// ── Radio CW timing, per speed ──
// How much longer than 1200/WPM the radio really makes each dit unit when it
// sends CWX, from the times of its "cwx sent=" reports: about 700 µs at 22
// and 25 WPM (2026-09-17). The sidetone copy adds it, so it stays with the air
// through a long message. It is learned per speed, because one percentage
// learned at 25 WPM over-corrected at 5-10 WPM, and kept in NVS so a reboot
// does not start from nothing. Measured only over runs of consecutive
// characters whose text the keyer queued itself, at one speed, with no clear
// in between, and with the next character already in the radio's buffer when
// the one before it ended. Without that last condition, a pause while the
// radio waited for text (typed CW, or a second message just behind the
// first) counted as sending time: by the evening of 2026-09-17 the 25 WPM
// entry had climbed to 1168 µs against about 760 µs really on the air.
struct IdxChar { long idx; char c; uint32_t at; };   // at: millis() it was queued
IdxChar  idxText[256];                 // radio buffer index -> character sent
const uint8_t  CW_WPM_MIN = 5, CW_WPM_MAX = 50;
const uint8_t  CW_N = CW_WPM_MAX - CW_WPM_MIN + 1;
const int16_t  CW_EXTRA_DEFAULT = 700; // µs a unit, until anything is learned
int16_t  cwExtra[CW_N];                // µs a unit, per WPM
int16_t  cwExtraSaved[CW_N];           // as last written to NVS
uint8_t  cwRuns[CW_N];                 // clean runs learned at that WPM (saturates)
bool     cwDirty = false;
uint32_t cwSavedMs = 0;
uint16_t latSaved = 0;                 // start latency as last written to NVS
bool     rateActive = false, rateOk = false;
uint32_t rateT0 = 0, rateLastT = 0;
long     rateLastIdx = 0;
uint32_t rateUnits = 0;
uint8_t  rateWpm = 0;

// Extra µs a unit at this speed: learned if it has been, else interpolated
// between the nearest learned speeds either side, else the default.
int16_t cwExtraFor(uint8_t wpm) {
  if (wpm < CW_WPM_MIN) wpm = CW_WPM_MIN;
  if (wpm > CW_WPM_MAX) wpm = CW_WPM_MAX;
  int i = wpm - CW_WPM_MIN;
  if (cwRuns[i]) return cwExtra[i];
  int lo = -1, hi = -1;
  for (int k = i - 1; k >= 0; k--)   if (cwRuns[k]) { lo = k; break; }
  for (int k = i + 1; k < CW_N; k++) if (cwRuns[k]) { hi = k; break; }
  if (lo < 0 && hi < 0) return CW_EXTRA_DEFAULT;
  if (lo < 0) return cwExtra[hi];
  if (hi < 0) return cwExtra[lo];
  return (int16_t)(cwExtra[lo] + (int32_t)(cwExtra[hi] - cwExtra[lo]) * (i - lo) / (hi - lo));
}
bool          forcedThisTx = false;   // one rescue per stuck transmission
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
    Log::printf("[FLEX] discovered %s at %s (%s)\n",
                  foundModel.c_str(), foundIp.c_str(), foundNick.c_str());
  }
  lastDiscovery = millis();
}

// ── Radio traffic trace ──────────────────────────────────
// Recent lines to and from the radio, stamped, for GET /api/flextrace. The
// radio's reply to a refused "cw key" only ever reached the console, which
// is muted while a logger holds the port, and the Mac cannot see this
// traffic at all (HANDOVER open item 13). Everything sent is kept; of what
// arrives only replies, messages and the interlock/cwx/client statuses —
// slice statuses are long and arrive in bursts whenever the radio is tuned.
// Everything here runs in loop(), so no locking.
const uint16_t FT_N   = 128;
const uint8_t  FT_LEN = 120;
struct FlexTraceEnt { uint32_t ms; char dir; char text[FT_LEN]; };
FlexTraceEnt ftrace[FT_N];
uint16_t ftHead  = 0;
uint32_t ftTotal = 0;

void ftAdd(char dir, const char* text) {
  FlexTraceEnt& e = ftrace[ftHead];
  e.ms  = millis();
  e.dir = dir;
  size_t n = strcspn(text, "\r\n");       // one line, no terminator
  if (n >= FT_LEN) n = FT_LEN - 1;
  memcpy(e.text, text, n);
  e.text[n] = 0;
  ftHead = (ftHead + 1) % FT_N;
  ftTotal++;
}

// Every command to the radio goes through here, so every one is traced.
void txf(const char* fmt, ...) {
  if (!tcp.connected()) return;
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  tcp.print(buf);
  ftAdd('>', buf);
}

void sendCmd(const String& cmd) {
  txf("C%lu|%s\n", (unsigned long)seq++, cmd.c_str());
}

// Exact key lookup in a space-separated "k=v k=v" status body.
//
// indexOf("mode=") is NOT good enough: a slice status also carries
// agc_mode=, rfgain_mode= and tx_ant_mode=, so a substring search returns
// whichever appears first and the slice mode then tracks inconsistently —
// switching to USB was seen, switching back to CW was not. Morconi's
// bridge tokenises on spaces and splits each token at '=' for this reason.
bool kv(const String& body, const char* key, String& out) {
  String k = String(key) + "=";
  int i = body.startsWith(k) ? 0 : body.indexOf(" " + k);
  if (i < 0) return false;
  if (i) i++;                       // step over the space
  int v = i + k.length();
  int e = v;
  while (e < (int)body.length() && body[e] > ' ') e++;
  out = body.substring(v, e);
  return true;
}

void rateFinish() {
  if (rateActive && rateOk && rateUnits >= 40 &&
      rateWpm >= CW_WPM_MIN && rateWpm <= CW_WPM_MAX) {
    uint32_t ms = rateLastT - rateT0;
    int32_t unitUs = (int32_t)((uint64_t)ms * 1000 / rateUnits);
    int32_t extra  = unitUs - (int32_t)(1200000UL / rateWpm);
    // A clean run is within a few percent of nominal, and the radio adds well
    // under 2 ms a unit at any speed (735-779 µs measured); anything else was
    // interrupted or not what it seemed.
    int32_t limit = (int32_t)(1200000UL / rateWpm) / 20;          // 5 %
    if (extra > -limit && extra < limit && extra < 2000) {
      int i = rateWpm - CW_WPM_MIN;
      int16_t old = cwExtra[i];
      // Weighted by length. The radio's "sent=" reports wobble by up to
      // ~100 ms, and a run of 40-90 units (a typed word) turns that into
      // 300-700 µs of error where a 234-unit CQ gives ~120. With every run
      // moving the entry by a quarter, one typed word undid two good CQs:
      // 25 WPM wandered 770-850 µs against ~740 from CQs (2026-09-17).
      // A CQ-length run (240 units) still moves it a quarter; 44 units, a
      // twentieth.
      uint32_t w = rateUnits < 240 ? rateUnits : 240;             // of 960
      cwExtra[i] = cwRuns[i] ? (int16_t)(old + (extra - old) * (int32_t)w / 960)
                             : (int16_t)extra;
      if (cwRuns[i] < 255) cwRuns[i]++;
      // Against what is in flash, not the previous value: smoothing moves an
      // entry in small steps, and 872 -> 774 µs in steps under 50 was never
      // written, so every reboot (and opening RUMlogNG reboots the keyer)
      // brought back 872 and the drift (2026-09-17).
      if (cwRuns[i] == 1 || abs(cwExtra[i] - cwExtraSaved[i]) > 50) cwDirty = true;
      Log::printf("[FLEX] radio CW at %u WPM: %d us/unit extra (%u units in %u ms), table %d\n",
                  rateWpm, (int)extra, (unsigned)rateUnits, (unsigned)ms, cwExtra[i]);
    }
  }
  rateActive = false;
}

void cwLoad() {
  for (int i = 0; i < CW_N; i++) { cwExtra[i] = CW_EXTRA_DEFAULT; cwRuns[i] = 0; }
  if (prefs.isKey("cwx")) prefs.getBytes("cwx", cwExtra, sizeof cwExtra);
  if (prefs.isKey("cwn")) prefs.getBytes("cwn", cwRuns, sizeof cwRuns);
  memcpy(cwExtraSaved, cwExtra, sizeof cwExtra);
  // isKey() first, as for "ip": a missing key is not an error on a fresh board.
  latSaved = prefs.isKey("lat") ? prefs.getUShort("lat", 0) : 0;
  startLatency = latSaved;
}

// Rarely: only when something changed enough, and at most once a minute.
void cwSaveIfDue() {
  bool latDue = startLatency && abs((int)startLatency - (int)latSaved) > 20;
  if (!cwDirty && !latDue) return;
  if (cwSavedMs && millis() - cwSavedMs < 60000) return;
  cwSavedMs = millis();
  prefs.begin("flex", false);
  if (cwDirty) {
    prefs.putBytes("cwx", cwExtra, sizeof cwExtra);
    prefs.putBytes("cwn", cwRuns, sizeof cwRuns);
    memcpy(cwExtraSaved, cwExtra, sizeof cwExtra);
    cwDirty = false;
  }
  if (latDue) { prefs.putUShort("lat", startLatency); latSaved = startLatency; }
  prefs.end();
}

// One "cwx sent=" report. Consecutive reports extend the run; each adds the
// length of the character just finished.
void rateSent(long idx) {
  uint32_t now = millis();
  uint8_t wpm = radioWpmVal ? radioWpmVal : cfgWpm;
  bool next = rateActive && idx == rateLastIdx + 1 && now - rateLastT < 3000 &&
              wpm == rateWpm;
  if (!next) {
    rateFinish();
    rateActive = true; rateOk = true;
    rateT0 = rateLastT = now; rateLastIdx = idx; rateUnits = 0; rateWpm = wpm;
    return;
  }
  const IdxChar& e = idxText[idx & 0xFF];
  // Queued too close to the end of the character before it: the radio may
  // have sat idle waiting for it, which is not sending time. Keep the run up
  // to the previous character and start a new one here.
  if (e.idx == idx && e.at + 100 > rateLastT) {
    rateFinish();
    rateActive = true; rateOk = true;
    rateT0 = rateLastT = now; rateLastIdx = idx; rateUnits = 0; rateWpm = wpm;
    return;
  }
  uint8_t u = 0;
  if (e.idx == idx) u = (e.c == ' ') ? 4 : Keyer::charUnits(e.c);
  if (!u) rateOk = false;                // not text we queued, or no Morse
  rateUnits += u;
  rateLastT = now;
  rateLastIdx = idx;
}

void onLine(const String& line) {
  if (line.length() < 2) return;
  char t = line[0];
  if (t == 'R' || t == 'M' ||
      (t == 'S' && (line.indexOf("|interlock ") > 0 ||
                    line.indexOf("|cwx ") > 0 ||
                    line.indexOf("|client ") > 0)))
    ftAdd('<', line.c_str());

  if (t == 'H') {                      // handle assigned on connect
    radioHandle = line.substring(1);
    radioHandle.trim();
    return;
  }
  if (t == 'V') {                      // API version banner
    Log::printf("[FLEX] api %s\n", line.substring(1).c_str());
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
      Log::printf("[FLEX] command error %s (%s)\n", status.c_str(), msg.c_str());
      // A refused command will never be acknowledged, so anything we were
      // waiting on is never going to complete. Drop it rather than leaving
      // the host stuck reading BUSY forever.
      if (queuedIdx > sentIdx) {
        Log::println("[FLEX] send refused — clearing pending");
        queuedIdx = sentIdx = 0;
        busyUntil = 0;
      }
      return;
    }
    // A successful "cwx send" answers with the buffer index its first
    // character landed at. Only replies to our own live sends count: other
    // commands answer with numbers too.
    uint32_t rseq = strtoul(line.c_str() + 1, nullptr, 10);
    if (rseq < clearSeq) return;
    for (auto& e : cwxSends) {
      if (e.len && e.seq == rseq) {
        long v = msg.toInt();
        if (v > 0) {
          long last = v + (long)e.len - 1;
          if (last > queuedIdx) queuedIdx = last;
          for (uint16_t i = 0; i < e.len && i < sizeof(e.text) - 1; i++)
            idxText[(v + i) & 0xFF] = { v + (long)i, e.text[i], millis() };
        }
        e.len = 0;
        break;
      }
    }
    return;
  }
  if (t == 'S') {                      // status: S<handle>|<object> ...
    int bar = line.indexOf('|');
    if (bar < 0) return;
    String body = line.substring(bar + 1);
    int k = body.indexOf("sent=");
    if (k >= 0) { sentIdx = body.substring(k + 5).toInt(); lastCwxMs = millis(); }
    int e = body.indexOf("erase_stop=");
    if (e >= 0) { sentIdx = body.substring(e + 11).toInt(); lastCwxMs = millis(); }
    if (body.startsWith("cwx ")) {
      String v;
      if (kv(body, "wpm", v) && v.toInt() > 0) radioWpmVal = (uint8_t)v.toInt();
      if (kv(body, "sent", v)) rateSent(v.toInt());
      if (body.indexOf("erase") > 0) rateActive = false;   // a cut run proves nothing
    }

    // Track whether there is a slice to key on at all. A radio with no
    // slice in use transmits nothing and reports no error, which is an
    // hour of debugging if the keyer stays silent about it.
    if (body.startsWith("interlock ")) {
      String v;
      if (kv(body, "state", v)) {
        bool was = radioTx;
        radioTx = (v == "TRANSMITTING");
        if (radioTx && !was) {
          radioTxSince = millis();
          forcedThisTx = false;
          // The measurement: text out → transmitter on. Ignore absurd gaps,
          // which mean the transmission was not the one we queued.
          if (cwxSendAt) {
            uint32_t d = millis() - cwxSendAt;
            cwxSendAt = 0;
            if (d <= 3000) {
              // Smoothed: one slow packet should not move the sidetone.
              startLatency = startLatency ? (uint16_t)((startLatency * 3 + d) / 4)
                                          : (uint16_t)d;
            }
          }
        }
        if (!radioTx) forcedThisTx = false;
      }
      if (kv(body, "source", v)) radioTxIsCw = (v == "SWCW");
    }

    if (body.startsWith("slice ")) {
      String v;
      if (kv(body, "in_use", v)) sliceInUse = (v == "1");
      if (kv(body, "mode",   v)) {
        sliceIsCw = (v == "CW");
        strlcpy(sliceMode, v.c_str(), sizeof sliceMode);
      }
    }

    // A non-GUI client cannot transmit in its own right: the radio only
    // allows TX in a GUI client's context (with none connected it reports
    // tx_allowed=0). So key on behalf of a GUI client, by putting its handle
    // on every "cw key". The radio accepts "cwx send" and "cw key" from us
    // WITHOUT "client bind" (measured 2026-09-17, HANDOVER item 13).
    //
    // The client has to be FOLLOWED, not captured once. A GUI client that
    // restarts comes back with a new handle, and the radio answers a cw key
    // under the old one with 0 and drops it: PTT, no CW, no error.
    //   client 0x7D62C96C connected local_ptt=1 client_id=9BC7… program=…
    //   client 0x7D62C96C disconnected forced=0 … duplicate_client_id=0
    // The state word is matched exactly: "disconnected" contains
    // "connected", and the disconnect line carries a "duplicate_client_id=".
    if (body.startsWith("client 0x")) {
      int hs = 7;                                   // at "0x"
      int he = body.indexOf(' ', hs);
      if (he < 0) return;
      String handle = body.substring(hs, he);
      int ws = he + 1, we = body.indexOf(' ', ws);
      String state = body.substring(ws, we < 0 ? body.length() : we);

      if (state == "disconnected") {
        if (handle == guiHandle) {
          Log::printf("[FLEX] GUI client %s (handle %s) left\n",
                      boundClientId.c_str(), guiHandle.c_str());
          boundClientId = "";
          guiHandle = "";
          // Another GUI client may already be connected, and the radio only
          // reports a client when it changes; ask for the current list.
          sendCmd("sub client all");
        }
        return;
      }
      if (state != "connected") return;

      String id;
      if (!kv(body, "client_id", id) || id.length() <= 8) return;   // not a GUI client
      if (handle == "0x" + radioHandle) return;                      // ourselves

      // A new GUI client when we have none, or ours again under a new
      // handle (the disconnect may not have been seen, e.g. across our own
      // reconnect). A second GUI client never displaces the first.
      bool adopt   = boundClientId.length() == 0;
      bool rehandle = !adopt && id == boundClientId && handle != guiHandle;
      if (!adopt && !rehandle) return;

      boundClientId = id;
      guiHandle = handle;
      if (cfgBind) {
        // Off by default: binding to a GUI client that has just connected
        // left the radio's CW generator wedged, so paddle keying and CWX
        // both transmitted at 0 W until a stalled "cwx clear" released it.
        // Kept as a switch so it can be tested again.
        sendCmd("client bind client_id=" + id);
        Log::printf("[FLEX] bound to GUI client %s (handle %s)\n",
                    id.c_str(), guiHandle.c_str());
      } else {
        Log::printf("[FLEX] keying for GUI client %s (handle %s)\n",
                    id.c_str(), guiHandle.c_str());
      }
    }
  }
}

// Read in BLOCKS, never a byte at a time. Every tcp.read() is a separate
// lwip_recv, and this Arduino core (2.0.17) can double-free a pbuf inside
// WiFiClientRxBuffer when the socket is torn down mid-read. That is a real
// crash, not a theory: 2026-09-12, "assert failed: pbuf_free ... p->ref > 0",
// backtrace WiFiClient::read() <- Flex::poll() <- loop(), after the radio's
// status burst that follows a memory. One read per 256 bytes instead of one
// per byte shrinks the window enormously and costs nothing. It does NOT fix
// the library — a core upgrade would — so treat a recurrence as that bug.
void pollSocket() {
  uint8_t buf[256];
  while (tcp.connected()) {
    int avail = tcp.available();
    if (avail <= 0) break;
    int n = tcp.read(buf, avail < (int)sizeof buf ? avail : (int)sizeof buf);
    if (n <= 0) break;                 // closed or failed under us
    for (int i = 0; i < n; i++) {
      char c = (char)buf[i];
      if (c == '\n') { onLine(rxLine); rxLine = ""; }
      else if (c != '\r' && rxLine.length() < 400) rxLine += c;
    }
  }
}

void tryConnect() {
  String ip = cfgManualIp.length() ? cfgManualIp : foundIp;
  if (ip.length() < 7) return;
  if (millis() - lastConnectTry < 5000) return;
  lastConnectTry = millis();

  Log::printf("[FLEX] connecting to %s:%d… ", ip.c_str(), FLEX_API_PORT);
  // WITH A TIMEOUT. WiFiClient's default runs to tens of seconds, and this
  // is called from loop() — which also carries the host link, the web page
  // and the key drain. A radio that has gone unreachable (a WiFi wobble is
  // enough) then parks loop() long enough to trip the 30 s task watchdog,
  // and the board resets mid-over with the radio left transmitting. Seen
  // 2026-09-12: reset reason "task WATCHDOG". The 5 s retry spacing above
  // is what paces the attempts; this only bounds each one.
  if (!tcp.connect(ip.c_str(), FLEX_API_PORT, 1500)) {
    Log::println("failed");
    return;
  }
  Log::println("ok");
  tcp.setNoDelay(true);
  rxLine = "";
  subscribed = false;
  boundClientId = "";
  queuedIdx = sentIdx = 0;
  radioWpmVal = 0;             // "sub cwx all" reports it again
  // A link that dropped mid-element never delivered its key-up, and the
  // radio is still keyed. Say so now. Only the key-up: "xmit 0" or a
  // "cwx clear" here could cut short a transmission from SmartSDR or MSHV.
  keyIsDown = false;
  xmitOn    = false;
  Flex::sendKeyUp();
  // That key-up went under the last handle on purpose: it is the one a
  // stuck key was sent under. From here the GUI client is re-learned from
  // "sub client all", so no keying goes out under a handle that may be gone.
  guiHandle = "";
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
  cwLoad();
  prefs.end();
  Log::printf("[FLEX] backend %s%s\n",
                cfgEnabled ? "enabled" : "disabled",
                cfgManualIp.length() ? (", fixed IP " + cfgManualIp).c_str() : "");
}

void keyEvent(bool down) {
  if (!cfgDirect || !keyQ) return;
  KeyEvt e{down, millis()};
  // Called from the keyer task, so this must never block: a zero-tick send.
  if (xQueueSend(keyQ, &e, 0) == pdTRUE) return;

  // The queue is full. A dropped key-DOWN costs one element and is
  // survivable. A dropped key-UP is not: keyIsDown then stays true, the
  // release below is gated on it, and the RADIO IS LEFT TRANSMITTING.
  // So an up event always gets in, evicting the oldest entry if it must.
  if (down) return;
  KeyEvt discard;
  xQueueReceive(keyQ, &discard, 0);
  xQueueSend(keyQ, &e, 0);
}

void     setPttTailMs(uint16_t ms) { cfgTailMs = ms; }
uint16_t pttTailMs() { return cfgTailMs; }
bool     transmitting() { return xmitOn; }

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
uint16_t    startLatencyMs() { return startLatency; }
int16_t     cwExtraUs(uint8_t wpm) { return cwExtraFor(wpm); }
void cwTableJson(JsonArray a) {
  for (int i = 0; i < CW_N; i++) {
    JsonObject o = a.createNestedObject();
    o["wpm"] = CW_WPM_MIN + i;
    o["us"]  = cwExtraFor(CW_WPM_MIN + i);
    o["runs"] = cwRuns[i];               // 0 = interpolated or default
  }
}
void cwTableReset() {
  for (int i = 0; i < CW_N; i++) { cwExtra[i] = CW_EXTRA_DEFAULT; cwRuns[i] = 0; }
  memcpy(cwExtraSaved, cwExtra, sizeof cwExtra);
  prefs.begin("flex", false);
  prefs.remove("cwx"); prefs.remove("cwn"); prefs.remove("lat");
  prefs.end();
  latSaved = 0;
}
uint8_t     radioWpm() { return radioWpmVal; }
bool        radioTransmitting() { return radioTx; }

void sliceWarning(char* out, size_t n, WarnForm form) {
  out[0] = '\0';
  if (!connected() || sliceReady()) return;
  if (!sliceInUse) {
    snprintf(out, n, form == WARN_LONG
                       ? "No slice in use in SmartSDR — the radio will not transmit."
                       : "NO SLICE IN USE");
    return;
  }
  const char* m = sliceMode[0] ? sliceMode : "?";
  switch (form) {
    case WARN_LONG:
      snprintf(out, n, "Radio slice is in %s, not CW — memories will not "
                       "transmit. Switch the slice to CW in SmartSDR.", m);
      break;
    case WARN_SHORT:
      snprintf(out, n, "SLICE %s, NOT CW", m);
      break;
    case WARN_TINY:   // 16 columns: "SLICE USB NOT CW" fits a 3-letter mode
      snprintf(out, n, strlen(m) <= 3 ? "SLICE %s NOT CW" : "%s: NOT CW", m);
      break;
  }
}

void setBind(bool on) {
  cfgBind = on;
  boundClientId = "";        // force re-evaluation on the next client status
  guiHandle = "";
  if (tcp.connected()) tcp.stop();
}
bool bindEnabled() { return cfgBind; }
String guiClientHandle() { return guiHandle; }

void traceDump(Print& out) {
  uint16_t n = ftTotal < FT_N ? (uint16_t)ftTotal : FT_N;
  uint16_t i = (ftHead + FT_N - n) % FT_N;
  out.printf("# total %lu, showing %u, now %lu ms, > keyer to radio, < radio to keyer, # why\n",
             (unsigned long)ftTotal, n, (unsigned long)millis());
  for (uint16_t k = 0; k < n; k++, i = (i + 1) % FT_N)
    out.printf("%lu %c %s\n", (unsigned long)ftrace[i].ms, ftrace[i].dir,
               ftrace[i].text);
}

void traceClear() { ftHead = 0; ftTotal = 0; }

// A key-up in the same form the elements use. "xmit 0" does NOT clear a
// key the radio still believes is down: it stays in TX on source=SWCW.
void sendKeyUp() {
  if (!tcp.connected()) return;
  txf("C%lu|cw %s 0 time=0x%04X index=%u client_handle=%s\n",
             (unsigned long)seq++, cfgKeyVerb,
             (unsigned)(millis() & 0xFFFF), (unsigned)(keyIndex++ & 0xFFFF),
             guiHandle.length() ? guiHandle.c_str() : "0x0");
  keyIsDown = false;
}

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
      Log::printf("[FLEX] warning: %s — the radio will not transmit\n",
                    !sliceInUse ? "no slice in use in SmartSDR"
                                : "the slice is not in CW mode");
    }
    if (e.down && !xmitOn && cfgUseXmit) {
      txf("C%lu|xmit 1\n", (unsigned long)seq++);
      xmitOn = true;
      if (logKeying) Log::println("[FLEX] xmit 1 (PTT)");
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
    txf("C%lu|cw %s %d time=0x%04X index=%u client_handle=%s\n",
               (unsigned long)seq++, cfgKeyVerb,
               e.down ? 1 : 0,
               (unsigned)(e.at & 0xFFFF), (unsigned)(keyIndex++ & 0xFFFF),
               guiHandle.length() ? guiHandle.c_str() : "0x0");
    if (logKeying) Log::printf("[FLEX] cw %s %d\n", cfgKeyVerb, e.down ? 1 : 0);
    keyIsDown = e.down;
    lastKeyMs = millis();
  }

    // Drive the LOCAL PTT line from what the radio is actually doing, not
  // from the monitor copy running through the local keyer. xmitOn covers
  // paddle keying; pending() is fed by the radio's own "cwx sent=" reports
  // and so covers buffered text. Timing the line from the local keyer meant
  // two independent CW generators drifting apart, with the gap growing over
  // the length of a transmission.
  {
    static bool     pttHeld    = false;
    static uint32_t lastBusyMs = 0;
    bool active = xmitOn || pending() > 0;
    if (active) lastBusyMs = millis();
    bool want = active ||
                (lastBusyMs && millis() - lastBusyMs < cfgTailMs);
    if (want != pttHeld) {
      pttHeld = want;
      Keyer::pttManual(want);
    }
  }

  // Release the transmitter once the operator has stopped sending — but
  // never while the key is still down, or a long element (or tune) would
  // drop PTT out from under itself.
  bool quiet = lastKeyMs && (millis() - lastKeyMs > cfgTailMs);
  // Safety net. Everything above can be defeated by one lost key-up, and
  // the cost of that is a transmitter left keyed for as long as nobody
  // notices. After this long with no key event at all, release regardless
  // of what the state machine believes.
  bool stuck = lastKeyMs && (millis() - lastKeyMs > 5000);
  // Gated on EITHER flag: a lost key-up can leave the radio keyed while
  // xmitOn is already false, and that used to mean nothing ever released.
  if ((xmitOn || keyIsDown) && ((quiet && !keyIsDown) || stuck)) {
    sendKeyUp();              // first, and always — see sendKeyUp()
    if (xmitOn) {
      txf("C%lu|xmit 0\n", (unsigned long)seq++);
      xmitOn = false;
    }
    if (stuck) {
      ftAdd('#', "clear: forced release, no key event for 5 s");
      sendCmd("cwx clear");   // and drop anything the radio never sent
      queuedIdx = sentIdx = 0;
      busyUntil = 0;
      Log::println("[FLEX] FORCED release, no key event for 5 s "
                   "(a key-up was lost)");
    } else if (logKeying) {
      Log::println("[FLEX] release (key up + xmit 0)");
    }
  }

  // The radio has stopped transmitting and stopped reporting progress, so
  // whatever it was sending is finished — however our provisional count
  // compares. Without this, pending() stays above zero after a memory (the
  // "cwx send" reply indexes the block's FIRST character), the local PTT
  // line is held on a buffer that no longer exists, and only the keyer's
  // 10 s backstop drops it: "PTT was stuck with no keying", seen 11:05:57.
  if (!radioTx && pending() > 0 && lastCwxMs && millis() - lastCwxMs > 1000) {
    queuedIdx = sentIdx = 0;
    busyUntil = 0;
  }

  // Catch-all, and the only one that sees a key-up lost in flight: the
  // radio says it is transmitting CW while nothing here is keying it.
  // Rescue once per transmission, and never touch a TX we did not cause
  // (source is SWCW only for CW keying — not MSHV, not a GUI client).
  // pending() is NOT enough on its own: for buffered text it collapses to
  // zero almost at once (see HANDOVER 11y), so a memory playing normally
  // looks idle from here. The radio's own progress reports are the
  // difference between "still sending" and "stuck".
  if (radioTx && radioTxIsCw && !forcedThisTx &&
      !keyIsDown && !xmitOn && pending() == 0 &&
      millis() - radioTxSince > 5000 &&
      (!lastCwxMs  || millis() - lastCwxMs  > 5000) &&
      (!lastKeyMs || millis() - lastKeyMs > 5000)) {
    forcedThisTx = true;
    ftAdd('#', "clear: radio transmitting CW with the keyer idle");
    sendKeyUp();
    sendCmd("cwx clear");
    queuedIdx = sentIdx = 0;
    busyUntil = 0;
    Log::println("[FLEX] radio still transmitting CW with the keyer idle — "
                 "forced key up + cwx clear");
  }
}

void poll() {
  if (!cfgEnabled || WiFi.status() != WL_CONNECTED) return;
  pumpKeying();
  if (rateActive && millis() - rateLastT > 3000) rateFinish();
  {
    int16_t x = cwExtraFor(Keyer::getWpm());
    if (Keyer::monitorExtraUs() != x) Keyer::setMonitorExtraUs(x);
  }
  cwSaveIfDue();

  static bool listening = false;
  if (!listening) {
    udpNew.begin(FLEX_DISCOVERY_PORT_NEW);
    udpOld.begin(FLEX_DISCOVERY_PORT_OLD);
    listening = true;
  }
  handleDiscovery(udpNew);
  handleDiscovery(udpOld);

  // Audible state: C when the radio becomes ready to key, D when it stops.
  //
  // "Ready" is connected AND holding a slice in CW mode — not merely
  // connected. Closing the SDR client removes the slice while leaving the
  // TCP session open, so a socket-based test stays silent through exactly
  // the event the operator needs to hear: the radio is still there and
  // will now transmit nothing.
  //
  // Sidetone only — Keyer::chirp() touches neither the key line nor PTT,
  // so this can never put the rig on the air.
  {
    static bool wasReady = false;
    bool isReady = connected() && sliceReady();
    if (isReady != wasReady) {
      wasReady = isReady;
      Log::printf("[FLEX] %s — chirp %c\n",
                  isReady ? "ready to key" : "NOT ready (no CW slice or link)",
                  isReady ? 'C' : 'D');
      Keyer::chirp(isReady ? 'C' : 'D');
    }
  }

  if (!tcp.connected()) { tryConnect(); return; }

  pollSocket();

  if (!subscribed && radioHandle.length()) {
    // No "client program" here: SmartSDR 1.4.0.0 rejects it with
    // 10000002 "unknown client program", and it buys us nothing —
    // the subscription is what actually matters.
    sendCmd("sub tx all");         // interlock: what the RADIO is doing
    sendCmd("sub cwx all");
    sendCmd("sub client all");     // so we can find a GUI client to bind to
    sendCmd("sub slice all");      // to warn when there is nothing to key on
    // A subscription delivers CHANGES. Without a snapshot the slice state
    // stays unknown until something happens to it, so ask outright.
    sendCmd("slice list");
    subscribed = true;
    Log::printf("[FLEX] subscribed (handle %s)\n", radioHandle.c_str());
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
  cwxSends[cwxSendNext].seq = seq;                            // seq sendCmd will use
  cwxSends[cwxSendNext].len = (uint16_t)strlen(text);
  strlcpy(cwxSends[cwxSendNext].text, text, sizeof(cwxSends[cwxSendNext].text));
  cwxSendNext = (cwxSendNext + 1) % (sizeof(cwxSends) / sizeof(cwxSends[0]));
  sendCmd("cwx send " + out);
  // Only time a start, not a continuation.
  if (!radioTx && pending() == 0) cwxSendAt = millis();
  lastCwxMs = millis();          // the radio is about to be busy sending
  // Provisional until the reply lands. Count on from wherever the radio has
  // got to: after a clear, a late "cwx erase"/"sent=" leaves sentIdx at the
  // radio's absolute index while queuedIdx restarts at 0, and counting on
  // from 0 put pending() below zero — every echo went out at once, before
  // the radio keyed a thing (seen 2026-09-13 after each Clear Buffer).
  if (queuedIdx < sentIdx) queuedIdx = sentIdx;
  queuedIdx += strlen(text);
  busyUntil = millis() + estimateMs(strlen(text)) + 5000;
}

void clear(const char* why) {
  if (!connected()) return;
  ftAdd('#', why);
  clearSeq = seq;              // anything already sent is now stale
  for (auto& e : cwxSends) e.len = 0;
  sendCmd("cwx clear");
  queuedIdx = sentIdx = 0;
  busyUntil = 0;
  rateActive = false;
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
  // BOTH conditions, or a long message dies mid-word: the estimate is made
  // per "cwx send", and a logger hands a memory over in small pieces, so
  // the deadline expires while the radio is still happily playing out the
  // accumulated buffer. Seen 2026-09-12 as "cwx erase=" chopping the tail
  // off every memory. The radio's own progress reports are the authority.
  if (busyUntil && (int32_t)(millis() - busyUntil) > 0 &&
      (!lastCwxMs || millis() - lastCwxMs > 5000)) {
    Log::println("[FLEX] no progress from radio — clearing pending "
                   "(slice not in CW mode? another client transmitting?)");
    ftAdd('#', "clear: no progress from radio");
    sendCmd("cwx clear");   // the radio can sit in TX on an unsent buffer
    queuedIdx = sentIdx = 0;
    busyUntil = 0;
    return 0;
  }
  return (int)d;
}

}  // namespace Flex

// ============================================================
//  LAN scan — find a radio that discovery cannot reach
//
//  Discovery listens for a UDP broadcast, and broadcasts stop at the subnet
//  edge: with the keyer on one VLAN and the radio on another it listens
//  forever. The SmartSDR API is ordinary TCP on 4992, which a router
//  forwards like anything else, so sweep a /24 for it instead.
//
//  Non-blocking connects, a few at a time. lwIP has 16 sockets and the web
//  server, host link, radio link, MQTT and the discovery listeners already
//  hold some of them — the window is kept small rather than fast, and a
//  host that cannot get a socket is retried, not skipped, so the radio can
//  never be missed for want of one. ~15 s for a /24; in its own task so
//  loop() and the keyer never wait on it.
// ============================================================

namespace {

constexpr int      SCAN_WINDOW   = 6;
constexpr uint32_t SCAN_WAIT_MS  = 300;   // per batch; a LAN answers in <10 ms
constexpr uint8_t  SCAN_MAX_HITS = 4;

portMUX_TYPE     scanMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool    scanBusy = false;
volatile uint8_t scanDone = 0;
uint8_t          scanNHits = 0;
Flex::ScanHit    scanList[SCAN_MAX_HITS];
char             scanPfx[16] = "";
const char*      scanErr = "";

// Value of key= in an "info" reply — comma-separated, values may be quoted.
void infoField(const char* s, const char* key, char* out, size_t len) {
  out[0] = '\0';
  size_t kl = strlen(key);
  for (const char* p = s; (p = strstr(p, key)) != nullptr; p++) {
    if (p != s && p[-1] != ',' && p[-1] != '|' && p[-1] != ' ') continue;
    if (p[kl] != '=') continue;
    p += kl + 1;
    if (*p == '"') p++;
    size_t n = 0;
    while (*p && *p != '"' && *p != ',' && *p != '\r' && *p != '\n' &&
           n + 1 < len)
      out[n++] = *p++;
    out[n] = '\0';
    return;
  }
}

// Read into buf until `until` appears or the deadline passes. The radio's
// greeting is ~1.4 KB (V, H, then a burst of status), bigger than buf, so
// a full buffer slides rather than stops — keeping a short tail so a
// marker split across two reads is still seen.
bool readUntil(int fd, char* buf, size_t len, size_t& have,
               const char* until, uint32_t deadline) {
  const size_t keep = 8;
  while ((int32_t)(millis() - deadline) < 0) {
    if (have + 1 >= len) {
      memmove(buf, buf + have - keep, keep);
      have = keep;
      buf[have] = '\0';
    }
    int r = recv(fd, buf + have, len - 1 - have, 0);
    if (r > 0) {
      have += r;
      buf[have] = '\0';
      if (strstr(buf, until)) return true;
    } else if (r == 0) {
      return false;                     // closed
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
      return false;
    }
  }
  return false;
}

// A host has 4992 open — confirm it is a Flex and learn what it is. The
// radio greets with "V<version>" the moment the socket opens; "info" gives
// the model and nickname. Nothing here changes any radio state.
bool identify(int fd, const char* ip, Flex::ScanHit& h) {
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
  timeval tv{0, 200000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

  char buf[768];
  size_t have = 0;
  buf[0] = '\0';
  if (!readUntil(fd, buf, sizeof buf, have, "\n", millis() + 800)) return false;
  if (buf[0] != 'V') return false;      // something else lives on 4992

  memset(&h, 0, sizeof h);
  strlcpy(h.ip, ip, sizeof h.ip);
  have = 0;
  buf[0] = '\0';
  const char q[] = "C1|info\n";
  send(fd, q, sizeof q - 1, 0);
  if (readUntil(fd, buf, sizeof buf, have, "R1|", millis() + 1500)) {
    // Move the reply to the front so the rest of its line fits behind it
    // (~370 bytes on a 6600) before looking for the end of it.
    char* r = strstr(buf, "R1|");
    have -= r - buf;
    memmove(buf, r, have + 1);
    if (!strchr(buf, '\n'))
      readUntil(fd, buf, sizeof buf, have, "\n", millis() + 600);
    r = buf;
    infoField(r, "model", h.model, sizeof h.model);
    infoField(r, "name", h.name, sizeof h.name);
    if (!h.name[0]) infoField(r, "callsign", h.name, sizeof h.name);
  }
  return true;
}

void scanTask(void*) {
  char ip[16];
  String self = WiFi.localIP().toString();
  int next = 1, starved = 0;

  while (next <= 254) {
    int fds[SCAN_WINDOW], n = 0;
    char ips[SCAN_WINDOW][16];

    while (n < SCAN_WINDOW && next <= 254) {
      snprintf(ip, sizeof ip, "%s.%d", scanPfx, next);
      if (self == ip) { next++; continue; }
      int fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) break;                // out of sockets: run what we have
      fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
      sockaddr_in a = {};
      a.sin_family = AF_INET;
      a.sin_port   = htons(FLEX_API_PORT);
      inet_pton(AF_INET, ip, &a.sin_addr);
      if (connect(fd, (sockaddr*)&a, sizeof a) < 0 && errno != EINPROGRESS) {
        close(fd);
        next++;
        continue;
      }
      fds[n] = fd;
      strlcpy(ips[n], ip, sizeof ips[n]);
      n++;
      next++;
    }

    if (n == 0) {
      // Every socket is taken. Wait for the rest of the firmware to hand
      // one back; give up after 5 s rather than spin forever.
      if (++starved > 25) { scanErr = "no free sockets"; break; }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    starved = 0;

    bool done[SCAN_WINDOW] = {}, open[SCAN_WINDOW] = {};
    int left = n;
    uint32_t t0 = millis();
    while (left) {
      uint32_t el = millis() - t0;
      if (el >= SCAN_WAIT_MS) break;
      fd_set w;
      FD_ZERO(&w);
      int mx = -1;
      for (int i = 0; i < n; i++)
        if (!done[i]) { FD_SET(fds[i], &w); if (fds[i] > mx) mx = fds[i]; }
      timeval tv{0, (long)(SCAN_WAIT_MS - el) * 1000};
      if (select(mx + 1, nullptr, &w, nullptr, &tv) <= 0) break;
      for (int i = 0; i < n; i++) {
        if (done[i] || !FD_ISSET(fds[i], &w)) continue;
        int err = 0;
        socklen_t l = sizeof err;
        getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &l);
        done[i] = true;
        open[i] = (err == 0);
        left--;
      }
    }

    for (int i = 0; i < n; i++) {
      Flex::ScanHit h;
      if (open[i] && identify(fds[i], ips[i], h)) {
        Log::printf("[FLEX] scan: %s at %s (%s)\n", h.model, h.ip, h.name);
        portENTER_CRITICAL(&scanMux);
        if (scanNHits < SCAN_MAX_HITS) scanList[scanNHits++] = h;
        portEXIT_CRITICAL(&scanMux);
      }
      close(fds[i]);
    }
    scanDone = next - 1;
  }

  scanDone = 254;
  Log::printf("[FLEX] scan of %s.x done — %u radio(s)%s%s\n", scanPfx,
              scanNHits, scanErr[0] ? ", " : "", scanErr);
  scanBusy = false;
  vTaskDelete(nullptr);
}

}  // namespace

namespace Flex {

bool scanStart(const char* prefix) {
  if (scanBusy || WiFi.status() != WL_CONNECTED) return false;
  unsigned a, b, c;
  char pfx[16];
  if (prefix && *prefix) {
    // Three octets; anything after them ("192.168.1.0/24", ".x") is ignored.
    if (sscanf(prefix, "%u.%u.%u", &a, &b, &c) != 3 || a > 255 || b > 255 ||
        c > 255)
      return false;
  } else {
    IPAddress me = WiFi.localIP();
    a = me[0]; b = me[1]; c = me[2];
  }
  snprintf(pfx, sizeof pfx, "%u.%u.%u", a, b, c);

  portENTER_CRITICAL(&scanMux);
  scanNHits = 0;
  portEXIT_CRITICAL(&scanMux);
  strlcpy(scanPfx, pfx, sizeof scanPfx);
  scanErr  = "";
  scanDone = 0;
  scanBusy = true;
  Log::printf("[FLEX] scanning %s.1-254 for port %d\n", scanPfx, FLEX_API_PORT);
  // Core 0, lowest priority: the keyer runs on core 1 and must not notice.
  if (xTaskCreatePinnedToCore(scanTask, "flexscan", 6144, nullptr, 1,
                              nullptr, 0) != pdPASS) {
    scanBusy = false;
    return false;
  }
  return true;
}

bool    scanRunning() { return scanBusy; }
uint8_t scanTried()   { return scanDone; }
String  scanNet()     { return String(scanPfx); }
String  scanError()   { return String(scanErr); }

uint8_t scanHits(ScanHit* out, uint8_t max) {
  portENTER_CRITICAL(&scanMux);
  uint8_t n = scanNHits < max ? scanNHits : max;
  for (uint8_t i = 0; i < n; i++) out[i] = scanList[i];
  portEXIT_CRITICAL(&scanMux);
  return n;
}

}  // namespace Flex
