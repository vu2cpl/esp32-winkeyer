// ============================================================
//  ESP32 WinKeyer — settings web server
//
//  Synchronous WebServer on port 80. Serviced from loop(), never
//  from the keyer task: serving the page takes a few ms of socket
//  work and element timing must not see it.
//
//  Routes:
//    GET  /              the settings console (one static page)
//    GET  /api/state     everything the page renders, as JSON
//    POST /api/set?k=..&v=..   one setting, via Settings::apply()
//    POST /api/send?t=..       queue text as CW
//    POST /api/tune?v=on|off   key down for tuning
//
//  Deliberately no authentication: this is shack-LAN kit on a
//  trusted VLAN, same posture as the WinKeyer TCP port next to it.
// ============================================================

#include "web.h"
#include "config.h"
#include "settings.h"
#include "keyer.h"
#include "winkeyer.h"
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <WiFi.h>

namespace {

WebServer server(80);
bool started = false;

// Styled after soft-MORCONI's console: dark chassis, amber panel labels,
// LED-style status dots. Fonts are system stacks, not Google Fonts — the
// keyer is often on a VLAN with no route to the internet, and a page that
// waits on fonts.googleapis.com would stall for every operator.
const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WinKeyer — Settings</title><style>
:root{--chassis:#2d2d30;--dark:#1a1a1c;--bezel:#0a0a0b;--label:#d8cfb8;
--dim:#8a8275;--amber:#ffaa22;--green:#2aff5a;--red:#ff2a1a;--off:#3a1a18}
*{box-sizing:border-box}
body{margin:0;padding:18px;background:#0c0c0e radial-gradient(ellipse at top,#1a1a1c,#050505);
color:var(--label);font:14px/1.45 ui-monospace,Menlo,Consolas,monospace;min-height:100vh}
.rig{max-width:720px;margin:0 auto;padding:16px;border-radius:12px;
background:linear-gradient(180deg,#4a4a50 0%,#2d2d30 30%,#1f1f22 100%);
box-shadow:inset 0 2px 0 rgba(255,255,255,.18),inset 0 -3px 0 rgba(0,0,0,.7),0 24px 48px rgba(0,0,0,.7)}
.brand{display:flex;justify-content:space-between;align-items:baseline;
padding-bottom:10px;border-bottom:2px solid var(--bezel);margin-bottom:14px}
.brand b{font-size:19px;letter-spacing:3px;color:var(--amber);text-shadow:0 0 12px rgba(255,170,34,.5)}
.brand span{font-size:11px;color:var(--dim)}
.leds{display:flex;gap:14px;flex-wrap:wrap;padding:10px 12px;margin-bottom:14px;
background:var(--bezel);border-radius:8px;font-size:11px;letter-spacing:1px}
.led{display:flex;align-items:center;gap:6px;color:var(--dim)}
.led i{width:9px;height:9px;border-radius:50%;background:var(--off);display:inline-block}
.led.on i{background:var(--green);box-shadow:0 0 8px var(--green)}
.led.warn i{background:var(--amber);box-shadow:0 0 8px var(--amber)}
.speed{background:var(--bezel);border-radius:8px;padding:12px 16px;margin-bottom:14px;
display:flex;align-items:baseline;gap:12px}
.speed b{font-size:44px;color:var(--amber);text-shadow:0 0 16px rgba(255,170,34,.45);line-height:1}
.speed span{color:var(--dim);font-size:12px;letter-spacing:2px}
fieldset{border:1px solid #44444a;border-radius:8px;margin:0 0 12px;padding:10px 14px 14px}
legend{color:var(--amber);font-size:11px;letter-spacing:2px;padding:0 6px}
.row{display:flex;align-items:center;gap:10px;margin:8px 0;flex-wrap:wrap}
.row label{flex:0 0 108px;color:var(--dim);font-size:12px}
input,select,button{font:inherit;background:#131315;color:var(--label);
border:1px solid #44444a;border-radius:5px;padding:5px 9px}
input[type=range]{flex:1;min-width:150px;padding:0;border:none;background:none;
-webkit-appearance:none;appearance:none;height:18px}
input[type=range]::-webkit-slider-runnable-track{height:5px;border-radius:3px;
background:#131315;box-shadow:inset 0 1px 2px rgba(0,0,0,.9)}
input[type=range]::-moz-range-track{height:5px;border-radius:3px;background:#131315}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;
width:15px;height:15px;margin-top:-5px;border-radius:50%;border:1px solid #1a1a1c;
background:linear-gradient(180deg,#d8cfb8,#8a8275);box-shadow:0 0 7px rgba(255,170,34,.55)}
input[type=range]::-moz-range-thumb{width:15px;height:15px;border-radius:50%;
border:1px solid #1a1a1c;background:linear-gradient(180deg,#d8cfb8,#8a8275)}
input[type=checkbox]{accent-color:var(--amber)}
input[type=text],input[type=number]{width:92px}
button{cursor:pointer;background:linear-gradient(180deg,#44444a,#26262a);letter-spacing:1px}
button:hover{border-color:var(--amber);color:var(--amber)}
button.hot{color:var(--red);border-color:#5a2420}
.val{color:var(--amber);min-width:56px;font-size:13px}
.hint{font-size:11px;color:var(--dim);margin-top:8px;line-height:1.4}
#msg{min-height:18px;font-size:12px;color:var(--green);margin-top:4px}
#msg.err{color:var(--red)}
.foot{margin-top:12px;font-size:11px;color:var(--dim);text-align:center}
</style></head><body><div class="rig">
<div class="brand"><b>WINKEYER</b><span>VU2CPL &middot; ESP32 &middot; K1EL WK3</span></div>

<div class="leds">
<div class="led" id="l-host"><i></i>HOST</div>
<div class="led" id="l-tcp"><i></i>TCP</div>
<div class="led" id="l-key"><i></i>KEY</div>
<div class="led" id="l-tune"><i></i>TUNE</div>
<div class="led" id="l-pot"><i></i>POT</div>
<div class="led" id="l-flex"><i></i>FLEX</div>
<div class="led" id="l-disp"><i></i>OLED</div>
</div>

<div class="speed"><b id="wpmBig">--</b><span>WPM</span><span id="src"></span></div>

<fieldset><legend>KEYER</legend>
<div class="row"><label>Speed</label>
  <input type="range" id="wpm" min="5" max="60"><span class="val" id="wpmV"></span></div>
<div class="row"><label>Mode</label>
  <select id="mode"><option value="a">Iambic A</option><option value="b">Iambic B</option></select>
  <label style="flex:0 0 auto"><input type="checkbox" id="swap"> swap paddles</label></div>
<div class="row"><label>Sidetone</label>
  <input type="range" id="sthz" min="300" max="2000" step="10"><span class="val" id="sthzV"></span></div>
</fieldset>

<fieldset><legend>TIMING</legend>
<div class="row"><label>Weight</label>
  <input type="range" id="weight" min="10" max="90"><span class="val" id="weightV"></span></div>
<div class="row"><label>Dah ratio</label>
  <input type="range" id="ratio" min="33" max="66"><span class="val" id="ratioV"></span></div>
<div class="row"><label>Farnsworth</label>
  <input type="number" id="farns" min="0" max="60"><span class="val">WPM &mdash; 0 = off</span></div>
<div class="hint">50 is nominal for both sliders. Weight shifts the mark/space
balance without changing WPM; ratio changes dah length.</div>
</fieldset>

<fieldset><legend>PTT</legend>
<div class="row"><label>Line</label>
  <label style="flex:0 0 auto"><input type="checkbox" id="ptt"> enabled (GPIO32)</label>
  <label style="flex:0 0 auto"><input type="checkbox" id="st"> sidetone</label></div>
<div class="row"><label>Lead-in</label>
  <input type="number" id="lead" min="0" max="2000"><span class="val">ms before the first element</span></div>
<div class="row"><label>Tail</label>
  <input type="number" id="tail" min="0" max="2000"><span class="val">ms after the last</span></div>
<div class="hint" id="pttNote"></div>
<div class="hint">Click a number box and use &uarr;/&darr; to step by 1,
Shift+&uarr;/&darr; by 10. The value is written once you pause, so holding a
key costs one save, not one per repeat.</div>
</fieldset>

<fieldset><legend>SPEED POT</legend>
<div class="row"><label>Knob</label>
  <label style="flex:0 0 auto"><input type="checkbox" id="pot"> enabled (GPIO34)</label></div>
<div class="row"><label>Range</label>
  <input type="number" id="potmin" min="5" max="59"> to
  <input type="number" id="potmax" min="6" max="60"><span class="val">WPM</span></div>
</fieldset>

<fieldset><legend>DISPLAY</legend>
<div class="row"><label>Panel</label>
  <label style="flex:0 0 auto"><input type="checkbox" id="disp"> enabled</label>
  <select id="dispctl"><option value="sh1106">SH1106 (1.3")</option>
  <option value="ssd1306">SSD1306 (0.96")</option></select></div>
</fieldset>

<fieldset><legend>SERIAL / USB</legend>
<div class="row"><label>Host baud</label>
  <select id="baud">
    <option value="1200">1200 8N2 &mdash; WinKeyer standard</option>
    <option value="9600">9600 8N1</option>
    <option value="19200">19200 8N1</option>
    <option value="38400">38400 8N1</option>
    <option value="57600">57600 8N1</option>
    <option value="115200">115200 8N1 &mdash; console</option>
  </select></div>
<div class="hint" id="baudNote"></div>
</fieldset>

<fieldset><legend>BACKEND</legend>
<div class="row"><label>Keying</label>
  <select id="backend"><option value="local">Local key line</option>
  <option value="flex">FlexRadio (network)</option></select>
  <span class="val" id="flexip"></span></div>
</fieldset>

<fieldset><legend>SEND</legend>
<div class="row"><input type="text" id="txt" style="flex:1;width:auto" placeholder="CQ TEST VU2CPL">
  <button onclick="send()">SEND</button>
  <button onclick="post('/api/tune?v=on')">TUNE</button>
  <button class="hot" onclick="post('/api/tune?v=off')">STOP</button></div>
<div id="msg"></div>
</fieldset>

<div class="foot">winkeyer.local &middot; settings persist in NVS</div>
</div><script>
const $=i=>document.getElementById(i);
let editing=null,pend=null;
function note(t,err){const m=$('msg');m.textContent=t;m.className=err?'err':''}
async function post(u){const r=await fetch(u,{method:'POST'});const t=await r.text();
  note(t,!r.ok);refresh()}
function set(k,v){post('/api/set?k='+k+'&v='+encodeURIComponent(v))}
function send(){const t=$('txt').value.trim();if(!t)return;
  post('/api/send?t='+encodeURIComponent(t));$('txt').value=''}
function led(id,on,warn){const e=$(id);e.className='led'+(on?(warn?' warn':' on'):'')}
async function refresh(){
  let s;try{s=await(await fetch('/api/state')).json()}catch(e){return}
  $('wpmBig').textContent=s.wpm;
  $('src').textContent=s.pot?'FROM KNOB':'HOST/WEB SET';
  led('l-host',s.host);led('l-tcp',s.tcp);led('l-key',s.key);
  led('l-tune',s.tune,true);led('l-pot',s.pot);
  led('l-flex',s.flex.enabled&&s.flex.connected,s.flex.enabled&&!s.flex.slice);
  led('l-disp',s.disp&&s.disphw);
  $('flexip').textContent=s.flex.enabled?(s.flex.ip||'searching'):'';
  $('baudNote').textContent = s.baud==1200
    ? 'A logger opening this port as a WinKeyer expects 1200 8N2. The serial '
      +'console runs at this rate too, so the boot log is trimmed to one line '
      +'to keep the handshake fast — use this page for status.'
    : 'Readable console rate. A logger looking for a WinKeyer will NOT talk to '
      +'the port at this setting unless you can set its baud to match.';
  $('pttNote').textContent = s.backend==='flex'
    ? 'Backend is FlexRadio: Tail releases both the local PTT line (GPIO32) '
      +'and the radio itself (xmit 0). Lead-in applies to GPIO32 only — still '
      +'live here for an amp or sequencer — since the radio does its own T/R. '
      +'The KEY line (GPIO33) is idle on this backend.'
    : 'Backend is local: lead-in and tail both sequence the PTT line on GPIO32, '
      +'around the KEY line on GPIO33.';
  const fill={wpm:s.wpm,sthz:s.sthz,mode:s.mode,potmin:s.potmin,potmax:s.potmax,
    backend:s.backend,dispctl:s.dispctl,baud:String(s.baud),
    weight:s.weight,ratio:s.ratio,
    farns:s.farns,lead:s.lead,tail:s.tail};
  for(const k in fill) if(editing!==k) $(k).value=fill[k];
  $('swap').checked=s.swap;$('pot').checked=s.pot;$('disp').checked=s.disp;
  $('ptt').checked=s.ptt;$('st').checked=s.st;
  $('wpmV').textContent=$('wpm').value+' WPM';
  $('sthzV').textContent=$('sthz').value+' Hz';
  $('weightV').textContent=$('weight').value+(s.weight==50?' (nominal)':'');
  $('ratioV').textContent=$('ratio').value+(s.ratio==50?' (nominal)':'');
}
// Sliders: track the label live, but only write on release — one NVS
// commit per drag instead of one per pixel.
for(const [id,lbl,suf] of [['wpm','wpmV',' WPM'],['sthz','sthzV',' Hz'],
                           ['weight','weightV',''],['ratio','ratioV','']]){
  $(id).oninput=e=>{editing=id;$(lbl).textContent=e.target.value+suf};
  $(id).onchange=e=>{editing=null;set(id,e.target.value)};
}
// Number boxes. Two things make these painful without help: the 1 Hz poll
// overwrites whatever you are part-way through entering, and every keypress
// would otherwise be its own POST and its own NVS write. So: claim the field
// while it has focus, and debounce the write so holding an arrow key costs
// one commit at the end rather than one per repeat.
function bindNum(id,min,max){
  const e=$(id);
  e.onfocus=()=>editing=id;
  e.onblur =()=>{editing=null;clearTimeout(pend);set(id,e.value)};
  e.oninput=()=>{editing=id;clearTimeout(pend);pend=setTimeout(()=>set(id,e.value),500)};
  e.onkeydown=ev=>{
    if(ev.key==='Enter'){clearTimeout(pend);set(id,e.value);e.blur();return}
    if(ev.key!=='ArrowUp'&&ev.key!=='ArrowDown')return;
    ev.preventDefault();                       // step ourselves so it clamps
    const d=(ev.shiftKey?10:1)*(ev.key==='ArrowUp'?1:-1);
    e.value=Math.max(min,Math.min(max,(parseInt(e.value,10)||0)+d));
    editing=id; clearTimeout(pend);
    pend=setTimeout(()=>set(id,e.value),400);
  };
}
bindNum('farns',0,60); bindNum('lead',0,2000); bindNum('tail',0,2000);
bindNum('potmin',5,59); bindNum('potmax',6,60);
for(const id of ['mode','backend','dispctl','baud'])
  $(id).onchange=e=>set(id,e.target.value);
for(const id of ['swap','pot','disp','ptt','st'])
  $(id).onchange=e=>set(id,e.target.checked?'on':'off');
$('txt').addEventListener('keydown',e=>{if(e.key==='Enter')send()});
refresh();setInterval(refresh,1000);
</script></body></html>)HTML";

void handleState() {
  StaticJsonDocument<1024> doc;
  Settings::toJson(doc);
  doc["rssi"] = (int)WiFi.RSSI();
  doc["ip"]   = WiFi.localIP().toString();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSet() {
  if (!server.hasArg("k") || !server.hasArg("v")) {
    server.send(400, "text/plain", "need k and v");
    return;
  }
  char msg[80];
  bool ok = Settings::apply(server.arg("k").c_str(), server.arg("v").c_str(),
                            msg, sizeof msg);
  Serial.printf("[WEB] %s=%s -> %s\n", server.arg("k").c_str(),
                server.arg("v").c_str(), msg);
  server.send(ok ? 200 : 400, "text/plain", msg);
}

void handleSend() {
  String t = server.arg("t");
  if (!t.length()) { server.send(400, "text/plain", "nothing to send"); return; }
  for (size_t i = 0; i < t.length(); i++) Keyer::sendChar(t[i]);
  Keyer::sendChar(' ');
  Serial.printf("[WEB] > %s\n", t.c_str());
  server.send(200, "text/plain", String("sent: ") + t);
}

void handleTune() {
  bool on = server.arg("v") == "on";
  Keyer::tune(on);
  server.send(200, "text/plain", on ? "tune on — key down" : "tune off");
}

}  // namespace

namespace Web {

void begin() {
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html", PAGE); });
  server.on("/api/state", HTTP_GET,  handleState);
  server.on("/api/set",   HTTP_POST, handleSet);
  server.on("/api/send",  HTTP_POST, handleSend);
  server.on("/api/tune",  HTTP_POST, handleTune);
  server.onNotFound([]() { server.send(404, "text/plain", "no such page"); });
  // Listening is deferred to poll(): WiFiManager is non-blocking, so at
  // setup() time there is usually no IP to bind to yet. Net::poll() brings
  // mDNS up on the same transition and registers the http service there.
}

void poll() {
  if (WiFi.status() != WL_CONNECTED) {
    if (started) { server.stop(); started = false; }
    return;
  }
  if (!started) {
    server.begin();
    started = true;
    Serial.printf("[WEB] settings at http://%s.local/ or http://%s/\n",
                  MDNS_HOSTNAME, WiFi.localIP().toString().c_str());
  }
  server.handleClient();
}

}  // namespace Web
