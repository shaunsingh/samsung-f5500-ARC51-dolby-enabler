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

Manual DVR remote presses are never overridden mid-item: switching triggers
only when a new media item or audio stream index begins playing.
"""
import argparse
import json
import os
import re
import signal
import socket
import sys
import time
import urllib.error
import urllib.request

STOP = False


def handle_sig(signum, frame):
    global STOP
    STOP = True


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
        while time.time() < deadline and (marker is None or marker not in out):
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
        read_until(None, time.time() + 2.0)
        s.sendall(b"stty -echo 2>/dev/null; PS1=; PS2=\n")
        read_until(None, time.time() + 1.0)
        s.sendall(cmd.encode())
        out = read_until(b"__END__", time.time() + timeout)
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


def jellyfin_sessions(base_url, api_key):
    req = urllib.request.Request(
        f"{base_url.rstrip('/')}/Sessions",
        headers={"X-Emby-Token": api_key, "Accept": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=4.0) as r:
        return json.loads(r.read().decode())


def active_audio_info(sessions, device_filter):
    """Return (track_key, channels, title, device) for the matching playing session."""
    filt = (device_filter or "").lower()
    for s in sessions:
        item = s.get("NowPlayingItem")
        if not item:
            continue
        ps = s.get("PlayState") or {}
        if ps.get("IsPaused"):
            continue
        dev = f"{s.get('DeviceName', '')} {s.get('Client', '')}".strip()
        if filt and filt not in dev.lower():
            continue

        item_id = item.get("Id", "")
        title = item.get("Name", "unknown")
        aidx = ps.get("AudioStreamIndex")
        channels = None

        # Prefer the selected source audio stream's true channel count
        for st in item.get("MediaStreams") or []:
            if st.get("Type") != "Audio":
                continue
            if aidx is None or st.get("Index") == aidx:
                channels = st.get("Channels")
                if aidx is not None:
                    break
        # Fall back to transcoding channel count if source metadata lacked Channels
        if channels is None:
            ti = s.get("TranscodingInfo") or {}
            channels = ti.get("AudioChannels")

        if channels is not None:
            return (item_id, aidx), int(channels), title, dev
    return None


def main():
    ap = argparse.ArgumentParser(description="Auto-switch PN60F5500 HDMI-ARC PCM/DD from Jellyfin audio channels")
    ap.add_argument("--jellyfin", default="http://127.0.0.1:8096", help="Jellyfin base URL")
    ap.add_argument("--api-key", default=os.environ.get("JELLYFIN_API_KEY", ""), help="Jellyfin API key (or JELLYFIN_API_KEY env)")
    ap.add_argument("--tv", required=True, help="Samsung TV IP address (SamyGO telnet port 23)")
    ap.add_argument("--tv-port", type=int, default=23)
    ap.add_argument("--device", default="xbox", help="Case-insensitive substring matching DeviceName/Client (empty = any)")
    ap.add_argument("--beam", default="", help="Optional Sonos Beam IP to log verified HTAudioInCode")
    ap.add_argument("--key", default="KEY:DVR", help="Keep this libArcToggle key armed on injection")
    ap.add_argument("--interval", type=float, default=2.0, help="Poll interval in seconds")
    ap.add_argument("--dry-run", action="store_true", help="Print decisions without calling the TV")
    ap.add_argument("--once", action="store_true", help="Check once and exit")
    args = ap.parse_args()

    if not args.api_key:
        sys.exit("error: provide --api-key or set JELLYFIN_API_KEY (Jellyfin Dashboard -> API Keys)")

    signal.signal(signal.SIGINT, handle_sig)
    signal.signal(signal.SIGTERM, handle_sig)

    last_key = None
    last_mode = None
    tv_down_logged = False

    print(f"[arc-sync] watching {args.jellyfin} (device~='{args.device}') -> TV {args.tv}", flush=True)
    while not STOP:
        try:
            sessions = jellyfin_sessions(args.jellyfin, args.api_key)
            info = active_audio_info(sessions, args.device)
            if info:
                track_key, channels, title, dev = info
                want = "PCM" if channels <= 2 else "DD"
                if track_key != last_key:
                    last_key = track_key
                    if want != last_mode:
                        print(f"[arc-sync] {dev}: '{title}' ({channels}ch) -> {want}", flush=True)
                        if not args.dry_run:
                            try:
                                out = tv_set_arc(args.tv, args.tv_port, want, key_opt=args.key)
                                last_mode = want
                                tv_down_logged = False
                                code = beam_audio_code(args.beam)
                                extra = f" beam={code}" if code else ""
                                print(f"[arc-sync] TV: {' | '.join(out.splitlines())}{extra}", flush=True)
                            except OSError as e:
                                if not tv_down_logged:
                                    print(f"[arc-sync] TV unreachable ({e}); will retry on next item", flush=True)
                                    tv_down_logged = True
                                last_key = None  # retry once TV wakes up
                        else:
                            last_mode = want
                    else:
                        print(f"[arc-sync] {dev}: '{title}' ({channels}ch) already {want}", flush=True)
        except urllib.error.HTTPError as e:
            print(f"[arc-sync] Jellyfin HTTP {e.code}: check API key", file=sys.stderr, flush=True)
        except Exception as e:
            print(f"[arc-sync] poll error: {e}", file=sys.stderr, flush=True)

        if args.once:
            break
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
