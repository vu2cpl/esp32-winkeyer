#!/usr/bin/env python3
"""Time each run of the radio's "cwx sent=" reports from a saved /api/flextrace dump, as the keyer's CW timing learner does. Usage: python3 tools/cw-runs.py <trace-file>"""
import re, sys
MORSE={'A':'.-','B':'-...','C':'-.-.','D':'-..','E':'.','F':'..-.','G':'--.','H':'....','I':'..','J':'.---','K':'-.-','L':'.-..','M':'--','N':'-.','O':'---','P':'.--.','Q':'--.-','R':'.-.','S':'...','T':'-','U':'..-','V':'...-','W':'.--','X':'-..-','Y':'-.--','Z':'--..','0':'-----','1':'.----','2':'..---','3':'...--','4':'....-','5':'.....','6':'-....','7':'--...','8':'---..','9':'----.','?':'..--..','/':'-..-.','=':'-...-','.':'.-.-.-',',':'--..--'}
def units(c):
    if c==' ': return 4
    p=MORSE.get(c.upper())
    return sum(3 if x=='-' else 1 for x in p)+len(p)-1+3 if p else 0
lines=[l.rstrip('\n') for l in open(sys.argv[1], encoding='latin-1') if not l.startswith('#')]
def ts(l): return int(l.split(' ',1)[0])
lines.sort(key=ts)
seqtxt={}; send={}; sent=[]; wpm=[]
for l in lines:
    m=re.match(r'(\d+) > C(\d+)\|cwx send (.*)$',l)
    if m: seqtxt[m.group(2)]=m.group(3).replace('\x7f',' ') or ' '
    m=re.match(r'(\d+) < R(\d+)\|0\|(\d+)$',l)
    if m and m.group(2) in seqtxt:
        for i,c in enumerate(seqtxt[m.group(2)]): send[int(m.group(3))+i]=(c,int(m.group(1)))
    m=re.match(r'(\d+) < S\w+\|cwx sent=(\d+)',l)
    if m: sent.append((int(m.group(2)),int(m.group(1))))
    m=re.match(r'(\d+) .*cwx wpm[= ](\d+)',l)
    if m: wpm.append((int(m.group(1)),int(m.group(2)),l[:60]))
print("wpm events:", [(a,b) for a,b,_ in wpm])
run=None
def fin(r):
    if not r: return
    ms=r['t1']-r['t0']
    tag = "LEARNED?" if r['u']>=40 else "short"
    ex = ms*1000/r['u']-48000 if r['u'] else 0
    print("%s idx %d-%d t=%d..%d units %3d %6d ms extra %5.0f  minslack %s  %r" % (tag, r['i0'],r['i1'],r['t0'],r['t1'],r['u'],ms,ex, r['ms'], r['txt']))
for idx,t in sent:
    if run and idx==run['i1']+1 and t-run['t1']<3000:
        if idx not in send: fin(run); run=None; continue
        c,at=send[idx]
        slack=run['t1']-at
        if at+100>run['t1']:
            fin(run); run=dict(i0=idx,i1=idx,t0=t,t1=t,u=0,txt='',ms=None); continue
        run['u']+=units(c); run['i1']=idx; run['t1']=t; run['txt']+=c
        run['ms']=slack if run['ms'] is None else min(run['ms'],slack)
    else:
        fin(run); run=dict(i0=idx,i1=idx,t0=t,t1=t,u=0,txt='',ms=None)
fin(run)

print("\nper character: idx char units interval_ms extra_ms(vs 48.0/unit+0.75) slack_ms(prev report - queued) prevchar_ms")
prev=None
for idx,t in sent:
    if prev and idx==prev[0]+1 and idx in send and t-prev[1]<3000:
        c,at=send[idx]; u=units(c)
        if u:
            exp=u*48.75
            pc = send.get(prev[0],(None,0))[0]
            print("%5d %r u=%2d int=%5d extra=%6.0f slack=%5d prev=%r(%d ms)" % (idx,c,u,t-prev[1],(t-prev[1])-exp, prev[1]-at, pc, units(pc)*48.75 if pc else 0))
    prev=(idx,t)
