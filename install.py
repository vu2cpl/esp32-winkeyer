#!/usr/bin/env python3
"""ESP32 WinKeyer — installer.

Bootstraps the toolchain (PlatformIO) on macOS or Raspberry Pi / Linux, then
builds the firmware to verify the environment. WiFi + MQTT creds are NOT set
here — WiFi is onboarded via the WiFiManager captive portal (vu2cpl-esp32-winkeyer-setup); MQTT
role/password go in include/secrets.h (copied from secrets.h.example).

Shack rule: install scripts must support both macOS (daily driver) and the Pi
(always-on host), branching the toolchain path per platform.
"""
import os
import platform
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def detect():
    sysname = platform.system()
    if sysname == "Darwin":
        return "macos"
    if sysname == "Linux":
        try:
            with open("/proc/cpuinfo") as f:
                info = f.read().lower()
            if "raspberry pi" in info or "bcm27" in info or "bcm28" in info:
                return "pi"
        except OSError:
            pass
        return "linux"
    return sysname.lower()


def confirm(auto):
    print(f"Auto-detected host: {auto}")
    ans = input("Is this correct? [Y/n] (or type macos/pi/linux to override): ").strip().lower()
    if ans in ("", "y", "yes"):
        return auto
    if ans in ("macos", "pi", "linux"):
        return ans
    return auto


def ensure_pio(host):
    if shutil.which("pio"):
        print("✓ PlatformIO already installed")
        return
    print("PlatformIO (pio) not found.")
    if host == "macos":
        print("  Install with:  brew install platformio")
        print("  or (no brew):  pip3 install --user platformio")
    else:  # pi / linux
        print("  Install with:  pip3 install --user platformio")
        print("  (Raspberry Pi OS Bookworm+ may need --break-system-packages)")
    print("Then re-run this script.")
    sys.exit(1)


def ensure_secrets():
    ex = os.path.join(HERE, "include", "secrets.h.example")
    lv = os.path.join(HERE, "include", "secrets.h")
    if os.path.exists(ex) and not os.path.exists(lv):
        shutil.copyfile(ex, lv)
        print("• Created include/secrets.h from example — fill in the MQTT password.")


def main():
    host = confirm(detect())
    ensure_pio(host)
    ensure_secrets()
    print("\nBuilding firmware to verify the toolchain…")
    r = subprocess.run(["pio", "run", "-e", "esp32-winkeyer"], cwd=HERE)
    if r.returncode == 0:
        print("\n✓ Build OK. Next: ./flash.sh   (then join the vu2cpl-esp32-winkeyer-setup portal)")
    else:
        print("\n✗ Build failed — see the output above.")
        sys.exit(r.returncode)


if __name__ == "__main__":
    main()
