#!/usr/bin/env python3
"""
jellyfin_arc_sync.py - Content-aware automatic Stereo PCM <-> Dolby Digital 5.1
HDMI-ARC switcher for Samsung PN60F5500 (libArcToggle 2.0.0) driven by a
Jellyfin server's local /Sessions API.

Solves the Xbox global-bitstream limitation cleanly without DSP tapping:
  - Stereo tracks (Channels <= 2, e.g. 2.0 AAC/Opus/FLAC) -> libArcToggle PCM
    so the soundbar receives true Stereo PCM 2.0 instead of re-encoded 5.1.
  - Multichannel tracks (Channels >= 3, e.g. 5.1/7.1)     -> libArcToggle DD
    so the soundbar receives true Dolby Digital 5.1 over HDMI-ARC.
  - Paused playback holds the active ARC mode (no audio dropout on unpause).
  - Unknown / idle / stopped (outside Jellyfin or missing channel metadata)
    -> defaults back to Dolby Digital 5.1 (DD) after --idle-delay seconds.

Manual DVR remote presses are never overridden mid-item: switching triggers
only when playback transitions to a new item/audio track or returns to idle.
"""
import argparse
import http.client
import json
import os
import re
import signal
import socket
import sys
import threading
import time
import urllib.parse
import urllib.request

STOP_EVENT = threading.Event()


def handle_sig(signum, frame):
    STOP_EVENT.set()


def tv_set_arc(tv_host, tv_port, mode, key_opt="KEY:DVR", timeout=10.0):
    """Invoke libArcToggle on exeAPP via the TV's SamyGO telnet shell."""
    word = "DD" if mode == "DD" else "PCM"
    cmd = (
        ". /dtv/SGO.env\n"
        "rm -f /dtv/ArcToggle.log\n"
        f"samyGOso -n exeAPP -B -r -l /mnt/opt/privateer/usr/libso/libArcToggle.so {key_opt} {word} > /dev/null\n"
        "sleep 1\n"
        "cat /dtv/ArcToggle.log\n"
        "echo __E''ND__\nexit\n"
    )
    s = socket.create_connection((tv_host, tv_port), 5)
    s.settimeout(0.8)

    def read_until(marker, deadline):
        out = b""
        while time.monotonic() < deadline and (marker is None or marker not in out):
            try:
                chunk = s.recv(65536)
                if not chunk:
                    break
                out += chunk
            except socket.timeout:
                if marker is None:
                    break
        return out

    try:
        read_until(None, time.monotonic() + 2.0)
        s.sendall(b"stty -echo 2>/dev/null; PS1=; PS2=\n")
        read_until(None, time.monotonic() + 1.0)
        s.sendall(cmd.encode())
        out = read_until(b"__END__", time.monotonic() + timeout)
    finally:
        s.close()
    text = re.sub(rb"\xff[\xfb-\xfe].|\xff\xfa.*?\xff\xf0|\r", b"", out, flags=re.S).decode(errors="replace")
    return text.replace("__END__", "").strip()


def beam_audio_code(beam_host):
    if not beam_host:
        return None
    try:
        with urllib.request.urlopen(f"http://{beam_host}:1400/status/zp", timeout=2.5) as r:
            body = r.read().decode(errors="replace")
        m = re.search(r"<HTAudioInCode>([^<]+)</HTAudioInCode>", body)
        return m.group(1) if m else None
    except Exception:
        return None


def beam_mode_from_code(code):
    if code == "33554434":
        return "PCM"
    if code == "84934713":
        return "DD"
    return None


class JellyfinClient:
    """Persistent HTTP/1.1 Keep-Alive client for low-overhead /Sessions polling."""

    def __init__(self, base_url, api_key, timeout=4.0):
        u = urllib.parse.urlparse(base_url.rstrip("/"))
        self.scheme = u.scheme or "http"
        self.host = u.hostname or "127.0.0.1"
        self.port = u.port or (443 if self.scheme == "https" else 80)
        self.path = f"{u.path}/Sessions"
        self.timeout = timeout
        self.headers = {
            "Authorization": f'MediaBrowser Token="{api_key}"',
            "Accept": "application/json",
            "Connection": "keep-alive",
        }
        self._conn = None

    def _connect(self):
        if self._conn is None:
            cls = http.client.HTTPSConnection if self.scheme == "https" else http.client.HTTPConnection
            self._conn = cls(self.host, self.port, timeout=self.timeout)
        return self._conn

    def close(self):
        if self._conn is not None:
            try:
                self._conn.close()
            except Exception:
                pass
            self._conn = None

    def sessions(self):
        for attempt in range(2):
            conn = self._connect()
            try:
                conn.request("GET", self.path, headers=self.headers)
                resp = conn.getresponse()
                body = resp.read()
                if resp.status != 200:
                    raise RuntimeError(f"Jellyfin HTTP {resp.status}")
                return json.loads(body.decode())
            except (http.client.HTTPException, OSError):
                self.close()
                if attempt == 1:
                    raise


