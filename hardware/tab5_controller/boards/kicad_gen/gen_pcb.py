#!/usr/bin/env python3
"""Generate a RetroPad console board as a KiCad 7 PCB.

    python3 gen_pcb.py snes            # place + net -> ../snes/kicad/retropad_snes_unrouted.kicad_pcb
    python3 gen_pcb.py snes --route    # ...then Freerouting -> ../snes/kicad/retropad_snes.kicad_pcb

The console names and ids come from ../layout.py (CONSOLES / BOARDS).

Needs KiCad 7+ (its Python module `pcbnew`) and, for --route, Java 17+ and
freerouting 1.9.0 (FREEROUTING_JAR, default ./freerouting.jar; xvfb-run
when there is no display).

Button positions come from ../layout.py, so the board always matches the
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
BOARDS_DIR = os.path.dirname(HERE)
sys.path.insert(0, BOARDS_DIR)
import layout  # noqa: E402

FP = "/usr/share/kicad/footprints"

# Project footprint library (kicad_lib/RetroPad.pretty): 4-pin versions of
# KiCad's tact-switch footprints, written by make_switch_footprints().
LIB_DIR = os.path.join(BOARDS_DIR, "kicad_lib")
LOCAL_LIB = "RetroPad"

# 4-leg tact switches: KiCad's footprints give each internally tied pair one
# number (1, 1, 2, 2). The 4-pin copies number the legs 1-4 like the parts'
# own symbols: pad 1 top-left, then clockwise. 1-2 and 3-4 are the tied pairs
# (the legs 6.5 / 12.5 mm apart); the contact joins the two pairs.
SWITCH_4PIN = {
    "SW_PUSH_6mm_4pin": ("SW_PUSH_6mm", {(0.0, 0.0): "1", (6.5, 0.0): "2", (6.5, 4.5): "3", (0.0, 4.5): "4"}),
    "SW_PUSH-12mm_4pin": ("SW_PUSH-12mm", {(0.0, 0.0): "1", (12.5, 0.0): "2", (12.5, 5.0): "3", (0.0, 5.0): "4"}),
}


def make_switch_footprints():
    """Write the 4-pin switch footprints into kicad_lib/RetroPad.pretty."""
    import re
    out_dir = os.path.join(LIB_DIR, LOCAL_LIB + ".pretty")
    os.makedirs(out_dir, exist_ok=True)
    for name, (src, numbers) in SWITCH_4PIN.items():
        text = open(os.path.join(FP, "Button_Switch_THT.pretty", src + ".kicad_mod")).read()
        text = text.replace('(footprint "%s"' % src, '(footprint "%s"' % name, 1)
        text = text.replace('(module %s ' % src, '(module %s ' % name, 1)

        def renumber(m):
            key = (float(m.group(3)), float(m.group(4)))
            return '(pad "%s" %s(at %s %s' % (numbers[key], m.group(2), m.group(3), m.group(4))
        text, n = re.subn(r'\(pad "(\d)" (thru_hole \w+ )\(at ([-\d.]+) ([-\d.]+)', renumber, text)
        assert n == 4, (name, n)
        text = re.sub(r'\(descr "([^"]*)"\)', r'(descr "\1; 4-pin numbering 1-4, tied pairs 1-2 and 3-4")', text, 1)
        with open(os.path.join(out_dir, name + ".kicad_mod"), "w") as f:
            f.write(text)
    return out_dir


def write_lib_tables(board_dir):
    """Point each board's KiCad project at the RetroPad footprint library."""
    rel = os.path.relpath(os.path.join(LIB_DIR, LOCAL_LIB + ".pretty"), board_dir)
    with open(os.path.join(board_dir, "fp-lib-table"), "w") as f:
        f.write('(fp_lib_table\n  (version 7)\n  (lib (name "%s")(type "KiCad")(uri "${KIPRJMOD}/%s")'
                '(options "")(descr "RetroPad 4-pin switch footprints"))\n)\n' % (LOCAL_LIB, rel))

# Footprints carry the UUID of their schematic symbol (gen_sch.py derives the
# same ones), which is what links each PCB to its schematic.
UUID_NS = uuid.UUID("8d3c5a62-1f0e-4f7e-9a51-5e7a0b2c9d11")


