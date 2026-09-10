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

### The radio in this shack (found 2026-09-10)

**192.168.1.50, SmartSDR API 1.4.0.0.** Connection and `sub cwx all`
verified from the keyer.

- **Discovery does not reach it.** The radio is on the 192.168.1.x
  segment, the keyer is on 192.168.10.x, and discovery is a raw UDP
  broadcast. It will never be auto-found from where the keyer sits — set
  it explicitly: `/flex ip 192.168.1.50`. The setting persists in NVS
  and reconnects across reboots.
- **`client program <name>` is rejected** by 1.4.0.0 with error
  `10000002 unknown client program`. Removed — the subscription is what
  matters and it succeeds.
- Finding it: nothing broadcast, so the radio was located by TCP-scanning
  the shack subnets for port 4992. A Flex answers immediately with
  `V<version>` / `H<handle>`, which makes it unmistakable.
- **Not yet tested: actually sending CW.** `cwx send` keys the
  transmitter and puts a signal on the air under Manoj's callsign, so
  that test needs him present and the radio set up deliberately (dummy
  load or a clear frequency). Requires `/backend flex`.

## Testing

`tools/wk-test.py` acts as a WinKeyer host over TCP or serial;
`tools/wk-bridge.py` creates a PTY (default `/tmp/winkeyer`) bridged to
the keyer's TCP port so logging software sees a serial device.

**Verified on hardware 2026-09-10, both transports.** Host open returns
0x17 (=23) in 44 ms, status and pot reports arrive, `request status`
answers, speed set works, and host close is clean. Element reporting is
exact:

- "TEST" → 6 KEYDOWN transitions (T·E·S·S·S·T = 1+1+3+1), BUSY held
  1.05 s against a predicted 1.07 s at 28 WPM.
- "CQ TEST VU2CPL" → **38 KEYDOWN transitions, which is exactly the
  element count of that text.** Nothing dropped, nothing duplicated.

Confirmed over **wired serial** and over **WiFi TCP**, by IP and by
`winkeyer.local`.

**When checking status output, timestamp it.** Counting status bytes in
fixed drain windows gave a false negative once: a delayed burst landed
outside its window and looked like "only one element was keyed", when
the CW had in fact gone out correctly. `scratchpad/timed_test.py`-style
timestamping distinguishes "not sent" from "reported late" immediately —
compare the BUSY duration against the text's expected duration.

### Host-link responsiveness — was mostly a firmware bug, now fixed

Symptom: the keyer appeared to stall. The version byte after host open
took **2.7 s** to arrive, and status bytes came in delayed bursts, so a
send that was actually correct looked like it had keyed one element.

Cause was **not** the RF link, despite appearances. `PubSubClient::
connect()` blocks, its default socket timeout is **15 s**, and it was
being retried every 5 s against a broker that refuses the credentials.
While it blocked, `loop()` did not run, so neither `Net::poll()` nor
`WinKeyer::poll()` serviced the host link. A blocked MQTT reconnect was
stalling CW status reporting.

Fixed 2026-09-10 by: never attempting MQTT while `Keyer::busy()` or a WK
host session is open, `setSocketTimeout(2)`, and exponential backoff to
60 s while the broker keeps refusing. **Result: host open reply went
2.7 s → 0.044 s.** Anything else added to `loop()` must respect the same
rule — the host link is the priority, and blocking calls belong behind
an idle check.

RF is still mediocre but no longer the limiting factor: 10 ms min /
131 ms avg / 585 ms max, 0% loss, RSSI -68 dBm. Worth improving (closer
AP, different channel, external-antenna board) but it is no longer what
makes the keyer feel slow.

### Diagnostic traps hit while testing — do not repeat

- **Do not reset the board with manual DTR/RTS toggling** to read the boot
  log. On this CP2102 board it produces a burst of repeated resets that
  looks exactly like a boot loop in the log. It is an artifact. Use
  `./monitor.sh`, or open the port and just listen.
- **Sleep before `reset_input_buffer()`.** The CP2102 driver holds a large
  backlog when nothing has read the port for a while; draining it
  immediately after open replays minutes of old output at once and looks
  like a runaway log flood. Measured with timestamps, the MQTT retry is
  exactly the intended 5 s.
- **macOS Sequoia cannot scan for the setup AP.** `system_profiler
  SPAirPortDataType` no longer lists nearby networks, so its silence is
  not evidence the AP is down. Read the board's own `/net` output.
