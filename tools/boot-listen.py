#!/usr/bin/env python3
"""Count ROM resets on the serial port — is the board boot-looping?

Listens at 115200 (the ROM loader's rate, whatever the app runs at) without
driving DTR/RTS, and reports every `rst:0x.. (REASON)` line plus how often
the bootloader handed off to the app (`entry 0x...`).

    POWERON_RESET repeating  -> supply collapsing (try another supply)
    SW_RESET repeating, no app output -> suspect stale flash: erase first
        (pio run -e esp32-winkeyer -t erase), then reflash, before
        retiring the board. This is what fixed the original ESP32.

Note: on a CP2102 devkit, macOS opening the port can still pulse the reset
line once — a single POWERON at the start is expected.

    tools/boot-listen.py /dev/cu.usbserial-0001 [seconds]
"""
import re, sys, time
import serial                            # pyserial

port = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 25
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.dtr = s.rts = False
s.open()
buf, t0 = b"", time.time()
while time.time() - t0 < secs:
    buf += s.read(4096)
s.close()
txt = buf.decode("latin1")
rsts = re.findall(r"rst:0x[0-9a-f]+ \([A-Z_]+\)", txt)
print(f"{secs:.0f} s: ROM resets {len(rsts)} | app hand-offs {txt.count('entry 0x')}")
for r in rsts[:5]:
    print("  ", r)
