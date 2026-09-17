#!/usr/bin/env python3
"""Record /api/flextrace and /api/cwtable changes to a file until stopped. Usage: KEYER=<ip> python3 tools/flex-trace-record.py out.log"""
import json, sys, time, urllib.request
K="http://"+__import__("os").environ.get("KEYER","vukeyer.local"); OUT=sys.argv[1]
seen=set(); last=None
with open(OUT,"a",buffering=1) as f:
    while True:
        try:
            body=urllib.request.urlopen(K+"/api/flextrace",timeout=4).read().decode("latin-1")
            for l in body.splitlines():
                if l.startswith("#") or l in seen: continue
                seen.add(l); f.write(l+"\n")
            t=json.load(urllib.request.urlopen(K+"/api/cwtable",timeout=3))["table"]
            v=[(r["wpm"],r["us"],r["runs"]) for r in t if r["runs"]]
            st=json.load(urllib.request.urlopen(K+"/api/state",timeout=3))
            cur=(v, st.get("wpm"))
            if cur!=last:
                f.write("#TABLE %d %s keyerwpm=%s\n" % (int(time.time()*1000), v, st.get("wpm"))); last=cur
        except Exception as e:
            f.write("#ERR %s\n" % e.__class__.__name__)
        time.sleep(0.4)
