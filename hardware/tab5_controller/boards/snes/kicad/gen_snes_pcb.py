#!/usr/bin/env python3
"""Generate the RetroPad SNES board (console ID 3) as a KiCad 7 PCB.

    python3 gen_snes_pcb.py            # place + net -> retropad_snes_unrouted.kicad_pcb
    python3 gen_snes_pcb.py --route    # ...then Freerouting -> retropad_snes.kicad_pcb

Needs KiCad 7+ (its Python module `pcbnew`) and, for --route, Java 17+ and
freerouting 1.9.0 (FREEROUTING_JAR, default ./freerouting.jar; xvfb-run
when there is no display).

Button positions come from ../../layout.py, so the board always matches the
reviewed layout. Coordinates in this file follow layout.py: mm, front view,
x from the left edge of the case, y up from the bottom edge.
"""
import os
import shutil
import subprocess
import sys
import uuid

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))
import layout  # noqa: E402

FP = "/usr/share/kicad/footprints"
OUT_UNROUTED = os.path.join(HERE, "retropad_snes_unrouted.kicad_pcb")
OUT = os.path.join(HERE, "retropad_snes.kicad_pcb")

# Footprints carry the UUID of their schematic symbol (gen_snes_sch.py derives
# the same ones), which is what links the PCB to retropad_snes.kicad_sch.
UUID_NS = uuid.UUID("8d3c5a62-1f0e-4f7e-9a51-5e7a0b2c9d11")
SCH_FILE = "retropad_snes.kicad_sch"


def symbol_uuid(ref):
    return str(uuid.uuid5(UUID_NS, "retropad_snes/" + ref))

# Sheet origin: layout (0, 0) lands here, and layout y is flipped (KiCad y points down).
OX, OY = 40.0, 140.0

# ── Board outline (inside the shell) ─────────────────────────────────────────
WALL = layout.WALL
EDGE_TOP = 56.5                 # 1.45 below the body top (57.95)
LATCH_Y = 46.0                  # above this the latch arms take the side walls
LATCH_X = 3.0                   # board edge clears the 2.2 mm arms
OUTLINE = [(WALL, WALL), (layout.CASE_W - WALL, WALL), (layout.CASE_W - WALL, LATCH_Y),
           (layout.CASE_W - LATCH_X, LATCH_Y), (layout.CASE_W - LATCH_X, EDGE_TOP),
           (LATCH_X, EDGE_TOP), (LATCH_X, LATCH_Y), (WALL, LATCH_Y)]

# ── Connector to the Tab5 (verify before fabrication, see README) ────────────
HEADER_X = layout.HEADER_X      # centre of the 5 pin columns
HEADER_BODY_DEPTH = 6.58        # pin row 1 to the front face of the header body
M3_Y = 45.0                     # estimate; caliper-check on a real keyboard
M3_X = (16.0, 112.0)

# Console ID 3 = ID0 + ID1 bridged to GND
STRAPS = [("R6", "ID0", True), ("R7", "ID1", True), ("R8", "ID2", False),
          ("R9", "ID3", False), ("R10", "AN_EN", False)]

# STM32F030C8T6 LQFP48 pin -> net (pins not listed are left unconnected)
MCU_PINS = {
    1: "+3V3", 24: "+3V3", 48: "+3V3", 9: "+3V3",
    8: "GND", 23: "GND", 47: "GND",
    7: "NRST", 44: "BOOT0",
    10: "COL0", 11: "COL1", 12: "COL2", 13: "COL3",
    18: "ROW0", 19: "ROW1", 20: "ROW2", 39: "ROW3", 40: "ROW4", 41: "ROW5", 42: "ROW6", 43: "ROW7",
    21: "SCL", 22: "SDA", 38: "INT", 34: "SWDIO", 37: "SWCLK",
    25: "ID0", 26: "ID1", 27: "ID2", 45: "ID3", 46: "AN_EN",
}

# M5Stack Tab5 Keyboard P1 pinout (SCH_Tab5_Keyboard_SCH_V1.0)
HEADER_PINS = {1: None, 2: "GND", 3: "GND", 4: "GND", 5: "+3V3", 6: None,
               7: "SCL", 8: "SDA", 9: "INT", 10: None}

