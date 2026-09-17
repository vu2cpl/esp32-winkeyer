#!/usr/bin/env python3
"""wk-trace-collect.py — keep a whole logger session's host traffic.

    KEYER=192.168.10.20 tools/wk-trace-collect.py

/api/wktrace is a 1024-byte ring, so a long session scrolls out of it. This
polls it every 3 s for 20 minutes, appends each new entry once to
wktrace-all.txt in the current directory, runs wk-trace-check.py on the lot,
and writes drop-N.txt the moment a character the host sent is never echoed.
Read-only: safe to run while RUMlogNG holds the port.
"""
import urllib.request, time, subprocess
import os; URL = f"http://{os.environ.get('KEYER', 'vukeyer.local')}/api/wktrace"
seen, allents, flagged = set(), [], 0
end = time.time() + 20 * 60
while time.time() < end:
    try:
        txt = urllib.request.urlopen(URL, timeout=4).read().decode()
    except Exception as e:
        print(time.strftime("%H:%M:%S"), "fetch error", type(e).__name__, flush=True); time.sleep(3); continue
    lines = [L for L in txt.splitlines() if L and not L.startswith("#")]
    # entries can repeat within one ms; key on (position-independent) ms+dir+byte+occurrence
    counts = {}
    new = 0
    for L in lines:
        counts[L] = counts.get(L, 0) + 1
        key = (L, counts[L])
        if key not in seen:
            seen.add(key); allents.append(L); new += 1
    if new:
        open("wktrace-all.txt", "w").write("# collected\n" + "\n".join(allents) + "\n")
        out = subprocess.run(["python3", os.path.join(os.path.dirname(os.path.abspath(__file__)), "wk-trace-check.py"), "wktrace-all.txt"], capture_output=True, text=True).stdout
        head = out.splitlines()[0] if out else "?"
        n_missing = int(head.split("never echoed:")[1].split()[0]) if "never echoed:" in head else 0
        print(time.strftime("%H:%M:%S"), f"+{new} bytes;", head, flush=True)
        if n_missing > flagged:
            flagged = n_missing
            open(f"drop-{flagged}.txt", "w").write(out)
            print(time.strftime("%H:%M:%S"), f"*** DROP #{flagged} captured -> drop-{flagged}.txt", flush=True)
    time.sleep(3)
