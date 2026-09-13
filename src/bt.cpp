// ============================================================
//  ESP32 WinKeyer — Bluetooth LE keyboard
//
//  Grown from src/probes/ble_kbd_probe.cpp, which proved the parts
//  on this board and core (HANDOVER, 2026-09-13).
//
//  Threads, and why keys go through a queue:
//  - the GAP callback runs in the Bluetooth task, the HID callbacks
//    in esp_hid's own event task. Neither may touch the keyer, the
//    memories or NVS — those belong to loop().
//  - so a key press is only decoded and queued there, and acted on
//    in poll(), exactly as the web page's SEND is.
//  - esp_hidh_dev_open() blocks until the keyboard's GATT database
//    has been read — seconds. In loop() that would stall the logger
//    link and Flex::poll(), so it runs in a small task of its own.
// ============================================================

#include "bt.h"
#include "log.h"
#include "settings.h"
#include "keyer.h"
#include "winkeyer.h"
#include "memories.h"
#include "sdkconfig.h"

namespace {
bool settingOn = false;
}

#if CONFIG_BT_BLUEDROID_ENABLED && __has_include("esp_hidh.h")

#include <Preferences.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_hidh.h"
#include "nvs.h"
#include "freertos/queue.h"

// initArduino() asks this before setup() runs, and frees the Bluetooth
// controller's memory when it says false. nvs_flash_init() has already run
// by then (esp32-hal-misc.c), so the saved switch can be read directly —
// which is what makes "off" cost nothing at all.
extern "C" bool bleInUse(void) {
  nvs_handle_t h;
  uint32_t v = 0;
  if (nvs_open("wk", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u32(h, "bten", &v);
    nvs_close(h);
  }
  return v != 0;
}

namespace {

const char* NS = "wk";

// Minimum free heap once Bluetooth is up, or it is switched back off at boot.
// On this board the full keyer left ~126 KB free and Bluedroid took ~90 KB;
// the ~36 KB left starved lwIP — no ping, no web page, sockets failing with
// "Not enough memory" (2026-09-13). The CLI could not rescue it either: the
// error flood at 1200 baud kept loop() busy printing. A setting must never be
// able to take the keyer off the network, so the check is automatic.
constexpr uint32_t BT_MIN_FREE_HEAP = 60000;
uint32_t errHeap = 0;   // heap seen when Bluetooth was last refused, 0 = none

enum class St : uint8_t { Idle, ScanStarting, Scanning, BgScan, OpenPending, Opening, Connected };
enum class Kind : uint8_t { User, Bg };

volatile St st = St::Idle;
Kind        scanKind = Kind::Bg;
bool        activeFlag = false;

struct Target { esp_bd_addr_t bda; uint8_t type; char name[24]; bool valid; };
Target target{};   // the paired keyboard, persisted
Target want{};     // the one being connected to; becomes target on success

struct Hit { esp_bd_addr_t bda; uint8_t type; int8_t rssi; char name[24]; };
const uint8_t MAX_HITS = 8;
Hit           hits[MAX_HITS];
uint8_t       nHits = 0;
portMUX_TYPE  mux = portMUX_INITIALIZER_UNLOCKED;

esp_hidh_dev_t* kbd = nullptr;
char            devName[24] = "";
volatile int16_t battery = -1;
volatile int32_t passkey = -1;   // shown on the page while pairing wants it
volatile bool    savePending = false;
volatile uint32_t idleSince = 0;
uint32_t         keysSeen = 0;
uint8_t          prevKeys[6];

struct KeyEv { uint8_t usage, mods; };
QueueHandle_t keyQ = nullptr;
TaskHandle_t  openTask = nullptr;

String fmtBda(const uint8_t* b) {
  char s[18];
  snprintf(s, sizeof s, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
  return s;
}

bool parseBda(const char* s, esp_bd_addr_t out) {
  unsigned v[6];
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) { if (v[i] > 0xff) return false; out[i] = (uint8_t)v[i]; }
  return true;
}

void goIdle() { st = St::Idle; idleSince = millis(); }

// ── persistence of the one paired keyboard ────────────────
void loadTarget() {
  Preferences p;
  p.begin(NS, true);
  String a = p.isKey("btaddr") ? p.getString("btaddr", "") : String("");
  target.valid = a.length() && parseBda(a.c_str(), target.bda);
  target.type  = p.getUInt("bttype", 0);
  strlcpy(target.name, p.isKey("btname") ? p.getString("btname", "").c_str() : "",
          sizeof target.name);
  p.end();
}

void saveTarget() {
  Preferences p;
  p.begin(NS, false);
  p.putString("btaddr", fmtBda(target.bda));
  p.putUInt("bttype", target.type);
  p.putString("btname", target.name);
  p.end();
}

void clearTarget() {
  target.valid = false;
  Preferences p;
  p.begin(NS, false);
  p.remove("btaddr"); p.remove("bttype"); p.remove("btname");
  p.end();
}

// Bonds other than `keep` (all of them when keep is null).
void removeBonds(const uint8_t* keep) {
  int n = esp_ble_get_bond_device_num();
  if (n <= 0) return;
  auto* list = (esp_ble_bond_dev_t*)malloc(sizeof(esp_ble_bond_dev_t) * n);
  if (!list) return;
  esp_ble_get_bond_device_list(&n, list);
  for (int i = 0; i < n; i++)
    if (!keep || memcmp(list[i].bd_addr, keep, 6)) esp_ble_remove_bond_device(list[i].bd_addr);
  free(list);
}

// ── keys ──────────────────────────────────────────────────
// Boot-keyboard report: [modifiers, reserved, key1..key6]; some keyboards
// drop the reserved byte. A key counts once, when it first appears.
void onReport(const uint8_t* d, uint16_t len) {
  if (len != 8 && len != 7) return;
  const uint8_t* k = (len == 8) ? d + 2 : d + 1;
  for (int i = 0; i < 6; i++) {
    uint8_t u = k[i];
    if (u < 0x04 || memchr(prevKeys, u, 6)) continue;   // empty, rollover, or held
    KeyEv e{u, d[0]};
    xQueueSend(keyQ, &e, 0);
  }
  memcpy(prevKeys, k, 6);
}

char charFor(uint8_t u, bool shift) {
  if (u >= 0x04 && u <= 0x1D) return 'A' + (u - 0x04);
  if (u >= 0x1E && u <= 0x27) return (shift ? "!@#$%^&*()" : "1234567890")[u - 0x1E];
  switch (u) {
    case 0x2C: return ' ';
    case 0x2D: return shift ? '_' : '-';
    case 0x2E: return shift ? '+' : '=';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
  }
  return 0;
}

// Speed from the keyboard is the operator's, so it persists like the page's.
void stepWpm(int d) {
  char v[8], msg[80];
  snprintf(v, sizeof v, "%d", (int)Keyer::getWpm() + d);
  Settings::apply("wpm", v, msg, sizeof msg);
}

void handleKey(const KeyEv& e) {
  keysSeen++;
  if (e.usage >= 0x3A && e.usage <= 0x3F) {            // F1..F6
    uint8_t n = e.usage - 0x3A + 1;
    if (!Memories::play(n)) Log::printf("[BT] F%u: memory is empty\n", n);
    return;
  }
  switch (e.usage) {
    case 0x29: WinKeyer::abort(); Keyer::tune(false); return;   // Esc
    case 0x4B: case 0x52: stepWpm(+1); return;                   // PgUp, Up
    case 0x4E: case 0x51: stepWpm(-1); return;                   // PgDn, Down
  }
  // Typed text goes out a character at a time, as a keyboard keyer does —
  // no Enter needed. Backspace cannot unsend, so it is ignored.
  char c = charFor(e.usage, e.mods & 0x22);
  if (c) { char s[2] = {c, 0}; WinKeyer::sendText(s); }
}

// ── scanning ──────────────────────────────────────────────
bool advIsHid(uint8_t* adv, uint16_t len, char* name, size_t nameLen) {
  uint8_t l = 0;
  name[0] = 0;
  uint8_t* n = esp_ble_resolve_adv_data_by_type(adv, len, ESP_BLE_AD_TYPE_NAME_CMPL, &l);
  if (!n) n = esp_ble_resolve_adv_data_by_type(adv, len, ESP_BLE_AD_TYPE_NAME_SHORT, &l);
  if (n) { size_t c = l < nameLen - 1 ? l : nameLen - 1; memcpy(name, n, c); name[c] = 0; }
  uint8_t* a = esp_ble_resolve_adv_data_by_type(adv, len, ESP_BLE_AD_TYPE_APPEARANCE, &l);
  if (a && l == 2 && (a[0] | a[1] << 8) == 0x03C1) return true;          // keyboard
  for (auto t : {ESP_BLE_AD_TYPE_16SRV_CMPL, ESP_BLE_AD_TYPE_16SRV_PART}) {
    uint8_t* u = esp_ble_resolve_adv_data_by_type(adv, len, t, &l);
    for (int i = 0; u && i + 1 < l; i += 2)
      if ((u[i] | u[i + 1] << 8) == 0x1812) return true;                 // HID service
  }
  return false;
}

// The operator's scan is active and dense so names arrive quickly. The
// background scan — looking only for the paired keyboard to wake up — is
// passive and ~2% duty, because every millisecond of it is air time WiFi
// does not get.
void startScan(Kind k) {
  static esp_ble_scan_params_t user = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE, .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50, .scan_window = 0x30,          // 50 / 30 ms
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
  };
  static esp_ble_scan_params_t bg = {
    .scan_type = BLE_SCAN_TYPE_PASSIVE, .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x800, .scan_window = 0x30,         // 1.28 s / 30 ms
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
  };
  scanKind = k;
  st = St::ScanStarting;
  esp_ble_gap_set_scan_params(k == Kind::User ? &user : &bg);   // starts on completion
}

void gapCb(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t* p) {
  switch (ev) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
      if (st != St::ScanStarting) break;
      esp_ble_gap_start_scanning(scanKind == Kind::User ? 10 : 30);
      st = (scanKind == Kind::User) ? St::Scanning : St::BgScan;
      break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
      auto& r = p->scan_rst;
      if (r.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
        if (st == St::Scanning || st == St::BgScan) goIdle();
        break;
      }
      if (r.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) break;
      // Waking up, a paired keyboard may send a bare reconnect advert with
      // no HID data in it, so the background scan matches the address alone.
      if (st == St::BgScan && target.valid && !memcmp(r.bda, target.bda, 6)) {
        want = target;
        want.type = r.ble_addr_type;
        st = St::OpenPending;
        esp_ble_gap_stop_scanning();
        break;
      }
      if (st != St::Scanning) break;
      char name[24];
      if (!advIsHid(r.ble_adv, r.adv_data_len + r.scan_rsp_len, name, sizeof name)) break;
      portENTER_CRITICAL(&mux);
      uint8_t i = 0;
      while (i < nHits && memcmp(hits[i].bda, r.bda, 6)) i++;
      if (i < MAX_HITS) {
        if (i == nHits) { nHits++; memcpy(hits[i].bda, r.bda, 6); hits[i].name[0] = 0; }
        hits[i].type = r.ble_addr_type;
        hits[i].rssi = (int8_t)r.rssi;
        if (name[0]) strlcpy(hits[i].name, name, sizeof hits[i].name);   // scan response
      }
      portEXIT_CRITICAL(&mux);
      break;
    }

    case ESP_GAP_BLE_SEC_REQ_EVT:
      esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr, true);
      break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
      passkey = (int32_t)p->ble_security.key_notif.passkey;
      break;
    case ESP_GAP_BLE_NC_REQ_EVT:
      esp_ble_confirm_reply(p->ble_security.key_notif.bd_addr, true);
      break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      passkey = -1;
      break;
    default:
      break;
  }
}