RP_BIT = {name: i for i, name in enumerate(
    "UP DOWN LEFT RIGHT A B C X Y Z L R L2 R2 START SELECT MENU VOLUME OPT1 OPT2".split())}


def mm(x, y):
    return pcbnew.VECTOR2I_MM(OX + x, OY - y)


class Builder:
    def __init__(self):
        self.board = pcbnew.BOARD()
        self.board.SetCopperLayerCount(2)
        ds = self.board.GetDesignSettings()
        ds.SetCopperLayerCount(2)
        ds.m_TrackMinWidth = pcbnew.FromMM(0.2)
        ds.m_MinClearance = pcbnew.FromMM(0.2)
        ds.m_ViasMinSize = pcbnew.FromMM(0.6)
        ds.m_MinThroughDrill = pcbnew.FromMM(0.3)
        ds.m_CopperEdgeClearance = pcbnew.FromMM(0.3)   # common fab minimum (e.g. JLCPCB)
        nc = ds.m_NetSettings.m_DefaultNetClass
        nc.SetTrackWidth(pcbnew.FromMM(0.25))
        nc.SetClearance(pcbnew.FromMM(0.2))
        nc.SetViaDiameter(pcbnew.FromMM(0.6))
        nc.SetViaDrill(pcbnew.FromMM(0.3))
        self.nets = {}

    def net(self, name):
        if name not in self.nets:
            n = pcbnew.NETINFO_ITEM(self.board, name)
            self.board.Add(n)
            self.nets[name] = n
        return self.nets[name]

    def place(self, lib, name, ref, value, x, y, rot=0.0, back=False, anchor="origin"):
        """Place a footprint. anchor='pads' centres the numbered pads on (x, y)."""
        fp = pcbnew.FootprintLoad(os.path.join(FP, lib + ".pretty"), name)
        fp.SetFPID(pcbnew.LIB_ID(lib, name))
        fp.SetReference(ref)
        fp.SetValue(value)
        fp.SetPath(pcbnew.KIID_PATH("/" + symbol_uuid(ref)))
        fp.SetProperty("Sheetfile", SCH_FILE)
        fp.SetProperty("Sheetname", "")
        self.board.Add(fp)
        fp.SetPosition(mm(x, y))
        fp.SetOrientationDegrees(rot)
        if back:
            fp.Flip(fp.GetPosition(), True)
        if anchor == "pads":
            pads = [p for p in fp.Pads() if p.GetNumber()]
            cx = sum(p.GetPosition().x for p in pads) / len(pads)
            cy = sum(p.GetPosition().y for p in pads) / len(pads)
            tgt = mm(x, y)
            fp.Move(pcbnew.VECTOR2I(int(tgt.x - cx), int(tgt.y - cy)))
        return fp

    def connect(self, fp, pin, netname):
        if netname is None:
            return
        for p in fp.Pads():
            if p.GetNumber() == str(pin):
                p.SetNet(self.net(netname))

    def line(self, layer, pts, closed=False, width=0.1):
        seq = pts + ([pts[0]] if closed else [])
        for a, b in zip(seq, seq[1:]):
            s = pcbnew.PCB_SHAPE(self.board)
            s.SetShape(pcbnew.SHAPE_T_SEGMENT)
            s.SetStart(mm(*a))
            s.SetEnd(mm(*b))
            s.SetLayer(layer)
            s.SetWidth(pcbnew.FromMM(width))
            self.board.Add(s)

    def text(self, layer, x, y, txt, size=1.0, mirror=False):
        t = pcbnew.PCB_TEXT(self.board)
        t.SetText(txt)
        t.SetPosition(mm(x, y))
        t.SetLayer(layer)
        t.SetTextSize(pcbnew.VECTOR2I_MM(size, size))
        t.SetTextThickness(pcbnew.FromMM(size * 0.15))
        if mirror:
            t.SetMirrored(True)
        self.board.Add(t)

    def zone(self, layer, netname):
        z = pcbnew.ZONE(self.board)
        z.SetLayer(layer)
        z.SetNet(self.net(netname))
        z.SetLocalClearance(pcbnew.FromMM(0.3))
        z.SetMinThickness(pcbnew.FromMM(0.25))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        ol = z.Outline()
        ol.NewOutline()
        for x, y in OUTLINE:
            ol.Append(mm(x + (0.3 if x < 64 else -0.3), y + (0.3 if y < 28 else -0.3)))
        self.board.Add(z)
        return z


