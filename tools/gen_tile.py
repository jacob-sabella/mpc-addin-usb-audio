#!/usr/bin/env python3
"""Draw the catalog tile (docs/tile.svg, docs/tile.png): the MPC on a USB cable to a computer recording it, its
main out travelling to the computer and the computer's audio travelling back. Generated from a seed, so a rerun
gives the same picture.

  python3 tools/gen_tile.py [--seed N]       needs chromium (or CHROMIUM=...) for the PNG
"""
import argparse
import math
import os
import random
import shutil
import subprocess
import tempfile

W, H = 1200, 600                  # the catalog card shows its picture at 2:1
BG, INK, DIM, BODY, EDGE = "#1b1c1f", "#f4f4f4", "#3a3c42", "#2b2d33", "#44474e"
ACCENT, AMBER, TEAL = "#d63a30", "#e0a100", "#3f8f8a"
PAD_COLOURS = [ACCENT, AMBER, TEAL, "#5a6bd0", "#9b4fb0", "#c46a2a"]


def signal(rng, n):
    """n samples of a made-up mix: a few partials under hits that decay."""
    parts = [(rng.uniform(1.5, 7), rng.uniform(0, 6.3), rng.uniform(0.2, 1)) for _ in range(4)]
    hits = sorted(rng.uniform(0, 1) for _ in range(rng.randint(3, 6)))
    out = []
    for i in range(n):
        t = i / (n - 1)
        env = 0.15 + sum(math.exp(-(t - h) * 9) for h in hits if t >= h)
        v = sum(a * math.sin(2 * math.pi * f * t * 6 + p) for f, p, a in parts)
        out.append(max(-1.0, min(1.0, v / 2.2 * min(env, 1.0))))
    return out


def wave(vals, x, y, w, h, colour, opacity=1.0):
    """A filled waveform (mirrored peak outline) in the box."""
    n = len(vals)
    top = ["%.1f,%.1f" % (x + w * i / (n - 1), y + h / 2 - abs(v) * h / 2) for i, v in enumerate(vals)]
    bot = ["%.1f,%.1f" % (x + w * i / (n - 1), y + h / 2 + abs(v) * h / 2) for i, v in reversed(list(enumerate(vals)))]
    return '<polygon points="%s" fill="%s" opacity="%g"/>' % (" ".join(top + bot), colour, opacity)


def mpc(rng, x, y):
    """The MPC from above: a small screen with meters, a 4x4 pad grid, a few knobs."""
    o = ['<rect x="%g" y="%g" width="360" height="330" rx="16" fill="%s" stroke="%s" stroke-width="2"/>' % (x, y, BODY, EDGE)]
    sx, sy, sw, sh = x + 22, y + 22, 316, 92
    o.append('<rect x="%g" y="%g" width="%g" height="%g" rx="4" fill="#0e0f11"/>' % (sx, sy, sw, sh))
    for i in range(4):                     # four channel meters: out 1-2, in 1-2
        lv = rng.uniform(0.45, 0.95)
        mx = sx + 18 + i * 34
        o.append('<rect x="%g" y="%g" width="16" height="64" fill="%s"/>' % (mx, sy + 14, DIM))
        o.append('<rect x="%g" y="%g" width="16" height="%g" fill="%s"/>'
                 % (mx, sy + 14 + 64 * (1 - lv), 64 * lv, AMBER if i < 2 else TEAL))
    o.append(wave(signal(rng, 60), sx + 160, sy + 16, 140, 60, AMBER, 0.9))
    px, py = x + 22, y + 132
    for r in range(4):
        for c in range(4):
            lit = rng.random() < 0.3
            o.append('<rect x="%g" y="%g" width="44" height="40" rx="4" fill="%s" opacity="%s"/>'
                     % (px + c * 50, py + r * 46, rng.choice(PAD_COLOURS) if lit else "#45474d", "1" if lit else ".9"))
    for r in range(2):                     # knobs
        for c in range(2):
            kx, ky = x + 270 + c * 44, y + 160 + r * 52
            a = rng.uniform(-2.4, 2.4)
            o.append('<circle cx="%g" cy="%g" r="16" fill="#1f2025" stroke="%s" stroke-width="2"/>' % (kx, ky, EDGE))
            o.append('<line x1="%g" y1="%g" x2="%g" y2="%g" stroke="%s" stroke-width="3" stroke-linecap="round"/>'
                     % (kx, ky, kx + 11 * math.sin(a), ky - 11 * math.cos(a), INK))
    o.append('<rect x="%g" y="%g" width="56" height="20" rx="4" fill="%s"/>' % (x + 270, y + 272, ACCENT))
    o.append('<rect x="%g" y="%g" width="56" height="20" rx="4" fill="%s"/>' % (x + 270, y + 300 - 2, EDGE))
    return "\n".join(o)


