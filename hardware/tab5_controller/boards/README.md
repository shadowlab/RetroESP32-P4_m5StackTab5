# RetroPad console boards

Each board is a console-specific controller PCB that fits where the M5Stack Tab5 Keyboard
goes. Every board:
* uses the same AVR32DD28 (SOIC-28), 2×5 Tab5 header and register protocol (see
  [`../README.md`](../README.md));
* wires each button to its own MCU pin (switch to GND, internal pull-up), so there is no key
  matrix and no diodes;
* runs the same firmware, [`../firmware_avrdd`](../firmware_avrdd), programmed over UPDI;
* uses 0805 resistors and capacitors with KiCad's hand-solder pads, so the boards can be
  assembled by hand;
* identifies itself through its console-ID straps, so the firmware maps its buttons for the
  emulator that is running.

| Board | Console ID | Buttons | Layout | KiCad project |
|---|---|---|---|---|
| [SNES](snes/README.md) | 3 | D-pad, A B X Y, L R (side edges), SELECT START, MENU | [svg](snes/layout.svg) | [`snes/kicad`](snes/kicad) |
| [NES / Game Boy](nes_gb/README.md) | 1 | D-pad, B A (A raised 4 mm), SELECT START, MENU | [svg](nes_gb/layout.svg) | [`nes_gb/kicad`](nes_gb/kicad) |
| [Genesis / Mega Drive 6-button](genesis/README.md) | 5 | D-pad, X Y Z over A B C, MODE START, MENU | [svg](genesis/layout.svg) | [`genesis/kicad`](genesis/kicad) |
| [Master System / Game Gear](sms/README.md) | 4 | D-pad, 1 2, START, MENU | [svg](sms/layout.svg) | [`sms/kicad`](sms/kicad) |

Every KiCad project:
* is a schematic plus a routed, two-layer, 125 × 55 mm PCB;
* passes DRC with 0 unconnected pads and no clearance, short or edge errors (only silkscreen
  warnings remain);
* has a schematic that matches its PCB net for net (`check_netlist.py`).

The SNES layout follows the user's reference drawing. The NES / Game Boy, Genesis (6-button)
and SMS/GG layouts follow each console's own pad, scaled the same way. X/Y/Z/MODE on the
Genesis board reach games through the Genesis core's 6-button support (`components/gwenesis`,
merged from https://github.com/shadowlab/RetroESP32-P4_m5StackTab5/pull/4). The NES / Game Boy board puts
A 4 mm above B, between the NES pad's level buttons and the Game Boy's diagonal.

## How the boards are made