- **Binary garbage on the serial port** used to mean the CLI had been
  flipped into WinKeyer mode by a stray 0x00 (fixed 2026-09-10). If it
  reappears, that is the first thing to suspect — not a crashed board.

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
  over **serial and WiFi TCP**. Onboarded to WiFi, moved segments with
  `/wifi reset`. Fixed: portal timeout stranding an un-onboarded board,
  serial flipping to binary mode on a stray 0x00, and WiFi modem sleep
  costing ~300 ms of host latency. Measured a poor RF link that remains
  unexplained (see link quality).

## Network placement (measured 2026-09-10)

Manoj's LAN is segmented and **routed between segments**. The keyer was
first onboarded to `<your-ssid>` (192.168.30.20), then moved via `/wifi reset`
to the segment the Mac is on — **currently 192.168.10.20**, Mac
192.168.10.30. The shack MQTT broker is on 192.168.1.10, a third
segment.

Measured rather than assumed:

- **mDNS crosses the segments here.** `winkeyer.local` resolved from the
  Mac even when the keyer was on a different subnet, so something on the
  network reflects mDNS. (An earlier note in this file claimed it would
  not — that was wrong for this LAN.)
- **Flex discovery will not.** It is a raw UDP broadcast and is not
  reflected the way mDNS is. If the radio sits on another segment, skip
  discovery and pin the address with `/flex ip <addr>`.
- **MQTT reaches the broker but is rejected `rc=5` (unauthorized)** — so
  `include/secrets.h` still holds the example password, not the real
  `iot` role password. Reachability is not the problem, even across
  segments.
- Same-subnet placement did **not** fix latency; see the link-quality
  section below.

**The WinKeyer TCP port is unauthenticated** — anyone who can reach port
8088 can key the transmitter. That is an argument for a trusted LAN, and
against exposing it beyond one.

## Open items

1. **Set the real MQTT password** in `include/secrets.h` (the `iot` role,
   from the shack password manager). Currently the example value, so the
   broker rejects the connection with `rc=5` every 5 s. Everything else
   works; this is the only thing failing.
2. **WiFi link is mediocre but no longer limiting** — 131 ms average,
   0% loss, RSSI -68. Improve when convenient (closer AP, different
   channel, external-antenna board); not a blocker.
3. **Try a real logger** — the protocol is verified against
   `tools/wk-test.py`, not yet against N1MM+/RUMlogNG through
   `tools/wk-bridge.py`. That is the last compatibility unknown.
4. **On-air timing check** — testing so far is functional, not
   calibrated. Verify element timing against a scope or a known-good
   decoder.
5. **Flex: connection verified, keying is not.** The keyer connects to
   192.168.1.50 and subscribes. Still untested because it transmits:
   the `cwx send` round-trip, the 0x7F space encoding, and the
   `pending()` busy heuristic (`cwx send` reply index vs `cwx sent=`
   status). Needs `/backend flex` and Manoj present — see above.
6. **Pin config command (WK 0x09)** — only bit 0 (PTT enable) is acted on.
   The remaining bits differ between WK revisions and guessing wrong would
   silently disable sidetone or key output. Revisit after testing with a
   real logger.
7. Hardware build: paddle/key/PTT interface (PC817 + 330 Ω), speed pot,
   enclosure.
8. **Sharing with Manoj's friend** — repo is private. Needs either a
   collaborator invite or an explicit decision to publish. Not done.
9. Considered but not built: **display** (SSD1306 on I²C 21/22 — pins now
   free for it) and **Bluetooth keyboard** (BT Classic HID *host* support
   is thin on ESP32 and BT/WiFi share the radio; prototype standalone
   before committing). Ordering rationale: display is low-risk, BT
   keyboard is the one with real unknowns.
10. Future: ESP32-S3 env for a native-USB descriptor; OTRSP/SO2R phase
    (pins reserved; SO2R docs in `~/projects/SO2R box`).

## Conventions (see ~/.claude/CLAUDE.md)

- **CDP** — Commit, Document, Push together on every substantive change.
- Never pin `upload_port`/`monitor_port` — use `flash.sh`/`monitor.sh`.
- Never commit secrets — they live in git-ignored `secrets.h`.
- GitHub repos are **private** unless explicitly published.
- Credit upstream: WinKeyer protocol = Steve K1EL; K3ng keyer = Anthony
  Good K3NG.
