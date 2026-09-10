# ESP32 WinKeyer — Project Handover
*For continuation in a new Claude session*

**Created:** 2026-08-26 · **Updated:** 2026-09-10 · **Type:** ESP firmware
(esp32dev, S3 env reserved) · **Status:** feature-complete on the bench;
OLED panel, settings web page and persisted settings all verified on
hardware, speed pot wired and tracking; awaiting on-air testing

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
| OLED SDA / SCL | 21 / 22 | SH1106 or SSD1306 128x64, optional; probed at boot |
| Status LED | 2 | onboard |
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

**`tools/flex-check.py` — run this first when the Flex will not key.**
It reports every silent prerequisite in one pass (slice in use, slice
mode, GUI client, interlock, break-in) and with `--key` drives a keying
test and watches interlock for proof of transmission. Written after a
session lost hours to "no slice in use", which the radio never mentions.

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
      breadboard leads. Detection now always runs at **100 kHz**;
      rendering moves to 400 kHz only after the panel proves it answers
      there, and the boot line reports the speed that won. A 100 kHz
      fallback in the log means the wiring wants pull-ups.
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
  - **Settings left behind by testing** (they persist, so they are real):
    pot range is **12-40 WPM**, not the 10-35 default. `/pot 10 35` to
    restore. Speed and mode were also written during the persistence
    test.

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
