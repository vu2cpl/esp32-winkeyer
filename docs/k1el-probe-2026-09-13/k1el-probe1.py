#!/usr/bin/env python3
"""Read-only probe of a genuine K1EL WinKeyer. Sends no text, changes no settings."""
import serial, time, sys
PORT = "/dev/cu.usbserial-AI02BVHE"
T0 = time.monotonic()
log = open("k1el-probe1.log", "w")

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
note(f"opened {PORT} 1200 8N2 dtr={ser.dtr} rts={ser.rts}")
rx(ser, 1.5, "unsolicited after open")
tx(ser, b"\x00\x04\xa5", "admin echo 0xA5 (before host open)")
rx(ser, 1.0, "echo reply", want=1)
tx(ser, b"\x00\x02", "admin host open")
v = rx(ser, 2.0, "version byte + anything after", want=1)
if v: note(f"   version = {v[0]} (0x{v[0]:02x})")
rx(ser, 1.0, "settle after open")
tx(ser, b"\x15", "request status")
rx(ser, 1.0, "status", want=1)
tx(ser, b"\x07", "get speed pot")
rx(ser, 1.0, "pot", want=1)
tx(ser, b"\x00\x07", "admin get values")
gv = rx(ser, 3.0, "get values", want=15)
if len(gv) >= 15:
    note("   get values bytes: " + " ".join(f"{i}:{b}" for i, b in enumerate(gv[:15])))
tx(ser, b"\x00\x03", "admin host close")
rx(ser, 1.0, "after host close")
ser.close(); note("closed")
