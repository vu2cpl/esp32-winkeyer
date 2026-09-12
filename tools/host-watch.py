#!/usr/bin/env python3
"""Watch what a logger does to the keyer's settings, over HTTP.

Why this exists: the console shares the serial wire with the host session,
so the one thing you cannot do while a logger is attached is watch the
keyer over that wire. /api/state answers over WiFi instead, and every
setting a host can touch is in it — including `pincfg` and `hostdef`, the
raw bytes of the two commands whose bit layout is not obvious from outside.

This is the tool that found, in one evening, that RUMlogNG sets PTT
lead/tail to 0, Farnsworth to 20, the sidetone to 1000 Hz and the pin
configuration to 0x00 on every session open (HANDOVER 12e, 12f). Those are
now recorded and ignored — run this against a new logger before assuming it
behaves.

    ./tools/host-watch.py --ip 192.168.10.209

Then open, close and re-open the logger's session. A logger sends its
defaults at session OPEN, so a setting that looks untouched mid-session may
still be overwritten the moment the session is re-established — close and
re-open before concluding anything.
"""
import argparse, json, time, urllib.request

# Settings a host can reach, plus the raw bytes of the awkward two.
FIELDS = ["host", "wpm", "farns", "weight", "ratio", "mode", "swap",
          "lead", "tail", "sthz", "ptt", "st", "potmin", "potmax",
          "modereg", "echo", "pecho", "pincfg", "hostdef"]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default="winkeyer.local",
                    help="keyer address; prefer the IP, mDNS has resolved stale here")
    ap.add_argument("--seconds", type=float, default=600)
    ap.add_argument("--interval", type=float, default=1.0)
    a = ap.parse_args()

    print(f"watching {a.ip} for {a.seconds:.0f}s — open, close and RE-OPEN the "
          f"logger's session\n")
    last, t0 = None, time.time()
    while time.time() - t0 < a.seconds:
        try:
            with urllib.request.urlopen(f"http://{a.ip}/api/state", timeout=4) as r:
                s = json.load(r)
        except Exception:
            time.sleep(a.interval); continue
        cur = {k: s.get(k) for k in FIELDS}
        if last is None:
            print(time.strftime("%H:%M:%S"), "baseline")
            for k, v in cur.items():
                print(f"    {k:<8} {v}")
            print()
        else:
            diff = {k: (last[k], v) for k, v in cur.items() if last[k] != v}
            if diff:
                stamp = time.strftime("%H:%M:%S")
                note = ""
                if "host" in diff:
                    note = "  <- session OPENED" if cur["host"] else "  <- session CLOSED"
                print(stamp, ", ".join(f"{k}: {o} -> {n}" for k, (o, n) in diff.items())
                      + note, flush=True)
        last = cur
        time.sleep(a.interval)
    print("\ndone")

if __name__ == "__main__":
    main()
