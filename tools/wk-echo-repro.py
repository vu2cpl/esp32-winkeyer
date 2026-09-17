#!/usr/bin/env python3
"""wk-echo-repro.py — send a message repeatedly; check every letter is echoed.

    KEYER=192.168.10.20 tools/wk-echo-repro.py
    MSG="cq test vu2cpl" tools/wk-echo-repro.py

Opens a WinKeyer host session over TCP 8088 with RUMlogNG's mode register
(0x47) at 26 WPM, sends MSG 8 times paced like a 1200-baud logger and 4 times
as a burst, and prints OK/DROP per send. Writes echo-repro.log in the current
directory. It KEYS THE RADIO on the Flex backend, and a logger holding the
serial port loses its session to it — close the logger first.
"""
import socket, time, sys
import os; H = os.environ.get("KEYER", "vukeyer.local"); MSG = os.environ.get("MSG", "cq cq vu2cpl vu2cpl k")
T0 = time.monotonic(); log = open("echo-repro-web.log", "w")
def note(s):
    line = f"{time.monotonic()-T0:8.3f}  {s}"; log.write(line + "\n"); log.flush()
s = socket.create_connection((H, 8088), timeout=5)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
def rx_until_idle(maxs=40):
    echoes, last_rx, seen_busy, t_end = "", time.monotonic(), False, time.monotonic() + maxs
    while time.monotonic() < t_end:
        s.settimeout(0.05)
        try: data = s.recv(64)
        except socket.timeout: data = b""
        for b in data:
            if b & 0xC0 == 0xC0:
                note(f"RX {b:02x} status"); seen_busy |= bool(b & 0x04)
                if b == 0xC0 and seen_busy: last_rx = time.monotonic()
            elif b & 0xC0 == 0x80: note(f"RX {b:02x} pot")
            else: echoes += chr(b); note(f"RX {b:02x} echo {chr(b)!r}")
        if data: last_rx = time.monotonic()
        if seen_busy and time.monotonic() - last_rx > 2.0: break
    return echoes
def tx(data, pace):
    note(f"TX {data!r} pace={pace}")
    if pace:
        for ch in data: s.sendall(bytes([ch])); time.sleep(pace)
    else: s.sendall(data)
tx(b"\x00\x02", 0); time.sleep(1); s.settimeout(0.5)
try: note(f"version {s.recv(8).hex()}")
except socket.timeout: pass
tx(b"\x0e\x47", 0); tx(b"\x02\x1a", 0); time.sleep(0.5)
results = []
for rep in range(12):
    pace = 0.0092 if rep < 8 else 0
    note(f"===== rep {rep} pace={pace}")
    tx(MSG.encode(), pace)
    got = rx_until_idle()
    ok = got.replace(" ", "").upper() == MSG.replace(" ", "").upper() and got.strip().upper() == MSG.upper()
    results.append((rep, pace, got, ok))
    print(f"rep {rep:2d} pace={pace:<6} {'OK  ' if ok else 'DROP'} echo={got!r}", flush=True)
    time.sleep(1.5)
tx(b"\x00\x03", 0); s.close()
print("drops:", sum(1 for r in results if not r[3]), "of", len(results))
