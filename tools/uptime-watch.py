#!/usr/bin/env python3
"""
uptime-watch.py — watch the keyer over HTTP and report every restart.

Polls /api/state a few times a second and prints ONLY events: a restart
(uptime went backwards, with the reset reason the firmware recorded), an
outage and its length, and a stall (a reply that took longer than the
threshold). Every sample goes to the log file, so after the fact you can
line an event up against what the keyer was doing.

This is how the 2026-09-12 faults were pinned down: "the board died" is
useless, "uptime 225 -> 2 at 12:31:35, reason PANIC / exception" is not.
A restart reason of POWERON on a board nobody touched means something
outside the firmware — on a devkit, usually its USB port's control lines.

    ./uptime-watch.py                          # winkeyer.local
    ./uptime-watch.py --keyer 192.168.1.77 --log /tmp/keyer.log
    ./uptime-watch.py --stall 1.0              # stricter stall threshold

Pair it with flex-ptt-watch.py when a radio is involved: this one says
whether the keyer was alive, that one says whether the radio was keyed.
"""
import argparse
import json
import sys
import time
import urllib.request

ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
ap.add_argument("--keyer", default="winkeyer.local", help="host or IP")
ap.add_argument("--log", default="", help="sample log (default: no file)")
ap.add_argument("--interval", type=float, default=0.25, help="seconds between polls")
ap.add_argument("--stall", type=float, default=3.0, help="report replies slower than this")
ap.add_argument("--timeout", type=float, default=2.0, help="per-request timeout")
args = ap.parse_args()

URL = f"http://{args.keyer}/api/state"
log = open(args.log, "a", buffering=1) if args.log else None


def event(msg):
    line = f"{time.strftime('%H:%M:%S')} {msg}"
    print(line, flush=True)
    if log:
        log.write("EVENT " + line + "\n")


event(f"watching {args.keyer}")
last_up = None
down_since = None

while True:
    started = time.time()
    try:
        s = json.load(urllib.request.urlopen(URL, timeout=args.timeout))
        took = time.time() - started
        up = s["uptime"]
        if log:
            log.write(f"{time.strftime('%H:%M:%S')} up={up} reset={s['resetreason']} "
                      f"busy={int(s['busy'])} key={int(s['key'])} ptton={int(s['ptton'])} "
                      f"flex={int(s['flex']['connected'])} rssi={s['rssi']} "
                      f"rtt={took*1000:.0f}ms\n")
        if down_since is not None:
            event(f"BACK after {time.time()-down_since:.1f}s — uptime {up}s, "
                  f"reset reason '{s['resetreason']}'")
            down_since = None
        # +2 s of slack: uptime is whole seconds and polls are not aligned.
        elif last_up is not None and up + 2 < last_up:
            event(f"RESTARTED — uptime {last_up}s -> {up}s, "
                  f"reset reason '{s['resetreason']}'")
        if took > args.stall:
            event(f"STALL — /api/state took {took*1000:.0f} ms")
        last_up = up
    except KeyboardInterrupt:
        sys.exit(0)
    except Exception as e:
        if down_since is None:
            down_since = started
            event(f"NO REPLY ({type(e).__name__}) — last uptime {last_up}s")
    time.sleep(max(0, args.interval - (time.time() - started)))
