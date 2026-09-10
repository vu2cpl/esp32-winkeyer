#!/usr/bin/env python3
"""
wk-test.py — exercise the WinKeyer protocol engine over TCP or serial.

Acts as a host the way a logger does: opens host mode, checks the version
byte, sets speed, sends text, and decodes the status/pot bytes coming back.
Use it to verify the firmware without installing a logging program.

    ./wk-test.py                          # winkeyer.local:8088
    ./wk-test.py --host 192.168.1.77
    ./wk-test.py --serial /dev/cu.usbserial-0001
    ./wk-test.py --text "CQ TEST VU2CPL"
"""

import argparse
import socket
import sys
import time

try:
    import serial  # optional, only for --serial
except ImportError:
    serial = None


class Link:
    """Uniform read/write over either a socket or a serial port."""

    def __init__(self, sock=None, ser=None):
        self.sock, self.ser = sock, ser

    def write(self, data):
        if self.sock:
            self.sock.sendall(data)
        else:
            self.ser.write(data)

    def read(self, timeout=1.0):
        if self.sock:
            self.sock.settimeout(timeout)
            try:
                return self.sock.recv(256)
            except socket.timeout:
                return b""
        self.ser.timeout = timeout
        return self.ser.read(256)

    def close(self):
        (self.sock or self.ser).close()


def decode(data):
    """Render the bytes a WinKeyer sends back to its host."""
    out = []
    for b in data:
        if b & 0xC0 == 0xC0:
            flags = []
            if b & 0x20: flags.append("WAIT")
            if b & 0x10: flags.append("KEYDOWN")
            if b & 0x08: flags.append("BUSY")
            if b & 0x04: flags.append("BREAKIN")
            if b & 0x02: flags.append("XOFF")
            out.append(f"status(0x{b:02X}) {'|'.join(flags) or 'idle'}")
        elif b & 0xC0 == 0x80:
            out.append(f"pot={b & 0x3F}")
        elif 0x20 <= b < 0x7F:
            out.append(f"echo '{chr(b)}'")
        else:
            out.append(f"byte 0x{b:02X}")
    return out


def drain(link, secs, label=""):
    end = time.time() + secs
    seen = b""
    while time.time() < end:
        chunk = link.read(0.2)
        if chunk:
            seen += chunk
            for line in decode(chunk):
                print(f"    <- {line}")
    if label and not seen:
        print(f"    <- (nothing){' — ' + label if label else ''}")
    return seen


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="winkeyer.local")
    ap.add_argument("--port", type=int, default=8088)
    ap.add_argument("--serial")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--wpm", type=int, default=25)
    ap.add_argument("--text", default="CQ DE VU2CPL")
    args = ap.parse_args()

    if args.serial:
        if serial is None:
            sys.exit("pyserial not installed: pip3 install pyserial")
        link = Link(ser=serial.Serial(args.serial, args.baud, timeout=1))
        time.sleep(0.3)
        link.ser.reset_input_buffer()
        print(f"opened {args.serial} @ {args.baud}")
    else:
        s = socket.create_connection((args.host, args.port), timeout=5)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        link = Link(sock=s)
        print(f"connected to {args.host}:{args.port}")

    print("\n>> host open (00 02)")
    link.write(bytes([0x00, 0x02]))
    reply = drain(link, 1.5)
    ver = next((b for b in reply if 0x14 <= b <= 0x3F), None)
    print(f"    version byte: {ver}" if ver else "    !! no version byte returned")

    print("\n>> request status (15)")
    link.write(bytes([0x15]))
    drain(link, 1.0)

    print(f"\n>> set speed {args.wpm} WPM (02 {args.wpm:02X})")
    link.write(bytes([0x02, args.wpm]))
    drain(link, 0.5)

    print(f"\n>> send text: {args.text!r}")
    link.write(args.text.upper().encode("ascii", "ignore"))
    print("    (watching for BUSY then idle — this takes as long as the CW does)")
    drain(link, max(6.0, len(args.text) * 60.0 / (args.wpm * 5)) + 3.0)

    print("\n>> host close (00 03)")
    link.write(bytes([0x00, 0x03]))
    drain(link, 0.5)
    link.close()
    print("\ndone")


if __name__ == "__main__":
    main()
