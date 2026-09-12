#!/usr/bin/env python3
"""Status + echo timing probe of a genuine K1EL WK3.1. Keys its outputs (unconnected)."""
import serial, time
PORT = "/dev/cu.usbserial-AI02BVHE"
T0 = time.monotonic()
log = open("k1el-probe3.log", "w")
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
        if b: note(f"   RX {b[0]:02x}  {dec(b[0])}")
tx(b"\x00\x02", "host open"); watch(1.5, "version")
tx(b"\x0e\x44", "mode reg 0x44: paddle echo + serial echo, iambic B"); watch(0.5, "")
tx(b"\x02\x14", "speed 20 WPM"); watch(0.5, "")
tx(b"\x04\x00\x00", "PTT lead/tail 0 (so status timing is keying only)"); watch(0.5, "")
tx(b"\x15", "status idle"); watch(0.5, "")
tx(b"PARIS E", "text PARIS E"); watch(0.7, "early in send")
tx(b"\x15", "status while sending"); watch(6.0, "rest of send + idle")
tx(b"\x04\x05\x0a", "PTT lead 50 ms / tail 100 ms"); watch(0.3, "")
tx(b"EE", "text EE with lead/tail"); watch(2.5, "lead/tail status")
tx(b"\x18\x01", "PTT on"); watch(0.8, ""); tx(b"\x15", "status PTT on"); watch(0.5, "")
tx(b"\x18\x00", "PTT off"); watch(0.8, "")
tx(b"TEST " * 30, "150 bytes to fill buffer"); watch(3.0, "buffer high-water / XOFF")
tx(b"\x15", "status buffer loaded"); watch(0.6, "")
tx(b"\x0a", "clear buffer"); watch(2.0, "after clear")
tx(b"\x15", "status after clear"); watch(0.6, "")
tx(b"\x00\x03", "host close"); watch(1.0, "")
ser.close(); note("closed")
