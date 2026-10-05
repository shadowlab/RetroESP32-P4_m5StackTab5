# SNES board — KiCad PCB

`retropad_snes.kicad_pcb` is a two-layer, fully routed board for the SNES RetroPad (console ID
3). It was made with KiCad 7 and opens in KiCad 7, 8 and 9.

![Front](front.png)
![Back (seen from the back)](back.png)

| File | What it is |
|---|---|
| `retropad_snes.kicad_pcb` / `.kicad_pro` | The routed board |
| `retropad_snes_bom.csv` | Bill of materials. R8–R10 are listed as DNP (not fitted). |
| `retropad_snes_drc.rpt` | KiCad DRC report for this board |
| `gen_snes_pcb.py` | Script that builds the whole board from `../../layout.py` |

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
* **Netlist check:** every switch is on the right row and column, diode polarity matches
  KiCad's convention (pad 1 = cathode), all MCU pins match the firmware's pin map, and J1
  carries M5Stack's P1 pinout.
* **Gerber and drill export:** both succeed.

There is **no schematic**. The board is generated with its nets defined in
`gen_snes_pcb.py`. KiCad's "update PCB from schematic" would therefore delete the nets, so
don't run it on this board.

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
```

Freerouting 1.9 is used single-threaded on purpose. In testing, 2.1's multi-threaded
optimiser reported "0 unrouted" but wrote a session with nets missing.

To get fab files:

```
kicad-cli pcb export gerbers -o fab/ retropad_snes.kicad_pcb
kicad-cli pcb export drill -o fab/ retropad_snes.kicad_pcb
```
