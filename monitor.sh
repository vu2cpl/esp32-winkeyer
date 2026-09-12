#!/usr/bin/env bash
# ============================================================
#  monitor.sh — serial monitor on a port picked LIVE.
#  Same port-enumeration rationale as flash.sh (never pin monitor_port).
#    ./monitor.sh
# ============================================================
set -euo pipefail
cd "$(dirname "$0")"

shopt -s nullglob
if [[ "$(uname)" == "Darwin" ]]; then
  PORTS=(/dev/cu.usbserial* /dev/cu.usbmodem*)
else
  PORTS=(/dev/ttyUSB* /dev/ttyACM*)
fi
shopt -u nullglob

if   [[ ${#PORTS[@]} -eq 0 ]]; then
  echo "No serial ports found. Is a board plugged in?" >&2; exit 1
elif [[ ${#PORTS[@]} -eq 1 ]]; then
  PORT="${PORTS[0]}"; echo "Using only port: $PORT"
else
  echo "Multiple ports — select the board to monitor:"
  select p in "${PORTS[@]}"; do [[ -n "${p:-}" ]] && PORT="$p" && break; done
fi
: "${PORT:?No port selected}"

# The firmware defaults to 1200 baud, because that is what a K1EL WinKeyer
# runs at and what a logger opens the port with. Pass a rate to override,
# e.g. ./monitor.sh 115200 if you set /baud 115200 for a readable console.
# ── find a PlatformIO that can build this project ─────────
# The pioarduino platform (Arduino core 3.x) requires Python >= 3.10, and a
# PlatformIO installed under an older Python fails with a bare
# "ERROR: Python version must be 3.10 ...". Prefer $PIO, then a 3.10+ venv,
# then whatever is on PATH — and only if its interpreter is new enough.
pio_ok() {
  local p="$1" shebang ver
  [ -n "$p" ] && [ -x "$p" ] || return 1
  shebang=$(head -1 "$p" 2>/dev/null | sed 's|^#!||' | awk '{print $1}')
  [ -x "$shebang" ] || return 1
  ver=$("$shebang" -c 'import sys; print("%d.%d" % sys.version_info[:2])' 2>/dev/null) || return 1
  [ "$(printf '3.10\n%s\n' "$ver" | sort -V | head -1)" = "3.10" ]
}
find_pio() {
  local c
  for c in "${PIO:-}" "$HOME/.pio-venv313/bin/pio" "$HOME/.platformio/penv/bin/pio" "$(command -v pio 2>/dev/null)"; do
    if pio_ok "$c"; then echo "$c"; return 0; fi
  done
  cat >&2 <<'MSG'
⚠  No PlatformIO on Python 3.10+ was found, and this project needs one:
   the Arduino 3.x platform refuses to run on older Python.

   Create one (macOS with Homebrew python, or a Pi with python3 >= 3.10):

     python3 -m venv ~/.pio-venv313
     ~/.pio-venv313/bin/pip install platformio

   Then re-run this script, or set PIO=/path/to/pio.
MSG
  return 1
}

PIO_BIN=$(find_pio) || exit 1

BAUD="${1:-1200}"
echo "→ $PIO_BIN device monitor --port $PORT -b $BAUD"
echo "  (firmware default is 1200 8N2 — WinKeyer standard. Boot log is one"
echo "   line at that rate; use http://winkeyer.local/ for full status.)"
exec "$PIO_BIN" device monitor --port "$PORT" -b "$BAUD"
