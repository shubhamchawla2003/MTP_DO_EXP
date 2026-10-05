#!/usr/bin/env python3
"""Log ESP32 dissolved-oxygen readings from the serial port into a CSV file.

Usage:
    python3 do_logger.py                      # auto-detect port, new file in data/
    python3 do_logger.py --label cold_water   # data/cold_water_<date>_<time>.csv
    python3 do_logger.py --label hot_water --minutes 10   # stop after 10 min
    python3 do_logger.py --port /dev/cu.usbmodemXXXX --out my_run.csv
"""

import argparse
import csv
import glob
import os
import sys
import threading
import time
from collections import deque
from datetime import datetime

import serial

BAUD = 115200
PORT_PATTERNS = (
    "/dev/cu.usbmodem*",
    "/dev/cu.wchusbserial*",
    "/dev/cu.usbserial*",
    "/dev/cu.SLAB_USBtoUART*",
)
CSV_COLUMNS = [
    "Experiment",
    "Phase",
    "Timestamp",
    "Elapsed_s",
    "Elapsed_min",
    "DO_mg_L",
    "Temperature_C",
    "Voltage_mV",
    "ADC",
    "Status",
]
DEVICE_FIELD_COUNT = 6  # Uptime_s,DO_mg_L,Temperature_C,Voltage_mV,ADC,Status
DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")


def find_port():
    for pattern in PORT_PATTERNS:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return None


def parse_line(line):
    """Return the device fields for a data line, or None for anything else."""
    if not line or line.startswith("#") or line.startswith("Uptime_s"):
        return None
    fields = [f.strip() for f in line.split(",")]
    if len(fields) != DEVICE_FIELD_COUNT:
        return None
    try:
        float(fields[0])
    except ValueError:
        return None
    return fields


def clean_name(text, default):
    return "".join(c if c.isalnum() or c in "-_" else "_" for c in str(text).strip()) or default


