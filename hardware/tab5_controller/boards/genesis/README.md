# Genesis / Mega Drive 6-button board (console ID 5)

![Genesis / Mega Drive 6-button layout inside the Tab5 Keyboard envelope](layout.svg)

The coordinate conventions, case outline and markings are the same as on the
[SNES board](../snes/README.md). The d-pad uses the SNES board's positions so all boards feel
the same.

## Layout

Based on the 6-button pad (MK-1653):

* **Face buttons:** X Y Z in a row above A B C. Each row has three 12×12 switches, 14 mm apart,
  with every column 1.5 mm higher than the one to its left, as on the pad.
  * The rows are 16.5 mm apart. The switches are turned 90° so neighbours in a row don't
    share pad space, and that makes each switch's courtyard (legs included) 16.02 mm tall.
  * The cluster sits low enough for Z's courtyard to clear the right M3 hole (0.54 mm).
* **MODE:** to the left of START, where SELECT sits on the SNES board. It is wired to the
  SELECT bit, as the console catalog specifies for Genesis MODE.
* **START:** in the SNES board's START position.
* **MENU:** centred above MODE/START.

> **Emulator support:** 6-button support for the Genesis core (gwenesis) is in
> https://github.com/shadowlab/RetroESP32-P4_m5StackTab5/pull/4. With that change, port 1 acts
> as a 6-button pad while this board is attached, and X/Y/Z/MODE reach games. Holding MODE
> when the board connects keeps it in 3-button mode, as on the real pad. Without #4, the
> board works as a 3-button pad (A/B/C/START) and X/Y/Z/MODE are ignored.

| Button | RetroPad bit | x | y | Switch | Rotation |
|---|---|---|---|---|---|
| UP | `RP_BTN_UP` | 29.97 | 38.25 | 6x6 | 0° |
| DOWN | `RP_BTN_DOWN` | 29.97 | 13.50 | 6x6 | 0° |
| LEFT | `RP_BTN_LEFT` | 17.47 | 26.00 | 6x6 | 0° |
| RIGHT | `RP_BTN_RIGHT` | 42.47 | 26.00 | 6x6 | 0° |
| A | `RP_BTN_A` | 84.03 | 13.50 | 12x12 | 90° |
| X | `RP_BTN_X` | 84.03 | 30.00 | 12x12 | 90° |
| B | `RP_BTN_B` | 98.03 | 15.00 | 12x12 | 90° |
| Y | `RP_BTN_Y` | 98.03 | 31.50 | 12x12 | 90° |
| C | `RP_BTN_C` | 112.03 | 16.50 | 12x12 | 90° |
| Z | `RP_BTN_Z` | 112.03 | 33.00 | 12x12 | 90° |
| MODE | `RP_BTN_SELECT` | 57.77 | 21.02 | 6x6 | 45° |
| START | `RP_BTN_START` | 70.22 | 21.02 | 6x6 | 45° |
| MENU | `RP_BTN_MENU` | 64.00 | 31.00 | 6x6 | 45° |

Clearance check (`python3 ../layout.py genesis`): extent x 14.2..118.0, y 7.2..41.2; closest bodies 2.00 mm; closest pads 1.80 mm; courtyards 0.48 mm; M3 holes 0.54 mm; inside wall margin: True; side buttons below latch arms: True.

Console-ID straps: fit R6, R8 (0 Ω), leave the others unfitted. Every board carries all four
strap footprints (R6–R9).

## PCB and schematic

[`kicad/`](kicad) holds a complete KiCad project (`retropad_genesis.kicad_pro`): a schematic, a
routed two-layer PCB, a BOM and the DRC report.

| Check | Result |
|---|---|
| DRC | 0 unconnected pads, no clearance/short/edge/courtyard errors (silkscreen warnings only) |
| Routing | 274 track segments, 13 vias, GND pour on both layers |
| Schematic vs PCB | 25 schematic nets, 25 PCB nets, 0 differences |
| Wiring vs firmware | every switch is on the MCU pin `firmware_avrdd/pinmap.h` gives it for console ID 5 |

Each switch connects its own AVR32DD28 pin to GND (internal pull-up, no diodes):

| Button | MCU pin | RetroPad bit |
|---|---|---|
| UP | PD1 (pin 7) | `RP_BTN_UP` |
| DOWN | PD2 (pin 8) | `RP_BTN_DOWN` |
| LEFT | PD3 (pin 9) | `RP_BTN_LEFT` |
| RIGHT | PD4 (pin 10) | `RP_BTN_RIGHT` |
| A | PD5 (pin 11) | `RP_BTN_A` |
| X | PD6 (pin 12) | `RP_BTN_X` |
| B | PD7 (pin 13) | `RP_BTN_B` |
| Y | PC0 (pin 2) | `RP_BTN_Y` |
| C | PC1 (pin 3) | `RP_BTN_C` |
| Z | PC2 (pin 4) | `RP_BTN_Z` |
| MODE | PC3 (pin 5) | `RP_BTN_SELECT` |
| START | PF0 (pin 16) | `RP_BTN_START` |
| MENU | PF1 (pin 17) | `RP_BTN_MENU` |

| Front | Back (seen from the back) |
|---|---|
| ![front](kicad/front.png) | ![back](kicad/back.png) |

![Schematic](kicad/schematic.png)

The MCU section (AVR32DD28, decoupling, I2C pull-ups, UPDI header J2), the J1 header and the
[pre-order checklist](../README.md#before-ordering-any-board) are shared with every board; see
[`../../README.md`](../../README.md) §2 for the pin plan.
