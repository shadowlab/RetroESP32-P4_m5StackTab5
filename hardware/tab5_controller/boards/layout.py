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
LATCH_BOTTOM = BODY_H - 11.0     # latch arms reach about 11 mm down the sides (STL)
SHOULDER_Y = 38.0                # side-edge shoulder buttons, below the latch arms

# Switch types: body (w, h) and pin positions in the local frame, pad radius.
#   6, 12 : THT tact switches 6x6 / 12x12, pins on a 6.5x4.5 / 12.5x5.0 grid
#   court : footprint courtyard (keep-out incl. legs), from the KiCad library
#   "ra"  : right-angle 6x6 tact switch at a side edge, actuator pointing out
#           along -x (rotate 180 for the right edge). Generic footprint; take
#           the final one from the chosen part's datasheet.
SWITCHES = {
    6:    dict(body=(6.0, 6.0), court=(9.5, 7.5), pins=[(sx * 3.25, sy * 2.25) for sx in (-1, 1) for sy in (-1, 1)], pad_r=0.9),
    12:   dict(body=(12.0, 12.0), court=(16.02, 12.5), pins=[(sx * 6.25, sy * 2.5) for sx in (-1, 1) for sy in (-1, 1)], pad_r=1.1),
    "ra": dict(body=(7.0, 6.0), court=(7.0, 7.5), pins=[(2.5, sy * 3.25) for sy in (-1, 1)] + [(0.0, sy * 3.5) for sy in (-1, 1)], pad_r=0.9),
}


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
        # Not on the reference pad: centred above SELECT/START
        ("MENU",   "RP_BTN_MENU",   cx, cy + 5.0, 6, 45),
        # Shoulders: the top edge docks into the Tab5, so L/R come out of the
        # side edges instead, below the latch arms
        ("L",      "RP_BTN_L",      WALL + 3.5, SHOULDER_Y, "ra", 0),
        ("R",      "RP_BTN_R",      CASE_W - WALL - 3.5, SHOULDER_Y, "ra", 180),
    ]
    outline = ("stadium", cx, cy, 41 * s, 31 * s)   # two circles of radius 31s at +-41s
    return buttons, outline


# Shared by the boards below: the SNES d-pad, START/SELECT and MENU positions
_S = 0.83


def _dpad(cx=CASE_W / 2, cy=KEY_H / 2):
    dx = cx - 41 * _S
    return [
        ("UP",    "RP_BTN_UP",    dx, cy + 12.25, 6, 0),
        ("DOWN",  "RP_BTN_DOWN",  dx, cy - 12.5, 6, 0),
        ("LEFT",  "RP_BTN_LEFT",  dx - 12.5, cy, 6, 0),
        ("RIGHT", "RP_BTN_RIGHT", dx + 12.5, cy, 6, 0),
    ]


def _menu(cx=CASE_W / 2, cy=KEY_H / 2):
    return [("MENU", "RP_BTN_MENU", cx, cy + 5.0, 6, 45)]


def _rrect(r):
    return ("rrect", CASE_W / 2, KEY_H / 2, 144 * _S / 2, 62 * _S / 2, r)


def nes():
    """NES pad: d-pad, SELECT/START pills, B and A side by side."""
    cx, cy = CASE_W / 2, KEY_H / 2
    fx = cx + 41 * _S
    # 12 mm switches turned 90 deg so their pins run vertically and the pads of
    # the two neighbouring switches stay clear of each other
    return _dpad() + [
        ("B",      "RP_BTN_B",      fx - 8.0, cy - 2.0, 12, 90),
        ("A",      "RP_BTN_A",      fx + 8.0, cy - 2.0, 12, 90),
        ("SELECT", "RP_BTN_SELECT", cx - 7.5 * _S, cy - 6 * _S, 6, 0),
        ("START",  "RP_BTN_START",  cx + 7.5 * _S, cy - 6 * _S, 6, 0),
    ] + _menu(), _rrect(4.0)


