# NES board (console ID 1)

![NES layout inside the Tab5 Keyboard envelope](layout.svg)

The coordinate conventions, case outline and markings are the same as on the
[SNES board](../snes/README.md). The d-pad uses the SNES board's positions so all boards feel
the same.

## Layout

* **B and A:** 12×12 switches side by side, just below the centre line, as on the NES pad. They are turned 90° so their pins run vertically, which keeps the two switches' pads well apart.
* **SELECT / START:** flat 6×6 switches, like the NES's rubber pills.
* **MENU:** centred above SELECT/START. It opens the in-game menu.
* **L / R:** none (the NES pad has no shoulder buttons).

| Button | RetroPad bit | x | y | Switch | Rotation |
|---|---|---|---|---|---|
| UP | `RP_BTN_UP` | 29.97 | 38.25 | 6x6 | 0° |
| DOWN | `RP_BTN_DOWN` | 29.97 | 13.50 | 6x6 | 0° |
| LEFT | `RP_BTN_LEFT` | 17.47 | 26.00 | 6x6 | 0° |
| RIGHT | `RP_BTN_RIGHT` | 42.47 | 26.00 | 6x6 | 0° |
| B | `RP_BTN_B` | 90.03 | 24.00 | 12x12 | 90° |
| A | `RP_BTN_A` | 106.03 | 24.00 | 12x12 | 90° |
| SELECT | `RP_BTN_SELECT` | 57.77 | 21.02 | 6x6 | 0° |
| START | `RP_BTN_START` | 70.22 | 21.02 | 6x6 | 0° |
| MENU | `RP_BTN_MENU` | 64.00 | 31.00 | 6x6 | 45° |

Clearance check (`python3 ../layout.py nes`): extent x 14.2..112.0, y 10.5..41.2; closest bodies 4.00 mm; closest pads 2.66 mm; inside wall margin: True; side buttons below latch arms: True.

Console-ID straps: fit R6 (0 Ω), leave the others unfitted. Every board carries all five
strap footprints.

## PCB and schematic

[`kicad/`](kicad) holds a complete KiCad project (`retropad_nes.kicad_pro`): a schematic, a
routed two-layer PCB, a BOM and the DRC report.

| Check | Result |
|---|---|
| DRC | 0 unconnected pads, no clearance/short/edge errors (silkscreen warnings only) |
| Routing | 345 track segments, 21 vias, GND pour on both layers |
| Schematic vs PCB | 35 schematic nets, 35 PCB nets, 0 differences |

| Front | Back (seen from the back) |
|---|---|
| ![front](kicad/front.png) | ![back](kicad/back.png) |

![Schematic](kicad/schematic.png)

The MCU section, support parts, J1 header and the
[pre-order checklist](../README.md#before-ordering-any-board) are shared with every board.
