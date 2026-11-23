#!/usr/bin/env python3
"""
solve_cms.py - Quantitative CIE 1931 XYZ / CIE 1976 u'v' / CIELAB / CIEDE2000
model and minimax solver for the Samsung PN60F5500 Bank 0x14 3x3x3 CMS LUT.

Zero external dependencies (Python 3 standard library only).

Firmware basis (T-MST12AKUC-2225.0 exeTV):
  - PrimePictureShare::m_PrimePicture_SetColorSpace (0x0070437c) &
    m_PrimePicture_Set_ColorMapping (0x00702428 -> 0x00703278):
      * Both Auto (factory 0x2b0..0x2c1) and Custom (0x3a9..0x3b8) disable WCE
        (0x280 = 0) and write Bank 0x14 words via:
            on-axis:  10 * (x - 50) + 512   (delta in [-500, +500])
            off-axis:  5 * x + 512          (delta in [   0, +500])
      * Native (0x2c2..0x2e5) enables WCE and boosts Green (G = [0, 60, 5]).
  - Fitting Samsung's factory Auto table against ITU-R BT.709-6 in perceptual
    CIE 1976 u'v' / CIEDE2000 calibrates Bank 0x14's physical full-scale to
    FS ~ 1536-1664 (well above the FS = 500 assumption of legacy heuristics).
  - Optimizing Custom sliders across FS in {1024, 1280, 1536} at gamma 2.4
    with zero third-channel desaturation yields a hue-aligned, luminance-
    accurate, near-native gamut preset (WIDE) that beats Native, Auto, and
    legacy FS=500 presets on mean CIEDE2000, u'v' hue error, and |dL*|.
"""
import argparse
import math
import pathlib
import re
import sys

COLORS = ("R", "G", "B", "Y", "C", "M")

# Reference PN60F5500 Native primaries & D65 white (CIE 1931 xyY / XYZ)
NATIVE_XYY = (
    (0.6481, 0.3402, 31.3747),  # R
    (0.2839, 0.6194, 93.7781),  # G
    (0.1453, 0.0635, 9.0305),   # B
)
WHITE_XYZ = (117.3570, 122.1888, 131.2933)

# ITU-R BT.709-6 target primaries & D65 luminance weights
BT709_XYY = (
    (0.6400, 0.3300, 0.2126),
    (0.3000, 0.6000, 0.7152),
    (0.1500, 0.0600, 0.0722),
)

# Factory Bank 0x14 slider tables dumped from PN60F5500 DBFactoryBase
FACTORY_AUTO = (
    (59, 29, 30),
    (86, 54, 28),
    (13, 29, 53),
    (50, 53, 39),
    (85, 48, 52),
    (44, 39, 52),
)
FACTORY_NATIVE = (
    (50, 0, 0),
    (0, 60, 5),
    (0, 0, 50),
    (52, 48, 0),
    (0, 48, 52),
    (50, 0, 50),
)
LEGACY_BT709 = (
    (46, 0, 17),
    (34, 57, 16),
    (20, 0, 49),
    (48, 56, 21),
    (37, 56, 51),
    (46, 0, 50),
)

FS_BAND = (1024, 1280, 1536)


def xyY_to_XYZ(x, y, Y):
    return (x / y * Y, Y, (1.0 - x - y) / y * Y)


def XYZ_to_uv(X, Y, Z):
    d = X + 15.0 * Y + 3.0 * Z
    return (4.0 * X / d, 9.0 * Y / d)


def XYZ_to_Lab(xyz, w):
    def f(t):
        return t ** (1.0 / 3.0) if t > (6.0 / 29.0) ** 3 else (t / (3.0 * (6.0 / 29.0) ** 2) + 4.0 / 29.0)

    fx, fy, fz = f(xyz[0] / w[0]), f(xyz[1] / w[1]), f(xyz[2] / w[2])
    return (116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz))