def gb():
    """Game Boy: B low-left / A high-right on a diagonal, SELECT/START angled."""
    cx, cy = CASE_W / 2, KEY_H / 2
    fx = cx + 41 * _S
    return _dpad() + [
        ("B",      "RP_BTN_B",      fx - 7.5, cy - 4.0, 12, 90),
        ("A",      "RP_BTN_A",      fx + 7.5, cy + 4.0, 12, 90),
        ("SELECT", "RP_BTN_SELECT", cx - 7.5 * _S, cy - 6 * _S, 6, 0),
        ("START",  "RP_BTN_START",  cx + 7.5 * _S, cy - 6 * _S, 6, 0),
    ] + _menu(), _rrect(10.0)


def genesis():
    """Genesis 6-button pad: X Y Z above A B C, MODE beside START.

    Two rows of three, columns 14 mm apart and each column 1.5 mm higher than
    the one to its left, like the 6-button pad. Rows are 16.5 mm apart: the 12 mm
    switches are turned 90 deg, which makes their courtyard (legs included) 16.02
    mm tall. The cluster sits low enough for Z's courtyard to clear the right M3
    hole's. MODE uses the SELECT bit (catalog)."""
    cx, cy = CASE_W / 2, KEY_H / 2
    fx = cx + 41 * _S
    face = []
    for col, (low, high) in enumerate((("A", "X"), ("B", "Y"), ("C", "Z"))):
        x, rise = fx + (col - 1) * 14.0, col * 1.5
        face.append((low, "RP_BTN_" + low, x, cy - 12.5 + rise, 12, 90))
        face.append((high, "RP_BTN_" + high, x, cy + 4.0 + rise, 12, 90))
    return _dpad() + face + [
        ("MODE",  "RP_BTN_SELECT", cx - 7.5 * _S, cy - 6 * _S, 6, 45),
        ("START", "RP_BTN_START",  cx + 7.5 * _S, cy - 6 * _S, 6, 45),
    ] + _menu(), ("stadium", cx, cy, 41 * _S, 31 * _S)


def sms():
    """Master System / Game Gear: buttons 1 and 2, START (GG start, SMS pause)."""
    cx, cy = CASE_W / 2, KEY_H / 2
    fx = cx + 41 * _S
    return _dpad() + [
        ("1",     "RP_BTN_B",     fx - 8.0, cy - 2.0, 12, 90),
        ("2",     "RP_BTN_A",     fx + 8.0, cy - 2.0, 12, 90),
        ("START", "RP_BTN_START", cx, cy - 6 * _S - 2.5, 6, 45),   # lower: clears MENU
    ] + _menu(), _rrect(4.0)


BOARDS = {"snes": snes, "nes": nes, "gb": gb, "genesis": genesis, "sms": sms,
          "snes_dd": snes}

# Board core: "stm32" = STM32F030C8 + 8x4 key matrix (M5Stack keyboard firmware
# patch), "avrdd" = AVR32DD28 in SOIC-28 with one pin per button.
CORES = {"snes_dd": "avrdd"}


def core_of(name):
    return CORES.get(name, "stm32")


# AVR DD boards: 13 button slots, one MCU pin each (switch to GND, internal
# pull-up). A console's buttons fill the slots in the order its layout lists
# them, so the slot table below is all the firmware needs to know per console
# (kicad_gen/gen_avrdd_pinmap.py writes it into firmware_avrdd/pinmap.h).
AVRDD_SLOTS = ["PD1", "PD2", "PD3", "PD4", "PD5", "PD6", "PD7",
               "PC0", "PC1", "PC2", "PC3", "PF0", "PF1"]


def avrdd_slots(name):
    """[(button name, RP bit name, slot pin)] for a console's layout."""
    buttons = BOARDS[name]()[0]
    if len(buttons) > len(AVRDD_SLOTS):
        raise ValueError("%s has %d buttons, AVR DD boards have %d slots" % (name, len(buttons), len(AVRDD_SLOTS)))
    return [(b[0], b[1], AVRDD_SLOTS[i]) for i, b in enumerate(buttons)]