// ── HID host ──────────────────────────────────────────────
void hidCb(void*, esp_event_base_t, int32_t id, void* data) {
  auto* p = (esp_hidh_event_data_t*)data;
  switch ((esp_hidh_event_t)id) {
    case ESP_HIDH_OPEN_EVENT:
      if (!p->open.dev) { if (st == St::Opening) goIdle(); break; }
      kbd = p->open.dev;
      memset(prevKeys, 0, sizeof prevKeys);
      battery = -1;
      { const char* n = esp_hidh_dev_name_get(kbd); strlcpy(devName, n ? n : "", sizeof devName); }
      savePending = true;
      st = St::Connected;
      break;
    case ESP_HIDH_BATTERY_EVENT:
      battery = p->battery.level;
      break;
    case ESP_HIDH_INPUT_EVENT:
      if (p->input.dev == kbd && p->input.usage == ESP_HID_USAGE_KEYBOARD)
        onReport(p->input.data, p->input.length);
      break;
    case ESP_HIDH_CLOSE_EVENT: {
      bool wasKbd = (p->close.dev == kbd);
      esp_hidh_dev_free(p->close.dev);
      if (wasKbd) { kbd = nullptr; battery = -1; }
      // Not when a connect to a replacement is already under way.
      if (st == St::Connected || st == St::Idle) goIdle();
      break;
    }
    default:
      break;
  }
}

