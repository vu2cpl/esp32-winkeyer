#!/usr/bin/env python3
"""wk-trace-check.py — did the keyer echo every character the logger sent?

    tools/wk-trace-check.py                          # winkeyer.local
    tools/wk-trace-check.py http://192.168.10.20/api/wktrace
    tools/wk-trace-check.py saved-trace.txt

Fetches /api/wktrace (the last 1024 host bytes, both directions) and checks
every text byte the host sent was echoed.

Parses the host->keyer stream as WinKeyer commands (so parameter bytes are not
mistaken for text), queues each text byte, and matches keyer->host echoes in
order. Prints each message with any character that never came back, plus the
raw bytes around it.
"""
import sys, urllib.request
IMM = [0,1,1,1,2,3,1,0,0,1,0,1,1,1,1,15,1,1,1,0,1,0,1,1,1,1,1,2,1,1,0,0]
ADM = {0x04:1, 0x0D:256, 0x0E:1, 0x0F:1, 0x13:2, 0x16:1, 0x19:1}
src = sys.argv[1] if len(sys.argv) > 1 else "http://winkeyer.local/api/wktrace"
txt = open(src).read() if not src.startswith("http") else urllib.request.urlopen(src, timeout=5).read().decode()
pass
ents = []
for L in txt.splitlines():
    if L.startswith("#") or not L.strip(): continue
    p = L.split()
    ents.append((int(p[0]), p[1], int(p[2], 16)))
need, admin_sub = 0, False
pending = []          # (index in ents, char) queued text awaiting echo
missing = []
sent_text = []
for i, (ms, d, b) in enumerate(ents):
    if d == "H>K":
        if need: need -= 1; continue
        if admin_sub: admin_sub = False; need = ADM.get(b, 0); continue
        if b == 0x00: admin_sub = True; continue
        if b < 0x20: need = IMM[b]; continue
        pending.append((i, chr(b))); sent_text.append((ms, chr(b)))
    else:
        if b & 0xC0 in (0x80, 0xC0): continue
        c = chr(b)
        # match the oldest queued char equal to c; anything older is missing
        for j, (qi, qc) in enumerate(pending):
            if qc == c:
                for mi, mc in pending[:j]:
                    missing.append((mi, mc))
                pending = pending[j+1:]
                break
print(f"text bytes from host: {len(sent_text)}   never echoed: {len(missing)}   still queued at end: {len(pending)}")
for mi, mc in missing:
    ms = ents[mi][0]
    print(f"\n--- missing {mc!r} sent at {ms} ms; bytes within ±1.5 s:")
    for ms2, d, b in ents:
        if abs(ms2 - ms) <= 1500:
            ch = chr(b) if 0x20 <= b < 0x7F else "."
            print(f"   {ms2:>9} {d} {b:02X} {ch}{'   <== this one' if (ms2, d, b) == ents[mi] else ''}")
