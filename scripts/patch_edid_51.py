#!/usr/bin/env python3
"""
Surgically patch 256-byte Samsung F-MST HDMI EDID dumps (DDCRAM_port0..3.bin)
to advertise Dolby Digital 5.1 (AC-3) and DTS 5.1 plus 5.1 Speaker Allocation
while preserving all native video timings, monitor descriptors, and per-port
CEC/HDMI physical addresses.

Usage:
    python3 scripts/patch_edid_51.py <input_dir_or_file> <output_dir_or_file>
"""

import os
import sys

EDID_MAGIC = bytes([0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00])

# Short Audio Descriptors (CEA-861):
#   AC-3 (Dolby Digital): format=2, 6ch (5.1), 32/44.1/48 kHz, max 640 kbps
SAD_AC3_51 = bytes([0x15, 0x07, 0x50])
#   DTS: format=7, 6ch (5.1), 32/44.1/48 kHz, max 1536 kbps
SAD_DTS_51 = bytes([0x3D, 0x07, 0xC0])


def checksum_ok(block: bytes) -> bool:
    return (sum(block) & 0xFF) == 0


def parse_cea_blocks(ext: bytes):
    if ext[0] != 0x02 or ext[1] < 0x03:
        raise ValueError(f"Unsupported extension block header: {ext[0]:02x} {ext[1]:02x}")
    dtd_off = ext[2]
    if dtd_off < 4 or dtd_off > 127:
        raise ValueError(f"Invalid DTD offset: {dtd_off}")
    blocks = []
    pos = 4
    while pos < dtd_off:
        tag = (ext[pos] >> 5) & 0x07
        ln = ext[pos] & 0x1F
        if pos + 1 + ln > dtd_off:
            raise ValueError(f"Malformed CEA data block at offset {pos}")
        blocks.append((tag, bytearray(ext[pos + 1 : pos + 1 + ln])))
        pos += 1 + ln
    dtd_tail = ext[dtd_off:127]
    return blocks, dtd_off, dtd_tail


def patch_edid(raw: bytes, include_dts: bool = True) -> bytes:
    if len(raw) != 256:
        raise ValueError(f"Expected 256 bytes, got {len(raw)}")
    if raw[:8] != EDID_MAGIC:
        raise ValueError("Invalid EDID header magic")
    if not checksum_ok(raw[:128]) or not checksum_ok(raw[128:256]):
        raise ValueError("Input EDID failed checksum validation")

    base = bytes(raw[:128])
    ext = bytes(raw[128:256])
    blocks, orig_dtd_off, dtd_tail = parse_cea_blocks(ext)

    new_blocks = []
    found_audio = False
    found_sadb = False

    for tag, payload in blocks:
        if tag == 1:  # Audio Data Block
            found_audio = True
            existing_sads = [bytes(payload[i : i + 3]) for i in range(0, len(payload), 3)]
            existing_formats = {(s[0] >> 3) & 0x0F for s in existing_sads if len(s) == 3}
            if 2 not in existing_formats:
                payload.extend(SAD_AC3_51)
            if include_dts and 7 not in existing_formats:
                payload.extend(SAD_DTS_51)
            if len(payload) > 31:
                raise ValueError("Audio Data Block exceeds 31 bytes")
            new_blocks.append((tag, payload))
        elif tag == 4 and len(payload) == 3:  # Speaker Allocation Data Block
            found_sadb = True
            # 0x0F = FL/FR (bit0) | LFE (bit1) | FC (bit2) | RL/RR (bit3) = 5.1
            payload[0] |= 0x0F
            new_blocks.append((tag, payload))
        else:
            new_blocks.append((tag, payload))

    if not found_audio:
        audio_payload = bytearray(b"\x09\x07\x07" + SAD_AC3_51 + (SAD_DTS_51 if include_dts else b""))
        new_blocks.append((1, audio_payload))
    if not found_sadb:
        new_blocks.append((4, bytearray(b"\x0f\x00\x00")))

    # Reassemble CEA data block collection
    dbc = bytearray()
    for tag, payload in new_blocks:
        dbc.append(((tag & 0x07) << 5) | (len(payload) & 0x1F))
        dbc.extend(payload)

    new_dtd_off = 4 + len(dbc)
    growth = new_dtd_off - orig_dtd_off
    if growth < 0:
        raise ValueError("Unexpected negative block growth")

    # Verify the trailing DTD padding has enough zero bytes to absorb the growth
    # without truncating any active 18-byte Detailed Timing Descriptor.
    if growth > 0 and any(b != 0 for b in dtd_tail[-growth:]):
        raise ValueError(
            f"Not enough trailing zero padding in CEA block ({growth} bytes needed) "
            "to shift DTDs without losing a timing descriptor"
        )

    new_dtd_area = dtd_tail[: 127 - new_dtd_off]
    new_ext = bytearray(128)
    new_ext[0] = ext[0]
    new_ext[1] = ext[1]
    new_ext[2] = new_dtd_off
    new_ext[3] = ext[3]
    new_ext[4:new_dtd_off] = dbc
    new_ext[new_dtd_off:127] = new_dtd_area
    new_ext[127] = (-sum(new_ext[:127])) & 0xFF

    out = base + bytes(new_ext)
    assert len(out) == 256
    assert checksum_ok(out[:128]) and checksum_ok(out[128:256])
    return out


def summarize(edid: bytes) -> str:
    blocks, dtd_off, _ = parse_cea_blocks(edid[128:256])
    adb = next((bytes([(1 << 5) | len(p)]) + bytes(p) for t, p in blocks if t == 1), b"")
    sadb = next((bytes([(4 << 5) | len(p)]) + bytes(p) for t, p in blocks if t == 4), b"")
    vsdb = next((p for t, p in blocks if t == 3 and len(p) >= 5 and p[:3] == b"\x03\x0c\x00"), None)
    phys = (
        f"{vsdb[3]>>4}.{vsdb[3]&0xf}.{vsdb[4]>>4}.{vsdb[4]&0xf}"
        if vsdb is not None
        else "?.?.?.?"
    )
    return f"phys={phys} dtd_off={dtd_off} ADB={adb.hex()} SADB={sadb.hex()}"


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input_file_or_dir> <output_file_or_dir>", file=sys.stderr)
        sys.exit(2)

    src, dst = sys.argv[1], sys.argv[2]
    if os.path.isdir(src):
        os.makedirs(dst, exist_ok=True)
        files = sorted(f for f in os.listdir(src) if f.endswith(".bin"))
        if not files:
            print(f"No .bin files found in {src}", file=sys.stderr)
            sys.exit(1)
        for fn in files:
            raw = open(os.path.join(src, fn), "rb").read()
            patched = patch_edid(raw)
            out_path = os.path.join(dst, fn)
            open(out_path, "wb").write(patched)
            print(f"{fn}:")
            print(f"  in : {summarize(raw)}")
            print(f"  out: {summarize(patched)}")
    else:
        raw = open(src, "rb").read()
        patched = patch_edid(raw)
        open(dst, "wb").write(patched)
        print(f"in : {summarize(raw)}")
        print(f"out: {summarize(patched)}")


if __name__ == "__main__":
    main()
