#!/usr/bin/env bash
# Compile + upload the sketch to the XIAO nRF52840, then wait for the sketch to
# come back on USB WITHOUT opening any serial port while the bootloader is up.
#
#   tools/flash.sh                 compile + upload
#   tools/flash.sh --log FILE [S]  ...then record the serial log to FILE for S seconds
#                                  (default 300) in the background
#   tools/flash.sh --no-compile    upload the last build
#
# USB IDs: 2886:8044 = sketch running, 2886:0045 = bootloader (serial DFU).
set -euo pipefail

if ! id -nG | grep -qw dialout; then        # group added but not active in this shell yet
  exec sg dialout -c "$(printf '%q ' "$0" "$@")"
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CLI="$HOME/.local/bin/arduino-cli"
FQBN=Seeeduino:nrf52:xiaonRF52840
SKETCH="$ROOT/mtg_life_counter"
LOG="" LOG_S=300 COMPILE=1
while [ $# -gt 0 ]; do
  case "$1" in
    --log) LOG="$2"; shift 2
           if [[ "${1:-}" =~ ^[0-9]+$ ]]; then LOG_S="$1"; shift; fi ;;
    --no-compile) COMPILE=0; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

usb_state() {
  if lsusb -d 2886:8044 >/dev/null 2>&1; then echo app
  elif lsusb -d 2886:0045 >/dev/null 2>&1; then echo bootloader
  else echo none; fi
}
# the XIAO's tty (never the ZMK keyboards, VID 1d50)
xiao_tty() {
  local l
  for l in /dev/serial/by-id/usb-Seeed_XIAO_nRF52840*-if00; do
    [ -e "$l" ] && { readlink -f "$l"; return 0; }
  done
  return 1
}
# true if the device still answers a USB control request (CDC SET_CONTROL_LINE_STATE)
answers_usb() {
  python3 -I -c '
import fcntl, os, struct, sys, termios
fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
try:
    fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack("I", termios.TIOCM_DTR))
finally:
    os.close(fd)' "$1" 2>/dev/null
}

# close any logger of ours that still holds the port
TTY="$(xiao_tty || true)"
if [ -n "$TTY" ]; then
  for fd in /proc/[0-9]*/fd/*; do
    if [ "$(readlink "$fd" 2>/dev/null)" = "$TTY" ]; then
      pid="${fd#/proc/}"; pid="${pid%%/*}"
      echo "closing $(cat /proc/$pid/comm) (pid $pid) holding $TTY"
      kill "$pid" 2>/dev/null || true
    fi
  done
  sleep 0.3
fi

STATE="$(usb_state)"
echo "before upload: $STATE${TTY:+ on $TTY}"
if [ "$STATE" = none ]; then echo "XIAO not on USB"; exit 1; fi
if [ "$STATE" = bootloader ] && ! answers_usb "$TTY"; then
  echo "STUCK: the bootloader is enumerated but does not answer USB requests."
  echo "Press the XIAO reset button once (or slide switch OFF and replug USB), then retry."
  exit 3
fi

if [ "$COMPILE" = 1 ]; then
  "$CLI" compile -b "$FQBN" "$SKETCH" 2>&1 | grep -E "error|Sketch uses|Global" || true
  [ "${PIPESTATUS[0]}" = 0 ] || { echo "compile failed"; exit 1; }
fi

T0=$(date +%s)
if ! "$CLI" upload -b "$FQBN" -p "$TTY" "$SKETCH" > /tmp/flash_upload.$$ 2>&1; then
  grep -E "rror|DFU" /tmp/flash_upload.$$ | head -5
  rm -f /tmp/flash_upload.$$
  echo "upload FAILED (state now: $(usb_state))"
  exit 1
fi
rm -f /tmp/flash_upload.$$
echo "upload done in $(( $(date +%s) - T0 )) s"

# Wait for the sketch to enumerate. Don't open the tty meanwhile: right after DFU it
# can still be the bootloader's port.
for i in $(seq 150); do
  [ "$(usb_state)" = app ] && TTY="$(xiao_tty || true)" && [ -n "$TTY" ] && break
  sleep 0.1
done
if [ "$(usb_state)" != app ]; then
  BT="$(xiao_tty || true)"
  echo "STUCK after upload: state=$(usb_state) 15 s after DFU."
  if [ -n "$BT" ] && ! answers_usb "$BT"; then
    echo "  bootloader does not answer USB control requests (CPU not servicing USB)."
  fi
  journalctl -k --since "-1 min" --no-pager 2>/dev/null | grep -E "usb 3-.*(2886|disconnect)" | tail -4
  echo "Press the XIAO reset button once (or slide switch OFF and replug USB)."
  exit 3
fi
echo "sketch running on $TTY"

if [ -n "$LOG" ]; then
  sleep 0.5
  stty -F "$TTY" 115200 raw -echo
  nohup timeout "$LOG_S" cat "$TTY" > "$LOG" 2>/dev/null &
  echo "logging to $LOG for $LOG_S s (pid $!)"
fi
