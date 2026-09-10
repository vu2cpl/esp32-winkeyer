# ESP32 WinKeyer — Project Handover
*For continuation in a new Claude session*

**Created:** 2026-08-26 · **Updated:** 2026-09-10 · **Type:** ESP firmware
(esp32dev, S3 env reserved) · **Status:** feature-complete on the bench,
awaiting WiFi onboarding + on-air testing

---

## What this is

A WinKeyer-compatible CW keyer on ESP32, reachable over WiFi, with an
optional FlexRadio backend. WinKeyer protocol by Steve K1EL; K3ng keyer
(Anthony Good K3NG) used as a behavioural reference — implementation is
original.

**Hardware (decided 2026-08-26):** bench and likely final board is a classic
**ESP32-D0WD-V3** devkit behind a CP2102 (`env:esp32-winkeyer`, board
`esp32dev`) — dual core, WiFi + BT Classic. An **ESP32-S3** env
(`env:esp32s3-winkeyer`) is the upgrade path: native USB CDC with a custom
descriptor would fix the CP2102 `usbserial-0001` port-identity problem for
wired use. A friend of Manoj's is building one too, on a 38-pin DevKitC
clone — same chip family, same firmware, no changes needed.

**Why WiFi is the primary link:** every CW element is timed on the keyer
(or, in Flex mode, on the radio), so the network only carries text and
status. Link latency never reaches the air. The one exception is
real-time paddle keying over the network, which is deliberately not
implemented — see "Flex backend" below.

## Pin map (`include/pins.h`)

| Signal | GPIO | Notes |
|---|---|---|
| Paddle dit (tip) | 25 | INPUT_PULLUP, closes to GND |
| Paddle dah (ring) | 26 | INPUT_PULLUP, closes to GND |
| Key out | 33 | active high → PC817 opto (330 Ω) or NPN |
| PTT out | 32 | active high → PC817 opto (330 Ω) or NPN |
| Sidetone | 4 | LEDC PWM → passive piezo |
| Speed pot | 34 | ADC1_CH6 (input-only) — **disabled in fw until wired** (`/pot on`), pin floats otherwise |
| Status LED | 2 | onboard |
| Reserved OTRSP | 16,17 (UART2), 27, 14, 13, 5 | revised 2026-09-10 — see below |

**Pin reservation revised 2026-09-10.** The original OTRSP block claimed
GPIO 21/22, which are the standard ESP32 I²C pins. If a display is ever
added it wants 21/22 (every OLED library assumes them), so OTRSP relay
outputs move to 27/14/13/5 and UART2 stays on 16/17. GPIO 18/19/23 are
free again; 35/36/39 remain available as input-only (straight-key jack,
command button).

## Architecture

Four modules, each transport- or backend-agnostic so they compose:

- **`src/keyer.cpp`** — 1 kHz FreeRTOS task, core 1, priority 10 (above
  loopTask; WiFi/BT live on core 0), so element timing is jitter-free
  regardless of network activity. Iambic A/B with Curtis semantics (both
  modes latch the opposite paddle during an element, B also during the
  space), squeeze alternation, 3 ms debounce, PARIS timing. Also:
  weighting, dit/dah ratio, Farnsworth, PTT lead/tail sequencing, tune,
  manual PTT hold, prosign merge (suppresses the inter-character gap),
  paddle break-in, 256-char queue, speed pot with WinKeyer override
  semantics (host speed rules until the pot moves).
- **`src/winkeyer.cpp`** — K1EL host protocol, reports **version 23
  (WK 2.3)**. Every logger supports WK2; claiming WK3 buys nothing and
  narrows compatibility. Implements the command set loggers actually use;
  unknown commands still have their parameters consumed from a table, so
  an unrecognised command can never desync the stream. Emits unsolicited
  status (0xC0|flags) and pot (0x80|value) bytes on change.
- **`src/net.cpp`** — WinKeyer byte stream over TCP 8088, mDNS
  `winkeyer.local` (`_winkeyer._tcp`). One client at a time; a new
  connection displaces the old one and resets the host session.
- **`src/flex.cpp`** — FlexRadio discovery + SmartSDR command API.

Serial (115200) is a text CLI that **auto-switches** to the WinKeyer
binary protocol when a host-open arrives (0x00 is never valid CLI input)
and back to the CLI on host close.

## Flex backend

- Command API **TCP 4992**, line oriented: send `C<seq>|<cmd>`, receive
  `R<seq>|<hex>|<msg>` replies and `S<handle>|...` status.
- Discovery: firmware > v1.1.3 sends VITA-49 on **UDP 4991**, older sends
  a proprietary format on **UDP 4992**. Both carry the same ASCII
  `key=value` payload, so we listen on both and scan for fields rather
  than parsing two header formats.
- Keying is `cwx send <text>`; **a literal space does not survive the
  command parser — send ASCII 0x7F and the radio translates it back.**
  Also used: `cwx clear`, `cwx wpm <n>`. Subscribe with `sub cwx all`;
  progress arrives as `S<handle>|cwx sent=<index>`.
