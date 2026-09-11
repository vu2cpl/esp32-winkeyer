#!/usr/bin/env python3
"""Log the keyer and the FlexRadio on one clock — for stuck/late PTT hunts.

Polls the keyer's /api/state at 5 Hz and prints every change of busy / key /
ptton (local PTT line) / xmit (radio PTT we hold). At the same time opens a
READ-ONLY SmartSDR API session to the radio and prints interlock state
changes (READY / TRANSMITTING src=...) and cwx progress (sent=N).

Nothing is sent to the radio except subscriptions: it cannot key anything.
This is how the 2026-09-11 memory-PTT report was checked — the radio unkeyed
0.67 s after its last character every time (its own CWX break_in_delay).

    tools/flex-ptt-watch.py --keyer 192.168.10.20 --radio 192.168.1.50
"""
import argparse, json, re, socket, threading, time, urllib.request

ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
ap.add_argument("--keyer", required=True, help="keyer IP or winkeyer.local")
ap.add_argument("--radio", required=True, help="radio IP (API on TCP 4992)")
ap.add_argument("--minutes", type=float, default=20)
args = ap.parse_args()
T0 = time.time()
END = T0 + args.minutes * 60


def out(tag, msg):
    print(f"{time.strftime('%H:%M:%S')} {time.time()-T0:7.2f}  {tag}  {msg}",
          flush=True)


def keyer():
    last = None
    while time.time() < END:
        try:
            s = json.load(urllib.request.urlopen(
                f"http://{args.keyer}/api/state", timeout=2))
            cur = (f"busy={int(s['busy'])} key={int(s['key'])} "
                   f"ptton={int(s['ptton'])} xmit={int(s['flex']['xmiton'])} "
                   f"wpm={s['wpm']}")
        except Exception as e:          # a stalled loop() shows up here
            cur = f"err {type(e).__name__}"
        if cur != last:
            out("KEYER", cur)
            last = cur
        time.sleep(0.2)


def radio():
    s = socket.create_connection((args.radio, 4992), timeout=5)
    s.settimeout(1.0)
    for i, c in enumerate(["sub tx all", "sub cwx all", "sub radio all"], 1):
        s.sendall(f"C{i}|{c}\n".encode())
    buf, last = b"", None
    while time.time() < END:
        try:
            d = s.recv(4096)
            if not d:
                out("RADIO", "closed")
                return
            buf += d
        except socket.timeout:
            continue
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            L = line.decode("latin1").strip()
            if "interlock" in L:
                m = re.search(r"state=(\S+)", L)
                st = m.group(1) if m else "?"
                if st != last:
                    src = re.search(r"source=(\S*)", L)
                    out("RADIO", f"interlock {st} src={src.group(1) if src else ''}")
                    last = st
            elif "|cwx " in L:
                out("RADIO", L.split("|", 1)[1][:120])


threading.Thread(target=keyer, daemon=True).start()
radio()
