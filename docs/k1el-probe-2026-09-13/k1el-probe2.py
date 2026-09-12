#!/usr/bin/env python3
"""Read-only probe of a genuine K1EL WinKeyer. Sends no text, changes no settings."""
import serial, time, sys
PORT = "/dev/cu.usbserial-AI02BVHE"
T0 = time.monotonic()
log = open("k1el-probe2.log", "w")

def note(s):
    line = f"{time.monotonic()-T0:8.3f}  {s}"
    print(line); log.write(line + "\n"); log.flush()

def tx(ser, data, label):
    note(f"TX {data.hex(' ')}   # {label}")
    ser.write(data); ser.flush()

def rx(ser, secs, label="", want=None):
    buf = bytearray(); end = time.monotonic() + secs
    while time.monotonic() < end:
        b = ser.read(64)
        if b:
            buf += b
            if want and len(buf) >= want:
                time.sleep(0.15); buf += ser.read(64); break
    note(f"RX {bytes(buf).hex(' ') or '(nothing)'}   # {label}  [{len(buf)} bytes]")
    return bytes(buf)

ser = serial.Serial(PORT, 1200, bytesize=8, parity="N", stopbits=2, timeout=0.1)
note(f"opened {PORT} 1200 8N2")
rx(ser, 1.0, "unsolicited")
tx(ser, b"\x00\x04\x5a", "admin echo (link check)")
rx(ser, 1.0, "echo", want=1)
tx(ser, b"\x00\x07", "admin get values, HOST CLOSED")
rx(ser, 4.0, "get values standby")
tx(ser, b"\x00\x02", "admin host open")
rx(ser, 2.5, "version + settle")
tx(ser, b"\x00\x07", "admin get values, HOST OPEN after 2.5 s settle")
rx(ser, 5.0, "get values open")
tx(ser, b"\x15", "request status (still in sync?)")
rx(ser, 1.0, "status", want=1)
tx(ser, b"\x00\x03", "admin host close")
rx(ser, 3.0, "after close (late bytes?)")
ser.close(); note("closed")
