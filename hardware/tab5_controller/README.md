# Tab5 console controller boards (RetroPad)

Controller boards for each console. Every board fits where the
[M5Stack Tab5 Keyboard](https://docs.m5stack.com/en/tab5/Tab5_Keyboard) goes and uses the same
connector and I/O. Each board tells the firmware which console it is, so when you start a
different emulator you can swap to that console's board, and its buttons land where that
core expects them. You can also swap boards while the system is running.

```
 ┌──────────────── Tab5 ────────────────┐
 │                                      │
 └───────────┬─ Ext.Port1 (2x5) ─┬──────┘
             │ G0 SDA  G1 SCL    │   I2C 400 kHz, addr 0x6D
 ┌───────────┴───────────────────┴──────┐
 │  AVR32DD28 (SOIC-28) ─ 1 pin/button  │   up to 13 buttons, no matrix
 │  ID straps ─ console id              │   NES / SNES / Genesis / ...
 │  PA4/PA5   ─ paddles / analog stick  │   optional
 └──────────────────────────────────────┘
```

The design has three parts. The boards are mechanically and electrically compatible with
M5Stack's keyboard port, and they answer on its I2C address:

| Part | Where | What it does |
|---|---|---|
| Board hardware | this document | Uses the keyboard's outline, latches and 2x5 header, with console-specific buttons on an AVR32DD28 |
| Board firmware | [`firmware_avrdd/`](firmware_avrdd/README.md) | One firmware image for every board. It answers at 0x6D with M5Stack's register map plus the RetroPad block. |
| Host driver | [`components/tab5_ctrl`](../../components/tab5_ctrl) | Detects the board, polls it and feeds `odroid_input_gamepad_read()` |

The stock M5Stack Tab5 Keyboard also works with the host driver. It acts as a generic pad
(see [Stock keyboard](#stock-m5stack-keyboard)).

---

## 1. Mechanical

Every console board uses the Tab5 Keyboard's (SKU A164) enclosure envelope. These figures come
from M5Stack's dimension sheet; all units are mm, and "front" is the key side.

**Dimensioned on the sheet**

| Item | Value |
|---|---|
| Overall width | 128.0 |
| Width between the two top latch arms | 123.6 |
| Overall height, including the latch arms | 59.4 |
| Height to the top edge of the body (excluding the latch arms) | 57.95 |
| Height of the key area (bottom edge to the top step) | 52.0 |
| Thickness, including the keycaps | 13.19 |
| Body thickness (excluding the keycaps) | 11.99 |
| M3 screw holes on the back | 2, **96.0 apart** |

**Taken from the drawing, not dimensioned**

These were scaled off the drawing, so they are good to about ±0.5 mm. Confirm them with
calipers before fabrication.

| Item | Approximate value |
|---|---|
| M3 hole X | 16.0 in from each side edge (the holes are drawn symmetric: (128 − 96) / 2) |
| M3 hole Y | about 14 down from the top of the latch arms |
| 2×5 header | Along the top edge with the pins pointing up (+Y) into the Tab5. Seen from the front it spans about 22–32 from the left edge (about 27 to its centre). |
| Top strip | The 5.95-tall band above the key area (57.95 − 52) holds the header and docks into the Tab5 |
| Case screws | 4 small screws, one near each corner of the back shell (not the PCB mounting) |
| Status LEDs | 2 small windows on the front, bottom-left |

**M5Stack's STL doesn't match the drawing.** M5Stack's own downloadable STL
(`Tab5_Keyboard.stl`, assembly `TAB5_ASM_V2`) describes a different case from the dimension
sheet and the schematic. Both files come from the product page:

| | Dimension sheet + schematic | STL `TAB5_ASM_V2` |
|---|---|---|
| Keys | 5 × 14 = 70 | 4 × 14 = 56 windows, 8.5 × 9.0 pitch |
| Height | 59.4 overall, 52 key area | 45.0 shell, 50.8 with latch arms |
| Width / thickness | 128.0 / 11.99 (13.19 with keycaps) | 128.0 / 12.0 (13.3 with keycaps) |
| Fixing | 2 × M3, 96 apart | Corner screws 120 apart (about 1.6 mm pilot holes) |
| Latch arms | About 2.2 wide (123.6 between them) | 3.0 wide × 16.5 tall, standing about 5.8 above the shell |
| Connector opening | Centred about 27 from the left (scaled off the drawing) | 13.2 wide, **centred 26.0 from the left** (front view) |

These boards follow the **dimension sheet**, because it agrees with the 70-key schematic and
with the product's published size (128 × 59.4 × 13.1). Both sources put the connector at about
26–27 mm, so use **26.0** from the STL. Before the first fabrication run, take these from a
real keyboard with calipers:

1. The latch-arm width and how far the arms stand above the body.
2. The M3 hole height.
3. The header's distance below the top edge.

Layout rules for every console board:

* Draw the PCB outline inside the shell. The PCB is **not** 128 × 59.4, because the shell
  walls take part of it. Allow at least a 1.5 mm wall all round until a shell is drawn.
* Keep everything that touches the Tab5 identical on every board: the header position and
  orientation, the top strip, the latch arms and the M3 holes. Only the button cut-outs and
  the silkscreen change.
* Keep total height ≤ 13.19 mm. A PCB body of ≤ 11.99 mm plus buttons that stand no taller
  than the keyboard's keycaps keeps the Tab5 sitting flat.

## 2. Electrical

### 2.1 Connector (Tab5 Ext.Port1)

This is P1 in M5Stack's keyboard schematic (`SCH_Tab5_Keyboard_SCH_V1.0`): a 2×5 header with
2.54 mm pitch. Copy it pin for pin.

| Pin | Net | Board side | | Pin | Net | Board side |
|---|---|---|---|---|---|---|
| 10 | G9 | **not connected** (optional 0 Ω to UPDI, unfitted) | | 9 | INT_G50 | **not connected** |
| 8 | SDA_G0 | PA2 (TWI0 SDA), 4.7 kΩ pull-up | | 7 | SCL_G1 | PA3 (TWI0 SCL), 4.7 kΩ pull-up |
| 6 | SYS_EXT5V | **not connected** | | 5 | VCC_3V3 | Board supply |
| 4 | GND | GND | | 3 | GND | GND |
| 2 | GND | GND | | 1 | SYS_VIN | **not connected** |

* The whole board runs from the Tab5's 3.3 V on pin 5. Leave SYS_VIN and SYS_EXT5V open, as
  M5Stack does.
* G9 (pin 10) is unused on the keyboard. It is the only spare line to the Tab5. The boards
  carry an unfitted 0 Ω from UPDI to it, as an experiment towards letting the Tab5 reprogram
  a board; G9's suitability isn't verified.
* INT is not wired. The host driver polls the board, and the AVR32DD28 has no pin left for it.

### 2.2 MCU and reference circuit

Every board uses a **Microchip AVR32DD28-I/SO** (SOIC-28, 24 MHz, 32 KB flash), programmed over
UPDI. Its SOIC-28 pinout was cross-checked between DxCore's diagram (generated from
Microchip's AVR64DD28 device pack) and KiCad's pin-compatible AVR32DB28 symbol.

| Pin | Port | Use | | Pin | Port | Use |
|---|---|---|---|---|---|---|
| 1 | PA7 | ID3 strap | | 15 | GND | |
| 2–5 | PC0–PC3 | Button slots S7–S10 | | 16–17 | PF0–PF1 | Button slots S11–S12 |
| 6 | VDDIO2 | 3V3 (supplies PORTC) | | 18 | PF6 | RESET (10 kΩ pull-up) |
| 7–13 | PD1–PD7 | Button slots S0–S6 | | 19 | PF7 | UPDI |
| 14 | VDD | 3V3 | | 20 | VDD | 3V3 |
| | | | | 21 | GND | |
| | | | | 22, 23 | PA0, PA1 | ID0, ID1 straps |
| | | | | 24, 25 | PA2, PA3 | SDA, SCL (TWI0) |
| | | | | 26, 27 | PA4, PA5 | AN0, AN1 (AIN24, AIN25) |
| | | | | 28 | PA6 | ID2 strap |

Support circuit:

| Part | Value / connection |
|---|---|
| Decoupling | 100 nF on each VDD pin (14, 20) and on VDDIO2 (6), plus 4.7 µF bulk |
| I2C pull-ups | 4.7 kΩ from PA2 and PA3 to 3V3 |
| RESET | 10 kΩ pull-up on PF6 |
| UPDI header | 3 pins: 1 = 3V3, 2 = UPDI (PF7), 3 = GND |

### 2.3 Buttons: one pin each

Each button connects its MCU pin to GND, and the pin has the MCU's internal pull-up. No
matrix, no diodes, so any combination of buttons reads correctly.

The 13 button slots (S0–S12 = PD1–PD7, PC0–PC3, PF0–PF1) are filled in the order the
console's layout lists its buttons. The firmware holds that slot table for every console ID,
generated from the board layouts, and reports each button under its canonical RetroPad bit:

| Bits | Buttons |
|---|---|
| 0–3 | `UP` `DOWN` `LEFT` `RIGHT` |
| 4–9 | `A` `B` `C` `X` `Y` `Z` |
| 10–13 | `L` `R` `L2` `R2` |
| 14–17 | `START` `SELECT` `MENU` `VOLUME` |
| 18–19 | `OPT1` `OPT2` |
| 20–31 | Keypad `KP1`–`KP9`, `KP*`, `KP0`, `KP#` |

### 2.4 Console ID straps

Each strap pin uses the MCU's internal pull-up, read once at boot. Fit a 0 Ω resistor to GND
to set that bit. With no straps fitted the ID is 0, which means Generic.

| ID | Console | ID3 PA7 | ID2 PA6 | ID1 PA1 | ID0 PA0 |
|---|---|---|---|---|---|
| 0 | Generic | – | – | – | – |
| 1 | NES | – | – | – | ● |
| 2 | Game Boy / Color | – | – | ● | – |
| 3 | SNES | – | – | ● | ● |
| 4 | Master System / Game Gear | – | ● | – | – |
| 5 | Genesis / Mega Drive | – | ● | – | ● |
| 6 | PC Engine | – | ● | ● | – |
| 7 | Atari 2600 | – | ● | ● | ● |
| 8 | Atari 7800 | ● | – | – | – |
| 9 | Atari Lynx | ● | – | – | ● |
| 10 | Atari 800XL / 5200 | ● | – | ● | – |
| 11 | ColecoVision | ● | – | ● | ● |
| 12 | Neo Geo | ● | ● | – | – |
| 13 | ZX Spectrum | ● | ● | – | ● |
| 14 | reserved | ● | ● | ● | – |

The ID also selects the button slot table, so **one firmware image serves every board**.

### 2.5 Analog (optional)

The firmware reads AN0 (PA4) and AN1 (PA5) as 8-bit values on the Atari 2600 and 5200 IDs.
Wire a 10 kΩ–100 kΩ potentiometer between 3V3 and GND for each channel, with the wiper on the
pin. AN0 drives the existing Atari paddle input (`odroid_paddle_adc_raw`).

## 3. Console catalog

Each row gives the buttons to populate and the bit to wire each one to. MENU (bit 16) and
VOLUME (bit 17) are recommended on every board. The current touch shoulder zones are
calibrated for the Guition panel, so on a Tab5 these buttons are the dependable way into the
in-game menu.

| ID | Console | Buttons (label → bit) | Notes |
|---|---|---|---|
| 1 | NES | D-pad, B→`B`, A→`A`, SELECT, START | |
| 2 | Game Boy | Same as NES | |
| 3 | SNES | D-pad, A, B, X, Y, L, R, SELECT, START | |
| 4 | SMS / GG | D-pad, 1→`B`, 2→`A`, START (Pause) | |
| 5 | Genesis | D-pad, A→`A`, B→`B`, C→`C`, START, MODE→`SELECT` | The host remaps these for the 3-button core |
| 6 | PC Engine | D-pad, II→`B`, I→`A`, SELECT, RUN→`START` | |
| 7 | Atari 2600 | Joystick→D-pad, FIRE→`A`, GAME SELECT→`SELECT`, GAME RESET→`START`, paddle→AN0 | |
| 8 | Atari 7800 | D-pad, left fire→`B`, right fire→`A`, PAUSE→`START`, SELECT, RESET→`OPT1` | |
| 9 | Atari Lynx | D-pad, A, B, OPTION 1→`OPT1`, OPTION 2→`OPT2`, PAUSE→`START` | |
| 10 | Atari 800 / 5200 | D-pad (or analog stick on AN0/AN1), fires→`A`/`B`, START, PAUSE→`SELECT`, RESET→`OPT1`, keypad `KP0`–`KP#` | |
| 11 | ColecoVision | D-pad, left fire→`B`, right fire→`A`, keypad `KP0`–`KP#` | |
| 12 | Neo Geo | D-pad, A, B, C→`C`, D→`X`, START, SELECT (coin) | The host remaps C/D for gngeo |
| 13 | ZX Spectrum | Kempston D-pad, FIRE→`A` | The stock keyboard is the better fit for typing |

### How bits reach each emulator

`components/odroid/odroid_input.c` (`tab5_pad_read`) ORs the board's buttons into the same
`odroid_gamepad_state` that the USB and GPIO pads fill, so every emulator gets them with no
emulator-side changes. The map is chosen by the console ID:

* **Default** (all IDs except the two below): `A`→A, `B`→B, `X`→X, `Y`→Y, `L`/`L2`→L,
  `R`/`R2`→R, `START`, `SELECT`, `MENU`, `VOLUME`, `OPT1`→SELECT, `OPT2`→START.
* **Genesis:** `A`→ODROID X (pad A), `B`→ODROID A (pad B), `C`→ODROID B (pad C).
* **Neo Geo:** `C`→ODROID X (C), `X`→ODROID Y (D).

The keypad bits and `C`/`Z` have no ODROID equivalent. Cores read them directly:

```c
#include "tab5_ctrl.h"
uint32_t b = tab5_ctrl_get_buttons();
if (b & RP_BIT(RP_BTN_KP5)) { /* keypad 5 */ }
```

## 4. Register protocol

The canonical definition is
[`components/tab5_ctrl/include/retropad_proto.h`](../../components/tab5_ctrl/include/retropad_proto.h),
which the board firmware includes directly.

The boards answer at M5Stack's address (0x6D) with the stock registers the host driver uses:
the configuration registers, FW_VERSION (0xFE), and an always-empty key-event queue. They
add:

| Reg | Len | Content |
|---|---|---|
| 0x70 | 4 | Signature `'R' 'P' 'A' 'D'` |
| 0x74 | 1 | Protocol version (1) |
| 0x75 | 1 | Console ID (straps) |
| 0x76 | 1 | Analog channel count (0 or 2) |
| 0x77 | 1 | Flags (0) |
| 0x80 | 4 | Live debounced button mask, little-endian |
| 0x84 | 4 | AN0, AN1 (8-bit), 2 reserved |

The block reads as one burst, so reading 8 bytes from 0x80 returns buttons and analog in a
single transfer. The board snapshots the block at the start of each read, so the 4-byte
button mask is always coherent.

## 5. Host side

Enable the driver in `menuconfig` under **Tab5 controller boards (RetroPad)**, or add this to
the sdkconfig defaults:

```
CONFIG_TAB5_CTRL_ENABLE=y
CONFIG_TAB5_CTRL_I2C_SDA=0
CONFIG_TAB5_CTRL_I2C_SCL=1
```

It is off by default, so the current Guition LCD and HDMI builds are unchanged.

Behaviour:

* `odroid_input_gamepad_init()` calls `tab5_ctrl_init()`. That call creates its own I2C bus
  on a free controller and starts a small polling task, which reads every 4 ms by default.
* At start-up, and whenever the port has been empty, the task probes 0x6D. If the board
  answers with the `RPAD` signature it is a console board, so the driver reads its ID and polls
  the 8-byte state block. Otherwise it is treated as a stock keyboard.
* After 5 failed transfers in a row the board is treated as removed and the port is probed
  again every second. The next emulator you start, or the one already running, picks up
  whichever board is attached. The log prints `RetroPad attached: Genesis (fw 0x21)`.
* `tab5_ctrl_get_info()` exposes `present`, `console` and `analog_count`. The launcher can use
  them to suggest the right board for a ROM; this is not wired up yet.

### Stock M5Stack keyboard

The driver puts the stock keyboard in Normal mode, tracks press and release events, and maps
keys to the canonical buttons:

| Keys | Button |
|---|---|
| Arrow keys | D-pad |
| `x` / `z` / `c` | A / B / C |
| `s` / `a` / `d` | X / Y / Z |
| `q` / `w` | L / R |
| `e` / `r` | L2 / R2 |
| `enter` / `space` | START / SELECT |
| `esc` / `del` | MENU / VOLUME |
| `tab` / `backspace` | OPT1 / OPT2 |
| `1`–`0`, `*`, `#` | Keypad |

## 6. Building and flashing the board firmware

The firmware is in [`firmware_avrdd/`](firmware_avrdd/README.md). It needs avr-gcc 13 or newer
with avr-libc 2.2 or newer for the AVR DD:

```
cd hardware/tab5_controller/firmware_avrdd
make AVR_GCC_DIR=/path/to/avr-gcc-14.1.0-x64-linux
make flash PORT=/dev/ttyUSB0       # SerialUPDI: USB-serial adapter + resistor
```

## 7. Status and open items

* **Console boards:** [`boards/`](boards/README.md) has routed, DRC-clean KiCad projects
  (schematic + PCB, AVR32DD28) for SNES, NES, Game Boy, Genesis and Master System / Game
  Gear. Each was checked against the firmware's pin map. Check the 2×5 header orientation
  before ordering.
* **Firmware:** builds to about 1.5 KB and passes a host test that replays the driver's I2C
  traffic. It has not yet run on real hardware.
* **Host driver:** the launcher builds with ESP-IDF v5.5.2 both with the driver off (the
  default) and with `CONFIG_TAB5_CTRL_ENABLE=y`. It has not yet run on real hardware.
* **This repo has no Tab5 board support yet.** The display (MIPI panel), touch, audio
  (ES8388/ES7210) and IO expanders are still Guition-specific. The controller driver is ready
  for a Tab5 port but does not make the firmware run on a Tab5 by itself.
* The connector pinout and the support circuit now come from M5Stack's schematic (§2.1,
  §2.2). The main outline is dimensioned (§1). M5Stack's STL conflicts with the drawing on
  height, key rows, fixing and latch arms (§1), so the latch arms, M3 hole height and header
  depth need checking with calipers on a real unit.
* Not done yet: consuming the keypad bits in the ColecoVision and 5200 cores, and a launcher
  hint such as "attach the Genesis pad".