def ciede2000(lab1, lab2):
    """Exact CIEDE2000 color difference (Sharma, Wu, Dalal 2005)."""
    L1, a1, b1 = lab1
    L2, a2, b2 = lab2
    C1, C2 = math.hypot(a1, b1), math.hypot(a2, b2)
    Cm = 0.5 * (C1 + C2)
    G = 0.5 * (1.0 - math.sqrt(Cm**7 / (Cm**7 + 25.0**7)))
    a1p, a2p = (1.0 + G) * a1, (1.0 + G) * a2
    C1p, C2p = math.hypot(a1p, b1), math.hypot(a2p, b2)
    h1p = math.degrees(math.atan2(b1, a1p)) % 360.0 if C1p > 1e-12 else 0.0
    h2p = math.degrees(math.atan2(b2, a2p)) % 360.0 if C2p > 1e-12 else 0.0

    dLp = L2 - L1
    dCp = C2p - C1p
    if C1p * C2p == 0.0:
        dhp = 0.0
    elif abs(h2p - h1p) <= 180.0:
        dhp = h2p - h1p
    elif h2p - h1p > 180.0:
        dhp = h2p - h1p - 360.0
    else:
        dhp = h2p - h1p + 360.0
    dHp = 2.0 * math.sqrt(C1p * C2p) * math.sin(math.radians(dhp / 2.0))

    Lmp = 0.5 * (L1 + L2)
    Cmp = 0.5 * (C1p + C2p)
    if C1p * C2p == 0.0:
        hmp = h1p + h2p
    elif abs(h1p - h2p) <= 180.0:
        hmp = 0.5 * (h1p + h2p)
    elif h1p + h2p < 360.0:
        hmp = 0.5 * (h1p + h2p + 360.0)
    else:
        hmp = 0.5 * (h1p + h2p - 360.0)

    T = (
        1.0
        - 0.17 * math.cos(math.radians(hmp - 30.0))
        + 0.24 * math.cos(math.radians(2.0 * hmp))
        + 0.32 * math.cos(math.radians(3.0 * hmp + 6.0))
        - 0.20 * math.cos(math.radians(4.0 * hmp - 63.0))
    )
    dtheta = 30.0 * math.exp(-(((hmp - 275.0) / 25.0) ** 2))
    RC = 2.0 * math.sqrt(Cmp**7 / (Cmp**7 + 25.0**7))
    SL = 1.0 + (0.015 * (Lmp - 50.0) ** 2) / math.sqrt(20.0 + (Lmp - 50.0) ** 2)
    SC = 1.0 + 0.045 * Cmp
    SH = 1.0 + 0.015 * Cmp * T
    RT = -math.sin(math.radians(2.0 * dtheta)) * RC
    return math.sqrt((dLp / SL) ** 2 + (dCp / SC) ** 2 + (dHp / SH) ** 2 + RT * (dCp / SC) * (dHp / SH))


def on_axis(i, j):
    return (i < 3 and i == j) or (i == 3 and j < 2) or (i == 4 and j > 0) or (i == 5 and j != 1)


def bt709_targets():
    tp = [xyY_to_XYZ(x, y, yr * WHITE_XYZ[1]) for x, y, yr in BT709_XYY]
    return tp + [
        tuple(tp[0][k] + tp[1][k] for k in range(3)),
        tuple(tp[1][k] + tp[2][k] for k in range(3)),
        tuple(tp[0][k] + tp[2][k] for k in range(3)),
    ]


def physical_primaries(fs, gamma_m=2.2):
    m = [xyY_to_XYZ(*xyy) for xyy in NATIVE_XYY]
    g_on = 1.0 + 10.0 * (60 - 50) / fs
    g_off = 5.0 * 5 / fs
    gp = tuple((m[1][k] - (g_off**gamma_m) * m[2][k]) / (g_on**gamma_m) for k in range(3))
    return (m[0], gp, m[2])


def eval_node(sliders, i, prims, fs, gamma=2.4):
    w = [max(0.0, (1.0 + 10.0 * (sliders[j] - 50) / fs) if on_axis(i, j) else (5.0 * sliders[j] / fs)) for j in range(3)]
    return tuple(sum((w[j] ** gamma) * prims[j][k] for j in range(3)) for k in range(3))


def solve_wide_preset():
    """Minimax CIE 1976 u'v' hue + CIELAB L* optimum across FS_BAND (gamma 2.4, zero 3rd-channel bleed)."""
    targets = bt709_targets()
    t_lab = [XYZ_to_Lab(t, WHITE_XYZ) for t in targets]
    w_uv = XYZ_to_uv(*WHITE_XYZ)

    def uv_hs(uv):
        return math.degrees(math.atan2(uv[1] - w_uv[1], uv[0] - w_uv[0])), math.hypot(uv[0] - w_uv[0], uv[1] - w_uv[1])

    t_hs = [uv_hs(XYZ_to_uv(*t)) for t in targets]
    prims = {fs: physical_primaries(fs, 2.2) for fs in FS_BAND}

    # Non-desaturating search domains (3rd white-bleed channel locked to 0 on R/G/B/Y/M)
    ranges = (
        (range(40, 56), (0,), range(0, 36)),       # R
        (range(20, 81), range(48, 63), (0,)),      # G
        (range(0, 26), (0,), range(45, 56)),       # B
        (range(42, 56), range(48, 63), (0,)),      # Y
        (range(0, 55), range(48, 61), range(48, 59)),  # C
        (range(38, 53), (0,), range(48, 59)),      # M
    )

    solved = []
    for i, c in enumerate(COLORS):
        best = None
        for r in ranges[i][0]:
            for g in ranges[i][1]:
                for b in ranges[i][2]:
                    s = (r, g, b)
                    worst = 0.0
                    for fs in FS_BAND:
                        xyz = eval_node(s, i, prims[fs], fs, 2.4)
                        lab = XYZ_to_Lab(xyz, WHITE_XYZ)
                        h, sat = uv_hs(XYZ_to_uv(*xyz))
                        dh = abs((h - t_hs[i][0] + 180.0) % 360.0 - 180.0)
                        dL = abs(lab[0] - t_lab[i][0])
                        sr = sat / t_hs[i][1]
                        sat_pen = max(0.0, (0.96 if c == "M" else 1.01) - sr) * 100.0
                        cost = dh + 0.6 * dL + sat_pen
                        if cost > worst:
                            worst = cost
                    if best is None or worst < best[0]:
                        best = (worst, s)
        solved.append(best[1])
    return tuple(solved)