def build():
    b = Builder()
    B = b.board
    buttons, _ = layout.snes()

    # Outline, mounting holes, labels
    b.line(pcbnew.Edge_Cuts, OUTLINE, closed=True, width=0.1)
    for i, x in enumerate(M3_X):
        b.place("MountingHole", "MountingHole_3.2mm_M3", f"H{i + 1}", "M3", x, M3_Y)
    b.text(pcbnew.F_SilkS, 64, 49.5, "RetroPad SNES  (console ID 3)", 1.2)
    b.text(pcbnew.B_SilkS, 100, 53.5, "RetroPad SNES rev 0.1", 1.0, mirror=True)

    # Buttons (front) and their matrix diodes (back)
    for n, (name, _bit, x, y, kind, rot) in enumerate(buttons, start=1):
        bit = RP_BIT[name]
        row, col = f"ROW{bit % 8}", f"COL{bit // 8}"
        key = f"K_{name}"
        sref, dref = f"SW{n}", f"D{n}"
        if kind == "ra":
            # Right-angle switch: body front flush with the side edge, actuator out
            left = rot == 0
            frot = 90.0 if left else -90.0
            px = (WALL + 2.59) if left else (layout.CASE_W - WALL - 2.59)
            # Pin 2 sits 4.5 mm from pin 1 along the edge (above it on the left
            # edge, below it on the right), so offset pin 1 to centre the pins on y.
            sw = b.place("Button_Switch_THT", "SW_Tactile_SPST_Angled_PTS645Vx31-2LFS", sref, name,
                         px, y - 2.25 if left else y + 2.25, frot)
            dx = x + (7.0 if left else -7.0)
            d = b.place("Diode_SMD", "D_SOD-123", dref, "1N4148W", dx, y, 90.0, back=True)
        else:
            lib, fpn = ("SW_PUSH_6mm", 6) if kind == 6 else ("SW_PUSH-12mm", 12)
            sw = b.place("Button_Switch_THT", lib, sref, name, x, y, rot, anchor="pads")
            d = b.place("Diode_SMD", "D_SOD-123", dref, "1N4148W", x, y, rot, back=True)
        # Switch: pin 1 -> diode cathode, pin 2 -> column. Diode anode -> row.
        b.connect(sw, 1, key)
        b.connect(sw, 2, col)
        b.connect(d, 1, key)    # SOD-123 pad 1 = cathode
        b.connect(d, 2, row)

    # MCU and support parts (back side, top-middle band)
    u = b.place("Package_QFP", "LQFP-48_7x7mm_P0.5mm", "U1", "STM32F030C8T6", 64.0, 47.0, 0.0, back=True)
    for pin, netname in MCU_PINS.items():
        b.connect(u, pin, netname)

    def passive(ref, value, x, y, a, c, rot=0.0, fp="R_0603_1608Metric", lib="Resistor_SMD"):
        f = b.place(lib, fp, ref, value, x, y, rot, back=True)
        b.connect(f, 1, a)
        b.connect(f, 2, c)
        return f

    cap = dict(fp="C_0603_1608Metric", lib="Capacitor_SMD")
    passive("C1", "100nF", 57.0, 47.0, "+3V3", "GND", 90.0, **cap)
    passive("C2", "100nF", 71.0, 47.0, "+3V3", "GND", 90.0, **cap)
    passive("C3", "100nF", 58.6, 41.4, "+3V3", "GND", 45.0, **cap)
    passive("C4", "4.7uF", 64.0, 53.6, "+3V3", "GND", 0.0, **cap)
    passive("C5", "100nF", 52.0, 41.0, "NRST", "GND", 90.0, **cap)
    passive("R1", "4.7k", 46.0, 47.0, "+3V3", "SCL", 90.0)
    passive("R2", "4.7k", 44.0, 47.0, "+3V3", "SDA", 90.0)
    passive("R3", "10k", 42.0, 47.0, "+3V3", "INT", 90.0)
    passive("R4", "10k", 54.0, 41.0, "+3V3", "NRST", 90.0)
    passive("R5", "10k", 76.0, 50.0, "BOOT0", "GND", 90.0)

    # Console-ID / analog straps: 0R to GND = bit set. The PCB carries all five;
    # the BOM decides the console (fit R6+R7 for SNES, leave R8-R10 unfitted).
    for i, (ref, sig, fitted) in enumerate(STRAPS):
        r = passive(ref, "0R" if fitted else "DNP", 79.0 + i * 3.0, 41.0, sig, "GND", 90.0)
        if not fitted:
            r.SetDNP(True) if hasattr(r, "SetDNP") else None
            r.SetExcludedFromBOM(True) if hasattr(r, "SetExcludedFromBOM") else None
    b.text(pcbnew.B_SilkS, 85.0, 37.8, "ID0 ID1 ID2 ID3 AN", 0.8, mirror=True)

    # 2x5 right-angle header to the Tab5: pins point up out of the top edge
    pin1_y = EDGE_TOP - HEADER_BODY_DEPTH
    j1 = b.place("Connector_PinHeader_2.54mm", "PinHeader_2x05_P2.54mm_Horizontal", "J1", "Tab5 Ext.Port1",
                 HEADER_X - 5.08, pin1_y, 90.0)
    for pin, netname in HEADER_PINS.items():
        b.connect(j1, pin, netname)
    b.text(pcbnew.F_SilkS, HEADER_X, 44.6, "VERIFY PIN 1 vs KEYBOARD", 0.8)

    # SWD (pads only; 1 = 3V3, 2 = SWCLK, 3 = SWDIO, 4 = NRST, 5 = GND as on the keyboard)
    j2 = b.place("Connector_PinHeader_2.54mm", "PinHeader_1x05_P2.54mm_Vertical", "J2", "SWD",
                 88.0, 53.5, 90.0, back=True)
    for pin, netname in {1: "+3V3", 2: "SWCLK", 3: "SWDIO", 4: "NRST", 5: "GND"}.items():
        b.connect(j2, pin, netname)
    b.text(pcbnew.B_SilkS, 93.0, 50.6, "SWD 3V3 CLK DIO RST GND", 0.8, mirror=True)

    return b


