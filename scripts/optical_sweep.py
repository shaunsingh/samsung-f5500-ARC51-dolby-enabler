#!/usr/bin/env python3
"""
optical_sweep.py - Metrology-grade hands-free smartphone photometer for
Samsung PN60F5500 (libPDPTune 2.5.0).

Cancels both 59.94Hz/30Hz plasma rolling-shutter beating (~0.06 Hz) and slow
camera auto-exposure drift via tight symmetric A -> B -> A bracketing:

    ratio = Y_B / (0.5 * (Y_A_before + Y_A_after))

Includes explicit NULL (A -> A) control steps at the start and end of each
sweep to prove the real measurement noise floor before drawing conclusions.
"""
import argparse
import json
import os
import re
import socket
import ssl
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

TV_HOST = ""
TV_PORT = 23
LIB_PATH = "/mnt/opt/privateer/usr/libso/libPDPTune.so"
RESULTS_PATH = "/tmp/optical_sweep_results_v3_fine.json"

BASE_OPTS = (
    "UNLOCK DRIVE:STANDARD DEEPBLACK:ON PEAKSTRETCH:ON "
    "DEEPBLACK_MODE:ON POWERSAVE:OFF APCSPEED:OFF SAPC:OFF "
    "OSD_GAMMA:0 GAMMA:2.4"
)

SWEEPS = {
    "duty2_fine": {
        "title": "Fine Peak Zoom: DUTY2 160..195 (A->B->A + Nulls)",
        "repeats": 3,
        "baseline_opts": f"{BASE_OPTS} SUBCONTRAST:132 DUTY2:128 PATTERN:WHITE",
        "steps": [
            {"id": "null1", "x": 152, "label": "NULL 128->128",
             "opts": f"{BASE_OPTS} SUBCONTRAST:132 DUTY2:128 PATTERN:WHITE"},
            {"id": "160", "x": 160, "label": "DUTY2:160",
             "opts": f"{BASE_OPTS} SUBCONTRAST:132 DUTY2:160 PATTERN:WHITE"},
            {"id": "168", "x": 168, "label": "DUTY2:168",
             "opts": f"{BASE_OPTS} SUBCONTRAST:132 DUTY2:168 PATTERN:WHITE"},
            {"id": "175", "x": 175, "label": "DUTY2:175",
             "opts": f"{BASE_OPTS} SUBCONTRAST:132 DUTY2:175 PATTERN:WHITE"},
            {"id": "182", "x": 182, "label": "DUTY2:182",
             "opts": f"{BASE_OPTS} SUBCONTRAST:132 DUTY2:182 PATTERN:WHITE"},
            {"id": "188", "x": 188, "label": "DUTY2:188",
             "opts": f"{BASE_OPTS} SUBCONTRAST:132 DUTY2:188 PATTERN:WHITE"},
            {"id": "195", "x": 195, "label": "DUTY2:195",
             "opts": f"{BASE_OPTS} SUBCONTRAST:132 DUTY2:195 PATTERN:WHITE"},
            {"id": "null2", "x": 203, "label": "NULL 128->128",
             "opts": f"{BASE_OPTS} SUBCONTRAST:132 DUTY2:128 PATTERN:WHITE"},
        ],
    },
    "total_vs_stock": {
        "title": "Total Gain vs Stock Movie (175 / 182 / 188)",
        "repeats": 3,
        "baseline_opts": "UNLOCK SUBCONTRAST:128 DUTY2:128 OSD_GAMMA:0 GAMMA:2.4 PATTERN:WHITE",
        "steps": [
            {"id": "null1", "x": 0, "label": "NULL Stock Movie",
             "opts": "UNLOCK SUBCONTRAST:128 DUTY2:128 OSD_GAMMA:0 GAMMA:2.4 PATTERN:WHITE"},
            {"id": "opt_175", "x": 1, "label": "Full Tuned 175",
             "opts": f"{BASE_OPTS} SUBCONTRAST:132 DUTY2:175 PATTERN:WHITE"},
            {"id": "opt_182", "x": 2, "label": "Full Tuned 182",
             "opts": f"{BASE_OPTS} SUBCONTRAST:132 DUTY2:182 PATTERN:WHITE"},
            {"id": "opt_188", "x": 3, "label": "Full Tuned 188",
             "opts": f"{BASE_OPTS} SUBCONTRAST:132 DUTY2:188 PATTERN:WHITE"},
        ],
    },
}

