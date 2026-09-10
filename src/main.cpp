// ============================================================
//  ESP32 WinKeyer
//  WinKeyer (K1EL WK protocol) clone on ESP32 — WiFi TCP bridge,
//  FlexRadio backend, iambic paddle keying, sidetone, speed pot.
//
//  Start order matters: the keyer core comes up first and runs on
//  its own high-priority task, so the paddles work with no network
//  at all. WiFi onboarding is a NON-blocking WiFiManager portal.
//
//  Serial (115200) is a text CLI by default and switches itself to
//  the WinKeyer binary protocol the moment a host-open command
//  arrives — 0x00 is not a byte a human types, so the two uses
//  cannot be confused. Host close returns it to the CLI.
//
//    /wpm 25   /mode a|b   /swap        /tune       /pot on|off
//    /st 700   /st on|off  /ptt on|off  /status     /net
//    /backend local|flex   /flex on|off /flex ip <addr>
//    /wifi     /wifi portal /wifi reset
//    anything else is sent as CW.
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "config.h"
#include "pins.h"
#include "keyer.h"
#include "winkeyer.h"
#include "flex.h"
#include "net.h"

WiFiClient   net;
PubSubClient mqtt(net);
WiFiManager  wm;
unsigned long lastBeat = 0;
bool serialWkMode = false;

// ── MQTT ─────────────────────────────────────────────────
void onMqtt(char* topic, byte* payload, unsigned int len) {
  char msg[64] = {0};
  strncpy(msg, (char*)payload, len < sizeof(msg) - 1 ? len : sizeof(msg) - 1);
  Serial.printf("[MQTT] %s = %s\n", topic, msg);
}

bool mqttConnect() {
  // LWT: broker publishes this retained if we drop off uncleanly.
  bool ok = mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS,
                         T_STATUS, 1, true, "{\"event\":\"offline\"}");
  // Log transitions, not every retry. A broker that refuses our credentials
  // will refuse them forever, and repeating that once a minute buries the
  // keyer's own output on the console.
  static int lastRc = 999;
  if (ok) {
    Serial.println("[MQTT] connected");
    mqtt.publish(T_STATUS, "{\"event\":\"online\"}", true);
    lastRc = 999;
  } else if (mqtt.state() != lastRc) {
    lastRc = mqtt.state();
    Serial.printf("[MQTT] connect failed rc=%d%s (further retries silent)\n",
                  lastRc, lastRc == 5 ? " — bad credentials in secrets.h" : "");
  }
  return ok;
}

// ── Backend selection ─────────────────────────────────────
// Switching backend has three coupled side effects, so they live in one
// place rather than being repeated by the CLI and by boot-time restore.
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
    p.begin("wk", false);
    p.putBool("flexbe", useFlex);
    p.end();
  }
}

bool loadBackend() {
  Preferences p;
  p.begin("wk", true);
  bool useFlex = p.isKey("flexbe") ? p.getBool("flexbe", false) : false;
  p.end();
  return useFlex;
}

// ── Serial as a WinKeyer transport ────────────────────────
void serialSink(const uint8_t* data, size_t len) { Serial.write(data, len); }

// ── Serial CLI ────────────────────────────────────────────
void printStatus() {
  Serial.printf("[KEYER] wpm=%u mode=%s swap=%s sidetone=%uHz tune=%s busy=%s\n",
                Keyer::getWpm(),
                Keyer::getMode() == KEYER_IAMBIC_A ? "A" : "B",
                Keyer::getPaddleSwap() ? "on" : "off",
                Keyer::getSidetoneHz(),
                Keyer::tuning() ? "on" : "off",
                Keyer::busy() ? "yes" : "no");
  Serial.printf("[WK]    backend=%s host=%s\n",
                WinKeyer::getBackend() == WK_BACKEND_FLEX ? "flex" : "local",
                WinKeyer::hostOpen() ? "open" : "closed");
  Serial.printf("[FLEX]  %s radio=%s %s\n",
                Flex::enabled() ? "enabled" : "disabled",
                Flex::radioIp().length() ? Flex::radioIp().c_str() : "(not found)",
                Flex::connected() ? "connected" : "");
}

