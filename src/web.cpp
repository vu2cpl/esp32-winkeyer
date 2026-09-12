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
#include "log.h"
#include "config.h"
#include "settings.h"
#include "keyer.h"
#include "fsk.h"
#include "memories.h"
#include "winkeyer.h"
#include "flex.h"
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
body{margin:0;padding:8px;background:#0c0c0e radial-gradient(ellipse at top,#1a1a1c,#050505);
color:var(--label);font:14px/1.45 ui-monospace,Menlo,Consolas,monospace;min-height:100vh}
/* Panels tile into as many columns as the window allows, so the whole
   console fits one screen on a desktop and falls back to a single column
   on a phone. auto-fit + minmax does that without a breakpoint per size. */
.rig{max-width:1500px;margin:0 auto;padding:10px;border-radius:12px;
display:grid;grid-template-columns:repeat(auto-fit,minmax(420px,1fr));
gap:6px 12px;align-items:start;
background:linear-gradient(180deg,#4a4a50 0%,#2d2d30 30%,#1f1f22 100%);
box-shadow:inset 0 2px 0 rgba(255,255,255,.18),inset 0 -3px 0 rgba(0,0,0,.7),0 24px 48px rgba(0,0,0,.7)}
/* Full-width furniture: the header, the lamps, a warning, the big speed
   readout and the footer read across the whole console, not per column. */
.rig>.brand,.rig>.leds,.rig>.slicewarn,.rig>.speed,.rig>.foot{grid-column:1/-1}
/* The backend panel carries the most rows; give it two columns when there
   is room, and let it collapse with everything else when there is not. */
@media(min-width:1040px){
  .wide{grid-column:span 2}
  /* Lamps and the speed readout share one line instead of taking two. */
  .rig>.leds{grid-column:1/-2}
  .rig>.speed{grid-column:-2/-1;margin-bottom:0}
}
.brand{display:flex;justify-content:space-between;align-items:baseline;
padding-bottom:8px;border-bottom:2px solid var(--bezel);margin-bottom:4px}
.brand b{font-size:19px;letter-spacing:3px;color:var(--amber);text-shadow:0 0 12px rgba(255,170,34,.5)}
.brand span{font-size:11px;color:var(--dim)}
/* no display rule here, so the hidden attribute still hides it */
.slicewarn{padding:9px 12px;margin:-6px 0 14px;border-radius:8px;font-size:13px;
background:rgba(255,170,34,.12);border:1px solid var(--amber);color:var(--amber)}
.leds{display:flex;gap:14px;flex-wrap:wrap;padding:8px 12px;margin-bottom:2px;
background:var(--bezel);border-radius:8px;font-size:11px;letter-spacing:1px}
.led{display:flex;align-items:center;gap:6px;color:var(--dim)}
.led i{width:9px;height:9px;border-radius:50%;background:var(--off);display:inline-block}
.led.on i{background:var(--green);box-shadow:0 0 8px var(--green)}
.led.warn i{background:var(--amber);box-shadow:0 0 8px var(--amber)}
.speed{background:var(--bezel);border-radius:8px;padding:8px 16px;margin-bottom:2px;
display:flex;align-items:baseline;gap:12px}
.speed b{font-size:34px;color:var(--amber);text-shadow:0 0 16px rgba(255,170,34,.45);line-height:1}
.speed span{color:var(--dim);font-size:12px;letter-spacing:2px}
fieldset{border:1px solid #44444a;border-radius:8px;margin:0;padding:6px 12px 9px;
min-width:0}   /* min-width:0 or a long row stretches its grid column */
/* Rows tile inside a panel too — a label plus one control is ~230px, so
   two or three sit side by side in a wide panel instead of stacking. */
fieldset{display:grid;grid-template-columns:repeat(auto-fit,minmax(222px,1fr));
gap:2px 14px;align-content:start}
fieldset>legend{grid-column:1/-1}
/* Anything with a slider, a free-text field or its own buttons wants the
   full width of its panel; :has covers the sliders without marking each. */
.row.full,.row:has(input[type=range]),.row:has(input[type=text]):not(.mem){grid-column:1/-1}

legend{color:var(--amber);font-size:11px;letter-spacing:2px;padding:0 6px}
.row{display:flex;align-items:center;gap:9px;margin:4px 0;flex-wrap:wrap}
/* An author rule beats the hidden attribute's default display:none, so
   .row{display:flex} kept "hidden" rows on screen. */
.row[hidden]{display:none!important}
.row label{flex:0 0 92px;color:var(--dim);font-size:12px}
input,select,button{font:inherit;background:#131315;color:var(--label);
border:1px solid #44444a;border-radius:5px;padding:3px 8px;font-size:13px}
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
button{cursor:pointer;background:linear-gradient(180deg,#44444a,#26262a);
letter-spacing:.5px;padding:3px 9px;font-size:12px}
button:hover{border-color:var(--amber);color:var(--amber)}
button.hot{color:var(--red);border-color:#5a2420}
.val{color:var(--amber);min-width:56px;font-size:13px}
/* Memories: six rows that must each stay on ONE line, or the panel becomes
   the tallest thing on the page and nothing else fits beside it. */
.row.mem{margin:3px 0;gap:6px}
.row.mem button{padding:4px 7px;font-size:11px;letter-spacing:0}
.row label[title]{border-bottom:1px dotted var(--dim);cursor:help}
legend[title]{cursor:help}
#msg{min-height:16px;font-size:12px;color:var(--green);margin-top:4px}
#msg.err{color:var(--red)}
.foot{margin-top:4px;font-size:11px;color:var(--dim);text-align:center}
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
<div class="slicewarn" id="slicewarn" hidden></div>

<div class="speed"><b id="wpmBig">--</b><span>WPM</span><span id="src"></span></div>

<fieldset><legend>KEYER</legend>
<div class="row"><label title="Sending speed in words per minute (PARIS timing: dit = 1200/WPM ms). A WinKeyer host or the speed pot can override this; only what you set here is saved.">Speed</label>
  <input type="range" id="wpm" min="5" max="60"><span class="val" id="wpmV"></span></div>
<div class="row"><label title="Echo of characters you send on the PADDLE, so a logger can capture hand-sent text. This is WinKeyer mode register bit 6, separate from character echo of buffered text. Auto follows what the host asks for — but RUMlogNG sets 0x07 and never requests it, so force it On if you want hand-sent text logged.">Paddle echo</label>
  <select id="pecho"><option value="auto">Auto (follow host)</option>
  <option value="on">On</option><option value="off">Off</option></select>
  <span class="val" id="pechoState"></span></div>
<div class="row"><label title="Iambic A releases both paddles to stop after the current element; iambic B sends one more. Swap exchanges dit and dah if the paddle is wired the other way round.">Mode</label>
  <select id="mode"><option value="a">Iambic A</option><option value="b">Iambic B</option></select>
  <label style="flex:0 0 auto"><input type="checkbox" id="swap"> swap paddles</label></div>
<div class="row"><label title="Monitor tone pitch in Hz, 300-2000. Local only — it never reaches the air.">Sidetone</label>
  <input type="range" id="sthz" min="300" max="2000" step="10"><span class="val" id="sthzV"></span></div>
</fieldset>

<fieldset><legend>TIMING</legend>
<div class="row"><label title="Mark/space balance, 10-90, nominal 50. Higher makes elements longer and gaps shorter WITHOUT changing the WPM. Raise it a little if your fist sounds clipped on the air.">Weight</label>
  <input type="range" id="weight" min="10" max="90"><span class="val" id="weightV"></span></div>
<div class="row"><label title="Dah length relative to a dit, 33-66, nominal 50 = the standard 3 dits. Away from 50 the CW stops being standard-weight, so move it only to match a fist you already like.">Dah ratio</label>
  <input type="range" id="ratio" min="33" max="66"><span class="val" id="ratioV"></span></div>
<div class="row"><label title="0 = off. Otherwise characters stay at the Speed above while the GAPS stretch to this slower WPM — the standard way to learn at speed. Must be 0 or 5-60; 1-4 is not a legal value.">Farnsworth</label>
  <input type="number" id="farns" min="0" max="60"><span class="val">WPM</span></div>
</fieldset>

<fieldset><legend id="legPtt" title="">PTT</legend>
<div class="row"><label title="The PTT line itself: GPIO32, and GPIO19 for radio 2. Unticked, PTT is never asserted at all. It stays live on both backends, for an amp or a sequencer.">PTT line</label>
  <label style="flex:0 0 auto"><input type="checkbox" id="ptt"> enabled</label></div>
<div class="row"><label title="Master on/off for the tone in your ear, from the piezo on GPIO4. Off means silence regardless of anything else.">Audio</label>
  <label style="flex:0 0 auto"><input type="checkbox" id="st"> sidetone</label></div>
<div class="row"><label title="Delay in ms between asserting PTT and the first element, so a relay or amp has time to switch. Applies to the local GPIO32 line; the Flex radio does its own T/R.">Lead-in</label>
  <input type="number" id="lead" min="0" max="2000"><span class="val">ms</span></div>
<div class="row"><label title="How long PTT is held after the last element, in ms. Releases BOTH the local line and, on the Flex backend, the radio. A useful reference: one word gap is 7 dits = 8400/WPM ms, so 400 ms is exactly one word space at 21 WPM.">Tail</label>
  <input type="number" id="tail" min="0" max="2000"><span class="val">ms</span></div>
</fieldset>

<fieldset><legend>SPEED POT</legend>
<div class="row"><label title="10k linear pot on GPIO34, wiper to the pin, 100nF to GND. Leave this OFF until one is actually wired: the pin floats and noise will drive your speed. The knob overrides a host-set speed the moment you turn it.">Knob</label>
  <label style="flex:0 0 auto"><input type="checkbox" id="pot"> enabled</label></div>
<div class="row"><label title="WPM at each end of the knob travel. Expect a small dead zone at the top: the ESP32 ADC saturates near 3.1 V rather than 3.3 V.">Range</label>
  <input type="number" id="potmin" min="5" max="59"> to
  <input type="number" id="potmax" min="6" max="60"><span class="val">WPM</span></div>
</fieldset>

<fieldset><legend>DISPLAY</legend>
<div class="row"><label title="Any I2C panel on 21/22, probed at boot. The FAMILY is auto-detected — OLEDs answer at 0x3C/0x3D, HD44780 LCD backpacks at 0x27/0x3F — so one firmware runs whichever is plugged in, and Auto-detect gets you back to the OLED after trying an LCD. What cannot be detected: SH1106 vs SSD1306 (same address; wrong choice shifts the image 2px right with a garbage left edge) and 16x2 vs 20x4 (same chip; wrong choice just truncates). Run /i2c to scan the bus.">Panel</label>
  <label style="flex:0 0 auto"><input type="checkbox" id="disp"> enabled</label>
  <select id="dispctl">
    <option value="auto">Auto-detect</option>
    <option value="sh1106">OLED SH1106 (1.3")</option>
    <option value="ssd1306">OLED SSD1306 (0.96")</option>
    <option value="lcd20x4">LCD 20x4 (I&sup2;C)</option>
    <option value="lcd16x2">LCD 16x2 (I&sup2;C)</option>
  </select></div>
</fieldset>

<fieldset><legend id="legSerial" title="">SERIAL / USB</legend>
<div class="row"><label title="WiFi transmit power. Lower draws less current in each transmit burst, which is what browns out a board on a marginal USB supply — paddle keying sends a packet per key edge, about twenty bursts a second, and this board reset within two characters at full power. Lower also means less range: it does not affect how well you hear the AP, only how well it hears you. 11 dBm was enough to stop the resets here. The real fix is a 470-1000uF capacitor across 3V3 at the board, after which full power can come back.">WiFi power</label>
  <select id="txpower">
    <option value="19">19 dBm (full)</option>
    <option value="17">17 dBm</option>
    <option value="15">15 dBm</option>
    <option value="13">13 dBm</option>
    <option value="11">11 dBm (low draw)</option>
    <option value="8">8 dBm</option>
    <option value="5">5 dBm</option>
    <option value="2">2 dBm (minimum)</option>
  </select>
  <span class="val" id="rssiVal"></span></div>
<div class="row"><label title="1200 8N2 is the K1EL WinKeyer standard and what loggers open the port with — at any other rate the handshake arrives as noise and the keyer looks dead. The console shares this port, so at 1200 the boot log is trimmed to one line. This page is unaffected by the serial rate, so it is the way back if you pick a rate you cannot monitor at.">Host baud</label>
  <select id="baud">
    <option value="1200">1200 8N2 &mdash; WinKeyer standard</option>
    <option value="9600">9600 8N1</option>
    <option value="19200">19200 8N1</option>
    <option value="38400">38400 8N1</option>
    <option value="57600">57600 8N1</option>
    <option value="115200">115200 8N1 &mdash; console</option>
  </select></div>
</fieldset>

<fieldset class="wide"><legend>BACKEND</legend>
<div class="row"><label title="Enable the FlexRadio backend: discovery, connection and keying over the network. Harmless with no radio present — it simply listens for a discovery broadcast that never arrives. Separate from Keying below, which decides where your CW actually goes.">FlexRadio</label>
  <label style="flex:0 0 auto"><input type="checkbox" id="flex"> enabled</label>
  <span class="val" id="flexState"></span></div>
<div class="row flexonly"><label title="Pin the radio's address. Discovery is a raw UDP broadcast and does not cross subnets or VLANs, so if the radio is on a different segment from the keyer it will never be found automatically — use Find radio below. Leave blank to use discovery.">Radio IP</label>
  <input type="text" id="flexip" style="width:130px" placeholder="auto (discovery)"></div>
<div class="row flexonly"><label title="Discovery only hears a radio on the keyer's own subnet — it listens for a UDP broadcast, and routers do not pass those between subnets or VLANs. SCAN tries every address in one /24 for the radio's API port (TCP 4992) instead, which does cross a router, and lists what answers; click one to use it. Leave the box blank to scan the keyer's own subnet, or type the first three numbers of the radio's (e.g. 192.168.1). Takes about 15 seconds. Read-only: it asks each radio what it is and disconnects.">Find radio</label>
  <input type="text" id="scannet" style="width:100px">
  <button onclick="scan()">SCAN</button>
  <span class="val" id="scanState"></span>
  <span id="scanHits"></span></div>
<div class="row flexonly"><label title="The radio generates buffered CW itself via cwx send, so this keyer produces no elements and no sound while the rig transmits. Monitor runs a second copy of that text through the local keyer purely to make sidetone, so you can hear what is going out. Needs Audio/sidetone on as well. Does nothing on the local backend, where the keyer makes the elements itself.">Monitor</label>
  <label style="flex:0 0 auto"><input type="checkbox" id="monitor"> sound what the radio sends</label></div>
<div class="row flexonly"><label title="The radio starts sending a few hundred ms after it is handed the text — network, then its own CW start — while the local sidetone copy starts at once, so the sidetone runs AHEAD of the air. This holds the copy back to match. Auto uses the delay the keyer measures from cwx send to the radio actually transmitting; set a number to override, 0 to disable.">Sidetone delay</label>
  <input type="number" id="mondelay" min="0" max="2000" placeholder="auto">
  <label style="flex:0 0 auto"><input type="checkbox" id="mondelayauto"> auto</label>
  <span class="val" id="mondelayNow"></span></div>
<div class="row flexonly"><label title="Which sub-command keys the radio. FlexRadio's wiki documents 'cw ptt'; MORCONI's author uses 'cw key'. Both are accepted by the radio and only a power meter can say which one actually keys, so it is switchable.">Key verb</label>
  <select id="flexcmd"><option value="key">cw key</option><option value="ptt">cw ptt</option></select>
  <label style="flex:0 0 auto"><input type="checkbox" id="flexbind"> bind GUI</label>
  <label style="flex:0 0 auto"><input type="checkbox" id="flexxmit"> xmit</label></div>
<div class="row"><label title="Which KEY/PTT pair the keyer drives. Radio 1 is GPIO33/32, radio 2 is GPIO18/19. Both keys them together — intended for a rig plus an amp or monitor, but it does mean two transmitters key at once.">Radio</label>
  <select id="radio"><option value="1">Radio 1</option>
  <option value="2">Radio 2</option><option value="both">Both</option></select></div>
<div class="row"><label title="Local keys the wire: KEY on GPIO33, PTT on GPIO32. FlexRadio keys the radio over the network instead and leaves GPIO33 idle so the rig is not keyed twice — it needs a slice in use and in CW mode or the radio transmits nothing and reports no error.">Keying</label>
  <select id="backend"><option value="local">Local key line</option>
  <option value="flex">FlexRadio (network)</option></select>
  <span class="val" id="flexip"></span></div>
</fieldset>

<fieldset><legend title="Six canned messages kept in flash, played through whichever backend is current. %C in the text expands to your callsign, so a memory survives a contest call change. No GPIO cost — front-panel buttons can be wired to these later.">MEMORIES</legend>
<div class="row"><label title="Expands wherever %C appears in a memory.">Callsign</label>
  <input type="text" id="call" style="width:120px" placeholder="VU2CPL"></div>
<div id="mems"></div>
</fieldset>

<fieldset><legend title="RTTY FSK keying line on GPIO27: Baudot at 45.45 baud, 1 start bit, 5 data bits, 1.5 stop bits, mark when idle. Invert if your rig wants mark low — wrong polarity prints as reversed-case gibberish at the far end rather than silence. Diddle sends LTRS while the transmitter is up with nothing to say, keeping the far end synchronised between overs. PTT is held for the whole over, not per character.">FSK / RTTY</legend>
<div class="row"><input type="text" id="fsktxt" style="flex:1;width:auto" placeholder="RYRYRY DE VU2CPL">
  <button onclick="fsksend()">SEND</button>
  <button class="hot" onclick="post('/api/fsk?stop=1')">STOP</button></div>
<div class="row"><label title="45.45 baud is standard amateur RTTY. 75 is used on some commercial circuits.">Baud</label>
  <select id="fskbaud"><option value="45.45">45.45 (standard)</option>
  <option value="50">50</option><option value="75">75</option></select>
  <label style="flex:0 0 auto"><input type="checkbox" id="fskinv"> invert</label>
  <label style="flex:0 0 auto"><input type="checkbox" id="fskdid"> diddle</label>
  <span class="val" id="fskState"></span></div>
</fieldset>

<fieldset><legend title="Type text and press Enter or SEND to transmit it. TUNE keys continuously for tuning an amp; STOP ends it. Number boxes on this page step with the arrow keys, Shift for 10.">SEND</legend>
<div class="row"><input type="text" id="txt" style="flex:1;width:auto" placeholder="CQ TEST VU2CPL">
  <button onclick="send()">SEND</button>
  <button onclick="post('/api/tune?v=on')">TUNE</button>
  <button class="hot" onclick="post('/api/tune?v=off')">STOP</button></div>
<div id="msg"></div>
</fieldset>

<div class="foot" id="foot">winkeyer.local &middot; settings persist in NVS</div>
</div><script>
const $=i=>document.getElementById(i);
let editing=null,pend=null;
// Held so it can be put back once Flex is enabled again.
const flexOpt=document.querySelector('#backend option[value="flex"]');
function note(t,err){const m=$('msg');m.textContent=t;m.className=err?'err':''}
async function post(u){const r=await fetch(u,{method:'POST'});const t=await r.text();
  note(t,!r.ok);refresh()}
const KEYMAP={fskbaud:'fskbaud',fskinv:'fskinv',fskdid:'fskdiddle',
              flexbind:'flexbind',flexxmit:'flexxmit',flexcmd:'flexcmd'};
function set(k,v){post('/api/set?k='+(KEYMAP[k]||k)+'&v='+encodeURIComponent(v))}
function send(){const t=$('txt').value.trim();if(!t)return;
  post('/api/send?t='+encodeURIComponent(t));$('txt').value=''}
function fsksend(){const t=$('fsktxt').value.trim();if(!t)return;
  post('/api/fsk?t='+encodeURIComponent(t));$('fsktxt').value=''}
function memPlay(n){post('/api/mem?play='+n)}
function memSave(n){post('/api/mem?n='+n+'&t='+encodeURIComponent($('m'+n).value))}
let memsBuilt=false;
function buildMems(list){
  if(memsBuilt)return; memsBuilt=true;
  // NB: the returned string starts on the same line as `return` — a line
  // break there and automatic semicolon insertion silently returns undefined.
  $('mems').innerHTML=list.map((t,i)=>{const n=i+1;
    return '<div class="row mem"><label style="flex:0 0 24px">F'+n+'</label>'
      +'<input type="text" id="m'+n+'" style="flex:1;width:auto;min-width:0">'
      +'<button onclick="memSave('+n+')">SAVE</button>'
      +'<button onclick="memPlay('+n+')">PLAY</button></div>';}).join('');
  list.forEach((t,i)=>$('m'+(i+1)).value=t);
}
function esc(t){return String(t).replace(/[&<>"']/g,c=>'&#'+c.charCodeAt(0)+';')}
async function scan(){
  const r=await fetch('/api/flexscan?net='+encodeURIComponent($('scannet').value.trim()),{method:'POST'});
  note(await r.text(),!r.ok); if(r.ok) pollScan();
}
async function pollScan(){
  let j;try{j=await(await fetch('/api/flexscan')).json()}catch(e){setTimeout(pollScan,1000);return}
  $('scanState').textContent=j.running?('scanning '+j.net+'.x \u2014 '+j.tried+'/254')
    :j.err?('stopped: '+j.err):j.hits.length?'':'no radio on '+j.net+'.x';
  $('scanHits').innerHTML=j.hits.map(h=>'<button onclick="useRadio(\''+esc(h.ip)+'\')">'
    +esc(h.ip)+(h.model?' &middot; '+esc(h.model):'')+(h.name?' &middot; '+esc(h.name):'')
    +'</button>').join(' ');
  if(j.running) setTimeout(pollScan,700);
}
function useRadio(ip){$('flexip').value=ip;set('flexip',ip);note('radio IP set to '+ip)}
function led(id,on,warn){const e=$(id);e.className='led'+(on?(warn?' warn':' on'):'')}
async function refresh(){
  let s;try{s=await(await fetch('/api/state')).json()}catch(e){return}
  $('wpmBig').textContent=s.wpm;
  $('src').textContent=s.pot?'FROM KNOB':'HOST/WEB SET';
  led('l-host',s.host);led('l-tcp',s.tcp);led('l-key',s.key);
  led('l-tune',s.tune,true);led('l-pot',s.pot);
  led('l-flex',s.flex.enabled&&s.flex.connected,s.flex.enabled&&!s.flex.slice);
  led('l-disp',s.disp&&s.disphw);
  const w=s.flex.slicewarn||'';
  $('slicewarn').textContent='⚠ '+w; $('slicewarn').hidden=!w;
  $('flexState').textContent = !s.flex.enabled ? 'off'
      : s.flex.connected ? (s.flex.slice ? 'ready' : 'no CW slice')
      : 'searching';
  $('flex').checked=s.flex.enabled;
  // Gated on the ENABLE. The detail rows go when Flex is off, and the
  // Keying selector stops offering Flex at all — so there is no way to
  // select a backend that is not enabled, and no lockout either, because
  // the enable checkbox itself always stays on screen.
  for (const el of document.querySelectorAll('.flexonly'))
    el.hidden = !s.flex.enabled;
  // Safari ignores hidden/disabled on <option> — it still lists them — so
  // the entry has to leave the DOM entirely. Kept if it is the CURRENT
  // value, otherwise the select would go blank and hide the real state.
  const sel = $('backend');
  if (s.flex.enabled) {
    if (!flexOpt.parentNode) sel.appendChild(flexOpt);
  } else if (flexOpt.parentNode) {
    flexOpt.remove();      // the firmware has already moved keying to local
  }
  $('flexbind').checked=s.flex.bind; $('flexxmit').checked=s.flex.xmit;
  if(editing!=='flexip') $('flexip').value=s.flex.ip||'';
  if(s.ip) $('scannet').placeholder=s.ip.split('.').slice(0,3).join('.');
  {
    const u = s.uptime|0;
    const t = u < 90 ? u + 's' : u < 5400 ? Math.round(u/60) + 'm'
                                          : (u/3600).toFixed(1) + 'h';
    $('foot').textContent = 'winkeyer.local \u00b7 up ' + t
      + ' \u00b7 last reset: ' + (s.resetreason || '?');
  }
  $('legSerial').title = s.baud==1200
    ? 'Ready for a logger: 1200 8N2 is what a WinKeyer host expects.'
    : 'Console rate. A logger looking for a WinKeyer will NOT talk to the port '
      +'at this setting.';
  $('legPtt').title = s.backend==='flex'
    ? 'FlexRadio backend: Tail releases both the local PTT line and the radio '
      +'(xmit 0). Lead-in drives GPIO32 only — still live for an amp — since '
      +'the radio does its own T/R. KEY on GPIO33 is idle.'
    : 'Local backend: lead-in and tail sequence PTT on GPIO32 around the KEY '
      +'line on GPIO33.';
  const fill={wpm:s.wpm,sthz:s.sthz,mode:s.mode,potmin:s.potmin,potmax:s.potmax,
    backend:s.backend,dispctl:s.dispctl,baud:String(s.baud),
    txpower:String(s.txpower),
    pecho:(s.pecho==2?'auto':(s.pecho==1?'on':'off')),
    // The board sends a float, so 45.45 arrives as 45.45000076 and matches
    // no <option value>, leaving the select blank. Round to hundredths.
    fskbaud:String(Math.round(s.fskbaud*100)/100),radio:(s.radio==3?'both':String(s.radio)),
    flexcmd:s.flex.cmd,
    weight:s.weight,ratio:s.ratio,
    farns:s.farns,lead:s.lead,tail:s.tail};
  for(const k in fill) if(editing!==k) $(k).value=fill[k];
  $('swap').checked=s.swap;$('pot').checked=s.pot;$('disp').checked=s.disp;
  $('ptt').checked=s.ptt;$('st').checked=s.st;$('monitor').checked=s.monitor;
  {const auto_=s.mondelay<0; $('mondelayauto').checked=auto_;
   $('mondelay').disabled=auto_;
   if(editing!=='mondelay') $('mondelay').value=auto_?'':s.mondelay;
   $('mondelayNow').textContent=(s.mondelaynow||0)+' ms'+
     (auto_?' (measured '+(s.flexlatency||0)+')':'');}
  $('pechoState').textContent = s.pechoon ? 'active' : 'inactive';
  $('rssiVal').textContent = s.rssi + ' dBm rx';
  buildMems(s.mems||[]);
  if(editing!=='call') $('call').value=s.call||'';
  $('fskinv').checked=s.fskinv; $('fskdid').checked=s.fskdid;
  $('fskState').textContent = s.fskbusy ? 'SENDING' : '';
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
bindNum('mondelay',0,2000);
// The tickbox is the mode; the box is the manual value it falls back to.
$('mondelayauto').onchange=e=>set('mondelay',
  e.target.checked ? 'auto' : ($('mondelay').value||'0'));
bindNum('potmin',5,59); bindNum('potmax',6,60);
$('call').onfocus=()=>editing='call';
$('call').onblur =()=>{editing=null;post('/api/mem?call='+encodeURIComponent($('call').value))};
$('flexip').onfocus=()=>editing='flexip';
$('flexip').onblur =()=>{editing=null;set('flexip',$('flexip').value)};
for(const id of ['mode','backend','dispctl','baud','pecho','fskbaud','radio','flexcmd','txpower'])
  $(id).onchange=e=>set(id,e.target.value);
for(const id of ['swap','pot','disp','ptt','st','monitor','fskinv','fskdid',
                 'flex','flexbind','flexxmit'])
  $(id).onchange=e=>set(id,e.target.checked?'on':'off');
$('txt').addEventListener('keydown',e=>{if(e.key==='Enter')send()});
$('fsktxt').addEventListener('keydown',e=>{if(e.key==='Enter')fsksend()});
refresh();setInterval(refresh,1000);
</script></body></html>)HTML";

void handleState() {
  // Heap, NOT the stack. This began as StaticJsonDocument<640> and grew to
  // 2560 as fields were added — 2.5 KB of stack, plus six String copies of
  // the memories and a String for the output, inside loopTask's 8 KB. The
  // page polls this every second, so the overflow presented as the board
  // rebooting at random rather than as anything pointing here.
  DynamicJsonDocument doc(2560);
  Settings::toJson(doc);
  JsonArray mems = doc.createNestedArray("mems");
  for (uint8_t i = 1; i <= Memories::COUNT; i++) mems.add(Memories::get(i));
  doc["call"] = Memories::call();
  doc["rssi"] = (int)WiFi.RSSI();
  doc["ip"]   = WiFi.localIP().toString();
  String out;
  out.reserve(1024);
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
  Log::printf("[WEB] %s=%s -> %s\n", server.arg("k").c_str(),
                server.arg("v").c_str(), msg);
  server.send(ok ? 200 : 400, "text/plain", msg);
}

void handleSend() {
  String t = server.arg("t");
  if (!t.length()) { server.send(400, "text/plain", "nothing to send"); return; }
  WinKeyer::sendText(t.c_str());
  Log::printf("[WEB] > %s\n", t.c_str());
  server.send(200, "text/plain", String("sent: ") + t);
}

void handleMem() {
  if (server.hasArg("call")) {
    Memories::setCall(server.arg("call").c_str());
    server.send(200, "text/plain", "callsign saved");
    return;
  }
  if (server.hasArg("play")) {
    uint8_t n = (uint8_t)server.arg("play").toInt();
    bool ok = Memories::play(n);
    server.send(ok ? 200 : 400, "text/plain",
                ok ? String("playing memory ") + n : String("memory is empty"));
    return;
  }
  uint8_t n = (uint8_t)server.arg("n").toInt();
  if (!Memories::set(n, server.arg("t").c_str())) {
    server.send(400, "text/plain", "bad slot, or over 100 characters");
    return;
  }
  server.send(200, "text/plain", String("memory ") + n + " saved");
}

void handleFsk() {
  if (server.hasArg("stop")) {
    Fsk::abort();
    server.send(200, "text/plain", "fsk stopped");
    return;
  }
  String t = server.arg("t");
  if (!t.length()) { server.send(400, "text/plain", "nothing to send"); return; }
  if (!Fsk::send(t.c_str())) {
    server.send(503, "text/plain", "fsk buffer full");
    return;
  }
  Log::printf("[FSK] > %s\n", t.c_str());
  server.send(200, "text/plain", String("fsk: ") + t);
}

// Radio finder. POST starts a sweep of one /24 for the SmartSDR API port;
// GET reports progress and what answered. Polled only while a scan runs,
// so it stays out of /api/state and that document's size budget.
void handleScanStart() {
  String net = server.arg("net");
  net.trim();
  if (Flex::scanRunning()) {
    server.send(409, "text/plain", "a scan is already running");
    return;
  }
  if (!Flex::scanStart(net.c_str())) {
    server.send(400, "text/plain", "subnet must be three octets, e.g. 192.168.1");
    return;
  }
  server.send(200, "text/plain",
              "scanning " + Flex::scanNet() + ".1-254 for a FlexRadio");
}

void handleScanState() {
  DynamicJsonDocument doc(768);
  doc["running"] = Flex::scanRunning();
  doc["tried"]   = Flex::scanTried();
  doc["net"]     = Flex::scanNet();
  doc["err"]     = Flex::scanError();
  JsonArray a = doc.createNestedArray("hits");
  Flex::ScanHit h[4];
  uint8_t n = Flex::scanHits(h, 4);
  for (uint8_t i = 0; i < n; i++) {
    JsonObject o = a.createNestedObject();
    o["ip"] = h[i].ip; o["model"] = h[i].model; o["name"] = h[i].name;
  }
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

void handleTune() {
  bool on = server.arg("v") == "on";
  Keyer::tune(on);
  server.send(200, "text/plain", on ? "tune on — key down" : "tune off");
}

}  // namespace

namespace Web {

void begin() {
  server.on("/", HTTP_GET, []() {
    // The page is baked into the firmware and changes on almost every
    // flash, so a cached copy is always the wrong one. Served with no
    // headers at all it could be held indefinitely.
    server.sendHeader("Cache-Control", "no-store, must-revalidate");
    server.send_P(200, "text/html", PAGE);
  });
  server.on("/api/state", HTTP_GET,  handleState);
  server.on("/api/set",   HTTP_POST, handleSet);
  server.on("/api/send",  HTTP_POST, handleSend);
  server.on("/api/tune",  HTTP_POST, handleTune);
  server.on("/api/fsk",   HTTP_POST, handleFsk);
  server.on("/api/mem",   HTTP_POST, handleMem);
  server.on("/api/flexscan", HTTP_POST, handleScanStart);
  server.on("/api/flexscan", HTTP_GET,  handleScanState);
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
    Log::printf("[WEB] settings at http://%s.local/ or http://%s/\n",
                  MDNS_HOSTNAME, WiFi.localIP().toString().c_str());
  }
  server.handleClient();
}

}  // namespace Web
