# ESP32 WinKeyer — Project Handover
*For continuation in a new Claude session*

**Created:** 2026-08-26 · **Updated:** 2026-09-17 · **Type:** ESP firmware
(esp32dev, S3 env reserved) · **Status:** working keyer, **public repo**
(MIT). RUMlogNG drives it over USB and keys the Flex; OLED/LCD panel,
speed pot, settings web page, memories, second radio and RTTY FSK all on
hardware.

**Read this first if you are picking the project up after 2026-09-12:**

- **FIXED 2026-09-17: on the Flex backend, CW went out at 0 W after a GUI
  client restarted or changed.** There were two faults: the keyer kept a stale
  GUI client handle, and `client bind` wedged the radio's CW generator. The
  keyer now follows the GUI client, and `flexbind` defaults to off. Verified
  on hardware with an AetherSDR restart and no command: paddle first, then a
  memory, both 4.47 W. Open item 13 has the full investigation; the method
  (`/api/flextrace` + a meter watcher, one variable per test) is worth
  reusing.

- **Arduino core 3.3.11 / IDF 5.5.5** via the pioarduino platform, and
  **PlatformIO must run on Python 3.10+** — the Mac's system one is 3.9 and
  cannot build this. `~/.pio-venv313` holds a suitable one; `flash.sh`,
  `monitor.sh` and `install.py` find it themselves.
- **Wiring as of 2026-09-13 (this supersedes the 2026-09-12 two-adapter
  setup described further down):** the keyer's **own USB-C** is back in use
  (`usbserial-0001`, CP2102) and carries RUMlogNG. The FTDI
  `usbserial-A9M9DV3R` has **RX only** — a read-only console for
  monitoring; its TX was removed, so it **cannot flash**. Flash on the USB-C:
  `pio run -e esp32-winkeyer -t upload --upload-port /dev/cu.usbserial-0001`
  (auto-reset works). The devkit's USB chip can still hold the ESP32 in
  reset when a logger drives RTS/DTR — watch for it.
- **Hardware on order (2026-09-16):** 2 × ESP32-S3 N16R8 dual-USB-C
  boards and 2 × LTV-847. Nothing in firmware or the enclosure changes until
  a board is on the bench and measured — see open items 7, 9 and 10.
- Outstanding: FSK polarity and on-air fist quality unverified; the LCD
  slice warning never seen on a panel; the board mod that would end the
  reset problem for good is not done.
- **BLE keyboard: in the firmware, off by default, PARKED (2026-09-13
  evening)** — it does not fit a classic ESP32's heap with the full keyer
  (~26 KB left → no WiFi). A boot guard turns it back off automatically.
  Open item 9 has the routes forward (S3/NimBLE, PSRAM). If the console
  says `=== BLE keyboard probe`, the board is running the standalone probe:
  `./flash.sh` puts the keyer back.

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
| Key out | 33 | active high → opto (PC817 + 330 Ω, or LTV847 + 220 Ω) or NPN |
| PTT out | 32 | active high → opto (PC817 + 330 Ω, or LTV847 + 220 Ω) or NPN |
| Sidetone | 4 | LEDC PWM → passive piezo |
| Speed pot | 34 | ADC1_CH6 (input-only) — 10 k linear + 100 nF wiper→GND; **off until `/pot on`** (now persisted), pin floats otherwise |
| Display SDA / SCL | 21 / 22 | OLED (SH1106/SSD1306 0x3C-0x3D) or HD44780 LCD backpack (16x2/20x4, 0x27-0x3F); family auto-detected |
| Status LED | 2 | onboard — lit while the key is down |
| KEY / PTT out 2 | 18 / 19 | radio 2; `/radio 1\|2\|both` |
| FSK out | 27 | RTTY keying line, mark = idle, invertible |

**The OTRSP reservation is gone (2026-09-11).** Manoj: *"there is no plan
for so2r or otrsp"*. Six pins — 16, 17, 27, 14, 13, 5 — had been held
since the scaffold for a phase that was never going to start, and by the
time FSK, a second KEY/PTT pair and buttons were all wanted, that
reservation was the thing forcing real features onto a resistor ladder.
SO2R lives in `~/projects/SO2R box` as separate hardware.

Free now: **13, 14, 16, 17, 18, 19, 23**, plus 35/36/39 input-only (no
internal pull-ups there — a button needs an external one). Avoid 5, 12
and 15: strapping pins, pulsed at boot. 6-11 are the SPI flash.

Earlier in the same session `pins.h` was also found carrying a *stale*
reservation comment claiming 18/19/21/22/23, contradicting a 2026-09-10
revision that had moved OTRSP off the I²C pins for the display. Both are
now moot.

## Architecture

Seven modules, each transport- or backend-agnostic so they compose:

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
- **`src/settings.cpp`** — validation + NVS (namespace `wk`), the single
  place that knows what a setting is called, what range it takes, and
  whether it persists. The CLI and the web page both call
  `Settings::apply()`, so they cannot drift apart. `applyBackend()` moved
  here from `main.cpp` because backend selection has three coupled side
  effects and every caller was at risk of doing two of them.
- **`src/display.cpp`** — optional SH1106/SSD1306 128x64 panel, rendered
  at 5 Hz from its own task at priority 1 on **core 0**. An I²C frame
  blocks ~25 ms, which must not sit in front of the keyer task (element
  timing) or `loop()` (the host link), so it gets neither.
- **`src/web.cpp`** — settings page + JSON API on port 80, serviced from
  `loop()`. Listening is deferred to `poll()` because WiFiManager is
  non-blocking and there is usually no IP at `setup()` time — same
  lazy-start pattern `net.cpp` already uses for mDNS.

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

**Flex 6600 "6600", callsign VU2CPL, 192.168.1.50, SmartSDR API
1.4.0.0.** Connection and `sub cwx all` verified from the keyer.

#### On-air test 2026-09-10 (dummy load): blocked on the radio, twice

Keying could not be proven. Both blockers are on the radio side — the
keyer's commands reach it and are accepted at the protocol level.

1. **The slice must be in CW mode.** Slice A was `mode=LSB` on
   7.146 MHz. CWX cannot key CW on a non-CW slice: the command is
   accepted, nothing is transmitted, and no `cwx sent=` ever arrives.
   `slice set 0 mode=CW` works and was verified; the slice was restored
   to LSB afterwards.
2. **Something else holds the CW/CWX transmit path.** With the slice in
   CW, `cwx send` is still refused:

       R3|500000C2|Cannot transmit since another client is transmitting
                   or sending a CW/CWX message

   This happens from a **direct session on the Mac too**, not just from
   the keyer, so it is not the keyer's doing — and `interlock
   state=READY tx_allowed=1` at the same time, so the radio itself is
   not inhibited. Clearing the keyer's own CWX buffer did not help, so
   it is not a stale buffer from the failed first attempt.

   **Who was holding it:** SmartSDR on the PC was one client; closing it
   was not enough, because a second client stayed connected — handle
   `0x68A1B220`, `local_ptt=1`, owning a slice on **28.074 MHz DIGU with
   `tx=1`**. That is FT8 on 10 m (WSJT-X / MSHV / JTDX). Whatever owns
   the transmit slice owns the CW path, and CWX cannot have it.

   **Practical consequence for real operating:** this keyer's Flex
   backend cannot key while another client holds TX. That is a genuine
   constraint on the design, not a bug to fix — worth weighing against
   simply wiring the local key output to the radio's KEY jack, which has
   none of this contention and none of the latency.

#### RESOLVED 2026-09-10: paddle keying over the network works

**The keyer keys the radio over WiFi with no KEY or PTT wire.** This is
the headline feature and it is verified on the dummy load: the radio goes
`READY → PTT_REQUESTED → TRANSMITTING` under keyer control and releases
cleanly. Everything in the CWX section below is superseded — CWX was
never the right mechanism.

**How it works** (`pumpKeying()` in `src/flex.cpp`):

1. `xmit 1` on the first element. A bare keying command does nothing,
   because the radio only keys for whichever client holds the
   transmitter, and `interlock.tx_client_handle` stays `0x00000000`
   until one asks.
2. Per element:

       cw key <1|0> time=0x<16-bit ms> index=<decimal> client_handle=<GUI handle>

   `time` is milliseconds as **16-bit** hex (rolls at 0xFFFF), `index` a
   decimal counter, and `client_handle` the **GUI client's** handle — the
   keying happens in that client's transmit context. The timestamps let
   the radio schedule the edge rather than key on arrival, which is what
   keeps CW readable over a jittery link. Same mechanism as Maestro and
   MORCONI.
3. `xmit 0` after a 400 ms tail — suppressed while the key is down, or a
   long element (or tune) drops PTT out from under itself.

**Prerequisites, all of which fail silently:**

- **A slice must be in use AND in CW mode.** With no slice the radio
  transmits nothing and reports *no error at all*. This was the final
  blocker and cost hours. The keyer now subscribes to slice status and
  warns; `/status` shows readiness.
- SmartSDR (a GUI client) must be connected — with none, the radio
  reports `tx_allowed=0` and nothing may transmit.

**`cw key` vs `cw ptt`:** FlexRadio's wiki documents `cw ptt [1|0] time=
index=` as the keying command, and the radio accepts it without error —
but it did **not** produce RF here. Only `cw key` did (6600, SmartSDR
4.2.20). Both are switchable with `/flex cmd key|ptt`; `/flex bind` and
`/flex ptt` toggle the other two variables. Those switches exist because
only a power meter can settle which combination keys, and reflashing per
guess is what made this slow.

Sidetone stays local and is generated from the operator's own paddle
timing, so the fist sounds right in the ear regardless of the link.

The keyer task must never block on the network, so `Flex::keyEvent()`
only enqueues (`xQueueSend`, zero tick) and `poll()` does the socket
write.

**Three traps, all of which cost hours:**

- **`0x50001000` is not an error.** Per FlexRadio it means the command
  ran but the handler did not set a result, so the processor substitutes
  that code. Treating it as a failure made `cw key` look unsupported when
  it had worked from the first probe. **Check this before concluding any
  Flex command is unavailable.**
- **`xQueueSendFromISR` from the keyer task is wrong** — it is a task,
  not an ISR. Events silently never reached the socket.
- **`backend` is not the same as `flex enabled`.** The backend selects
  where keying goes and now persists in NVS (`wk`/`flexbe`); before that
  it silently reverted to local on every flash, which repeatedly made a
  working build look broken.
- **No slice in use = no RF, no error.** Check `/status` first when
  keying does nothing. Also check that a log line reports what was
  actually sent: the keying log printed `cw key` regardless of the verb
  in use for a while, which hid the one distinction that mattered.
- **Only a power meter can confirm keying.** Interlock state moves for
  PTT but is not proof of a carrier, and there is no RF reading over the
  TCP API. Every "is it working" question in this session needed the
  operator. Build the runtime toggles first next time.

#### Superseded: earlier verdict on CWX as a second client

Every configuration was tried on 2026-09-10, into a dummy load, and
`cwx send` was refused with `500000C2` in all of them:

| Configuration | Result |
|---|---|
| SmartSDR open, keyer unbound | `500000C2` refused |
| SmartSDR open, **bound to the GUI client**, slice in CW, `tx_allowed=1` | `500000C2` refused |
| Everything closed, fresh CW slice created by us | `tx_allowed=0` — nothing may transmit |
| Direct session from the Mac (keyer uninvolved) | `500000C2` refused |

At the point of refusal the radio also reported
`tx_client_handle=0x00000000` — nobody held the transmitter — so the
error text is not describing the actual state.

**Conclusion: CWX behaves as exclusively owned by the GUI client.** With
SmartSDR running it holds CWX; with SmartSDR closed there is no transmit
context at all. Binding (`client bind client_id=…`) is implemented and
demonstrably works — the keyer logs the bind and the radio accepts it —
but it does not unlock CWX. This matches community reports of CWX
conflicting with third-party programs.

This is a limitation of the approach, not a defect in the keyer: every
layer the keyer owns is verified working, and the refusal is identical
when the keyer is taken out of the loop entirely.

CWX remains unavailable to a second client, and that is fine — it is the
wrong mechanism for a keyer. It sends *text* for the radio to key itself,
which cannot carry a fist. Direct `cw key` keying (above) is the right
answer and works. CWX would only matter if buffered text from a logger
should be keyed by the radio rather than by us; the local keyer handles
that case already.

Wiring GPIO 33 to the KEY jack also still works and remains the
lowest-latency option, but it is no longer necessary.

#### Gotcha: slice indices are not stable

`slice set 0 …` failed with `5000000D` after SmartSDR closed, because
**slice 0 no longer existed** — the remaining client's slice was index 1.
Slices belong to clients and come and go with them. Never assume slice 0;
read `sub slice all` and use whatever index reports `in_use=1`.

Also learned: `sub interlock all` is rejected with `500000A3 Invalid
subscription object name` — interlock status arrives unsolicited anyway,
so do not subscribe to it.

- **Discovery does not reach it.** The radio is on the 192.168.1.x
  segment, the keyer is on 192.168.10.x, and discovery is a raw UDP
  broadcast. It will never be auto-found from where the keyer sits — set
  it explicitly: `/flex ip 192.168.1.50`, or use **Find radio** on the
  web page (added 2026-09-11), which TCP-scans a /24 from the keyer
  itself. The setting persists in NVS and reconnects across reboots.
- **`client program <name>` is rejected** by 1.4.0.0 with error
  `10000002 unknown client program`. Removed — the subscription is what
  matters and it succeeds.
- Finding it: nothing broadcast, so the radio was located by TCP-scanning
  the shack subnets for port 4992. A Flex answers immediately with
  `V<version>` / `H<handle>`, which makes it unmistakable. That manual
  scan is now built in (Find radio, `Flex::scanStart()`).
- **Not yet tested: actually sending CW.** `cwx send` keys the
  transmitter and puts a signal on the air under Manoj's callsign, so
  that test needs him present and the radio set up deliberately (dummy
  load or a clear frequency). Requires `/backend flex`.

## Testing

`tools/wk-test.py` acts as a WinKeyer host over TCP or serial;
`tools/wk-bridge.py` creates a PTY (default `/tmp/winkeyer`) bridged to
the keyer's TCP port so logging software sees a serial device.

**`tools/flex-check.py` — run this first when the Flex will not key.**
It reports every silent prerequisite in one pass (slice in use, slice
mode, GUI client, interlock, break-in) and with `--key` drives a keying
test and watches interlock for proof of transmission. Written after a
session lost hours to "no slice in use", which the radio never mentions.

**`tools/flex-ptt-watch.py` — use it for any stuck or late PTT report.**
It polls the keyer's `/api/state` at 5 Hz and holds a read-only API
session on the radio (subscriptions only — it cannot key), printing both
on one clock: `busy`/`key`/`ptton`/`xmit` changes against interlock state
and `cwx sent=` progress. It is what showed, 2026-09-11, that the radio
unkeys 0.67 s after its last character on every memory (its own CWX
break-in delay). A run of `err TimeoutError` on the KEYER side is loop()
stalling, not the network.

**`tools/ptt-check.py`** drives the keyer through its own API on the local
backend and polls `/api/state` at 10 Hz: the lead-in over after a long
idle, a message stopped mid-flight, tune stopped, a memory reporting itself
in `memplay`. Every one of those was a fault that looked like "the LED
flickered" or "the STOP worked" from outside (items 12c, 12d). No paddle,
no logger, no radio; it restores the backend it found, but it does key
GPIO33/32.

**`tools/host-watch.py`** prints every setting a host can reach as it
changes, including `pincfg` and `hostdef` — the raw bytes of the two
commands whose bit layout is not obvious from outside. It is what found
that RUMlogNG zeroes PTT timing and the pin configuration at session open
(12e, 12f). **Close and RE-OPEN the session** when using it: that is when a
logger sends its defaults, and a mid-session toggle can send nothing at all.

**`GET /api/wktrace`** (2026-09-13) is the host wire: last 1024 bytes both
ways, ms-stamped, `?clear=1` to reset. **`tools/wk-trace-check.py`** parses
it as WinKeyer commands and lists text the host sent that never came back as
echo. **`tools/wk-trace-collect.py`** polls it every 3 s for a whole session
and writes `drop-N.txt` when an echo goes missing (read-only, runs beside
RUMlogNG). **`tools/wk-echo-repro.py`** replays a message over TCP and checks
every echo (keys the radio; close the logger).

**`GET /api/flextrace`** (2026-09-17) is the radio wire: the last 128 lines,
ms-stamped. `>` lines are every command the keyer sends (`xmit`, `cw key`
with `time`/`index`/`client_handle`, `cwx …`, `sub …`). `<` lines are the
radio's replies (`R<seq>|<code>|`, so a refused command is visible), its
messages, and its interlock, cwx and client statuses. Slice statuses are
left out because they flood the ring when the radio is tuned. Lines are
cut at 119 characters, so the end of a long interlock status is lost.
`?clear=1` empties it. It is streamed in 1 KB chunks. Since 14:55 each
`cwx clear` is preceded by a `# clear: <cause>` line. This is the only way
to see the keyer↔radio traffic: the Mac cannot sniff it (item 13), and the
console is muted during a logger session.

