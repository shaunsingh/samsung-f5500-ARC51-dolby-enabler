#!/usr/bin/env python3
"""
optical_sweep.py - Hands-free live-video smartphone photometer & automated
optical sweep controller for Samsung PN60F5500 (libPDPTune 2.5.0).

Uses Full-Field White (PATTERN:WHITE, 0x8114=0x0101) so smartphone cameras
meter cleanly in the linear mid-range (~140-210 sRGB, zero sensor clipping),
with a median-filtered post-transition window (tDone+60ms..tDone+360ms) that
rejects both 1-frame LogicFHD blanking and 60Hz/600Hz plasma rolling-shutter
beat. Runs 2 passes per step and reports mean +/- spread.
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
RESULTS_PATH = "/tmp/optical_sweep_results.json"

BASE_OPTS = (
    "UNLOCK DRIVE:STANDARD DEEPBLACK:ON PEAKSTRETCH:ON "
    "DEEPBLACK_MODE:ON POWERSAVE:OFF APCSPEED:OFF SAPC:OFF "
    "OSD_GAMMA:0 GAMMA:2.4"
)

SWEEPS = {
    "duty2_full": {
        "title": "1. DUTY2 Optical Curve (Full-Field White, 2x avg)",
        "roi": "center",
        "repeats": 2,
        "baseline_opts": f"{BASE_OPTS} SUBCONTRAST:132 DUTY2:128 PATTERN:WHITE",
        "steps": [
            {"id": str(v), "x": v, "label": f"DUTY2:{v}",
             "opts": f"{BASE_OPTS} SUBCONTRAST:132 DUTY2:{v} PATTERN:WHITE"}
            for v in (128, 139, 155, 175, 195, 215, 235, 255)
        ],
    },
    "modes_full": {
        "title": "2. Drive / DeepBlackMode / PeakStretch / SubContrast (Full White)",
        "roi": "center",
        "repeats": 2,
        "baseline_opts": "UNLOCK SUBCONTRAST:128 DUTY2:128 OSD_GAMMA:0 GAMMA:2.4 PATTERN:WHITE",
        "steps": [
            {"id": "movie_128", "x": 1, "label": "Stock Movie 128",
             "opts": "UNLOCK SUBCONTRAST:128 DUTY2:128 OSD_GAMMA:0 GAMMA:2.4 PATTERN:WHITE"},
            {"id": "std_128", "x": 2, "label": "DRIVE:STD 128",
             "opts": f"{BASE_OPTS} SUBCONTRAST:132 DUTY2:128 PATTERN:WHITE"},
            {"id": "std_200_dbm0", "x": 3, "label": "STD200 DBMode:0",
             "opts": "UNLOCK DRIVE:STANDARD DEEPBLACK:ON PEAKSTRETCH:ON DEEPBLACK_MODE:OFF POWERSAVE:OFF APCSPEED:OFF SAPC:OFF SUBCONTRAST:132 DUTY2:200 OSD_GAMMA:0 GAMMA:2.4 PATTERN:WHITE"},
            {"id": "std_200_dbm1", "x": 4, "label": "STD200 DBMode:1",
             "opts": f"{BASE_OPTS} SUBCONTRAST:132 DUTY2:200 PATTERN:WHITE"},
            {"id": "std_200_ps0", "x": 5, "label": "STD200 PeakStr:0",
             "opts": "UNLOCK DRIVE:STANDARD DEEPBLACK:ON PEAKSTRETCH:OFF DEEPBLACK_MODE:ON POWERSAVE:OFF APCSPEED:OFF SAPC:OFF SUBCONTRAST:132 DUTY2:200 OSD_GAMMA:0 GAMMA:2.4 PATTERN:WHITE"},
            {"id": "std_200_sub150", "x": 6, "label": "STD200 SubC:150",
             "opts": f"{BASE_OPTS} SUBCONTRAST:150 DUTY2:200 PATTERN:WHITE"},
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
        cmd += "; sleep 0.5; cat /dtv/PDPTune.log"
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
<title>PN60F5500 Optical Photometer v2</title>
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
  button.primary { background:#1f6feb; border-color:#388bfd; font-weight:700; }
  button.ok { background:#238636; border-color:#2ea043; font-weight:700; }
  button.warn { background:#9e6a03; border-color:#d29922; font-weight:600; }
  button.bad { background:#b62324; border-color:#f85149; font-weight:700; }
  button:disabled { opacity:.45; }
  .vwrap { position:relative; width:100%; max-height:200px; overflow:hidden; border-radius:8px; background:#000; border:1px solid var(--bd); }
  video { width:100%; height:200px; object-fit:cover; display:block; }
  .reticle { position:absolute; left:50%; top:50%; width:42%; height:44%; transform:translate(-50%,-50%);
    border:2px solid #3fb950; box-shadow:0 0 0 9999px rgba(0,0,0,.22); pointer-events:none; border-radius:4px; }
  .hud { position:absolute; left:8px; bottom:8px; background:rgba(0,0,0,.74); padding:4px 8px; border-radius:6px; font-family:monospace; font-size:12px; }
  .pill { font-size:12px; padding:2px 8px; border-radius:12px; background:#21262d; border:1px solid var(--bd); }
  table { width:100%; border-collapse:collapse; font-size:12.5px; margin-top:6px; }
  th, td { padding:5px 4px; border-bottom:1px solid var(--bd); text-align:right; }
  th:first-child, td:first-child { text-align:left; }
  .clip { color:var(--bad); font-weight:700; }
  .good { color:var(--ok); font-weight:700; }
  svg { width:100%; height:175px; background:#090c10; border-radius:8px; border:1px solid var(--bd); margin-top:6px; }
  #status { font-size:13px; color:#c9d1d9; margin-top:6px; line-height:1.35; }
</style></head><body>
<h1>PN60F5500 Unclipped Full-White Photometer (v2)</h1>

<div class="card">
  <div class="vwrap">
    <video id="vid" autoplay playsinline muted></video>
    <div class="reticle"></div>
    <div class="hud" id="hud">Ready</div>
  </div>
  <div class="row" style="margin-top:8px">
    <button id="goBtn" class="ok" style="flex:1;padding:13px;font-size:15px" onclick="oneClickRun()">&#9654; Start Camera &amp; Run Full Sweep (~60s)</button>
    <button id="stopBtn" class="bad" onclick="abortSweep = true" disabled>Stop</button>
  </div>
  <div class="row" style="margin-top:8px;justify-content:space-between">
    <select id="sweepSel" style="flex:1"></select>
    <span class="pill" id="aePill">AE: --</span>
    <span class="pill" id="tempPill">Temp: --</span>
  </div>
  <div id="status">Tap the green button once and hold your phone facing the TV (~60s). It automatically opens the camera, runs both unclipped Full-White sweeps, and restores Live HDMI!</div>
</div>

<div class="card">
  <div class="row" style="justify-content:space-between">
    <strong>Measured Luminance Curve (Median-Filtered)</strong>
    <button onclick="clearSweep()" style="padding:4px 8px;font-size:12px">Clear</button>
  </div>
  <svg id="chart" viewBox="0 0 360 175"></svg>
  <div id="summary" style="margin-top:6px;font-size:13px"></div>
  <table>
    <thead><tr><th>Step</th><th>Rel Lum</th><th>+/-</th><th>vs Base</th><th>sRGB</th></tr></thead>
    <tbody id="tbl"></tbody>
  </table>
</div>

<canvas id="cnv" width="160" height="120" style="display:none"></canvas>

<script>
let SWEEPS = {}, MEAS = {}, curSweep = 'duty2_full';
let stream = null, track = null, aeLocked = false, running = false, abortSweep = false;
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
    track = stream.getVideoTracks()[0];
    await tryLockAE();
    requestAnimationFrame(sampleLoop);
    return true;
  } catch (e) {
    setStatus('Camera error: ' + e.message);
    return false;
  }
}

async function oneClickRun() {
  if (running) return;
  const ok = await startCamera();
  if (!ok) return;
  await runAllSweeps();
}

async function tryLockAE() {
  aeLocked = false;
  if (track && track.getCapabilities) {
    const caps = track.getCapabilities();
    const adv = {};
    if (caps.exposureMode && caps.exposureMode.includes('manual')) adv.exposureMode = 'manual';
    if (caps.whiteBalanceMode && caps.whiteBalanceMode.includes('manual')) adv.whiteBalanceMode = 'manual';
    if (Object.keys(adv).length > 0) {
      try {
        await track.applyConstraints({ advanced: [adv] });
        if (adv.exposureMode === 'manual') aeLocked = true;
      } catch (e) {}
    }
  }
  document.getElementById('aePill').textContent = aeLocked ? 'AE: HW-LOCKED' : 'AE: MEDIAN-STEP';
  setStatus('Camera ready (' + (aeLocked ? 'HW AE-Locked' : 'Median Step-Diff') + '). Fill green box with the TV screen and tap Run Unclipped 2x Sweep.');
}

function sampleLoop() {
  const vid = document.getElementById('vid');
  if (vid.readyState >= 2) {
    const cnv = document.getElementById('cnv');
    const ctx = cnv.getContext('2d', { willReadFrequently: true });
    ctx.drawImage(vid, 0, 0, 160, 120);
    // Large 68x52 center box averages across plasma rolling-shutter scanlines cleanly!
    const img = ctx.getImageData(46, 34, 68, 52).data;
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
      (clipped ? ' <span style="color:#f85149">CLIP!</span>' : ' <span style="color:#3fb950">OK</span>');
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

async function quickPattern(pat) {
  const opts = pat === 'OFF'
    ? 'UNLOCK DRIVE:STANDARD DEEPBLACK:ON PEAKSTRETCH:ON DEEPBLACK_MODE:ON POWERSAVE:OFF APCSPEED:OFF SAPC:OFF SUBCONTRAST:132 DUTY2:200 OSD_GAMMA:0 GAMMA:2.4'
    : 'UNLOCK DRIVE:STANDARD DEEPBLACK:ON PEAKSTRETCH:ON DEEPBLACK_MODE:ON POWERSAVE:OFF APCSPEED:OFF SAPC:OFF SUBCONTRAST:132 DUTY2:200 OSD_GAMMA:0 GAMMA:2.4 PATTERN:' + pat;
  setStatus('Setting TV pattern: ' + pat + '...');
  await tvApply(opts, true);
  setStatus('TV pattern: ' + pat);
}

function medianStats(tStart, tEnd) {
  const fs = frameRing.filter(f => f.t >= tStart && f.t <= tEnd);
  if (!fs.length) return { y: latestFrame.y, rgb: latestFrame.rgb, clipped: latestFrame.clipped, n: 1 };
  const sorted = [...fs].sort((a, b) => a.y - b.y);
  // Trimmed mean (drop lowest 20% & highest 20% to kill both blanking dips and rolling-shutter spikes)
  const lo = Math.floor(sorted.length * 0.2), hi = Math.max(lo + 1, Math.ceil(sorted.length * 0.8));
  const mid = sorted.slice(lo, hi);
  const y = mid.reduce((a, f) => a + f.y, 0) / mid.length;
  const r = mid.reduce((a, f) => a + f.rgb[0], 0) / mid.length;
  const g = mid.reduce((a, f) => a + f.rgb[1], 0) / mid.length;
  const b = mid.reduce((a, f) => a + f.rgb[2], 0) / mid.length;
  return { y, rgb: [r, g, b], clipped: mid.some(f => f.clipped), n: mid.length };
}

async function runAllSweeps() {
  if (running) return;
  running = true; abortSweep = false;
  document.getElementById('goBtn').disabled = true;
  document.getElementById('stopBtn').disabled = false;
  const sweepKeys = Object.keys(SWEEPS);

  try {
    setStatus('Showing Full-Field White — aim green box at TV (starting in 3s)...');
    await tvApply(SWEEPS[sweepKeys[0]].baseline_opts, true);
    for (let s = 3; s >= 1; s--) {
      if (abortSweep) break;
      setStatus(`Aim green box at white TV screen — starting in ${s}...`);
      await sleep(1000);
    }

    for (let si = 0; si < sweepKeys.length; si++) {
      if (abortSweep) break;
      curSweep = sweepKeys[si];
      document.getElementById('sweepSel').value = curSweep;
      render();
      const sw = SWEEPS[curSweep];
      const reps = sw.repeats || 2;

      setStatus(`[Sweep ${si+1}/${sweepKeys.length}] Settling baseline...`);
      await tvApply(sw.baseline_opts, si === 0);
      await sleep(1200);
      if (aeLocked) await tryLockAE();
      await sleep(300);

      for (let i = 0; i < sw.steps.length; i++) {
        if (abortSweep) break;
        const st = sw.steps[i];

        if (i === 0 && !aeLocked) {
          const now = performance.now();
          const pre = medianStats(now - 450, now);
          await postMeasurement(curSweep, st.id, {
            lum: 100.0, spread: 0, rgb_srgb: pre.rgb.map(v => +v.toFixed(1)),
            clipped: pre.clipped, mode: 'median-step', runs: [100.0]
          });
          continue;
        }

        const runs = [];
        let lastRgb = [0,0,0], anyClip = false;
        for (let rep = 0; rep < reps; rep++) {
          if (abortSweep) break;
          if (aeLocked) {
            setStatus(`[Sweep ${si+1}/${sweepKeys.length} · ${i+1}/${sw.steps.length}] ${st.label} (${rep+1}/${reps})...`);
            await tvApply(st.opts, false);
            await sleep(300);
            const t0 = performance.now();
            await sleep(450);
            const m = medianStats(t0, performance.now());
            runs.push(m.y * 100.0);
            lastRgb = m.rgb; anyClip = anyClip || m.clipped;
          } else {
            if (i > 0 || rep > 0) {
              setStatus(`[Sweep ${si+1}/${sweepKeys.length} · ${i+1}/${sw.steps.length}] ${st.label} (${rep+1}/${reps}): baseline...`);
              await tvApply(sw.baseline_opts, false);
              await sleep(1150);
            }
            const preEnd = performance.now();
            const pre = medianStats(preEnd - 450, preEnd);
            setStatus(`[Sweep ${si+1}/${sweepKeys.length} · ${i+1}/${sw.steps.length}] ${st.label} (${rep+1}/${reps}): measuring...`);
            await tvApply(st.opts, false);
            const tPost0 = performance.now() + 80;
            await sleep(320);
            const post = medianStats(tPost0, performance.now());
            const ratio = pre.y > 1e-5 ? (post.y / pre.y) * 100.0 : 100.0;
            runs.push(+ratio.toFixed(2));
            lastRgb = post.rgb; anyClip = anyClip || post.clipped;
          }
        }
        if (runs.length) {
          const mean = runs.reduce((a, b) => a + b, 0) / runs.length;
          const spread = runs.length > 1 ? Math.abs(runs[1] - runs[0]) / 2 : 0;
          await postMeasurement(curSweep, st.id, {
            lum: +mean.toFixed(2),
            spread: +spread.toFixed(2),
            rgb_srgb: lastRgb.map(v => +v.toFixed(1)),
            clipped: anyClip,
            mode: aeLocked ? 'hw-lock' : 'median-step',
            runs
          });
        }
      }
    }

    setStatus(abortSweep ? 'Stopped — restoring Live HDMI...' : 'All sweeps complete! Restoring Live HDMI...');
    await tvApply('UNLOCK DRIVE:STANDARD DEEPBLACK:ON PEAKSTRETCH:ON DEEPBLACK_MODE:ON POWERSAVE:OFF APCSPEED:OFF SAPC:OFF SUBCONTRAST:132 DUTY2:155 OSD_GAMMA:0 GAMMA:2.4 CINEMA_SMOOTH:ON', true);
    setStatus(abortSweep ? 'Stopped & Live HDMI restored.' : 'All done! Live HDMI restored — you can put the phone down!');
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
  const baseM = sm[steps[0].id];
  const baseL = baseM ? baseM.lum : null;
  const tbody = document.getElementById('tbl');
  tbody.innerHTML = '';
  const pts = [];

  steps.forEach(st => {
    const m = sm[st.id];
    const tr = document.createElement('tr');
    let pct = '--', rgb = '--', lumStr = '--', sp = '--';
    if (m) {
      pts.push({ x: st.x, label: st.id, lum: m.lum, clipped: m.clipped });
      lumStr = m.lum.toFixed(1) + (m.clipped ? '!' : '');
      sp = m.spread !== undefined ? ('\u00b1' + m.spread.toFixed(1)) : '--';
      if (baseL && baseL > 0) {
        const dp = ((m.lum / baseL) - 1) * 100;
        pct = (dp >= 0 ? '+' : '') + dp.toFixed(1) + '%';
      }
      if (m.rgb_srgb) rgb = m.rgb_srgb.map(Math.round).join('/');
    }
    tr.innerHTML = `<td>${st.label}</td><td class="${m && m.clipped ? 'clip' : ''}">${lumStr}</td><td>${sp}</td><td class="good">${pct}</td><td>${rgb}</td>`;
    tbody.appendChild(tr);
  });

  const svg = document.getElementById('chart');
  if (!pts.length) {
    svg.innerHTML = '<text x="180" y="90" fill="#8b949e" font-size="13" text-anchor="middle">Hold phone at TV and tap Run Unclipped 2x Sweep</text>';
    document.getElementById('summary').textContent = '';
    return;
  }
  const minX = Math.min(...steps.map(s => s.x)), maxX = Math.max(...steps.map(s => s.x));
  const minY = Math.min(...pts.map(p => p.lum)) * 0.94;
  const maxY = Math.max(...pts.map(p => p.lum)) * 1.05 || 1;
  const sx = x => 40 + ((x - minX) / (maxX - minX || 1)) * 302;
  const sy = y => 148 - ((y - minY) / (maxY - minY || 1)) * 128;

  let g = `<line x1="40" y1="148" x2="345" y2="148" stroke="#30363d"/><line x1="40" y1="18" x2="40" y2="148" stroke="#30363d"/>`;
  g += `<text x="36" y="24" fill="#8b949e" font-size="10" text-anchor="end">${maxY.toFixed(1)}</text>`;
  g += `<text x="36" y="148" fill="#8b949e" font-size="10" text-anchor="end">${minY.toFixed(1)}</text>`;
  if (pts.length > 1) {
    const path = pts.map((p, i) => (i ? 'L' : 'M') + sx(p.x).toFixed(1) + ',' + sy(p.lum).toFixed(1)).join(' ');
    g += `<path d="${path}" fill="none" stroke="#58a6ff" stroke-width="2.5"/>`;
  }
  pts.forEach(p => {
    const cx = sx(p.x).toFixed(1), cy = sy(p.lum).toFixed(1);
    const col = p.clipped ? '#f85149' : '#3fb950';
    g += `<circle cx="${cx}" cy="${cy}" r="4.5" fill="${col}"/><text x="${cx}" y="164" fill="#8b949e" font-size="10" text-anchor="middle">${p.label}</text>`;
  });
  svg.innerHTML = g;

  const peak = pts.reduce((a, b) => b.lum > a.lum ? b : a, pts[0]);
  const knee = pts.find(p => p.lum >= peak.lum * 0.985);
  document.getElementById('summary').innerHTML =
    `Peak: <strong>${peak.label}</strong> (${peak.lum.toFixed(1)})` +
    (knee ? ` &middot; 98.5% knee: <strong class="good">${knee.label}</strong>` : '');
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
    ap = argparse.ArgumentParser(description="Hands-free smartphone photometer for PN60F5500 libPDPTune")
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
    print(f"PN60F5500 Photometer listening on https://{host_ip}:{args.port}/ (TV={TV_HOST})", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
