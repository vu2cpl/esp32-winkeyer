// ============================================================
//  ESP32 WinKeyer
//  WinKeyer (K1EL WK3 protocol) clone on ESP32 — WiFi TCP bridge,
//  iambic paddle keying, sidetone, speed pot.
//
//  The keyer core (src/keyer.cpp) starts first and runs on its own
//  high-priority task — the keyer keys even with no WiFi. WiFi
//  onboarding is a NON-blocking WiFiManager portal; MQTT reports
//  status to the shack broker once the network is up.
//
//  Serial test CLI (115200) until the WK3 engine lands:
//    /wpm 25    /mode a|b    /swap      /tune      /pot on|off
//    /st 700    /st on|off   /ptt on|off            /status
//    anything else is sent as CW.
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "pins.h"
#include "keyer.h"

WiFiClient   net;
PubSubClient mqtt(net);
WiFiManager  wm;
unsigned long lastBeat = 0;

// ── MQTT ─────────────────────────────────────────────────
void onMqtt(char* topic, byte* payload, unsigned int len) {
  char msg[64] = {0};
  strncpy(msg, (char*)payload, len < sizeof(msg) - 1 ? len : sizeof(msg) - 1);
  Serial.printf("[MQTT] %s = %s\n", topic, msg);
}

bool mqttConnect() {
  Serial.print("[MQTT] connecting… ");
  // LWT: broker publishes this retained if we drop off uncleanly.
  bool ok = mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS,
                         T_STATUS, 1, true, "{\"event\":\"offline\"}");
  if (ok) {
    Serial.println("ok");
    mqtt.publish(T_STATUS, "{\"event\":\"online\"}", true);
  } else {
    Serial.printf("failed rc=%d\n", mqtt.state());
  }
  return ok;
}

// ── Serial test CLI ───────────────────────────────────────
void printStatus() {
  Serial.printf("[KEYER] wpm=%u mode=%s swap=%s sidetone=%uHz tune=%s busy=%s\n",
                Keyer::getWpm(),
                Keyer::getMode() == KEYER_IAMBIC_A ? "A" : "B",
                Keyer::getPaddleSwap() ? "on" : "off",
                Keyer::getSidetoneHz(),
                Keyer::tuning() ? "on" : "off",
                Keyer::busy() ? "yes" : "no");
}

void handleLine(char* line) {
  if (line[0] == '\0') return;
  if (line[0] == '/') {
    char* cmd = strtok(line + 1, " ");
    char* arg = strtok(nullptr, " ");
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
    } else if (!strcasecmp(cmd, "status")) {
      printStatus();
    } else {
      Serial.println("[CLI] /wpm N /mode a|b /swap /tune /pot on|off /ptt on|off /st N|on|off /status");
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
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      buf[len] = '\0';
      handleLine(buf);
      len = 0;
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
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
  Serial.printf("[KEYER] up — %u WPM, iambic B, sidetone %u Hz\n",
                Keyer::getWpm(), Keyer::getSidetoneHz());

  // Non-blocking onboarding: portal runs in the background, loop keeps running.
  wm.setHostname("esp32-winkeyer");
  wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
  wm.setConfigPortalBlocking(false);
  Serial.println("[WiFi] autoConnect (portal: vu2cpl-esp32-winkeyer-setup)");
  wm.autoConnect(WIFI_AP_NAME, WIFI_AP_PASS);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqtt);
}

// ── Loop ──────────────────────────────────────────────────
void loop() {
  wm.process();          // captive portal, when active
  pollSerial();

  if (Keyer::paddleBreakIn()) Serial.println("[KEYER] paddle break-in — buffer cleared");

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      static unsigned long lastTry = 0;
      if (millis() - lastTry > 5000) { mqttConnect(); lastTry = millis(); }
    }
    mqtt.loop();
  }

  if (millis() - lastBeat > 10000) {
    lastBeat = millis();
    digitalWrite(PIN_STATUS_LED, !digitalRead(PIN_STATUS_LED));
    StaticJsonDocument<160> doc;
    doc["event"]    = "heartbeat";
    doc["uptime_s"] = millis() / 1000;
    doc["rssi"]     = WiFi.RSSI();
    doc["wpm"]      = Keyer::getWpm();
    doc["busy"]     = Keyer::busy();
    char buf[160];
    serializeJson(doc, buf);
    if (mqtt.connected()) mqtt.publish(T_STATUS, buf, true);
  }
}
