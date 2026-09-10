# ESP32 WinKeyer

A WinKeyer-compatible CW keyer on an ESP32, reachable over WiFi. Iambic
paddle keying with local element generation, a K1EL-compatible host
protocol so logging software talks to it as a WinKeyer, and an optional
FlexRadio backend that keys a 6000/8000-series radio over the network.
Settings are edited from a front-panel OLED's companion web page at
`winkeyer.local` and persist across power cycles.

WinKeyer protocol by Steve K1EL. K3ng CW keyer by Anthony Good K3NG used
as a behavioural reference; the implementation here is original.

## Status

| Piece | State |
|---|---|
| Keyer core — iambic A/B, sidetone, PTT, pot, break-in | working, bench-verified |
| WinKeyer protocol engine (WK 2.3 host mode) | working, verified with `tools/wk-test.py` |
| WiFi TCP transport + mDNS `winkeyer.local` | working, verified over WiFi |
| FlexRadio backend — **paddle keying over the network** | working, verified on a 6600 |
| Host bridge (`tools/wk-bridge.py`) | implemented, not yet driven by a real logger |
| OLED status panel (SH1106/SSD1306 128x64) | working, SH1106 found at 0x3C on hardware |
| Settings web page at `winkeyer.local` | working, exercised on hardware |
| Persisted settings (NVS) | working, verified across a hard reset |

Bluetooth keyboard is considered but not built — see `HANDOVER.md`.

## Quick start

```bash
python3 install.py     # bootstraps PlatformIO (macOS/Pi aware), verifies the build
./flash.sh             # build + upload (picks the serial port)
./monitor.sh           # serial monitor
```

1. First boot opens WiFi AP **`vu2cpl-esp32-winkeyer-setup`** (password
   `vu2cpl1234`). Join it from a laptop or phone; a captive portal opens
   where you pick **your own network** and enter its password. Creds
   persist in NVS. The portal does not time out, and the keyer keys
   normally while it is open — onboarding is non-blocking by design.

   Put the keyer on the **same subnet as the logging computer** (and the
   radio, if using the Flex backend): `winkeyer.local` and Flex discovery
   are both broadcast-based and do not cross subnets or VLANs.

   `/wifi` shows the current network, `/wifi portal` reopens the portal,
   `/wifi reset` clears the saved credentials and reboots.

   **Security note:** the WinKeyer TCP port is unauthenticated — anyone on
   that network can key the transmitter. Use a trusted LAN, not a guest or
   open network.
2. Copy `include/secrets.h.example` → `include/secrets.h` and set the MQTT
   role password. `secrets.h` is git-ignored.

## Wiring

| Signal | GPIO | Notes |
|---|---|---|
| Paddle dit (tip) | 25 | internal pullup, paddle closes to GND |
| Paddle dah (ring) | 26 | internal pullup, paddle closes to GND |
| Key out | 33 | active high → PC817 opto (330 Ω) or NPN → rig KEY |
| PTT out | 32 | active high → PC817 opto (330 Ω) or NPN → rig PTT |
| Sidetone | 4 | passive piezo to GND |
| Speed pot | 34 | ADC1, 10 k **linear** pot across 3V3–GND, wiper to 34; **enable with `/pot on`** |
| Status LED | 2 | onboard |
| OLED SDA | 21 | 128x64 I²C panel, optional |
| OLED SCL | 22 | |

Paddles need no external parts.

**Speed pot.** CW end to 3V3, wiper to GPIO 34, CCW end to GND, plus a
100 nF ceramic from wiper to GND at the pot — GPIO 34 is an unbuffered
ADC input and picks up sidetone PWM hash without it. Reversing the ends
just reverses the knob. Linear taper: a log pot wastes most of its travel.
Default range is 10–35 WPM (`/pot 10 35` to change). The knob is disabled
in firmware until `/pot on` is given, because GPIO 34 floats on a board
with no pot wired; that setting then persists.

Expect the top of the knob's travel to be a small dead zone: ADC_11db
saturates near 3.1 V rather than 3.3 V. Normal ESP32 behaviour.

