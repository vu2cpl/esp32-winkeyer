#!/usr/bin/env python3
"""Admin replies, WK2-mode status, pause vs clear buffer."""
import serial, time
PORT = "/dev/cu.usbserial-AI02BVHE"
T0 = time.monotonic()
log = open("k1el-probe6.log", "w")
def note(s):
    line = f"{time.monotonic()-T0:8.3f}  {s}"; print(line); log.write(line+"\n"); log.flush()
def dec(b):
    if b & 0xC0 == 0xC0: return f"STATUS {b:08b} (low5={b&0x1f:05b})"
    if b & 0xC0 == 0x80: return f"POT {b&0x3f}"
    return f"ECHO {chr(b)!r}" if 32 <= b < 127 else f"BYTE"
ser = serial.Serial(PORT, 1200, bytesize=8, parity="N", stopbits=2, timeout=0.005)
def tx(data, label):
    note(f"TX {data.hex(' ')}   # {label}"); ser.write(data); ser.flush()
def watch(secs, label):
    note(f"-- watch {secs}s: {label}")
    end = time.monotonic() + secs
    while time.monotonic() < end:
        b = ser.read(1)
        if b: note(f"   RX {b[0]:02x}  {b[0]:3d}  {dec(b[0])}")
def sync(tag):
    tx(b"\x00\x04" + tag, f"echo test {tag!r} — in sync?"); watch(0.6, "")
# Read-only admin replies, host CLOSED
for sub, name in ((0x09, "get FW major rev"), (0x17, "get FW minor rev"),
                  (0x18, "get IC type"), (0x15, "read back Vcc")):
    tx(bytes([0, sub]), f"admin {sub} (0x{sub:02x}) {name}"); watch(1.0, name); sync(b"S")
# WK1 mode (default after open): key immediate status
tx(b"\x00\x02", "host open"); watch(1.0, "version")
tx(b"\x0b\x01", "WK1 mode: key immediate ON"); watch(0.5, ""); tx(b"\x0b\x00", "OFF"); watch(0.5, "")
# WK2 mode set while OPEN
tx(b"\x00\x0b", "admin 11 set WK2 mode (host open)"); watch(0.8, "reply?")
tx(b"\x0b\x01", "WK2 mode: key immediate ON"); watch(0.5, ""); tx(b"\x15", "status"); watch(0.4, "")
tx(b"\x0b\x00", "OFF"); watch(0.5, "")
tx(b"\x0e\x04", "serial echo"); tx(b"\x04\x00\x00", "lead/tail 0"); tx(b"\x02\x14", "20 WPM"); watch(0.3, "")
tx(b"EEE", "WK2 mode: text EEE"); watch(1.5, "busy status in WK2 mode")
# Pause then clear buffer: does clear cancel pause?
tx(b"\x02\x0a", "10 WPM"); tx(b"MMMM", "text MMMM"); watch(0.5, "")
tx(b"\x06\x01", "pause ON"); watch(1.5, ""); tx(b"\x0a", "clear buffer (should cancel pause)"); watch(0.8, "")
tx(b"E", "text E after clear — sent if pause cancelled"); watch(1.5, "")
tx(b"\x06\x00", "pause OFF (cleanup)"); watch(0.5, "")
tx(b"\x00\x03", "host close"); watch(0.8, "")
# Re-open: is WK1 mode restored?
tx(b"\x00\x02", "host open again"); watch(1.0, "version")
tx(b"\x0b\x01", "after re-open: key immediate ON"); watch(0.5, ""); tx(b"\x0b\x00", "OFF"); watch(0.5, "")
tx(b"\x00\x03", "host close"); watch(0.5, "")
ser.close(); note("closed")
