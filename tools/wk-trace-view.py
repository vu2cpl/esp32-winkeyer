#!/usr/bin/env python3
"""Show /api/wktrace with repeated host-open / 0x17 rounds collapsed. Usage: KEYER=<ip> python3 tools/wk-trace-view.py [lines]"""
import sys, json, urllib.request
body = urllib.request.urlopen("http://" + __import__("os").environ.get("KEYER", "vukeyer.local") + "/api/wktrace", timeout=5).read().decode("latin-1")
cur=None; out=[]
for l in body.splitlines():
    if l.startswith("#"): print(l); continue
    ts,d,b,*r = l.split(); ts=int(ts)
    if cur and cur[1]==d and ts-cur[2]<40: cur[3].append(b); cur[2]=ts
    else:
        if cur: out.append(cur)
        cur=[ts,d,ts,[b]]
if cur: out.append(cur)
res=[]; cnt=0; first=None
for o in out:
    txt=" ".join(o[3])
    if (o[1],txt) in (("H>K","00 02"),("K>H","17")):
        if not cnt: first=o[0]
        cnt+=1; continue
    if cnt: res.append("%d..%d  %d open/17 groups" % (first, o[0], cnt)); cnt=0
    res.append("%d %s %s" % (o[0], o[1], txt))
if cnt: res.append("%d..  %d open/17 groups" % (first, cnt))
print("\n".join(res[-int(sys.argv[1]) if len(sys.argv)>1 else -60:]))
