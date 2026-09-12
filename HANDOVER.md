# ESP32 WinKeyer — Project Handover
*For continuation in a new Claude session*

**Created:** 2026-08-26 · **Updated:** 2026-09-12 (evening) · **Type:** ESP firmware
(esp32dev, S3 env reserved) · **Status:** working keyer, **public repo**
(MIT). RUMlogNG drives it over USB and keys the Flex; OLED/LCD panel,
speed pot, settings web page, memories, second radio and RTTY FSK all on
hardware.

**Read this first if you are picking the project up after 2026-09-12:**

- **Arduino core 3.3.11 / IDF 5.5.5** via the pioarduino platform, and
  **PlatformIO must run on Python 3.10+** — the Mac's system one is 3.9 and
  cannot build this. `~/.pio-venv313` holds a suitable one; `flash.sh`,
  `monitor.sh` and `install.py` find it themselves.
- **The keyer is driven through a SECOND USB-serial adapter** wired to
  TX/RX/GND only (`usbserial-A9M9DV3R`), with the board's own USB-C
  unplugged and external power. The devkit's USB chip can hold the ESP32 in
  reset, which stops the keyer mid-over and leaves the radio transmitting.
- **Flashing therefore needs BOOT held and RST tapped by hand**, then
  `pio run -t upload --upload-port <the FTDI>`. The board cannot be reset
  by the adapter.
- Outstanding: FSK polarity and on-air fist quality unverified; the LCD
  slice warning never seen on a panel; the board mod that would end the
  reset problem for good is not done.

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

12a. **OPEN (reported 2026-09-12 at wrap-up): characters missing from the
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
6. **Pin config command (WK 0x09)** — only bit 0 (PTT enable) is acted on.
   The remaining bits differ between WK revisions and guessing wrong would
   silently disable sidetone or key output. Revisit after testing with a
   real logger.
7. Hardware build: paddle/key/PTT interface (PC817 + 330 Ω), enclosure.
   The speed pot and the OLED are **wired and working** (2026-09-10);
   what remains is the opto-isolated key/PTT interface and the box.
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
   21/22 (see 7a for the bench test that still owes). Still considered
   but not built: **Bluetooth keyboard** (BT Classic HID *host* support is
   thin on ESP32 and BT/WiFi share the radio; prototype standalone before
   committing) — it was always the one with real unknowns.

   Also not built, now that a display exists to make them worth having:
   a **command button** on one of the input-only spares (35/36/39) for
   menu/message playback, and showing **decoded sent text** on the panel.
10. Future: ESP32-S3 env for a native-USB descriptor. **OTRSP/SO2R is not
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

## Conventions (see ~/.claude/CLAUDE.md)

- **CDP** — Commit, Document, Push together on every substantive change.
- Never pin `upload_port`/`monitor_port` — use `flash.sh`/`monitor.sh`.
- Never commit secrets — they live in git-ignored `secrets.h`.
- GitHub repos are **private** unless explicitly published.
- Credit upstream: WinKeyer protocol = Steve K1EL; K3ng keyer = Anthony
  Good K3NG.