# Console id (retropad_proto.h rp_console_t) and the name printed on each board
CONSOLES = {
    "snes_dd": (3, "SNES (AVR DD)"),
    "nes":     (1, "NES"),
    "gb":      (2, "Game Boy"),
    "snes":    (3, "SNES"),
    "sms":     (4, "Master System / Game Gear"),
    "genesis": (5, "Genesis / Mega Drive 6-button"),
}


def _xf(b, pts):
    _, _, x, y, _, rot = b
    r = math.radians(rot)
    return [(x + u * math.cos(r) - v * math.sin(r), y + u * math.sin(r) + v * math.cos(r)) for u, v in pts]


def body(b):
    w, h = SWITCHES[b[4]]["body"]
    return _xf(b, [(-w / 2, -h / 2), (w / 2, -h / 2), (w / 2, h / 2), (-w / 2, h / 2)])


def court(b):
    w, h = SWITCHES[b[4]]["court"]
    return _xf(b, [(-w / 2, -h / 2), (w / 2, -h / 2), (w / 2, h / 2), (-w / 2, h / 2)])


M3_Y_EST = 45.0                  # estimated M3 hole height (see README §1)
M3_COURT_R = 3.45                # MountingHole_3.2mm_M3 courtyard radius


def pads(b):
    return _xf(b, SWITCHES[b[4]]["pins"])


def pad_r(b):
    return SWITCHES[b[4]]["pad_r"]


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


def _point_gap(pt, poly):
    """Distance from a point to a convex polygon (0 if inside)."""
    px, py = pt
    best, inside = 1e9, True
    for i in range(len(poly)):
        (ax, ay), (bx, by) = poly[i], poly[(i + 1) % len(poly)]
        dx, dy = bx - ax, by - ay
        t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
        best = min(best, math.hypot(px - ax - t * dx, py - ay - t * dy))
        if dx * (py - ay) - dy * (px - ax) < 0:
            inside = False
    return 0.0 if inside else best


def check(buttons):
    pts = [pt for b in buttons for pt in body(b) + pads(b)]
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    body_gap = min(_gap(body(a), body(b)) for a, b in itertools.combinations(buttons, 2))
    pad_gap = min(math.dist(p, q) - pad_r(a) - pad_r(b)
                  for a, b in itertools.combinations(buttons, 2) for p in pads(a) for q in pads(b))
    court_gap = min(_gap(court(a), court(b)) for a, b in itertools.combinations(buttons, 2))
    # distance from each M3 hole centre to the nearest courtyard corner/edge, minus its courtyard
    hole_gap = min(_point_gap((hx, M3_Y_EST), court(b)) - M3_COURT_R for hx, _ in M3_HOLES for b in buttons)
    inside = min(xs) >= WALL and max(xs) <= CASE_W - WALL and min(ys) >= WALL and max(ys) <= KEY_H - WALL
    side = [p for b in buttons if b[4] == "ra" for p in body(b)]
    below_latch = all(p[1] <= LATCH_BOTTOM for p in side)
    return dict(x=(min(xs), max(xs)), y=(min(ys), max(ys)), body_gap=body_gap, pad_gap=pad_gap,
                court_gap=court_gap, hole_gap=hole_gap, inside=inside, below_latch=below_latch)


