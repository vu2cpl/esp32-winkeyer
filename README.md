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
| Speed pot on GPIO 34 | working, wired and tracking on hardware |
| WinKeyer protocol engine (WK 2.3 host mode) | working, verified with `tools/wk-test.py` |
| WiFi TCP transport + mDNS `winkeyer.local` | working, verified over WiFi |
| FlexRadio backend — **paddle keying over the network** | working, verified on a 6600 |
| RUMlogNG over USB serial (1200 8N2) | working — memories, typed text, echo |
| Host bridge (`tools/wk-bridge.py`) | implemented, never driven by a real logger; macOS will not let a PTY appear as `/dev/cu.*`, so a logger cannot select it there |
| OLED status panel (SH1106/SSD1306 128x64) | working, SH1106 at 0x3C on hardware |
| HD44780 LCD 16x2 / 20x4 | working; the slice warning on it is untested |
| Settings web page at `winkeyer.local` | working, exercised on hardware |
| Persisted settings (NVS) | working, verified across a hard reset |
| Message memories (6) with `%C` expansion | working from the web page and a logger |
| RTTY FSK on GPIO27 | implemented, polarity unverified on air |
| MQTT status + heartbeat | working against the shack broker |
| Arduino core | **3.3.11 / IDF 5.5.5** (pioarduino); 2.0.17 crashes under load |

Bluetooth keyboard is considered but not built — see `HANDOVER.md`.

**One hardware caveat on a devkit.** The USB-serial chip's DTR/RTS lines
reach the ESP32's reset pin, so a logger holding that port can reset the
board or hold it in reset — mid-over, which leaves a networked radio
transmitting. Measured: `RTS` asserted with `DTR` not asserted holds it
down indefinitely. Either drive the keyer through a separate USB-serial
adapter wired to TX/RX/GND only, or cut the auto-reset link to EN. See
"Serial / USB" below.

## Quick start

```bash
python3 install.py     # detects the host, prompts for local settings, verifies the build
./flash.sh             # build + upload (picks the serial port)
./monitor.sh           # serial monitor, 1200 baud
```

**PlatformIO must run on Python 3.10 or newer.** This firmware builds
against Arduino core 3.x (the pioarduino platform), which refuses older
Python with a bare `ERROR: Python version must be 3.10 ...` that never says
which PlatformIO it means. `install.py` checks first and offers to create
one in `~/.pio-venv313`, leaving any existing install alone; `flash.sh` and
`monitor.sh` find it the same way (`PIO=/path/to/pio` overrides). Why the
newer core: 2.0.17's `WiFiClient` double-frees an lwIP buffer under heavy
inbound traffic, which crashed this keyer within minutes of sending
memories to a FlexRadio — see `HANDOVER.md`.

The app partition is the single 3 MB `huge_app` layout, because the newer
core filled 94.8% of the default one. Nothing here uses OTA. Flashing
rewrites the partition table, so an older board needs no special steps.

`install.py` detects macOS, Raspberry Pi, Linux or Windows (with a manual
override), then asks for your MQTT broker, mDNS name and setup-AP details.
Answers go to **`include/secrets.h`**, which is git-ignored and overrides
the defaults in `config.h` — so a clone stays clean and an upgrade never
collides with your local edits. Press Enter through it all to accept the
defaults; nothing here is required to key CW.

**On Windows** `flash.sh` / `monitor.sh` cannot run, so use the
cross-platform equivalents, which pick the port the same way:

```bash
python install.py flash            # build + upload
python install.py monitor          # 1200 baud
python install.py monitor 115200   # after /baud 115200
```

The port is never guessed when several boards are attached — some CP2102s
share factory serial `0001`, so the wrong board would be flashed silently.

