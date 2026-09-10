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
BAUD="${1:-1200}"
echo "→ pio device monitor --port $PORT -b $BAUD"
echo "  (firmware default is 1200 8N2 — WinKeyer standard. Boot log is one"
echo "   line at that rate; use http://winkeyer.local/ for full status.)"
exec pio device monitor --port "$PORT" -b "$BAUD"