void openLoop(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // A failed connect (GATT 0x85 was seen) returns NULL and sends no open
    // event, so the failure has to be noticed here.
    if (!esp_hidh_dev_open(want.bda, ESP_HID_TRANSPORT_BLE, want.type) && st == St::Opening) {
      Log::printf("[BT] could not connect to %s\n", fmtBda(want.bda).c_str());
      goIdle();
    }
  }
}

const char* stateStr() {
  if (!activeFlag) return "off";
  if (kbd) return "connected";
  switch (st) {
    case St::ScanStarting:
    case St::Scanning:    return scanKind == Kind::User ? "scanning" : "waiting for keyboard";
    case St::OpenPending:
    case St::Opening:     return "connecting";
    default:              return target.valid ? "waiting for keyboard" : "not paired";
  }
}

}  // namespace

namespace Bt {

bool available() { return true; }
bool setting()   { return settingOn; }
bool active()    { return activeFlag; }

void setSetting(bool on) {
  settingOn = on;
  if (on && errHeap) {          // trying again: drop the old refusal note
    errHeap = 0;
    Preferences p;
    p.begin(NS, false);
    p.remove("bterr");
    p.end();
  }
}

void begin(bool enabled) {
  settingOn = enabled;
  {
    Preferences p;
    p.begin(NS, true);
    errHeap = p.isKey("bterr") ? p.getUInt("bterr", 0) : 0;
    p.end();
  }
  if (!enabled) return;
  uint32_t heap0 = ESP.getFreeHeap();
  // BLE-only controller: less memory, and no Classic radio time to share.
  if (!btStartMode(BT_MODE_BLE)) { Log::println("[BT] controller start failed"); return; }
  if (esp_bluedroid_init() != ESP_OK || esp_bluedroid_enable() != ESP_OK) {
    Log::println("[BT] bluedroid start failed");
    return;
  }
  esp_ble_gap_register_callback(gapCb);
  esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler);