1. First boot opens WiFi AP **`vu2cpl-esp32-winkeyer-setup`** (password
   `vu2cpl1234`). Join it from a laptop or phone; a captive portal opens
   where you pick **your own network** and enter its password. Creds
   persist in NVS. The portal does not time out, and the keyer keys
   normally while it is open — onboarding is non-blocking by design.

   Put the keyer on the **same subnet as the logging computer** (and the
   radio, if using the Flex backend): `winkeyer.local` and Flex discovery
   are both broadcast-based and do not cross subnets or VLANs. For the
   radio there is a way round it — **Find radio** on the web page (see
   FlexRadio below).

   `/wifi` shows the current network, `/wifi portal` reopens the portal,
   `/wifi reset` clears the saved credentials and reboots.

   **Security note:** the WinKeyer TCP port is unauthenticated — anyone on
   that network can key the transmitter. Use a trusted LAN, not a guest or
   open network.
2. Copy `include/secrets.h.example` → `include/secrets.h` and set
   `MQTT_HOST` plus the MQTT role account and password. `secrets.h` is
   git-ignored. Leave `MQTT_HOST` out and the build uses the placeholder
   in `config.h`, which is almost certainly not your broker.

## Wiring

| Signal | GPIO | Notes |
|---|---|---|
| Paddle dit (tip) | 25 | internal pullup, paddle closes to GND |
| Paddle dah (ring) | 26 | internal pullup, paddle closes to GND |
| Key out | 33 | active high → PC817 opto (330 Ω) or NPN → rig KEY |
| PTT out | 32 | active high → PC817 opto (330 Ω) or NPN → rig PTT |
| Sidetone | 4 | passive piezo to GND |
| Speed pot | 34 | ADC1, 10 k **linear** pot across 3V3–GND, wiper to 34; **enable with `/pot on`** |
| Status LED | 2 | onboard — lit while the key is down |
| OLED SDA | 21 | 128x64 I²C panel, optional |
| OLED SCL | 22 | |
| KEY out 2 | 18 | radio 2 KEY, same drive as radio 1 |
| PTT out 2 | 19 | radio 2 PTT |
| FSK out | 27 | RTTY keying line, mark = idle (invertible) |

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

The speed follows the knob through a **0.6 WPM hysteresis band**. A pot
parked on a step boundary otherwise alternates between two speeds forever,
and each flip is both a speed change and an unsolicited WinKeyer pot byte —
a stream that saturates a 1200-baud host link. Hysteresis is the right cure
rather than heavier smoothing or a settle delay: those fix the dither by
adding lag to every deliberate turn as well, which the operator feels at
once as a sluggish knob.

**OLED (optional).** VCC→3V3, GND→GND, SDA→21, SCL→22. Most breakouts
carry their own pull-ups; if yours does not, add 4.7 kΩ from each line to
3V3. The firmware probes 0x3C then 0x3D at boot and stays off if nothing
answers, so an un-wired board is unaffected.

**The I²C bus runs at 100 kHz by default**, and that is deliberate. An
earlier version tried 400 kHz and kept it if the panel answered its address
there — which proves nothing, because an address probe is one byte and a
frame is a thousand. A panel on breadboard leads passes the probe and
renders nothing, giving a display the firmware reports as present and
enabled while the glass stays dark. `/disp fast` opts into 400 kHz on
wiring that deserves it (~25 ms a frame against ~100 ms); the choice
persists, and `/disp slow` goes back.

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

**Four panel types are supported, all I²C on the same two pins:**

