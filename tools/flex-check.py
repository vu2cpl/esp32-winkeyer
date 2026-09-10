#!/usr/bin/env python3
"""
flex-check.py — why isn't the keyer keying the Flex?

Every prerequisite for network keying fails SILENTLY: the radio reports no
error for a missing slice, a slice in the wrong mode, or a missing GUI
client. It simply transmits nothing, which looks identical to a broken
keyer. This checks all of them in one pass, and can drive a keying test.

    ./flex-check.py                        # report prerequisites only
    ./flex-check.py --radio 192.168.1.50
    ./flex-check.py --key                  # also key via the keyer (TRANSMITS)
    ./flex-check.py --key --serial /dev/cu.usbserial-0001

--key drives /tune on the keyer and watches the radio's interlock for
proof it actually keyed. Use a dummy load.
"""

import argparse
import socket
import threading
import time

DEFAULT_RADIO = "192.168.1.50"
DEFAULT_SERIAL = "/dev/cu.usbserial-0001"


class Radio:
    def __init__(self, host):
        self.sock = socket.create_connection((host, 4992), timeout=5)
        self.sock.settimeout(0.2)
        self.seq = 0
        self.lines = []
        self.replies = {}
        self.interlock = []
        self.stop = threading.Event()
        threading.Thread(target=self._read, daemon=True).start()
        time.sleep(1.2)

    def cmd(self, text):
        self.seq += 1
        self.sock.sendall(f"C{self.seq}|{text}\n".encode())
        return self.seq

    def _read(self):
        buf = b""
        last = None
        while not self.stop.is_set():
            try:
                data = self.sock.recv(65536)
            except (socket.timeout, OSError):
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                txt = line.decode(errors="replace").strip()
                if not txt:
                    continue
                self.lines.append(txt)
                if txt.startswith("R"):
                    p = txt.split("|", 2)
                    n = int(p[0][1:]) if p[0][1:].isdigit() else -1
                    self.replies[n] = (p[1] if len(p) > 1 else "?",
                                       p[2] if len(p) > 2 else "")
                elif "interlock" in txt.lower():
                    st = next((x[6:] for x in txt.split()
                               if x.startswith("state=")), None)
                    if st and st != last:
                        last = st
                        self.interlock.append(st)

    def close(self):
        self.stop.set()
        time.sleep(0.3)
        self.sock.close()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--radio", default=DEFAULT_RADIO)
    ap.add_argument("--key", action="store_true",
                    help="drive a keying test through the keyer (TRANSMITS)")
    ap.add_argument("--serial", default=DEFAULT_SERIAL)
    args = ap.parse_args()

    r = Radio(args.radio)
    r.cmd("sub slice all")
    r.cmd("sub client all")
    r.cmd("sub tx all")
    time.sleep(3.0)

    print(f"radio {args.radio}\n")

    # --- slice: the one that cost hours ---
    active = [l for l in r.lines if "in_use=1" in l and "slice " in l]
    if not active:
        print("  slice ........... NONE IN USE   <-- the radio will not transmit")
        print("                    open a slice/receiver in SmartSDR")
        mode = None
    else:
        d = dict(p.split("=", 1) for p in active[-1].split() if "=" in p)
        mode = d.get("mode")
        idx = active[-1].split("slice ")[1].split()[0]
        ok = "ok" if mode == "CW" else "NOT CW  <-- will not key"
        print(f"  slice ........... {idx} @ {d.get('RF_frequency')} mode={mode}  {ok}")

    # --- GUI client: without one, tx_allowed=0 ---
    gui = [l for l in r.lines if "client_id=" in l and "connected" in l]
    if gui:
        h = gui[-1].split("client ")[1].split()[0]
        print(f"  GUI client ...... connected, handle {h}")
    else:
        print("  GUI client ...... none reported (SmartSDR running?)")

    # --- interlock ---
    il = [l for l in r.lines if "interlock" in l.lower() and "tx_allowed" in l]
    if il:
        bits = [x for x in il[-1].split()
                if x.startswith(("state=", "tx_allowed=", "tx_client_handle="))]
        print(f"  interlock ....... {' '.join(bits)}")

    # --- break-in ---
    bi = [l for l in r.lines if "break_in=" in l]
    if bi:
        v = next((x for x in bi[-1].split() if x.startswith("break_in=")), "?")
        print(f"  break-in ........ {v}")

    if not args.key:
        print("\n(--key also drives a keying test through the keyer)")
        r.close()
        return

    if mode != "CW":
        print("\nrefusing to key: the slice is not in CW mode")
        r.close()
        return

    try:
        import serial
    except ImportError:
        print("\npyserial not installed: pip3 install pyserial")
        r.close()
        return

    print("\nkeying via the keyer's /tune — watch your power meter")
    ser = serial.Serial(args.serial, 115200, timeout=0.1)
    end = time.time() + 3
    while time.time() < end:
        ser.read(16384)

    before = len(r.interlock)
    ser.write(b"/tune\n")
    time.sleep(3.0)
    ser.write(b"/tune\n")
    time.sleep(2.5)
    out = ser.read(65536).decode(errors="replace")
    ser.close()

    for line in out.splitlines():
        if "FLEX" in line:
            print("   " + line.strip())

    changes = r.interlock[before:]
    print(f"\n   interlock: {changes or 'no change'}")
    print("   >>> TRANSMITTED" if "TRANSMITTING" in changes
          else "   >>> did not transmit")
    r.close()


if __name__ == "__main__":
    main()
