# Genuine K1EL WinKeyer probe — 2026-09-13

Device: K1EL WKUSB-type keyer, FT232R `usbserial-AI02BVHE`, 1200 8N2.
Admin host open returns **31 (0x1F) = WK3.1**. Reference: K1EL "WinKeyer3 IC
Interface & Operation Manual" Rev 1.3 (3/19/2019). Raw logs and the scripts that produced them: `k1el-probe1..6.{log,py}`
(probe 5 had Manoj on the pot and paddle).
The WinKeyer protocol and datasheet are Steve K1EL's; the datasheet is not
redistributed here — fetch it from k1elsystems.com (`WK3_Datasheet_v1.3.pdf`).
Every item below was measured on the hardware AND agrees with the datasheet
unless marked otherwise.

## Firmware defects found (src/winkeyer.cpp)

1. **Status flags are all one bit too high** (`ST_*`, lines 33-37).
   Real: XOFF 0x01, BREAKIN 0x02, BUSY 0x04, KEYDOWN 0x08 (WK1 mode; in WK2
   mode bit 3 = pushbutton tag), WAIT 0x10. Ours: 0x02/0x04/0x08/0x10/0x20.
   Our WAIT gives 0xE0, not a status byte at all. A logger reads our BUSY as
   KEYDOWN/pushbutton and our BREAKIN as BUSY. `tools/wk-test.py` decode()
   carries the same wrong map, which is why self-tests passed.
2. **0x0F load-defaults field order wrong from byte 3.** Datasheet Table 13:
   mode, speed, sidetone, weight, lead, tail, minWPM, WPMrange, X2MODE,
   keycomp, farnsworth, paddle setpoint, ratio, pincfg, X1MODE.
   Ours: mode, speed, setpoint, potmin, range, farns, weight, lead, tail, -,
   ratio, comp, firstext — so the pot range is taken from weight/lead-in.
   (No hardware round-trip possible: admin 7 is unsupported in WK3.)
3. **Pot byte scaled wrong.** Real = WPM − MINWPM, unscaled (min 10 / range
   30 swept 0..30). Ours = (wpm−min)·31/range. Real sends one unsolicited
   byte per step.
4. **Admin parameter counts desync the parser** (`adminParams`): 0x0D load
   EEPROM takes 256 bytes (ours 15); 0x13 set RTTY registers takes 2 (ours
   0, treated as mode select); 0x14 is Set WK3 Mode, 0 params (ours 1, as
   sidetone volume); 0x16 load X2MODE takes 1 (ours 0); 0x19 sidetone volume
   takes 1. Datasheet text for 25 says `<00><24><n>` — ambiguous, verify.
5. **Admin replies missing/wrong:** 0x09 = Get FW major rev (ours "get
   calibration", emits 0); 0x15 Vcc, 0x17 FW minor, 0x18 IC type each return
   one byte (ours nothing). Measured: 0x09 → 31, 0x17 → 2 (fw 31.02),
   0x18 → 1 (SMT), 0x15 → 62 (26214/62 = 4.22 V); echo test after each
   stayed in sync. Admin 7 Get Values: WK3 datasheet says "always
   returns 0"; this unit returned **nothing** (5 s, open and closed). Ours
   returns 15 bytes — correct for the WK2 we claim to be.
6. **Pause does not set WAIT** on the real keyer (status stayed 0xC4).
   Ours sets WAIT while paused. Real pause finishes the current character;
   Clear Buffer cancels pause (measured: text sent after 0x0A went out with
   pause never lifted). Ours does not clear `paused` on 0x0A.
7. **BREAKIN is a level, not a pulse.** Real: set at the first paddle
   element (0xC6) and held until paddle hang ends (word space after the last
   paddled letter, whose echo is a space sent with BREAKIN still high), then
   0xC4 → 0xC0 within ~16 ms. Ours (`Keyer::paddleBreakIn()`) is sticky-
   until-read, so the host sees it for one status byte only.
8. **Break-in discards serial text** — the buffer is cleared at break-in and
   text arriving while paddling is ignored (only immediate commands are
   processed). Measured twice (TEST sent at 126.1 s and 192.7 s, never
   echoed). Datasheet p.4 "Paddle Input Priority".
9. **XOFF threshold** = buffer > 2/3 full of a 160-character buffer
   (datasheet p.1), i.e. ~107; measured at ~120-128 queued with a few
   already sent. Ours: > 480 of 512.

10. **No WK2 status mode.** Admin 11 (Set WK2 Mode) — accepted while the
   host is OPEN — answers at once with 0xC8 (a pushbutton status byte, none
   pressed). From then on bit 3 tags pushbutton bytes and KEYDOWN is no
   longer reported: key immediate gives 0xD0, 0xD4 instead of 0xD8, 0xDC.
   Host open always returns to WK1 mode. Ours ignores 0x0A/0x0B.
11. **KEYDOWN is tune only.** Sending text (PARIS, EEE, TEST at 12-20 WPM)
   produced BUSY and idle bytes only — never a KEYDOWN per element. KEYDOWN
   appeared solely for key immediate (0x0B). Ours reports every key
   transition (`Keyer::keyIsDown()`), roughly two status bytes per element on
   a 1200-baud link; `tools/wk-timing.py` treats one KEYDOWN per element as
   correct and needs the same correction.

## Behaviour confirmed (reference for 12a echo work)

- Host speed commands (0x02) do not produce a pot byte; pot bytes report the
  knob only, one per step, 0..WPMrange.

- Echo is sent after the letter has been completely sent (end of its last
  element), for serial and paddle alike. Source is told apart by BREAKIN.
- Status is sent unsolicited on every change (idle→BUSY on text arrival,
  BUSY→idle ~110 ms after the last echo at 20 WPM, lead/tail 0).
- Key immediate: 0xD8 then 0xDC (WAIT|KEYDOWN(|BUSY)), 0xC0 on release.
- Clear buffer: 0xC0 at once, then one late echo for the character that
  was in progress.
- PTT on (0x18) sets no status bit.
- Paddle break-in: 0xC6 (BUSY|BREAKIN); after release 0xC4 then 0xC0 at the
  end of the word space.
- 0x04 lead/tail units are 10 ms; tail delay = 3 dits + tail×10 ms.
- 0x09 PINCFG: bit0 PTT enable, bit1 sidetone enable, bit2 KeyOut2,
  bit3 KeyOut1; bits 7-6 ultimatic priority, 5-4 paddle hang time.
- 0x01 sidetone: WK2 = low-nibble N table (4000/N); WK3 mode = 62500/Hz.
