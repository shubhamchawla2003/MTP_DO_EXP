#!/usr/bin/env python3
"""Phone-friendly web control panel for the DO logger.

Run on the Mac:   python3 do_web.py
Then open the printed http://... address in your phone's browser
(phone and Mac must be on the same Wi-Fi / hotspot).
"""

import argparse
import json
import os
import socket
import subprocess
from collections import deque
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote

from do_logger import DATA_DIR, Recorder

messages = deque(maxlen=20)
recorder = None


def log_message(msg):
    line = f"{datetime.now():%H:%M:%S} {msg}"
    messages.append(line)
    print(line, flush=True)


def list_files():
    if not os.path.isdir(DATA_DIR):
        return []
    names = [n for n in os.listdir(DATA_DIR) if n.endswith(".csv")]
    names.sort(key=lambda n: os.path.getmtime(os.path.join(DATA_DIR, n)), reverse=True)
    return [{"name": n, "size": os.path.getsize(os.path.join(DATA_DIR, n))} for n in names[:30]]


def status():
    running = recorder.running
    return {
        "running": running,
        "connected": recorder.connected,
        "label": recorder.label,
        "phase": recorder.phase,
        "minutes": recorder.minutes,
        "elapsed_s": round(recorder.elapsed(), 1) if running else None,
        "rows": recorder.rows,
        "file": os.path.basename(recorder.out_path) if recorder.out_path else None,
        "latest": recorder.latest,
        "messages": list(messages)[-5:],
        "files": list_files(),
    }


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def _send(self, code, data, content_type, extra_headers=()):
        if isinstance(data, str):
            data = data.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        for key, value in extra_headers:
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(data)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj), "application/json")

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/":
            return self._send(200, PAGE, "text/html; charset=utf-8")
        if path == "/api/status":
            return self._json(status())
        if path.startswith("/data/"):
            name = os.path.basename(unquote(path[len("/data/"):]))
            full = os.path.join(DATA_DIR, name)
            if name.endswith(".csv") and os.path.isfile(full):
                with open(full, "rb") as f:
                    data = f.read()
                return self._send(200, data, "text/csv",
                                  [("Content-Disposition", f'attachment; filename="{name}"')])
        self._json({"error": "Not found"}, 404)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            body = {}

        try:
            if self.path == "/api/start":
                minutes = body.get("minutes")
                minutes = float(minutes) if minutes not in (None, "") else None
                out = recorder.start(body.get("label") or "DO", body.get("phase") or "start", minutes)
                log_message(f"Started '{recorder.label}' -> {os.path.basename(out)}")
            elif self.path == "/api/stop":
                recorder.stop()
            elif self.path == "/api/phase":
                recorder.set_phase(body.get("phase") or "mark")
            else:
                return self._json({"error": "Not found"}, 404)
        except (RuntimeError, ValueError) as exc:
            return self._json({"error": str(exc)}, 400)
        self._json(status())


def local_ip():
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))
            return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"


def main():
    global recorder
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", help="ESP32 serial port (default: auto-detect)")
    parser.add_argument("--http-port", type=int, default=8000, help="web page port (default 8000)")
    args = parser.parse_args()

    recorder = Recorder(args.port, on_message=log_message)
    server = ThreadingHTTPServer(("0.0.0.0", args.http_port), Handler)

    # Keep the Mac from idle-sleeping while the control panel is running.
    try:
        subprocess.Popen(["caffeinate", "-i", "-w", str(os.getpid())])
    except OSError:
        pass

    host = socket.gethostname()
    if not host.endswith(".local"):
        host += ".local"
    print("DO logger control panel is running.")
    print("Open one of these on your phone (same Wi-Fi as this Mac):")
    print(f"    http://{local_ip()}:{args.http_port}")
    print(f"    http://{host}:{args.http_port}")
    print("Press Ctrl+C here to shut it down.\n", flush=True)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()
    finally:
        if recorder.running:
            recorder.stop()
        server.server_close()


