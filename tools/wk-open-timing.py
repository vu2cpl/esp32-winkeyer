#!/usr/bin/env python3
# Usage: KEYER=<ip> python3 tools/wk-open-timing.py   (port must be free: close the logger first)
"""Time the WinKeyer admin-open handshake on the keyer's USB port.

Phase A: open the port as a logger does (1200 8N2), watch for a reboot.
Phase B: one 00 02 at a time, time each 0x17 reply (until answered, then 20).
Phase C: flood 00 02 as fast as the OS takes it until the first reply, count
         what was queued, and time how long the keyer keeps receiving opens.
Phase D: 00 03 host close.
"""
import json, threading, time, urllib.request
import serial

PORT = "/dev/cu.usbserial-0001"
import os
K = "http://" + os.environ.get("KEYER", "vukeyer.local")
T0 = time.monotonic()
def t(): return (time.monotonic() - T0) * 1000.0
def log(msg): print(f"{t():9.1f} ms  {msg}", flush=True)

# ── uptime watcher: detects a reboot caused by opening the port ──
ups = []
stop = False
def watch():
    while not stop:
        try:
            st = json.load(urllib.request.urlopen(K + "/api/state", timeout=1))
            ups.append((t(), st["uptime"]))
        except Exception:
            ups.append((t(), None))
        time.sleep(0.25)
threading.Thread(target=watch, daemon=True).start()
time.sleep(1.0)
log(f"before open: keyer uptime {ups[-1][1]}")

# ── A ──
s = serial.Serial(PORT, 1200, bytesize=8, parity="N", stopbits=2, timeout=0)
log(f"port open (dtr={s.dtr} rts={s.rts})")

def read_for(ms):
    end = time.monotonic() + ms / 1000
    got = b""
    while time.monotonic() < end:
        b = s.read(64)
        if b: got += b
        else: time.sleep(0.001)
    return got

# ── B: single opens until answered ──
first_ok = None
for i in range(80):                       # up to ~40 s
    s.write(b"\x00\x02"); s.flush()
    sent = t()
    end = time.monotonic() + 0.5
    reply = b""; rt = None
    while time.monotonic() < end:
        b = s.read(64)
        if b:
            if rt is None: rt = t() - sent
            reply += b
            if b"\x17" in reply: break
        else:
            time.sleep(0.001)
    if b"\x17" in reply:
        log(f"open #{i+1}: 0x17 after {rt:.1f} ms (bytes {reply.hex(' ')})")
        first_ok = t(); break
    log(f"open #{i+1}: no 0x17 in 500 ms (got {reply[:24].hex(' ') or 'nothing'})")
if first_ok is None:
    log("never answered"); stop = True; s.close(); raise SystemExit(1)

read_for(300)
lat = []
for i in range(20):
    s.reset_input_buffer()
    s.write(b"\x00\x02"); s.flush()
    sent = t(); rt = None
    end = time.monotonic() + 0.5
    while time.monotonic() < end:
        b = s.read(64)
        if b and b"\x17" in b: rt = t() - sent; break
        if not b: time.sleep(0.0005)
    lat.append(rt)
    time.sleep(0.1)
ok = [x for x in lat if x is not None]
log(f"20 clean round trips: {len(ok)} answered, min {min(ok):.1f} / median {sorted(ok)[len(ok)//2]:.1f} / max {max(ok):.1f} ms")
log("  (2 bytes out at 1200 8N2 = 18.3 ms on the wire, 1 byte back = 9.2 ms)")

# ── C: flood like a logger that does not wait ──
read_for(500); s.reset_input_buffer()
writes = 0; t_start = t(); t_first = None
while t() - t_start < 5000:
    s.write(b"\x00\x02"); writes += 1      # timeout=0 read, no flush: as fast as the OS takes it
    b = s.read(64)
    if b and b"\x17" in b:
        t_first = t(); break
log(f"flood: {writes} opens written in {t_first - t_start if t_first else 5000:.0f} ms before the first 0x17")
replies = 0; last = t()
while t() - last < 1500:                   # keep counting until the replies stop for 1.5 s
    b = s.read(256)
    if b:
        replies += b.count(b"\x17"); last = t()
    else:
        time.sleep(0.005)
log(f"flood drained: {replies} more replies, last one {last - t_first:.0f} ms after the first")
log(f"  i.e. {writes} x 18.3 ms = {writes * 18.33 / 1000:.1f} s expected for a pure 1200-baud backlog")

# ── D ──
s.write(b"\x00\x03"); s.flush(); time.sleep(0.1)
s.close()
log("host close sent, port closed")
stop = True
time.sleep(0.3)
reboots = [(a, b) for (a, b), (c, d) in zip(ups, ups[1:]) if b is not None and d is not None and d < b]
gaps = [a for a, b in ups if b is None]
log(f"uptime samples {len(ups)}; reboot seen: {'yes at %.0f ms' % reboots[0][0] if reboots else 'no'}; "
    f"unreachable samples {len(gaps)}; final uptime {ups[-1][1]}")