class Console:
    """Names, paths and console-ID straps for one board."""

    def __init__(self, name):
        self.name = name
        self.id, self.title = layout.CONSOLES[name]
        self.base = "retropad_" + name
        self.dir = os.path.join(BOARDS_DIR, name, "kicad")
        self.pcb = os.path.join(self.dir, self.base + ".kicad_pcb")
        self.pcb_unrouted = os.path.join(self.dir, self.base + "_unrouted.kicad_pcb")
        self.sch_file = self.base + ".kicad_sch"
        # ID0..ID3 = bits of the console id (0R fitted = bit set)
        self.straps = [("R%d" % (6 + i), "ID%d" % i, bool(self.id >> i & 1)) for i in range(4)]

    def buttons(self):
        return layout.BOARDS[self.name]()[0]

    def strap_note(self):
        fit = [r for r, _, f in self.straps if f]
        dnp = [r for r, _, f in self.straps if not f]
        return "%s = ID %d (fit %s; %s DNP)" % (self.title, self.id, ", ".join(fit) or "none", ", ".join(dnp))


CFG = None


def configure(name):
    global CFG
    CFG = Console(name)
    os.makedirs(CFG.dir, exist_ok=True)
    return CFG


def symbol_uuid(ref):
    return str(uuid.uuid5(UUID_NS, CFG.base + "/" + ref))

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

# AVR32DD28 SOIC-28 pin -> net (checked against DxCore's pinout from Microchip's
# DFP and KiCad's pin-compatible AVR32DB28 symbol). Button slots are added from
# layout.avrdd_slots(); pins not listed are left unconnected.
AVRDD_PIN_NO = {"PA7": 1, "PC0": 2, "PC1": 3, "PC2": 4, "PC3": 5, "PD1": 7, "PD2": 8, "PD3": 9,
                "PD4": 10, "PD5": 11, "PD6": 12, "PD7": 13, "PF0": 16, "PF1": 17, "PA0": 22,
                "PA1": 23, "PA2": 24, "PA3": 25, "PA4": 26, "PA5": 27, "PA6": 28}
AVRDD_PINS = {
    14: "+3V3", 20: "+3V3", 6: "+3V3",          # VDD, VDD, VDDIO2 (PORTC supply)
    15: "GND", 21: "GND",
    18: "RESET", 19: "UPDI",                    # PF6 / PF7
    24: "SDA", 25: "SCL",                       # PA2 / PA3 (TWI0 default)
    22: "ID0", 23: "ID1", 28: "ID2", 1: "ID3",  # PA0 / PA1 / PA6 / PA7
    26: "AN0", 27: "AN1",                       # PA4 / PA5 (AIN24 / AIN25)
}

# M5Stack Tab5 Keyboard P1 pinout (SCH_Tab5_Keyboard_SCH_V1.0). INT (pin 9)
# is not wired (the host polls); G9 (pin 10) is unused, as on the keyboard.
HEADER_PINS = {1: None, 2: "GND", 3: "GND", 4: "GND", 5: "+3V3", 6: None,
               7: "SCL", 8: "SDA", 9: None, 10: None}


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
        lib_dir = os.path.join(LIB_DIR, lib + ".pretty") if lib == LOCAL_LIB else os.path.join(FP, lib + ".pretty")
        fp = pcbnew.FootprintLoad(lib_dir, name)
        fp.SetFPID(pcbnew.LIB_ID(lib, name))
        fp.SetReference(ref)
        fp.SetValue(value)
        fp.SetPath(pcbnew.KIID_PATH("/" + symbol_uuid(ref)))
        fp.SetProperty("Sheetfile", CFG.sch_file)
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
        # Thermal reliefs on through-hole pads only; small SMD pads connect solidly
        # (a small SMD pad squeezed by tracks can otherwise end up with one spoke).
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THT_THERMAL)
        ol = z.Outline()
        ol.NewOutline()
        for x, y in OUTLINE:
            ol.Append(mm(x + (0.3 if x < 64 else -0.3), y + (0.3 if y < 28 else -0.3)))
        self.board.Add(z)
        return z


