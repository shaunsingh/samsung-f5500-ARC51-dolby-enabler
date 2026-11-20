#!/usr/bin/env python3
"""Add Dolby Digital 5.1 and DTS 5.1 to the audio a 256-byte HDMI EDID advertises.

    patch_edid_51.py <dump.bin | directory of *.bin> <output file | directory>

Only the CEA-861 extension block changes: the Audio Data Block gains an AC-3
and a DTS descriptor, the Speaker Allocation Data Block gains the 5.1
speakers, and the detailed timings behind them move up into the block's
padding. Everything else, the physical address included, stays as dumped.
"""

import sys
from pathlib import Path

HEADER = bytes.fromhex("00ffffffffffff00")
DTD_SIZE = 18

# CEA-861 Short Audio Descriptors: format and channels, sample rates, bit rate / 8 kbps.
AUDIO_LPCM_20 = bytes([0x09, 0x07, 0x07])  # 2 channels, 32/44.1/48 kHz, 16/20/24 bit
AUDIO_FORMATS = {
    2: bytes([0x15, 0x07, 0x50]),  # AC-3, 6 channels, 32/44.1/48 kHz, 640 kbps
    7: bytes([0x3D, 0x07, 0xC0]),  # DTS, 6 channels, 32/44.1/48 kHz, 1536 kbps
}
SPEAKERS_51 = 0x0F  # front left/right, LFE, front centre, rear left/right

TAG_AUDIO, TAG_VENDOR, TAG_SPEAKERS = 1, 3, 4


def data_blocks(cea: bytes) -> list[tuple[int, bytes]]:
    """The (tag, payload) of each data block in a CEA-861 extension block."""
    if cea[0] != 0x02 or cea[1] < 3:
        raise ValueError(f"not a CEA-861 extension of version 3: {cea[:2].hex()}")
    end = cea[2]
    if not 4 <= end <= 127:
        raise ValueError(f"bad detailed timing offset {end}")

    blocks, at = [], 4
    while at < end:
        length = cea[at] & 0x1F
        if at + 1 + length > end:
            raise ValueError(f"data block at {at} runs past the data block collection")
        blocks.append((cea[at] >> 5, cea[at + 1 : at + 1 + length]))
        at += 1 + length
    return blocks


def detailed_timings(cea: bytes) -> bytes:
    """The 18-byte descriptors of a CEA-861 extension block, without its padding."""
    at = start = cea[2]
    while at + DTD_SIZE <= 127 and cea[at : at + 2] != b"\0\0":
        at += DTD_SIZE
    if any(cea[at:127]):
        raise ValueError("unexpected data after the detailed timings")
    return cea[start:at]


def with_surround(blocks: list[tuple[int, bytes]]) -> list[tuple[int, bytes]]:
    blocks = list(blocks)
    for tag, default in ((TAG_AUDIO, AUDIO_LPCM_20), (TAG_SPEAKERS, bytes(3))):
        if not any(t == tag for t, _ in blocks):
            blocks.append((tag, default))

    patched = []
    for tag, payload in blocks:
        if tag == TAG_AUDIO:
            present = {payload[i] >> 3 & 0x0F for i in range(0, len(payload), 3)}
            payload += b"".join(sad for fmt, sad in AUDIO_FORMATS.items() if fmt not in present)
        elif tag == TAG_SPEAKERS and len(payload) == 3:
            payload = bytes([payload[0] | SPEAKERS_51]) + payload[1:]
        if len(payload) > 31:
            raise ValueError("a data block would exceed 31 bytes")
        patched.append((tag, payload))
    return patched


def checked(edid: bytes) -> bytes:
    if len(edid) != 256:
        raise ValueError(f"expected 256 bytes, got {len(edid)}")
    if edid[:8] != HEADER:
        raise ValueError("no EDID header")
    if sum(edid[:128]) % 256 or sum(edid[128:]) % 256:
        raise ValueError("bad checksum")
    return edid


def patch(edid: bytes) -> bytes:
    cea = checked(edid)[128:]
    collection = b"".join(
        bytes([tag << 5 | len(payload)]) + payload for tag, payload in with_surround(data_blocks(cea))
    )
    body = cea[:2] + bytes([4 + len(collection)]) + cea[3:4] + collection + detailed_timings(cea)
    if len(body) > 127:
        raise ValueError(f"the extension block has no room for {len(body) - 127} more bytes")
    body = body.ljust(127, b"\0")
    return checked(edid[:128] + body + bytes([-sum(body) % 256]))


def summary(edid: bytes) -> str:
    blocks = data_blocks(edid[128:])

    def first(tag: int) -> str:
        return next((bytes([tag << 5 | len(p)]) + p for t, p in blocks if t == tag), b"").hex() or "-"

    hdmi = next((p for t, p in blocks if t == TAG_VENDOR and len(p) >= 5 and p[:3] == b"\x03\x0c\x00"), None)
    phys = ".".join(f"{nibble:x}" for byte in hdmi[3:5] for nibble in divmod(byte, 16)) if hdmi else "-"
    return f"phys={phys} ADB={first(TAG_AUDIO)} SADB={first(TAG_SPEAKERS)}"


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    source, target = Path(sys.argv[1]), Path(sys.argv[2])

    if source.is_dir():
        target.mkdir(parents=True, exist_ok=True)
        pairs = [(path, target / path.name) for path in sorted(source.glob("*.bin"))]
    else:
        pairs = [(source, target)]
    if not pairs:
        print(f"no *.bin files in {source}", file=sys.stderr)
        return 1

    failed = False
    for path, output in pairs:
        try:
            edid = path.read_bytes()
            patched = patch(edid)
        except (OSError, ValueError) as error:
            print(f"{path}: {error}", file=sys.stderr)
            failed = True
            continue
        output.write_bytes(patched)
        print(f"{path.name}\n  in : {summary(edid)}\n  out: {summary(patched)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