PAGE = r"""<!doctype html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DO Logger</title>
<style>
body{font-family:-apple-system,system-ui,sans-serif;margin:0 auto;padding:12px;background:#f1f5f9;color:#1e293b;max-width:640px}
h1{font-size:20px;margin:4px 0 12px}
.card{background:#fff;border-radius:12px;padding:14px;margin-bottom:12px;box-shadow:0 1px 3px rgba(0,0,0,.08)}
.big{display:flex;gap:8px;margin-top:10px}
.big div{flex:1;text-align:center}
.val{font-size:30px;font-weight:600}
.unit{font-size:12px;color:#64748b}
label{display:block;font-size:13px;color:#475569;margin-top:10px}
input{width:100%;box-sizing:border-box;font-size:17px;padding:10px;border:1px solid #cbd5e1;border-radius:8px}
button{font-size:17px;padding:13px;border:0;border-radius:8px;width:100%;margin-top:10px;color:#fff;background:#2563eb}
button.stop{background:#dc2626}
button.alt{background:#475569}
.row{display:flex;gap:8px;align-items:flex-end}
.row>*{flex:1}
.on{color:#16a34a}.off{color:#64748b}.warn{color:#d97706}
a{color:#2563eb}
.small{font-size:13px;color:#64748b;line-height:1.6}
</style>
</head><body>
<h1>Dissolved Oxygen Logger</h1>

<div class="card">
  <div>Status: <b id="state">connecting...</b></div>
  <div class="small" id="info"></div>
  <div class="big">
    <div><div class="val" id="do">-</div><div class="unit">DO mg/L</div></div>
    <div><div class="val" id="temp">-</div><div class="unit">Temp &deg;C</div></div>
    <div><div class="val" id="volt">-</div><div class="unit">Voltage mV</div></div>
  </div>
</div>

<div class="card" id="startCard" style="display:none">
  <label>Experiment name</label><input id="label" value="cold_water">
  <label>Starting phase</label><input id="phase0" value="in_air">
  <label>Stop after (minutes, blank = until you press Stop)</label>
  <input id="minutes" type="number" inputmode="decimal" value="12">
  <button onclick="startRun()">Start recording</button>
</div>

<div class="card" id="runCard" style="display:none">
  <div>Current phase: <b id="phase">-</b></div>
  <div class="row">
    <button class="alt" onclick="setPhase('in_air')">in_air</button>
    <button class="alt" onclick="setPhase('in_water')">in_water</button>
  </div>
  <div class="row">
    <input id="customPhase" placeholder="custom phase name">
    <button class="alt" style="flex:0 0 28%" onclick="setPhase(document.getElementById('customPhase').value)">Set</button>
  </div>
  <button class="stop" onclick="stopRun()">Stop recording</button>
  <div class="small" style="margin-top:8px">Recording runs on the Mac. Refreshing or closing this page does not stop it;
    every reading is already saved to the CSV.</div>
</div>

<div class="card"><b>Saved CSV files</b><div id="files" class="small"></div></div>
<div class="card small" id="msgs"></div>

<script>
const $ = id => document.getElementById(id);
const esc = s => String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));

async function api(path, body) {
  const opts = body === undefined ? {} :
    {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)};
  const r = await fetch(path, opts);
  const j = await r.json();
  if (!r.ok) { alert(j.error || 'Error'); return null; }
  return j;
}
const FORM_FIELDS = ['label', 'phase0', 'minutes'];
FORM_FIELDS.forEach(id => {
  const saved = localStorage.getItem('do_' + id);
  if (saved !== null) $(id).value = saved;
});
function startRun() {
  FORM_FIELDS.forEach(id => localStorage.setItem('do_' + id, $(id).value));
  api('/api/start', {label: $('label').value, phase: $('phase0').value, minutes: $('minutes').value}).then(render);
}
function stopRun() { if (confirm('Stop recording?')) api('/api/stop', {}).then(render); }
function setPhase(p) { if (p && p.trim()) api('/api/phase', {phase: p}).then(render); }
function fmt(s) { s = Math.round(s || 0); return Math.floor(s / 60) + 'm ' + String(s % 60).padStart(2, '0') + 's'; }

function render(s) {
  if (!s) return;
  const st = $('state');
  if (s.running && s.connected) { st.textContent = 'Recording'; st.className = 'on'; }
  else if (s.running) { st.textContent = 'Waiting for ESP32...'; st.className = 'warn'; }
  else { st.textContent = 'Idle'; st.className = 'off'; }
  $('startCard').style.display = s.running ? 'none' : '';
  $('runCard').style.display = s.running ? '' : 'none';
  $('info').textContent = s.running
    ? `${s.label} | ${fmt(s.elapsed_s)}${s.minutes ? ' of ' + s.minutes + ' min' : ''} | ${s.rows} rows | ${s.file}`
    : (s.file ? `Last run: ${s.file} (${s.rows} rows)` : 'Fill in the form and press Start.');
  const l = s.latest || {};
  $('do').textContent = l.do ? Number(l.do).toFixed(2) : '-';
  $('temp').textContent = l.temp ? Number(l.temp).toFixed(1) : '-';
  $('volt').textContent = l.voltage ? Number(l.voltage).toFixed(0) : '-';
  $('phase').textContent = s.phase || '-';
  $('files').innerHTML = s.files.length
    ? s.files.map(f => `<div><a href="/data/${encodeURIComponent(f.name)}">${esc(f.name)}</a> (${(f.size / 1024).toFixed(1)} KB)</div>`).join('')
    : 'No files yet';
  $('msgs').innerHTML = s.messages.map(m => `<div>${esc(m)}</div>`).join('');
}

async function poll() {
  try { render(await api('/api/status')); }
  catch (e) { $('state').textContent = 'Cannot reach the Mac'; $('state').className = 'warn'; }
}
poll();
setInterval(poll, 2000);
</script>
</body></html>
"""


if __name__ == "__main__":
    main()
