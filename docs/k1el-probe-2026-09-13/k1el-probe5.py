#!/usr/bin/env python3
"""Operator-in-the-loop probe: pot bytes (0-120 s idle) then break-in while sending (120-240 s)."""
import serial, time
PORT = "/dev/cu.usbserial-AI02BVHE"
T0 = time.monotonic()
log = open("k1el-probe5.log", "w")
def note(s):
    line = f"{time.strftime('%H:%M:%S')} {time.monotonic()-T0:8.3f}  {s}"; print(line, flush=True); log.write(line+"\n"); log.flush()
def dec(b):
    if b & 0xC0 == 0xC0:
        f = [n for bit, n in ((0x10,"WAIT"),(0x08,"KEYDOWN"),(0x04,"BUSY"),(0x02,"BREAKIN"),(0x01,"XOFF")) if b & bit]
        return f"STATUS {'|'.join(f) or 'idle'}"
    if b & 0xC0 == 0x80: return f"POT {b&0x3f}"
    return f"ECHO {chr(b)!r}" if 32 <= b < 127 else "BYTE"
ser = serial.Serial(PORT, 1200, bytesize=8, parity="N", stopbits=2, timeout=0.005)
def tx(data, label):
    note(f"TX {data.hex(' ')}   # {label}"); ser.write(data); ser.flush()
def poll():
    b = ser.read(1)
    if b: note(f"   RX {b[0]:02x}  {dec(b[0])}")
    return b
tx(b"\x00\x02", "host open")
end = time.monotonic() + 1.0
while time.monotonic() < end: poll()
tx(b"\x0e\x44", "mode reg 0x44: paddle echo + serial echo, iambic B")
tx(b"\x05\x0a\x1e\x00", "pot setup: min 10 WPM, range 30")
tx(b"\x04\x00\x00", "lead/tail 0")
tx(b"\x07", "get pot (baseline)")
note("===== PHASE A (0-120 s): turn the POT; tap the paddle idle =====")
end = T0 + 120
while time.monotonic() < end: poll()
note("===== PHASE B (120-240 s): keyer sends TEST at 12 WPM; squeeze paddle to break in =====")
tx(b"\x02\x0c", "12 WPM")
end = T0 + 240; last_fill = 0
while time.monotonic() < end:
    poll()
    if time.monotonic() - last_fill > 6:
        tx(b"TEST ", "text"); last_fill = time.monotonic()
tx(b"\x0a", "clear buffer")
end = time.monotonic() + 1.5
while time.monotonic() < end: poll()
tx(b"\x00\x03", "host close"); time.sleep(0.5); ser.close(); note("closed")