def laptop(rng, x, y):
    """A computer recording: four lanes in, one lane out to the MPC."""
    w, h = 470, 300
    o = ['<rect x="%g" y="%g" width="%g" height="%g" rx="12" fill="%s" stroke="%s" stroke-width="2"/>' % (x, y, w, h, BODY, EDGE),
         '<path d="M %g %g h %g l 34 26 h %g z" fill="%s" stroke="%s" stroke-width="2"/>'
         % (x, y + h, w, -(w + 68), "#24262b", EDGE)]
    o[-1] = '<path d="M %g %g L %g %g L %g %g L %g %g Z" fill="#24262b" stroke="%s" stroke-width="2"/>' % (
        x, y + h, x + w, y + h, x + w + 34, y + h + 24, x - 34, y + h + 24, EDGE)
    sx, sy, sw, sh = x + 16, y + 16, w - 32, h - 32
    o.append('<rect x="%g" y="%g" width="%g" height="%g" rx="3" fill="#0e0f11"/>' % (sx, sy, sw, sh))
    o.append('<rect x="%g" y="%g" width="%g" height="22" fill="#26282d"/>' % (sx, sy, sw))
    o.append('<circle cx="%g" cy="%g" r="6" fill="%s"/>' % (sx + 16, sy + 11, ACCENT))    # record
    o.append('<rect x="%g" y="%g" width="70" height="8" rx="2" fill="%s"/>' % (sx + 34, sy + 7, DIM))
    lanes = [(AMBER, 1), (AMBER, 1), (TEAL, 1), (TEAL, 1), (INK, 0)]
    lh = (sh - 34) / len(lanes)
    for i, (col, rec) in enumerate(lanes):
        ly = sy + 28 + i * lh
        o.append('<rect x="%g" y="%g" width="54" height="%g" rx="2" fill="#26282d"/>' % (sx + 6, ly + 3, lh - 6))
        o.append('<rect x="%g" y="%g" width="8" height="%g" rx="1" fill="%s"/>' % (sx + 10, ly + 8, lh - 16, col))
        o.append('<rect x="%g" y="%g" width="26" height="6" rx="1" fill="%s"/>' % (sx + 24, ly + lh / 2 - 3, DIM))
        o.append(wave(signal(rng, 110), sx + 68, ly + 4, sw - 76, lh - 8, col, 0.85 if rec else 0.6))
    o.append('<rect x="%g" y="%g" width="2" height="%g" fill="%s"/>' % (sx + 68 + (sw - 76) * 0.72, sy + 24, sh - 26, ACCENT))
    return "\n".join(o)


