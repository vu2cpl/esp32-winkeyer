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

# ── refuse a busy port (monitor still open?) ──────────────
if command -v lsof >/dev/null && lsof "$PORT" >/dev/null 2>&1; then
  echo "⚠  $PORT is busy — a serial monitor is probably open." >&2
  echo "   Close it (Ctrl-C the monitor) or pick another port, then retry." >&2
  exit 1
fi

echo "→ pio run -e $ENV -t $TARGET --upload-port $PORT"
exec pio run -e "$ENV" -t "$TARGET" --upload-port "$PORT"
