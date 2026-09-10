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
//    /pot 10 35            /st 700      /st on|off  /ptt on|off
//    /disp on|off          /disp sh1106|ssd1306      /i2c (scan the bus)
//    /weight 50  /ratio 50  /farns 0  /lead 50  /tail 250
//    /backend local|flex   /flex on|off /flex ip <addr>
//    /wifi     /wifi portal /wifi reset  /status     /net
//    anything else is sent as CW.
//
//  Operator settings persist in NVS (see settings.cpp) and are also
//  editable from the web page at http://winkeyer.local/ — CLI and web
//  both go through Settings::apply(), so they cannot disagree.
// ============================================================

#include <Arduino.h>
#include <stdarg.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "log.h"
#include "pins.h"
#include "keyer.h"
#include "winkeyer.h"
#include "flex.h"
#include "net.h"
#include "display.h"
#include "settings.h"
#include "web.h"
#include "fsk.h"
#include "memories.h"

WiFiClient   net;
PubSubClient mqtt(net);
WiFiManager  wm;
unsigned long lastBeat = 0;
bool serialWkMode = false;
bool quiet = false;          // trim boot chatter when the link is slow

// Boot-time logging that costs the host link nothing when it matters.
void boot(const char* fmt, ...) {
  if (quiet) return;
  va_list ap; va_start(ap, fmt);
  char b[160]; vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  Serial.print(b);
}

// ── MQTT ─────────────────────────────────────────────────
void onMqtt(char* topic, byte* payload, unsigned int len) {
  char msg[64] = {0};
  strncpy(msg, (char*)payload, len < sizeof(msg) - 1 ? len : sizeof(msg) - 1);
  Log::printf("[MQTT] %s = %s\n", topic, msg);
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
    Log::println("[MQTT] connected");
    mqtt.publish(T_STATUS, "{\"event\":\"online\"}", true);
    lastRc = 999;
  } else if (mqtt.state() != lastRc) {
    lastRc = mqtt.state();
    Log::printf("[MQTT] connect failed rc=%d%s (further retries silent)\n",
                  lastRc, lastRc == 5 ? " — bad credentials in secrets.h" : "");
  }
  return ok;
}

// ── Serial as a WinKeyer transport ────────────────────────
void serialSink(const uint8_t* data, size_t len) { Serial.write(data, len); }

// ── Serial CLI ────────────────────────────────────────────
void printStatus() {
  Log::printf("[KEYER] wpm=%u mode=%s swap=%s sidetone=%uHz tune=%s busy=%s\n",
                Keyer::getWpm(),
                Keyer::getMode() == KEYER_IAMBIC_A ? "A" : "B",
                Keyer::getPaddleSwap() ? "on" : "off",
                Keyer::getSidetoneHz(),
                Keyer::tuning() ? "on" : "off",
                Keyer::busy() ? "yes" : "no");
  Log::printf("[KEYER] weight=%u ratio=%u farns=%u  ptt=%s lead=%ums tail=%ums\n",
                Keyer::getWeighting(), Keyer::getRatio(), Keyer::getFarnsworth(),
                Keyer::getPttEnabled() ? "on" : "off",
                Keyer::getPttLeadMs(), Keyer::getPttTailMs());
  Log::printf("[KEYER] pot=%s (%u-%u WPM on GPIO34)  display=%s\n",
                Keyer::getPotEnabled() ? "on" : "off",
                Keyer::getPotMin(), Keyer::getPotMin() + Keyer::getPotRange(),
                Display::present()
                  ? (Display::enabled() ? Display::controller() : "off")
                  : "not detected");
  Log::printf("[KEYER] serial %u baud %s — %s\n",
                (unsigned)Settings::hostBaud(),
                Settings::hostBaud() == 1200 ? "8N2" : "8N1",
                Settings::hostBaud() == 1200
                  ? "WinKeyer standard, loggers open the port this way"
                  : "console rate; a logger expecting a WinKeyer needs 1200");
  Log::printf("[WK]    backend=%s host=%s\n",
                WinKeyer::getBackend() == WK_BACKEND_FLEX ? "flex" : "local",
                WinKeyer::hostOpen() ? "open" : "closed");
  Log::printf("[FLEX]  %s radio=%s %s\n",
                Flex::enabled() ? "enabled" : "disabled",
                Flex::radioIp().length() ? Flex::radioIp().c_str() : "(not found)",
                Flex::connected() ? "connected" : "");
  if (Flex::connected())
    Log::printf("[FLEX]  keying: cw %s, slice %s — %s\n",
                  Flex::keyVerb(),
                  Flex::sliceReady() ? "CW/in use" : "NOT ready",
                  Flex::sliceReady() ? "ready to key"
                                     : "SmartSDR needs a slice in CW mode");
}

