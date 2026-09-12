#!/usr/bin/env python3
"""
console-capture.py — timestamped capture of the keyer's console.

Opens a serial port read-only, WITHOUT touching DTR/RTS, and timestamps
every line into a file. Two reasons it exists:

1. **It does not reset the board.** pyserial asserts DTR on open by
   default, and on a devkit those lines reach EN — opening the port to
   "have a look" is what resets the thing you are trying to observe.
2. **It catches a panic.** A crash dump is printed once and then gone.
   Holding the port through a reproduction attempt is the difference
   between "assert failed: pbuf_free" and a full backtrace you can feed to
   addr2line.

    ./console-capture.py /dev/cu.usbserial-0001 --log /tmp/keyer.log
    ./console-capture.py /dev/cu.usbserial-A9M9DV3R --baud 115200

Defaults to the WinKeyer rate (1200 8N2). At that rate the firmware keeps
the console muted, because a logger owns the port — so set `/baud 115200`
(web page or POST /api/set?k=baud&v=115200) before hunting a crash, and
capture at 115200 8N1.

Decoding a backtrace afterwards:

    xtensa-esp32-elf-addr2line -pfiaC \\
      -e .pio/build/esp32-winkeyer/firmware.elf <addresses>

The ELF must be the one that was running: check the "ELF file SHA256" the
panic prints against the build you still have.
"""
import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("needs pyserial:  pip install pyserial  (PlatformIO's venv has it)")

ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
ap.add_argument("port", help="serial device, e.g. /dev/cu.usbserial-0001")
ap.add_argument("--baud", type=int, default=1200)
ap.add_argument("--log", default="", help="write here as well as to stdout")
args = ap.parse_args()

p = serial.Serial()
p.port = args.port
p.baudrate = args.baud
p.bytesize = 8
p.parity = "N"
# 1200 is the WinKeyer rate and 8N2 with it; console rates are 8N1.
p.stopbits = 2 if args.baud == 1200 else 1
p.timeout = 0.2
# BEFORE open: this is the whole point — see the docstring.
p.dtr = False
p.rts = False
p.open()

out = open(args.log, "a", buffering=1) if args.log else None
banner = (f"=== capture started {time.strftime('%H:%M:%S')} "
          f"({args.baud} {'8N2' if p.stopbits == 2 else '8N1'}) ===")
print(banner, flush=True)
if out:
    out.write(banner + "\n")

buf = b""
try:
    while True:
        data = p.read(256)
        if not data:
            continue
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            stamped = f"{time.strftime('%H:%M:%S')} {line.decode(errors='replace').rstrip()}"
            print(stamped, flush=True)
            if out:
                out.write(stamped + "\n")
except KeyboardInterrupt:
    pass
finally:
    p.close()
