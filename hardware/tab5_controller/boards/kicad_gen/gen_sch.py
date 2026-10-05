#!/usr/bin/env python3
"""Generate the KiCad 7 schematic for a RetroPad console board.

    python3 gen_sch.py snes        # writes ../snes/kicad/retropad_snes.kicad_sch

The connectivity is not typed in twice: the board from gen_pcb.build()
is the single source, and every symbol pin takes the net of the footprint pad
with the same number. Symbols are copied from KiCad's standard libraries and
get the same deterministic UUIDs that gen_pcb.py writes into the
footprints, so the schematic and the PCB stay linked in one KiCad project.
"""
import os
import re
import uuid

import sys

import gen_pcb as pcb

SYMS = "/usr/share/kicad/symbols"
PROJECT = None        # "retropad_<console>", set by configure()
ROOT_UUID = None


def configure(name):
    global PROJECT, ROOT_UUID
    cfg = pcb.configure(name)
    PROJECT = cfg.base
    ROOT_UUID = str(uuid.uuid5(pcb.UUID_NS, PROJECT + "/root"))
    return cfg
POWER = {"GND": "power:GND", "+3V3": "power:+3V3"}
STUB = 2.54


# ── s-expressions ────────────────────────────────────────────────────────────
class Sym(str):
    """A bare (unquoted) token."""


def parse(text):
    toks = re.findall(r'"(?:[^"\\]|\\.)*"|[()]|[^\s()]+', text)
    stack, cur = [], []
    for t in toks:
        if t == "(":
            stack.append(cur)
            cur = []
        elif t == ")":
            done, cur = cur, stack.pop()
            cur.append(done)
        elif t.startswith('"'):
            cur.append(t[1:-1].replace('\\"', '"').replace("\\\\", "\\"))
        else:
            cur.append(Sym(t))
    return cur[0]


def dump(x, ind=0):
    if isinstance(x, list):
        inner = [dump(e, ind + 1) for e in x]
        flat = "(" + " ".join(inner) + ")"
        if len(flat) < 100 and "\n" not in flat:
            return flat
        return "(" + inner[0] + "".join("\n" + "  " * (ind + 1) + e for e in inner[1:]) + ")"
    if isinstance(x, Sym):
        return str(x)
    if isinstance(x, (int, float)):
        return ("%.4f" % x).rstrip("0").rstrip(".")
    return '"' + str(x).replace("\\", "\\\\").replace('"', '\\"') + '"'


def S(*a):
    return [Sym(a[0])] + list(a[1:])


def uid(key):
    return str(uuid.uuid5(pcb.UUID_NS, PROJECT + "/" + key))


# ── library symbols ──────────────────────────────────────────────────────────
_libs = {}


def lib_symbol(lib_id):
    lib, name = lib_id.split(":")
    if lib not in _libs:
        _libs[lib] = parse(open(os.path.join(SYMS, lib + ".kicad_sym")).read())
    for s in _libs[lib]:
        if isinstance(s, list) and s[0] == "symbol" and s[1] == name:
            s = list(s)
            s[1] = lib_id
            return s
    raise KeyError(lib_id)


def pins_of(sym):
    """{number: (x, y, angle)} for a library symbol (unit 1 / common unit)."""
    out = {}

    def walk(node):
        for e in node:
            if isinstance(e, list):
                if e[0] == "pin":
                    at = next(x for x in e if isinstance(x, list) and x[0] == "at")
                    num = next(x for x in e if isinstance(x, list) and x[0] == "number")
                    out[num[1]] = (float(at[1]), float(at[2]), float(at[3]))
                elif e[0] == "symbol":
                    walk(e)
    walk(sym)
    return out