def build_core(b):
    """AVR32DD28 (SOIC-28) on the back, one pin per button, UPDI programming."""
    u = b.place("Package_SO", "SOIC-28W_7.5x17.9mm_P1.27mm", "U1", "AVR32DD28-I/SO", 64.0, 46.5, 90.0, back=True)
    pins = dict(AVRDD_PINS)
    for name, _rp, slot in layout.avrdd_slots(CFG.name):
        pins[AVRDD_PIN_NO[slot]] = "K_" + name
    for pin, netname in pins.items():
        b.connect(u, pin, netname)

    def passive(ref, value, x, y, a, c, rot=0.0, fp="R_0805_2012Metric_Pad1.20x1.40mm_HandSolder", lib="Resistor_SMD"):
        f = b.place(lib, fp, ref, value, x, y, rot, back=True)
        b.connect(f, 1, a)
        b.connect(f, 2, c)
        return f

    cap = dict(fp="C_0805_2012Metric_Pad1.18x1.45mm_HandSolder", lib="Capacitor_SMD")
    passive("C1", "100nF", 52.0, 44.0, "+3V3", "GND", 90.0, **cap)    # VDD (pin 14)
    passive("C2", "100nF", 62.1, 54.3, "+3V3", "GND", 0.0, **cap)     # VDD (pin 20)
    passive("C3", "100nF", 66.0, 38.4, "+3V3", "GND", 0.0, **cap)     # VDDIO2 (pin 6)
    passive("C4", "4.7uF", 76.0, 49.5, "+3V3", "GND", 90.0, **cap)    # bulk
    passive("R1", "4.7k", 46.0, 47.0, "+3V3", "SCL", 90.0)
    passive("R2", "4.7k", 44.0, 47.0, "+3V3", "SDA", 90.0)
    passive("R3", "10k", 57.6, 54.3, "+3V3", "RESET", 90.0)

    # Console-ID straps: 0R to GND = bit set (internal pull-ups read at boot)
    # Each strap gets its own label centred under it. One mirrored string would read
    # in the wrong order from the back, where the parts appear right to left.
    for i, (ref, sig, fitted) in enumerate(CFG.straps):
        x = 81.0 + i * 3.0
        r = passive(ref, "0R" if fitted else "DNP", x, 41.0, sig, "GND", 90.0)
        if not fitted:
            r.SetExcludedFromBOM(True)
        b.text(pcbnew.B_SilkS, x, 37.8, sig, 0.8, mirror=True)

    place_header(b, HEADER_PINS)

    # UPDI programming header: 1 = 3V3, 2 = UPDI, 3 = GND
    j2 = b.place("Connector_PinHeader_2.54mm", "PinHeader_1x03_P2.54mm_Vertical", "J2", "UPDI",
                 90.0, 53.5, 90.0, back=True)
    for pin, netname in {1: "+3V3", 2: "UPDI", 3: "GND"}.items():
        b.connect(j2, pin, netname)
    for p in j2.Pads():
        if p.GetNetname() == "GND":
            p.SetZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
    b.text(pcbnew.B_SilkS, 92.5, 50.6, "UPDI 3V3 UPDI GND", 0.8, mirror=True)


def place_header(b, pins):
    """2x5 right-angle header to the Tab5: pins point up out of the top edge."""
    pin1_y = EDGE_TOP - HEADER_BODY_DEPTH
    j1 = b.place("Connector_PinHeader_2.54mm", "PinHeader_2x05_P2.54mm_Horizontal", "J1", "Tab5 Ext.Port1",
                 HEADER_X - 5.08, pin1_y, 90.0)
    for pin, netname in pins.items():
        b.connect(j1, pin, netname)
    # Connector GND pins (here and on J2) connect solidly: tracks around them
    # can leave a thermal relief with a single spoke
    for p in j1.Pads():
        if p.GetNetname() == "GND":
            p.SetZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
    b.text(pcbnew.F_SilkS, HEADER_X, 44.6, "VERIFY PIN 1 vs KEYBOARD", 0.8)
    return j1


def build():
    b = Builder()
    buttons = CFG.buttons()

    # Outline, mounting holes, labels
    b.line(pcbnew.Edge_Cuts, OUTLINE, closed=True, width=0.1)
    for i, x in enumerate(M3_X):
        b.place("MountingHole", "MountingHole_3.2mm_M3", f"H{i + 1}", "M3", x, M3_Y)
    b.text(pcbnew.F_SilkS, 64, 49.5, "RetroPad %s  (console ID %d)" % (CFG.title, CFG.id), 1.2)
    b.text(pcbnew.B_SilkS, 100, 53.5, "RetroPad %s rev 0.1" % CFG.name.upper(), 1.0, mirror=True)

    # Buttons (front): one MCU pin each, switch to GND, internal pull-up in the MCU.
    # 4-leg tact switches: tied pair 1-2 to GND, tied pair 3-4 to K_<button>.
    make_switch_footprints()
    for n, (name, _rp, x, y, kind, rot) in enumerate(buttons, start=1):
        sref = f"SW{n}"
        if kind == "ra":
            # Right-angle switch: body front flush with the side edge, actuator out
            left = rot == 0
            frot = 90.0 if left else -90.0
            px = (WALL + 2.59) if left else (layout.CASE_W - WALL - 2.59)
            # Pin 2 sits 4.5 mm from pin 1 along the edge (above it on the left
            # edge, below it on the right), so offset pin 1 to centre the pins on y.
            # Two contacts (pads 1 and 2); the other two legs are mounting pegs
            sw = b.place("Button_Switch_THT", "SW_Tactile_SPST_Angled_PTS645Vx31-2LFS", sref, name,
                         px, y - 2.25 if left else y + 2.25, frot)
            b.connect(sw, 1, f"K_{name}")
            b.connect(sw, 2, "GND")
        else:
            fpn = "SW_PUSH_6mm_4pin" if kind == 6 else "SW_PUSH-12mm_4pin"
            sw = b.place(LOCAL_LIB, fpn, sref, name, x, y, rot, anchor="pads")
            for pin, netname in ((1, "GND"), (2, "GND"), (3, f"K_{name}"), (4, f"K_{name}")):
                b.connect(sw, pin, netname)

    build_core(b)
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


