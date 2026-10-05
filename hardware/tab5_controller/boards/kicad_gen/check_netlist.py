#!/usr/bin/env python3
"""Check that a board's schematic and PCB have identical connectivity.

    python3 check_netlist.py snes

Exports the schematic netlist with kicad-cli and compares every named net,
pad by pad, with the board. Exit status 1 on any difference.
"""
import collections
import os
import subprocess
import sys
import tempfile

import pcbnew

import gen_sch as G


def sch_nets(cfg):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "sch.net")
        subprocess.run(["kicad-cli", "sch", "export", "netlist", "--format", "kicadsexpr", "-o", out,
                        os.path.join(cfg.dir, cfg.sch_file)], check=True, capture_output=True)
        tree = G.parse(open(out).read())

    def field(node, key):
        return next(x for x in node if isinstance(x, list) and x[0] == key)[1]

    nets = next(x for x in tree if isinstance(x, list) and x[0] == "nets")
    result = {}
    for n in nets[1:]:
        name = field(n, "name")
        if name.startswith("unconnected-"):
            continue
        result[name] = frozenset((field(x, "ref"), field(x, "pin"))
                                 for x in n if isinstance(x, list) and x[0] == "node")
    return result


def pcb_nets(cfg):
    board = pcbnew.LoadBoard(cfg.pcb)
    nets = collections.defaultdict(set)
    for f in board.GetFootprints():
        for p in f.Pads():
            if p.GetNumber() and p.GetNetname():
                nets[p.GetNetname()].add((f.GetReference(), p.GetNumber()))
    return {k: frozenset(v) for k, v in nets.items()}


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in G.pcb.layout.CONSOLES:
        sys.exit("usage: check_netlist.py {%s}" % ",".join(G.pcb.layout.CONSOLES))
    cfg = G.configure(sys.argv[1])
    sch, pcb = sch_nets(cfg), pcb_nets(cfg)
    diffs = 0
    for name in sorted(set(sch) | set(pcb)):
        a, b = sch.get(name, frozenset()), pcb.get(name, frozenset())
        if a != b:
            diffs += 1
            print(f"{name}: schematic only {sorted(a - b)}, PCB only {sorted(b - a)}")
    print(f"{len(sch)} schematic nets, {len(pcb)} PCB nets, {diffs} differences")
    sys.exit(1 if diffs else 0)


if __name__ == "__main__":
    main()
