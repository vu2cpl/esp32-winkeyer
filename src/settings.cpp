// ============================================================
//  ESP32 WinKeyer — persisted operator settings
//
//  NVS namespace "wk". Keys are <=15 chars (an NVS limit) and are
//  stored as u32 regardless of their logical type, so adding a
//  setting never needs a migration — a key that has never been
//  written simply falls back to its default.
//
//  "flexbe" predates this module and is a bool; it is read and
//  written as one so boards already in the shack keep their
//  backend across this upgrade.
// ============================================================

#include "settings.h"
#include "config.h"
#include "keyer.h"
#include "winkeyer.h"
#include "flex.h"
#include "net.h"
#include "display.h"
#include "fsk.h"
#include <Preferences.h>

namespace {

const char* NS = "wk";

// Create the namespace once, read-write. A read-only open of a namespace
// that has never been written FAILS, and fails SLOWLY — about 630 ms, with
// an ERROR logged each time. hostBaud() is reached from toJson(), which the
// settings page polls every second, so on a factory-fresh board that made
// the endpoint unusable and filled the console. Same failure mode that took
// the web server down when memories were first added.
void ensureNs() {
  static bool done = false;
  if (done) return;
  Preferences p;
  p.begin(NS, false);
  p.end();
  done = true;
}

uint32_t loadU32(const char* key, uint32_t def) {
  ensureNs();
  Preferences p;
  p.begin(NS, true);
  uint32_t v = p.isKey(key) ? p.getUInt(key, def) : def;
  p.end();
  return v;
}

void saveU32(const char* key, uint32_t v) {
  Preferences p;
  p.begin(NS, false);
  p.putUInt(key, v);
  p.end();
}

void saveStr(const char* key, const char* v) {
  Preferences p;
  p.begin(NS, false);
  p.putString(key, v);
  p.end();
}

String loadStr(const char* key) {
  ensureNs();
  Preferences p;
  p.begin(NS, true);
  String v = p.isKey(key) ? p.getString(key, "") : String("");
  p.end();
  return v;
}

bool truthy(const char* v) {
  return !strcasecmp(v, "on") || !strcasecmp(v, "1") ||
         !strcasecmp(v, "true") || !strcasecmp(v, "yes");
}
bool boolish(const char* v) {
  return truthy(v) || !strcasecmp(v, "off") || !strcasecmp(v, "0") ||
         !strcasecmp(v, "false") || !strcasecmp(v, "no");
}

// Defaults mirror the Keyer's own, so a board with empty NVS behaves
// exactly as it did before this module existed.
constexpr uint32_t D_WPM = 20, D_STHZ = 600, D_LEAD = 50, D_TAIL = 250;
constexpr uint32_t D_WEIGHT = 50, D_RATIO = 50, D_FARNS = 0;
constexpr uint32_t D_POTMIN = 10, D_POTRNG = 25;

}  // namespace

