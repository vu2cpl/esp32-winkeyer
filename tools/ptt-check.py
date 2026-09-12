#!/usr/bin/env python3
"""Check PTT sequencing and the STOP paths over HTTP — no paddle, no logger.

Why this exists: the faults it covers are invisible from the outside. A PTT
line that comes up and drops one millisecond into the lead-in still looks
like "the LED flickered", and a STOP that only stops tune looks like a STOP.
Both were real here (HANDOVER 12c, 12d). Polling /api/state at 10 Hz while
driving the keyer through the same API answers them in seconds.

It switches to the LOCAL backend for the duration and restores the backend
it found. On the local backend the keyer drives GPIO33/32 only, so nothing
reaches the FlexRadio and nothing goes on air — but it DOES key whatever is
wired to those pins, so use it into a dummy load or with nothing attached.

    ./tools/ptt-check.py --ip 192.168.10.209

Checks, in order:
  lead-in   an over started after a long idle keeps PTT for the whole over.
            This is the one that needs the 15 s wait: the backstop that used
            to break it measures from the last element keyed.
  stop      a message in flight stops, and PTT releases after the tail.
  tune      tune keys down, and STOP ends it.
  memory    a memory reports itself in memplay and stops.
"""
import argparse, json, sys, time, urllib.parse, urllib.request

def api(ip, path, data=None, timeout=6):
    url = f"http://{ip}{path}"
    if data is None:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return json.load(r)
    body = urllib.parse.urlencode(data).encode()
    with urllib.request.urlopen(url, body, timeout=timeout) as r:
        return r.read().decode().strip()

def state(ip):   return api(ip, "/api/state")
def post(ip, p, d): return api(ip, p, d)

def watch(ip, secs, hz=10):
    """Sample the keyer's live lines. Returns [(t, ptton, key, busy), ...]."""
    rows, t0 = [], time.time()
    while time.time() - t0 < secs:
        try:
            s = state(ip)
            rows.append((round(time.time() - t0, 2), s["ptton"], s["key"], s["busy"]))
        except Exception:
            pass
        time.sleep(1.0 / hz)
    return rows

def span(rows, field=1):
    on = [r for r in rows if r[field] is True]
    return (on[0][0], on[-1][0]) if on else None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default="winkeyer.local",
                    help="keyer address; prefer the IP, mDNS has resolved stale here")
    ap.add_argument("--idle", type=float, default=15.0,
                    help="seconds to wait before the lead-in check (must exceed "
                         "the 10 s PTT backstop window)")
    a = ap.parse_args()
    ip = a.ip

    s = state(ip)
    backend, lead, tail = s["backend"], s["lead"], s["tail"]
    print(f"keyer {ip}: backend {backend}, lead {lead} ms, tail {tail} ms, "
          f"{s['wpm']} WPM")
    if backend != "local":
        print("  switching to the local backend for the test")
        post(ip, "/api/set", {"k": "backend", "v": "local"}); time.sleep(0.5)

    fails = []
    try:
        print("\n[1] priming over — stamps the last-element time")
        post(ip, "/api/send", {"t": "V"}); watch(ip, 3)

        print(f"[2] idle {a.idle:.0f} s, longer than the 10 s PTT backstop window")
        time.sleep(a.idle)

        print("[3] lead-in: the over that used to go out with PTT down")
        post(ip, "/api/send", {"t": "TEST"})
        rows = watch(ip, 7)
        sp = span(rows)
        if not sp:
            fails.append("PTT never came up for the over")
        elif sp[0] > 0.5:
            fails.append(f"PTT came up late ({sp[0]}s) — lead-in not honoured")
        elif not any(r[2] for r in rows):
            fails.append("no elements keyed during the over")
        else:
            gaps = [r for r in rows if r[0] < sp[1] and r[1] is not True]
            if gaps:
                fails.append(f"PTT dropped mid-over at {gaps[0][0]}s")
        print(f"    PTT up {sp[0]}s → {sp[1]}s" if sp else "    PTT never up")

        print("[4] stop: a message in flight")
        post(ip, "/api/send", {"t": "CQ CQ CQ DE VU2CPL VU2CPL K"}); time.sleep(1.5)
        if not state(ip)["busy"]:
            fails.append("keyer was not busy 1.5 s into a long message")
        post(ip, "/api/send", {"stop": "1"}); time.sleep(0.4)
        if state(ip)["busy"]:
            fails.append("STOP did not end the message")
        time.sleep(tail / 1000.0 + 0.4)
        if state(ip)["ptton"]:
            fails.append("PTT still up after the tail following STOP")
        print("    stopped, PTT released after the tail")

        print("[5] tune, and STOP ending it")
        post(ip, "/api/tune", {"v": "on"}); time.sleep(0.5)
        s = state(ip)
        if not (s["tune"] and s["key"]):
            fails.append("tune did not key down")
        post(ip, "/api/send", {"stop": "1"}); time.sleep(0.5)
        s = state(ip)
        if s["tune"] or s["key"] or s["ptton"]:
            fails.append("STOP did not end tune")
        print("    tune keyed down and stopped")

        print("[6] memory: plays, reports itself, stops")
        r = post(ip, "/api/mem", {"play": "1"})
        if "playing" not in r:
            print(f"    skipped — {r}")
        else:
            time.sleep(1.2)
            if state(ip).get("memplay") != 1:
                fails.append("memplay did not report the slot being played")
            post(ip, "/api/send", {"stop": "1"}); time.sleep(0.5)
            if state(ip).get("memplay"):
                fails.append("memplay did not clear after STOP")
            print("    played, reported in memplay, stopped")
    finally:
        if backend != "local":
            print(f"\nrestoring the {backend} backend")
            post(ip, "/api/set", {"k": "backend", "v": backend})
            time.sleep(1.5)

    print()
    if fails:
        for f in fails: print("FAIL:", f)
        sys.exit(1)
    print("all checks passed")

if __name__ == "__main__":
    main()
