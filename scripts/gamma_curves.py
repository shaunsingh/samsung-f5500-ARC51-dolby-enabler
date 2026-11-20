#!/usr/bin/env python3
"""Regenerate (or check) the computed EOTF rows of src/gamma_tables.h.

Each row maps a full-range input code i (0..255, after the TV's HDMI range
expansion) to a 12-bit drive value d. At OSD Gamma 0 the firmware's own LUT
is linear (exponent about 1.0 on R, G and B), and the panel's response to d
is taken to be the nominal 2.2 power above its black floor:

    L(d) = Lb + (Lw - Lb) * (d / 4095) ** 2.2

so the drive that produces a target luminance L is

    d = 4095 * ((L - Lb) / (Lw - Lb)) ** (1 / 2.2)

with the target taken from:

    2.3, 2.4   L = Lb + (Lw - Lb) * V ** gamma       (power law above black)
    BT1886     ITU-R BT.1886 Annex 1 EOTF for Lw and Lb:
               a = (Lw^(1/2.4) - Lb^(1/2.4)) ** 2.4
               b = Lb^(1/2.4) / (Lw^(1/2.4) - Lb^(1/2.4))
               L = a * max(V + b, 0) ** 2.4

where V = i / 255. Only the ratio Lw / Lb matters. CONTRAST is this panel's
full-on/full-off ratio: CNET measured 0.004 fL black on the F5500 series,
about 1/10000 of a 40 fL calibrated white. With a meter, measure both and
pass --contrast. The TOE row is hand-shaped and left alone.

usage: gamma_curves.py [--check | --write] [--contrast N]
"""
import argparse
import re
import sys
from pathlib import Path

HEADER = Path(__file__).resolve().parent.parent / "src" / "gamma_tables.h"
CONTRAST = 10000
PANEL_GAMMA = 2.2


def drive(rel):
    return round(4095 * max(0.0, min(1.0, rel)) ** (1 / PANEL_GAMMA))


def power_row(gamma):
    return [drive((i / 255) ** gamma) for i in range(256)]


def bt1886_row(contrast):
    lw, lb = 1.0, 1.0 / contrast
    root_w, root_b = lw ** (1 / 2.4), lb ** (1 / 2.4)
    a = (root_w - root_b) ** 2.4
    b = root_b / (root_w - root_b)
    return [drive((a * max(i / 255 + b, 0.0) ** 2.4 - lb) / (lw - lb)) for i in range(256)]


def format_row(values):
    lines = []
    for k in range(0, 256, 16):
        lines.append("        " + " ".join(f"{v:4d}," for v in values[k:k + 16]))
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--write", action="store_true")
    parser.add_argument("--contrast", type=float, default=CONTRAST)
    args = parser.parse_args()

    rows = {
        "GAMMA_23": power_row(2.3),
        "GAMMA_24": power_row(2.4),
        "GAMMA_BT1886": bt1886_row(args.contrast),
    }
    for name, values in rows.items():
        if values[0] != 0 or values[255] != 4095 or any(x > y for x, y in zip(values, values[1:])):
            sys.exit(f"{name}: not monotonic from 0 to 4095")

    text = HEADER.read_text()
    out = text
    for name, values in rows.items():
        pattern = re.compile(r"(\[%s\] = \{\n)(.*?)(\n    \},)" % name, re.S)
        if not pattern.search(out):
            sys.exit(f"{HEADER}: row {name} not found")
        out = pattern.sub(lambda m: m.group(1) + format_row(values) + m.group(3), out, count=1)

    if args.check:
        if out != text:
            sys.exit(f"{HEADER} is out of date; run scripts/gamma_curves.py --write")
        print(f"gamma tables match (BT1886 at {args.contrast:g}:1, panel {PANEL_GAMMA})")
    else:
        HEADER.write_text(out)
        print(f"wrote {HEADER} (BT1886 at {args.contrast:g}:1)")


if __name__ == "__main__":
    main()
