#!/usr/bin/env python3
"""Status + echo timing probe of a genuine K1EL WK3.1. Keys its outputs (unconnected)."""
import serial, time
PORT = "/dev/cu.usbserial-AI02BVHE"
T0 = time.monotonic()
log = open("k1el-probe4.log", "w")
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
    end = time.monotonic() + secs; got = []
    while time.monotonic() < end:
        b = ser.read(1)
        if b: note(f"   RX {b[0]:02x}  {dec(b[0])}"); got.append(b[0])
    return got
tx(b"\x00\x02", "host open"); watch(1.0, "version")
tx(b"\x0e\x04", "mode reg 0x04: serial echo only"); watch(0.3, "")
tx(b"\x04\x00\x00", "lead/tail 0"); watch(0.3, "")
# KEYDOWN bit via key immediate
tx(b"\x0b\x01", "key immediate ON"); watch(0.5, "unsolicited?")
tx(b"\x15", "status key down"); watch(0.4, "")
tx(b"\x0b\x00", "key immediate OFF"); watch(0.5, "")
tx(b"\x15", "status key up"); watch(0.4, "")
# WAIT bit via pause
tx(b"\x02\x0a", "10 WPM"); watch(0.3, "")
tx(b"MMMM", "text MMMM"); watch(0.8, "")
tx(b"\x06\x01", "pause ON"); watch(1.5, "while paused")
tx(b"\x15", "status paused"); watch(0.4, "")
tx(b"\x06\x00", "pause OFF"); watch(0.6, "")
tx(b"\x15", "status unpaused"); watch(0.3, "")
tx(b"\x0a", "clear buffer"); watch(1.5, "")
# XOFF threshold: 5 WPM, feed 8 bytes at a time with status after each chunk
tx(b"\x02\x05", "5 WPM"); watch(0.3, "")
sent = 0; xoff_at = None
for i in range(45):
    tx(b"EEEEEEEE", f"chunk {i}, total {sent+8}"); sent += 8
    got = watch(0.25, "")
    if any(b & 0xC1 == 0xC1 for b in got):
        xoff_at = sent; note(f"== XOFF seen after {sent} bytes queued"); break
tx(b"\x15", "status at threshold"); watch(0.4, "")
tx(b"\x0a", "clear buffer"); watch(1.5, "clear at XOFF")
tx(b"\x15", "status after clear"); watch(0.4, "")
tx(b"\x00\x03", "host close"); watch(0.6, "")
ser.close(); note(f"closed; xoff_at={xoff_at}")