- In Flex mode the local key output is disabled (`Keyer::setKeyOutEnabled`)
  so the rig is not keyed twice; sidetone stays local. Slice must be in CW.
- **Deliberately not implemented:** real-time paddle keying over the
  network — element timing sent packet-by-packet inherits WiFi jitter.
  Paddles stay on the local key output.
- **Worth knowing:** SmartSDR CAT already provides a WinKeyer-emulation
  COM port, and N1MM+ uses that rather than CWX. So for a Flex owner this
  keyer's value is real paddles with local element generation, not
  replacing the built-in emulation. Prior art: the WKFlex community
  project.

## Testing

`tools/wk-test.py` acts as a WinKeyer host over TCP or serial;
`tools/wk-bridge.py` creates a PTY (default `/tmp/winkeyer`) bridged to
the keyer's TCP port so logging software sees a serial device.

**Verified on hardware 2026-09-10** (wired serial): host open returns
0x17 (=23), status and pot reports arrive, `request status` answers,
speed set works, and sending "TEST" produced exactly 6 KEYDOWN
transitions (T·E·S·S·S·T = 1+1+3+1) before returning to idle. That is
correct WinKeyer host behaviour.

## What changed

- **2026-08-26** — scaffold; identified the bench board as ESP32-D0WD-V3
  (CP2102); dual-env platformio.ini; keyer core implemented, flashed and
  bench-verified; WiFiManager made non-blocking.
- **2026-09-10** — researched and confirmed the SmartSDR API facts above.
  Implemented the **WK protocol engine**, **TCP transport + mDNS**, the
  **Flex backend**, and the **host bridge / test tools**. Extended the
  keyer core (weighting, ratio, Farnsworth, prosign merge, manual PTT,
  key-out disable, queue introspection). Revised the OTRSP pin
  reservation off the I²C pins. Flashed and verified the protocol engine
  over serial.

## Network placement

The keyer must sit on the **same subnet/VLAN as the logging computer**,
and as the Flex radio if that backend is used. Both `winkeyer.local`
(mDNS) and Flex discovery are broadcast-based and do not cross subnets —
an IoT-VLAN placement would break discovery even though the MQTT
heartbeat would still work. The shack MQTT broker is on 192.168.1.10,
so the shack/trusted LAN is the natural home.

**The WinKeyer TCP port is unauthenticated** — anyone who can reach port
8088 can key the transmitter. That is an argument for a trusted LAN, and
against exposing it beyond one.

## Open items

1. **WiFi onboarding is not done** — the board sits in the captive portal
   (`vu2cpl-esp32-winkeyer-setup` / `vu2cpl1234`). Until Manoj joins it to
   a network, the TCP transport, mDNS, MQTT and Flex paths are
   **implemented but unverified**. This is the next step and needs a human.
   Portal timeout was 180 s, which strands an un-onboarded board; set to
   0 (never) on 2026-09-10. Note macOS Sequoia's `system_profiler
   SPAirPortDataType` no longer lists nearby networks, so it cannot be
   used to check whether the AP is broadcasting — read the board's own
   `/net` output instead.
2. **On-air timing check** — bench testing is functional, not calibrated.
   Verify element timing against a scope or a known-good decoder.
3. **Flex backend needs a radio** to verify: discovery parsing, `cwx`
   round-trip, and the `pending()` busy heuristic (derived from
   `cwx send` reply index vs `cwx sent=` status) are all untested against
   real hardware.
4. **Pin config command (WK 0x09)** — only bit 0 (PTT enable) is acted on.
   The remaining bits differ between WK revisions and guessing wrong would
   silently disable sidetone or key output. Revisit after testing with a
   real logger.
5. Hardware build: paddle/key/PTT interface (PC817 + 330 Ω), speed pot,
   enclosure.
6. **Sharing with Manoj's friend** — repo is private. Needs either a
   collaborator invite or an explicit decision to publish. Not done.
7. Considered but not built: **display** (SSD1306 on I²C 21/22 — pins now
   free for it) and **Bluetooth keyboard** (BT Classic HID *host* support
   is thin on ESP32 and BT/WiFi share the radio; prototype standalone
   before committing). Ordering rationale: display is low-risk, BT
   keyboard is the one with real unknowns.
8. Future: ESP32-S3 env for a native-USB descriptor; OTRSP/SO2R phase
   (pins reserved; SO2R docs in `~/projects/SO2R box`).

## Conventions (see ~/.claude/CLAUDE.md)

- **CDP** — Commit, Document, Push together on every substantive change.
- Never pin `upload_port`/`monitor_port` — use `flash.sh`/`monitor.sh`.
- Never commit secrets — they live in git-ignored `secrets.h`.
- GitHub repos are **private** unless explicitly published.
- Credit upstream: WinKeyer protocol = Steve K1EL; K3ng keyer = Anthony
  Good K3NG.
