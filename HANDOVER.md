# ESP32 WinKeyer — Project Handover
*For continuation in a new Claude session*

**Created:** 2026-08-26 · **Type:** ESP firmware (esp32dev, S3 env reserved) · **Status:** scaffold

---

## What this is

WinKeyer (K1EL WK3 protocol) clone on ESP32 — WiFi TCP serial bridge as the
primary PC link (link latency doesn't affect CW: all element timing is on the
keyer), wired serial as fallback, iambic paddle keying, sidetone, speed pot.
Protocol by Steve K1EL; K3ng keyer (Anthony Good K3NG) used as reference.

**Hardware (decided 2026-08-26):** development/likely-final board is a classic
**ESP32-D0WD-V3** devkit behind a CP2102 (`env:esp32-winkeyer`, board
`esp32dev`) — dual core, WiFi + BT Classic (BT SPP is a possible alternative
transport on this chip). An **ESP32-S3** env (`env:esp32s3-winkeyer`) is kept
as the upgrade path: native USB CDC with a custom descriptor (unique serial,
"VU2CPL WinKeyer" product string) would fix the CP2102 `usbserial-0001`
port-identity problem for wired use. Design scope: WinKeyer only, but pinout
reserves headroom for a future OTRSP (SO2R) port.

Single PlatformIO firmware. WiFi via WiFiManager captive portal
(**non-blocking** — the keyer keys with no WiFi); reports to the shack MQTT
broker. The **keyer core is implemented and bench-verified** (2026-08-26);
the WK3 protocol engine and TCP bridge are next.

## Pin map (classic devkit, `include/pins.h`)

| Signal | GPIO | Notes |
|---|---|---|
| Paddle dit (tip) | 25 | INPUT_PULLUP, closes to GND |
| Paddle dah (ring) | 26 | INPUT_PULLUP, closes to GND |
| Key out | 33 | active high → NPN/optocoupler |
| PTT out | 32 | active high → NPN/optocoupler |
| Sidetone | 4 | LEDC PWM → piezo |
| Speed pot | 34 | ADC1_CH6 (input-only) — **pot disabled in fw until wired** (`/pot on`), pin floats otherwise |
| Status LED | 2 | onboard |
| Reserved OTRSP | 16,17,18,19,21,22,23 | 16/17 = UART2 |

## Keyer core (src/keyer.cpp)

1 kHz FreeRTOS task, core 1, priority 10 (above loopTask; WiFi/BT are on
core 0) — element timing is jitter-free regardless of network activity.
Iambic A/B (Curtis semantics: both modes latch opposite paddle during an
element, B also during the space), squeeze alternation, paddle debounce
3 ms, PARIS timing (dit = 1200/WPM ms). PTT lead-in/tail sequencing,
paddle break-in aborts buffered sends (edge-triggered, sets a flag the WK
engine will report), tune mode, 128-char send buffer via FreeRTOS queue,
speed pot with IIR smoothing + host-override semantics (host speed rules
until pot moves — matches WinKeyer). API in `include/keyer.h` is
transport-agnostic: the serial CLI today, the WK3 engine later.

**Serial test CLI** (115200): `/wpm N` `/mode a|b` `/swap` `/tune`
`/pot on|off` `/ptt on|off` `/st N|on|off` `/status`; any other line is
sent as CW.

## Layout

```
platformio.ini         env:esp32-winkeyer
include/config.h        broker, topics, AP name (includes git-ignored secrets.h)
include/secrets.h.example  MQTT role creds template (copy → secrets.h)
src/main.cpp            firmware — WiFiManager + MQTT skeleton
flash.sh / monitor.sh   serial-port pickers
install.py              toolchain bootstrap (macOS/Pi)
```

## Flash

```bash
./flash.sh              # firmware (picks port)
./monitor.sh            # serial monitor
```

## WiFi onboarding

First boot opens AP **`vu2cpl-esp32-winkeyer-setup`** (password `vu2cpl1234`, hostname
`esp32-winkeyer`). Join it → captive portal → pick network. Creds persist in NVS; the
portal reappears only if the saved network can't be joined within 180 s.

## MQTT

- Broker `192.168.1.10:1883` — auth required; role account +
  password in `include/secrets.h` (git-ignored; from the shack password manager).
- `shack/esp32-winkeyer/status` — retained; LWT `{"event":"offline"}`.

## What changed

- **2026-08-26** — scaffold; identified connected board as ESP32-D0WD-V3
  (CP2102); dual-env platformio.ini (`esp32dev` default, S3 reserved);
  **keyer core implemented** (iambic A/B task, sidetone, PTT, pot, CLI),
  WiFiManager made non-blocking, flashed and bench-verified on the board
  (boot, CLI, speed set, CW send all confirmed over serial).

## Open items

1. **WK3 protocol engine** (`src/winkeyer.cpp`) — transport-agnostic K1EL
   command parser: host open/close, speed/sidetone/PTT commands, buffered
   send, 0xC0 status + 0x80 pot reports. Serial CLI then becomes secondary.
2. **WiFi TCP bridge** — TCP server + mDNS `winkeyer.local`, feeding the WK
   engine; document socat/launchd (macOS) and Parallels/VMware
   serial-over-TCP (N1MM VM) bridge recipes.
3. Hardware: build paddle/key/PTT interface (NPN or optocoupler), wire
   speed pot, then enable with `/pot on` default.
4. On-air element-timing check (scope or WK host app) — bench test so far
   is functional, not timing-calibrated.
5. Decide GitHub repo creation (private) — pending Manoj's go-ahead; local
   commits only until then.
6. Future: ESP32-S3 env for native-USB descriptor; possible OTRSP phase
   (pins reserved); possible BT Classic SPP transport on this chip.

## Conventions (see ~/.claude/CLAUDE.md)

- **CDP** — Commit, Document, Push together on every substantive change.
- Never pin `upload_port`/`monitor_port` — use `flash.sh`/`monitor.sh`.
- Never commit secrets — they live in git-ignored `secrets.h`.
- GitHub repos are **private** unless explicitly published.