| Type | Address | Layout |
|---|---|---|
| OLED SH1106 128x64 (1.3") | 0x3C / 0x3D | large WPM digits, activity box |
| OLED SSD1306 128x64 (0.96") | 0x3C / 0x3D | as above |
| LCD 20x4 via PCF8574 backpack | 0x27 / 0x3F | four text rows |
| LCD 16x2 via PCF8574 backpack | 0x27 / 0x3F | speed + one status row |

The **family is auto-detected** — OLEDs and LCD backpacks live at different
addresses — so one firmware runs whichever panel is plugged in. Swap the
panel and pick **Auto-detect** on the web page (or `/disp auto`) to re-probe;
no reflash.

What *cannot* be detected, because each pair shares an address:

- **SH1106 vs SSD1306** — wrong choice puts the image 2 px right with a
  garbage sliver down the left edge (SH1106 has 132 columns of RAM to the
  SSD1306's 128). Fix with `/disp ssd1306`.
- **16x2 vs 20x4** — same chip, so a wrong choice just truncates or leaves
  rows blank. Fix with `/disp lcd16x2`.

### Reading the display

**OLED, 128x64:**

```
WinKeyer            -52dBm     link quality, or "no wifi"
--------------------------
 28 WPM  POT        [ KEY ]    speed, where it came from, activity
--------------------------
FLX1   B  HOST+NET             backend+radio, iambic mode, host links
192.168.1.20                   address, or "join <setup AP>"
```

**LCD 20x4** carries the same fields as text:

```
28 WPM POT   KEY
FLX1   B HOST+NET
192.168.1.20
-52dBm  tail 400ms
```

**LCD 16x2** has room for two rows, so the iambic mode letter is dropped —
it changes once a year, whereas the live radio can change between overs:

```
28WPM POT FLX1
192.168.1.20
```

Every indicator:

| Shown | Meaning |
|---|---|
| `28 WPM` | current speed |
| `POT` | speed is following the knob |
| `FIX` | speed was set by host, CLI or web — the knob is off |
| `KEY` | key is down right now |
| `TUNE` | continuous carrier, latched until you stop it |
| empty box | idle |
| `LOCAL` | keying the wire: KEY on GPIO33, PTT on GPIO32 |
| `FLX` | keying the radio over the network, slice ready |
| `FLX!` | radio connected but **the slice is not in CW mode** — it will transmit nothing |
| `FLX?` | **not connected** to the radio at all |
| `…1` `…2` `…B` | which radio the key line drives — 1, 2, or **both** |
| `A` / `B` | iambic mode (not shown on 16x2) |
| `HOST` | a WinKeyer host session is open (a logger is attached) |
| `----` | no host session — placeholder, so the field keeps its width |
| `+NET` | a TCP client is connected over WiFi as well |
| `-52dBm` | WiFi signal; `no wifi` if the link is down |
| `SLICE USB, NOT CW` | the Flex's slice is in another mode; `NO SLICE IN USE` if there is none. OLED: replaces the `WinKeyer` title. 20x4: replaces row 4 (dBm/tail). 16x2: alternates with the IP every 2 s when not sending, as `SLICE USB NOT CW` (`DIGU: NOT CW` for 4-letter modes) |

The radio number is attached to the backend as one token — `LOCAL1`,
`FLX2`, `FLXB` — rather than spaced, because the "both" letter `B` would
otherwise sit beside the iambic mode letter, which is also `A` or `B`.

`FLX!` is the one worth knowing on sight: everything looks connected and
the keyer reports no error, but SmartSDR has no slice in CW mode so nothing
reaches the air. Every panel also spells it out with the slice's mode (see
the table), and the web page shows an amber banner. Memories go to the
radio as `cwx` text, which it sends only
on a CW slice. The paddle's PTT still keys the radio in other modes, so
this can look like "paddle works, memories don't".

### LCD power — read this before blaming the firmware

**HD44780 LCDs want 5V, and the ESP32 is a 3.3V part.** Run one from 3V3
and the characters come out so faint they look absent, at any setting of
the contrast trimmer, with a dim backlight to match. Nothing in software
can help: contrast on an HD44780 is the analogue Vo pin, not a driver
setting. This is why the OLEDs are trouble-free — SSD1306 and SH1106
modules are native 3.3V.

Feed the LCD's **VCC from the devkit's 5V / VIN pin** (USB 5V). But note
the trap: the PCF8574 backpack's onboard pull-ups then tie SDA and SCL to
5V, and **ESP32 GPIOs are not 5V tolerant** — that is how GPIO21/22 get
damaged. Two safe ways:

1. **Move the pull-ups.** Remove the backpack's two pull-up resistors and
   fit 4.7 kΩ from SDA and SCL to **3V3** instead. I²C is open-drain — the
   chip only ever pulls the line low — so with the pull-ups on 3.3V the bus
   never exceeds 3.3V while the LCD still runs at 5V. Tidiest answer.
2. **A bidirectional level shifter** (BSS138-type) between the ESP32 and
   the backpack. No soldering on the module.

Before either, sweep the contrast trimmer through its full range with the
panel powered. Faint ghosting at one end confirms the voltage diagnosis; a
completely dead panel at every setting points at wiring or the address
instead — run `/i2c`.

See `include/pins.h`.

## Serial / USB — baud matters

**The firmware defaults to 1200 baud, 8N2.** That is the K1EL WinKeyer
serial standard, and loggers open the port that way without asking —
RUMlogNG was observed doing exactly this (`stty` reported
`speed 1200 baud; cs8 cstopb`). At any other rate a logger's handshake
arrives as noise and no session ever opens, which looks exactly like a
dead keyer.

The console shares that port, so at 1200 baud the boot log is trimmed to
one line and WiFiManager's chatter is silenced — every character printed
is one the host waits through before its Host Open is answered. Use the
web page or `/status` for detail.

```bash
/baud 115200     # readable console; a logger will NOT talk to it here
/baud 1200       # WinKeyer standard; what loggers expect
./monitor.sh          # defaults to 1200
./monitor.sh 115200   # when you have set the console rate
```

The **SERIAL / USB** panel on the web page sets the same thing, and is the
escape hatch if you pick a rate you cannot monitor at — WiFi is unaffected
by the serial rate.

**Known limitation of this board, and it is worse than noise.** The USB
chip's DTR and RTS lines drive the ESP32's EN (reset) and GPIO0 through the
devkit's auto-reset transistors. Two consequences, both measured here:

- **Opening or closing the port resets the board.** The ROM then prints its
  banner at 115200 whatever the firmware's rate is, so a logger sitting at
  1200 sees a burst of garbage before the handshake. Noise, not a fault.
- **`RTS` asserted while `DTR` is not holds EN low — the board stays dead**
  until the lines change. Silent: no boot banner, no WiFi, no app. A logger
  that parks the lines that way stops the keyer mid-over, and on the Flex
  backend the radio is left transmitting because the key-up never goes out.
  Measured 2026-09-12 with all four combinations; only that one kills it.

A real K1EL WinKeyer ignores DTR/RTS, so loggers drive them freely — this
is a devkit problem, not a logger bug, and no firmware can defend against
its own reset pin. Fixes, in order of preference:

1. **Disable the auto-reset link to EN.** Find the two `J3Y` transistors by
   the USB chip, identify by continuity which one's top leg (collector)
   reaches the `EN` header pin, and lift that leg. Flashing then needs
   BOOT held while EN is tapped. Bring the cut ends to a jumper if you want
   auto-flash back for development.
2. **Give the logger a serial adapter with only TX/RX/GND wired** to the
   keyer, DTR/RTS unconnected. No soldering on the devkit, but a second lead.
3. **Move to the ESP32-S3 env**, whose native USB has no DTR-driven reset.

## Sharing the port with a logger

**At 1200 baud the console is silent by default**, because that rate means
a logger owns the port: every character the keyer prints lands in the
logger's CW window as text. That covers the boot log, the reset reason, the
watchdog line and the ESP core's own logger (`E (1816) task_wdt: ...`),
which writes to the UART without passing through `Log::` — muting one and
not the other is why an early attempt still leaked. `/log on` turns it back
on; `/baud 115200` means "a human is here" and leaves it on. One thing
firmware cannot suppress: the ROM's own boot banner is printed at 115200
before any of our code runs, so a port opened at 1200 sees a character or
two of noise after every reset.

The console and the WinKeyer protocol share one serial port, so **while a
host session is open the console goes silent** (`src/log.cpp`). A `[FLEX]`
line written during a session is not a log message to the logger — it is
protocol data, and it appears in the logger's CW window as garbage text.
The web page and MQTT report the same state and do not touch serial, so
use those while a logger is connected.

### Sidetone while the radio is keying

On the Flex backend the radio generates buffered CW itself, so the keyer
has no elements of its own to sound and is silent while the rig transmits.
The same text is therefore also run through the local keyer purely for
**monitor sidetone** at the same WPM (`/monitor on|off`, or the checkbox on
the web page).

Those monitored elements are withheld from the key hook
(`Keyer::setHookPaddleOnly`) — otherwise they would key the radio a second
time on top of `cwx send`. Paddle elements and `tune` still reach the hook.

### Echo

WinKeyer character echo is **host-controlled**: mode-register bit 2. If a
logger never sets it there is no echo, and that is the protocol, not a
fault. `/api/state` reports `echo` and `modereg` so you can see what the
host actually asked for rather than guessing — RUMlogNG sets `0x07`, so it
does want echo.

Echo exists so the host can highlight the character being **sent**, which
makes it a timing signal, not just a copy. On the Flex backend the whole
buffer is handed to the radio in one batch, so echoing as characters are
queued dumps the entire message instantly: the host's highlight runs ahead
of the air and desynchronises, and after the first message later echoes are
discarded. Echo is therefore paced against the radio's own `cwx sent=`
progress reports. Measured at 20 WPM, `TEST DE VU2CPL` echoes over 5.5 s
with the gaps matching each character's length.

### Paddle echo — capturing hand-sent text

Separate from character echo, and separate in the protocol too: **mode
register bit 6**. When active, characters you send on the paddle are echoed
to the host so a logger can capture what was keyed by hand.

RUMlogNG sets `0x07` — it asks for character echo but **not** this — so
`auto` leaves it silent. Force it if you want hand-sent text logged:

```bash
/pecho on      # regardless of what the host asks for
/pecho auto    # follow the mode register (default)
/pecho off
```

The web page has the same control with a live `active`/`inactive` readout.

The decode is exact rather than signal decoding: the keyer generated those
elements itself, so it knows precisely what they were and only has to judge
where a character ends — 2 dit-times of silence for a character, 5 for a
word, both scaling with WPM automatically. Buffered text is excluded; the
host already knows what it asked for.

Two limits worth knowing: characters not in the Morse table decode to
nothing and are dropped, and a prosign keyed as merged elements comes back
as whatever single pattern it forms, not as the letters you had in mind.
Forcing echo on when the host did not request it may also confuse a logger
that is not expecting unsolicited characters — hence the switch.

## Two radios

`/radio 1|2|both` selects which KEY/PTT pair the keyer drives — radio 1 on
GPIO33/32, radio 2 on GPIO18/19 — and persists. `both` is deliberate, for a
rig plus an amp or monitor, but it keys two transmitters at once so it is
never the default. Switching radios drops every line first, so a
transmission can never strand the outgoing radio keyed.

## Message memories

Six slots of canned text in flash, played through whichever backend is
current. `%C` expands to your callsign, so a memory survives a contest call
change.

```bash
/call VU2CPL
/mem 1 CQ TEST %C %C K
/mem 1              # play it
/mem                # list all six
```

The web page has a MEMORIES panel with SAVE and PLAY per slot. No GPIO
cost — front-panel buttons can be wired to these later.

**Anything that originates text must go through `WinKeyer::sendText()`**,
not `Keyer::sendChar()`. On the Flex backend the radio generates the CW and
those local elements are deliberately withheld from the key hook, so a
direct send produces sidetone and no RF.

## RTTY / FSK

An FSK keying line on **GPIO27** for a rig's FSK input: Baudot (ITA2) at
45.45 baud, 1 start bit, 5 data bits LSB first, 1.5 stop bits, mark idle.

```bash
/fsk RYRY DE VU2CPL     # send
/fsk baud 45.45         # or 50, 75
/fsk invert on          # if the rig wants mark low
/fsk diddle on          # LTRS while idle, keeps the far end synchronised
/fsk stop
/fsk                    # status
```

The web page has an FSK panel with its own send box.

**Polarity is the one thing you must confirm on air.** Getting `invert`
wrong prints reversed-case gibberish at the far end rather than nothing, so
silence means a wiring fault and garbage means the wrong polarity.

Two implementation notes that matter if you touch this code. The bit timer
runs at the **half-bit** period, because the 1.5-bit stop is three
half-bits and a 22 ms bit cannot be timed from the 1 ms FreeRTOS tick
without ~4% jitter. And the far end's **LTRS/FIGS shift state is tracked
explicitly**: `Q` and `1` are the same five bits, so losing the shift
prints digits as letters for the rest of the over. Shift characters are
injected only when a character actually needs the other case, and space,
CR and LF are treated as neutral so they never force one.

Verified by timing, not by scope: `RYRY DE VU2CPL` takes 2.87 s at 45.45
baud against a theoretical 2.81 s, and 1.77 s at 75 baud against 1.70 s.
The 14-character message occupies 17 code times, which is the two shift
characters `2` forces.

## Power — read this if the keyer resets while you key

The ESP32 draws its heaviest current in short WiFi transmit bursts, and
paddle keying on the Flex backend sends a packet per key edge — roughly
twenty bursts a second at 25 WPM. On a marginal supply the 3.3 V rail sags
through those and the chip **browns out and restarts**.

It is **intermittent**, which is what makes it hard to pin: in testing the
same board and cable browned out within two characters of keying on one
run and survived minutes of heavy paddling on the next. It does not track
any one cable or power setting reliably — it tips over when several small
things happen to line up (connector contact resistance, other load on the
same USB bus or hub, how the cable is seated).

How to recognise it: **the web page footer, or `/api/state`, reports
`last reset: BROWNOUT (power)`.** That comes from the chip's own brownout
detector, so it is conclusive. (A plain `power-on` is not — opening the
serial port pulls EN on this devkit and reports the same.)

What to do about it, in order of robustness:

1. **Fit a 470–1000 µF capacitor across 3V3 and GND**, short leads, at the
   board. This is the real fix: it adds margin without needing to know
   which factor is marginal on a given day.
2. **Power from a decent 5 V supply on VIN** rather than USB. An external
   supply held full power through heavy keying in testing.
3. **Lower the WiFi transmit power** — `/txpower 11`, or *WiFi power* on the
   web page. That roughly halves the burst current. It costs range (it
   affects how well the access point hears you, not how well you hear it),
   so treat it as a workaround rather than a cure.

## Bench-testing the key and PTT lines

The four outputs are plain active-high 3.3 V GPIOs, so an LED and a
resistor to GND is enough to watch them:

| Line | GPIO |
|---|---|
| KEY / PTT, radio 1 | 33 / 32 |
| KEY / PTT, radio 2 | 18 / 19 |

Anode to the GPIO, cathode through **330 Ω** to GND — about 4 mA, bright
enough and well inside what an ESP32 pin should source. Do not go below
~150 Ω.

**On the Flex backend the KEY line is deliberately idle** (the radio is
keyed over the network instead, so the rig is not keyed twice) — a KEY LED
will stay dark however much you send. Use `/backend local` to test it. PTT
is live on both backends.

`/tune` is the easiest test: a continuous key-down with PTT asserted, so
both LEDs sit steady rather than flickering through elements. Sending text
afterwards shows the sequencing — PTT leads the first element by the
lead-in and holds for the tail after the last.

`/radio 2` moves keying to the second pair and `/radio both` drives all
four, which is also the quickest way to confirm the `LOCALB` / `FLXB`
indicator on the display.

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

**Find radio** on the web page does that for you. It tries every address
in one /24 for the SmartSDR API port (TCP 4992) — a plain TCP connection,
which a router forwards where a broadcast is dropped — and lists what
answers as `IP · model · nickname` buttons; click one to set Radio IP.
Leave its box blank to scan the keyer's own subnet, or type the radio's
first three octets (`192.168.1`). About 15 s per /24, in a background
task so keying is unaffected. It is read-only on the radio: connect, read
the `V<version>` greeting that marks a Flex, ask `info`, disconnect.
`POST /api/flexscan?net=192.168.1` starts one; `GET /api/flexscan`
reports progress and results.

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
  radio simply transmits nothing. `/status` reports readiness, the web
  page and OLED show a warning naming the slice's mode, and the console
  warns when you key without it. The keyer follows the last slice the
  radio reports, not specifically the TX slice, so with two slices open
  the warning can be wrong.
- SmartSDR (a GUI client) must be connected — with none the radio reports
  `tx_allowed=0` and nothing may transmit at all.

**Sidetone delay (`/mondelay`, default auto).** The radio generates
buffered CW itself, so it starts a few hundred ms after being handed the
text — the network hop plus its own CW start — while the local monitor copy
starts at once. The sidetone therefore runs AHEAD of the air, which sounds
like the radio unkeying early. The keyer times that gap itself (from
`cwx send` to the interlock reporting TRANSMITTING, smoothed, and only when
the radio was idle first) and holds the monitor copy by it: measured 229 ms
on a 6600 over WiFi here. `/mondelay 250` pins a value instead, `/mondelay 0`
disables it. The first transmission after a reset has nothing measured yet.

The monitor copy also ignores weighting and Farnsworth, because the radio
has neither — it exposes speed, iambic, break_in and qsk, nothing else. A
logger that sets Farnsworth (RUMlogNG sets 20 every session) would otherwise
stretch the sidetone against an unstretched transmission.

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
./tools/flex-ptt-watch.py --keyer <ip> --radio <ip>  # keyer + radio PTT on one clock
./tools/boot-listen.py /dev/cu.usbserial-0001        # is it boot-looping?
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
`/weight N` `/ratio N` `/farns N` `/lead N` `/tail N`
`/backend local|flex` `/flex on|off|ip <addr>|auto` `/wifi [portal|reset]`
`/i2c` `/net` `/status`. Any other line is sent as CW.

## Settings web page

`http://winkeyer.local/` (or the IP — `/net` prints it). Every label with a
dotted underline carries **hover help** — ranges, what a setting actually
does, and which GPIO it drives — so the panel stays scannable. Speed, mode,
paddle swap, sidetone, **weighting, dah ratio, Farnsworth, PTT lead and
tail**, pot enable and range, display, backend (with a **Find radio**
LAN scan for the Flex), plus a send box and
tune/stop. Live status LEDs for host, TCP, key, tune, pot, Flex and OLED,
polled once a second.

Timing settings, all persisted and all validated the same way from the
CLI and the page:

| Setting | Range | Nominal | What it does |
|---|---|---|---|
| Weight | 10–90 | 50 | mark/space balance, **without** changing WPM |
| Dah ratio | 33–66 | 50 | dah length (50 = the standard 3 dits) |
| Farnsworth | 0, or 5–60 | 0 (off) | stretches only the gaps, to this WPM |
| PTT lead-in | 0–2000 ms | 50 | delay before the first element (GPIO32) |
| PTT tail | 0–2000 ms | 250 | hold after the last element |

**The tail releases two transmitters.** It sets both the local PTT line
and, on the Flex backend, the radio itself (`xmit 0`) — these are separate
timers and setting only one makes the control appear dead on whichever you
are listening to. `/api/state` reports `flextail` next to `tail` so you can
see they agree.

On the Flex backend the **KEY line (GPIO33) is idle** so the rig is not
keyed twice, but the **PTT line (GPIO32) stays live** for an amp or
sequencer — that is what lead-in still drives. The radio handles its own
T/R, so there is no Flex lead-in.

Out-of-range values are refused with the accepted range and nothing is
changed — `farns 3` answers `farnsworth: 0 (off) or 5..60`.

Number boxes step with **↑/↓ by 1, Shift+↑/↓ by 10**, clamped to the
setting's range. The value is written once you pause, so holding a key
costs one save rather than one per key repeat — these go to NVS. A field
you are editing is also exempt from the 1 Hz status poll, which would
otherwise overwrite what you are part-way through typing.

Same trust posture as the WinKeyer TCP port: **no authentication**, so
keep it on a trusted LAN. Visual style is borrowed from soft-MORCONI
(`~/projects/Morconi`) — that project is a browser UI plus a Node bridge,
so the look carried over and none of the code did.

### Host overrides end with the session

A WinKeyer host may set speed, mode, weighting and PTT lead/tail for the
length of its session — that is the protocol working as intended. RUMlogNG,
for instance, sets lead and tail to zero. Those values are **restored from
NVS when the host closes or the transport drops**, so a logger can never
leave the keyer reconfigured: without that, a session that set the tail to
zero left it there until the next reboot.

The web page shows the *effective* values, so while a logger is connected
you are seeing the host's numbers, not your saved ones.

### FlexRadio and where keying goes

Enabling the FlexRadio backend switches keying **to the radio**; disabling
it hands keying back to the **local key line**. Either is overridable from
the Keying selector afterwards. The coupling exists because the two
half-states are traps: keying a switched-off backend sends CW nowhere
while the page still claims the radio.

With Flex disabled the page hides its settings and drops it from the
Keying choices altogether, so an unreachable backend cannot be selected.

### Checking the settings page without flashing

The page is a PROGMEM string inside the firmware, so it normally takes a
flash to look at. It doesn't have to:

```bash
python3 tools/web-preview.py     # http://127.0.0.1:8791/
```

That serves the real page out of `src/web.cpp` against a stubbed API — edit
and reload, no board involved. **Use it before flashing UI changes.** A
clean build says nothing about whether the JavaScript runs: one session put
three faults on hardware that a single page load would have caught — a line
in the wrong scope that blanked the page entirely, `hidden` on a `.row`
doing nothing because `.row{display:flex}` outranks it, and `hidden` on an
`<option>`, which Safari ignores.

### Which settings stick

The **operator's** panel settings persist in NVS: speed, mode, swap,
sidetone, PTT, weighting/ratio/Farnsworth, pot enable and range, display,
backend. The **host's** session settings do not — a speed N1MM sets over
the WinKeyer protocol is gone at the next boot, so a contest never leaves
the keyer permanently reconfigured. CLI and web page both go through
`Settings::apply()`, so they cannot disagree about ranges or names.

## MQTT

- Broker: `MQTT_HOST` in `secrets.h` (`config.h` default `192.168.1.10` is a
  placeholder); auth required — role account in `secrets.h`. The account
  needs **write** on `shack/esp32-winkeyer/#`, or the broker accepts the
  login and silently drops every publish.
- A broker that is down or unreachable costs nothing: the connect is capped
  at 500 ms and never attempted while CW or PTT is active.
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
tools/web-preview.py   serve the settings page locally, before flashing
tools/flex-ptt-watch.py  keyer state + radio interlock/cwx, one timeline
tools/boot-listen.py   count ROM resets on the serial port (needs pyserial)
flash.sh / monitor.sh  serial-port pickers (never pin the port)
install.py             toolchain bootstrap, macOS/Pi branch
```

See `HANDOVER.md` for design decisions and open items.

## Licence

MIT — see [LICENSE](LICENSE). Use it, change it, build one; just keep the
copyright notice.

The WinKeyer protocol is Steve K1EL's, and the K3ng CW keyer by Anthony
Good K3NG was used as a behavioural reference. The implementation here is
original and carries no upstream licence obligations.
