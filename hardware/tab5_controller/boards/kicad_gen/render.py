#!/usr/bin/env python3
"""Render a board's front/back views, schematic PNG and schematic PDF for its README.

    python3 render.py snes      # needs kicad-cli and cairosvg (pip install cairosvg)
"""
import glob
import os
import subprocess
import sys
import tempfile

import cairosvg

import gen_pcb as g


def svg_to_png(svg, png, width):
    cairosvg.svg2png(url=svg, write_to=png, output_width=width, background_color="white")


def main():
    cfg = g.configure(sys.argv[1])
    pcb, sch = cfg.pcb, os.path.join(cfg.dir, cfg.sch_file)
    with tempfile.TemporaryDirectory() as tmp:
        for side, layers, extra in (("front", "F.Cu,F.SilkS,F.Mask,Edge.Cuts", []),
                                    ("back", "B.Cu,B.SilkS,B.Mask,Edge.Cuts", ["--mirror"])):
            svg = os.path.join(tmp, side + ".svg")
            subprocess.run(["kicad-cli", "pcb", "export", "svg", "--exclude-drawing-sheet", "--page-size-mode", "2",
                            "-l", layers, "-o", svg, pcb] + extra, check=True, capture_output=True)
            svg_to_png(svg, os.path.join(cfg.dir, side + ".png"), 1200)
        subprocess.run(["kicad-cli", "sch", "export", "svg", "-o", tmp, sch], check=True, capture_output=True)
        svg_to_png(glob.glob(os.path.join(tmp, "*.svg"))[0], os.path.join(cfg.dir, "schematic.png"), 1600)
    subprocess.run(["kicad-cli", "sch", "export", "pdf", "-o",
                    os.path.join(cfg.dir, cfg.base + "_schematic.pdf"), sch], check=True, capture_output=True)
    print("rendered", cfg.dir)


if __name__ == "__main__":
    main()