def tile(seed):
    rng = random.Random(seed)
    o = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">' % (W, H, W, H),
         '<rect width="%d" height="%d" fill="%s"/>' % (W, H, BG)]
    for gx in range(12, W, 24):           # faint dot grid
        for gy in range(12, H, 24):
            o.append('<circle cx="%d" cy="%d" r="1" fill="#2a2c31"/>' % (gx, gy))

    mx, my = 60, 135
    lx, ly = 670, 120
    o.append(mpc(rng, mx, my))
    o.append(laptop(rng, lx, ly))

    # the USB cable: from the MPC's side to the laptop's side, sagging a little between them
    x0, y0 = mx + 360 + 12, my + 165
    x1, y1 = lx - 12, ly + 200
    c1, c2 = (x0 + 80, y0 + 110), (x1 - 80, y1 + 110)
    path = "M %g %g C %g %g, %g %g, %g %g" % (x0, y0, c1[0], c1[1], c2[0], c2[1], x1, y1)
    o.append('<path d="%s" fill="none" stroke="#0e0f11" stroke-width="12" stroke-linecap="round"/>' % path)
    o.append('<path d="%s" fill="none" stroke="%s" stroke-width="7" stroke-linecap="round"/>' % (path, "#55585f"))
    for (px, py), ang in (((x0, y0), 0), ((x1, y1), 180)):   # USB-C plugs
        o.append('<g transform="translate(%g %g) rotate(%g)"><rect x="-4" y="-9" width="26" height="18" rx="5" fill="%s" stroke="%s" stroke-width="2"/>'
                 '<rect x="-12" y="-5" width="10" height="10" rx="3" fill="#b9b9b6"/></g>' % (px, py, ang, BODY, EDGE))

    # audio along the cable: main out to the computer (amber, above), computer audio back (teal, below)
    def bez(t):
        u = 1 - t
        return (u ** 3 * x0 + 3 * u * u * t * c1[0] + 3 * u * t * t * c2[0] + t ** 3 * x1,
                u ** 3 * y0 + 3 * u * u * t * c1[1] + 3 * u * t * t * c2[1] + t ** 3 * y1)
    out_sig, back_sig = signal(rng, 90), signal(rng, 90)
    for sig, side, col in ((out_sig, -1, AMBER), (back_sig, 1, TEAL)):
        for i in range(10, 80):
            t = i / 89
            (ax, ay), (bx, by) = bez(t), bez(t + 0.002)
            nx, ny = -(by - ay), bx - ax
            n = math.hypot(nx, ny) or 1
            nx, ny = nx / n, ny / n
            amp = 4 + 30 * abs(sig[i])
            off = 10 * side
            o.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="3" stroke-linecap="round" opacity="%.2f"/>'
                     % (ax + nx * off, ay + ny * off, ax + nx * (off + side * amp), ay + ny * (off + side * amp), col,
                        0.45 + 0.5 * abs(sig[i])))
    # direction arrows at each end of the two streams
    for t, side, col, fwd in ((0.93, -1, AMBER, 1), (0.07, 1, TEAL, -1)):
        (ax, ay), (bx, by) = bez(t), bez(t + 0.01 * fwd)
        ang = math.degrees(math.atan2(by - ay, bx - ax))
        nx, ny = -(by - ay), bx - ax
        n = math.hypot(nx, ny) or 1
        cx, cy = ax + nx / n * 16 * side, ay + ny / n * 16 * side
        o.append('<path d="M -8 -8 L 8 0 L -8 8 z" fill="%s" transform="translate(%.1f %.1f) rotate(%.1f)"/>' % (col, cx, cy, ang))
    o.append("</svg>")
    return "\n".join(o)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=44100)
    a = ap.parse_args()
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs")
    os.makedirs(root, exist_ok=True)
    svg = os.path.join(root, "tile.svg")
    with open(svg, "w") as f:
        f.write(tile(a.seed))
    chrome = os.environ.get("CHROMIUM") or shutil.which("chromium") or shutil.which("chromium-browser")
    if not chrome:
        print("wrote %s (no chromium for the PNG)" % svg)
        return
    with tempfile.TemporaryDirectory() as tmp:
        page = os.path.join(tmp, "t.html")
        with open(page, "w") as f:
            f.write('<html><body style="margin:0">%s</body></html>' % open(svg).read())
        png = os.path.abspath(os.path.join(root, "tile.png"))
        subprocess.run([chrome, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--user-data-dir=" + tmp,
                        "--window-size=%d,%d" % (W, H), "--screenshot=" + png, "file://" + page],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print("wrote %s and %s" % (svg, png))


if __name__ == "__main__":
    main()