**OLED (optional).** VCC→3V3, GND→GND, SDA→21, SCL→22. Most breakouts
carry their own pull-ups; if yours does not, add 4.7 kΩ from each line to
3V3. The firmware probes 0x3C then 0x3D at boot and stays off if nothing
answers, so an un-wired board is unaffected.

**If the panel stays dark, run `/i2c`.** It scans the whole bus and prints
every address that answers, so "wired wrong" and "wrong address" stop
looking alike. It also adopts a panel wired up after boot — no reset
needed. Detection runs at 100 kHz on purpose: a panel on breadboard leads
answers reliably at 100 kHz but only intermittently at 400 kHz, which
otherwise shows up as a display that works on some boots and not others.
Rendering then moves to 400 kHz only after the panel proves it answers
there (a 128x64 frame is 1 KB — ~25 ms at 400 kHz, ~100 ms at 100 kHz, all
of it inside a blocking transaction). The boot line reports which speed
won; a 100 kHz fallback is your cue to add pull-ups or shorten leads.

Controller choice is a **setting, not a probe** — SH1106 and SSD1306
answer identically on I²C. Default is `sh1106`, correct for nearly all
1.3" panels; 0.96" panels are usually `ssd1306`. Wrong choice is obvious
and harmless: the image sits 2 px right with a garbage sliver down the
left edge (the SH1106 has 132 columns of RAM to the SSD1306's 128). Fix
it with `/disp ssd1306` — no reflash.

See `include/pins.h`.

## Connecting logging software

The keyer speaks the WinKeyer protocol over a TCP socket. Loggers want a
serial port, so run the bridge:

```bash
./tools/wk-bridge.py
```

That creates `/tmp/winkeyer` (a symlink to a PTY) and shuttles bytes to
`winkeyer.local:8088`. Point the logger at that path and choose WinKeyer
as the keyer type. Verified hosts: anything speaking WK2 — N1MM+, DXLog,
RUMlogNG, MacLoggerDX, SkookumLogger, fldigi.

For **N1MM+ in a VM**, map the VM's COM port to the host TCP socket with
the VM's serial-over-TCP option instead of using the bridge.

Network latency does not affect CW: every element is timed on the keyer
(or on the radio, in Flex mode). The socket only carries text and status.

Wired fallback: the USB serial port is a text CLI at 115200 that switches
itself into the WinKeyer binary protocol as soon as a host-open command
arrives, and back to the CLI on host close.

## FlexRadio

```
/flex on                 # enable the backend (off by default, persists in NVS)
/flex ip 192.168.1.50   # pin the radio's address
/flex auto               # or rely on discovery (same subnet only)
/backend flex            # route buffered text to the radio — this transmits
```

**Discovery only works on the radio's own subnet** — it is a raw UDP
broadcast, unlike mDNS. If the radio is on another segment, pin the IP.
To find it, TCP-scan for port 4992; a Flex answers immediately with
`V<version>` / `H<handle>`.

**In Flex mode the paddle keys the radio over WiFi — no KEY or PTT wire.**
The keyer asserts PTT (`xmit 1`), sends each element as `cw key 1/0` with
a `time=` timestamp so the radio *schedules* the edge rather than keying
on arrival, and releases PTT after a tail. That timestamp is what keeps
the CW readable across a jittery link; it is the same mechanism Maestro
and MORCONI use.

Sidetone stays local and follows your own paddle timing, so your fist
sounds right in the ear whatever the network is doing. The local key
output is disabled in this mode so the rig is not keyed twice.

**Requirements — both fail silently, with no error from the radio:**

- SmartSDR must have **a slice in use, in CW mode**. With no slice the
  radio simply transmits nothing. `/status` reports readiness, and the
  keyer warns when you key without it.
- SmartSDR (a GUI client) must be connected — with none the radio reports
  `tx_allowed=0` and nothing may transmit at all.

Tuning knobs, should keying misbehave on a different radio or firmware:
`/flex cmd key|ptt` (which keying command), `/flex bind on|off`,
`/flex ptt on|off` (whether we assert `xmit`). Defaults are what works on
a 6600 running SmartSDR 4.2.20.

The `backend` setting persists in NVS, so it survives a reboot.

Not used: `cwx send`. It sends *text* for the radio to key itself, which
cannot carry a fist, and this radio refuses CWX to a second client
anyway. Wiring GPIO 33 to the KEY jack still works and is the
lowest-latency option, but is no longer necessary.

Paddle keying deliberately stays on the local key output — real-time
element timing over WiFi would carry the jitter. For a Flex in the same
shack, wire the key output to the radio's KEY jack and use the network for
buffered text.

## Tools

```bash
./tools/wk-test.py --serial /dev/cu.usbserial-0001   # exercise the protocol
./tools/wk-test.py --host winkeyer.local             # ...over WiFi
./tools/wk-bridge.py                                 # TCP → serial port
./tools/wk-timing.py --text "CQ TEST"                # timestamped status
./tools/flex-check.py                                # why isn't it keying?
./tools/flex-check.py --key                          # ...and key it (TRANSMITS)
```

**`flex-check.py` is the first thing to run when the Flex will not key.**
Every prerequisite fails silently — the radio reports no error for a
missing slice, a slice in the wrong mode, or a missing GUI client — so it
checks all of them at once.

**`wk-timing.py` timestamps every status byte** and counts KEYDOWN against
the text's actual element count. Counting status bytes in fixed windows
gives false negatives when a delayed burst lands outside its window; this
tells "not sent" from "reported late".

## Serial CLI

`/wpm N` `/mode a|b` `/swap` `/tune` `/pot on|off` `/pot <min> <max>`
`/ptt on|off` `/st N|on|off` `/disp on|off` `/disp sh1106|ssd1306`
`/backend local|flex` `/flex on|off|ip <addr>|auto` `/wifi [portal|reset]`
`/i2c` `/net` `/status`. Any other line is sent as CW.

## Settings web page

`http://winkeyer.local/` (or the IP — `/net` prints it). Speed, mode,
paddle swap, sidetone, pot enable and range, display, backend, plus a
send box and tune/stop. Live status LEDs for host, TCP, key, tune, pot,
Flex and OLED, polled once a second.

Same trust posture as the WinKeyer TCP port: **no authentication**, so
keep it on a trusted LAN. Visual style is borrowed from soft-MORCONI
(`~/projects/Morconi`) — that project is a browser UI plus a Node bridge,
so the look carried over and none of the code did.

### Which settings stick

The **operator's** panel settings persist in NVS: speed, mode, swap,
sidetone, PTT, weighting/ratio/Farnsworth, pot enable and range, display,
backend. The **host's** session settings do not — a speed N1MM sets over
the WinKeyer protocol is gone at the next boot, so a contest never leaves
the keyer permanently reconfigured. CLI and web page both go through
`Settings::apply()`, so they cannot disagree about ranges or names.

## MQTT

- Broker: `192.168.1.10:1883` (auth required — role account in `secrets.h`).
- Status: `shack/esp32-winkeyer/status` (retained; LWT `{"event":"offline"}`),
  heartbeat carries WPM, busy, backend, host/TCP/Flex connection state.

## Layout

```
platformio.ini        env:esp32-winkeyer (esp32dev) + env:esp32s3-winkeyer
include/config.h       broker, topics, ports, AP name (+ git-ignored secrets.h)
include/pins.h         GPIO map (I²C + OTRSP pins reserved)
src/keyer.cpp          iambic keyer engine (1 kHz task, core 1)
src/winkeyer.cpp       K1EL WinKeyer protocol engine
src/flex.cpp           FlexRadio discovery + SmartSDR command API
src/net.cpp            WinKeyer-over-TCP server + mDNS
src/settings.cpp       validation + NVS, shared by the CLI and the web page
src/display.cpp        SH1106/SSD1306 status panel (own task, core 0)
src/web.cpp            settings web server (port 80)
src/main.cpp           wiring, WiFi, MQTT, serial CLI
tools/wk-bridge.py     TCP → PTY bridge for logging software
tools/wk-test.py       protocol test harness
tools/wk-timing.py     timestamped status — "not sent" vs "reported late"
tools/flex-check.py    Flex prerequisite diagnostic + keying test
flash.sh / monitor.sh  serial-port pickers (never pin the port)
install.py             toolchain bootstrap, macOS/Pi branch
```

See `HANDOVER.md` for design decisions and open items.
