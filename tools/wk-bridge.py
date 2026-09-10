#!/usr/bin/env python3
"""
wk-bridge.py — expose the ESP32 WinKeyer's TCP port as a local serial port.

Logging software wants a serial device, not a socket. This creates a PTY,
symlinks it to a stable path, and shuttles bytes to the keyer over WiFi.
Point N1MM/RUMlogNG/fldigi at the symlink and set the port to WinKeyer.

    ./wk-bridge.py                      # find winkeyer.local, link /tmp/winkeyer
    ./wk-bridge.py --host 192.168.1.77  # fixed address
    ./wk-bridge.py --link ~/winkeyer    # choose where the symlink lands

macOS and Linux. On Windows use com0com + a TCP client, or run this in WSL.
Byte timing is not critical on this link: the keyer generates the CW itself,
so the socket only carries text and status.
"""

import argparse
import os
import pty
import select
import signal
import socket
import sys
import time

DEFAULT_HOST = "winkeyer.local"
DEFAULT_PORT = 8088
DEFAULT_LINK = "/tmp/winkeyer"


def log(msg):
    print(f"[bridge] {msg}", file=sys.stderr, flush=True)


def resolve(host):
    try:
        return socket.gethostbyname(host)
    except OSError:
        return None


def connect(host, port, retry=True):
    while True:
        ip = resolve(host) or host
        try:
            s = socket.create_connection((ip, port), timeout=5)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.settimeout(None)
            log(f"connected to {ip}:{port}")
            return s
        except OSError as e:
            if not retry:
                raise
            log(f"connect to {host}:{port} failed ({e}); retrying in 3 s")
            time.sleep(3)


def make_pty(link):
    master, slave = pty.openpty()
    name = os.ttyname(slave)
    try:
        if os.path.islink(link) or os.path.exists(link):
            os.unlink(link)
        os.symlink(name, link)
    except OSError as e:
        log(f"could not create symlink {link}: {e} — use {name} directly")
        link = None
    log(f"serial port ready: {link or name}")
    return master, slave, link


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--link", default=DEFAULT_LINK)
    args = ap.parse_args()

    master, slave, link = make_pty(args.link)

    def cleanup(*_):
        if link and os.path.islink(link):
            try:
                os.unlink(link)
            except OSError:
                pass
        log("stopped")
        sys.exit(0)

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    sock = connect(args.host, args.port)

    while True:
        try:
            r, _, _ = select.select([master, sock], [], [], 1.0)
        except (OSError, ValueError):
            break

        if master in r:
            try:
                data = os.read(master, 1024)
            except OSError:
                data = b""
            if data:
                try:
                    sock.sendall(data)
                except OSError:
                    log("keyer link dropped while writing; reconnecting")
                    sock.close()
                    sock = connect(args.host, args.port)

        if sock in r:
            try:
                data = sock.recv(1024)
            except OSError:
                data = b""
            if not data:
                log("keyer closed the connection; reconnecting")
                sock.close()
                sock = connect(args.host, args.port)
                continue
            try:
                os.write(master, data)
            except OSError:
                pass


if __name__ == "__main__":
    main()