def svg(buttons, outline, path):
    S = 6.0                                   # px per mm
    W, H = CASE_W * S + 40, CASE_H * S + 40

    def P(x, y):
        return f"{20 + x * S:.1f},{20 + (CASE_H - y) * S:.1f}"

    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W:.0f}" height="{H:.0f}" '
         f'viewBox="0 0 {W:.0f} {H:.0f}" font-family="sans-serif" font-size="10">',
         '<rect width="100%" height="100%" fill="#fff"/>']
    # case: key area, top strip, latch arms
    o.append(f'<polygon points="{P(0,0)} {P(CASE_W,0)} {P(CASE_W,BODY_H)} {P(0,BODY_H)}" fill="#f4f4f4" stroke="#333"/>')
    o.append(f'<polygon points="{P(0,KEY_H)} {P(CASE_W,KEY_H)} {P(CASE_W,BODY_H)} {P(0,BODY_H)}" fill="#e2e2e2" stroke="#333"/>')
    for x0 in (0, CASE_W - 2.2):
        o.append(f'<polygon points="{P(x0,LATCH_BOTTOM)} {P(x0+2.2,LATCH_BOTTOM)} {P(x0+2.2,CASE_H)} {P(x0,CASE_H)}" '
                 f'fill="#ccc" stroke="#333"/>')
    o.append(f'<polygon points="{P(HEADER_X-6.6,KEY_H+0.5)} {P(HEADER_X+6.6,KEY_H+0.5)} {P(HEADER_X+6.6,BODY_H-0.5)} '
             f'{P(HEADER_X-6.6,BODY_H-0.5)}" fill="none" stroke="#c33" stroke-dasharray="3 2"/>')
    o.append(f'<text x="{20+(HEADER_X+8)*S:.0f}" y="{20+(CASE_H-KEY_H-3)*S:.0f}" fill="#c33">2x5 header to the Tab5</text>')
    for x, _ in M3_HOLES:
        o.append(f'<line x1="{20+x*S}" y1="{20+(CASE_H-KEY_H)*S}" x2="{20+x*S}" y2="{20+CASE_H*S}" stroke="#36c" stroke-dasharray="2 3"/>')
    # wall margin
    o.append(f'<polygon points="{P(WALL,WALL)} {P(CASE_W-WALL,WALL)} {P(CASE_W-WALL,KEY_H-WALL)} {P(WALL,KEY_H-WALL)}" '
             f'fill="none" stroke="#999" stroke-dasharray="4 3"/>')
    # silhouette of the original pad
    if outline[0] == "rrect":
        _, cx, cy, hw, hh, rr = outline
        o.append(f'<rect x="{20+(cx-hw)*S:.1f}" y="{20+(CASE_H-cy-hh)*S:.1f}" width="{2*hw*S:.1f}" '
                 f'height="{2*hh*S:.1f}" rx="{rr*S:.1f}" fill="none" stroke="#2a8" stroke-width="1.5"/>')
    else:
        _, cx, cy, d, r = outline
        o.append(f'<path d="M {P(cx-d, cy+r)} L {P(cx+d, cy+r)} A {r*S:.1f} {r*S:.1f} 0 0 1 {P(cx+d, cy-r)} '
             f'L {P(cx-d, cy-r)} A {r*S:.1f} {r*S:.1f} 0 0 1 {P(cx-d, cy+r)} Z" fill="none" stroke="#2a8" stroke-width="1.5"/>')
    for b in buttons:
        o.append(f'<polygon points="{" ".join(P(*p) for p in body(b))}" fill="#fff8d0" stroke="#a80"/>')
        for p in pads(b):
            o.append(f'<circle cx="{P(*p).split(",")[0]}" cy="{P(*p).split(",")[1]}" r="{pad_r(b)*S:.1f}" fill="#c9a227"/>')
        if b[4] == "ra":
            d = -1 if b[5] == 0 else 1
            ex = b[2] + d * 5.0
            o.append(f'<polygon points="{P(ex, b[3]-1.5)} {P(ex, b[3]+1.5)} {P(ex + d*2.5, b[3])}" fill="#a80"/>')
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
        kind = "6x6 right-angle" if sz == "ra" else f"{sz}x{sz}"
        print(f"| {n} | `{bit}` | {x:.2f} | {y:.2f} | {kind} | {rot}° |")
    print(f"\nextent x {c['x'][0]:.1f}..{c['x'][1]:.1f}, y {c['y'][0]:.1f}..{c['y'][1]:.1f}; "
          f"closest bodies {c['body_gap']:.2f} mm; closest pads {c['pad_gap']:.2f} mm; "
          f"courtyards {c['court_gap']:.2f} mm; M3 holes {c['hole_gap']:.2f} mm; "
          f"inside wall margin: {c['inside']}; side buttons below latch arms: {c['below_latch']}")


if __name__ == "__main__":
    main()
