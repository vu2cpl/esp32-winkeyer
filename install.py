#!/usr/bin/env python3
"""ESP32 WinKeyer — installer.

Bootstraps the toolchain (PlatformIO) on macOS or Raspberry Pi / Linux, then
builds the firmware to verify the environment. WiFi + MQTT creds are NOT set
here — WiFi is onboarded via the WiFiManager captive portal (vu2cpl-esp32-winkeyer-setup); MQTT
role/password go in include/secrets.h (copied from secrets.h.example).

Shack rule: install scripts must support both macOS (daily driver) and the Pi
(always-on host), branching the toolchain path per platform.
"""
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ENV = "esp32-winkeyer"


def detect():
    sysname = platform.system()
    if sysname == "Darwin":
        return "macos"
    if sysname == "Windows":
        return "windows"
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
    try:
        ans = input("Is this correct? [Y/n] (or macos/pi/linux/windows to override): ").strip().lower()
    except EOFError:
        return auto
    if ans in ("", "y", "yes"):
        return auto
    if ans in ("macos", "pi", "linux", "windows"):
        return ans
    return auto


VENV_DIR = Path.home() / ".pio-venv313"


def venv_bin(venv, exe):
    """Windows puts venv executables in Scripts\\ and suffixes them .exe."""
    if platform.system() == "Windows":
        return venv / "Scripts" / (exe + ".exe")
    return venv / "bin" / exe


VENV_PIO = venv_bin(VENV_DIR, "pio")


def pio_python_ok(pio_path):
    """True if this PlatformIO runs on Python 3.10+.

    The Arduino 3.x platform refuses anything older, with a bare
    "ERROR: Python version must be 3.10 ..." that says nothing about which
    PlatformIO it means — so check before building rather than after.
    """
    try:
        out = subprocess.run([str(pio_path), "system", "info", "--json-output"],
                             capture_output=True, text=True, timeout=300)
        if out.returncode != 0:
            return False
        info = json.loads(out.stdout).get("python_version", "")
        # PlatformIO reports {"title": ..., "value": "3.13.0"}; older cores
        # returned the bare string.
        ver = info.get("value", "") if isinstance(info, dict) else str(info)
        major, minor = (int(x) for x in ver.split(".")[:2])
        return (major, minor) >= (3, 10)
    except Exception:
        return False


def make_pio_venv():
    """Build a PlatformIO virtualenv on the newest Python we can find."""
    cands = ["python3.14", "python3.13", "python3.12", "python3.11",
             "python3.10", "python3"]
    if platform.system() == "Windows":
        cands = ["python", "py"] + cands
    for cand in cands:
        exe = shutil.which(cand)
        if not exe:
            continue
        try:
            v = subprocess.run([exe, "-c",
                                "import sys;print('%d.%d' % sys.version_info[:2])"],
                               capture_output=True, text=True, timeout=30).stdout.strip()
            if tuple(int(x) for x in v.split(".")) < (3, 10):
                continue
        except Exception:
            continue
        print(f"  creating a PlatformIO environment on {cand} ({v})…")
        if subprocess.run([exe, "-m", "venv", str(VENV_DIR)]).returncode != 0:
            print("  could not create the virtualenv (python3-venv missing?)")
            return None
        pip = venv_bin(VENV_DIR, "pip")
        print("  installing PlatformIO — this takes a minute…")
        if subprocess.run([str(pip), "install", "platformio"]).returncode != 0:
            return None
        return venv_bin(VENV_DIR, "pio")
    return None


_PIO_CACHE = None