| File | Role |
|---|---|
| [`layout.py`](layout.py) | Button positions for every console, the console IDs, and clearance checks (switch bodies, pads, footprint courtyards, M3 holes, wall margin, latch arms). It also draws `<console>/layout.svg`. |
| [`kicad_gen/gen_pcb.py`](kicad_gen/gen_pcb.py) | Builds the PCB: switches and the 2×5 header on the front; MCU, support parts, console-ID straps and the UPDI header on the back. It routes with Freerouting, pours GND, runs DRC and writes a BOM. |
| [`kicad_gen/gen_sch.py`](kicad_gen/gen_sch.py) | Builds the schematic from the same board data, so it can't drift from the PCB. |
| [`kicad_gen/check_netlist.py`](kicad_gen/check_netlist.py) | Compares the schematic netlist with the PCB pad by pad |
| [`kicad_gen/render.py`](kicad_gen/render.py) | Writes the front/back/schematic PNGs and the schematic PDF |
| [`kicad_gen/gen_avrdd_pinmap.py`](kicad_gen/gen_avrdd_pinmap.py) | Writes `../firmware_avrdd/pinmap.h`, the per-console button slot table |
| [`kicad_lib/RetroPad.pretty`](kicad_lib/RetroPad.pretty) | 4-pin tact-switch footprints, written by `gen_pcb.py` (see [Switches](#switches)) |

A board has 13 button slots (PD1–PD7, PC0–PC3, PF0–PF1). A console's buttons fill them in
the order its layout lists them. `gen_pcb.py` wires that order, and `gen_avrdd_pinmap.py`
writes it into the firmware's pin map, keyed by console ID. The firmware is therefore the same
for every console; only the ID straps differ.

### Switches

The 6×6 and 12×12 tact switches are 4-leg parts, so the schematics and footprints use all
four pins, numbered like the parts' own symbols:

```
  1 ───┬─── 2      1–2: tied inside the switch  → GND
       /
  4 ───┴─── 3      4–3: tied inside the switch  → K_<button> (MCU pin)
```

* **Footprints:** `RetroPad:SW_PUSH_6mm_4pin` and `RetroPad:SW_PUSH-12mm_4pin` are KiCad's
  `SW_PUSH_6mm` / `SW_PUSH-12mm` with the pads renumbered 1–4. Pad 1 is top-left, then
  clockwise. The tied pairs are the legs 6.5 mm (12.5 mm) apart, as in KiCad's original
  numbering.
* **Symbol:** `RetroPad:SW_Push_4pin`, embedded in each schematic. KiCad's own 4-pin
  `Switch:SW_Push_Dual` is a two-contact switch (1–2 and 3–4 each switched), so it doesn't
  match these parts.
* **L / R (right angle):** two contacts plus two mounting legs, so they keep KiCad's 2-pin
  `SW_Push` and the stock PTS645Vx31 footprint.
* **Other EDA tools:** a vendor's own symbol and footprint can number the legs differently, and
  some parts tie 1–3 / 2–4 instead. Before wiring a part, check its datasheet's
  internal-connection drawing. GND and the MCU pin must go to opposite sides of the contact.
  If they share a tied pair, the button always reads as pressed.

Each board's `kicad/fp-lib-table` points KiCad at `kicad_lib/RetroPad.pretty`.

### Regenerating

```
sudo apt install kicad xvfb default-jre            # KiCad 7+ (with its Python module), Java, virtual display
pip install cairosvg pillow                        # only for render.py
curl -LO https://github.com/freerouting/freerouting/releases/download/v1.9.0/freerouting-1.9.0.jar

cd kicad_gen
export FREEROUTING_JAR=$PWD/../freerouting-1.9.0.jar
for c in snes nes_gb genesis sms; do
  python3 gen_pcb.py $c --route      # FR_PASSES=300 for more router passes
  python3 gen_sch.py $c
  python3 check_netlist.py $c        # must report 0 differences
  python3 render.py $c
done
python3 gen_avrdd_pinmap.py          # after changing a layout's buttons
```

Freerouting 1.9 runs single-threaded on purpose. In testing, 2.1's multi-threaded optimiser
reported "0 unrouted" but wrote a session with nets missing.

Check the DRC report for 0 unconnected pads after every route: if 1.9 stalls with a net
unrouted, re-run with `FR_PASSES=300`.

BOMs list the orderable switches and Tab5 header (Manufacturer / MPN / DigiKey columns, from `PARTS` in
`kicad_gen/gen_pcb.py`):

| Footprint | Part | DigiKey |
|---|---|---|
| `SW_PUSH_6mm_4pin` | C&K PTS645SM43-2 LFS (6×6 mm, 4.3 mm, ~160 gf) | search by MPN |
| `SW_PUSH-12mm_4pin` | Omron B3F-4055 (12×12 mm, 7.3 mm, 260 gf, takes B32 caps) | SW414-ND |
| `SW_Tactile_SPST_Angled_PTS645Vx31-2LFS` | C&K PTS645VL31-2 LFS (right angle) | CKN9094-ND |
| `PinHeader_2x05_P2.54mm_Horizontal` (J1, to the Tab5) | Samtec TSW-105-08-G-D-RA (2×5 right angle, 5.84 mm mating pins); check the pin length against M5Stack's keyboard first | SAM1037-05-ND |

After changing `PARTS`, run `python3 gen_pcb.py <console> --bom-only` to rewrite a BOM from
the routed board without re-routing.

Fab files: `kicad-cli pcb export gerbers -o fab/ <board>.kicad_pcb` and
`kicad-cli pcb export drill -o fab/ <board>.kicad_pcb`.

## Before ordering any board

These apply to every board, because they all share the header, outline and mounting:

1. **J1 orientation.** Getting this wrong could put the Tab5's 5 V (pin 6) onto the board's
   3.3 V rail.
   * On a real keyboard, use a multimeter to find pin 5 (3V3, which connects to the keyboard
     MCU's VDD pins) and pins 2/3/4 (GND).
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