# ── schematic builder ───────────────────────────────────────────────────────
class Sheet:
    def __init__(self):
        self.items = []
        self.used = {}
        self.pwr = 0

    def lib(self, lib_id):
        if lib_id not in self.used:
            self.used[lib_id] = lib_symbol(lib_id)
        return self.used[lib_id]

    def symbol(self, lib_id, ref, value, x, y, rot=0, footprint="", key=None, in_bom=True, dnp=False,
               hide_ref=False, ref_at=(2.54, -1.27), val_at=(2.54, 1.27)):
        sym = self.lib(lib_id)
        pins = pins_of(sym)
        u = uid(key or ref)
        props = [
            S("property", "Reference", ref, S("at", x + ref_at[0], y + ref_at[1], 0),
              S("effects", S("font", S("size", 1.27, 1.27)), S("justify", Sym("left"))) + ([Sym("hide")] if hide_ref else [])),
            S("property", "Value", value, S("at", x + val_at[0], y + val_at[1], 0),
              S("effects", S("font", S("size", 1.27, 1.27)), S("justify", Sym("left")))),
            S("property", "Footprint", footprint, S("at", x, y, 0),
              S("effects", S("font", S("size", 1.27, 1.27)), Sym("hide"))),
            S("property", "Datasheet", "~", S("at", x, y, 0), S("effects", S("font", S("size", 1.27, 1.27)), Sym("hide"))),
        ]
        node = S("symbol", S("lib_id", lib_id), S("at", x, y, rot), S("unit", 1),
                 S("in_bom", Sym("yes" if in_bom else "no")), S("on_board", Sym("yes")),
                 S("dnp", Sym("yes" if dnp else "no")), S("uuid", u), *props)
        for n in pins:
            node.append(S("pin", n, S("uuid", uid((key or ref) + "/pin/" + n))))
        node.append(S("instances", S("project", PROJECT, S("path", "/" + ROOT_UUID, S("reference", ref), S("unit", 1)))))
        self.items.append(node)
        return {n: self.pin_xy(x, y, rot, p) for n, p in pins.items()}

    @staticmethod
    def pin_xy(x, y, rot, p):
        """Schematic position and outward direction (deg) of a pin."""
        px, py, pa = p
        r = rot % 360
        # library y is up, schematic y is down; rotation is counter-clockwise
        rx, ry = {0: (px, py), 90: (-py, px), 180: (-px, -py), 270: (py, -px)}[r]
        out = (pa + 180 + r) % 360           # pin body points into the symbol
        return (round(x + rx, 2), round(y - ry, 2), out)

    def wire(self, a, b):
        self.items.append(S("wire", S("pts", S("xy", *a), S("xy", *b)),
                            S("stroke", S("width", 0), S("type", Sym("default"))), S("uuid", uid("w/%s/%s" % (a, b)))))

    def label(self, net, x, y, ang):
        self.items.append(S("global_label", net, S("shape", Sym("bidirectional")), S("at", x, y, ang),
                            S("fields_autoplaced"),
                            S("effects", S("font", S("size", 1.27, 1.27)),
                              S("justify", Sym("left" if ang in (0, 90) else "right"))),
                            S("uuid", uid("gl/%s/%s/%s" % (net, x, y))),
                            S("property", "Intersheetrefs", "${INTERSHEET_REFS}", S("at", x, y, 0),
                              S("effects", S("font", S("size", 1.27, 1.27)), Sym("hide")))))

    def power(self, net, x, y, ang):
        self.pwr += 1
        lib_id = POWER[net]
        # GND hangs below its pin; +3V3 stands above it
        rot = {"GND": {270: 0, 90: 180, 0: 90, 180: 270}, "+3V3": {90: 0, 270: 180, 0: 270, 180: 90}}[net][ang]
        self.symbol(lib_id, "#PWR%02d" % self.pwr, net, x, y, rot, key="pwr%d" % self.pwr, in_bom=False,
                    hide_ref=True)

    def noconnect(self, x, y):
        self.items.append(S("no_connect", S("at", x, y), S("uuid", uid("nc/%s/%s" % (x, y)))))

    def text(self, s, x, y, size=1.27):
        self.items.append(S("text", s, S("at", x, y, 0), S("effects", S("font", S("size", size, size)),
                                                              S("justify", Sym("left"))), S("uuid", uid("t/" + s))))

    def terminate(self, pin, net, stub=STUB, symbol=True, power_as_label=False):
        """Stub + global label / power symbol / no-connect on one pin.

        Returns the stub end. symbol=False draws only the stub (for power pins
        that share one rail and one power symbol)."""
        x, y, ang = pin
        if not net:
            self.noconnect(x, y)
            return None
        dx, dy = {0: (stub, 0), 90: (0, -stub), 180: (-stub, 0), 270: (0, stub)}[int(ang)]
        end = (round(x + dx, 2), round(y + dy, 2))
        self.wire((x, y), end)
        if not symbol:
            return end
        if net in POWER and not power_as_label:
            self.power(net, end[0], end[1], int(ang))
        else:
            self.label(net, end[0], end[1], int(ang))
        return end

    def save(self, path):
        doc = S("kicad_sch", S("version", 20230121), S("generator", Sym("eeschema")),
                S("uuid", ROOT_UUID), S("paper", "A3"),
                S("title_block", S("title", "RetroPad %s controller (console ID %d)" % (pcb.CFG.title, pcb.CFG.id)), S("date", "2026-10-05"),
                  S("rev", "0.1"), S("company", "RetroESP32-P4"),
                  S("comment", 1, "Generated by kicad_gen/gen_sch.py from gen_pcb.py and layout.py"),
                  S("comment", 2, "Tab5 keyboard-port board: STM32F030 + RetroPad firmware, I2C 0x6D")),
                S("lib_symbols", *self.used.values()), *self.items,
                S("sheet_instances", S("path", "/", S("page", "1"))))
        with open(path, "w") as f:
            f.write(dump(doc) + "\n")


