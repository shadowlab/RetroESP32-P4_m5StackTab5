# SNES board — KiCad PCB

A complete KiCad project for the SNES RetroPad (console ID 3): a schematic and a two-layer,
fully routed board, linked to each other. It was made with KiCad 7 and opens in KiCad 7, 8 and
9.

![Schematic](schematic.png)

![Front](front.png)
![Back (seen from the back)](back.png)

| File | What it is |
|---|---|
| `retropad_snes.kicad_pro` | The KiCad project; open this |
| `retropad_snes.kicad_sch` / `retropad_snes_schematic.pdf` | Schematic (one A3 sheet) and a PDF of it |
| `retropad_snes.kicad_pcb` | The routed board |
| `retropad_snes_bom.csv` | Bill of materials. R8, R9 and R11 are listed as DNP (not fitted). |
| `retropad_snes_drc.rpt` | KiCad DRC report for this board |
| [`../../kicad_gen/`](../../kicad_gen) | Shared generators: `gen_pcb.py`, `gen_sch.py`, `check_netlist.py` and `gen_avrdd_pinmap.py` for every console board |

## What's on it

* **Front:** 13 switches.
  * D-pad, SELECT, START and MENU: 6×6 tact switches.
  * A, B, X, Y: 12×12 tact switches at 45°.
  * L and R: right-angle 6×6 switches (PTS645 footprint) whose actuators come out of the side
    edges.
  * The 2×5 right-angle header J1 to the Tab5. Its pins point up out of the top edge, centred
    26 mm from the left edge.
* **Back:**
  * AVR32DD28-I/SO (SOIC-28) with 100 nF on each VDD pin and on VDDIO2, 4.7 µF bulk, 4.7 k
    I2C pull-ups and a 10 k RESET pull-up.
  * Console-ID resistors R6–R9: 0 Ω on ID0 and ID1 for SNES, R8 and R9 not fitted.
  * UPDI header J2: 3V3, UPDI, GND.
  * R11 (not fitted): UPDI to J1 pin 10 (G9), for experiments with programming from the Tab5.
* **Buttons:** each switch connects its own MCU pin to GND, with the MCU's internal pull-up.
  No matrix, no diodes. The slot order (PD1–PD7, PC0–PC3, PF0–PF1) follows the layout's
  button order and matches `firmware_avrdd/pinmap.h`; the table is in
  [`../README.md`](../README.md#pcb-and-schematic). J1 pin 9 (INT) is not connected; the host
  polls.
* **Outline:**
  * 125 × 55 mm, inset 1.5 mm from the case on each side.
  * Notched 3 mm in at the top corners above y = 46 to clear the latch arms.
  * Two M3 holes, 96 mm apart.
* **Stack-up and rules:**
  * 2 layers, 1.6 mm.
  * 0.25 mm tracks, 0.2 mm clearance, 0.6/0.3 mm vias, 0.3 mm copper-to-edge.
  * GND pour on both layers.

## Checks run

* **DRC:** 0 unconnected pads and no clearance, short or edge errors. The report still lists
  silkscreen warnings (labels overlapping pads or running past the edge where L/R overhang),
  plus "library not configured" notes that only appear on a machine without KiCad's library
  table. All are cosmetic.
* **Schematic vs board:** `check_netlist.py` reports the same 26 nets on each side with 0
  differences.
* **Wiring vs firmware:** every switch lands on the MCU pin the firmware's pin map gives it
  for console ID 3, the straps encode ID 3, and J1 carries M5Stack's P1 pinout.
* **Gerber and drill export:** both succeed.

## Schematic

The schematic covers the same circuit on one A3 sheet:
* **MCU:** the AVR32DD28. KiCad 7 has no AVR DD symbol, so the schematic embeds one derived
  from the pin-compatible AVR32DB28, with pins 13/14/15/19 renamed (PD7, VDD, GND, UPDI/PF7).
* **Tab5 header and UPDI header.**
* **Support parts:** decoupling, I2C pull-ups and the RESET pull-up.
* **Console-ID straps:** R8 and R9 are marked DNP, as is R11.
* **Mounting holes.**
* **Buttons:** one line per button, K_<button> → switch → GND.

Nets are named with global labels, so the net names match the board exactly (K_UP and so on,
SCL, SDA, ID0–ID3, AN0/AN1, RESET, UPDI, +3V3, GND).

How it stays in step with the board:
* `gen_sch.py` doesn't keep its own list of connections. It builds the board in memory
  and gives each symbol pin the net of the footprint pad with the same number.
* Each footprint carries its symbol's UUID, so in KiCad **Tools → Update PCB from
  Schematic** matches every part and reports no changes.
* `check_netlist.py snes` exports the schematic netlist with `kicad-cli` and compares it with the
  board pad by pad. Current result: 26 nets on each side, 0 differences.

You can now edit the design in KiCad the normal way, schematic first. Just remember that
re-running the generator scripts overwrites the `.kicad_sch` and `.kicad_pcb` files. Once you
start editing by hand, treat the scripts as the starting point and stop re-running them.

KiCad 7's command line has no ERC, so ERC hasn't been run. When you open the schematic, run
**Inspect → Electrical Rules Checker** once. Expect only notes about the power pins being
driven from the connector (J1 has no power-flag symbol).

## ⚠ Before ordering

These can't be settled from M5Stack's drawings, and getting the first one wrong could put
the Tab5's 5 V rail (pin 6) onto this board's 3.3 V supply:

1. **J1 orientation.** The two header rows end up at different heights once the pins are bent
   up, and either end of the 5-pin row could be pin 1. On a real keyboard, use a multimeter to
   find **pin 5 (3V3)**: it connects to the keyboard MCU's VDD pins and the 100 nF capacitors. Also
   find **pins 2/3/4 (GND)**. Note which physical pin positions they are, seen from the front,
   and compare with J1's pin 1 marker here. In the board as drawn, pin 1 is the lower-left pin
   (front view) and the odd row is the one farther from the edge. If yours differs, flip
   `HEADER_PINS` or the header rotation in `../../kicad_gen/gen_pcb.py` (shared by every board) and regenerate.
2. **J1 height and side.** The header sits on the front side, with the pin rows about 1.5 mm
   and 4 mm above the board. Check that this matches where the Tab5's socket is once the shell
   is designed.
3. **M3 hole height** (`M3_Y`, now 45 mm, an estimate) and **latch-arm length** (the board
   notch starts at y = 46).
4. **L/R switch part.** The footprint is C&K PTS645Vx31 (right-angle). Check the actuator
   length against your shell wall, and design the side openings to match.

## Regenerating

See [`../../README.md`](../../README.md#regenerating). For this board:

```
cd ../../kicad_gen
FREEROUTING_JAR=/path/to/freerouting-1.9.0.jar python3 gen_pcb.py snes --route
python3 gen_sch.py snes
python3 check_netlist.py snes          # schematic vs board: must report 0 differences
```