void printNet() {
  Serial.printf("[NET]   wifi=%s ip=%s rssi=%d\n",
                WiFi.status() == WL_CONNECTED ? "up" : "down",
                WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  Serial.printf("[NET]   %s.local:%d  client=%s\n",
                MDNS_HOSTNAME, WK_TCP_PORT,
                Net::clientConnected() ? "connected" : "none");
}

void handleLine(char* line) {
  if (line[0] == '\0') return;
  if (line[0] == '/') {
    char* cmd = strtok(line + 1, " ");
    char* arg = strtok(nullptr, " ");
    char* arg2 = strtok(nullptr, " ");
    if (!cmd) return;
    if (!strcasecmp(cmd, "wpm") && arg) {
      Keyer::setWpm(atoi(arg));
      Serial.printf("[KEYER] wpm=%u\n", Keyer::getWpm());
    } else if (!strcasecmp(cmd, "mode") && arg) {
      Keyer::setMode(tolower(arg[0]) == 'a' ? KEYER_IAMBIC_A : KEYER_IAMBIC_B);
      Serial.printf("[KEYER] mode=%c\n", toupper(arg[0]));
    } else if (!strcasecmp(cmd, "swap")) {
      Keyer::setPaddleSwap(!Keyer::getPaddleSwap());
      Serial.printf("[KEYER] swap=%s\n", Keyer::getPaddleSwap() ? "on" : "off");
    } else if (!strcasecmp(cmd, "tune")) {
      Keyer::tune(!Keyer::tuning());
      Serial.printf("[KEYER] tune=%s\n", Keyer::tuning() ? "on" : "off");
    } else if (!strcasecmp(cmd, "pot") && arg) {
      Keyer::setPotEnabled(!strcasecmp(arg, "on"));
      Serial.printf("[KEYER] pot=%s\n", arg);
    } else if (!strcasecmp(cmd, "ptt") && arg) {
      Keyer::setPttEnabled(!strcasecmp(arg, "on"));
      Serial.printf("[KEYER] ptt=%s\n", arg);
    } else if (!strcasecmp(cmd, "st") && arg) {
      if (!strcasecmp(arg, "on"))       Keyer::setSidetone(true);
      else if (!strcasecmp(arg, "off")) Keyer::setSidetone(false);
      else                              Keyer::setSidetoneHz(atoi(arg));
      Serial.printf("[KEYER] sidetone %s\n", arg);
    } else if (!strcasecmp(cmd, "backend") && arg) {
      bool useFlex = !strcasecmp(arg, "flex");
      applyBackend(useFlex, true);
      Serial.printf("[WK] backend=%s (paddle keying %s, saved)\n",
                    useFlex ? "flex" : "local",
                    useFlex ? "-> radio over network" : "-> local key output");
    } else if (!strcasecmp(cmd, "flex")) {
      if (arg && !strcasecmp(arg, "on"))       { Flex::setEnabled(true);  Serial.println("[FLEX] enabled"); }
      else if (arg && !strcasecmp(arg, "off")) { Flex::setEnabled(false); Serial.println("[FLEX] disabled"); }
      else if (arg && !strcasecmp(arg, "ip") && arg2) {
        Flex::setManualIp(arg2);
        Serial.printf("[FLEX] fixed IP %s\n", arg2);
      } else if (arg && !strcasecmp(arg, "auto")) {
        Flex::setManualIp("");
        Serial.println("[FLEX] using discovery");
      } else if (arg && !strcasecmp(arg, "bind") && arg2) {
        Flex::setBind(!strcasecmp(arg2, "on"));
        Serial.printf("[FLEX] client bind %s — reconnecting\n", arg2);
      } else if (arg && !strcasecmp(arg, "ptt") && arg2) {
        Flex::setUseXmit(!strcasecmp(arg2, "on"));
        Serial.printf("[FLEX] ptt(xmit) %s — with it off, break-in must "
                      "switch T/R from the key edge\n", arg2);
      } else {
        Serial.printf("[FLEX] %s radio=%s connected=%s\n",
                      Flex::enabled() ? "enabled" : "disabled",
                      Flex::radioIp().c_str(), Flex::connected() ? "yes" : "no");
      }
    } else if (!strcasecmp(cmd, "wifi")) {
      if (arg && !strcasecmp(arg, "reset")) {
        wm.resetSettings();
        Serial.println("[WiFi] credentials cleared — rebooting into the portal");
        delay(300);
        ESP.restart();
      } else if (arg && !strcasecmp(arg, "portal")) {
        wm.startConfigPortal(WIFI_AP_NAME, WIFI_AP_PASS);
        Serial.printf("[WiFi] portal open: %s\n", WIFI_AP_NAME);
      } else {
        Serial.printf("[WiFi] %s ssid=%s ip=%s\n",
                      WiFi.status() == WL_CONNECTED ? "connected" : "not connected",
                      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        Serial.printf("[WiFi] setup AP: %s / %s\n", WIFI_AP_NAME, WIFI_AP_PASS);
      }
    } else if (!strcasecmp(cmd, "paddle")) {
      // Bring-up diagnostic: reports the debounced levers for 10 s so a
      // wiring fault can be told apart from a firmware problem. Blocks the
      // host link while it runs — deliberate, it is a bench tool.
      Serial.println("[PADDLE] squeeze each lever — 10 s (idle = both open)");
      unsigned long until = millis() + 10000;
      bool pd = false, ph = false, sawAny = false;
      while (millis() < until) {
        bool d = Keyer::paddleDit(), h = Keyer::paddleDah();
        if (d != pd || h != ph) {
          pd = d; ph = h;
          if (d || h) sawAny = true;
          Serial.printf("[PADDLE] dit=%s dah=%s\n", d ? "DOWN" : "up", h ? "DOWN" : "up");
        }
        delay(5);
      }
      Serial.println(sawAny ? "[PADDLE] levers detected — wiring is good"
                            : "[PADDLE] nothing seen — check tip/ring to GPIO25/26 "
                              "and sleeve to GND");
    } else if (!strcasecmp(cmd, "net")) {
      printNet();
    } else if (!strcasecmp(cmd, "status")) {
      printStatus();
    } else {
      Serial.println("[CLI] /wpm /mode /swap /tune /pot /ptt /st /backend /flex /wifi /paddle /net /status");
    }
    return;
  }
  // Plain text → CW
  for (char* p = line; *p; p++) Keyer::sendChar(*p);
  Keyer::sendChar(' ');
  Serial.printf("[CW] > %s\n", line);
}

void pollSerial() {
  static char buf[80];
  static uint8_t len = 0;
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) return;

    if (serialWkMode) {
      WinKeyer::feed((uint8_t)c, serialSink);
      if (!WinKeyer::hostOpen()) {      // host closed — hand the port back to the CLI
        serialWkMode = false;
        len = 0;
        Serial.println("\n[WK] serial host closed — CLI active");
      }
      continue;
    }

    // A null byte starts a WinKeyer admin command — but a single stray 0x00
    // is exactly what line noise produces when a host opens the port, and
    // switching on that alone silently kills the CLI and leaves the port
    // spewing status bytes. So feed the 0x00 plus its sub-command, and only
    // commit to binary mode if that actually opened a host session.
    if (c == 0x00) {
      WinKeyer::feed(0x00, serialSink);
      unsigned long deadline = millis() + 50;
      int sub = -1;
      while (millis() < deadline) {
        if (Serial.available()) { sub = Serial.read(); break; }
      }
      if (sub >= 0) WinKeyer::feed((uint8_t)sub, serialSink);
      if (WinKeyer::hostOpen()) {
        serialWkMode = true;
        len = 0;
        Serial.println();     // the host ignores this; a human sees the switch
      } else {
        WinKeyer::closeHost();   // discard the partial command, stay on the CLI
      }
      continue;
    }

    if (c == '\n' || c == '\r') {
      buf[len] = '\0';
      handleLine(buf);
      len = 0;
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = (char)c;
    }
  }
}