namespace Settings {

void applyBackend(bool useFlex, bool persist) {
  WinKeyer::setBackend(useFlex ? WK_BACKEND_FLEX : WK_BACKEND_LOCAL);
  // On the Flex path the radio is keyed over the network; the local key
  // line stays idle so the rig is not keyed twice. Sidetone stays local,
  // generated from the operator's own paddle timing, so the fist sounds
  // right in the ear regardless of what the link is doing.
  Keyer::setKeyOutEnabled(!useFlex);
  Flex::setDirectKeying(useFlex);
  Keyer::setKeyEventHook(useFlex ? Flex::keyEvent : nullptr);
  // On Flex, buffered text is keyed by the radio and only monitored locally,
  // so those elements must not reach the hook. Paddle elements still must.
  Keyer::setHookPaddleOnly(useFlex);
  // On Flex the radio owns the transmission, so it owns the PTT line too.
  Keyer::setPttAuto(!useFlex);
  if (!useFlex) Keyer::pttManual(false);   // hand the line back cleanly
  if (persist) {
    Preferences p;
    p.begin(NS, false);
    p.putBool("flexbe", useFlex);
    p.end();
  }
}

bool loadBackend() {
  ensureNs();
  Preferences p;
  p.begin(NS, true);
  bool useFlex = p.isKey("flexbe") ? p.getBool("flexbe", false) : false;
  p.end();
  return useFlex;
}

const char* resetWhy = "?";
void        setResetReason(const char* why) { resetWhy = why; }
const char* resetReason() { return resetWhy; }

uint32_t hostBaud() { return loadU32("baud", WK_HOST_BAUD_DEFAULT); }

// Read before Display::begin(), which runs long before begin() restores
// the rest — a disabled panel must not be probed at all.
bool displayEnabled() { return loadU32("dispen", 1) != 0; }

// Below ~9600 the boot log itself becomes the problem: every character
// printed is a character the host is waiting through before its Host Open
// gets an answer, and loggers give up.
bool quietBoot() { return hostBaud() <= 9600; }

void restoreKeyer() {
  Keyer::setWpm(loadU32("wpm", D_WPM));
  Keyer::setMode(loadU32("mode", 1) ? KEYER_IAMBIC_B : KEYER_IAMBIC_A);
  Keyer::setPaddleSwap(loadU32("swap", 0));
  Keyer::setSidetone(loadU32("st", 1));
  Keyer::setSidetoneHz(loadU32("sthz", D_STHZ));
  Keyer::setPttEnabled(loadU32("ptt", 1));
  Keyer::setPttLeadMs(loadU32("lead", D_LEAD));
  Keyer::setPttTailMs(loadU32("tail", D_TAIL));
  Flex::setPttTailMs(loadU32("tail", D_TAIL));
  Keyer::setWeighting(loadU32("weight", D_WEIGHT));
  Keyer::setRatio(loadU32("ratio", D_RATIO));
  Keyer::setFarnsworth(loadU32("farns", D_FARNS));
  Keyer::setPotRange(loadU32("potmin", D_POTMIN), loadU32("potrng", D_POTRNG));
  Keyer::setPotEnabled(loadU32("poten", 0));
  Keyer::setRadio(loadU32("radio", 1));
}

void begin() {
  Keyer::setWpm(loadU32("wpm", D_WPM));
  Keyer::setMode(loadU32("mode", 1) ? KEYER_IAMBIC_B : KEYER_IAMBIC_A);
  Keyer::setPaddleSwap(loadU32("swap", 0));
  Keyer::setSidetone(loadU32("st", 1));
  Keyer::setSidetoneHz(loadU32("sthz", D_STHZ));
  Keyer::setPttEnabled(loadU32("ptt", 1));
  Keyer::setPttLeadMs(loadU32("lead", D_LEAD));
  Keyer::setPttTailMs(loadU32("tail", D_TAIL));
  Flex::setPttTailMs(loadU32("tail", D_TAIL));
  Keyer::setWeighting(loadU32("weight", D_WEIGHT));
  Keyer::setRatio(loadU32("ratio", D_RATIO));
  Keyer::setFarnsworth(loadU32("farns", D_FARNS));

  // Pot range before pot enable: enabling re-arms the "first reading does
  // not stomp the boot speed" guard, and we want that armed against the
  // range actually in force.
  Keyer::setPotRange(loadU32("potmin", D_POTMIN),
                     loadU32("potrng", D_POTRNG));
  // Still defaults OFF — a board with no pot wired must not have GPIO34's
  // floating input driving the speed. Once /pot on is given, it sticks.
  Keyer::setPotEnabled(loadU32("poten", 0));

  // Bus rate before the panel type, so the panel is brought up at the
  // rate the operator chose rather than re-initialised twice.
  Display::setController(loadU32("dispfast", 0) ? "fast" : "slow");
  String ctl = loadStr("dispctl");
  Display::setController(ctl.length() ? ctl.c_str() : "sh1106");
  Display::setEnabled(loadU32("dispen", 1));
  WinKeyer::setMonitor(loadU32("monitor", 1));
  WinKeyer::setPaddleEcho(loadU32("pecho", 2));
  Keyer::setRadio(loadU32("radio", 1));
  // Flex keying details. These were CLI-only and unpersisted, so they had
  // to be re-entered after every reflash or board swap.
  { String v = loadStr("flexcmd"); if (v.length()) Flex::setKeyVerb(v.c_str()); }
  Flex::setBind(loadU32("flexbind", 1));
  Flex::setUseXmit(loadU32("flexxmit", 1));
  Fsk::setBaud(loadU32("fskbaud", 4545) / 100.0f);
  Fsk::setInvert(loadU32("fskinv", 0));
  Fsk::setDiddle(loadU32("fskdiddle", 0));
}

bool apply(const char* key, const char* val, char* msg, size_t msgLen) {
  auto fail = [&](const char* why) {
    snprintf(msg, msgLen, "%s", why);
    return false;
  };
  if (!key || !val) return fail("missing key or value");

  int n = atoi(val);

  if (!strcasecmp(key, "wpm")) {
    if (n < 5 || n > 60) return fail("wpm: 5..60");
    Keyer::setWpm(n); saveU32("wpm", n);
    snprintf(msg, msgLen, "wpm=%d", n);

  } else if (!strcasecmp(key, "mode")) {
    bool b = (tolower(val[0]) == 'b');
    Keyer::setMode(b ? KEYER_IAMBIC_B : KEYER_IAMBIC_A); saveU32("mode", b);
    snprintf(msg, msgLen, "mode=%c", b ? 'B' : 'A');

  } else if (!strcasecmp(key, "swap")) {
    if (!boolish(val)) return fail("swap: on|off");
    bool b = truthy(val);
    Keyer::setPaddleSwap(b); saveU32("swap", b);
    snprintf(msg, msgLen, "swap=%s", b ? "on" : "off");

  } else if (!strcasecmp(key, "st")) {
    if (boolish(val)) {
      bool b = truthy(val);
      Keyer::setSidetone(b); saveU32("st", b);
      snprintf(msg, msgLen, "sidetone=%s", b ? "on" : "off");
    } else {
      if (n < 300 || n > 2000) return fail("sidetone: on|off or 300..2000 Hz");
      Keyer::setSidetoneHz(n); saveU32("sthz", n);
      snprintf(msg, msgLen, "sidetone=%d Hz", n);
    }

  } else if (!strcasecmp(key, "sthz")) {
    if (n < 300 || n > 2000) return fail("sthz: 300..2000");
    Keyer::setSidetoneHz(n); saveU32("sthz", n);
    snprintf(msg, msgLen, "sidetone=%d Hz", n);

  } else if (!strcasecmp(key, "ptt")) {
    if (!boolish(val)) return fail("ptt: on|off");
    bool b = truthy(val);
    Keyer::setPttEnabled(b); saveU32("ptt", b);
    snprintf(msg, msgLen, "ptt=%s", b ? "on" : "off");

  } else if (!strcasecmp(key, "lead")) {
    if (n < 0 || n > 2000) return fail("lead: 0..2000 ms");
    Keyer::setPttLeadMs(n); saveU32("lead", n);
    snprintf(msg, msgLen, "ptt lead=%d ms", n);

  } else if (!strcasecmp(key, "tail")) {
    if (n < 0 || n > 2000) return fail("tail: 0..2000 ms");
    // Two transmitters to release: the local PTT line (GPIO32, live on both
    // backends for an amp or sequencer) and, on the Flex backend, the radio
    // itself via "xmit 0". Flex kept its own hardcoded 400 ms, so this
    // control moved the local line while the operator was listening to the
    // radio — audibly doing nothing.
    Keyer::setPttTailMs(n);
    Flex::setPttTailMs(n);
    saveU32("tail", n);
    snprintf(msg, msgLen, "ptt tail=%d ms (local + flex)", n);

  } else if (!strcasecmp(key, "weight")) {
    if (n < 10 || n > 90) return fail("weight: 10..90 (50 nominal)");
    Keyer::setWeighting(n); saveU32("weight", n);
    snprintf(msg, msgLen, "weighting=%d", n);

  } else if (!strcasecmp(key, "ratio")) {
    if (n < 33 || n > 66) return fail("ratio: 33..66 (50 nominal)");
    Keyer::setRatio(n); saveU32("ratio", n);
    snprintf(msg, msgLen, "ratio=%d", n);

  } else if (!strcasecmp(key, "farns")) {
    if (n != 0 && (n < 5 || n > 60)) return fail("farnsworth: 0 (off) or 5..60");
    Keyer::setFarnsworth(n); saveU32("farns", n);
    snprintf(msg, msgLen, "farnsworth=%d", n);

  } else if (!strcasecmp(key, "pot")) {
    if (!boolish(val)) return fail("pot: on|off");
    bool b = truthy(val);
    Keyer::setPotEnabled(b); saveU32("poten", b);
    snprintf(msg, msgLen, "pot=%s (%u-%u WPM)", b ? "on" : "off",
             Keyer::getPotMin(), Keyer::getPotMin() + Keyer::getPotRange());

  } else if (!strcasecmp(key, "potmin") || !strcasecmp(key, "potmax")) {
    int lo = Keyer::getPotMin();
    int hi = lo + Keyer::getPotRange();
    if (!strcasecmp(key, "potmin")) lo = n; else hi = n;
    if (lo < 5 || hi > 60 || hi <= lo) return fail("pot range: 5..60, max > min");
    Keyer::setPotRange(lo, hi - lo);
    saveU32("potmin", lo); saveU32("potrng", hi - lo);
    snprintf(msg, msgLen, "pot range=%d-%d WPM", lo, hi);

  } else if (!strcasecmp(key, "disp")) {
    if (!boolish(val)) return fail("disp: on|off");
    bool b = truthy(val);
    Display::setEnabled(b); saveU32("dispen", b);
    snprintf(msg, msgLen, "display=%s%s", b ? "on" : "off",
             Display::present() ? "" : " (no panel detected)");

  } else if (!strcasecmp(key, "dispctl")) {
    if (!Display::setController(val))
      return fail("dispctl: sh1106|ssd1306|lcd16x2|lcd20x4|auto|slow|fast");
    // "slow"/"fast" set the bus rate, not the panel type, so they persist
    // under their own key and must not overwrite the controller.
    if (!strcasecmp(val, "slow") || !strcasecmp(val, "fast"))
      saveU32("dispfast", !strcasecmp(val, "fast"));
    else
      saveStr("dispctl", Display::controller());
    snprintf(msg, msgLen, "display controller=%s", Display::controller());

  } else if (!strcasecmp(key, "radio")) {
    uint8_t sel = 0;
    if      (!strcasecmp(val, "1")) sel = 1;
    else if (!strcasecmp(val, "2")) sel = 2;
    else if (!strcasecmp(val, "both") || !strcasecmp(val, "3")) sel = 3;
    else return fail("radio: 1|2|both");
    Keyer::setRadio(sel); saveU32("radio", sel);
    snprintf(msg, msgLen, "radio=%s%s",
             sel == 3 ? "both" : (sel == 2 ? "2" : "1"),
             sel == 3 ? " — BOTH transmitters key together" : "");

  } else if (!strcasecmp(key, "fskbaud")) {
    float b = atof(val);
    if (!Fsk::setBaud(b)) return fail("fsk baud: 10..300 (45.45 standard RTTY)");
    saveU32("fskbaud", (uint32_t)(b * 100));      // hundredths: 45.45 is not an int
    snprintf(msg, msgLen, "fsk %.2f baud", b);

  } else if (!strcasecmp(key, "fskinv")) {
    if (!boolish(val)) return fail("fskinv: on|off");
    bool b = truthy(val);
    Fsk::setInvert(b); saveU32("fskinv", b);
    snprintf(msg, msgLen, "fsk mark = %s", b ? "low (inverted)" : "high");

  } else if (!strcasecmp(key, "fskdiddle")) {
    if (!boolish(val)) return fail("fskdiddle: on|off");
    bool b = truthy(val);
    Fsk::setDiddle(b); saveU32("fskdiddle", b);
    snprintf(msg, msgLen, "fsk diddle=%s", b ? "on" : "off");

  } else if (!strcasecmp(key, "pecho")) {
    uint8_t m;
    if      (!strcasecmp(val, "auto")) m = 2;
    else if (truthy(val))              m = 1;
    else if (boolish(val))             m = 0;
    else return fail("pecho: on|off|auto");
    WinKeyer::setPaddleEcho(m); saveU32("pecho", m);
    snprintf(msg, msgLen, "paddle echo=%s (now %s)",
             m == 2 ? "auto" : (m ? "on" : "off"),
             WinKeyer::paddleEchoActive() ? "active" : "inactive");

  } else if (!strcasecmp(key, "monitor")) {
    if (!boolish(val)) return fail("monitor: on|off");
    bool b = truthy(val);
    WinKeyer::setMonitor(b); saveU32("monitor", b);
    snprintf(msg, msgLen, "sidetone monitor=%s", b ? "on" : "off");

  } else if (!strcasecmp(key, "baud")) {
    // Only rates a WinKeyer host or a human console would actually use.
    const uint32_t allowed[] = {1200, 4800, 9600, 19200, 38400, 57600, 115200};
    bool ok = false;
    for (uint32_t a : allowed) if ((uint32_t)n == a) ok = true;
    if (!ok) return fail("baud: 1200 (WinKeyer) 4800 9600 19200 38400 57600 115200");
    saveU32("baud", n);
    snprintf(msg, msgLen, "serial %d baud %s — switching now", n,
             n == 1200 ? "8N2 (WinKeyer)" : "8N1");
    Serial.flush();                       // get the reply out at the old rate
    Serial.end();
    Serial.begin(n, n == 1200 ? SERIAL_8N2 : SERIAL_8N1);

  } else if (!strcasecmp(key, "backend")) {
    bool useFlex = !strcasecmp(val, "flex");
    applyBackend(useFlex, true);
    snprintf(msg, msgLen, "backend=%s", useFlex ? "flex" : "local");

  } else if (!strcasecmp(key, "flex")) {
    if (!boolish(val)) return fail("flex: on|off");
    bool b = truthy(val);
    Flex::setEnabled(b);          // persists in its own NVS namespace
    snprintf(msg, msgLen, "flex=%s", b ? "enabled" : "disabled");

  } else if (!strcasecmp(key, "flexcmd")) {
    if (strcasecmp(val, "key") && strcasecmp(val, "ptt"))
      return fail("flexcmd: key|ptt");
    Flex::setKeyVerb(val); saveStr("flexcmd", val);
    snprintf(msg, msgLen, "flex keying command: cw %s", Flex::keyVerb());

  } else if (!strcasecmp(key, "flexbind")) {
    if (!boolish(val)) return fail("flexbind: on|off");
    bool b = truthy(val);
    Flex::setBind(b); saveU32("flexbind", b);
    snprintf(msg, msgLen, "flex client bind=%s — reconnecting", b ? "on" : "off");

  } else if (!strcasecmp(key, "flexxmit")) {
    if (!boolish(val)) return fail("flexxmit: on|off");
    bool b = truthy(val);
    Flex::setUseXmit(b); saveU32("flexxmit", b);
    snprintf(msg, msgLen, "flex xmit(PTT)=%s", b ? "on" : "off");

  } else if (!strcasecmp(key, "flexip")) {
    Flex::setManualIp(val);       // "" = back to discovery
    snprintf(msg, msgLen, "flex ip=%s", strlen(val) ? val : "(discovery)");

  } else {
    return fail("unknown setting");
  }
  return true;
}

void toJson(JsonDocument& doc) {
  doc["wpm"]     = Keyer::getWpm();
  doc["mode"]    = Keyer::getMode() == KEYER_IAMBIC_A ? "a" : "b";
  doc["swap"]    = Keyer::getPaddleSwap();
  doc["st"]      = Keyer::getSidetone();
  doc["sthz"]    = Keyer::getSidetoneHz();
  doc["ptt"]     = Keyer::getPttEnabled();
  doc["lead"]    = Keyer::getPttLeadMs();
  doc["tail"]    = Keyer::getPttTailMs();
  doc["flextail"]= Flex::pttTailMs();   // proves the two are in step
  doc["baud"]    = hostBaud();
  doc["resetreason"] = resetReason();
  doc["uptime"]      = (uint32_t)(millis() / 1000);
  doc["echo"]    = WinKeyer::echoEnabled();
  doc["monitor"] = WinKeyer::monitor();
  doc["pecho"]   = WinKeyer::paddleEcho();
  doc["pechoon"] = WinKeyer::paddleEchoActive();
  doc["radio"]   = Keyer::getRadio();
  doc["fskbaud"] = Fsk::baud();
  doc["fskinv"]  = Fsk::invert();
  doc["fskdid"]  = Fsk::diddle();
  doc["fskbusy"] = Fsk::busy();
  doc["modereg"] = WinKeyer::modeRegister();
  doc["weight"]  = Keyer::getWeighting();
  doc["ratio"]   = Keyer::getRatio();
  doc["farns"]   = Keyer::getFarnsworth();
  doc["pot"]     = Keyer::getPotEnabled();
  doc["potmin"]  = Keyer::getPotMin();
  doc["potmax"]  = Keyer::getPotMin() + Keyer::getPotRange();
  doc["busy"]    = Keyer::busy();
  doc["key"]     = Keyer::keyIsDown();
  // NOT "ptt": that key is the enable SETTING, and reusing it here made
  // the web checkbox mirror the live line instead — so it read false almost
  // always and appeared impossible to turn on.
  doc["ptton"]   = Keyer::pttIsOn();      // the line right now, GPIO32/19
  doc["tune"]    = Keyer::tuning();
  doc["backend"] = WinKeyer::getBackend() == WK_BACKEND_FLEX ? "flex" : "local";
  doc["host"]    = WinKeyer::hostOpen();
  doc["tcp"]     = Net::clientConnected();
  doc["disp"]    = Display::enabled();
  doc["dispctl"] = Display::controller();
  doc["disphw"]  = Display::present();

  JsonObject f = doc.createNestedObject("flex");
  f["enabled"]   = Flex::enabled();
  f["connected"] = Flex::connected();
  f["ip"]        = Flex::radioIp();
  f["slice"]     = Flex::sliceReady();
  f["cmd"]       = Flex::keyVerb();
  f["bind"]      = Flex::bindEnabled();
  f["xmit"]      = Flex::useXmit();
  f["xmiton"]    = Flex::transmitting();  // is the RADIO keyed right now
}

}  // namespace Settings