class Recorder:
    """Reads the ESP32 in a background thread and appends each reading to a CSV."""

    def __init__(self, port=None, on_row=None, on_message=None, history_size=900):
        self.port_arg = port
        self.on_row = on_row
        self.on_message = on_message or (lambda msg: None)
        self.history = deque(maxlen=history_size)
        self.label = None
        self.phase = None
        self.minutes = None
        self.out_path = None
        self.rows = 0
        self.latest = None
        self.connected = False
        self._start_mono = None
        self._thread = None
        self._stop = threading.Event()
        self._lock = threading.Lock()

    @property
    def running(self):
        return self._thread is not None and self._thread.is_alive()

    def elapsed(self):
        return 0.0 if self._start_mono is None else time.monotonic() - self._start_mono

    def start(self, label="DO", phase="start", minutes=None, out_path=None):
        if self.running:
            raise RuntimeError("Already recording. Stop the current run first.")
        self.label = clean_name(label, "DO")
        self.phase = clean_name(phase, "start")
        self.minutes = minutes if minutes and minutes > 0 else None
        if out_path is None:
            os.makedirs(DATA_DIR, exist_ok=True)
            stamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
            out_path = os.path.join(DATA_DIR, f"{self.label}_{stamp}.csv")
        self.out_path = out_path
        self.rows = 0
        self.latest = None
        self._start_mono = None
        with self._lock:
            self.history.clear()
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return out_path

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=5)

    def wait(self):
        while self.running:
            self._thread.join(timeout=0.5)

    def set_phase(self, phase):
        self.phase = clean_name(phase, "mark")
        self.on_message(f">> Phase is now '{self.phase}'")
        return self.phase

    def recent(self, n):
        with self._lock:
            return list(self.history)[-n:]

    def _open_serial(self):
        last_msg = None
        while not self._stop.is_set():
            port = self.port_arg or find_port()
            if port and os.path.exists(port):
                try:
                    ser = serial.Serial(port, BAUD, timeout=1)
                    ser.reset_input_buffer()
                    self.connected = True
                    self.on_message(f"Connected to {port} at {BAUD} baud")
                    return ser
                except serial.SerialException as exc:
                    msg = f"Cannot open {port}: {exc}. Retrying..."
            else:
                msg = "Waiting for ESP32 to be plugged in..."
            if msg != last_msg:
                self.on_message(msg)
                last_msg = msg
            self._stop.wait(2)
        return None

    def _run(self):
        ser = self._open_serial()
        if ser is None:
            return
        self._start_mono = time.monotonic()
        try:
            with open(self.out_path, "w", newline="") as f:
                writer = csv.writer(f)
                writer.writerow(CSV_COLUMNS)
                f.flush()

                while not self._stop.is_set():
                    if self.minutes and self.elapsed() >= self.minutes * 60:
                        self.on_message(f"Reached {self.minutes:g} min.")
                        break
                    try:
                        raw = ser.readline()
                    except (serial.SerialException, OSError):
                        self.connected = False
                        self.on_message("Serial connection lost, reconnecting...")
                        ser.close()
                        ser = self._open_serial()
                        if ser is None:
                            break
                        continue

                    line = raw.decode("utf-8", errors="ignore").strip()
                    if line.startswith("#"):
                        self.on_message(line)
                        continue
                    fields = parse_line(line)
                    if fields is None:
                        continue

                    _, do_mg_l, temp_c, voltage_mv, adc, status = fields
                    now = datetime.now()
                    elapsed = self.elapsed()
                    phase = self.phase

                    writer.writerow([
                        self.label,
                        phase,
                        now.strftime("%Y-%m-%d %H:%M:%S"),
                        f"{elapsed:.1f}",
                        f"{elapsed / 60:.3f}",
                        do_mg_l,
                        temp_c,
                        voltage_mv,
                        adc,
                        status,
                    ])
                    f.flush()
                    self.rows += 1

                    row = {
                        "time": now.strftime("%H:%M:%S"),
                        "elapsed_s": round(elapsed, 1),
                        "do": do_mg_l,
                        "temp": temp_c,
                        "voltage": voltage_mv,
                        "status": status,
                        "phase": phase,
                    }
                    self.latest = row
                    with self._lock:
                        self.history.append(row)
                    if self.on_row:
                        self.on_row(row)
        finally:
            if ser:
                ser.close()
            self.connected = False
            self.on_message(f"Stopped. {self.rows} rows saved to {self.out_path}")


def watch_keyboard(recorder):
    """Typing a word + Enter changes the Phase written to every following row."""
    for line in sys.stdin:
        recorder.set_phase(line)


def print_row(row):
    print(
        f"{row['time']}  t={row['elapsed_s']:7.1f}s  "
        f"DO={row['do'] or '-':>6} mg/L  T={row['temp'] or '-':>6} C  "
        f"V={row['voltage']:>6} mV  {row['status']}  [{row['phase']}]"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", help="serial port (default: auto-detect)")
    parser.add_argument("--out", help="CSV path (default: data/<label>_<date>_<time>.csv)")
    parser.add_argument("--label", default="DO",
                        help="experiment name, used in the file name and Experiment column")
    parser.add_argument("--minutes", type=float,
                        help="stop automatically after this many minutes")
    parser.add_argument("--phase", default="start",
                        help="initial Phase value; type a new one + Enter while logging")
    args = parser.parse_args()
    sys.stdout.reconfigure(line_buffering=True)

    recorder = Recorder(args.port, on_row=print_row, on_message=print)
    label = clean_name(args.label, "DO")
    print(f"Experiment: {label}   Phase: {clean_name(args.phase, 'start')}")
    if args.minutes:
        print(f"Will stop automatically after {args.minutes:g} min.")
    print("Type a phase name + Enter to mark a change (e.g. in_water). Ctrl+C to stop.")
    out_path = recorder.start(label, args.phase, args.minutes, args.out)
    print(f"Saving to {out_path}\n")
    threading.Thread(target=watch_keyboard, args=(recorder,), daemon=True).start()

    try:
        recorder.wait()
    except KeyboardInterrupt:
        print()
        recorder.stop()


if __name__ == "__main__":
    sys.exit(main())