STATE_LOCK = threading.Lock()
TV_LOCK = threading.Lock()
STATE = {
    "temp_c": None,
    "measurements": {},
}


class PersistentTV:
    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.sock = None

    def _connect(self):
        if self.sock:
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None
        s = socket.create_connection((self.host, self.port), 8)
        s.settimeout(0.6)
        self._drain(s, 2.0)
        s.sendall(b"stty -echo 2>/dev/null; PS1=; PS2=; . /dtv/SGO.env\n")
        self._drain(s, 1.0)
        self.sock = s

    def _drain(self, s, max_s):
        deadline = time.time() + max_s
        while time.time() < deadline:
            try:
                chunk = s.recv(65536)
                if not chunk:
                    break
            except socket.timeout:
                break

    def run(self, cmd, timeout=12.0):
        marker = f"__OK_{int(time.time()*1000)%1000000}__"
        payload = f"{cmd}\necho {marker}\n".encode()
        for attempt in range(2):
            try:
                if self.sock is None:
                    self._connect()
                self.sock.sendall(payload)
                out = b""
                deadline = time.time() + timeout
                bmark = marker.encode()
                while time.time() < deadline and bmark not in out:
                    try:
                        chunk = self.sock.recv(65536)
                        if not chunk:
                            raise ConnectionError("EOF")
                        out += chunk
                    except socket.timeout:
                        continue
                if bmark not in out:
                    raise TimeoutError("TV shell timeout")
                text = re.sub(rb"\xff[\xfb-\xfe].|\xff\xfa.*?\xff\xf0|\r", b"", out, flags=re.S).decode(errors="replace")
                return text.split(marker)[0].strip()
            except Exception:
                self.sock = None
                if attempt == 1:
                    raise


TV = None


def load_results():
    if os.path.exists(RESULTS_PATH):
        try:
            with open(RESULTS_PATH, "r") as f:
                STATE["measurements"] = json.load(f)
        except Exception:
            pass


def save_results():
    try:
        with open(RESULTS_PATH, "w") as f:
            json.dump(STATE["measurements"], f, indent=2)
    except Exception:
        pass


def tv_apply(opts, with_dump=False):
    dump_tok = " DUMP" if with_dump else ""
    cmd = (
        f"LIB={LIB_PATH}; [ -f \"$LIB\" ] || LIB=/mnt/opt/privateer/usr/libso/libPDPTune.so; "
        f"rm -f /dtv/PDPTune.log; "
        f"samyGOso -T -B -r -l \"$LIB\" {opts}{dump_tok} > /dev/null"
    )
    if with_dump:
        cmd += "; sleep 0.4; cat /dtv/PDPTune.log"
    with TV_LOCK:
        t0 = time.time()
        log = TV.run(cmd)
        ms = int((time.time() - t0) * 1000)
    m = re.search(r"panel temperature\s+(-?\d+)\s*C", log)
    temp_c = int(m.group(1)) if m else STATE["temp_c"]
    if m:
        with STATE_LOCK:
            STATE["temp_c"] = temp_c
    return log, temp_c, ms


