#!/usr/bin/env python3
"""Button layouts for RetroPad console boards, placed inside the Tab5 Keyboard envelope.

Coordinates are in mm, seen from the front (key side):
    x = 0 at the left edge of the case, y = 0 at the bottom edge.
The key area is 128 x 52; the 5.95 mm strip above it (y 52..57.95) carries the
2x5 header and docks into the Tab5, so no buttons go there.

    python3 layout.py snes            # writes snes/layout.svg and prints the placement table
"""
import itertools
import math
import os
import sys

CASE_W, CASE_H, KEY_H, BODY_H = 128.0, 59.4, 52.0, 57.95
WALL = 1.5                       # assumed shell wall until a shell is drawn
M3_HOLES = [(16.0, None), (112.0, None)]   # x known (96 apart); y still to be measured
HEADER_X = 26.0                  # connector centre, from M5's STL (front view)

# Tact-switch pin grid (centre to pin, local frame) and pad radius
PINS = {6: (3.25, 2.25), 12: (6.25, 2.5)}
PAD_R = {6: 0.9, 12: 1.1}


def snes(s=0.83, k=1.0):
    """SNES pad from the 144 x 62 reference drawing.

    s scales the outline and the distance between the two clusters so the pad
    fits the key area; k scales the spacing inside each cluster. k = 1 keeps
    the reference spacing, because the switches themselves do not shrink.
    """
    cx, cy = CASE_W / 2, KEY_H / 2
    dx, fx = cx - 41 * s, cx + 41 * s
    buttons = [
        # name, RetroPad bit, x, y, switch size, rotation
        ("UP",     "RP_BTN_UP",     dx, cy + 12.25 * k, 6, 0),
        ("DOWN",   "RP_BTN_DOWN",   dx, cy - 12.5 * k, 6, 0),
        ("LEFT",   "RP_BTN_LEFT",   dx - 12.5 * k, cy, 6, 0),
        ("RIGHT",  "RP_BTN_RIGHT",  dx + 12.5 * k, cy, 6, 0),
        ("X",      "RP_BTN_X",      fx, cy + 10.5 * k, 12, 45),
        ("B",      "RP_BTN_B",      fx, cy - 10.5 * k, 12, 45),
        ("Y",      "RP_BTN_Y",      fx - 13.5 * k, cy, 12, 45),
        ("A",      "RP_BTN_A",      fx + 13.5 * k, cy, 12, 45),
        ("SELECT", "RP_BTN_SELECT", cx - 7.5 * s, cy - 6 * s, 6, 45),
        ("START",  "RP_BTN_START",  cx + 7.5 * s, cy - 6 * s, 6, 45),
    ]
    outline = ("stadium", cx, cy, 41 * s, 31 * s)   # two circles of radius 31s at +-41s
    return buttons, outline


BOARDS = {"snes": snes}


def _xf(b, pts):
    _, _, x, y, _, rot = b
    r = math.radians(rot)
    return [(x + u * math.cos(r) - v * math.sin(r), y + u * math.sin(r) + v * math.cos(r)) for u, v in pts]


def body(b):
    h = b[4] / 2
    return _xf(b, [(-h, -h), (h, -h), (h, h), (-h, h)])


def pads(b):
    px, py = PINS[b[4]]
    return _xf(b, [(sx * px, sy * py) for sx in (-1, 1) for sy in (-1, 1)])


def _gap(p, q):
    best = -1e9
    for poly in (p, q):
        for i in range(len(poly)):
            (ax, ay), (bx, by) = poly[i], poly[(i + 1) % len(poly)]
            nx, ny = by - ay, ax - bx
            n = math.hypot(nx, ny)
            pa = [(nx * x + ny * y) / n for x, y in p]
            qa = [(nx * x + ny * y) / n for x, y in q]
            best = max(best, min(qa) - max(pa), min(pa) - max(qa))
    return best


def check(buttons):
    pts = [pt for b in buttons for pt in body(b) + pads(b)]
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    body_gap = min(_gap(body(a), body(b)) for a, b in itertools.combinations(buttons, 2))
    pad_gap = min(math.dist(p, q) - PAD_R[a[4]] - PAD_R[b[4]]
                  for a, b in itertools.combinations(buttons, 2) for p in pads(a) for q in pads(b))
    inside = min(xs) >= WALL and max(xs) <= CASE_W - WALL and min(ys) >= WALL and max(ys) <= KEY_H - WALL
    return dict(x=(min(xs), max(xs)), y=(min(ys), max(ys)), body_gap=body_gap, pad_gap=pad_gap, inside=inside)