def drc(path):
    rpt = path.replace(".kicad_pcb", "_drc.rpt")
    pcbnew.WriteDRCReport(pcbnew.LoadBoard(path), rpt, pcbnew.EDA_UNITS_MILLIMETRES, True)
    return rpt


def _sexpr(text):
    """Minimal s-expression reader for Specctra session files."""
    import re
    stack, cur = [], []
    for tok in re.findall(r'"[^"]*"|[()]|[^\s()]+', text):
        if tok == "(":
            stack.append(cur)
            cur = []
        elif tok == ")":
            done, cur = cur, stack.pop()
            cur.append(done)
        else:
            cur.append(tok.strip('"'))
    return cur[0]


def import_ses(board, path):
    """Add Freerouting's wires and vias to the board.

    KiCad 7's pcbnew.ImportSpecctraSES() only works on the board open in the
    GUI, so the session file is read here instead. Session coordinates are in
    (resolution) units with y pointing up; KiCad's y points down.
    """
    ses = _sexpr(open(path).read())
    routes = next(x for x in ses if isinstance(x, list) and x[0] == "routes")
    res = next(x for x in routes if isinstance(x, list) and x[0] == "resolution")
    per_mm = {"um": 1000.0, "mil": 1 / 0.0254, "mm": 1.0}[res[1]] * float(res[2])
    layers = {"F.Cu": pcbnew.F_Cu, "B.Cu": pcbnew.B_Cu}
    vias = {}
    for item in routes:
        if isinstance(item, list) and item[0] == "library_out":
            for ps in item[1:]:
                if ps[0] == "padstack":
                    shape = next(x for x in ps if isinstance(x, list) and x[0] == "shape")
                    vias[ps[1]] = float(shape[1][2]) / per_mm

    def pt(x, y):
        return pcbnew.VECTOR2I_MM(float(x) / per_mm, -float(y) / per_mm)

    n_tracks = n_vias = 0
    net_out = next(x for x in routes if isinstance(x, list) and x[0] == "network_out")
    for net in net_out[1:]:
        ni = board.FindNet(net[1])
        for el in net[2:]:
            if el[0] == "wire":
                path_ = el[1]
                layer, width, xy = layers[path_[1]], float(path_[2]) / per_mm, path_[3:]
                pts = [pt(xy[i], xy[i + 1]) for i in range(0, len(xy) - 1, 2)]
                for a, b in zip(pts, pts[1:]):
                    t = pcbnew.PCB_TRACK(board)
                    t.SetStart(a)
                    t.SetEnd(b)
                    t.SetWidth(pcbnew.FromMM(width))
                    t.SetLayer(layer)
                    t.SetNet(ni)
                    board.Add(t)
                    n_tracks += 1
            elif el[0] == "via":
                v = pcbnew.PCB_VIA(board)
                v.SetPosition(pt(el[2], el[3]))
                v.SetWidth(pcbnew.FromMM(vias.get(el[1], 0.6)))
                v.SetDrill(pcbnew.FromMM(0.3))
                v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
                v.SetNet(ni)
                board.Add(v)
                n_vias += 1
    return n_tracks, n_vias