# ── page layout ──────────────────────────────────────────────────────────────
def lib_for(fp):
    ref = fp.GetReference()
    if ref.startswith("U"):
        return "MCU_ST_STM32F0:STM32F030C8Tx"
    if ref.startswith("SW"):
        return "Switch:SW_Push"
    if ref.startswith("D"):
        return "Device:D"
    if ref.startswith("C"):
        return "Device:C"
    if ref.startswith("R"):
        return "Device:R"
    if ref == "J1":
        return "Connector_Generic:Conn_02x05_Odd_Even"
    if ref == "J2":
        return "Connector_Generic:Conn_01x05"
    if ref.startswith("H"):
        return "Mechanical:MountingHole"
    raise KeyError(ref)


def g(v):
    """Snap to the 1.27 mm schematic grid."""
    return round(round(v / 1.27) * 1.27, 2)


def build():
    board = pcb.build().board
    fps = {f.GetReference(): f for f in board.GetFootprints()}

    def nets(ref):
        d = {}
        for p in fps[ref].Pads():
            if p.GetNumber():
                d[p.GetNumber()] = p.GetNetname() or None
        return d

    def fpid(ref):
        f = fps[ref].GetFPID()
        return "%s:%s" % (f.GetLibNickname(), f.GetLibItemName())

    sh = Sheet()

    def place(ref, x, y, rot=0, stub=STUB, power_as_label=False, **kw):
        f = fps[ref]
        dnp = f.GetValue() == "DNP"
        pins = sh.symbol(lib_for(f), ref, f.GetValue(), g(x), g(y), rot, footprint=fpid(ref),
                         in_bom=not dnp, dnp=dnp, **kw)
        n = nets(ref)
        # Power pins on the same side of a symbol share one rail and one symbol
        rails = {}
        for num, pin in pins.items():
            if n.get(num) in POWER:
                rails.setdefault((n[num], int(pin[2])), []).append(pin)
        # Connectors name their supply pins with labels instead, so adjacent
        # GND / +3V3 pins don't stack power symbols on top of each other.
        shared = {} if power_as_label else {k: v for k, v in rails.items() if len(v) > 1}
        for num, pin in pins.items():
            if (n.get(num), int(pin[2])) not in shared:
                sh.terminate(pin, n.get(num), stub, power_as_label=power_as_label)
        for (net, ang), plist in shared.items():
            ends = sorted(sh.terminate(p, net, stub, symbol=False) for p in plist)
            for a, b in zip(ends, ends[1:]):
                sh.wire(a, b)
            sh.power(net, ends[0][0], ends[0][1], ang)
        return pins

    # MCU
    sh.text("MCU — STM32F030C8T6 running the RetroPad firmware (I2C slave 0x6D)", 30, 25, 2.0)
    place("U1", 80, 95, stub=5.08)

    # Tab5 connector + SWD
    sh.text("Tab5 Ext.Port1 (M5Stack Tab5 Keyboard P1 pinout) — verify pin 1 orientation before fab", 150, 25, 1.5)
    place("J1", 175, 45, stub=7.62, power_as_label=True, ref_at=(0, -10.16), val_at=(0, 10.16))
    sh.text("SWD (same order as the keyboard's P2)", 150, 70, 1.5)
    place("J2", 175, 85, stub=7.62, power_as_label=True, ref_at=(0, -10.16), val_at=(0, 10.16))

    # Support parts
    sh.text("Decoupling, pull-ups, reset, BOOT0 (copied from the M5Stack keyboard)", 150, 110, 1.5)
    for i, ref in enumerate(["C1", "C2", "C3", "C5", "C4"]):
        place(ref, 160 + i * 15, 130)
    for i, ref in enumerate(["R1", "R2", "R3", "R4", "R5"]):
        place(ref, 160 + i * 15, 155)
    sh.text("Console ID straps: 0R to GND = bit set. " + pcb.CFG.strap_note(), 150, 172, 1.5)
    for i, ref in enumerate(["R6", "R7", "R8", "R9", "R10"]):
        place(ref, 160 + i * 15, 190)
    sh.text("Mounting holes (M3, 96 mm apart)", 150, 210, 1.5)
    place("H1", 160, 220)
    place("H2", 175, 220)

    # Button matrix: ROWn -> diode (A->K) -> switch -> COLn, one line per button
    sh.text("Button matrix — row driven high, column read with pull-down; diode anode on the row", 260, 25, 1.5)
    buttons = pcb.CFG.buttons()
    for i, (name, *_r) in enumerate(buttons):
        n = i + 1
        y = g(40 + i * 12.7)
        dref, sref = "D%d" % n, "SW%d" % n
        dn, sn = nets(dref), nets(sref)
        # Device:D rotated 180: anode (pin 2) on the left, cathode (pin 1) on the right
        dp = sh.symbol(lib_for(fps[dref]), dref, fps[dref].GetValue(), g(285), y, 180, footprint=fpid(dref),
                       ref_at=(-2.54, -3.81), val_at=(-2.54, 3.81))
        sp = sh.symbol(lib_for(fps[sref]), sref, fps[sref].GetValue(), g(305), y, 0, footprint=fpid(sref),
                       ref_at=(-2.54, -5.08), val_at=(-2.54, 3.81))
        sh.terminate(dp["2"], dn["2"])                  # row label on the anode
        sh.wire(dp["1"][:2], sp["1"][:2])               # cathode -> switch
        k = dn["1"]
        mid = (g((dp["1"][0] + sp["1"][0]) / 2), y)
        sh.label(k, mid[0], mid[1], 90)                 # names the K_<button> net
        sh.terminate(sp["2"], sn["2"])                  # column label on the switch
        sh.text(name, g(325), y - 1.27, 1.27)
    return sh


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in pcb.layout.CONSOLES:
        sys.exit("usage: gen_sch.py {%s}" % ",".join(pcb.layout.CONSOLES))
    cfg = configure(sys.argv[1])
    out = os.path.join(cfg.dir, cfg.sch_file)
    build().save(out)
    print("saved", out)


if __name__ == "__main__":
    main()