def ensure_pio(host):
    """Return a PlatformIO that can build this project.

    This firmware is built against Arduino core 3.x (pioarduino platform),
    which needs Python 3.10+. A PlatformIO installed under an older Python —
    common on machines that have had it for a while — cannot build it at all.
    """
    global _PIO_CACHE
    if _PIO_CACHE:
        return _PIO_CACHE

    override = os.environ.get("PIO")
    if override and pio_python_ok(override):
        _PIO_CACHE = override
        return _PIO_CACHE

    if VENV_PIO.exists() and pio_python_ok(VENV_PIO):
        _PIO_CACHE = str(VENV_PIO)
        return _PIO_CACHE

    on_path = shutil.which("pio")
    if on_path and pio_python_ok(on_path):
        _PIO_CACHE = on_path
        return _PIO_CACHE

    if on_path:
        print("\nThe PlatformIO on PATH runs on Python older than 3.10, which")
        print("cannot build this firmware (Arduino core 3.x needs 3.10+).")
        print("Your existing install is left alone; a separate one is needed.")
    else:
        print("\nPlatformIO is not on PATH.")

    try:
        ans = input("Create a PlatformIO environment in ~/.pio-venv313 now? [Y/n] ")
    except EOFError:
        ans = "n"
    if ans.strip().lower() in ("", "y", "yes"):
        pio = make_pio_venv()
        if pio and pio_python_ok(pio):
            print(f"  ready: {pio}")
            _PIO_CACHE = str(pio)
            return _PIO_CACHE
        print("  that did not work — install one by hand.")

    print("\nInstall PlatformIO on Python 3.10+ and re-run, e.g.:")
    if host == "macos":
        print("  brew install python@3.13")
        print("  python3.13 -m venv ~/.pio-venv313")
        print("  ~/.pio-venv313/bin/pip install platformio")
    elif host == "windows":
        print("  py -3.13 -m venv %USERPROFILE%\\.pio-venv313")
        print("  %USERPROFILE%\\.pio-venv313\\Scripts\\pip install platformio")
    else:
        print("  sudo apt install python3-venv        # if needed")
        print("  python3 -m venv ~/.pio-venv313")
        print("  ~/.pio-venv313/bin/pip install platformio")
        print("  (Raspberry Pi OS Bookworm+ ships Python 3.11, which is fine)")
    sys.exit(1)


# Everything a new operator plausibly needs to change. Defaults match
# config.h, so pressing Enter through the whole thing is a valid answer.
SETTINGS = [
    ("MQTT_HOST",      "MQTT broker IP (blank = no broker)", "192.168.1.10", False),
    ("MQTT_USER",      "MQTT username",                      "iot",          False),
    ("MQTT_PASS",      "MQTT password",                      "",             True),
    ("MDNS_HOSTNAME",  "mDNS name (-> <name>.local)",         "winkeyer",     False),
    ("WIFI_AP_NAME",   "Setup AP name shown on first boot",  "vu2cpl-esp32-winkeyer-setup", False),
    ("WIFI_AP_PASS",   "Setup AP password (8+ chars)",       "vu2cpl1234",   False),
]