def svg(buttons, outline, path):
    S = 6.0                                   # px per mm
    W, H = CASE_W * S + 40, CASE_H * S + 40

    def P(x, y):
        return f"{20 + x * S:.1f},{20 + (CASE_H - y) * S:.1f}"

    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W:.0f}" height="{H:.0f}" '
         f'viewBox="0 0 {W:.0f} {H:.0f}" font-family="sans-serif" font-size="10">',
         f'<rect width="100%" height="100%" fill="#fff"/>']
    # case: key area, top strip, latch arms
    o.append(f'<polygon points="{P(0,0)} {P(CASE_W,0)} {P(CASE_W,BODY_H)} {P(0,BODY_H)}" fill="#f4f4f4" stroke="#333"/>')
    o.append(f'<polygon points="{P(0,KEY_H)} {P(CASE_W,KEY_H)} {P(CASE_W,BODY_H)} {P(0,BODY_H)}" fill="#e2e2e2" stroke="#333"/>')
    for x0 in (0, CASE_W - 2.2):
        o.append(f'<polygon points="{P(x0,BODY_H)} {P(x0+2.2,BODY_H)} {P(x0+2.2,CASE_H)} {P(x0,CASE_H)}" fill="#ccc" stroke="#333"/>')
    o.append(f'<polygon points="{P(HEADER_X-6.6,KEY_H+0.5)} {P(HEADER_X+6.6,KEY_H+0.5)} {P(HEADER_X+6.6,BODY_H-0.5)} '
             f'{P(HEADER_X-6.6,BODY_H-0.5)}" fill="none" stroke="#c33" stroke-dasharray="3 2"/>')
    o.append(f'<text x="{20+(HEADER_X+8)*S:.0f}" y="{20+(CASE_H-KEY_H-3)*S:.0f}" fill="#c33">2x5 header (back side)</text>')
    for x, _ in M3_HOLES:
        o.append(f'<line x1="{20+x*S}" y1="{20+(CASE_H-KEY_H)*S}" x2="{20+x*S}" y2="{20+CASE_H*S}" stroke="#36c" stroke-dasharray="2 3"/>')
    # wall margin
    o.append(f'<polygon points="{P(WALL,WALL)} {P(CASE_W-WALL,WALL)} {P(CASE_W-WALL,KEY_H-WALL)} {P(WALL,KEY_H-WALL)}" '
             f'fill="none" stroke="#999" stroke-dasharray="4 3"/>')
    # stadium outline of the original pad
    _, cx, cy, d, r = outline
    o.append(f'<path d="M {P(cx-d, cy+r)} L {P(cx+d, cy+r)} A {r*S:.1f} {r*S:.1f} 0 0 1 {P(cx+d, cy-r)} '
             f'L {P(cx-d, cy-r)} A {r*S:.1f} {r*S:.1f} 0 0 1 {P(cx-d, cy+r)} Z" fill="none" stroke="#2a8" stroke-width="1.5"/>')
    for b in buttons:
        o.append(f'<polygon points="{" ".join(P(*p) for p in body(b))}" fill="#fff8d0" stroke="#a80"/>')
        for p in pads(b):
            o.append(f'<circle cx="{P(*p).split(",")[0]}" cy="{P(*p).split(",")[1]}" r="{PAD_R[b[4]]*S:.1f}" fill="#c9a227"/>')
        tx, ty = P(b[2], b[3]).split(",")
        o.append(f'<text x="{tx}" y="{float(ty)+3:.1f}" text-anchor="middle" font-weight="bold">{b[0]}</text>')
    o.append('</svg>')
    with open(path, "w") as f:
        f.write("\n".join(o))


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else "snes"
    buttons, outline = BOARDS[name]()
    here = os.path.dirname(os.path.abspath(__file__))
    os.makedirs(os.path.join(here, name), exist_ok=True)
    svg(buttons, outline, os.path.join(here, name, "layout.svg"))
    c = check(buttons)
    print("| Button | RetroPad bit | x | y | Switch | Rotation |\n|---|---|---|---|---|---|")
    for n, bit, x, y, sz, rot in buttons:
        print(f"| {n} | `{bit}` | {x:.2f} | {y:.2f} | {sz}x{sz} | {rot}° |")
    print(f"\nextent x {c['x'][0]:.1f}..{c['x'][1]:.1f}, y {c['y'][0]:.1f}..{c['y'][1]:.1f}; "
          f"closest bodies {c['body_gap']:.2f} mm; closest pads {c['pad_gap']:.2f} mm; "
          f"inside wall margin: {c['inside']}")


if __name__ == "__main__":
    main()
