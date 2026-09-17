// ============================================================
//  ble_kbd_probe.cpp — standalone BLE keyboard test, NOT the keyer.
//
//  Question it answers: can this classic ESP32, on the same Arduino core
//  as the keyer (3.3.11 / IDF 5.5), pair with a BLE keyboard and receive
//  keys reliably WHILE WiFi is connected — and what does that cost WiFi?
//
//  Build/flash (replaces the keyer firmware until you flash it back):
//    ENV=ble-kbd-probe ./flash.sh
//    ./monitor.sh 115200
//  Restore the keyer:  ./flash.sh
//  Keyer settings survive: they live in NVS, which a flash does not touch.
//
//  Put the keyboard in BLUETOOTH pairing mode (not its 2.4 GHz dongle
//  mode). The probe scans, connects to the first device advertising as a
//  keyboard / HID, pairs, bonds, and prints every key press with a
//  timestamp. Every 10 s it prints heap, WiFi RSSI and link state.
//
//  WiFi cost: while Bluetooth runs, ESP-IDF requires WiFi modem sleep ON,
//  which the keyer turns OFF (main.cpp) to kill ~300 ms of packet latency.
//  Measure it from the Mac while the keyboard is connected:
//    ping <ip printed at boot>
//
//  Serial commands (115200, over the board's own USB):
//    f  forget all bonded keyboards      s  status now
//
//  Uses ESP-IDF's esp_hid host (BLE transport) directly — the only HID
//  host the precompiled core ships. Classic-Bluetooth keyboards are not
//  supported by that build.
// ============================================================
#include <Arduino.h>
#include <WiFi.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_hidh.h"
#include "esp_wifi.h"

// The core frees the Bluetooth controller's memory in initArduino() unless
// a BT library claims it. We use IDF directly, so claim it ourselves —
// without this, the controller init fails (or crashes) at runtime.
extern "C" bool bleInUse(void) { return true; }

// ── state ─────────────────────────────────────────────────
enum class St { StartScan, Scanning, OpenPending, Opening, Connected };
static volatile St st = St::StartScan;

static esp_bd_addr_t     targetBda;
static esp_ble_addr_type_t targetAddrType;
static char              targetName[32];
static esp_hidh_dev_t   *kbd = nullptr;
static uint32_t          keysSeen = 0, reconnects = 0;
static uint32_t          connectedAt = 0;

