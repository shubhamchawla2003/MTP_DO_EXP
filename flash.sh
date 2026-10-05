#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SKETCH="$ROOT/dissolved_oxygen"
LIB_ROOT="${ARDUINO_LIBRARIES:-$HOME/Documents/Arduino/libraries}"
FQBN="${FQBN:-esp32:esp32:esp32s3}"
BAUD="${BAUD:-115200}"
ACTION="upload"
PORT="${PORT:-}"
LOG_ARGS=()

for arg in "$@"; do
  case "$arg" in
    monitor)
      ACTION="monitor"
      ;;
    log)
      ACTION="log"
      ;;
    web)
      ACTION="web"
      ;;
    upload|flash)
      ACTION="upload"
      ;;
    calibrate)
      ACTION="upload"
      SKETCH="$ROOT/do_calibration"
      ;;
    /dev/*)
      PORT="$arg"
      ;;
    *)
      if [[ "$ACTION" == "log" && "$arg" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
        LOG_ARGS+=(--minutes "$arg")
      elif [[ "$ACTION" == "log" ]]; then
        LOG_ARGS+=(--label "$arg")
      else
        echo "Unknown argument: $arg"
        echo "Usage: ./flash [upload|calibrate|monitor] [port]"
        echo "       ./flash log [label] [minutes] [port]"
        echo "       ./flash web          (phone control panel)"
        exit 1
      fi
      ;;
  esac
done

if [[ "$ACTION" == "upload" && ! -f "$SKETCH/$(basename "$SKETCH").ino" ]]; then
  echo "Sketch not found: $SKETCH"
  exit 1
fi

if [[ -z "$PORT" ]]; then
  for pattern in /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.usbserial* /dev/cu.SLAB_USBtoUART*; do
    for candidate in $pattern; do
      if [[ -e "$candidate" ]]; then
        PORT="$candidate"
        break 2
      fi
    done
  done
fi

if [[ -z "$PORT" ]]; then
  echo "No ESP32 serial port found. Plug in the board and try again."
  exit 1
fi

if [[ ! -e "$PORT" ]]; then
  echo "Port does not exist: $PORT"
  exit 1
fi

holders="$(lsof -t "$PORT" 2>/dev/null || true)"
if [[ -n "$holders" ]]; then
  for pid in $holders; do
    command_line="$(ps -p "$pid" -o command= 2>/dev/null || true)"
    case "$command_line" in
      *serial-monitor*|*serial-mo*|*arduino-cli\ monitor*)
        echo "Closing serial monitor (pid $pid) on $PORT"
        kill "$pid" 2>/dev/null || true
        ;;
      *)
        echo "Port $PORT is in use by pid $pid: $command_line"
        exit 1
        ;;
    esac
  done
  sleep 1
fi

if [[ "$ACTION" == "web" ]]; then
  exec python3 "$ROOT/do_web.py" --port "$PORT"
fi

if [[ "$ACTION" == "log" ]]; then
  exec python3 "$ROOT/do_logger.py" --port "$PORT" ${LOG_ARGS[@]+"${LOG_ARGS[@]}"}
fi

if [[ "$ACTION" == "monitor" ]]; then
  echo "Monitoring $PORT at $BAUD baud. Press Ctrl+C to stop."
  exec arduino-cli monitor -p "$PORT" -b "$FQBN" -c "baudrate=$BAUD" -q
fi

echo "Flashing $SKETCH"
echo "Board: $FQBN"
echo "Port:  $PORT"

arduino-cli compile --upload \
  --fqbn "$FQBN" \
  --port "$PORT" \
  --library "$LIB_ROOT/OneWire" \
  --library "$LIB_ROOT/DallasTemperature" \
  "$SKETCH"
