#!/usr/bin/env python3
"""Serve the settings page from src/web.cpp locally, with a stubbed API.

Why this exists: the page is a PROGMEM string inside the firmware, so the
only way to look at it is normally to flash a board. That makes a broken
page expensive to find — and a clean build says nothing about whether the
JavaScript runs. Three separate UI faults reached hardware during one
session because the build was green: a line placed in the wrong scope
blanked the whole page, `hidden` on a .row did nothing because
`.row{display:flex}` outranks it, and `hidden` on an <option> is ignored
by Safari entirely.

Run this, open the URL, and check the page actually renders and responds
BEFORE flashing.

    python3 tools/web-preview.py            # then open http://127.0.0.1:8791/

Writes fall into a stub that always succeeds, so this exercises rendering
and the request paths, not firmware behaviour. Values below are made up.
"""

import http.server
import json
import os
import re
import socketserver
import sys

PORT = 8791
HERE = os.path.dirname(os.path.abspath(__file__))
WEB_CPP = os.path.join(HERE, "..", "src", "web.cpp")

# A plausible board. Deliberately generic — no real addresses live here.
STATE = {
    "wpm": 28, "mode": "b", "swap": False, "st": True, "sthz": 600,
    "ptt": True, "ptton": False, "lead": 50, "tail": 400,
    "weight": 50, "ratio": 50, "farns": 0,
    "pot": True, "potmin": 12, "potmax": 40,
    "busy": False, "key": False, "tune": False,
    "backend": "flex", "host": True, "tcp": False,
    "disp": True, "dispctl": "sh1106", "disphw": True,
    "radio": 1, "monitor": True, "pecho": 2, "pechoon": False,
    "baud": 1200, "flextail": 400,
    "fskbaud": 45.45000076,          # the float really does arrive like this
    "fskinv": False, "fskdid": False, "fskbusy": False,
    "resetreason": "power-on", "uptime": 274,
    "call": "VU2CPL",
    "mems": ["CQ CQ %C %C K", "UR 5NN 5NN TU", "73 TU", "", "", ""],
    "flex": {"enabled": True, "connected": True, "ip": "192.168.1.50",
             "slice": True, "slicewarn": "", "cmd": "key", "bind": True,
             "xmit": True, "xmiton": False},
    "rssi": -62, "ip": "192.168.1.20",
}


# Fake radio finder: a sweep that takes ~6 s and finds one radio.
SCAN = {"t0": 0.0, "net": ""}


def scan_state():
    import time
    if not SCAN["t0"]:
        return {"running": False, "tried": 0, "net": "", "err": "", "hits": []}
    tried = min(254, int((time.time() - SCAN["t0"]) * 42))
    hits = [{"ip": SCAN["net"] + ".50", "model": "FLEX-6600", "name": "SHACK"}] \
        if tried >= 50 else []
    return {"running": tried < 254, "tried": tried, "net": SCAN["net"],
            "err": "", "hits": hits}


def extract_page():
    src = open(WEB_CPP).read()
    m = re.search(r'R"HTML\((.*?)\)HTML"', src, re.S)
    if not m:
        sys.exit("could not find the R\"HTML(...)HTML\" block in src/web.cpp")
    return m.group(1).encode()


class Handler(http.server.BaseHTTPRequestHandler):
    def _send(self, body, ctype):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/api/flexscan"):
            self._send(json.dumps(scan_state()).encode(), "application/json")
        elif self.path.startswith("/api/state"):
            self._send(json.dumps(STATE).encode(), "application/json")
        else:
            # Re-read every request, so editing web.cpp and reloading is enough.
            self._send(extract_page(), "text/html")

    def do_POST(self):
        if self.path.startswith("/api/flexscan"):
            import time, urllib.parse
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            net = (q.get("net", [""])[0] or STATE["ip"].rsplit(".", 1)[0])
            SCAN.update(t0=time.time(), net=".".join(net.split(".")[:3]))
            self._send(f"scanning {SCAN['net']}.1-254 (preview stub)".encode(),
                       "text/plain")
            return
        self._send(b"ok (preview stub)", "text/plain")

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    extract_page()          # fail early if the block cannot be found
    print(f"settings page from src/web.cpp -> http://127.0.0.1:{PORT}/")
    print("edit web.cpp and just reload; Ctrl-C to stop")
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", PORT), Handler) as srv:
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            print()