void printNet() {
  Log::printf("[NET]   wifi=%s ip=%s rssi=%d\n",
                WiFi.status() == WL_CONNECTED ? "up" : "down",
                WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  Log::printf("[NET]   %s.local:%d  client=%s\n",
                MDNS_HOSTNAME, WK_TCP_PORT,
                Net::clientConnected() ? "connected" : "none");
}

// Every persisted setting goes through Settings::apply() so the CLI and the
// web page validate identically and both end up in NVS.
void setting(const char* key, const char* val) {
  char msg[80];
  bool ok = Settings::apply(key, val, msg, sizeof msg);
  Log::printf("[%s] %s\n", ok ? "SET" : "ERR", msg);
}

void handleLine(char* line) {
  if (line[0] == '\0') return;

  // /fsk carries free text, so it is handled before tokenising — strtok
  // would cut it at the first space and there is no reliable way to sew
  // a line back together afterwards.
  if (!strncasecmp(line, "/call", 5) && (line[5] == ' ' || line[5] == '\0')) {
    char* c = line + 5;
    while (*c == ' ') c++;
    if (*c) Memories::setCall(c);
    Log::printf("[MEM] callsign = %s\n", Memories::call().c_str());
    return;
  }

  if (!strncasecmp(line, "/mem", 4) && (line[4] == ' ' || line[4] == '\0')) {
    char* r = line + 4;
    while (*r == ' ') r++;
    if (!*r) {                                   // list them
      for (uint8_t i = 1; i <= Memories::COUNT; i++) {
        String m = Memories::get(i);
        Log::printf("[MEM] %u: %s\n", i, m.length() ? m.c_str() : "(empty)");
      }
      return;
    }
    uint8_t slot = (uint8_t)atoi(r);
    char* text = strchr(r, ' ');
    if (text) { while (*text == ' ') text++; }
    if (slot < 1 || slot > Memories::COUNT) {
      Log::printf("[MEM] slot must be 1..%u\n", Memories::COUNT);
    } else if (text && *text) {
      Log::printf(Memories::set(slot, text) ? "[MEM] %u stored\n"
                                            : "[MEM] %u too long\n", slot);
    } else if (!Memories::play(slot)) {
      Log::printf("[MEM] %u is empty\n", slot);
    }
    return;
  }

  if (!strncasecmp(line, "/fsk", 4) && (line[4] == ' ' || line[4] == '\0')) {
    char* rest = line + 4;
    while (*rest == ' ') rest++;
    if (!*rest) {
      Log::printf("[FSK] %.2f baud, mark=%s, diddle=%s, %s (%u queued)\n",
                  Fsk::baud(), Fsk::invert() ? "low" : "high",
                  Fsk::diddle() ? "on" : "off",
                  Fsk::busy() ? "SENDING" : "idle", (unsigned)Fsk::pending());
    } else if (!strncasecmp(rest, "stop", 4)) {
      Fsk::abort();
      Log::println("[FSK] stopped");
    } else if (!strncasecmp(rest, "baud ", 5))   { setting("fskbaud", rest + 5);
    } else if (!strncasecmp(rest, "invert ", 7)) { setting("fskinv", rest + 7);
    } else if (!strncasecmp(rest, "diddle ", 7)) { setting("fskdiddle", rest + 7);
    } else if (!Fsk::send(rest)) {
      Log::println("[FSK] buffer full");
    } else {
      Log::printf("[FSK] > %s\n", rest);
    }
    return;
  }

  if (line[0] == '/') {
    char* cmd = strtok(line + 1, " ");
    char* arg = strtok(nullptr, " ");
    char* arg2 = strtok(nullptr, " ");
    if (!cmd) return;
    if (!strcasecmp(cmd, "swap")) {
      // Toggles, so they read the current value rather than taking one.
      setting("swap", Keyer::getPaddleSwap() ? "off" : "on");
    } else if (!strcasecmp(cmd, "tune")) {
      Keyer::tune(!Keyer::tuning());        // never persisted — it is an action
      Log::printf("[KEYER] tune=%s\n", Keyer::tuning() ? "on" : "off");
    } else if (!strcasecmp(cmd, "pot") && arg) {
      // "/pot on|off" toggles the knob, "/pot 10 35" sets its range.
      if (isdigit((unsigned char)arg[0]) && arg2) {
        // Each half is validated against the other, so raising the range
        // must widen it before narrowing it — otherwise "/pot 40 55" is
        // rejected against the old 10-35 max before the max has moved.
        if (atoi(arg) >= Keyer::getPotMin() + Keyer::getPotRange()) {
          setting("potmax", arg2); setting("potmin", arg);
        } else {
          setting("potmin", arg);  setting("potmax", arg2);
        }
      } else {
        setting("pot", arg);
      }
    } else if (!strcasecmp(cmd, "disp") && arg) {
      bool onoff = !strcasecmp(arg, "on") || !strcasecmp(arg, "off");
      setting(onoff ? "disp" : "dispctl", arg);
    } else if (!strcasecmp(cmd, "radio")  && arg) { setting("radio", arg);
    } else if (!strcasecmp(cmd, "pecho")  && arg) { setting("pecho", arg);
    } else if (!strcasecmp(cmd, "monitor") && arg) { setting("monitor", arg);
    } else if (!strcasecmp(cmd, "baud")   && arg) { setting("baud", arg);
    } else if (!strcasecmp(cmd, "weight") && arg) { setting("weight", arg);
    } else if (!strcasecmp(cmd, "ratio")  && arg) { setting("ratio", arg);
    } else if (!strcasecmp(cmd, "farns")  && arg) { setting("farns", arg);
    } else if (!strcasecmp(cmd, "lead")   && arg) { setting("lead", arg);
    } else if (!strcasecmp(cmd, "tail")   && arg) { setting("tail", arg);
    } else if (!strcasecmp(cmd, "wpm")   && arg) { setting("wpm", arg);
    } else if (!strcasecmp(cmd, "mode")  && arg) { setting("mode", arg);
    } else if (!strcasecmp(cmd, "ptt")   && arg) { setting("ptt", arg);
    } else if (!strcasecmp(cmd, "st")    && arg) { setting("st", arg);
    } else if (!strcasecmp(cmd, "backend") && arg) {
      setting("backend", arg);
      Log::printf("[WK] paddle keying %s\n",
                    !strcasecmp(arg, "flex") ? "-> radio over network"
                                             : "-> local key output");
    } else if (!strcasecmp(cmd, "flex")) {
      if (arg && !strcasecmp(arg, "on"))       { Flex::setEnabled(true);  Log::println("[FLEX] enabled"); }
      else if (arg && !strcasecmp(arg, "off")) { Flex::setEnabled(false); Log::println("[FLEX] disabled"); }
      else if (arg && !strcasecmp(arg, "ip") && arg2) {
        Flex::setManualIp(arg2);
        Log::printf("[FLEX] fixed IP %s\n", arg2);
      } else if (arg && !strcasecmp(arg, "auto")) {
        Flex::setManualIp("");
        Log::println("[FLEX] using discovery");
      } else if (arg && !strcasecmp(arg, "cmd") && arg2) {
        Flex::setKeyVerb(arg2);
        Log::printf("[FLEX] keying command: cw %s\n", Flex::keyVerb());
      } else if (arg && !strcasecmp(arg, "bind") && arg2) {
        Flex::setBind(!strcasecmp(arg2, "on"));
        Log::printf("[FLEX] client bind %s — reconnecting\n", arg2);
      } else if (arg && !strcasecmp(arg, "ptt") && arg2) {
        Flex::setUseXmit(!strcasecmp(arg2, "on"));
        Log::printf("[FLEX] ptt(xmit) %s — with it off, break-in must "
                      "switch T/R from the key edge\n", arg2);
      } else {
        Log::printf("[FLEX] %s radio=%s connected=%s\n",
                      Flex::enabled() ? "enabled" : "disabled",
                      Flex::radioIp().c_str(), Flex::connected() ? "yes" : "no");
      }
    } else if (!strcasecmp(cmd, "wifi")) {
      if (arg && !strcasecmp(arg, "reset")) {
        wm.resetSettings();
        Log::println("[WiFi] credentials cleared — rebooting into the portal");
        delay(300);
        ESP.restart();
      } else if (arg && !strcasecmp(arg, "portal")) {
        wm.startConfigPortal(WIFI_AP_NAME, WIFI_AP_PASS);
        Log::printf("[WiFi] portal open: %s\n", WIFI_AP_NAME);
      } else {
        Log::printf("[WiFi] %s ssid=%s ip=%s\n",
                      WiFi.status() == WL_CONNECTED ? "connected" : "not connected",
                      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        Log::printf("[WiFi] setup AP: %s / %s\n", WIFI_AP_NAME, WIFI_AP_PASS);
      }
    } else if (!strcasecmp(cmd, "paddle")) {
      // Bring-up diagnostic: reports the debounced levers for 10 s so a
      // wiring fault can be told apart from a firmware problem. Blocks the
      // host link while it runs — deliberate, it is a bench tool.
      Log::println("[PADDLE] squeeze each lever — 10 s (idle = both open)");
      unsigned long until = millis() + 10000;
      bool pd = false, ph = false, sawAny = false;
      while (millis() < until) {
        bool d = Keyer::paddleDit(), h = Keyer::paddleDah();
        if (d != pd || h != ph) {
          pd = d; ph = h;
          if (d || h) sawAny = true;
          Log::printf("[PADDLE] dit=%s dah=%s\n", d ? "DOWN" : "up", h ? "DOWN" : "up");
        }
        delay(5);
      }
      Log::println(sawAny ? "[PADDLE] levers detected — wiring is good"
                            : "[PADDLE] nothing seen — check tip/ring to GPIO25/26 "
                              "and sleeve to GND");
    } else if (!strcasecmp(cmd, "i2c")) {
      Display::scan();
    } else if (!strcasecmp(cmd, "net")) {
      printNet();
    } else if (!strcasecmp(cmd, "status")) {
      printStatus();
    } else {
      Log::println("[CLI] /wpm /mode /swap /tune /pot /ptt /st /disp /i2c\n"
                     "      /weight /ratio /farns /lead /tail /baud /monitor /pecho\n"
                     "      /fsk <text> | /fsk baud|invert|diddle|stop\n"
                     "      /radio 1|2|both   /mem N [text]   /call <sign>\n"
                     "      /backend /flex /wifi /paddle /net /status");
    }
    return;
  }
  // Plain text → CW
  WinKeyer::sendText(line);
  Log::printf("[CW] > %s\n", line);
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
        Log::setMuted(false);
        Log::println("\n[WK] serial host closed — CLI active");
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
        // From here every byte on this wire is protocol. A console line
        // would be read by the logger as CW text and shown in its window,
        // so the console goes quiet until the host closes.
        Log::setMuted(true);
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
  // Baud first, before anything is printed. A logger that opens this port
  // has already sent its Host Open by the time we get here, and a real
  // WinKeyer answers at 1200 8N2 — see WK_HOST_BAUD_DEFAULT.
  uint32_t baud = Settings::hostBaud();
  Serial.begin(baud, baud == 1200 ? SERIAL_8N2 : SERIAL_8N1);
  quiet = Settings::quietBoot();
  if (quiet) {
    // Every character here is one the host waits through before its
    // handshake is answered. One line, then silence.
    Log::printf("\n[BOOT] ESP32 WinKeyer @ %u 8N2 (quiet — use the web page)\n",
                  (unsigned)baud);
    wm.setDebugOutput(false);
  } else {
    Log::printf("\n[BOOT] ESP32 WinKeyer @ %u baud\n", (unsigned)baud);
  }
  pinMode(PIN_STATUS_LED, OUTPUT);

  // Keyer first — it must work with no WiFi at all.
  Keyer::begin();
  WinKeyer::begin();
  Display::begin();     // optional panel; silently absent if none is wired
  Fsk::begin();
  Memories::begin();
  boot("[KEYER] up — %u WPM, iambic B, sidetone %u Hz\n",
       Keyer::getWpm(), Keyer::getSidetoneHz());

  // Non-blocking onboarding: portal runs in the background, loop keeps running.
  wm.setHostname(MDNS_HOSTNAME);
  wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
  wm.setConfigPortalBlocking(false);
  boot("[WiFi] autoConnect (portal: %s)\n", WIFI_AP_NAME);
  wm.autoConnect(WIFI_AP_NAME, WIFI_AP_PASS);

  // The ESP32 defaults to modem sleep, waking only on DTIM beacons. That
  // adds 100–300 ms of latency and heavy jitter to every inbound packet —
  // measured at 307 ms average on the bench, on the same subnet. A keyer
  // is mains-powered and wants a responsive host link, so trade the ~20 mA.
  WiFi.setSleep(false);

  Net::begin();
  Flex::begin();
  Web::begin();

  // Restore settings last: the backend needs Flex::begin() to have created
  // the key queue before the hook can be attached, and the display needs its
  // controller choice before the first frame goes out.
  Settings::begin();
  bool useFlex = Settings::loadBackend();
  Settings::applyBackend(useFlex, false);
  boot("[WK] backend=%s, %u WPM, pot %s (restored)\n",
       useFlex ? "flex" : "local", Keyer::getWpm(),
       Keyer::getPotEnabled() ? "on" : "off");

  Keyer::chirp('R');      // "roger" — sidetone only, the board is up

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqtt);
  mqtt.setSocketTimeout(2);   // default is 15 s of blocked loop() on a bad link
}

// ── Loop ──────────────────────────────────────────────────
void loop() {
  wm.process();          // captive portal, when active
  pollSerial();
  // Keep the radio's cwx speed in step with the keyer's, whatever changed
  // it. Flex::setWpm() used to be reachable only from a host's in-band
  // speed escape, so a speed set from the pot, the web page, the CLI or the
  // WK set-speed command moved the local keyer and left the radio sending
  // at its previous rate. The two then ran at different speeds and the
  // error grew with the length of the transmission — visible as PTT held
  // progressively longer on long overs. Polled here rather than hooked
  // because the pot updates from the 1 kHz task, which must not do network
  // work.
  if (WinKeyer::getBackend() == WK_BACKEND_FLEX) {
    static uint8_t lastWpmToRadio = 0;
    uint8_t w = Keyer::getWpm();
    if (w != lastWpmToRadio) { lastWpmToRadio = w; Flex::setWpm(w); }
  }

  Net::poll();
  Web::poll();
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