def inspect_sessions(sessions, device_filter):
    """Return ('playing', track_key, channels, title, dev) | ('paused',) | ('idle',)."""
    filt = (device_filter or "").lower()
    saw_paused = False
    for s in sessions:
        item = s.get("NowPlayingItem")
        if not item:
            continue
        dev = f"{s.get('DeviceName', '')} {s.get('Client', '')}".strip()
        if filt and filt not in dev.lower():
            continue
        ps = s.get("PlayState") or {}
        if ps.get("IsPaused"):
            saw_paused = True
            continue

        item_id = item.get("Id", "")
        title = item.get("Name", "unknown")
        aidx = ps.get("AudioStreamIndex")
        channels = None
        audio_streams = [st for st in (item.get("MediaStreams") or []) if st.get("Type") == "Audio"]

        if aidx is not None:
            for st in audio_streams:
                if st.get("Index") == aidx:
                    channels = st.get("Channels")
                    break
        if channels is None:
            ti = s.get("TranscodingInfo") or {}
            channels = ti.get("AudioChannels")
        if channels is None and audio_streams:
            default_st = next((st for st in audio_streams if st.get("IsDefault")), audio_streams[0])
            channels = default_st.get("Channels")

        return ("playing", (item_id, aidx), int(channels) if channels is not None else None, title, dev)
    return ("paused",) if saw_paused else ("idle",)


def main():
    ap = argparse.ArgumentParser(description="Auto-switch PN60F5500 HDMI-ARC PCM/DD from Jellyfin audio channels")
    ap.add_argument("--jellyfin", default="http://127.0.0.1:8096", help="Jellyfin base URL")
    ap.add_argument("--api-key", default=os.environ.get("JELLYFIN_API_KEY", ""), help="Jellyfin API key (or JELLYFIN_API_KEY env)")
    ap.add_argument("--tv", required=True, help="Samsung TV IP address (SamyGO telnet port 23)")
    ap.add_argument("--tv-port", type=int, default=23)
    ap.add_argument("--device", default="xbox", help="Case-insensitive substring matching DeviceName/Client (empty = any)")
    ap.add_argument("--beam", default="", help="Optional Sonos Beam IP to check/log live HTAudioInCode")
    ap.add_argument("--key", default="KEY:DVR", help="Keep this libArcToggle key armed on injection")
    ap.add_argument("--interval", type=float, default=2.0, help="Poll interval in seconds")
    ap.add_argument("--idle-delay", type=float, default=10.0, help="Seconds stopped/unknown before defaulting back to DD")
    ap.add_argument("--dry-run", action="store_true", help="Print decisions without calling the TV")
    ap.add_argument("--once", action="store_true", help="Check once and exit")
    args = ap.parse_args()

    if not args.api_key:
        sys.exit("error: provide --api-key or set JELLYFIN_API_KEY (Jellyfin Dashboard -> API Keys)")

    signal.signal(signal.SIGINT, handle_sig)
    signal.signal(signal.SIGTERM, handle_sig)

    jf = JellyfinClient(args.jellyfin, args.api_key)
    last_key = None
    last_mode = None
    idle_since = None
    tv_down_logged = False
    last_err = None

    def apply_mode(want, reason):
        nonlocal last_mode, tv_down_logged
        live_code = beam_audio_code(args.beam) if args.beam else None
        live_mode = beam_mode_from_code(live_code)
        if live_mode == want:
            print(f"[arc-sync] {reason} -> already {want} (beam={live_code})", flush=True)
            last_mode = want
            return True
        if live_mode is None and want == last_mode:
            print(f"[arc-sync] {reason} -> already {want}", flush=True)
            return True

        print(f"[arc-sync] {reason} -> {want}", flush=True)
        if args.dry_run:
            last_mode = want
            return True
        try:
            out = tv_set_arc(args.tv, args.tv_port, want, key_opt=args.key)
            last_mode = want
            tv_down_logged = False
            code = beam_audio_code(args.beam)
            extra = f" beam={code}" if code else ""
            print(f"[arc-sync] TV: {' | '.join(out.splitlines())}{extra}", flush=True)
            return True
        except OSError as e:
            if not tv_down_logged:
                print(f"[arc-sync] TV unreachable ({e}); will retry on state change", flush=True)
                tv_down_logged = True
            return False

    print(
        f"[arc-sync] watching {args.jellyfin} (device~='{args.device}') -> TV {args.tv} "
        f"(idle/unknown -> DD after {args.idle_delay:.0f}s)",
        flush=True,
    )
    try:
        while not STOP_EVENT.is_set():
            try:
                sessions = jf.sessions()
                if last_err is not None:
                    print("[arc-sync] Jellyfin connection restored", flush=True)
                    last_err = None
                state = inspect_sessions(sessions, args.device)
                if state[0] == "playing":
                    idle_since = None
                    _, track_key, channels, title, dev = state
                    if channels is None:
                        want = "DD"
                        desc = f"{dev}: '{title}' (unknown ch -> default DD)"
                    else:
                        want = "PCM" if channels <= 2 else "DD"
                        desc = f"{dev}: '{title}' ({channels}ch)"
                    if track_key != last_key:
                        if apply_mode(want, desc):
                            last_key = track_key
                elif state[0] == "paused":
                    # Hold active ARC mode across pauses so unpausing stereo never drops audio.
                    idle_since = None
                else:
                    now = time.monotonic()
                    if idle_since is None:
                        idle_since = now
                    if last_key != "__IDLE__" and (args.once or (now - idle_since) >= args.idle_delay):
                        if apply_mode("DD", "idle/unknown source"):
                            last_key = "__IDLE__"
            except Exception as e:
                err_str = str(e)
                if err_str != last_err:
                    print(f"[arc-sync] poll error: {err_str}", file=sys.stderr, flush=True)
                    last_err = err_str

            if args.once:
                break
            STOP_EVENT.wait(args.interval)
    finally:
        jf.close()


if __name__ == "__main__":
    main()