  // "Display only": the page is the display. A keyboard that wants MITM
  // protection gets a passkey shown there; the rest pair with Just Works.
  esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_SC_BOND;
  esp_ble_io_cap_t   iocap = ESP_IO_CAP_OUT;
  uint8_t keySize = 16;
  uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, 1);
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, 1);
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &keySize, 1);
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, 1);
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, 1);

  keyQ = xQueueCreate(32, sizeof(KeyEv));
  esp_hidh_config_t cfg = { .callback = hidCb, .event_stack_size = 4096, .callback_arg = nullptr };
  if (esp_hidh_init(&cfg) != ESP_OK) { Log::println("[BT] HID host init failed"); return; }
  xTaskCreatePinnedToCore(openLoop, "btopen", 6144, nullptr, 2, &openTask, 0);

  uint32_t heap = ESP.getFreeHeap();
  if (heap < BT_MIN_FREE_HEAP) {
    // Switch it off and restart, rather than try to tear the stack down: a
    // fresh boot with the setting off gives the memory back for certain,
    // because initArduino() then releases it before anything allocates.
    Log::printf("[BT] only %u bytes free with Bluetooth up (need %u) — "
                "switching it off and restarting\n", (unsigned)heap,
                (unsigned)BT_MIN_FREE_HEAP);
    Preferences p;
    p.begin(NS, false);
    p.putUInt("bten", 0);
    p.putUInt("bterr", heap);
    p.end();
    delay(200);
    ESP.restart();
  }

  loadTarget();
  activeFlag = true;
  goIdle();
  Log::printf("[BT] keyboard host up — heap %u -> %u, %s\n", (unsigned)heap0,
              (unsigned)ESP.getFreeHeap(), target.valid ? target.name : "not paired");
}

void poll() {
  if (!activeFlag) return;
  KeyEv e;
  while (xQueueReceive(keyQ, &e, 0) == pdTRUE) handleKey(e);

  if (savePending) {
    savePending = false;
    target = want;
    target.valid = true;
    if (devName[0]) strlcpy(target.name, devName, sizeof target.name);
    saveTarget();
    removeBonds(target.bda);   // one keyboard: stale addresses must not pile up
    Log::printf("[BT] keyboard connected: %s\n", target.name);
  }

  static St       last = St::Idle;
  static uint32_t since = 0;
  if (st != last) { last = st; since = millis(); }

  switch (st) {
    case St::OpenPending:
      st = St::Opening;
      xTaskNotifyGive(openTask);
      break;
    case St::Idle:
      // Only look for the paired keyboard while it is away.
      if (!kbd && target.valid && millis() - idleSince > 3000) startScan(Kind::Bg);
      break;
    case St::ScanStarting:
    case St::Opening:
      if (millis() - since > 20000) goIdle();   // an event that never came
      break;
    default:
      break;
  }
}