// ── Setup ─────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n[BOOT] ESP32 WinKeyer");
  pinMode(PIN_STATUS_LED, OUTPUT);

  // Keyer first — it must work with no WiFi at all.
  Keyer::begin();
  WinKeyer::begin();
  Serial.printf("[KEYER] up — %u WPM, iambic B, sidetone %u Hz\n",
                Keyer::getWpm(), Keyer::getSidetoneHz());

  // Non-blocking onboarding: portal runs in the background, loop keeps running.
  wm.setHostname(MDNS_HOSTNAME);
  wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
  wm.setConfigPortalBlocking(false);
  Serial.println("[WiFi] autoConnect (portal: vu2cpl-esp32-winkeyer-setup)");
  wm.autoConnect(WIFI_AP_NAME, WIFI_AP_PASS);

  // The ESP32 defaults to modem sleep, waking only on DTIM beacons. That
  // adds 100–300 ms of latency and heavy jitter to every inbound packet —
  // measured at 307 ms average on the bench, on the same subnet. A keyer
  // is mains-powered and wants a responsive host link, so trade the ~20 mA.
  WiFi.setSleep(false);

  Net::begin();
  Flex::begin();

  // Restore the backend last: it needs Flex::begin() to have created the
  // key queue before the hook can be attached.
  bool useFlex = loadBackend();
  applyBackend(useFlex, false);
  Serial.printf("[WK] backend=%s (restored)\n", useFlex ? "flex" : "local");

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqtt);
  mqtt.setSocketTimeout(2);   // default is 15 s of blocked loop() on a bad link
}