def ask_settings():
    """Prompt for local settings and write them to include/secrets.h.

    Written to secrets.h rather than patched into config.h: secrets.h is
    git-ignored, so a clone stays clean and an upgrade never collides with
    local edits. Anything defined there wins over the config.h default.
    """
    print("\nLocal settings — press Enter to accept the default in [brackets].")
    print("These go in include/secrets.h, which is git-ignored.\n")
    vals = {}
    for key, prompt, default, secret in SETTINGS:
        shown = "" if secret and not default else f" [{default}]"
        try:
            got = input(f"  {prompt}{shown}: ").strip()
        except EOFError:
            got = ""     # non-interactive run: take the defaults
        vals[key] = got if got else default

    if len(vals["WIFI_AP_PASS"]) < 8:
        print("  ! AP password must be 8+ characters — keeping the default.")
        vals["WIFI_AP_PASS"] = "vu2cpl1234"

    lines = [
        "#pragma once",
        "// Written by install.py. Git-ignored: safe for local settings.",
        "// Anything here overrides the default in config.h.",
        "",
    ]
    for key, _, _, _ in SETTINGS:
        v = vals[key]
        if key == "MQTT_HOST" and not v:
            lines.append("// no broker configured")
            continue
        lines.append(f'#define {key:<16}"{v}"')
    with open(os.path.join(HERE, "include", "secrets.h"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n• Wrote include/secrets.h")


def ensure_secrets():
    ex = os.path.join(HERE, "include", "secrets.h.example")
    lv = os.path.join(HERE, "include", "secrets.h")
    if os.path.exists(ex) and not os.path.exists(lv):
        shutil.copyfile(ex, lv)
        print("• Created include/secrets.h from example — fill in the MQTT password.")


def list_ports():
    """Ask PlatformIO for the ports — it knows COM*, /dev/cu.* and /dev/tty*
    alike, so this works the same on Windows, macOS and the Pi. Globbing
    device paths would need a branch per OS and still miss Windows."""
    try:
        out = subprocess.run([ensure_pio(detect()), "device", "list", "--json-output"],
                             capture_output=True, text=True, check=True,
                             cwd=HERE).stdout
        ports = [d["port"] for d in json.loads(out)]
    except Exception:
        return []
    # macOS advertises Bluetooth links and a debug console as serial ports;
    # offering those as flash targets is noise at best. Keep the USB-serial
    # devices a board actually appears as, on any OS, and fall back to the
    # raw list if the filter matches nothing.
    likely = [p for p in ports
              if any(k in p.lower() for k in
                     ("usbserial", "usbmodem", "ttyusb", "ttyacm", "wchusb"))
              or p.upper().startswith("COM")]
    return likely or ports


def pick_port():
    ports = list_ports()
    if not ports:
        print("No serial ports found. Is a board plugged in?")
        sys.exit(1)
    if len(ports) == 1:
        print(f"Using only port: {ports[0]}")
        return ports[0]
    # Never guess with several attached: some CP2102s share factory serial
    # 0001, so the wrong board would be flashed silently.
    print("Multiple ports — pick the board:")
    for i, p in enumerate(ports, 1):
        print(f"  {i}) {p}")
    while True:
        try:
            sel = input(f"Port [1-{len(ports)}]: ").strip()
        except EOFError:
            print("\nNo selection (stdin closed) — nothing flashed.")
            sys.exit(1)
        if sel.isdigit() and 1 <= int(sel) <= len(ports):
            return ports[int(sel) - 1]


def do_flash():
    pio = ensure_pio(detect())
    port = pick_port()
    sys.exit(subprocess.run([pio, "run", "-e", ENV, "-t", "upload",
                             "--upload-port", port], cwd=HERE).returncode)


def do_monitor(baud):
    pio = ensure_pio(detect())
    port = pick_port()
    print(f"Monitoring {port} at {baud} baud. Ctrl-C to stop.")
    sys.exit(subprocess.run([pio, "device", "monitor", "--port", port,
                             "-b", str(baud)], cwd=HERE).returncode)


def main():
    # Subcommands work on every OS, which matters on Windows where
    # flash.sh / monitor.sh cannot run at all.
    if len(sys.argv) > 1:
        cmd = sys.argv[1].lower()
        if cmd == "flash":
            return do_flash()
        if cmd == "monitor":
            return do_monitor(sys.argv[2] if len(sys.argv) > 2 else 1200)
        if cmd in ("-h", "--help", "help"):
            print("ESP32 WinKeyer installer\n"
                  "  python3 install.py              set up the toolchain and verify the build\n"
                  "  python3 install.py flash        build + upload (picks the serial port)\n"
                  "  python3 install.py monitor [b]  serial monitor, default 1200 baud\n"
                  "\nPlatformIO must run on Python 3.10+ (Arduino core 3.x); this\n"
                  "script finds or creates one. PIO=/path/to/pio overrides.")
            return
        print(f"Unknown command '{cmd}'. Use: flash | monitor [baud] | --help")
        sys.exit(2)

    host = confirm(detect())
    pio = ensure_pio(host)
    existing = os.path.exists(os.path.join(HERE, "include", "secrets.h"))
    if existing:
        ans = input("\ninclude/secrets.h already exists. Re-enter settings? [y/N] ")
        if ans.strip().lower() in ("y", "yes"):
            ask_settings()
    else:
        ask_settings()
    ensure_secrets()
    print("\nBuilding firmware to verify the toolchain…")
    print("  (the first build downloads the Arduino 3.x platform and its")
    print("   toolchain — a few hundred MB, several minutes)")
    r = subprocess.run([pio, "run", "-e", "esp32-winkeyer"], cwd=HERE)
    if r.returncode == 0:
        print("\n✓ Build OK.")
        if host == "windows":
            # flash.sh / monitor.sh are bash; on Windows drive pio directly.
            print("  1. python install.py flash   build + upload (picks the port)")
        else:
            print("  1. ./flash.sh        build + upload (picks the serial port)")
        print("  2. join WiFi AP 'vu2cpl-esp32-winkeyer-setup' (pw vu2cpl1234)")
        print("     and pick your network in the captive portal")
        print("  3. http://winkeyer.local/  for settings")
        print()
        print("  Note: the serial console runs at 1200 baud 8N2 by default —")
        print("  that is the WinKeyer standard, and what a logger expects. So")
        print("  the boot log is one line; use the web page for status.")
        if host == "windows":
            print("  python install.py monitor         1200 baud (default)")
            print("  python install.py monitor 115200  after /baud 115200")
        else:
            print("  ./monitor.sh          1200 baud (default)")
            print("  ./monitor.sh 115200   after /baud 115200, for a readable log")
    else:
        print("\n✗ Build failed — see the output above.")
        sys.exit(r.returncode)


if __name__ == "__main__":
    main()