# Orderable parts for the switch and Tab5-header footprints. The KiCad footprints were drawn for
# these families: SW_PUSH_6mm(_4pin) = 6x6 mm THT tact (6.5 x 4.5 mm pins),
# SW_PUSH-12mm(_4pin) = Omron B3F-40xx (12.5 x 5.0 mm pins), and the angled footprint
# is named after the C&K PTS645Vx31. A DigiKey number is filled in only where it
# was checked against DigiKey's own listing; otherwise search DigiKey by MPN.
PARTS = {
    "SW_PUSH_6mm_4pin": ("C&K", "PTS645SM43-2 LFS", ""),
    "SW_PUSH-12mm_4pin": ("Omron", "B3F-4055", "SW414-ND"),
    "SW_Tactile_SPST_Angled_PTS645Vx31-2LFS": ("C&K", "PTS645VL31-2 LFS", "CKN9094-ND"),
    # J1 to the Tab5: 2x5 right-angle male, 5.84 mm mating pins. Confirm the pin
    # length against M5Stack's keyboard before ordering (see README checklist).
    "PinHeader_2x05_P2.54mm_Horizontal": ("Samtec", "TSW-105-08-G-D-RA", "SAM1037-05-ND"),
}


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
        w.writerow(["Qty", "References", "Value", "Footprint", "Side", "Fit", "Manufacturer", "MPN", "DigiKey"])
        for (value, fpn, side, dnp), refs in groups.items():
            mfr, mpn, dk = PARTS.get(fpn, ("", "", ""))
            w.writerow([0 if dnp else len(refs), " ".join(refs), value, fpn, side, "DNP" if dnp else "yes",
                        mfr, mpn, dk])
    return path


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 1 or args[0] not in layout.CONSOLES:
        sys.exit("usage: gen_pcb.py {%s} [--route] [--reuse-ses] [--bom-only]" % ",".join(layout.CONSOLES))
    configure(args[0])
    OUT, OUT_UNROUTED = CFG.pcb, CFG.pcb_unrouted
    if "--bom-only" in sys.argv:
        # Rewrite the BOM from the routed board, without re-placing or re-routing
        print("BOM:", write_bom(pcbnew.LoadBoard(OUT), os.path.join(CFG.dir, CFG.base + "_bom.csv")))
        return
    route = "--route" in sys.argv
    write_lib_tables(CFG.dir)
    b = build()
    pcbnew.SaveBoard(OUT_UNROUTED, b.board)
    print("saved", OUT_UNROUTED)
    if not route:
        return

    work = os.environ.get("ROUTE_DIR", os.path.join(HERE, "build"))
    os.makedirs(work, exist_ok=True)
    dsn = os.path.join(work, CFG.base + ".dsn")
    ses = os.path.join(work, CFG.base + ".ses")
    if "--reuse-ses" not in sys.argv or not os.path.exists(ses):
        if not pcbnew.ExportSpecctraDSN(b.board, dsn):
            sys.exit("DSN export failed")
        jar = os.environ.get("FREEROUTING_JAR", os.path.join(HERE, "freerouting.jar"))
        # Freerouting 1.9, single-threaded (2.x's multi-threaded optimiser has
        # returned sessions with nets missing). 1.9 opens a window, so run it
        # under a virtual display when there is no real one.
        passes = os.environ.get("FR_PASSES", "100")
        cmd = ["java", "-jar", jar, "-de", dsn, "-do", ses, "-mp", passes, "-mt", "1"]
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
    print("BOM:", write_bom(board, os.path.join(CFG.dir, CFG.base + "_bom.csv")))


if __name__ == "__main__":
    main()
