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
#include "keyer.h"
#include "winkeyer.h"
#include "flex.h"
#include "net.h"
#include "display.h"
#include <Preferences.h>

namespace {

const char* NS = "wk";

uint32_t loadU32(const char* key, uint32_t def) {
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
  if (persist) {
    Preferences p;
    p.begin(NS, false);
    p.putBool("flexbe", useFlex);
    p.end();
  }
}

bool loadBackend() {
  Preferences p;
  p.begin(NS, true);
  bool useFlex = p.isKey("flexbe") ? p.getBool("flexbe", false) : false;
  p.end();
  return useFlex;
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

  String ctl = loadStr("dispctl");
  Display::setController(ctl.length() ? ctl.c_str() : "sh1106");
  Display::setEnabled(loadU32("dispen", 1));
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
    Keyer::setPttTailMs(n); saveU32("tail", n);
    snprintf(msg, msgLen, "ptt tail=%d ms", n);

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
    if (!Display::setController(val)) return fail("dispctl: sh1106|ssd1306");
    saveStr("dispctl", Display::controller());
    snprintf(msg, msgLen, "display controller=%s", Display::controller());

  } else if (!strcasecmp(key, "backend")) {
    bool useFlex = !strcasecmp(val, "flex");
    applyBackend(useFlex, true);
    snprintf(msg, msgLen, "backend=%s", useFlex ? "flex" : "local");

  } else if (!strcasecmp(key, "flex")) {
    if (!boolish(val)) return fail("flex: on|off");
    bool b = truthy(val);
    Flex::setEnabled(b);          // persists in its own NVS namespace
    snprintf(msg, msgLen, "flex=%s", b ? "enabled" : "disabled");

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
  doc["weight"]  = Keyer::getWeighting();
  doc["ratio"]   = Keyer::getRatio();
  doc["farns"]   = Keyer::getFarnsworth();
  doc["pot"]     = Keyer::getPotEnabled();
  doc["potmin"]  = Keyer::getPotMin();
  doc["potmax"]  = Keyer::getPotMin() + Keyer::getPotRange();
  doc["busy"]    = Keyer::busy();
  doc["key"]     = Keyer::keyIsDown();
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
}

}  // namespace Settings
