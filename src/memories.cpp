#include "memories.h"
#include "winkeyer.h"
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
  for (uint8_t i = 1; i <= COUNT; i++)
    cache[i] = p.getString(slotKey(i).c_str(), "");
  callCache = p.getString("call", "");
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

bool play(uint8_t slot) {
  String raw = get(slot);
  if (!raw.length()) return false;
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