static String bdaStr(const uint8_t *b) {
  char s[18];
  snprintf(s, sizeof s, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
  return s;
}

// ── HID usage → text ──────────────────────────────────────
// Boot keyboard report: [modifiers, reserved, key1..key6].
static const char *keyName(uint8_t u, bool shift) {
  static char one[2];
  if (u >= 0x04 && u <= 0x1D) { one[0] = (shift ? 'A' : 'a') + (u - 0x04); one[1] = 0; return one; }
  if (u >= 0x1E && u <= 0x27) {
    static const char n[] = "1234567890", s[] = "!@#$%^&*()";
    one[0] = (shift ? s : n)[u - 0x1E]; one[1] = 0; return one;
  }
  if (u >= 0x3A && u <= 0x45) {
    static const char *f[] = {"F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12"};
    return f[u - 0x3A];
  }
  switch (u) {
    case 0x28: return "Enter";   case 0x29: return "Esc";
    case 0x2A: return "Bksp";    case 0x2B: return "Tab";
    case 0x2C: return "Space";   case 0x2D: return shift ? "_" : "-";
    case 0x2E: return shift ? "+" : "=";  case 0x2F: return shift ? "{" : "[";
    case 0x30: return shift ? "}" : "]";  case 0x31: return shift ? "|" : "\\";
    case 0x33: return shift ? ":" : ";";  case 0x34: return shift ? "\"" : "'";
    case 0x35: return shift ? "~" : "`";  case 0x36: return shift ? "<" : ",";
    case 0x37: return shift ? ">" : ".";  case 0x38: return shift ? "?" : "/";
    case 0x39: return "CapsLock";
    case 0x49: return "Ins";     case 0x4A: return "Home";
    case 0x4B: return "PgUp";    case 0x4C: return "Del";
    case 0x4D: return "End";     case 0x4E: return "PgDn";
    case 0x4F: return "Right";   case 0x50: return "Left";
    case 0x51: return "Down";    case 0x52: return "Up";
  }
  return nullptr;
}

static void handleKeyboardReport(const uint8_t *d, uint16_t len) {
  // Some keyboards drop the reserved byte; accept both layouts.
  if (len != 8 && len != 7) return;
  static uint8_t prev[6];
  uint8_t mods = d[0];
  const uint8_t *k = (len == 8) ? d + 2 : d + 1;
  bool shift = mods & 0x22;
  for (int i = 0; i < 6; i++) {
    uint8_t u = k[i];
    if (u < 0x04) continue;                        // empty / rollover error
    if (memchr(prev, u, 6)) continue;              // still held, not new
    const char *name = keyName(u, shift);
    keysSeen++;
    if (name) Serial.printf("[%8lu] key %-6s (usage 0x%02x mods 0x%02x)\n", millis(), name, u, mods);
    else      Serial.printf("[%8lu] key ?      (usage 0x%02x mods 0x%02x)\n", millis(), u, mods);
  }
  memcpy(prev, k, 6);
}

// ── HID host events (own task, not the BT task) ───────────
static void hidhCb(void *, esp_event_base_t, int32_t id, void *data) {
  auto *p = (esp_hidh_event_data_t *)data;
  switch ((esp_hidh_event_t)id) {
    case ESP_HIDH_OPEN_EVENT:
      if (!p->open.dev) {
        Serial.printf("[HID] open FAILED (status %d) — rescanning\n", p->open.status);
        st = St::StartScan;
        break;
      }
      kbd = p->open.dev;
      connectedAt = millis();
      Serial.printf("[HID] CONNECTED  %s  \"%s\"  usage 0x%x  vid %04x pid %04x\n",
                    bdaStr(esp_hidh_dev_bda_get(kbd)).c_str(),
                    esp_hidh_dev_name_get(kbd) ? esp_hidh_dev_name_get(kbd) : "",
                    esp_hidh_dev_usage_get(kbd),
                    esp_hidh_dev_vendor_id_get(kbd), esp_hidh_dev_product_id_get(kbd));
      {
        // Which reports the host subscribed to — the thing to check when a
        // keyboard connects but no keys arrive.
        size_t n = 0;
        esp_hid_report_item_t *r = nullptr;
        if (esp_hidh_dev_reports_get(kbd, &n, &r) == ESP_OK) {
          for (size_t i = 0; i < n; i++)
            Serial.printf("[HID]   report map %u id %u usage 0x%x type %u mode %u len %u\n",
                          r[i].map_index, r[i].report_id, r[i].usage,
                          r[i].report_type, r[i].protocol_mode, r[i].value_len);
          free(r);
        }
      }
      Serial.println("[HID] type on the keyboard now");
      st = St::Connected;
      break;
    case ESP_HIDH_BATTERY_EVENT:
      Serial.printf("[HID] battery %u%%\n", p->battery.level);
      break;
    case ESP_HIDH_INPUT_EVENT:
      if (p->input.usage == ESP_HID_USAGE_KEYBOARD) {
        handleKeyboardReport(p->input.data, p->input.length);
      } else {
        Serial.printf("[%8lu] report usage 0x%x id %u len %u:", millis(),
                      p->input.usage, p->input.report_id, p->input.length);
        for (int i = 0; i < p->input.length; i++) Serial.printf(" %02x", p->input.data[i]);
        Serial.println();
      }
      break;
    case ESP_HIDH_CLOSE_EVENT:
      Serial.printf("[HID] DISCONNECTED reason 0x%x after %lu s — rescanning\n",
                    p->close.reason, (millis() - connectedAt) / 1000);
      esp_hidh_dev_free(p->close.dev);
      kbd = nullptr;
      reconnects++;
      st = St::StartScan;
      break;
    default:
      break;
  }
}

// ── GAP: scan + pairing (runs in the BT task — never block here) ──
static bool looksLikeKeyboard(uint8_t *adv, uint16_t len, char *name, size_t nameLen) {
  uint8_t l = 0;
  name[0] = 0;
  uint8_t *n = esp_ble_resolve_adv_data_by_type(adv, len, ESP_BLE_AD_TYPE_NAME_CMPL, &l);
  if (!n) n = esp_ble_resolve_adv_data_by_type(adv, len, ESP_BLE_AD_TYPE_NAME_SHORT, &l);
  if (n) { size_t c = l < nameLen - 1 ? l : nameLen - 1; memcpy(name, n, c); name[c] = 0; }

  uint8_t *a = esp_ble_resolve_adv_data_by_type(adv, len, ESP_BLE_AD_TYPE_APPEARANCE, &l);
  if (a && l == 2 && (a[0] | a[1] << 8) == 0x03C1) return true;   // Keyboard

  for (auto t : {ESP_BLE_AD_TYPE_16SRV_CMPL, ESP_BLE_AD_TYPE_16SRV_PART}) {
    uint8_t *u = esp_ble_resolve_adv_data_by_type(adv, len, t, &l);
    for (int i = 0; u && i + 1 < l; i += 2)
      if ((u[i] | u[i + 1] << 8) == 0x1812) return true;          // HID service
  }
  return false;
}

static void gapCb(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t *p) {
  switch (ev) {
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
      auto &r = p->scan_rst;
      if (r.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
        if (st == St::Scanning) st = St::StartScan;   // nothing found; go again
        break;
      }
      if (r.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT || st != St::Scanning) break;
      char name[32];
      if (!looksLikeKeyboard(r.ble_adv, r.adv_data_len + r.scan_rsp_len, name, sizeof name)) break;
      Serial.printf("[SCAN] keyboard/HID %s \"%s\" rssi %d addrtype %d\n",
                    bdaStr(r.bda).c_str(), name, r.rssi, r.ble_addr_type);
      memcpy(targetBda, r.bda, sizeof targetBda);
      targetAddrType = r.ble_addr_type;
      strlcpy(targetName, name, sizeof targetName);
      st = St::OpenPending;
      esp_ble_gap_stop_scanning();
      break;
    }
    case ESP_GAP_BLE_SEC_REQ_EVT:
      Serial.println("[SEC] keyboard requested security — accepting");
      esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr, true);
      break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
      Serial.printf("[SEC] >>> TYPE %06lu ON THE KEYBOARD, THEN ENTER <<<\n",
                    (unsigned long)p->ble_security.key_notif.passkey);
      break;
    case ESP_GAP_BLE_NC_REQ_EVT:
      Serial.printf("[SEC] numeric comparison %06lu — accepting\n",
                    (unsigned long)p->ble_security.key_notif.passkey);
      esp_ble_confirm_reply(p->ble_security.key_notif.bd_addr, true);
      break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
      auto &a = p->ble_security.auth_cmpl;
      if (a.success) Serial.printf("[SEC] paired/encrypted OK (auth mode 0x%x)\n", a.auth_mode);
      else           Serial.printf("[SEC] pairing FAILED reason 0x%x\n", a.fail_reason);
      break;
    }
    default:
      break;
  }
}

