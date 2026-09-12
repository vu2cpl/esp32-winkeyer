#!/usr/bin/env python3
"""Replay a k1el-probeN.py against OUR keyer over WinKeyer TCP (port 8088).

    run-tcp.py k1el-probe3.py 192.168.10.209

The probe is run unchanged except that serial.Serial is replaced by a socket
with the same read/write/timeout semantics, and its log is written to
ours-probeN.log so it can be diffed against the K1EL's.
"""
import socket, sys, types, re

HOST = sys.argv[2] if len(sys.argv) > 2 else "winkeyer.local"
PORT = 8088

class TcpSerial:
    def __init__(self, *a, timeout=0.1, **kw):
        self.sock = socket.create_connection((HOST, PORT), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.timeout = timeout
        self.dtr = self.rts = True
    def write(self, data): self.sock.sendall(data); return len(data)
    def flush(self): pass
    def read(self, n=1):
        self.sock.settimeout(self.timeout)
        try: return self.sock.recv(n)
        except socket.timeout: return b""
    def close(self): self.sock.close()

sys.modules["serial"] = types.SimpleNamespace(Serial=TcpSerial, VERSION="tcp")
src = open(sys.argv[1]).read()
src = src.replace('"k1el-probe', '"ours-probe')
src = re.sub(r'PORT = "/dev/[^"]+"', f'PORT = "tcp://{HOST}:{PORT}"', src)
exec(compile(src, sys.argv[1].replace("k1el", "ours"), "exec"), {"__name__": "__main__"})
