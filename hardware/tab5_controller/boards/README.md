# RetroPad console boards

Each board is a console-specific controller PCB that fits where the M5Stack Tab5 Keyboard
goes. Every board:
* uses the same STM32F030, 2×5 Tab5 header and register protocol (see
  [`../README.md`](../README.md));
* identifies itself through its console-ID straps, so the firmware maps its buttons for the
  emulator that is running.

| Board | Console ID | Buttons | Layout | KiCad project |
|---|---|---|---|---|
| [SNES](snes/README.md) | 3 | D-pad, A B X Y, L R (side edges), SELECT START, MENU | [svg](snes/layout.svg) | [`snes/kicad`](snes/kicad) |
| [NES](nes/README.md) | 1 | D-pad, B A, SELECT START, MENU | [svg](nes/layout.svg) | [`nes/kicad`](nes/kicad) |
| [Game Boy](gb/README.md) | 2 | D-pad, B A (diagonal), SELECT START, MENU | [svg](gb/layout.svg) | [`gb/kicad`](gb/kicad) |
| [Genesis / Mega Drive 6-button](genesis/README.md) | 5 | D-pad, X Y Z over A B C, MODE START, MENU | [svg](genesis/layout.svg) | [`genesis/kicad`](genesis/kicad) |
| [Master System / Game Gear](sms/README.md) | 4 | D-pad, 1 2, START, MENU | [svg](sms/layout.svg) | [`sms/kicad`](sms/kicad) |

Every KiCad project:
* is a schematic plus a routed, two-layer, 125 × 55 mm PCB;
* passes DRC with 0 unconnected pads and no clearance, short or edge errors (only silkscreen
  warnings remain);
* has a schematic that matches its PCB net for net (`check_netlist.py`).

The SNES layout follows the user's reference drawing. The NES, Game Boy, Genesis (6-button)
and SMS/GG layouts follow each console's own pad, scaled the same way. X/Y/Z/MODE on the
Genesis board need 6-button support in the Genesis core before games can use them (see
[`genesis/README.md`](genesis/README.md)).

## How the boards are made

| File | Role |
|---|---|
| [`layout.py`](layout.py) | Button positions for every console, the console IDs, and clearance checks (switch bodies, pads, footprint courtyards, M3 holes, wall margin, latch arms). It also draws `<console>/layout.svg`. |
| [`kicad_gen/gen_pcb.py`](kicad_gen/gen_pcb.py) | Builds the PCB: switches and the 2×5 header on the front; MCU, support parts, one diode per switch, console-ID straps and SWD pads on the back. It routes with Freerouting, pours GND, runs DRC and writes a BOM. |
| [`kicad_gen/gen_sch.py`](kicad_gen/gen_sch.py) | Builds the schematic from the same board data, so it can't drift from the PCB. |
| [`kicad_gen/check_netlist.py`](kicad_gen/check_netlist.py) | Compares the schematic netlist with the PCB pad by pad |
| [`kicad_gen/render.py`](kicad_gen/render.py) | Writes the front/back/schematic PNGs and the schematic PDF |

Each button is wired to its RetroPad bit's position in the button matrix (row = bit % 8,
column = bit / 8; see `../README.md` §2.3). The board firmware is therefore the same for
every console.

### Regenerating

```
sudo apt install kicad xvfb default-jre            # KiCad 7+ (with its Python module), Java, virtual display
pip install cairosvg pillow                        # only for render.py
curl -LO https://github.com/freerouting/freerouting/releases/download/v1.9.0/freerouting-1.9.0.jar

cd kicad_gen
export FREEROUTING_JAR=$PWD/../freerouting-1.9.0.jar
for c in snes nes gb genesis sms; do
  python3 gen_pcb.py $c --route      # FR_PASSES=300 for more router passes
  python3 gen_sch.py $c
  python3 check_netlist.py $c        # must report 0 differences
  python3 render.py $c
done
```

Freerouting 1.9 runs single-threaded on purpose. In testing, 2.1's multi-threaded optimiser
reported "0 unrouted" but wrote a session with nets missing.

Fab files: `kicad-cli pcb export gerbers -o fab/ <board>.kicad_pcb` and
`kicad-cli pcb export drill -o fab/ <board>.kicad_pcb`.

## Before ordering any board

These apply to every board, because they all share the header, outline and mounting:

1. **J1 orientation.** Getting this wrong could put the Tab5's 5 V (pin 6) onto the board's
   3.3 V rail.
   * On a real keyboard, use a multimeter to find pin 5 (3V3, which connects to the STM32's VDD
     pins) and pins 2/3/4 (GND).
   * Compare them with J1's pin 1 marker. As drawn, pin 1 is the lower-left pin in the front
     view, and the odd row is the one farther from the edge.
   * If yours differs, change `HEADER_PINS` or the header rotation in `kicad_gen/gen_pcb.py`
     and regenerate every board.
2. **J1 height and side.** The header is on the front, with the pin rows about 1.5 mm and
   4 mm above the board. Check this against the Tab5 socket once the shell is designed.
3. **Mechanical estimates.**
   * M3 hole height: `M3_Y` = 45 mm.
   * Latch-arm notch: starts at y = 46.
   * The SNES board's right-angle L/R switch part.
4. **ERC.** KiCad 7's command line can't run ERC. Open each schematic once and run
   **Inspect → Electrical Rules Checker**.