// ── bring-up ──────────────────────────────────────────────
static bool btUp() {
  // BLE-only controller: less memory, and no Classic radio time to share.
  if (!btStartMode(BT_MODE_BLE)) { Serial.println("[BT] controller start FAILED"); return false; }
  esp_err_t e;
  if ((e = esp_bluedroid_init()) != ESP_OK)   { Serial.printf("[BT] bluedroid init %s\n", esp_err_to_name(e)); return false; }
  if ((e = esp_bluedroid_enable()) != ESP_OK) { Serial.printf("[BT] bluedroid enable %s\n", esp_err_to_name(e)); return false; }
  esp_ble_gap_register_callback(gapCb);
  esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler);

  // Declare "display only": the serial console is the display. A keyboard
  // that wants MITM protection gets a passkey printed to type on it; one
  // that doesn't pairs with Just Works and nothing is printed.
  esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_SC_BOND;
  esp_ble_io_cap_t   iocap = ESP_IO_CAP_OUT;
  uint8_t keySize = 16;
  uint8_t initKey = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t rspKey  = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, 1);
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, 1);
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &keySize, 1);
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &initKey, 1);
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rspKey, 1);

  esp_hidh_config_t cfg = { .callback = hidhCb, .event_stack_size = 4096, .callback_arg = nullptr };
  if ((e = esp_hidh_init(&cfg)) != ESP_OK) { Serial.printf("[HID] init %s\n", esp_err_to_name(e)); return false; }

  static esp_ble_scan_params_t sp = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,          // active: names come in scan responses
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,                      // 50 ms
    .scan_window = 0x30,                        // 30 ms — leaves WiFi air time
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
  };
  esp_ble_gap_set_scan_params(&sp);
  return true;
}

