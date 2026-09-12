#!/usr/bin/env bash
# ============================================================
#  flash.sh — build + upload to a serial port picked LIVE.
#
#  Shack rule: never pin upload_port in platformio.ini. Multiple ESP
#  boards re-enumerate to different /dev names (and some CP2102s share
#  factory serial 0001), so the only safe path is to enumerate and pick
#  each run.
#
#  Usage:
#    ./flash.sh              # upload firmware (menu/auto port)
#    ./flash.sh uploadfs     # upload LittleFS image (web UI), if present
# ============================================================
set -euo pipefail
cd "$(dirname "$0")"

ENV="esp32-winkeyer"
TARGET="${1:-upload}"      # upload | uploadfs

# ── enumerate serial ports (macOS + Linux) ────────────────
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
  echo "Multiple ports — select the target board:"
  select p in "${PORTS[@]}"; do [[ -n "${p:-}" ]] && PORT="$p" && break; done
fi
: "${PORT:?No port selected}"

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

# ── refuse a busy port (monitor still open?) ──────────────
if command -v lsof >/dev/null && lsof "$PORT" >/dev/null 2>&1; then
  echo "⚠  $PORT is busy — a serial monitor is probably open." >&2
  echo "   Close it (Ctrl-C the monitor) or pick another port, then retry." >&2
  exit 1
fi

echo "→ $PIO_BIN run -e $ENV -t $TARGET --upload-port $PORT"
exec "$PIO_BIN" run -e "$ENV" -t "$TARGET" --upload-port "$PORT"