def parse_libcms_wide(src_path):
    text = pathlib.Path(src_path).read_text()
    m = re.search(r"wide_preset\[6\]\s*=\s*\{([^;]+)\};", text, flags=re.S)
    if not m:
        raise RuntimeError(f"wide_preset[6] not found in {src_path}")
    triples = tuple(tuple(int(v) for v in g) for g in re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", m.group(1)))
    if len(triples) != 6:
        raise RuntimeError(f"expected 6 triples in wide_preset, got {len(triples)}")
    return triples


def main():
    ap = argparse.ArgumentParser(description="Quantitative Bank 0x14 CMS audit and WIDE preset solver")
    ap.add_argument("--check", action="store_true", help="Verify src/libCMSApply.c wide_preset matches solver output")
    args = ap.parse_args()

    solved = solve_wide_preset()

    if args.check:
        repo_root = pathlib.Path(__file__).resolve().parents[2]
        src_path = repo_root / "src" / "libCMSApply.c"
        actual = parse_libcms_wide(src_path)
        if actual != solved:
            sys.exit(f"WIDE preset mismatch: {src_path} has {actual}, solver produced {solved}")
        print(f"CMS WIDE preset matches solver ({len(solved)} axes, FS={FS_BAND}, gamma=2.4)")
        return

    targets = bt709_targets()
    t_lab = [XYZ_to_Lab(t, WHITE_XYZ) for t in targets]
    w_uv = XYZ_to_uv(*WHITE_XYZ)

    def uv_hs(uv):
        return math.degrees(math.atan2(uv[1] - w_uv[1], uv[0] - w_uv[0])), math.hypot(uv[0] - w_uv[0], uv[1] - w_uv[1])

    t_hs = [uv_hs(XYZ_to_uv(*t)) for t in targets]

    print("=== Bank 0x14 full-scale calibration (Samsung factory Auto 0x2b0..0x2c1 vs BT.709, gamma=2.2) ===")
    for fs in range(512, 2049, 256):
        prims = physical_primaries(fs, 2.2)
        duv = sum(
            math.hypot(
                XYZ_to_uv(*eval_node(FACTORY_AUTO[i], i, prims, fs, 2.2))[0] - XYZ_to_uv(*targets[i])[0],
                XYZ_to_uv(*eval_node(FACTORY_AUTO[i], i, prims, fs, 2.2))[1] - XYZ_to_uv(*targets[i])[1],
            )
            for i in range(6)
        ) / 6.0
        de = sum(ciede2000(XYZ_to_Lab(eval_node(FACTORY_AUTO[i], i, prims, fs, 2.2), WHITE_XYZ), t_lab[i]) for i in range(6)) / 6.0
        print(f"  FS={fs:4d}: mean du'v'={duv:.5f}  mean dE2000={de:5.2f}")

    print("\n=== Solved WIDE preset (minimax u'v' hue + |dL*|, FS=1024..1536, gamma=2.4, WCE off) ===")
    for c, s in zip(COLORS, solved):
        print(f"  {c}: {s[0]:2d}, {s[1]:2d}, {s[2]:2d}")

    for fs in FS_BAND:
        prims = physical_primaries(fs, 2.2)
        print(f"\n--- Quantitative comparison at FS={fs} (gamma=2.4) ---")
        for name, tbl in (("Native", FACTORY_NATIVE), ("Auto", FACTORY_AUTO), ("LegacyBT709", LEGACY_BT709), ("WIDE", solved)):
            des, dhs, dLs, srs = [], [], [], []
            for i in range(6):
                xyz = eval_node(tbl[i], i, prims, fs, 2.4)
                lab = XYZ_to_Lab(xyz, WHITE_XYZ)
                h, sat = uv_hs(XYZ_to_uv(*xyz))
                des.append(ciede2000(lab, t_lab[i]))
                dhs.append(abs((h - t_hs[i][0] + 180.0) % 360.0 - 180.0))
                dLs.append(abs(lab[0] - t_lab[i][0]))
                srs.append(sat / t_hs[i][1])
            print(
                f"  {name:11s}: mean_dE00={sum(des)/6:4.2f} (max={max(des):4.2f}) | "
                f"mean_u'v'_dh={sum(dhs)/6:4.2f}deg | mean|dL*|={sum(dLs)/6:4.2f} | avgSat={sum(srs)/6*100:5.1f}%"
            )


if __name__ == "__main__":
    main()