HTML = r"""<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>PN60F5500 Bracketed Photometer v3</title>
<style>
  :root { color-scheme: dark; --bg:#0d1117; --card:#161b22; --bd:#30363d; --ac:#58a6ff; --ok:#3fb950; --warn:#d29922; --bad:#f85149; }
  body { margin:0; padding:10px; font-family:-apple-system,system-ui,sans-serif; background:var(--bg); color:#e6edf3; font-size:14px; }
  h1 { font-size:17px; margin:0 0 8px; }
  .card { background:var(--card); border:1px solid var(--bd); border-radius:10px; padding:10px; margin-bottom:10px; }
  .row { display:flex; gap:8px; flex-wrap:wrap; align-items:center; }
  button, select {
    background:#21262d; color:#e6edf3; border:1px solid var(--bd); border-radius:8px;
    padding:10px 12px; font-size:14px; cursor:pointer;
  }
  button.ok { background:#238636; border-color:#2ea043; font-weight:700; }
  button.bad { background:#b62324; border-color:#f85149; font-weight:700; }
  button:disabled { opacity:.45; }
  .vwrap { position:relative; width:100%; max-height:190px; overflow:hidden; border-radius:8px; background:#000; border:1px solid var(--bd); }
  video { width:100%; height:190px; object-fit:cover; display:block; }
  .reticle { position:absolute; left:10%; top:10%; width:80%; height:80%;
    border:2px solid #3fb950; pointer-events:none; border-radius:4px; box-sizing:border-box; }
  .hud { position:absolute; left:8px; bottom:8px; background:rgba(0,0,0,.76); padding:4px 8px; border-radius:6px; font-family:monospace; font-size:12px; }
  .pill { font-size:12px; padding:2px 8px; border-radius:12px; background:#21262d; border:1px solid var(--bd); }
  table { width:100%; border-collapse:collapse; font-size:12.5px; margin-top:6px; }
  th, td { padding:5px 4px; border-bottom:1px solid var(--bd); text-align:right; }
  th:first-child, td:first-child { text-align:left; }
  .clip { color:var(--bad); font-weight:700; }
  .good { color:var(--ok); font-weight:700; }
  svg { width:100%; height:175px; background:#090c10; border-radius:8px; border:1px solid var(--bd); margin-top:6px; }
  #status { font-size:13px; color:#c9d1d9; margin-top:6px; line-height:1.35; }
</style></head><body>
<h1>PN60F5500 A&rarr;B&rarr;A Bracketed Photometer (v3)</h1>

<div class="card">
  <div class="vwrap">
    <video id="vid" autoplay playsinline muted></video>
    <div class="reticle"></div>
    <div class="hud" id="hud">Ready</div>
  </div>
  <div class="row" style="margin-top:8px">
    <button id="goBtn" class="ok" style="flex:1;padding:13px;font-size:15px" onclick="oneClickRun()">&#9654; Start Camera &amp; Run Bracketed Sweep (~75s)</button>
    <button id="stopBtn" class="bad" onclick="abortSweep = true" disabled>Stop</button>
  </div>
  <div class="row" style="margin-top:8px;justify-content:space-between">
    <select id="sweepSel" style="flex:1"></select>
    <span class="pill" id="tempPill">Temp: --</span>
  </div>
  <div id="status">Uses symmetric A&rarr;B&rarr;A bracketing + NULL (128&rarr;128) controls to cancel the 0.06 Hz rolling-shutter beat! Fill the green box with the white TV screen and hold steady.</div>
</div>

<div class="card">
  <div class="row" style="justify-content:space-between">
    <strong>Measured Luminance (A&rarr;B&rarr;A Cancelled)</strong>
    <button onclick="clearSweep()" style="padding:4px 8px;font-size:12px">Clear</button>
  </div>
  <svg id="chart" viewBox="0 0 360 175"></svg>
  <div id="summary" style="margin-top:6px;font-size:13px"></div>
  <table>
    <thead><tr><th>Step</th><th>Ratio</th><th>&plusmn;SD</th><th>Drift</th><th>Runs</th></tr></thead>
    <tbody id="tbl"></tbody>
  </table>
</div>

<canvas id="cnv" width="160" height="120" style="display:none"></canvas>

<script>
let SWEEPS = {}, MEAS = {}, curSweep = 'duty2_fine';
let stream = null, running = false, abortSweep = false;
let latestFrame = null, frameRing = [];

const srgbLin = new Float32Array(256);
for (let i = 0; i < 256; i++) {
  const x = i / 255;
  srgbLin[i] = x <= 0.04045 ? x / 12.92 : Math.pow((x + 0.055) / 1.055, 2.4);
}

async function init() {
  const r = await fetch('/api/state');
  const d = await r.json();
  SWEEPS = d.sweeps;
  MEAS = d.measurements || {};
  if (d.temp_c !== null) document.getElementById('tempPill').textContent = 'Temp: ' + d.temp_c + 'C';
  const sel = document.getElementById('sweepSel');
  sel.innerHTML = '';
  for (const [k, v] of Object.entries(SWEEPS)) {
    const o = document.createElement('option');
    o.value = k; o.textContent = v.title;
    sel.appendChild(o);
  }
  sel.value = curSweep;
  sel.onchange = () => { curSweep = sel.value; render(); };
  render();
}

async function startCamera() {
  if (stream) return true;
  try {
    stream = await navigator.mediaDevices.getUserMedia({
      video: { facingMode: { ideal: 'environment' }, width: { ideal: 640 }, height: { ideal: 480 }, frameRate: { ideal: 60 } },
      audio: false
    });
    const vid = document.getElementById('vid');
    vid.srcObject = stream;
    await vid.play();
    requestAnimationFrame(sampleLoop);
    return true;
  } catch (e) {
    setStatus('Camera error: ' + e.message);
    return false;
  }
}

function sampleLoop() {
  const vid = document.getElementById('vid');
  if (vid.readyState >= 2) {
    const cnv = document.getElementById('cnv');
    const ctx = cnv.getContext('2d', { willReadFrequently: true });
    ctx.drawImage(vid, 0, 0, 160, 120);
    // Integrate 80% x 80% of the whole frame to average across rolling-shutter bands!
    const img = ctx.getImageData(16, 12, 128, 96).data;
    let rSum = 0, gSum = 0, bSum = 0, rLin = 0, gLin = 0, bLin = 0, n = img.length / 4;
    for (let i = 0; i < img.length; i += 4) {
      const r = img[i], g = img[i+1], b = img[i+2];
      rSum += r; gSum += g; bSum += b;
      rLin += srgbLin[r]; gLin += srgbLin[g]; bLin += srgbLin[b];
    }
    const rm = rSum / n, gm = gSum / n, bm = bSum / n;
    const yl = (0.2126 * rLin + 0.7152 * gLin + 0.0722 * bLin) / n;
    const clipped = Math.max(rm, gm, bm) >= 247;
    latestFrame = { t: performance.now(), y: yl, rgb: [rm, gm, bm], clipped };
    frameRing.push(latestFrame);
    if (frameRing.length > 240) frameRing.shift();
    document.getElementById('hud').innerHTML =
      `Y_lin: ${(yl*100).toFixed(2)} | sRGB: ${Math.round(rm)}/${Math.round(gm)}/${Math.round(bm)}` +
      (clipped ? ' <span style="color:#f85149">CLIP</span>' : ' <span style="color:#3fb950">OK</span>');
  }
  requestAnimationFrame(sampleLoop);
}

const sleep = ms => new Promise(r => setTimeout(r, ms));

async function tvApply(opts, withDump = false) {
  const r = await fetch('/api/apply', {
    method: 'POST', headers: {'Content-Type':'application/json'},
    body: JSON.stringify({ opts, dump: withDump })
  });
  const d = await r.json();
  if (d.temp_c !== null) document.getElementById('tempPill').textContent = 'Temp: ' + d.temp_c + 'C';
  return d;
}

function windowMean(tStart, tEnd) {
  const fs = frameRing.filter(f => f.t >= tStart && f.t <= tEnd);
  if (!fs.length) return { y: latestFrame.y, rgb: latestFrame.rgb, clipped: latestFrame.clipped, n: 1 };
  const sorted = [...fs].sort((a, b) => a.y - b.y);
  const lo = Math.floor(sorted.length * 0.15), hi = Math.max(lo + 1, Math.ceil(sorted.length * 0.85));
  const mid = sorted.slice(lo, hi);
  const y = mid.reduce((a, f) => a + f.y, 0) / mid.length;
  const r = mid.reduce((a, f) => a + f.rgb[0], 0) / mid.length;
  const g = mid.reduce((a, f) => a + f.rgb[1], 0) / mid.length;
  const b = mid.reduce((a, f) => a + f.rgb[2], 0) / mid.length;
  return { y, rgb: [r, g, b], clipped: mid.some(f => f.clipped), n: mid.length };
}

async function oneClickRun() {
  if (running) return;
  const ok = await startCamera();
  if (!ok) return;
  running = true; abortSweep = false;
  document.getElementById('goBtn').disabled = true;
  document.getElementById('stopBtn').disabled = false;
  const sweepKeys = Object.keys(SWEEPS);

  try {
    setStatus('Showing Full-Field White — aim green box so TV fills the frame...');
    await tvApply(SWEEPS[sweepKeys[0]].baseline_opts, true);
    for (let s = 3; s >= 1; s--) {
      if (abortSweep) break;
      setStatus(`Fill green box with white TV screen — starting in ${s}...`);
      await sleep(1000);
    }

    for (let si = 0; si < sweepKeys.length; si++) {
      if (abortSweep) break;
      curSweep = sweepKeys[si];
      document.getElementById('sweepSel').value = curSweep;
      render();
      const sw = SWEEPS[curSweep];
      const reps = sw.repeats || 3;

      await tvApply(sw.baseline_opts, si === 0);
      await sleep(800);

      for (let i = 0; i < sw.steps.length; i++) {
        if (abortSweep) break;
        const st = sw.steps[i];
        const runs = [], drifts = [];
        let lastRgb = [0,0,0], anyClip = false;

        for (let rep = 0; rep < reps; rep++) {
          if (abortSweep) break;
          setStatus(`[Sweep ${si+1}/${sweepKeys.length} · ${i+1}/${sw.steps.length}] ${st.label} (bracket ${rep+1}/${reps})...`);
          // Ensure we're settled on A (baseline)
          await sleep(250);
          const tA0End = performance.now();
          const a0 = windowMean(tA0End - 180, tA0End);

          // Step A -> B
          await tvApply(st.opts, false);
          const tBStart = performance.now() + 90;
          await sleep(260);
          const b = windowMean(tBStart, performance.now());

          // Step B -> A (symmetric bracket closes in ~0.85s!)
          await tvApply(sw.baseline_opts, false);
          const tA1Start = performance.now() + 90;
          await sleep(260);
          const a1 = windowMean(tA1Start, performance.now());

          const aMid = 0.5 * (a0.y + a1.y);
          const driftPct = aMid > 1e-5 ? Math.abs(a1.y - a0.y) / aMid * 100.0 : 99.0;
          const ratio = aMid > 1e-5 ? (b.y / aMid) * 100.0 : 100.0;
          runs.push(+ratio.toFixed(2));
          drifts.push(+driftPct.toFixed(1));
          lastRgb = b.rgb;
          anyClip = anyClip || b.clipped;
        }
        if (runs.length) {
          const mean = runs.reduce((a, b) => a + b, 0) / runs.length;
          const variance = runs.reduce((a, b) => a + (b - mean) ** 2, 0) / runs.length;
          const sd = Math.sqrt(variance);
          const meanDrift = drifts.reduce((a, b) => a + b, 0) / drifts.length;
          await postMeasurement(curSweep, st.id, {
            lum: +mean.toFixed(2),
            sd: +sd.toFixed(2),
            drift: +meanDrift.toFixed(1),
            rgb_srgb: lastRgb.map(v => +v.toFixed(1)),
            clipped: anyClip,
            runs
          });
        }
      }
    }

    setStatus(abortSweep ? 'Stopped — restoring Live HDMI...' : 'All sweeps complete! Restoring Live HDMI...');
    await tvApply('UNLOCK DRIVE:STANDARD DEEPBLACK:ON PEAKSTRETCH:ON DEEPBLACK_MODE:ON POWERSAVE:OFF APCSPEED:OFF SAPC:OFF SUBCONTRAST:132 DUTY2:168 OSD_GAMMA:0 GAMMA:2.4 CINEMA_SMOOTH:ON', true);
    setStatus(abortSweep ? 'Stopped & Live HDMI restored.' : 'All done! Live HDMI restored — check NULL controls & curve!');
  } catch (e) {
    setStatus('Error: ' + e.message);
  } finally {
    running = false;
    document.getElementById('goBtn').disabled = false;
    document.getElementById('stopBtn').disabled = true;
  }
}

async function postMeasurement(sweep, stepId, res) {
  const r = await fetch('/api/save_measurement', {
    method: 'POST', headers: {'Content-Type':'application/json'},
    body: JSON.stringify({ sweep, step_id: stepId, result: res })
  });
  const d = await r.json();
  MEAS = d.measurements;
  render();
}

async function clearSweep() {
  const r = await fetch('/api/clear', {
    method: 'POST', headers: {'Content-Type':'application/json'},
    body: JSON.stringify({ sweep: curSweep })
  });
  const d = await r.json();
  MEAS = d.measurements;
  render();
}

function setStatus(msg) { document.getElementById('status').textContent = msg; }

function render() {
  const sw = SWEEPS[curSweep];
  const steps = sw.steps;
  const sm = MEAS[curSweep] || {};
  const tbody = document.getElementById('tbl');
  tbody.innerHTML = '';
  const pts = [];

  steps.forEach(st => {
    const m = sm[st.id];
    const tr = document.createElement('tr');
    let lumStr = '--', sdStr = '--', drStr = '--', runsStr = '--';
    if (m) {
      pts.push({ x: st.x, label: st.id, lum: m.lum, clipped: m.clipped, isNull: st.id.startsWith('null') });
      lumStr = m.lum.toFixed(1) + '%';
      sdStr = '\u00b1' + (m.sd ?? 0).toFixed(1) + '%';
      drStr = (m.drift ?? 0).toFixed(1) + '%';
      runsStr = (m.runs || []).map(v => v.toFixed(0)).join('/');
    }
    tr.innerHTML = `<td>${st.label}</td><td class="${m && m.clipped ? 'clip' : 'good'}">${lumStr}</td><td>${sdStr}</td><td>${drStr}</td><td>${runsStr}</td>`;
    tbody.appendChild(tr);
  });

  const svg = document.getElementById('chart');
  if (!pts.length) {
    svg.innerHTML = '<text x="180" y="90" fill="#8b949e" font-size="13" text-anchor="middle">Tap green button once to run A->B->A bracketed sweep</text>';
    document.getElementById('summary').textContent = '';
    return;
  }
  const minX = Math.min(...steps.map(s => s.x)), maxX = Math.max(...steps.map(s => s.x));
  const minY = Math.min(95, ...pts.map(p => p.lum)) * 0.96;
  const maxY = Math.max(105, ...pts.map(p => p.lum)) * 1.04;
  const sx = x => 40 + ((x - minX) / (maxX - minX || 1)) * 302;
  const sy = y => 148 - ((y - minY) / (maxY - minY || 1)) * 128;

  let g = `<line x1="40" y1="148" x2="345" y2="148" stroke="#30363d"/><line x1="40" y1="18" x2="40" y2="148" stroke="#30363d"/>`;
  g += `<line x1="40" y1="${sy(100).toFixed(1)}" x2="345" y2="${sy(100).toFixed(1)}" stroke="#30363d" stroke-dasharray="4,4"/>`;
  g += `<text x="36" y="24" fill="#8b949e" font-size="10" text-anchor="end">${maxY.toFixed(0)}%</text>`;
  g += `<text x="36" y="148" fill="#8b949e" font-size="10" text-anchor="end">${minY.toFixed(0)}%</text>`;
  if (pts.length > 1) {
    const path = pts.map((p, i) => (i ? 'L' : 'M') + sx(p.x).toFixed(1) + ',' + sy(p.lum).toFixed(1)).join(' ');
    g += `<path d="${path}" fill="none" stroke="#58a6ff" stroke-width="2.5"/>`;
  }
  pts.forEach(p => {
    const cx = sx(p.x).toFixed(1), cy = sy(p.lum).toFixed(1);
    const col = p.isNull ? '#d29922' : (p.clipped ? '#f85149' : '#3fb950');
    g += `<circle cx="${cx}" cy="${cy}" r="4.5" fill="${col}"/><text x="${cx}" y="164" fill="#8b949e" font-size="9.5" text-anchor="middle">${p.label}</text>`;
  });
  svg.innerHTML = g;
}
init();
</script></body></html>
"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def _json(self, obj, code=200):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        p = urlparse(self.path).path
        if p == "/":
            data = HTML.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        elif p == "/api/state":
            with STATE_LOCK:
                self._json({
                    "sweeps": SWEEPS,
                    "measurements": STATE["measurements"],
                    "temp_c": STATE["temp_c"],
                })
        else:
            self.send_error(404)

    def do_POST(self):
        p = urlparse(self.path).path
        n = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(n).decode() or "{}")
        if p == "/api/apply":
            log, temp_c, ms = tv_apply(body["opts"], with_dump=bool(body.get("dump")))
            self._json({"ok": True, "temp_c": temp_c, "ms": ms})
        elif p == "/api/save_measurement":
            with STATE_LOCK:
                res = body["result"]
                res["temp_c"] = STATE["temp_c"]
                res["ts"] = int(time.time())
                STATE["measurements"].setdefault(body["sweep"], {})[body["step_id"]] = res
                save_results()
                meas = STATE["measurements"]
            self._json({"measurements": meas})
        elif p == "/api/clear":
            with STATE_LOCK:
                STATE["measurements"].pop(body["sweep"], None)
                save_results()
                meas = STATE["measurements"]
            self._json({"measurements": meas})
        else:
            self.send_error(404)


def ensure_cert(cert_path, key_path, host_ip):
    if os.path.exists(cert_path) and os.path.exists(key_path):
        return
    subprocess.run([
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", key_path, "-out", cert_path, "-days", "365",
        "-subj", f"/CN={host_ip}",
        "-addext", f"subjectAltName=IP:{host_ip}",
    ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def detect_local_ip(target_host):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect((target_host, TV_PORT))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def main():
    global TV_HOST, LIB_PATH, TV
    ap = argparse.ArgumentParser(description="A->B->A bracketed smartphone photometer for PN60F5500 libPDPTune")
    ap.add_argument("--tv", required=True, help="TV IP address (telnet root shell on port 23)")
    ap.add_argument("--lib", default=LIB_PATH, help="libPDPTune.so path on the TV")
    ap.add_argument("--port", type=int, default=8443, help="HTTPS port to bind")
    ap.add_argument("--host-ip", default="", help="Server LAN IP for self-signed TLS SAN (auto-detected if omitted)")
    args = ap.parse_args()
    TV_HOST = args.tv
    LIB_PATH = args.lib
    host_ip = args.host_ip or detect_local_ip(TV_HOST)
    TV = PersistentTV(TV_HOST, TV_PORT)
    load_results()
    cert, key = "/tmp/optical_sweep.crt", "/tmp/optical_sweep.key"
    ensure_cert(cert, key, host_ip)
    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=cert, keyfile=key)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print(f"PN60F5500 Bracketed Photometer v3 listening on https://{host_ip}:{args.port}/ (TV={TV_HOST})", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