// ── Loop ──────────────────────────────────────────────────
void loop() {
  wm.process();          // captive portal, when active
  pollSerial();
  Net::poll();
  WinKeyer::poll();
  Flex::poll();

  // MQTT is the lowest-priority thing here and the only blocking call in the
  // loop. PubSubClient::connect() waits on the socket, and while it waits the
  // WinKeyer host link is not being serviced — a logger sees the keyer stall.
  // So: never attempt it while CW is in flight, cap the wait, and back off
  // when the broker keeps refusing instead of stalling every 5 s forever.
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      static unsigned long lastTry = 0;
      static uint32_t      backoff = 5000;
      bool quiet = !Keyer::busy() && !WinKeyer::hostOpen();
      if (quiet && millis() - lastTry > backoff) {
        lastTry = millis();
        if (mqttConnect()) backoff = 5000;
        else               backoff = min<uint32_t>(backoff * 2, 60000);
      }
    }
    mqtt.loop();
  }

  if (millis() - lastBeat > 10000) {
    lastBeat = millis();
    digitalWrite(PIN_STATUS_LED, !digitalRead(PIN_STATUS_LED));
    StaticJsonDocument<256> doc;
    doc["event"]    = "heartbeat";
    doc["uptime_s"] = millis() / 1000;
    doc["rssi"]     = WiFi.RSSI();
    doc["wpm"]      = Keyer::getWpm();
    doc["busy"]     = Keyer::busy();
    doc["backend"]  = WinKeyer::getBackend() == WK_BACKEND_FLEX ? "flex" : "local";
    doc["wk_host"]  = WinKeyer::hostOpen();
    doc["tcp"]      = Net::clientConnected();
    if (Flex::enabled()) doc["flex"] = Flex::connected();
    char buf[256];
    serializeJson(doc, buf);
    if (mqtt.connected()) mqtt.publish(T_STATUS, buf, true);
  }
}
