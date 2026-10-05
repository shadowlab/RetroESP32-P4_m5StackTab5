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
| `retropad_snes_bom.csv` | Bill of materials. R8–R10 are listed as DNP (not fitted). |
| `retropad_snes_drc.rpt` | KiCad DRC report for this board |
| `gen_snes_pcb.py` | Builds the board from `../../layout.py` |
| `gen_snes_sch.py` | Builds the schematic from the same board data |
| `check_netlist.py` | Confirms the schematic and the board have identical connectivity |

## What's on it

* **Front:** 13 switches.
  * D-pad, SELECT, START and MENU: 6×6 tact switches.
  * A, B, X, Y: 12×12 tact switches at 45°.
  * L and R: right-angle 6×6 switches (PTS645 footprint) whose actuators come out of the side
    edges.
  * The 2×5 right-angle header J1 to the Tab5. Its pins point up out of the top edge, centred
    26 mm from the left edge.
* **Back:**
  * STM32F030C8T6 with the support circuit copied from M5Stack's keyboard: 4.7 k I2C pull-ups,
    10 k INT, NRST and BOOT0 resistors, 100 nF decoupling plus 4.7 µF bulk.
  * One 1N4148W per switch (anode to the row, cathode to the switch).
  * Console-ID resistors R6–R10: 0 Ω on ID0 and ID1 for SNES, R8–R10 not fitted.
  * SWD pads J2, ordered 3V3, SWCLK, SWDIO, NRST, GND as on the keyboard.
* **Matrix:** each switch is wired to its RetroPad bit's position (main README §2.3), so the
  stock RetroPad firmware needs no changes:
  * UP, DOWN, LEFT, RIGHT, A, B, X: rows 0–7 on COL0
  * Y, L, R, START, SELECT: COL1
  * MENU: COL2
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
* **Schematic vs board:** `check_netlist.py` reports the same 39 nets on each side with 0
  differences.
* **Netlist check:** every switch is on the right row and column, diode polarity matches
  KiCad's convention (pad 1 = cathode), all MCU pins match the firmware's pin map, and J1
  carries M5Stack's P1 pinout.
* **Gerber and drill export:** both succeed.

## Schematic

The schematic covers the same circuit on one A3 sheet:
* **MCU:** the STM32F030C8T6.
* **Tab5 header and SWD:** P1/P2 order as on the keyboard.
* **Support parts:** decoupling, pull-ups, reset and BOOT0.
* **Console-ID straps:** R8–R10 are marked DNP.
* **Mounting holes.**
* **Button matrix:** one line per button, ROWn → diode → switch → COLn.

Nets are named with global labels, so the net names match the board exactly (ROW0–7,
COL0–3, K_UP and so on, SCL, SDA, INT, ID0–ID3, +3V3, GND).

How it stays in step with the board:
* `gen_snes_sch.py` doesn't keep its own list of connections. It builds the board in memory
  and gives each symbol pin the net of the footprint pad with the same number.
* Each footprint carries its symbol's UUID, so in KiCad **Tools → Update PCB from
  Schematic** matches every part and reports no changes.
* `check_netlist.py` exports the schematic netlist with `kicad-cli` and compares it with the
  board pad by pad. Current result: 39 nets on each side, 0 differences.

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
   find **pin 5 (3V3)**: it connects to the STM32's VDD pins and the 100 nF capacitors. Also
   find **pins 2/3/4 (GND)**. Note which physical pin positions they are, seen from the front,
   and compare with J1's pin 1 marker here. In the board as drawn, pin 1 is the lower-left pin
   (front view) and the odd row is the one farther from the edge. If yours differs, flip
   `HEADER_PINS` or the header rotation in `gen_snes_pcb.py` and regenerate.
2. **J1 height and side.** The header sits on the front side, with the pin rows about 1.5 mm
   and 4 mm above the board. Check that this matches where the Tab5's socket is once the shell
   is designed.
3. **M3 hole height** (`M3_Y`, now 45 mm, an estimate) and **latch-arm length** (the board
   notch starts at y = 46).
4. **L/R switch part.** The footprint is C&K PTS645Vx31 (right-angle). Check the actuator
   length against your shell wall, and design the side openings to match.

## Regenerating

```
sudo apt install kicad xvfb openjdk-21-jre        # KiCad 7+, Java, virtual display
curl -LO https://github.com/freerouting/freerouting/releases/download/v1.9.0/freerouting-1.9.0.jar
FREEROUTING_JAR=$PWD/freerouting-1.9.0.jar python3 gen_snes_pcb.py --route
python3 gen_snes_sch.py
python3 check_netlist.py                          # schematic vs board: must report 0 differences
```

Freerouting 1.9 is used single-threaded on purpose. In testing, 2.1's multi-threaded
optimiser reported "0 unrouted" but wrote a session with nets missing.

To get fab files:

```
kicad-cli pcb export gerbers -o fab/ retropad_snes.kicad_pcb
kicad-cli pcb export drill -o fab/ retropad_snes.kicad_pcb
```
