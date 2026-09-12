#include "memories.h"
#include "winkeyer.h"
#include "keyer.h"
#include "log.h"
#include <Preferences.h>

namespace {
const char* NS = "wkmsg";

// Cached in RAM and written through on change. The web page polls state
// once a second and every poll wanted all six slots plus the callsign:
// seven NVS opens, and an open of a namespace that does not exist yet
// FAILS SLOWLY — about 630 ms each — which made /api/state take four
// seconds and starved the whole server. NVS belongs nowhere near a poll.
String cache[Memories::COUNT + 1];   // 1-based; [0] unused
String callCache;
bool   loaded = false;
// The slot last started, held only while it is still going out.
uint8_t lastPlayed = 0;

String slotKey(uint8_t slot) { return String("m") + slot; }


// Expand %C to the operator's callsign. Kept deliberately small: a full
// macro language belongs in the logger, which already has one — this is
// only so a memory does not have to be retyped when the call changes.
String expand(const String& raw, const String& call) {
  String out;
  out.reserve(raw.length() + call.length());
  for (size_t i = 0; i < raw.length(); i++) {
    if (raw[i] == '%' && i + 1 < raw.length() &&
        (raw[i + 1] == 'C' || raw[i + 1] == 'c')) {
      out += call;
      i++;
    } else {
      out += raw[i];
    }
  }
  return out;
}
}  // namespace

namespace Memories {

void begin() {
  Preferences p;
  // Opened read-WRITE so the namespace is created if it has never been
  // used. A read-only open of a missing namespace is the slow failure.
  p.begin(NS, false);
  // isKey() first: getString() logs "nvs_get_str len fail: NOT_FOUND" at
  // ERROR level for a missing key even when a default is supplied, so an
  // unused memory slot printed an alarming line at every boot.
  for (uint8_t i = 1; i <= COUNT; i++) {
    // Hold the String: slotKey() returns a temporary, so keeping only its
    // c_str() would leave a dangling pointer once the expression ended.
    String k = slotKey(i);
    cache[i] = p.isKey(k.c_str()) ? p.getString(k.c_str(), "") : String("");
  }
  callCache = p.isKey("call") ? p.getString("call", "") : String("");
  p.end();
  loaded = true;
}

bool set(uint8_t slot, const char* text) {
  if (slot < 1 || slot > COUNT) return false;
  if (text && strlen(text) > MAX_LEN) return false;
  cache[slot] = text ? text : "";
  Preferences p;
  p.begin(NS, false);
  if (cache[slot].length()) p.putString(slotKey(slot).c_str(), cache[slot]);
  else                      p.remove(slotKey(slot).c_str());
  p.end();
  return true;
}

String get(uint8_t slot) {
  if (slot < 1 || slot > COUNT) return String("");
  if (!loaded) begin();
  return cache[slot];
}

uint8_t playing() {
  // Anything the keyer is doing ends the claim, including the operator
  // breaking in on the paddle — the memory is aborted by that anyway.
  if (!Keyer::busy()) lastPlayed = 0;
  return lastPlayed;
}

bool play(uint8_t slot) {
  String raw = get(slot);
  if (!raw.length()) return false;
  lastPlayed = slot;
  String out = expand(raw, call());
  // Through the backend router, never straight to the keyer: on the Flex
  // path the radio generates the CW and a direct send would be silent.
  WinKeyer::sendText(out.c_str());
  Log::printf("[MEM] %u > %s\n", slot, out.c_str());
  return true;
}

void setCall(const char* c) {
  callCache = c ? c : "";
  Preferences p;
  p.begin(NS, false);
  p.putString("call", callCache);
  p.end();
}

String call() {
  if (!loaded) begin();
  return callCache;
}

}  // namespace Memories