**`tools/uptime-watch.py`** polls `/api/state` and prints only events —
restarts (with the reset reason), outages and their length, stalls. "The
board died" is useless; "uptime 225 -> 2 at 12:31:35, reason PANIC" is not.

**`tools/console-capture.py`** timestamps the console to a file WITHOUT
touching DTR/RTS, so opening the port does not reset the board — and holds
it through a reproduction attempt, which is how the lwIP backtrace was
caught. Decode with `xtensa-esp32-elf-addr2line -pfiaC -e …/firmware.elf`.

**`tools/boot-listen.py`** counts `rst:` lines at 115200 without driving
DTR/RTS. Repeating `POWERON_RESET` points at the supply; repeating
`SW_RESET` with no app output points at stale flash — **erase and reflash
before retiring the board**. Needs pyserial (not in PlatformIO's penv).

**`tools/wk-timing.py`** timestamps each status byte and counts KEYDOWN
against the text's real element count. Fixed drain windows produce false
negatives when a delayed burst lands outside its window — this was
mistaken once for "only one element was keyed" when the CW was correct.

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
- **2026-09-10 (later session)** — **speed pot wiring documented and its
  enable made persistent**; **OLED status panel** and **settings web
  page** added; new `settings.cpp` owning validation + NVS.
  - `/pot on` used to be RAM-only, so a wired pot went dead at every power
    cycle. Now persisted, along with its range (`/pot 10 35`).
  - Persistence rule established: **operator settings stick, host session
    settings do not.** A speed set over the WK protocol by a logger is
    gone at the next boot; a speed set from the panel or the web page is
    not. Without this a contest would silently leave the keyer
    reconfigured for good.
  - Display controller (SH1106 vs SSD1306) is a **setting, not a probe** —
    both answer at the same I²C address, so it cannot be detected. Default
    `sh1106` (Manoj's panel is a 1.3"). Wrong choice shows a 2 px shift
    with a garbage left edge; `/disp ssd1306` fixes it without a reflash.
  - Web page style borrowed from soft-MORCONI (`~/projects/Morconi`).
    That project turned out to be a browser UI + Node bridge with **no
    embedded server to reuse** — the look carried over, none of the code.
    Fonts are system stacks, not Google Fonts: the keyer often sits on a
    VLAN with no internet route and a font fetch would stall every load.
  - Verified: both envs build; the page's rendering, state polling and
    POST paths were exercised against a stub server (screenshotted).
    **Nothing here has been run on the ESP32 yet** — no panel has been
    wired, and no power-cycle test of NVS restore has been done.
  - Flash is now at **75.7%** of the 1.31 MB app partition on `esp32dev`.
    Worth watching before the OTRSP phase adds more; a bigger partition
    table is the escape hatch.
  - **Bench-tested the same day.** Persistence verified across a hard
    reset (wpm, mode, pot enable, pot range). Web page and API exercised
    from the Mac across subnets. Two things found and fixed:
    - **The I²C probe ran only at boot**, so a panel wired to a running
      board stayed dark with no explanation. Added `/i2c` (full bus scan
      with an ordered list of physical causes) and hot-adoption from both
      `/i2c` and `/disp on`.
    - **The panel was found on some boots and not others.** Detection was
      running at 400 kHz, which this panel only manages intermittently on
      breadboard leads. Detection now always runs at **100 kHz**.

      **Rendering had the same problem and the first fix was wrong**
      (corrected 2026-09-11): it kept 400 kHz for frames whenever the panel
      answered its *address* at 400 kHz. That is not evidence — an address
      probe is one byte, a frame is a thousand. The panel passed the probe
      and drew nothing, presenting as a display the firmware reported as
      present and enabled while the glass stayed dark, which cost a long
      detour through imagined causes. The bus is now 100 kHz for everything
      unless `/disp fast` opts in; the choice persists in NVS.
    - Gotcha for future sessions: **opening the CP2102 port resets the
      board.** Serial captures during Manoj's live web-UI testing were
      rebooting it under him, and a burst of those resets reads exactly
      like a boot loop in the log. Use HTTP (`/api/state`) to observe a
      running board; use serial only when a reset is acceptable.
    - Live demo of why the pot defaults off: with the knob not yet
      connected, enabling it let the floating GPIO 34 drive the speed,
      which wandered 20→33 WPM on its own.
  - **Both the OLED and the speed pot are now wired and confirmed working
    on hardware.** SH1106 at 0x3C rendering at 400 kHz, no controller
    override needed (the `sh1106` default was right for the 1.3" panel);
    the pot tracks properly on GPIO 34. That closes the display and pot
    lines of the roadmap — everything in this feature set has now run on
    real hardware.
  - **The two-tails bug.** `/tail` and the web PTT panel wrote the keyer's
    `cfgTailMs`, which sequences the local PTT line — but `flex.cpp` kept
    its own private `pttTailMs = 400` for releasing the radio (`xmit 0`).
    On the Flex backend the operator therefore heard no change from any
    value, because the thing he was listening to was never being set.
    `Settings::apply("tail")` now drives both, and `/api/state` reports
    `flextail` alongside `tail` so the two can be seen to agree.

    Related and easy to get wrong (it was, twice, in comments): on the
    Flex backend the **KEY line GPIO33 is idle** (`setKeyOutEnabled(false)`
    so the rig is not keyed twice) but the **PTT line GPIO32 is still
    live**, gated only by `/ptt`, for an amp or sequencer. Lead-in applies
    to GPIO32 on both backends; the radio does its own T/R, so there is no
    Flex lead.
  - **The console was corrupting the host stream.** Every `Serial.printf`
    in the firmware wrote ASCII to the same wire the WinKeyer session used,
    so `[FLEX]` lines appeared in RUMlogNG's CW window as text. All console
    output now goes through `src/log.cpp`, muted for the duration of a
    serial host session. Diagnose over the web page instead; it does not
    share the port.
  - **Local sidetone while the radio keys.** On the Flex backend buffered
    text goes straight to `cwx send`, so the operator heard nothing at all
    while transmitting. The text is now also run through the local keyer
    for monitor sidetone (`/monitor`, default on). The trap: those elements
    must NOT reach the key hook or the radio is keyed twice — hence
    `Keyer::setHookPaddleOnly()`. Watch out that `curIsAuto` is stale
    outside `startElement()`, which silently swallowed `tune` until fixed.
  - **A logger could leave the keyer reconfigured.** RUMlogNG sets PTT
    lead/tail to zero via WK command 0x04 — normal host behaviour — but
    `resetToDefaults()` never restored the operator's values, so the zeros
    survived the session and Manoj found lead/tail at 0 after a flash. Now
    `Settings::restoreKeyer()` runs on host close AND on transport drop.
    The same command also set only the keyer's tail and not Flex's, the
    identical two-tails split as before: fixed in the protocol path too.
    **Superseded 2026-09-12 (evening): the host no longer sets PTT timing
    at all** — see item 12e. Restoring on close was only ever half a fix,
    because the zeros were in force for the whole session.
  - Web page prose moved to hover help (dotted labels, `title=`), on
    request — the panel had grown more explanation than controls.
  - **Echo was arriving all at once.** RUMlogNG showed the first message
    and then nothing. Echo is a timing signal — the host highlights the
    character being SENT — but the Flex path hands the buffer over in one
    batch, so echoing at queue time put the host's highlight ahead of the
    air and it discarded everything after. Now paced against `cwx sent=`.
  - **Paddle echo did not exist.** Manoj found hand-sent characters were
    never echoed. Two independent causes: `applyModeRegister()` handled
    only bits 5:4, 3 and 2 and ignored bit 6 entirely, so there was no
    path from paddle to host at all — and RUMlogNG sets `0x07`, which
    never requests it, so a correct implementation would still have been
    silent. Both addressed: the keyer now decodes hand-sent characters
    (exact reverse lookup of the elements it generated, not a signal
    decoder) and `/pecho on|off|auto` overrides the host, defaulting to
    auto. **Not yet confirmed against RUMlogNG** — needs a paddle test.
  - **Process note: an on-air transmission was made without asking.** The
    echo timing was verified by sending `TEST DE VU2CPL` while the backend
    was Flex and the slice was in CW mode, so it went out on the air.
    Correctly identified, but the operator had not been asked. Verify
    keying behaviour on `/backend local` unless on-air is explicitly
    agreed.
  - **Random reboots from a stack overflow in `/api/state`.** The state
    document started as `StaticJsonDocument<640>` and was grown three
    times in one session — to 1024, 1536, then 2560 — as fields were
    added, without anyone noticing it lives on **loopTask's 8 KB stack**.
    Adding six `String` copies of the message memories and a `String` for
    the serialised output beside it pushed it over. The page polls that
    endpoint once a second, so it presented as the board rebooting at
    random rather than as anything pointing at JSON. Now a
    `DynamicJsonDocument` on the heap. After the fix: 160 consecutive
    polls and 20 NVS writes with no failures, against 0/60 before.

    **Rule: nothing large goes on the stack in a polled handler.** If the
    state document grows again, it must stay on the heap.
  - **PTT on the Flex backend now follows the radio, not the local
    monitor.** Buffered text goes to the radio as `cwx send` while a copy
    runs through the local keyer for sidetone; timing GPIO32 from that copy
    meant two independent CW generators drifting apart, so PTT was held
    progressively longer on longer transmissions. The line now follows
    `xmitOn` for paddle keying and `pending()` — fed by the radio's own
    `cwx sent=` reports — for buffered text. **Not yet confirmed by Manoj**
    on a long over.
  - **The radio's speed only ever followed a host's in-band escape.**
    `Flex::setWpm()` had exactly one caller, so a speed set from the pot,
    the web page, the CLI or the WK set-speed command moved the local
    keyer and left the radio at its previous rate. Now polled in `loop()`
    and pushed on change, which catches every path including the pot
    (which updates from the 1 kHz task and must not do network work).
  - **Never leave a transmitter keyed.** `Flex::keyEvent()` dropped queue
    entries when full "rather than stall element timing" — fine for a
    key-DOWN, fatal for a key-UP, because `keyIsDown` then stays true and
    the `xmit` release is gated on it. Key-ups now evict the oldest entry
    rather than being dropped. Both PTT paths gained absolute backstops
    that LOG when they fire (radio: 5 s with no key event; local line:
    10 s of PTT with no keying, tune excluded). A backstop firing means a
    transition was lost upstream — treat it as a bug report, not a fix.
  - **`/api/state` now carries `resetreason` and `uptime`.** The reset
    reason is printed to serial exactly once at boot, and a logger usually
    owns that port, so every crash during real operation used to destroy
    its own evidence. Ask the board over HTTP instead.
  - **A fresh board exposed two first-boot bugs.** Every ESP32 this
    project had run on was already written to, so nothing exercised an
    empty NVS: the settings namespace was opened read-only, and a
    read-only open of a namespace that has never been written fails
    *slowly* (~630 ms, logged), once per poll of `/api/state`. Created
    read-write up front now. Flex was also unreachable from the web page
    (enable and IP were CLI-only) and its keying options — key verb, bind,
    xmit — were never persisted at all, so they had to be re-entered after
    every reflash.
  - **Slice tracking was substring-matching.** `indexOf("mode=")` also
    matches `agc_mode=`, `rfgain_mode=` and `tx_ant_mode=`, so which value
    was read depended on field order — a slice switched to USB registered,
    switched back to CW did not. Keys are tokenised now, and a `slice list`
    snapshot is requested at connect because a subscription delivers only
    deltas. Both fixes came from **soft-morconi's bridge**, which had
    already solved this; that project is now a private repo rather than
    three untracked files.
  - **Keying is now tied to the FlexRadio enable** (2026-09-11): enabling
    moves keying to the radio, disabling returns it to the local key line,
    and the page drops Flex from the Keying choices while it is off. The
    half-states were traps — keying a disabled backend sends CW nowhere
    while the UI still claims the radio.
  - **`tools/web-preview.py` serves the settings page from `src/web.cpp`
    with a stubbed API.** Use it before flashing any UI change. Three
    faults reached hardware in one session past a clean build, each
    invisible to the compiler: a line in the wrong scope blanked the whole
    page, `hidden` on a `.row` did nothing because `.row{display:flex}`
    outranks it, and `hidden` on an `<option>` is ignored by Safari (but
    honoured by Chromium, so it "verified" fine). **Check what is
    RENDERED** — `getComputedStyle`/`offsetParent` — not what
    `element.hidden` reports.
  - **Settings left behind by testing** (they persist, so they are real):
    pot range is **12-40 WPM**, not the 10-35 default. `/pot 10 35` to
    restore. Speed and mode were also written during the persistence
    test.
- **2026-09-11 (day)** — **Find radio: a LAN scan for the Flex**, plus the
  old board brought back.
  - Manoj had to type the radio's IP: discovery listens for a UDP
    broadcast and the radio (192.168.1.x) and keyer (192.168.10.x) are on
    different segments, so it could never hear one. `Flex::scanStart()`
    sweeps a /24 for TCP 4992 in its own task (core 0, prio 1): six
    non-blocking connects at a time, 300 ms per batch — lwIP has 16
    sockets and the firmware already holds several, and a host that cannot
    get a socket is retried rather than skipped. A host with 4992 open
    must greet with `V` to count; then `info` gives model and nickname.
    Web: a Find radio row under Radio IP, `/api/flexscan` POST/GET.
    Verified on hardware: `192.168.1.50 · FLEX-6600 · 6600` found and
    adopted in one click.
  - **Bug caught on hardware, not the stub:** the first build listed the
    radio with a blank model. The radio's greeting is ~1.4 KB (`V`, `H`,
    a client-connected message, then radio status), the read buffer was
    768 bytes, and the read stopped when it filled — before the `info`
    reply arrived. The buffer now slides.
  - **Old board (the original ESP32) was stale flash, not dead.** It
    boot-looped on USB (31 resets in 12 s) and still looped on an
    external supply — but there as `rst:0x3 SW_RESET`, never reaching
    `setup()`. A full `pio run -t erase` and reflash fixed it on both
    supplies. A normal flash only rewrites the regions it touches; old
    content elsewhere was tripping the bootloader. **Try an erase before
    retiring a board.** The erase wipes WiFi credentials and all NVS, so
    it comes back as the setup AP with default settings (txpower 11).
  - **Memory-play PTT: not reproduced.** After one report of PTT staying
    on after a memory, the keyer (`/api/state` at 5 Hz) and the radio (a
    read-only API subscription: interlock + `cwx sent=`) were logged on
    one clock through 7 memory plays and a paddle session. Every time the
    radio unkeyed **0.67 s after its last character** — its own CWX
    `break_in_delay` (782 ms), not us — and the local sidetone copy ended
    within 0.3–1 s of the radio. The one hang seen was already in progress
    when logging started (radio TRANSMITTING src=SWCW with the keyer idle,
    released ~6 s later), so its cause is unknown.
- **2026-09-12** — **Onboard LED shows keying; MQTT can no longer stall
  the loop; a dead keyer leaves the Flex transmitting.**
  - **GPIO2 LED follows the key** — set in `keyDown()`/`keyUp()` beside the
    sidetone, so it tracks every element on every backend. The 10 s
    heartbeat toggle was removed (it would have left the LED lit at
    random); the MQTT heartbeat message is unchanged. Verified with a
    5-blink boot test that the devkit's only LED is on GPIO2.
  - **`secrets.h` had no `MQTT_HOST`**, so the build used the public-repo
    placeholder `192.168.1.10`, which does not exist. Every 60 s (the
    backoff cap) `mqttConnect()` sat in WiFiClient's 3 s TCP connect,
    blocking `loop()` — which is also where `Flex::poll()` sends key-ups
    and `xmit 0`. `tools/flex-ptt-watch.py` showed an HTTP timeout at :04
    past every minute for 30 min. The retry guard checked only
    `Keyer::busy()`, which is already false in the PTT tail, so a retry
    could start while the radio was still keyed and hold it for the stall.
    Fix: the socket is opened with a 500 ms cap before `mqtt.connect()`
    (PubSubClient skips its own connect when the socket is up), and the
    guard also requires `!Keyer::pttIsOn() && !Flex::transmitting()`.
    After: 738 polls over 150 s, zero timeouts, worst 73 ms. `secrets.h`
    now sets the real `MQTT_HOST` (local only — never commit it; this repo
    is public); `[MQTT] connected` as `iot`.
    The broker ACL needed `topic write shack/esp32-winkeyer/#` under
    `iot` — `iot` cannot write `shack/` by default and the broker drops
    such publishes silently. Publish flow not yet confirmed from a reader.
  - **A dead keyer leaves the Flex in TX, indefinitely.** Twice this
    session the keyer went down mid-over and the radio stayed
    `TRANSMITTING src=SW` until an `xmit 0` was sent from another API
    client. The radio's interlock reports `timeout=0` — no TX time-out —
    so nothing radio-side releases it. The keyer's own backstops die with
    the keyer. The second outage was a reset loop while paddling (29
    `rst:` lines in 8 s) — cause OPEN, see 11x. It is **not** the 11w
    brownout: the board has an external supply as well as USB.
  - **Slice warning on the web page and OLED.** Manoj's memories played
    sidetone but never transmitted, while the paddle still keyed PTT: the
    slice was in LSB. `cwx` only sends on a CW slice, but `xmit 1` puts the
    radio in TX in any mode, so "paddle works, memories don't" is what a
    wrong mode looks like. The keyer knew (`slice:false`) and said so only
    on the serial console, which is unplugged or muted in real use.
    `Flex::sliceWarning()` now gives the reason with the mode name; the
    page shows an amber banner under the LEDs (`flex.slicewarn` in
    `/api/state`) and the OLED title reads e.g. `SLICE USB, NOT CW`.
    Verified on hardware by Manoj. Then extended to the HD44780 LCD, with
    Manoj choosing where it goes: 20x4 row 4 (dBm/tail) gives way; on the
    16x2 it alternates with the IP every 2 s when not sending
    (`lcdPhase()`, and it joins the redraw signature only on a 16x2 so
    the OLED does not resend frames). Three lengths from
    `Flex::sliceWarning()`: LONG (web), SHORT ≤18 (OLED, 20x4), TINY ≤16
    (16x2). **LCD version flashed but NOT yet seen on a panel.** Manoj
    will test later. Still not done: tracking the TX slice specifically.
    The flags follow whichever slice reported last, so two open slices
    can mislead.
  - **Restart hunt, external power only (no USB, 0 W so no RF):** 15 min,
    12 overs, no restart. One 3 s WiFi drop and a few ~1 s HTTP replies,
    all while keying. A drop mid-over would hold the radio in TX for
    its length — another stuck-PTT candidate. USB is plugged back in now,
    so the next restart will say whether the USB lead matters.

- **2026-09-12 (morning)** — **the resets are the USB port's control
  lines, and a release fix for stuck PTT.**
  - **ROOT CAUSE of every unexplained reset and most stuck PTT: `RTS`
    asserted with `DTR` deasserted holds EN low.** Measured by holding the
    port and stepping all four combinations, checking liveness over WiFi
    after each: (0,0) up, (1,0) up, **(0,1) DEAD**, (1,1) up. In the dead
    state the chip prints NOTHING (not even a ROM banner — so it is reset,
    not download mode) and boots normally the instant the lines change.
    Every other combination change causes a reset pulse, so a logger
    opening or closing the port reboots the keyer. RUMlogNG holds the port
    while it has a WinKeyer session; a real K1EL ignores DTR/RTS, so this
    is a devkit problem, not a logger bug. **No firmware can defend against
    its own reset pin** — 11w's brownout theory and the "EN noise" theory
    are both dead. Fix options are in README (disable the auto-reset link
    to EN; or a serial adapter with only TX/RX/GND; or the S3 env).
  - **Stuck PTT had a second, independent cause, now fixed.** The release
    path only ever sent `xmit 0`, which does NOT clear a key the radio
    thinks is still down (interlock `source=SWCW`), and the whole release
    was gated on `xmitOn` — so when a key-up was lost, the keyer's own
    state said "idle" and nothing ever released the radio. Caught live at
    09:20 with the keyer idle and the radio transmitting; `cwx clear`
    returned `2875,2894`, i.e. 19 characters the radio never sent. Now:
    every release sends a real `cw <verb> 0` first; the 5 s backstop fires
    on `xmitOn || keyIsDown`, adding `cwx clear`; a new watchdog subscribes
    to `sub tx all` and, if the radio reports transmitting CW while the
    keyer is idle for 5 s, forces a key-up and `cwx clear` (once per
    transmission, and only for `source=SWCW`, so MSHV and SmartSDR are
    untouched); and a reconnect sends a key-up, since a link that dropped
    mid-element never delivered one. Verified once on hardware (clean
    release at 09:29:10); the watchdog path has NOT yet been seen firing.
  - `tools/flex-ptt-watch.py` plus a scratch HTTP uptime watcher and an
    `lsof` port-opener poller are what made this visible. The port poller
    is what caught RUMlogNG holding the port across a failure.

- **2026-09-12 (late morning)** — **the mystery deaths were an lwIP crash,
  and three keying bugs found by watching the radio.**
  - **CRASH, root cause, with a decoded backtrace:**
    `assert failed: pbuf_free ... (p->ref > 0)` →
    `WiFiClient::read()` → `Flex::pollSocket()` → `loop()`. The socket was
    read **one byte at a time** (`while (tcp.available()) tcp.read()`), and
    Arduino core **2.0.17**'s `WiFiClientRxBuffer` can double-free a pbuf
    when the socket is torn down mid-read. It fired after the radio's
    status burst that follows a memory. Both read loops are now block
    reads with a `connected()` guard and a `n <= 0` bail —
    `Flex::pollSocket()` and `Net::poll()`. **This narrows the window; it
    does not fix the library. A recurrence means the core upgrade is the
    real fix.** Presents as: board stops dead, radio left transmitting, no
    reboot, RST needed — indistinguishable from the DTR/RTS reset fault
    without a console, which is why both hid behind one symptom for a day.
  - **How to catch it again:** `/baud 115200` (web page or
    `POST /api/set?k=baud&v=115200`), hold the FTDI port with a capture
    script, reproduce, then decode with
    `xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/esp32-winkeyer/firmware.elf <addrs>`.
    At 1200 baud the panic never finishes printing — we got the assert
    line and no backtrace, twice. A logger holding the port hides it
    entirely.
  - **`Flex::tryConnect()` had no connect timeout.** WiFiClient's default
    runs to tens of seconds, in `loop()`; a wobble on a weak link parked
    the whole keyer and the **30 s task watchdog** reset the board (seen:
    reset reason `task WATCHDOG`). Now `tcp.connect(ip, port, 1500)`.
    Same bug as the MQTT connect fixed earlier the same day — when one
    turns up, grep for the others.
  - **`cwx clear` must never fire on a live message.** The morning's
    backstops cleared the radio's buffer on a deadline, and a logger hands
    a memory over in pieces, so the deadline expired mid-send: the radio
    logged `cwx erase=2981,2985` and every memory lost its tail. Both
    clear paths now also require **no `cwx sent=` progress for 5 s**
    (`lastCwxMs`). Verified: no `erase` since.
  - **Stuck local PTT (item 11y) — cause found and fixed.** `pending()`
    stays above zero after a memory because the `cwx send` reply indexes
    the block's FIRST character, so the line was held until the keyer's
    10 s backstop dropped it — `[KEYER] PTT was stuck with no keying`,
    logged at 11:05:57 and 11:06:32. Now the radio's own interlock ends
    it: not transmitting + no progress for 1 s ⇒ `queuedIdx = sentIdx = 0`.
  - Console at 1200 8N2 is what a logger needs; 115200 is what debugging
    needs. It is a runtime setting — switch it over HTTP, no reflash.

- **2026-09-12 (midday)** — **Arduino core 3.3.11 / IDF 5.5.5, and it fixed
  the crash.**
  - Platform is now the **pioarduino** fork, pinned to release `55.03.311`,
    because official PlatformIO's espressif32 stops at Arduino 2.0.17.
    `board_build.partitions = huge_app.csv` — the new core hit **94.8%** of
    the default table; it is 39.5% of the 3 MB one, and nothing here uses OTA.
  - **Result: 26 memories sent back-to-back, no crash, uptime continuous.**
    The old core crashed within minutes of the same test, every time.
  - **Requires PlatformIO on Python 3.10+.** The Mac's PlatformIO runs on
    3.9 and simply refuses. A separate one lives in `~/.pio-venv313`;
    `flash.sh`, `monitor.sh` and `install.py` all locate a suitable one
    (checking the interpreter version, not just that `pio` exists) and
    `install.py` offers to create it. `PIO=` overrides.
  - **Code changes the upgrade forced:** `ledcSetup`/`ledcAttachPin` →
    `ledcAttach` addressing the PIN, and `esp_task_wdt_init()` now takes a
    config struct (with `esp_task_wdt_reconfigure()` when Arduino already
    started it).
  - **Two real bugs the stricter core exposed**, both silent on 2.0.17:
    - **Radio 2's KEY/PTT (GPIO 18/19) were never `pinMode`d**, so those
      lines were never driven — `/radio 2` and `/radio both` cannot have
      worked on any earlier build. Now declared and dropped low at boot.
    - The **speed pot's attenuation** was set on a pin that had no ADC
      channel yet; a first `analogRead()` creates it.
  - **`pttAssert()` set `pttOn` even with the PTT line disabled**, so the
    web page and panel showed PTT active while nothing was driven — the
    exact thing that misleads someone debugging a dead PTT wire. Fixed
    while answering whether a friend's radio-1 PTT fault was the same bug
    (it was not: radio 1's path was always correct).
  - Flashing over the FTDI adapter: `--before no_reset --after no_reset`,
    with BOOT held and RST tapped by hand. PlatformIO's own upload works
    too and gets the offsets right, which matters now the table changed.

- **2026-09-12 (early afternoon)** — **console leak closed properly, and
  the sidetone now tracks the radio.**
  - **The console leak took three goes, and the first two were wrong.**
    (1) Muting `Log::` was not enough: the boot banner, reset reason and
    watchdog line printed through `Serial` directly, and the **ESP core's
    own logger** writes to the UART without passing through `Log::` at all
    — that is where `E (1816) task_wdt: ...` in RUMlogNG's CW window came
    from. `Log::setMuted()` now drives `esp_log_level_set()` too. (2) "Any
    printable byte means a human is typing" was a bad heuristic: WinKeyer
    traffic is full of ordinary text, so the logger's own data un-muted the
    console. Gone; `/log on` is the only way in. Policy now: **1200 baud ⇒
    console silent from the first character**, any other rate ⇒ console on.
    What remains is the ROM banner at 115200 on every reset — a character
    or two of noise at 1200, and not suppressible in firmware.
  - **Sidetone delay, measured not guessed.** Manoj heard the sidetone
    ~0.5 s ahead of the air and asked whether the board could work the
    delay out itself. It can: `cwx send` → interlock TRANSMITTING is
    exactly the start latency. Smoothed (`startLatency`), only timed when
    the radio was idle first, and applied by holding the monitor copy in a
    small queue (`monQ`). **Measured 229 ms** on this radio/WiFi.
    `/mondelay auto|0..2000`, persisted, with the web page showing both the
    applied and the measured value. Manoj: "almost perfect".
  - RUMlogNG lost its session across every reflash and does NOT re-open it
    by itself — it sits on the port doing nothing, so nothing keys. **Fully
    quit and restart it after a flash**, not just the CW window. Its
    settings (echo, Farnsworth) go with the session, which is why echo
    vanished mid-session.
  - `tools/wk-test.py --serial <port> --baud 1200` proves the protocol
    end-to-end in seconds, and is how the keyer was cleared of blame.

- **2026-09-12 (wrap-up)** — **`docs/wiring.svg`**: the station on one page
  — paddle, pot, piezo, I²C panel, both rigs' key/PTT, USB-C power, the
  WiFi services, the FlexRadio and how it is keyed with no KEY/PTT wire,
  and the second USB-serial adapter used as a listen-only console (RX +
  GND, TX deliberately absent). The "why there are two serial paths" note
  is on the drawing so nobody tidies the second adapter away. Also moved
  the two diagnostics that had been living in a scratchpad into `tools/`.

- **2026-09-12 (evening)** — **the PTT safety backstop was firing on
  legitimate transmissions.** Reported by a Windows user on the local
  backend, keying by hand: *"on first tx, ptt blinks and then off, next tx
  onwards it works."* See item 12c — one stale timestamp, fixed in
  `pttSafety()`, plus an FSK keep-alive so an RTTY over longer than 10 s
  keeps its PTT.

12a. **STILL OPEN, intermittent (2026-09-13).** After the Flex echo pacing
    rewrite (12h) Manoj's RUMlogNG session still dropped a letter from the
    echo "several times" — a random letter mid-message (first seen: V of
    VU2CPL in `cq cq vu2cpl vu2cpl k`), radio transmitting it fine, web page
    open, nothing else happening. NOT reproduced: 24 sends of the same text
    over TCP (paced at 1200-baud byte spacing and in bursts, with and
    without `/api/state` polled like the page) all echoed completely, and a
    traced RUMlogNG session (`/api/wktrace`, 11 messages) dropped nothing.
    Next drop: pull the trace at once and run `tools/wk-trace-check.py` — it
    says whether the keyer emitted the letter (then the loss is between the
    wire and RUMlogNG's window) or not. Original report: Reported 2026-09-12 at wrap-up: characters missing from the
    host echo, while the radio sends them all.** So the text reaches the
    radio; only the echo stream back to the logger is short.

    Where to look, in order:

    - `WinKeyer::pumpEcho()` releases echoes as
      `echoCount() - Flex::pending()`. `pending()` changed twice today —
      it no longer clears early, and it is now zeroed when the radio stops
      transmitting — so the echo release is paced by a counter with new
      behaviour. Suspect echoes left sitting in `echoQ` at the end of a
      message and surfacing during the NEXT one, which reads as "missing"
      at the time and "wrong" later.
    - `queuedIdx` is absolute (the radio's buffer index, from the
      `cwx send` reply) while `sentIdx` starts at 0 after the reset, so
      `pending()` is briefly enormous and the first characters' echoes are
      withheld until the first `cwx sent=` arrives.
    - The link is 1200 baud: status bytes, pot bytes and echo share
      ~120 char/s. Saturation would delay, not drop — but worth measuring.

    How to capture: `tools/uptime-watch.py` for liveness,
    `tools/flex-ptt-watch.py` for `cwx sent=`, and RUMlogNG's CW window
    for what actually arrived. Compare all three against the text sent.
    Ask Manoj whether it is the FIRST characters, the LAST, or scattered —
    that alone separates the three hypotheses above.

12b. **Windows build failure at the platform's own penv (2026-09-12).**
    VU2LBW, building the same repo on Windows 10, got the platform and
    `tool-esp_install@5.3.4` to download and then:
    `Error: Failed to install Python dependencies (exit code: 2)` /
    `Failed to install Python dependencies into penv`. That is PlatformIO
    building its own Python environment for the ESP32 platform — nothing
    to do with this firmware, and it will stop ANY pioarduino project.

    Not yet diagnosed on his machine. Candidates, in the order worth
    trying: an old PlatformIO core (the platform wants a recent one); a
    half-installed platform that never repairs itself (delete
    `~/.platformio/platforms/espressif32*` and
    `~/.platformio/packages/tool-esp_install*`); the **Microsoft Store
    build of Python**, whose sandboxed paths break virtualenv creation;
    antivirus or a proxy blocking the pip step. `pio run -v` prints the
    real pip error — ask for that before guessing further.

    `install.py` now prints the PlatformIO core and Python it is about to
    use, flags a Store-Python install, and prints this list when a build
    fails, so the next person gets the facts rather than "it failed".

12c. **FIXED 2026-09-12 (evening), needs confirming on the reporter's
    board: PTT dropped one millisecond into the lead-in.** VU2LBW's report,
    keying by hand on the local backend: *"on first tx, ptt blinks and then
    off, next tx onwards it works."*

    Cause: the 10 s safety backstop (`pttSafety()`, `src/keyer.cpp`) timed
    its idle window from `lastKeyDownMs` — **the last element keyed, which
    can be from a transmission minutes ago** — and nothing else. The
    sequence on any over that starts after a quiet spell:

    1. Paddle pressed. `startActivity()` asserts PTT and enters `ST_LEAD`
       for the 50 ms lead-in. No element has been keyed yet.
    2. One tick later `pttSafety()` runs — before the state machine — sees
       PTT up, `keyDownFlag` false, and a `lastKeyDownMs` older than 10 s.
       It drops the line and sets the "stuck" flag.
    3. The lead-in expires and the whole over is sent **with PTT down**.
       Nothing re-asserts it: PTT goes up only on the way out of idle.
    4. The next over, keyed while the stamp is fresh, is fine — until the
       operator pauses for more than 10 s again.

    So "first tx" is really **the first over after any gap longer than
    10 s**, which on a bench is nearly always the first one. The only over
    that was immune was the very first after boot, where `lastKeyDownMs` is
    still 0 and the backstop is disabled. The console prints
    `[KEYER] PTT was stuck with no keying` each time it happens — that line
    is the confirmation to ask for.

    Fix: the deadline now runs from the LATER of "PTT came up" (`pttUpMs`,
    stamped on the rising edge only, so a repeated assert cannot push the
    deadline forward for ever) and "an element was keyed". The backstop
    still fires after a genuine 10 s of a held line with no keying.

    Second bug, same root: **RTTY.** `Fsk::send()` holds PTT for the whole
    over through `Keyer::pttManual(true)` and keys no CW elements, so any
    over longer than 10 s — most of them — would have lost its PTT. FSK now
    calls the new `Keyer::pttKeepAlive()` as each character starts.

    Likely also the explanation for **item 11y's unexplained recurrence**
    and for the 2026-09-11 observation that on Flex memory plays "the line
    comes on only for a moment at the start": the Flex path asserts PTT
    when the radio starts transmitting, and the local monitor copy does not
    key its first element until the sidetone delay (~230 ms) has elapsed —
    a window in which a stale `lastKeyDownMs` drops the line. Plausible,
    not proven: it was never captured.

    **Flashed and verified on Manoj's board, 2026-09-12 evening.** Test
    driven entirely over HTTP, no paddle needed: `/backend local`, send one
    character to stamp the last-element time, idle 14 s (longer than the
    backstop window), then send `TEST` while polling `/api/state` at 10 Hz.
    `ptton` was true from 0.05 s to 1.85 s — lead-in, the whole over with
    `key` toggling inside it, and the 400 ms tail after `busy` cleared.
    Before the fix that over would have had PTT down throughout. Backend
    restored to `flex` afterwards; radio reconnected.

    That `/backend local` + `/api/send` + poll `ptton` recipe is the way to
    test PTT sequencing without a paddle or a logger, and without keying
    the Flex — nothing is wired to GPIO32/33 on this board.

    **Why this never showed on Manoj's own station:** RUMlogNG sets the PTT
    lead to 0 for its session, and with a zero lead-in the first element is
    keyed in the same millisecond PTT comes up, so the backstop never got
    its window. It needs a nonzero lead — the operator default is 50 ms —
    which is what VU2LBW was keying with by hand.

12d. **Settings-page layout faults and the SEND/STOP merge (2026-09-12,
    evening).** All reported from a screenshot of Manoj's own window; all
    verified in `tools/web-preview.py` at 1130px and 880px before flashing.

    - **`ms` and `WPM` suffixes wrapped to their own line** in PTT, TIMING
      and SPEED POT. `.val` reserves 56px so a CHANGING readout does not
      resize the slider beside it on every drag — but a fixed unit is not a
      readout, and label (92) + field (92) + unit (56) + gaps overflowed a
      196px-minimum panel column by a hair. Units now use `.unit`, which
      reserves nothing and never wraps; number fields are 78px.
    - **Speed-pot Range wrapped** — two fields, a joining word and a unit
      never fit one column. That row is now full width.
    - **Host baud hung outside the card.** A `<select>` is as wide as its
      longest option and will not shrink below it, so "1200 8N2 — WinKeyer
      standard" ran past the panel border. The row is full width, WiFi
      power with it, plus a `select{max-width:100%}` guard for the future.
    - **MEMORIES now matches the BACKEND card's height** (`.stretch` —
      `align-self:stretch` against `.rig{align-items:start}`); they share a
      grid row and ended at different heights.
    - **SEND and STOP are one button**, in both the SEND and FSK panels:
      SEND when idle, red STOP while `busy`/`tune` (or `fskbusy`), driven
      by the 1 Hz poll with an optimistic flip on click. The page's STOP
      used to call `/api/tune?v=off` and could not stop a message or a
      memory at all. It now posts `/api/send?stop=1` → new
      `WinKeyer::abort()` (the internals of host command 0x0A: local buffer,
      keyer queue, and on Flex the radio's buffer, echo and monitor) plus
      tune off.
    - `tools/web-preview.py` had no `txpower` in its stub, so the WiFi power
      select rendered blank in the preview and looked like a page bug.

    Second round, same evening, all flashed and checked on the board:

    - **STOP is now per control, not per page.** TUNE becomes STOP while
      tuning, each memory's PLAY becomes STOP while THAT memory is going
      out, and the text SEND keeps the rest (`busy && !tune && !memplay`).
      Manoj's steer: the stop for a memory must not appear on the text send
      button. Needs `Memories::playing()` — the slot last started, cleared
      the moment `Keyer::busy()` goes false — exposed as `memplay` in
      `/api/state`. "Busy" alone cannot say WHICH row to mark.
    - **Sidetone range is 300-1000 Hz** (was 300-2000), default 600
      unchanged. 2000 was there because the host command can ask for it;
      the cost was that everything anyone uses, 500-800, sat in the first
      quarter of the slider.
    - **The host sidetone command was reading its flag bits as part of the
      frequency.** K1EL 0x01: the LOW NIBBLE is N, tone = 4000/N, and bit 7
      means paddle-only sidetone. `4000 / p[0]` on the whole byte sent
      0x85 (paddle-only, N=5) to 30 Hz — the floor — and a small N to the
      ceiling. Masked to `p[0] & 0x0F`.
    - **Manoj's stored tone was above the new ceiling**: the board booted
      reading 1000 (clamped), so NVS held 2000. Set back to 600 over the
      API and stable since. How 2000 got SAVED is not established — the
      host command only writes RAM, so it took a slider click on the page
      as well. **If it runs to the top again the moment a logger connects,
      the host command is the culprit and its tone should be made
      session-only or ignored outright.**

    **Chased down and settled the same evening.** The board was watched
    across a reboot and a RUMlogNG connect: it came up at 600 from NVS with
    no host, and went to **1000 the moment the session opened**. That is
    RUMlogNG sending N=4 — a perfectly legal 1000 Hz, not a bug in the
    parse and not the clamp. **The host's sidetone command is now parsed and
    ignored** (`case 0x01`, `winkeyer.cpp`): the pitch is the one setting
    here that only the operator hears — not timing, never on the air, never
    read back by the logger — so a logger does not get to take it. The
    earlier NVS 2000 is explained by the same override plus a slider click
    saving what the page was showing. Verified: 600 at boot, 600 with the
    session open.

    - **FSK panel: Baud, invert and diddle on one row overflowed at every
      width**, leaving `diddle` wrapped underneath on its own. Split — Baud
      and its select on one full row, the two switches on the next behind a
      92px spacer so they line up with the controls above.

12e. **What a logger may and may not change (2026-09-12, evening).** Two
    settings are now the operator's alone, both after Manoj watched a
    RUMlogNG connect undo them:

    - **Sidetone pitch.** RUMlogNG sends N=4 (a legal 1000 Hz) on every
      session open. Parsed and ignored now: the pitch is the one setting
      here that only the operator hears — not timing, never on the air,
      never read back by the logger.
    - **PTT lead-in and tail.** RUMlogNG sends 0,0 (WK 0x04, and the same
      bytes in 0x0F "load defaults"), which turns off the sequencing on a
      station where the keyer is what sequences PTT. Recorded for the
      status dump, not applied: lead and tail belong to the rig and the amp,
      not to whichever logger is attached. Manoj: *"i need 50/400"*.

    Everything else a logger sets still applies for its session and is
    restored from NVS on close — speed, Farnsworth, weighting, mode register.
    To hand either of these back to the host, restore the setters named in
    the comments on `case 0x01` and `case 0x04` in `winkeyer.cpp`.

    Verified on the board: boot 50/400/600 with no host, unchanged through a
    RUMlogNG session open. **Note the side effect:** his station now runs
    with a 50 ms lead-in during logger sessions for the first time, which is
    exactly the path item 12c's backstop bug used to break — the zero lead
    is why that bug never bit him here.

12f. **DONE 2026-09-12 (evening): the fist is the operator's, the QSO is the
    host's.** Manoj's call after 12e — *"do that, keep speed and echo bits
    host controlled"*. Farnsworth, weighting, dit/dah ratio, keyer mode and
    paddle swap joined pitch and PTT timing: parsed, recorded for the status
    dump, not applied. Speed (`0x02`, `0x1C`, `0x0F` b1), the echo bits
    (`0x0E` bits 2 and 6) and the pot range stay the host's.

    Verified against a live RUMlogNG session — and the Farnsworth test is
    the one that needed care, because his NVS already held 20, so "still 20
    with a logger attached" would have proved nothing. Set to 0 through the
    operator path, then the session was closed and re-opened (a logger sends
    its defaults at session OPEN, so nothing less tests it): **farns stayed
    0**, while `wpm` still tracked the host 25→27 and `modereg` still went
    0→71 with `echo` following. His stored 20 is left at 0 — it looks like a
    leftover from the era when the host could write it.

    **`0x09` pin configuration: first left to the host, then taken back
    once it was measured.** Asked directly, Manoj said *"ptt and key, let it
    be settable from rumlog"*, so bit 0 (PTT line enable) stayed the host's.
    What changed his mind was data, below.

    **MEASURED 2026-09-12, and the decision needs revisiting: RUMlogNG
    always sends `0x00`.** Manoj reported its PTT / key-out boxes had no
    effect on the keyer. `/api/state` now carries `pincfg` (the last 0x09
    byte, -1 if never) and `hostdef` (the last load-defaults payload, hex),
    because the console cannot be read while the session that sends them
    owns the wire. What the capture shows:

    - Session open → `pincfg=0`, and bit 0 clear means **the keyer's PTT
      output is disabled for the whole session**. On this station nothing
      showed on air: the Flex does its own T/R and nothing is wired to
      GPIO32.
    - Toggling the boxes mid-session sends **nothing at all** — not one
      change in ten minutes of 1 Hz polling.
    - Toggling them and then restarting RUMlogNG still sends `0x00`. So
      those boxes drive RUMlogNG's own rig control, not the WinKeyer pin
      configuration; the logger cannot set this byte to anything else.
    - `hostdef` stayed empty: **RUMlogNG never sends load defaults (0x0F)**,
      so our field order for that command remains unverified against a real
      host. Worth checking against the K1EL datasheet before trusting it.

    Which means honouring the byte only ever cost him the PTT line, and
    "settable from the logger" was not on offer here. **DONE the same
    evening on his word — 0x09 joins the recorded-not-applied set.** The
    byte is still captured in `pincfg`, so the next logger to try this can
    be judged on evidence rather than on a comment. Verified after flashing:
    `ptt` stays true through a RUMlogNG session open, with `pincfg` showing
    the 0 that used to disable it. The other bits of that command differ between WK revisions and
    are still ignored on purpose — acting on them would silently kill the
    sidetone or the key output.

    Key output enable is not a host command at all: it follows the backend
    (`Keyer::setKeyOutEnabled()` from `applyBackend()`), idle on Flex so the
    rig is not keyed twice.

    The original analysis follows.

    Not implemented — deciding which is his call, and each one flipped
    is one more place this keyer stops behaving like a WinKeyer.

    What a logger can still change, and what it costs:

    | Host cmd | Sets | Keep host control? |
    |---|---|---|
    | `0x02` (+`0x0F` b1) | speed WPM | **yes** — loggers change speed per QSO and per F-key, and the pot already overrides it the moment it moves |
    | `0x0D` (+`0x0F` b5) | Farnsworth | **no** — RUMlogNG sets 20 every session; that is a personal sending style, and it is what made the fist sound wrong here before |
    | `0x03` (+`0x0F` b6) | weighting | **no** — fist |
    | `0x17` (+`0x0F` b10) | dit/dah ratio | **no** — fist |
    | `0x0E` bits 5:4, 3 | iambic A/B, paddle swap | **no** — that is the paddle in the operator's hand, and swap wired the wrong way is unusable |
    | `0x0E` bits 2, 6 | serial echo, paddle echo | **yes** — protocol behaviour the logger needs, not fist |
    | `0x05` (+`0x0F` b3/b4) | speed pot range | open question — it is the operator's knob, but a logger that reads pot bytes back expects its own range |

    The pattern from 12e: **anything that shapes the fist or the local
    monitor is the operator's; anything the logger needs to drive a QSO or
    read back is the host's.** Applying that rule flips Farnsworth,
    weighting, ratio, mode and swap, and leaves speed and the echo bits.
    Same mechanics as 12e — record the value for the status dump, do not
    apply it — and the same one-line way back.

12g. **DONE 2026-09-13 — probed; see 12h.** A real K1EL WinKeyer will be available to probe — Manoj, 2026-09-12:
    *"When I am back will let you probe a real winkeyer."* Every protocol
    question this firmware has had to guess at can be settled by asking the
    genuine article, so do not spend the session on whatever seems
    interesting at the time. The list, in the order the answers matter:

    1. **`0x0F` load-defaults field order** — the one known gap (12f):
       RUMlogNG never sends it, so ours has never met a real host and this
       file's mapping may simply be wrong. Best experiment: send a
       load-defaults packet of 15 *distinctive* values, then read them back
       with admin "get values" (`0x00 0x07`), which returns 15 bytes. The
       round trip names every field without needing the datasheet.
    2. **`0x09` pin configuration bits** — what each bit actually drives.
       Set them one at a time and watch the real keyer's KEY and PTT pins
       (an ESP input or a meter). We act on bit 0 only and deliberately
       ignore the rest; this says whether that is right.
    3. **Echo timing** — when a real WK emits an echoed character relative
       to sending it. Ours paces echo against the radio on the Flex path
       (item 12a's open report), and there is no reference for "correct".
    4. **Status byte semantics** — exactly which bits move, and when, across
       buffer full/empty, break-in, and PTT.
    5. **Speed pot reporting** — when it sends unsolicited pot bytes, and
       what it does at the ends of the range.

    `tools/wk-test.py --serial <port> --baud 1200` already speaks the
    protocol and is the natural harness: point it at the real keyer instead
    of ours. `tools/host-watch.py` is the other half — run it against OUR
    keyer with the same logger attached and compare what each one is told. **Capture the raw bytes both ways and commit the log**, so the
    next question does not need the hardware back.

- **2026-09-13 (after midnight)** — **audited against a genuine K1EL
  WinKeyer.** Manoj connected a WK3.1 (`usbserial-AI02BVHE`, version byte
  31, firmware 31.02) and drove its pot and paddle while six probe scripts
  logged every byte. Everything is in `docs/k1el-probe-2026-09-13/`: the
  logs, the scripts, and a write-up with each finding checked against the
  K1EL WK3 datasheet Rev 1.3. See 12h.

12h. **FIXED 2026-09-13, verified against the K1EL logs (flex backend):
    eleven places where this firmware was not a WinKeyer** — plus a twelfth,
    a status byte volunteered after the version. What still differs is
    listed at the end of this item. Measured on the real keyer, confirmed in the datasheet;
    detail and raw bytes in `docs/k1el-probe-2026-09-13/README.md`. Worst
    first:

    1. **Every status flag is one bit too high** — real XOFF 0x01, BREAKIN
       0x02, BUSY 0x04, KEYDOWN 0x08, WAIT 0x10. A logger has been reading
       our BUSY as KEYDOWN. `tools/wk-test.py` and `tools/wk-timing.py`
       decode the same wrong map, which is how self-tests passed.
    2. **`0x0F` load-defaults order wrong from byte 3** — real: mode, speed,
       sidetone, weight, lead, tail, minWPM, range, X2MODE, key comp,
       Farnsworth, setpoint, ratio, pincfg, X1MODE. Ours took the pot range
       from weight and lead-in. 12g's round-trip plan was impossible: admin 7
       Get Values is unsupported on WK3 (it sends nothing at all).
    3. Pot byte is the raw knob step above MINWPM (0..range), one byte per
       step, never produced by a host speed command. Ours scales to 0..31
       and follows `getWpm()`.
    4. Admin parameter counts that desync the parser (0x0D = 256 bytes,
       0x13 = 2, 0x14 = 0, 0x16 = 1, 0x19 = 1).
    5. Admin replies: 0x09 FW major, 0x15 Vcc, 0x17 FW minor, 0x18 IC type.
    6. Pause sets no WAIT; Clear Buffer cancels pause.
    7. BREAKIN is a level held for the whole paddle session, not a pulse.
    8. Paddle break-in clears the serial buffer and text arriving while
       paddling is ignored.
    9. XOFF at 2/3 of the buffer.
    10. Set WK2 Mode (admin 11, accepted while open) answers 0xC8 and turns
        bit 3 into the pushbutton tag; host open returns to WK1 mode.
    11. **KEYDOWN is reported for tune only**, never per element — ours sends
        about two status bytes per element at 1200 baud.

    Pot and break-in were measured with Manoj on the K1EL's controls. Pin
    configuration (12g item 2) was NOT measured — nothing was wired to the
    K1EL's outputs; the datasheet layout is in the write-up.

    **Verification:** `docs/k1el-probe-2026-09-13/run-tcp.py` replays the
    K1EL probe scripts unchanged against our keyer over TCP 8088, so the two
    logs diff directly. Probes 1-4 and 6 now agree on everything listed
    above. **Not yet verified:** items 3, 7 and 8 (pot, BREAKIN level,
    break-in clearing text) need probe 5 with an operator on OUR pot and
    paddle; and the **local** backend has not been compared at all.

    **Flex echo pacing FIXED later the same night** (`flex.cpp`): pending
    now runs to the block's last index (reply index + length - 1), counts on
    from `sentIdx` after a clear, and ignores replies to sends made before a
    `cwx clear`. PARIS now echoes per letter within ~150 ms of the K1EL, and
    the spurious BUSY after Clear Buffer / break-in is gone. Probe 5 was
    also run on ours (pot, BREAKIN level, break-in erasing the radio's
    text): all as the K1EL.

    **Local echo FIXED too:** the keyer reports buffered characters as they
    finish (`Keyer::sentRead()`), and the engine echoes a host character only
    then — PARIS within ~60 ms of the K1EL. Web-page and memory text is never
    echoed to a logger (it has no entry in `echoQ`).

    **Still OPEN:** local pause lets the keyer's 3-character look-ahead go
    out (K1EL: only the current letter); Flex pause cannot hold text the
    radio already has and XOFF never asserts (inherent to handing text
    over); pin configuration never observed on hardware. **Needs Manoj:**
    a RUMlogNG session to confirm 12a and that nothing a logger relied on
    changed with the status-bit fix. Break-in now clears the radio's
    cwx buffer too (`Flex::clear()`), as a WinKeyer clears its own — a
    change Manoj will hear on the air.

- **2026-09-13 (morning)** — **"PTT drops before TX ends" was a speed
  mismatch; fixing it exposed a protocol desync. Both fixed, verified on air
  with RUMlogNG.**
  - **Radio at 30 WPM, keyer at 25.** `tools/flex-ptt-watch.py` over three
    CQ memories: the local PTT line dropped within 40 ms of the radio's
    interlock going READY every time — it was not early. The sidetone copy
    ran on 1.4–3 s longer, because the radio's `cwx wpm` had been changed
    outside the keyer (SmartSDR's CWX control or similar) and `loop()` only
    pushed the speed when the *keyer's* speed changed. Now `Flex` records the
    radio's reported `cwx wpm=`, and `loop()` re-sends ours whenever the
    radio is idle and differs (≤ every 2 s, never mid-message). Verified: the
    radio followed the pot 14→25→24; radio READY, PTT drop and sidetone end
    within ~0.6 s of each other.
  - **Admin 0 (Calibrate) takes one byte; `adminParams` said none.** After
    the first flash the keyer showed 60 WPM and the pot moved only
    RUMlogNG's speed. `/api/wktrace`: RUMlogNG's setup began
    `00 00 02 00 0B 00 0F 00 01 08 …`, parsed as speed 0 then a bogus
    0x0F load-defaults eating 15 bytes — pot min 0x47 (the mode byte) = 71,
    range 9 → clamped 60. With the argument counted, the stream frames into
    RUMlogNG's normal set-up and the pot range is 10–40 again. It hit only
    sessions whose open began with that stray `00 00 02` — later re-opens
    (`00 02 00 0B …`) parsed fine, which is why it had not shown before.
    Not probed on the genuine K1EL.
  - **HANDOVER's wiring note was stale** (FTDI "TX/RX/GND"): the FTDI is now
    RX-only console and flashing is on the USB-C `usbserial-0001` — top of
    file corrected.
  - **`esp32s3-winkeyer` builds again.** With `ARDUINO_USB_CDC_ON_BOOT`
    `Serial` is `HWCDC`, whose `begin()` takes a baud rate but no
    `SerialConfig`, so the 1200 8N2 call in `setup()` and the `/baud`
    handler would not compile (broken since the core 3.3.11 move). Both now
    skip the framing under that flag — USB CDC has none to set. Build-only:
    no S3 board has run it, and whether a logger opening native USB at 1200
    baud behaves is untested.

- **2026-09-13 (afternoon)** — **BLE keyboard proven in a standalone probe
  on the keyer's board and core. Not merged into the keyer.**
  - **What the core supports** (checked in the installed libs, not assumed):
    Bluedroid dual-mode, WiFi/BT coexistence on, `libesp_hid.a` with the
    **BLE** HID host (`esp_ble_hidh_*`). `CONFIG_BT_HID_HOST_ENABLED` is off
    and there is no Classic transport, so **Classic-only keyboards would
    need a rebuilt core** — not worth it.
  - **Probe:** `src/probes/ble_kbd_probe.cpp`, env `ble-kbd-probe`,
    flashed with `ENV=ble-kbd-probe ./flash.sh` (`flash.sh` now takes
    `ENV=`). Scans for appearance 0x03C1 / service 0x1812, opens with
    `esp_hidh_dev_open()`, prints keys, heap, RSSI and modem-sleep state.
    Console at 115200.
  - **Result with an Amkette Optimus ("Optimus 1", vid 32c2 pid 1001):**
    paired Just-Works, 97 keys over ~8 min with **none dropped**
    (`cq test vu2cpl 5nn ?` checked character by character), Shift, digits,
    Esc, PgUp/PgDn correct, link held through ~3 min idle. F1/F6 without Fn
    send consumer Play/Pause and Win+Shift+S; with Fn they are real F1/F6.
    Logitech K220 is 2.4 GHz with a USB receiver, not Bluetooth — S3 USB
    host territory.
  - **Costs:** heap 164 → 74 KB with BT up, ~59 KB with the keyboard
    connected (min 54 KB). **WiFi modem sleep is forced on** (`ps=1`): ping
    avg 90 ms scanning / 83 ms connected+typing, max 221–280 ms, 0% loss.
    The keyboard itself adds nothing measurable; modem sleep is the cost.
  - **Traps hit:**
    - `initArduino()` frees the BT controller's memory unless
      `bleInUse()` returns true — the probe defines it; the keyer must too.
    - A failed `esp_hidh_dev_open()` (GATT 0x85) returns NULL and sends **no**
      open event; the first probe build waited forever. Now rescans.
    - The Amkette uses a **new random address each pairing** (…9d, 9e, 9f,
      a0) and each became its own bond, and it disconnects (0x13) every
      5–15 s while still in pairing mode.
    - `esp_hidh_dev_reports_get()` returns a malloc'd copy — caller frees
      (verified by disassembling `esp_hidh.c.obj`).
    - The first upload failed at esptool's verify stage after a 100% write
      and the image still booted; the second verified clean.
    - Occasional runs of `0xFF` over console lines on `usbserial-0001` —
      key counts stayed right, so it is serial-link noise, not lost input.

- **2026-09-13 (evening)** — **Bluetooth keyboard built into the keyer,
  then parked: it does not fit this board's heap.**
  - **Built:** `include/bt.h` + `src/bt.cpp` (grown from the probe). Keys are
    decoded in the HID task, queued, and acted on in `loop()` — text →
    `WinKeyer::sendText()` per character, F1–F6 → `Memories::play()`, Esc →
    abort + tune off, PgUp/PgDn/Up/Down → `Settings::apply("wpm")`.
    `esp_hidh_dev_open()` runs in its own task (`btopen`) because it blocks
    for seconds. One keyboard kept, other bonds pruned on connect; a
    passive ~2%-duty scan reconnects it while away. Web card BT KEYBOARD +
    KBD lamp, `GET/POST /api/bt`, `/bt on|off|scan|forget`, preview stubs.
    Setting `bten` (default 0) is read by `bleInUse()` inside initArduino
    (NVS is already up there), so off releases the BT memory exactly as
    before; `WiFi.setSleep()` follows the setting. S3 build = stubs (NimBLE
    core, no Bluedroid). Cost even when off: flash 39.8% → 62.6%, static RAM
    +15 KB.
  - **Failed on hardware:** enabled + restarted → no ping, no web page.
    The FTDI RX-only console (1200 8N2) showed `fillBuffer(): Not enough
    memory to allocate buffer`, `socket: 105`, `write(): fail ... errno 11`
    every second. The probe had 59 KB free, but it runs no web server, Flex
    client or MQTT; the keyer ran 126 KB free with BT off.
  - **Could not be rescued from the CLI:** `/bt off` over `usbserial-0001`
    never saved, sent once or 8×. The error flood at 1200 baud (~0.7 s per
    line) kept loop() busy printing. An RTS pulse from pyserial also did not
    reset the board; `esptool read_mac` (default_reset/hard_reset) did — run
    it with `~/.platformio/penv/bin/python -m esptool`, the 3.13 venv lacks
    `rich_click`.
  - **Recovered by a boot guard (flashed, verified):** after the stack is up,
    free heap < 60 KB → save `bten=0` and `bterr=<heap>`, restart. It
    tripped at **25,896 B**; the board came back with BT off, 130 KB free,
    Flex connected, ping 7–14 ms. The card shows "switched off at boot — only
    25 KB free with Bluetooth"; ticking enabled clears the note. **Rule: no
    setting may be able to take the keyer off the network.**

- **2026-09-13 (night)** — **3D-printable enclosure: `enclosure/`.**
  Parametric OpenSCAD (`winkeyer-case.scad`), STLs, README with renders.
  Two parts, no supports, ~145 × 89 × 45 mm. Front: OLED window, 3 mm KEY
  LED (GPIO2, parallel to the onboard LED), WPM pot. Back: PDL, K1, P1, K2,
  P2, FSK 3.5 mm jacks, 5 V DC jack (VIN — 5 V only), USB-C. Lid: piezo
  ring + sound holes, hold-down post onto the WROOM shield. Devkit (38-pin
  USB-C) sits on shelves under its short ends 20 mm off the floor for
  Dupont leads, since the pin rows run along the long edges.
  - **Fit checks live in the .scad**: stand-ins for every part and three
    `check_*` modes that must render empty. The first draft failed all
    three (paddle jack and devkit corner into screw bosses, lid lip into
    the tray's rounded inside corners, OLED post shoulders over the glass)
    — interior grew 130 → 140 mm to fix it. All empty now.
  - **Measured the same night, then committed again:** jack holes stay
    6.3 (6 mm threads); DC hole 11.2 → **8.3** (8 mm thread); pot hole 7.3
    → **7.0** (6.7 mm bushing), tab offset 8.0 with its recess widened to
    3.6 mm because the measurement was "around 8". OLED: board 35 × 33,
    glass 34 × 23, window **32 × 20** chosen by Manoj — which changed the
    mounting: the pins through the PCB holes are now OFF (`oled_posts`),
    replaced by a 0.8 mm glass pocket that aligns the window plus hot glue,
    because the hole spacing was never measured and 23 mm glass leaves no
    room for post shoulders. Window cut changed from a full-radius slot
    (which would be a stadium at 32 × 20) to a rectangle with 1.5 mm
    corners. Devkit and USB-C height confirmed; piezo 12 mm. Fit checks
    re-run after each change: all empty. **Still not printed.**
  - **Rig connectors changed to RCA, and the optos got a home.** Manoj:
    KEY/PTT never go on 3.5 mm jacks in this shack — RCA (8 mm thread, hole
    8.3). Back panel is now two rows: K1 P1 K2 P2 FSK on RCA above (16 mm
    pitch, kept left of the devkit), paddle 3.5 mm + 5 V DC below (DC moved
    x 94 → 36), USB-C unchanged. The PC817s had been "loose wiring"; they
    now sit on a 30 × 40 perfboard on four 6 mm posts (M2.5 self-tap) at the
    back-left, directly in front of the RCA bodies — 5 optos + 5 × 330 Ω,
    one per socket, RCA shells isolated from keyer GND. Stand-ins for the
    RCA bodies and the board added to the fit checks: all empty. OLED stays
    pocket + hot glue (asked, glue is fine). The earlier 3.5 mm jack
    description in this entry is superseded.

- **2026-09-16** — **LTV847 written up as a second opto option; no build
  change.** Manoj priced ready-made opto modules against the
  documented 5 × PC817 + 330 Ω board and rejected all of them: the 7Semi
  and SmartElex **ILD213T** breakouts, the **HW-399 TLP281-4** 4-channel
  board and the generic green 4-channel **PC817** module. Every one of
  them buffers the isolator's output with a transistor and a pull-up, so
  the output needs a supply on the rig side (HV / HVCC / HGND pins) and is
  a logic level, not the plain switch-to-ground a rig's KEY line wants;
  powering that from the keyer would join the grounds and throw away the
  isolation. Modifying one (cut the traces, bypass the buffer, wire the
  isolator's collector/emitter straight to the output pins) is possible but
  is more work at 1.27 mm SOIC pitch than soldering a through-hole chip.
  What survived: **LTV847** (quad, DIP-16) + **LTV817** for the fifth line,
  with **220 Ω** input resistors — ~9 mA per LED against ~6 mA now, so
  ≥4.5 mA out at the worst-case 50 % CTR versus ~3 mA today, 5000 Vrms
  isolation, every channel's emitter separate, ~₹50 for the pair. PC847 and
  EL847 substitute pin for pin. `enclosure/README.md` now carries both
  builds with the LTV847 pin table (channels 3 and 4 are mirrored: anodes
  are pins 12 and 16). **Nothing is decided and no board is built** — the
  PC817 build is still the documented default.

- **2026-09-16** — **ESP32-S3 boards ordered for the Bluetooth keyboard and
  native USB.** Manoj ordered **2 × generic "ESP32-S3 N16R8 Dual C-Type USB"**
  boards (₹798 each) and **2 × LTV-847** for the opto board. Why the S3:
  NimBLE and 8 MB PSRAM are the first route in open item 9 for the parked
  Bluetooth keyboard; native USB can carry WinKeyer to RUMlog, which ends the
  CP2102 reset problem; and USB host can take the keyboard's 2.4 GHz dongle,
  which avoids Bluetooth's WiFi ping spikes entirely. Considered and not
  chosen: the **Waveshare ESP32-S3-DEV-KIT-N16R8** (one USB-C through a
  CH334 hub, so the S3's USB is device-only and cannot host the dongle;
  63.3 × 25.4 mm) and the **ESP32-C5 WiFi 6** board (single core — the keyer
  task is pinned to core 1 in `keyer.cpp` and WiFi tasks would preempt it; no
  USB OTG; Bluetooth still shares the radio even with WiFi on 5 GHz). The
  ordered board looks like the common **YD-ESP32-S3 layout** from the listing
  photo: both USB-C sockets on one short end, RGB LED, DevKitC-1 pin order,
  probably a CH343, and solder pads by the LED (likely IN-OUT and USB-OTG —
  USB-OTG must be bridged to power the dongle). PCB antenna, so no WiFi gain.
  **The listing says 57 × 28 mm but the photo implies about 65–70 mm long** —
  measure before touching `enclosure/`.

- **2026-09-17** — **Bug found, not fixed: on the Flex backend, CW keying
  dies after every paddle key.** No firmware change. It was found while
  chasing a radio-side problem ("CW is gone") from the Mac. Recorded read-only
  with the radio's FWDPWR meter, interlock and CWX status, and a capture of
  RUMlogNG's own radio session. The capture showed RUMlogNG sends no keying,
  so the paddle keying and the memories reach the radio through this keyer.
  Full data and next steps: open item 13.

- **2026-09-17 (later)** — **`/api/state` now reports `flex.guihandle`**,
  the GUI client handle the keyer sends every `cw key` under (new accessor
  `Flex::guiClientHandle()`). It makes step 1 of open item 13 a single
  look, not a console read, which matters because the console is muted
  while RUMlogNG holds the port. Both envs build; the preview stub carries
  the field. Flashed and seen on hardware the same day: `""` at 5 s
  uptime, then the Maestro's handle by 13 s, once the keyer had bound.

- **2026-09-17 (13:58)** — **`GET /api/flextrace`: the keyer↔radio wire,
  over HTTP.** It keeps a 128-line ring of every command sent (all
  radio-bound writes now go through one `txf()` in `flex.cpp`) and of the
  radio's replies, messages and interlock/cwx/client statuses. It is there
  for open item 13: the radio's reply to a `cw key` had only ever reached
  the console, and the Mac cannot see this traffic at all. About 16 KB of
  static RAM (RAM 29.9%). Both envs build. Flashed, and read on hardware
  straight after boot.

- **2026-09-17 (14:40)** — **Fixed: Flex CW at 0 W after a GUI client
  restart (open item 13).** `flex.cpp` now follows the GUI client instead of
  capturing it once per radio session. On `client <h> disconnected` for the
  client in use, it forgets the handle and re-sends `sub client all`. On
  `client <h> connected` it adopts a GUI client if it has none, and takes a
  new handle when the same `client_id` comes back. The state word is matched
  exactly; `indexOf("connected")` also matched "disconnected". `guiHandle`
  is also cleared after the reconnect key-up and in `setBind()`.
  **`flexbind` now defaults to off** (`cfgBind` and the NVS default), and the
  comments claiming the radio refuses CWX from an unbound client are
  corrected: measured, it accepts both `cwx send` and `cw key`. Both envs
  build. Flashed and verified: AetherSDR restarted with no command, the
  keyer followed to `0x105F4906`, then a paddle key first (13 `cw key`, 4.47
  W) and a memory (`cwx sent=1440…1444`, 4.47 W). **Maestro handover
  verified 14:43–14:44.** The Maestro connected (`0x7157AD38`) while
  AetherSDR was still up, and the keyer stayed on AetherSDR. AetherSDR then
  left, and the keyer moved to the Maestro by itself. Paddle keys at
  14:44:02/04 and memories after that were fine (Manoj). That covers the
  second-GUI-client case. **Side observation, not a fault in the fix:** the
  first memory after the switch (`cwx send qrl?`, 14:44:20) was cut short
  because the keyer sent `cwx clear` **233 ms** after the send. The radio
  had sent one character (`cwx erase=1446,1448`, then `cwx sent=1445`). No
  logger or TCP host was attached, so the clear came from the web STOP
  (the PLAY button turns into STOP when clicked, so a double click stops it)
  or a paddle break-in. The replays 4.8 s and 26 s later ran in full. Not
  yet exercised: the new NVS default on a keyer that never saved `flexbind`
  (Manoj's board had it saved as off).

- **2026-09-17 (14:55)** — **Each `cwx clear` now records its cause in
  `/api/flextrace`.** Prompted by the first memory after the Maestro
  takeover being cut short 233 ms in by a keyer-sent `cwx clear`. Manoj did
  not double-click or touch the paddle, and no host was attached. With the
  stuck-key safety releases ruled out (both send a key-up first, and the
  record has none), the web STOP and a paddle break-in looked like the
  only candidates. **Manoj's corrections:** RFI at 10 W is not credible
  here; no Bluetooth keyboard is connected; and both STOP and break-in
  would also have cut the local sidetone (`monReset()`). **If the sidetone
  played the whole message, neither of them sent it.** The only path left
  that clears the radio without a key-up and without touching the sidetone
  is `pending()`'s "no progress from radio" backstop in `flex.cpp`. On
  reading, that backstop should not be able to fire 233 ms after a
  `cwx send`, so if it did, that is a bug. **Manoj confirmed the sidetone
  played the full message.** Reading the code with that:
  - The web STOP calls `Keyer::clearBuffer()`, which would have cut the
    sidetone, so it is excluded.
  - A paddle *contact* would also have flushed the keyer's queue on its
    edge (`keyer.cpp`, "Paddle break-in"), so a contact glitch is excluded.
  - The two stuck-key safety releases are excluded (no key-up in the
    record), and so is the "no progress" backstop (`send()` stamps
    `lastCwxMs`, so it cannot fire for 5 s).
  - Timing: the monitor copy is queued with one due time, now + the
    measured start latency (~230 ms), and `monPump()` hands every character
    to the keyer at once when it falls due. The clear came 233 ms after the
    send, about 3 ms after that. At that point `monReset()` clears nothing,
    so a break-in there would not cut the sidetone.
  - So the best fit is a **paddle session opened by the keyer's own element
    state** (`!curIsAuto && state == ST_KEYDOWN/ST_GAP`, cause `element`),
    not by a contact, as the monitor copy starts after a paddle element
    left `curIsAuto` false. The code path that would do it has not been
    found. Paddle-then-memory played fine at 14:41, so it is intermittent.
    The new `# clear:` line will say `paddle break-in (element)` if this is
    right.
  - **Repro attempt, 14:56–14:59, dummy load: 0 of 9.** Five cycles of a
    Tune pulse (a keyed element not from a message; the trace confirmed
    `xmit 1` + `cw key 1/0`), 15 s idle, then memory 5 (`qrl?`), plus four
    memory-only cycles. Every run gave 1 `cwx send`, 4 `cwx sent=`, no
    erase and no clear. That weakens the stale-`curIsAuto` theory. The
    condition not yet reproduced is the one present at 14:44: the first
    memory after a **GUI client change** (Maestro takeover), with real
    paddle keys in between. Script and log:
    `~/projects/MSHV-Mac/user-reports/2026-09-17-cw-gone/repro.py`, `repro.log`.
  `Flex::clear(why)` and `WinKeyer::abort(why)` now take a reason, written
  as a `#` line: `web STOP`, `paddle break-in (dit|dah|element)` (from the new
  `Keyer::paddleSessionCause()`), `host 0x0A clear buffer`,
  `Bluetooth keyboard Esc`, and the three backstops in `flex.cpp`. Both envs
  build. Flashed; a web STOP read back as `# clear: web STOP`. **Next time a
  memory is cut short, read the trace before anything else.** If it says
  paddle break-in with nobody on the paddle, look at the paddle input. If
  it says no progress from radio, look at the `pending()` backstop.

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
  discovery: **Find radio** on the web page scans a /24 over TCP, or pin
  the address with `/flex ip <addr>`.
- **MQTT connects (fixed 2026-09-12).** It had two faults at once: no
  `MQTT_HOST` in `secrets.h`, so the build used the public placeholder,
  and the example password. Reachability across segments was never the
  problem.
- Same-subnet placement did **not** fix latency; see the link-quality
  section below.

**The WinKeyer TCP port is unauthenticated** — anyone who can reach port
8088 can key the transmitter. That is an argument for a trusted LAN, and
against exposing it beyond one.

## Open items

1. ~~Set the real MQTT password~~ **DONE 2026-09-12** — `[MQTT] connected`
   as `iot`. `secrets.h` also needed **MQTT_HOST**, which was missing, so
   the build had been using the public placeholder `192.168.1.10` all
   along; that dead address is what blocked `loop()` once a minute. The
   broker ACL needed `topic write shack/esp32-winkeyer/#` under `iot`.
   **Not yet confirmed:** that the published topic actually arrives — read
   it in Node-RED or MQTT Explorer.
2. **WiFi link is mediocre but no longer limiting** — 131 ms average,
   0% loss, RSSI -68. Improve when convenient (closer AP, different
   channel, external-antenna board); not a blocker.
3. **RUMlogNG drives it over USB and keys the radio** (2026-09-10) — the
   last compatibility unknown, now closed. It took two fixes: the serial
   link had to move to **1200 baud 8N2** (a real WinKeyer's rate, which
   loggers open without asking — the firmware was at 115200 and every
   handshake arrived as noise), and the console had to stop sharing the
   wire (see below). Handshake now answers version 23 in 0.0 s with zero
   garbage. Still unverified: character **echo**, which is host-controlled
   via mode-register bit 2 — check `echo`/`modereg` in `/api/state` to see
   whether RUMlogNG asks for it at all, and note that on the Flex path echo
   fired when characters were queued rather than as each was sent — fixed
   2026-09-10 by pacing echo against the radio's `cwx sent=` reports, and
   verified: `TEST DE VU2CPL` at 20 WPM echoes over 5.5 s with per-character
   gaps matching Morse durations. RUMlogNG sets mode register `0x07`, so it
   does request echo.
4. **On-air timing check** — testing so far is functional, not
   calibrated. Verify element timing against a scope or a known-good
   decoder.
5. **Flex network keying works** (2026-09-10) and persists across
   reboots. **PTT tail is 400 ms**, now a real persisted setting.

   Correction to an earlier claim in this file: a note said Manoj had
   compared 400 ms against 250 ms by ear and chosen 400. That was wrong —
   at the time, `/tail` could not affect the Flex path at all (see the
   two-tails bug below), so every value he tried was still the hardcoded
   400. The 400 ms figure is the original blind guess, now confirmed only
   as "sounds OK", never A/B'd against anything.

   **Worth actually A/B-ing now that the control works.** Measured on the
   wire: 150→157 ms, 250→255 ms, 400→406 ms from the last `cw key 0` to
   `xmit 0`.

   Still to do: confirm on-air fist quality with a decoder. `logKeying` in
   `flex.cpp` prints every edge; turn it off once happy.
6. **Pin config command (WK 0x09)** — recorded, not applied (12f). The
   WK2/WK3 datasheet layout is now known: bit 0 PTT enable, bit 1 sidetone,
   bit 2 KeyOut 2, bit 3 KeyOut 1, bits 5-4 paddle hang time, 7-6 ultimatic
   priority. Not yet observed on real hardware (12h).
7. Hardware build: paddle/key/PTT interface (PC817 + 330 Ω, or the LTV847
   + 220 Ω option added 2026-09-16 — 2 × LTV-847 ordered the same day, board
   not built), enclosure.
   The speed pot and the OLED are **wired and working** (2026-09-10);
   what remains is the opto-isolated key/PTT interface and the box.
   **Box designed and measured 2026-09-13 (`enclosure/`), NOT YET PRINTED.**
   KEY/PTT/FSK are 5 × RCA with the optos on a 30 × 40 perfboard inside —
   5 × PC817 + 330 Ω as documented, or 1 × LTV847 + 1 × LTV817 + 220 Ω
   (see `enclosure/README.md`); ready-made opto modules were evaluated and
   rejected, all of them need a supply on the rig side.
   Every vendor-variable part was measured or confirmed by Manoj: paddle
   jack 6 mm, RCA 8 mm, DC jack 8 mm, pot bushing 6.7 mm with its tab ~8 mm off the shaft, OLED
   board 35 × 33 with 34 × 23 glass (window chosen 32 × 20), devkit and its
   USB-C height as modelled, piezo 12 mm. Fit checks all empty. Next: print,
   ideally the back wall as a test strip first, and report what did not fit.
   The OLED is held by a glass pocket + hot glue — its hole spacing was never
   measured, so `oled_posts` stays off.
8. **Repo is PUBLIC** since 2026-09-11 — github.com/vu2cpl/esp32-winkeyer.
   Manoj's friend can clone it directly; no invite needed.

   **Before publishing, the git history was rewritten** to scrub real shack
   addresses: the broker, radio, keyer and Mac IPs, the segment map and the
   SSID appeared in both file contents and two commit messages. Sanitising
   the working tree is NOT enough — publishing a repo publishes every
   commit. Two `git filter-branch` passes were needed (`--tree-filter` for
   contents, `--msg-filter` for messages), verified against a *fresh clone
   of the remote*, which is the only check that reflects what the public
   sees. `git log --all` is misleading here: it includes `refs/original`,
   filter-branch's local backup, and will keep reporting the old history
   forever. Pre-rewrite commit was `2a7f7df`.

   **Keep it sanitised.** Placeholders now in use: broker `192.168.1.10`,
   radio `192.168.1.50`, keyer `192.168.10.20`, Mac `192.168.10.30`, SSID
   `<your-ssid>`. Never commit the real ones again — put local values in
   `include/secrets.h`, which is git-ignored and overrides `config.h`.

   The setup-AP password `vu2cpl1234` was deliberately left as-is: it is
   already public in esp8266-gps-ntp and vu2cpl-as3935-bridge, so changing
   it here alone would achieve nothing and break a shack-wide convention.
9. **Recently resolved:** the display is no longer "considered but not
   built" — `src/display.cpp` implements it for SH1106/SSD1306 on I²C
   21/22 (see 7a for the bench test that still owes).

   **Bluetooth keyboard — built (`src/bt.cpp`), off by default, PARKED
   2026-09-13: does not fit on the classic ESP32.** The code is complete
   and the page/API/CLI are verified against the preview stub, but with the
   whole keyer running Bluedroid leaves **25,896 bytes** free and lwIP
   starves (see What changed, evening). A boot guard switches it back off.
   Never verified on hardware past that point: pairing from the card, key
   actions in the keyer, background reconnect, bond pruning.

   Routes forward, in order of promise:
   - **ESP32-S3 + NimBLE.** The S3 core ships NimBLE (much lighter than
     Bluedroid) and `esp_hidh_nimble.h`; `bt.cpp` compiles to stubs there
     today and would need a NimBLE transport (GAP scan/security are
     different APIs; the key/queue/UI half carries over). The S3 also has
     USB host for the K220's 2.4 GHz receiver. No S3 board has run the
     keyer yet. **Two S3 N16R8 dual-USB-C boards ordered 2026-09-16** (see
     What changed).
   - **PSRAM board (ESP32-WROVER).** Unverified whether this precompiled
     core lets Bluedroid/lwIP allocate from SPIRAM — check sdkconfig first.
   - **Freeing ~35 KB on esp32dev.** Unlikely without dropping features.

   Still true if it is revived: Bluetooth forces WiFi modem sleep on (~85 ms
   avg ping, spikes 220–280 ms) — listen to paddle keying via the Flex
   before relying on it. F-keys need Fn on the Amkette (without it F1 is
   consumer Play/Pause, F6 is Win+Shift+S).

   Also not built, now that a display exists to make them worth having:
   a **command button** on one of the input-only spares (35/36/39) for
   menu/message playback, and showing **decoded sent text** on the panel.
10. **ESP32-S3 port — boards ordered 2026-09-16, not yet arrived.** On
    arrival: check the shield says WROOM-1-N16R8; photograph both sides
    (identify the USB-serial chip, read the solder pads); `esptool flash_id`
    and the PSRAM size at boot; measure length and both USB-C positions.
    Port work: full pin remap (no GPIO 32/33/34 on the S3; speed pot to
    ADC1 = GPIO 1–10; avoid 35–37 octal PSRAM, 19/20 native USB, 0/3/45/46
    strapping), the `esp32s3-winkeyer` env's native-USB CDC as the WinKeyer
    port, a NimBLE transport for `bt.cpp` or USB host for the dongle, and the
    enclosure resized (currently modelled on the esp32dev 55.3 × 28.3 with one
    USB-C cutout). **OTRSP/SO2R is not
    planned for this box** and its pin reservation has been dropped — SO2R
    stays in `~/projects/SO2R box`.
11. **RTTY FSK on GPIO27** (2026-09-11): Baudot/ITA2, 45.45 baud, 1.5 stop
    bits, LTRS/FIGS shift tracking, diddle, invertible polarity. Timing
    verified against theory; **polarity and on-air copy are unverified** —
    wrong `invert` prints reversed-case gibberish rather than silence.
    Not driven by any logger yet: text comes from `/fsk`, the web page or
    the API, so hooking RUMlogNG's RTTY output to it is the open question.
11w. **Superseded 2026-09-12 for the September-12 resets** — those were the
    USB chip holding EN low (see the What-changed entry), not power: the
    board had an external supply, 3.3 V measured good, and the four-state
    DTR/RTS test reproduced the fault on demand. The 2026-09-11 brownouts
    below were real (the chip's own detector reported them) and the
    capacitor advice still stands for USB-only operation, but do not reach
    for it first when a board "dies mid-over" — check what owns the serial
    port. Original note follows.

    **The resets were BROWNOUTS, and they are intermittent** (2026-09-11).
    `last reset: BROWNOUT (power)` was reported by the chip's own detector,
    so the diagnosis is not in doubt — but the trigger is. It was first
    read as deterministic: paddle keying at full WiFi power died, reduced
    power survived, an external supply survived, one cable was worse than
    another. Then the same board and cable ran minutes of heavy paddling at
    full power on USB with no reset. **Do not trust the earlier table.** It
    was built on too few runs, and I told Manoj the capacitor had become
    optional on the strength of it; that was wrong.

    Mechanism: paddle keying on the Flex backend sends one TCP packet per
    key EDGE — about twenty WiFi transmit bursts a second — and the rail
    sags through them when the margin is thin. The margin varies with
    contact resistance, other load on the USB bus, cable seating.

    **Fix: 470–1000 µF across 3V3/GND at the board**, precisely because it
    works without knowing which factor is marginal. `/txpower` is now a
    persisted runtime setting (2–19 dBm) as a workaround and a diagnostic
    lever. Still worth doing in firmware: coalesce key edges into fewer TCP
    writes, which would cut the burst rate at the source.

    The earlier cable swap and ESP32 swap were most likely both chasing
    this, which is why neither gave a clean answer.

11x. **RESOLVED 2026-09-12: the cause was the USB port's control lines.**
    `RTS` asserted with `DTR` deasserted holds EN low; every other
    combination change resets the board. Measured across all four states,
    with the chip silent (no ROM banner) while held. A logger holding the
    port therefore stops the keyer mid-over and leaves the radio keyed.
    Workaround in use: a second USB-serial adapter on TX/RX/GND only.
    Permanent fix, not done: lift the collector of whichever `J3Y`
    transistor reaches EN, ideally onto a jumper so auto-flash can be
    restored. Original note follows.

    **HARDWARE: the board was swapped, and the cause is still unproven.**
    The original ESP32 began spontaneously restarting, always reporting
    `power-on` — never a panic, never a watchdog. Software cannot cause a
    power-on reset, so it is a supply or connection fault. A USB cable
    change and then a **new ESP32** were tried in quick succession, so if
    the resets are gone we do not know which fixed it. **Ask whether they
    have recurred.** `uptime` in `/api/state` makes an unwitnessed restart
    obvious.

    Suspect, in order: the USB cable (one tried was charge-only and would
    not enumerate at all), the devkit's 3V3 regulator under WiFi current
    spikes, a breadboard short, RF ingress on the USB lead during TX.

    **Update 2026-09-11 (day): the ORIGINAL board is back in service**
    after an erase fixed its boot loop (see What changed), deliberately
    in the configuration that used to fail: **USB power only, WiFi at
    full 19 dBm, the original cable**. Through ~10 min of memory plays,
    paddling and scans it showed no unexplained reset — every restart
    matched a flash. RSSI −77. Too short to clear it; keep watching
    `uptime` and `resetreason`.

    **Recurred 2026-09-12:** while paddling on the Flex backend the board
    went into a reset loop, and earlier the same session dropped off the
    network mid-over. Both times the radio was left transmitting. Manoj
    also saw the new keying LED flash once then go dark — the reset, not
    the LED. **Not a supply sag: the board runs from an external supply
    plus USB to the Mac.** The loop was 29 `rst:` lines in ~2 KB of output
    — ~70 bytes each, barely one ROM banner line, so the chip was being
    reset before it could boot. That points at something driving **EN**
    (an EN reset also reports POWERON): RF on EN / the USB lead while
    transmitting, or the CP2102's DTR/RTS auto-reset being toggled. The
    single `POWERON` actually read was most likely caused by opening the
    port to read it. Next: log the ROM `rst:` lines with DTR/RTS held off
    through a paddle session until it happens again.

    **Caught again 00:53:21 (USB + external supply, 0 W so no RF, PTT
    output wired to nothing):** mid-over, radio left TX. Serial with
    DTR/RTS held off: 33 resets in 6 s, every one cut off right after the
    ROM printed `ets Jul 29 2019` / `rst:` — before the reason, before any
    firmware runs. So: **EN or the chip's supply, not software.** Loop
    lasted ~30 s, then it recovered alone. The macOS USB log shows the
    CP2102 stayed enumerated throughout — no USB dropout. External power
    only: 15 min / 12 overs clean (thin — the USB crashes came after 5+
    min of sending).

    **01:19–01:21:** RUMlogNG held the port 01:19:08→01:20:40 (found by
    an `lsof` poller); the keyer went silent 4 s after RUMlogNG closed it,
    then Manoj unplugged USB and it was still not answering at 01:22 on
    external power. A port close changes DTR/RTS, which reach EN through
    the auto-reset circuit, so a logger closing the port can reset the
    board. That alone does not explain staying down, nor the earlier
    crashes, which had no known opener (the poller only started 00:55).

    **Still unknown, ask Manoj:** where the external supply connects
    (5V/VIN or 3V3) and what it is; what else is wired to the keyer.
    Voiced, not done: a 1–10 µF cap EN→GND, the standard fix for devkits
    that reset on EN noise; a long external-only run; watching for any
    process that opens the port while it happens. Tools used (scratch,
    not in repo): an HTTP uptime watcher that reports restarts with their
    reset reason, an `lsof` port-opener poller, and a pyserial capture
    with `dtr=rts=False` set before `open()`.

11u. **RESOLVED 2026-09-12 — it was both suspects at once.** The once-a-
    minute stall was `mqttConnect()` on a dead placeholder address (capped
    at 500 ms now), and the long ones were `Flex::tryConnect()`'s unbounded
    `tcp.connect` (capped at 1500 ms), which could park loop() long enough
    for the 30 s task watchdog to reset the board. Original note below.

    **OPEN (was): the web server stalls for 1–2 s at regular intervals** on the
    old board (2026-09-11): `/api/state` timed out at :03 past the minute
    for several minutes running, and roughly every 10 s just after boot.
    That is loop() blocked, and loop() is also what sends the radio
    `xmit 0` — a stall during keying would read as a late PTT release.
    Suspect a blocking reconnect (MQTT, whose password is still the
    placeholder, or `Flex::tryConnect()`'s blocking `tcp.connect`).
    Unconfirmed. Also noticed: two elements share `id="flexip"` on the
    page (the Radio IP input and an unused span in the Keying row) —
    harmless today because the input comes first.

11v. **Find radio defaults to the keyer's own /24**, which is exactly the
    subnet a routed radio is not on. Manoj typed `192.168.1` and it worked.
    Voiced, parked at his request: scan own + 192.168.0 + 192.168.1 when
    blank, or remember the last subnet that found a radio.

11y. **MOSTLY FIXED 2026-09-12, one recurrence unexplained.** Cause found:
    `pending()` stays above zero after a memory because the `cwx send`
    reply indexes the block's FIRST character, so the line was held until
    the keyer's 10 s backstop dropped it. The radio's own interlock now
    ends the message (not transmitting + no progress for 1 s ⇒ counters
    cleared). **But `[KEYER] PTT was stuck with no keying` printed once
    more at 12:13:46 after that fix**, on a web-page memory, so something
    can still hold it. **2026-09-12 (evening): that print was probably not
    a stuck line at all** — see item 12c. The backstop was timing its
    window from the last element keyed, so it dropped PTT (and printed)
    one millisecond after the line came up whenever the previous element
    was more than 10 s old. Re-test before chasing this further. Next time it appears, capture `/api/state` at 5 Hz
    across the whole over and compare `ptton` against the radio's
    interlock. Only the local line is affected; nothing is wired to it on
    Manoj's board. Original note follows.

    **OPEN (was): the local PTT line releases far too late on the Flex backend.**
    Observed releasing ~20 s after a transmission against a 250 ms tail.
    The radio's own `xmit` released correctly; only GPIO32 hung on. That
    timing matches the **10 s safety backstop** firing rather than the
    normal path, which would mean the primary release is still broken and
    the net is covering for it. **To confirm: reproduce with RUMlogNG
    closed and watch for** `PTT was stuck with no keying — forced off by
    the safety backstop` **on the console.** If that line appears, fix the
    release rather than the symptom.

    Related and unverified: **paddle keying on the Flex backend**. That is
    the path that actually uses our `xmit` (buffered text goes via
    `cwx send` and the radio keys itself), so it is the likely source of
    the original stuck-PTT report and it has never been tested.

    **New evidence 2026-09-11: for memories the line barely comes on at
    all.** Logged at 5 Hz through 7 memory plays, `ptton` was true only
    for a moment at the start of each, while the radio transmitted for up
    to 11 s. The line follows `Flex::pending()` = queuedIdx − sentIdx, so
    pending is collapsing to zero almost at once. Guess, unverified:
    the `cwx send` reply carries the buffer index of the block's FIRST
    character, not its last, so the first `cwx sent=` catches up with it.
    Checking needs a real `cwx send` (it transmits). Matters only with an
    amp or sequencer on GPIO32.

    **2026-09-12: two stuck-PTT mechanisms found, neither proven to be
    Manoj's intermittent report.** (1) A 60 s MQTT retry blocking `loop()`
    during the PTT tail — fixed, see What changed. A 30-min paddle capture
    before the fix showed every over releasing correctly (radio READY,
    local line ~0.3 s later), so it is a timing-window bug, not every-over.
    (2) **The keyer dying mid-over** (brownout, 11w) leaves the radio in
    TX with no time-out. Voiced, not done: set a TX time-out in the Flex's
    interlock settings — the only backstop that survives a dead keyer.

11z. **NOT SEEN SINCE 2026-09-11.** The OLED has come up and run on every
    boot through a day of flashing on 2026-09-12, including on Arduino core
    3.3.11. The pull-ups were there all along (see the correction below),
    so the original diagnosis was wrong and the hang may have been one of
    the faults since fixed — the lwIP crash presents as a frozen board too.
    Treat as dormant, not proven cured. Original note follows.

    **OPEN AND ACTIVE (was): the display hangs the board.** Confirmed
    2026-09-11 — with the OLED enabled the board hangs during display
    init and never reaches the web server or the host link; with it
    disabled it boots and runs. **An overnight soak is running with the
    display off** to confirm nothing else contributes. First thing to do
    next session: ask how that soak went.

    Evidence: the boot log stops at the display init line every time, one
    boot, no panic, no reset loop. An I²C transaction only blocks forever
    when SDA or SCL is held low. Detection (one byte) succeeds while
    rendering (1 KB frames) fails; the 20x4 LCD, whose frames are ~80
    bytes, was reliable on the same wiring.

    **Correction 2026-09-12: 4.7 kΩ pull-ups on SDA/SCL were fitted long
    ago** — this item wrongly said they were untried. Missing pull-ups are
    therefore ruled out. Not yet examined: whether two tasks touch `Wire`
    at once (the display task on core 0 against probe/`/i2c` elsewhere),
    which can deadlock the ESP32 I²C driver silently. The display came up
    and ran on every boot on 2026-09-12, so the hang may not be current.

    **`Wire.setTimeOut(50)` did NOT prevent it** — U8g2 does not appear to
    go through the path that timeout covers. Do not mistake that for a
    guard. What does work is `/disp off`, which now skips the probe and
    init entirely so the bus is untouched from power-up.

    A great deal of firmware was flashed at this before the cause was
    clear, including a spell running the bus at 100 kHz. That treated the
    symptom; the rate is back at 400 kHz with `/disp slow` available.

11a. **Display: four panel types, family auto-detected** (2026-09-11).
    OLEDs answer at 0x3C/0x3D and HD44780 backpacks at 0x27/0x3F, so one
    firmware runs whichever is plugged in and `/disp auto` re-probes after
    a swap — no reflash. Geometry within a family is NOT detectable
    (SH1106 vs SSD1306, 16x2 vs 20x4 each share an address), so those stay
    settings that fail visibly.

    **HD44780 LCDs want 5V and are unreadable on 3V3** — faint at any
    contrast setting, which presents as a firmware fault. Contrast is the
    analogue Vo pin; no driver can fix it. Powering from VIN puts the
    backpack pull-ups on 5V, which ESP32 GPIOs do not tolerate, so the
    pull-ups must move to 3V3 or a level shifter goes in. Documented in
    README; the OLEDs are native 3.3V and unaffected.

12. **Done 2026-09-11:** second KEY/PTT pair on 18/19 with `/radio`, and
    six message memories with `%C` callsign expansion. Still wanted:
    front-panel buttons (13/14/23 have internal pull-ups), and LCD
    support alongside the OLED.

    **Pins left: 13, 14, 16, 17, 23** plus 35/36/39 input-only.

    Two traps found doing this, both worth remembering:
    - **`Keyer::sendChar()` is not how you send text.** The monitor
      feature withholds buffered elements from the key hook, so on the
      Flex backend a direct send makes sidetone and no RF. The web SEND
      box was silently broken this way until memories needed the same
      path. Everything now goes through `WinKeyer::sendText()`.
    - **A failing `nvs_open` takes ~630 ms.** Memories were read from NVS
      on every `/api/state`, which is polled once a second: seven opens
      of a namespace that did not exist yet made the endpoint take four
      seconds and the web server stopped responding entirely. Memories
      are cached in RAM and written through; the namespace is created
      read-write at boot.

13. **RESOLVED 2026-09-17 (14:40) — Flex backend: CW keying dies after
    every paddle key.** Fixed by following the GUI client and not binding;
    see What changed, 14:40. The investigation below is kept as it ran,
    including leads that turned out wrong. A Maestro takeover, with a second
    GUI client present when the first left, was verified at 14:44.

    **Measured.** 40 transmissions, 13:15–13:27 IST, read-only from the Mac:
    `flex_status_lines.py` and `flex_meters_watch.py` from
    `~/projects/MSHV-Mac/tools/`, plus a packet capture. FlexRadio Maestro was
    the only GUI client; slice in CW; RF power 10; the radio had been factory
    reset that morning and was not restarted during the test.

    | what Manoj did | interlock `source=` | forward power | count |
    |---|---|---|---|
    | paddle key | `SW` (PTT with **no CW element**) | **0.00 W** | 24 of 24 |
    | first memory after a paddle key | `SWCW` | **0.00 W** | **5 of 5** |
    | memory after a memory | `SWCW` | 9.46–9.75 W | 10 of 10 |

    In a dead memory, **the radio's CWX queue never advances.** No
    `cwx sent=` arrives until the operator presses stop; then `cwx erase=a,b`
    clears the rest and only then is the first index reported sent. A good
    memory reports its first `cwx sent=` about 0.7 s after keying. Stop and
    replay recovers it, and no radio restart is needed. Manoj's summary:
    *any time the paddle is used the keying goes; the first memory gives no
    output, the next onwards are fine; the next paddle use kills it again.*
    On the first memory he hears one character (local sidetone) then
    nothing.

    **Established.**
    - RUMlogNG, on `usbserial-0001` and the only program on the Mac connected
      to the radio, sent **only `cwx clear` + `xmit 0` pairs** during a
      3-minute capture: 7 pairs, for Manoj's stops and his paddle
      break-ins. It sent **no keying**.
    - So paddle keying and memory text reach the radio **through this
      keyer's own Flex session**. Every `cwx sent=` carried the Maestro's
      handle.
    - A client that is not on the Mac (`0x3CF2DCF6`) erased the CWX queue
      at every paddle break-in. That is almost certainly this keyer; break-in
      erasing the radio's remaining text is documented in
      `docs/k1el-probe-2026-09-13/`.

    **Lead to test first (from reading the code, NOT proven).**
    `src/flex.cpp:308` binds to the **first GUI client it sees** and keeps
    that client's handle in `guiHandle` for every
    `cw key … client_handle=`. That is only re-evaluated when the keyer's
    radio TCP session reconnects (`flex.cpp:385`) or `setBind()` runs.
    The comment at `flex.cpp:318` says that with the wrong handle *the radio
    accepts cw key but produces no RF*, which is exactly the paddle
    symptom: `xmit 1` still keys PTT, and the elements go nowhere. On 09-17
    the GUI client changed under a live radio: AetherSDR until 11:52:19, the
    Maestro from 11:52:06. **Not reconciled yet:** CW "came back" around noon
    and failed again at 13:15 with the Maestro still the GUI client, so a
    stale handle may not be the whole story. **Also possible:** this keyer
    sends `cw key` under the Maestro's handle with its own `index` counter
    while the Maestro keeps its own for the same handle, and the radio may
    drop out-of-sequence indices. The CWX freeze after a paddle key is also
    unexplained; see `sendKeyUp()`: *"xmit 0" does NOT clear a key the
    radio still believes is down*.

    **Stale handle ruled out (later on 09-17).** After a reboot the keyer
    and the radio agreed on the handle: `flex.guihandle` was `0x7E7FD26E`
    and `sub client all` listed only the Maestro, on the same handle.
    Manoj then used the paddle and it was **still dead**. A second check
    after that test still matched. The keyer had not reset (uptime 121 s),
    the Flex session was connected, the slice was in CW and there was no
    slice warning. So a wrong `client_handle` does not explain the paddle
    fault. Steps 1, 3 and 5 below are answered, and step 2 (the exact
    `cw key` lines and their `index`) is next.

    **Then it recovered on its own (13:39–13:42, same logs).** After the
    reboot every transmission was 0 W until two memories had been played.
    Paddle keys at 13:40:43–13:40:53 showed `source=SW > SW,SWCW`, so the
    `cw key` elements DID reach the radio, and still made 0 W. This morning
    the dead paddle keys showed `SW` only. The first memory (13:41:29,
    `SWCW`) was 0 W, the second (13:41:32) was 10.2 W. From then on every
    paddle key (`SW > SW,SWCW`) and memory made ~10.3 W, and paddle use no
    longer killed the next memory. Manoj confirmed both by ear, and Tune
    from the Maestro worked at 13:41:51 (after the recovery, so step 4 is
    still open for the stuck state). Elements accepted but no RF points at
    the radio's CW/TX chain needing the CWX path to start it, not at
    dropped commands. **Untested:** whether a fresh keyer session (reboot
    or Flex reconnect) brings the dead state back, which would reproduce it
    on demand.

    **Stale handle: NOT the cause after all (13:46–13:51).** At 13:46,
    after Manoj switched the GUI client from the Maestro to AetherSDR, the
    paddle was dead and the keyer still held the Maestro's handle. That
    looked like confirmation, but it was only correlation:
    - A forced rebind (`POST /api/set?k=flexbind&v=on`) picked up
      AetherSDR's handle (`0x0D528C43`, matching `sub client all`, slice 0
      CW and TX), and the paddle **stayed dead**.
    - Manoj then restarted AetherSDR, which got a new handle
      (`0x54A57F28`); the keyer kept `0x0D528C43`. The paddle was dead.
      He played a memory and stopped it, and **the paddle came back with
      the keyer still holding the stale handle** (checked 13:50:46).

    So the handle in `cw key` does not decide whether the paddle makes
    RF. What does: after any fresh start (keyer reboot, keyer Flex
    reconnect, GUI client change or restart), paddle keying makes 0 W
    until a memory has been played. The radio still marks those paddle
    transmissions as CW (`SW,SWCW`, 13:40). The first memory can itself
    stall at 0 W; stopping and replaying works. Note: `client bind` uses
    the `client_id`, which stays the same across AetherSDR restarts
    (`9BC74D63-…`), while the handle changes each time. Unconfirmed: whether
    Manoj's STOP (`cwx clear` alone) was tried before the memory, which
    would show that the fix needs a `cwx send`. The stale handle is still
    wrong and worth refreshing, but fixing it alone will not fix this.

    **Recovery is not reliable either (Manoj, 13:53).** STOP (`cwx clear`)
    does bring the paddle back sometimes, but it often takes several
    presses. So neither STOP nor a memory is a guaranteed reset, and the
    fault may depend on timing. What the radio replies to each `cw key`
    (`R<seq>|<code>|`) has never been looked at, and the Mac cannot see
    that traffic (see above).

    **`GET /api/flextrace` added and flashed (13:58)** so the next test can
    be read line by line. The first read after boot shows the radio's CW
    settings at connect: `cwx wpm=22 break_in_delay=5 qsk_enabled=1`. **QSK
    is on**, and the keyer also asserts `xmit 1` around paddle keying. The
    trace should show whether that combination plays a part.

    **Trace evidence, 14:08–14:09: the stale handle DOES kill the paddle,
    and the radio does not complain.** The same short paddle key was
    traced both ways (raw traces kept in the local evidence folder):

    | | working 14:08 | dead 14:09, after an AetherSDR restart |
    |---|---|---|
    | GUI client handle (radio) | `0x54A57F28` | `0x2311E20D` (new) |
    | `cw key … client_handle=` | `0x54A57F28` | `0x54A57F28` (stale) |
    | radio reply to every `xmit`/`cw key` | `R…\|0\|` | `R…\|0\|` (no error) |
    | interlock after first `cw key 1` | `SW` → **`SW,SWCW`** | stays **`SW`** |

    In both, `xmit 1` gives `PTT_REQUESTED`/`TRANSMITTING` under the *GUI
    client's* handle, and `index`/`time` run in order. With the stale handle
    the radio answers every `cw key` with 0 and never adds `SWCW`, so the
    elements are dropped without an error. This explains this morning's 24
    of 24 `SW`-only paddle keys: the GUI client changed at 11:52 and the
    keyer was never re-bound.

    **This partly retracts the 13:51 correction above**, which relied on
    reports with no trace. Two observations there still do not fit: the
    paddle stayed dead after a rebind that fixed the handle (13:47), and it
    came back with a stale handle (13:50). Neither was traced. There is
    also still a second signature: `SW,SWCW` at 0 W with a matching handle
    (13:40, after a reboot), plus this morning's first memory after a
    paddle key stalling at 0 W. That one is not explained by the handle.

    **Failure B measured, 14:10–14:13 (handle correct, radio still silent).**
    After a rebind the keyer sent `cw key` under AetherSDR's current handle
    `0x2311E20D`. The radio answered every one with 0 and showed
    `TRANSMITTING source=SW,SWCW`, and the meter watcher read **0.00 W on 5
    of 5 paddle keys** (RF power 5, `tx_slice_mode=CW`). The record is
    identical to the working 14:08 one apart from the `time`/`index`
    values. A memory then showed this morning's freeze exactly: `cwx send`
    accepted (`R|0|1269`), `TRANSMITTING source=SWCW` at 0 W, and **no
    `cwx sent=` for 8 s** (Manoj heard one dot of sidetone and stopped
    it). Only after `cwx clear` did the radio report `cwx erase=1270,1310`
    and then `cwx sent=1269`. So in failure B the radio's CW generator is
    wedged for both inputs, paddle `cw key` and CWX text, and `cwx clear`
    is the only thing seen to un-wedge it (not every time). What wedges it
    is unknown. Seen after a keyer reboot (13:40) and after a rebind
    (14:10). Traces: `flextrace-1411-paddle-dead-metered.txt`,
    `flextrace-1413-memory-frozen.txt`, `meters-1411.log`, `lines-1411.log`.

    **Recovery measured, 14:15–14:16.** After that `cwx clear`, the next
    memory ran normally (`cwx sent=1320…1331`, one per character, peak 4.42 W
    at RF power 5), and the paddle key after it made 4.47 W with
    `SW,SWCW`. All 40 replies were 0. The paddle traffic is identical to
    the dead 14:11 key, so the keyer sends the same thing in both states.
    The difference is the radio's CW generator.

    **Open question: what wedges it at the start of a session?** Candidates
    from the keyer's connect sequence: `cw key 0 … client_handle=` sent
    **before** binding (with `0x0` after boot, or the previous client's
    handle after a rebind, because `guiHandle` is not cleared), then
    `client bind` and `cwx wpm`. Or the first paddle key of a session wedges
    it, not the connect. Test: rebind, then play a memory **before** any
    paddle key.

    **Rebind then memory FIRST works (14:18).** After a rebind, the first
    thing sent was a memory. It ran normally: `cwx sent=1332…`, one per
    character, 13.5 s, 4.47 W. The record was emptied after the rebind, so
    the connect lines are not in it and the rebind is from Manoj's report.
    Compare 14:10, where a rebind followed by a paddle key first was dead.
    So the connect sequence does not wedge the radio. **The first `cw key`
    of a fresh session does**, unless CWX has run first. Next test: rebind,
    then `cwx clear` alone (STOP, which does not transmit), then a paddle
    key. If that works, the fix is to send `cwx clear` once bound.

    **Rebind, STOP, then paddle first works (14:19), but it is not yet
    conclusive.** Paddle `SW,SWCW`, 4.47 W, all 41 replies 0. There was no
    control: the radio was already un-wedged by the 14:18 memory, and
    AetherSDR had not been restarted. The 14:10 dead case followed an
    AetherSDR restart as well as a rebind. So the priming may belong to
    the GUI client, not the keyer session. Controls still needed:
    (a) rebind then paddle first, no STOP; (b) restart AetherSDR, rebind,
    STOP, then paddle.

    **Controls, 14:21–14:22.** (a) Rebind, then paddle first, no STOP:
    **worked** (14:21:22, 4.47 W). A keyer reconnect alone does not wedge
    the radio. (b) Restart AetherSDR, rebind, STOP, paddle: 0 W on 3 keys,
    but **the test was invalid**. The keyer still held the old handle
    `0x2311E20D` against AetherSDR's new `0x0E43A5E8`, and the radio showed
    `SW` only, which is failure A. Most likely the rebind ran before
    AetherSDR had reconnected, so the keyer re-bound to the old client and
    then kept it. That is failure A's fix again: follow the GUI client, not
    a one-shot bind. (b) needs a rerun with the rebind after AetherSDR is
    back, confirmed by `flex.guihandle`.

    **(b) rerun, 14:24: a new GUI client is the trigger, and `cwx clear`
    does not prime it.** AetherSDR had restarted (`0x0E43A5E8`). The keyer
    was re-bound and `flex.guihandle` checked to match before keying. Then
    STOP (`cwx clear`), then a paddle key first: all 25 `cw key` commands
    under the correct handle, all replies 0, radio `SW,SWCW`, **0.00 W**.
    With (a): a keyer reconnect with the same GUI client is fine, while a
    new GUI client starts in, or falls into, failure B, and `cwx clear`
    beforehand does not prevent it. Not reconciled: 13:40, a keyer reboot
    with the Maestro unchanged, was also failure B. Next: restart
    AetherSDR, rebind, then a memory first. If that memory stalls, a new
    client starts wedged whatever the keyer sends. If it works, the first
    `cw key` to a new client is what wedges it.

    **(c) 14:26: a new GUI client starts wedged, even with a memory
    first.** AetherSDR was restarted (`0x7D62C96C`), the keyer re-bound
    (handle checked), the record emptied, and a memory played with no
    paddle key and no STOP. The keyer sent only
    `cwx send CQCQVU2CPLVU2CPLK` (`R|0|1353`). The radio showed
    `TRANSMITTING source=SWCW` under AetherSDR for **17 s at 0.00 W**, with
    no `cwx sent=` at all. The keyer's own backstop then sent
    `cwx clear` + `cw key 0` + `cwx clear`, the radio reported
    `cwx erase=1354,1373` then `cwx sent=1353`, and it released. Manoj
    heard this as **PTT stuck ~5 s after the memory**: local sidetone
    finished at ~12 s, and the radio stayed keyed until the backstop fired.
    That is the backstop working, not a new fault. So after a new GUI
    client connects, the radio's CW generator is wedged before the keyer
    has sent a single `cw key`. Nothing the keyer sends first avoids it,
    and the first CWX has to be cleared to release it. **Decisive open
    question: is it the keyer at all?** Restart AetherSDR with the keyer's
    Flex backend off (or the keyer unplugged) and send CW from AetherSDR
    itself (its CWX/keyboard, or a paddle on the radio). If AetherSDR's own
    first CW also stalls, it is the radio or AetherSDR.

    **(d) 14:29: AetherSDR's own CW works straight after its restart,
    with the keyer connected but NOT bound** (`flexbind off`). AetherSDR
    reconnected as `0x1A141CB0` and sent CWX itself: `cwx sent=1374`,
    `1375`, `1376`, three transmissions of `SWCW`, **4.47 W each**, on the
    first attempt. So a new GUI client is not wedged by itself. What
    differs from (c), where the first memory stalled, is that the text came
    from the keyer, a bound non-GUI client, not from the GUI client.
    Details from the same record:
    - On connect AetherSDR resets CWX (`cwx wpm=5 break_in_delay=0`, then
      back to 25/5). The keyer answered with `cwx wpm 25` within 5 ms, and
      AetherSDR's CWX still worked, so that race is not the wedge.
    - With bind off the keyer still captured a `guiHandle`, but the OLD
      client's (`0x7D62C96C`, present when the keyer reconnected), and kept
      it after that client disconnected. That is failure A again.
    - `flexbind` persists in NVS. **Restore it to `on`** after testing.
    Next test (e): bind off, keyer's handle refreshed, then a keyer memory
    first. If it works, `client bind` is what wedges the radio. If it is
    refused, binding is required and something else about a bound client's
    first CWX wedges it.

    **(e) 14:32: the keyer's memory works UNBOUND, but the test is not
    clean.** With `flexbind off`, the handle refreshed to `0x1A141CB0` and
    no `client bind` sent, the keyer's `cwx send` was accepted
    (`R|0|1377`) and ran normally: `cwx sent=1377…1397`, one per
    character, 12 s, 4.47 W. So an unbound keyer can transmit CWX on this
    radio, contrary to the old comment in `flex.cpp` saying CWX from an
    unbound client is refused. **Confound:** AetherSDR's own CWX in (d) had
    already run on this client, which may have primed it. Clean rerun:
    restart AetherSDR, `flexbind off` again (to refresh the handle), then a
    keyer memory first, with no CW from AetherSDR. Then a paddle key, also
    unbound.

    **(f) 14:34, clean: `client bind` is the trigger for failure B.**
    AetherSDR was restarted (`0x4BBEFA80`) and sent no CW itself. The
    keyer's handle was refreshed with `flexbind off`, and no `client bind`
    was sent. A keyer memory first ran normally (`cwx sent=1402…1418`,
    11.9 s, 4.47 W), then a paddle key worked (33 `cw key`, all replies 0,
    `SW,SWCW`, 4.52 W).

    | new GUI client, keyer… | first CW | result |
    |---|---|---|
    | bound, STOP first (b, 14:24) | paddle | 0 W |
    | bound (c, 14:26) | memory | stalled 17 s, 0 W |
    | not bound (d, 14:29) | AetherSDR's own CWX | 4.47 W |
    | not bound (f, 14:34) | keyer memory, then paddle | 4.47 / 4.52 W |

    The samples are small (2 bound, 2 unbound), but the split is clean.
    **Proposed fix, not implemented yet:** (1) stop sending `client bind`,
    by defaulting `flexbind` to off or removing it, because the radio
    accepts both `cwx send` and `cw key … client_handle=` from an unbound
    client; (2) follow the GUI client: on its `disconnected` status, clear
    `boundClientId` and `guiHandle` and take the next GUI client that
    connects, and clear `guiHandle` on reconnect so the connect key-up never
    goes out under a stale handle; (3) tighten the `indexOf("connected")`
    test, which also matches "disconnected"; (4) correct the comments in
    `flex.cpp`/`flex.h` that say an unbound client's CWX is refused.
    Untested: paddle key FIRST, unbound, on a fresh client. Manoj's board is
    running with `flexbind` OFF in NVS since 14:28, which is the workaround
    until the fix lands.

    **(g) 14:36–14:42: paddle first unbound, then the fix.** On a fresh
    AetherSDR with the keyer unbound, a paddle key first made 4.47 W (51
    `cw key`, all replies 0) and a memory after it ran normally (4.47 W).
    With the fix flashed, AetherSDR was restarted with **no command**: the
    keyer followed to the new handle `0x105F4906` by itself, and a paddle
    key first (4.47 W) and then a memory (4.47 W) both transmitted.

    **Next session, in order** *(written before the cause was found; kept
    for the record)*.
    1. Compare `flex.guihandle` in `/api/state` (added and flashed later on
       09-17) with the Maestro's current handle
       (`sub client all` from any API session). It is the handle every
       `cw key` goes out under. `""` means no GUI client was seen yet (it
       stays empty for the first ~10 s after boot). The
       value is NOT cleared when the radio session drops, only overwritten
       at the next bind, so right after a reconnect it can still show the
       old client's handle. The console's
       `[FLEX] bound to GUI client … (handle …)` line says the same thing.
    2. With the keying log on, record one paddle key and one memory: the
       exact `xmit` / `cw key` lines, their `client_handle` and `index`.
    3. Force a rebind (`setBind`, or drop and reconnect the Flex session)
       and repeat the paddle test on the meter.
    4. In the stuck state, try Tune from the Maestro. If Tune is 0 W too, it
       is the radio's own carrier fault seen 2026-09-15, not this keyer.
    5. If the handle is the cause: re-bind whenever the bound client
       disconnects, and prefer the GUI client that owns the TX slice.

    **Evidence** (local, deliberately not in this public repo):
    `~/projects/MSHV-Mac/user-reports/2026-09-17-cw-gone/`. It holds the
    README, `lines.log`, `meters.log`, `transmissions.txt` and
    `rumlog-to-radio.pcap`, plus the decoded capture.

## Conventions (see ~/.claude/CLAUDE.md)

- **CDP** — Commit, Document, Push together on every substantive change.
- Never pin `upload_port`/`monitor_port` — use `flash.sh`/`monitor.sh`.
- Never commit secrets — they live in git-ignored `secrets.h`.
- GitHub repos are **private** unless explicitly published.
- Credit upstream: WinKeyer protocol = Steve K1EL; K3ng keyer = Anthony
  Good K3NG.