static void status() {
  wifi_ps_type_t ps = WIFI_PS_NONE;
  esp_wifi_get_ps(&ps);
  int bonds = esp_ble_get_bond_device_num();
  Serial.printf("[STAT] up %lus  heap %u (min %u)  WiFi %s %s rssi %d ps=%d  "
                "kbd %s  keys %lu  reconnects %lu  bonds %d\n",
                millis() / 1000, ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                WiFi.isConnected() ? "up" : "DOWN", WiFi.localIP().toString().c_str(),
                WiFi.RSSI(), (int)ps, kbd ? "CONNECTED" : "none",
                keysSeen, reconnects, bonds);
}

static void forgetBonds() {
  int n = esp_ble_get_bond_device_num();
  if (n <= 0) { Serial.println("[SEC] no bonds"); return; }
  auto *list = (esp_ble_bond_dev_t *)malloc(sizeof(esp_ble_bond_dev_t) * n);
  esp_ble_get_bond_device_list(&n, list);
  for (int i = 0; i < n; i++) {
    Serial.printf("[SEC] forgetting %s\n", bdaStr(list[i].bd_addr).c_str());
    esp_ble_remove_bond_device(list[i].bd_addr);
  }
  free(list);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== BLE keyboard probe (esp32-vukeyer) ===");

  // Uses the WiFi credentials the keyer's WiFiManager already saved.
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin();
  for (int i = 0; i < 30 && !WiFi.isConnected(); i++) delay(500);
  if (WiFi.isConnected()) Serial.printf("[WiFi] up  %s  rssi %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else                    Serial.println("[WiFi] not connected (no saved creds?) — BLE test continues");

  Serial.printf("[MEM] heap before BT %u\n", ESP.getFreeHeap());
  if (!btUp()) { Serial.println("[BT] bring-up failed — halting"); return; }
  Serial.printf("[MEM] heap after BT  %u\n", ESP.getFreeHeap());
  Serial.println("[SCAN] put the keyboard in Bluetooth pairing mode");
  status();
}

void loop() {
  static uint32_t lastStat = 0;

  switch (st) {
    case St::StartScan:
      st = St::Scanning;
      esp_ble_gap_start_scanning(10);            // seconds; restarts on completion
      break;
    case St::OpenPending:
      // Opening blocks until the GATT database is read — must not run in the
      // GAP callback (BT task), so it happens here.
      st = St::Opening;
      Serial.printf("[HID] connecting to %s \"%s\"...\n", bdaStr(targetBda).c_str(), targetName);
      // A failed connect (e.g. GATT 0x85) returns NULL and sends NO open
      // event — without this check the probe waits in Opening forever.
      if (!esp_hidh_dev_open(targetBda, ESP_HID_TRANSPORT_BLE, targetAddrType) && st == St::Opening) {
        Serial.println("[HID] open returned NULL — rescanning in 2 s");
        delay(2000);
        st = St::StartScan;
      }
      break;
    default:
      break;
  }

  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'f') forgetBonds();
    if (c == 's') status();
  }

  if (millis() - lastStat >= 10000) { lastStat = millis(); status(); }
  delay(20);
}