bool scanStart() {
  if (!activeFlag) return false;
  if (st == St::OpenPending || st == St::Opening) return false;
  if (st == St::Scanning && scanKind == Kind::User) return true;
  if (st == St::BgScan || st == St::Scanning) esp_ble_gap_stop_scanning();
  portENTER_CRITICAL(&mux);
  nHits = 0;
  portEXIT_CRITICAL(&mux);
  startScan(Kind::User);
  return true;
}

bool connect(const char* addr, uint8_t addrType) {
  if (!activeFlag) return false;
  Target t{};
  if (!parseBda(addr, t.bda)) return false;
  t.type = addrType;
  portENTER_CRITICAL(&mux);
  for (uint8_t i = 0; i < nHits; i++)
    if (!memcmp(hits[i].bda, t.bda, 6)) strlcpy(t.name, hits[i].name, sizeof t.name);
  portEXIT_CRITICAL(&mux);
  if (kbd && target.valid && !memcmp(target.bda, t.bda, 6)) return true;   // already on it
  if (st == St::Scanning || st == St::BgScan) esp_ble_gap_stop_scanning();
  want = t;
  st = St::OpenPending;
  if (kbd) esp_hidh_dev_close(kbd);   // pairing another replaces this one
  return true;
}

void forget() {
  if (!activeFlag) return;
  if (st == St::Scanning || st == St::BgScan) esp_ble_gap_stop_scanning();
  if (kbd) esp_hidh_dev_close(kbd);
  removeBonds(nullptr);
  clearTarget();
  goIdle();
  Log::println("[BT] paired keyboard forgotten");
}

void toJson(JsonObject o, bool full) {
  o["en"]   = settingOn;
  o["on"]   = activeFlag;
  o["st"]   = stateStr();
  o["name"] = String(kbd ? devName : (target.valid ? target.name : ""));
  o["batt"] = (int)battery;
  o["pk"]   = (long)passkey;
  o["err"]  = errHeap;          // switched off at boot for want of heap
  if (!full) return;
  o["addr"]     = target.valid ? fmtBda(target.bda) : String("");
  o["keys"]     = keysSeen;
  o["heap"]     = ESP.getFreeHeap();
  o["minheap"]  = ESP.getMinFreeHeap();
  o["scanning"] = (st == St::Scanning || st == St::ScanStarting) && scanKind == Kind::User;
  Hit copy[MAX_HITS];
  portENTER_CRITICAL(&mux);
  uint8_t n = nHits;
  memcpy(copy, hits, sizeof(Hit) * n);
  portEXIT_CRITICAL(&mux);
  JsonArray a = o.createNestedArray("hits");
  for (uint8_t i = 0; i < n; i++) {
    JsonObject h = a.createNestedObject();
    h["addr"]   = fmtBda(copy[i].bda);          // String: copied into the doc
    h["name"]   = String(copy[i].name);
    h["rssi"]   = copy[i].rssi;
    h["type"]   = copy[i].type;
    h["paired"] = target.valid && !memcmp(copy[i].bda, target.bda, 6);
  }
}

}  // namespace Bt

#else   // no Bluedroid (the S3 core ships NimBLE): the switch exists, nothing runs

namespace Bt {
bool available() { return false; }
void begin(bool enabled) {
  settingOn = enabled;
  if (enabled) Log::println("[BT] no Bluedroid BLE HID host in this build — keyboard unavailable");
}
void poll() {}
bool setting()   { return settingOn; }
void setSetting(bool on) { settingOn = on; }
bool active()    { return false; }
bool scanStart() { return false; }
bool connect(const char*, uint8_t) { return false; }
void forget() {}
void toJson(JsonObject o, bool) {
  o["en"] = settingOn; o["on"] = false; o["st"] = "unsupported";
  o["name"] = ""; o["batt"] = -1; o["pk"] = -1; o["err"] = 0;
}
}  // namespace Bt

#endif
