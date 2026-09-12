#!/usr/bin/env python3
"""
wk-timing.py — timestamp every WinKeyer status byte.

Counting status bytes inside fixed drain windows gives false negatives: a
delayed burst lands outside the window it belongs to and a perfectly good
send looks like it keyed one element. Timestamps distinguish "not sent"
from "reported late" immediately — compare the BUSY span against the
text's expected duration.

    ./wk-timing.py                            # winkeyer.local
    ./wk-timing.py --host 192.168.10.20 --wpm 25 --text "CQ TEST"

A correct run shows NO KEYDOWN bytes — a genuine K1EL reports KEYDOWN for
tune only, never per element (measured 2026-09-13, docs/k1el-probe-2026-09-13)
— and a BUSY span close to the predicted duration.
"""

import argparse
import socket
import time

ELEMENTS = {
    'A': 2, 'B': 4, 'C': 4, 'D': 3, 'E': 1, 'F': 4, 'G': 3, 'H': 4,
    'I': 2, 'J': 4, 'K': 3, 'L': 4, 'M': 2, 'N': 2, 'O': 3, 'P': 4,
    'Q': 4, 'R': 3, 'S': 3, 'T': 1, 'U': 3, 'V': 4, 'W': 3, 'X': 4,
    'Y': 4, 'Z': 4, '0': 5, '1': 5, '2': 5, '3': 5, '4': 5, '5': 5,
    '6': 5, '7': 5, '8': 5, '9': 5, '/': 5, '?': 6, '.': 6, ',': 6,
}


def expected(text):
    """Elements, and rough duration in dit units including spacing."""
    n = sum(ELEMENTS.get(c.upper(), 0) for c in text)
    units = 0
    for c in text.upper():
        if c == ' ':
            units += 4
        else:
            e = ELEMENTS.get(c, 0)
            units += e * 2 + 2      # crude: mark+gap per element, plus char gap
    return n, units


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="winkeyer.local")
    ap.add_argument("--port", type=int, default=8088)
    ap.add_argument("--wpm", type=int, default=25)
    ap.add_argument("--text", default="TEST")
    args = ap.parse_args()

    n_elem, units = expected(args.text)
    predicted = units * (1200.0 / args.wpm) / 1000.0

    s = socket.create_connection((args.host, args.port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.settimeout(0.2)
    t0 = time.time()
    span = {}
    keydowns = 0

    def drain(secs):
        nonlocal keydowns
        end = time.time() + secs
        while time.time() < end:
            try:
                data = s.recv(256)
            except socket.timeout:
                continue
            for b in data:
                t = time.time() - t0
                if b & 0xC0 == 0xC0:
                    f = []
                    if b & 0x10: f.append("WAIT")
                    if b & 0x08: f.append("KEYDOWN")
                    if b & 0x04: f.append("BUSY")
                    if b & 0x02: f.append("BREAKIN")
                    if b & 0x01: f.append("XOFF")
                    state = "|".join(f) or "idle"
                    print(f"{t:7.3f}s  status {state}")
                    if b & 0x08:
                        keydowns += 1
                    if b & 0x04:
                        span.setdefault("start", time.time())
                    elif state == "idle" and "start" in span:
                        span.setdefault("end", time.time())
                elif b & 0xC0 == 0x80:
                    print(f"{t:7.3f}s  pot={b & 0x3F}")
                else:
                    print(f"{t:7.3f}s  byte 0x{b:02X}")

    s.sendall(bytes([0x00, 0x02]))          # host open
    drain(1.5)
    s.sendall(bytes([0x02, args.wpm]))      # speed
    drain(1.0)

    print(f"\n--- sending {args.text!r} ---")
    s.sendall(args.text.upper().encode("ascii", "ignore"))
    drain(max(8.0, predicted * 2 + 4))
    s.sendall(bytes([0x00, 0x03]))          # host close
    drain(0.5)
    s.close()

    print()
    print(f"elements sent: {n_elem}   KEYDOWN bytes: {keydowns}"
          f"   {'OK (tune only, as a real WinKeyer)' if keydowns == 0 else 'WRONG — real WK sends none for text'}")
    if "start" in span and "end" in span:
        held = span["end"] - span["start"]
        print(f"BUSY held {held:.2f}s   predicted ~{predicted:.2f}s at {args.wpm} wpm")
    else:
        print("BUSY span incomplete — status arrived late or not at all")


if __name__ == "__main__":
    main()