def write_bom(board, path):
    """Group footprints by value + footprint; unfitted straps are listed as DNP."""
    import collections
    import csv
    groups = collections.OrderedDict()
    for f in sorted(board.GetFootprints(), key=lambda f: (f.GetReference()[0], len(f.GetReference()), f.GetReference())):
        if f.GetReference().startswith("H"):
            continue
        dnp = f.GetValue() == "DNP"
        key = (f.GetValue(), str(f.GetFPID().GetUniStringLibItemName()), "back" if f.IsFlipped() else "front", dnp)
        groups.setdefault(key, []).append(f.GetReference())
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["Qty", "References", "Value", "Footprint", "Side", "Fit"])
        for (value, fpn, side, dnp), refs in groups.items():
            w.writerow([0 if dnp else len(refs), " ".join(refs), value, fpn, side, "DNP" if dnp else "yes"])
    return path


def main():
    route = "--route" in sys.argv
    b = build()
    pcbnew.SaveBoard(OUT_UNROUTED, b.board)
    print("saved", OUT_UNROUTED)
    if not route:
        return

    work = os.environ.get("ROUTE_DIR", os.path.join(HERE, "build"))
    os.makedirs(work, exist_ok=True)
    dsn = os.path.join(work, "retropad_snes.dsn")
    ses = os.path.join(work, "retropad_snes.ses")
    if "--reuse-ses" not in sys.argv or not os.path.exists(ses):
        if not pcbnew.ExportSpecctraDSN(b.board, dsn):
            sys.exit("DSN export failed")
        jar = os.environ.get("FREEROUTING_JAR", os.path.join(HERE, "freerouting.jar"))
        # Freerouting 1.9, single-threaded (2.x's multi-threaded optimiser has
        # returned sessions with nets missing). 1.9 opens a window, so run it
        # under a virtual display when there is no real one.
        cmd = ["java", "-jar", jar, "-de", dsn, "-do", ses, "-mp", "100", "-mt", "1"]
        if not os.environ.get("DISPLAY") and shutil.which("xvfb-run"):
            cmd = ["xvfb-run", "-a"] + cmd
        subprocess.run(cmd, check=True)
    board = pcbnew.LoadBoard(OUT_UNROUTED)
    print("imported %d track segments, %d vias" % import_ses(board, ses))

    # GND pours on both layers, after routing
    zb = Builder.__new__(Builder)
    zb.board = board
    zb.nets = {n: board.FindNet(n) for n in ("GND",)}
    zb.zone(pcbnew.F_Cu, "GND")
    zb.zone(pcbnew.B_Cu, "GND")
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    pcbnew.SaveBoard(OUT, board)
    print("saved", OUT, "| DRC report:", drc(OUT))
    print("BOM:", write_bom(board, os.path.join(HERE, "retropad_snes_bom.csv")))


if __name__ == "__main__":
    main()
