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
 │  STM32F030C8T6  ── 8x4 key matrix    │   same MCU as the M5 keyboard
 │  ID straps ─ console id              │   NES / SNES / Genesis / ...
 │  PA6/PA7   ─ paddles / analog stick  │   optional
 └──────────────────────────────────────┘
```

The design has three parts, and each one stays compatible with M5Stack's own:

| Part | Where | What it does |
|---|---|---|
| Board hardware | this document | Uses the keyboard's outline, latches, 2x5 header and MCU, with console-specific buttons |
| Board firmware | [`firmware/`](firmware) | A patch on M5Stack's MIT-licensed keyboard firmware. It adds a read-only register block and changes nothing else. |
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
| 10 | G9 | **not connected** | | 9 | INT_G50 | STM32 PA15, 10 kΩ pull-up to 3V3 |
| 8 | SDA_G0 | STM32 PB11 (I2C2_SDA), 4.7 kΩ pull-up | | 7 | SCL_G1 | STM32 PB10 (I2C2_SCL), 4.7 kΩ pull-up |
| 6 | SYS_EXT5V | **not connected** | | 5 | VCC_3V3 | Board supply |
| 4 | GND | GND | | 3 | GND | GND |
| 2 | GND | GND | | 1 | SYS_VIN | **not connected** |

* The whole board runs from the Tab5's 3.3 V on pin 5. Leave SYS_VIN and SYS_EXT5V open, as
  M5Stack does.
* G9 (pin 10) is unused on the keyboard. It is the only spare line to the Tab5, so it could
  carry a future signal. Leave it open for now.
* The host driver polls the board, so INT is optional. Keep the pin and its pull-up anyway, so
  the board still works with software written for the stock keyboard.

### 2.2 MCU and reference circuit

Use an **STM32F030C8T6 (LQFP48)**, the same part as the keyboard, so M5Stack's bootloader and
pinout carry over unchanged. Copy M5Stack's support circuit:

| Part | Keyboard ref | Value / connection |
|---|---|---|
| I2C pull-ups | R1, R2 | 4.7 kΩ from PB10 and PB11 to 3V3 |
| INT pull-up | R3 | 10 kΩ from PA15 to 3V3 |
| Reset | R4, C4 | 10 kΩ pull-up and 100 nF to GND on NRST (pin 7) |
| BOOT0 | R5 | 10 kΩ to GND (pin 44) |
| Decoupling | C1, C2, C5 | 3 × 100 nF on VDD (pins 1, 24, 48) and VDDA (pin 9). VSS/VSSA (pins 23, 47, 8) go to GND. |
| SWD header | P2 | 1 = 3V3, 2 = SWCLK (PA14), 3 = SWDIO (PA13), 4 = NRST, 5 = GND |
| Status LEDs (optional) | U2, U3, R7 | 2 × WS2812E-1313 in a chain, with the data input from PB15 through 1 kΩ |

Pin use on a RetroPad board:

| STM32 pin | Keyboard use | RetroPad use |
|---|---|---|
| PB0–PB7 | ROW0–ROW7 | Same: matrix rows, each driven high in turn |
| PA0–PA3 | COL0–COL3 | Same: matrix columns, with internal pull-downs |
| PA4, PA5, PA8 | COL4, COL5, COL8 | Spare matrix columns (future use) |
| PA6, PA7 | COL6, COL7 | **AN0 / AN1** analog inputs (ADC_IN6/7), taken out of the matrix |
| PA9, PA10 | COL9, COL10 (two FN keys straight to GND) | Unused |
| PB12, PB13, PB14, PB8 | unconnected | **Console ID straps** ID0–ID3 |
| PB9 | unconnected | **AN strap.** Fit it to enable AN0/AN1. |
| PB10 / PB11 | I2C2 | Same |
| PA15 | INT | Same |
| PB15 | RGB_DATA | Same (optional) |

Every pin the RetroPad adds was unconnected on the keyboard, so nothing on the keyboard
circuit has to move.

### 2.3 Buttons: one matrix wiring for every board

Each button **always** uses the same matrix position on every board. A board only populates
the buttons its console has. Bit *b* sits at **row = b % 8, column = b / 8**:

| Bit | Button | Row | Column |
|---|---|---|---|
| 0 | `UP` | ROW0 (PB0) | COL0 (PA0) |
| 1 | `DOWN` | ROW1 (PB1) | COL0 (PA0) |
| 2 | `LEFT` | ROW2 (PB2) | COL0 (PA0) |
| 3 | `RIGHT` | ROW3 (PB3) | COL0 (PA0) |
| 4 | `A` | ROW4 (PB4) | COL0 (PA0) |
| 5 | `B` | ROW5 (PB5) | COL0 (PA0) |
| 6 | `C` | ROW6 (PB6) | COL0 (PA0) |
| 7 | `X` | ROW7 (PB7) | COL0 (PA0) |
| 8 | `Y` | ROW0 (PB0) | COL1 (PA1) |
| 9 | `Z` | ROW1 (PB1) | COL1 (PA1) |
| 10 | `L` | ROW2 (PB2) | COL1 (PA1) |
| 11 | `R` | ROW3 (PB3) | COL1 (PA1) |
| 12 | `L2` | ROW4 (PB4) | COL1 (PA1) |
| 13 | `R2` | ROW5 (PB5) | COL1 (PA1) |
| 14 | `START` | ROW6 (PB6) | COL1 (PA1) |
| 15 | `SELECT` | ROW7 (PB7) | COL1 (PA1) |
| 16 | `MENU` | ROW0 (PB0) | COL2 (PA2) |
| 17 | `VOLUME` | ROW1 (PB1) | COL2 (PA2) |
| 18 | `OPT1` | ROW2 (PB2) | COL2 (PA2) |
| 19 | `OPT2` | ROW3 (PB3) | COL2 (PA2) |
| 20 | `KP1` | ROW4 (PB4) | COL2 (PA2) |
| 21 | `KP2` | ROW5 (PB5) | COL2 (PA2) |
| 22 | `KP3` | ROW6 (PB6) | COL2 (PA2) |
| 23 | `KP4` | ROW7 (PB7) | COL2 (PA2) |
| 24 | `KP5` | ROW0 (PB0) | COL3 (PA3) |
| 25 | `KP6` | ROW1 (PB1) | COL3 (PA3) |
| 26 | `KP7` | ROW2 (PB2) | COL3 (PA3) |
| 27 | `KP8` | ROW3 (PB3) | COL3 (PA3) |
| 28 | `KP9` | ROW4 (PB4) | COL3 (PA3) |
| 29 | `KP*` | ROW5 (PB5) | COL3 (PA3) |
| 30 | `KP0` | ROW6 (PB6) | COL3 (PA3) |
| 31 | `KP#` | ROW7 (PB7) | COL3 (PA3) |

**Put a diode on every switch, wired the way the keyboard does it:** anode on the ROW net,
cathode to one side of the switch, and the other side of the switch to the COL net (D1–D69
in M5Stack's matrix sheet). Gamepads routinely hold a diagonal plus two buttons. Without the
diodes that creates phantom presses.

### 2.4 Console ID straps

Each strap pin has an internal pull-up. Fit a 0 Ω resistor or a solder jumper to GND to set
that bit. With no straps fitted the ID is 0, which means Generic.

| ID | Console | ID3 PB8 | ID2 PB14 | ID1 PB13 | ID0 PB12 |
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

Because the ID is set by the straps, **one firmware image serves every board**.

### 2.5 Analog (optional)

Fit the AN strap (PB9 → GND). Then wire a 10 kΩ–100 kΩ potentiometer between 3V3 and GND
for each channel, with the wiper going to PA6 (AN0) or PA7 (AN1). Values are read as 8-bit.
AN0 drives the existing Atari paddle input (`odroid_paddle_adc_raw`).

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
| 5 | Genesis | D-pad, A→`A`, B→`B`, C→`C`, X→`X`, Y→`Y`, Z→`Z`, START, MODE→`SELECT` | A/B/C are remapped for the core. With this board attached, port 1 is a 6-button pad and the Genesis app passes X/Y/Z/MODE through. Holding MODE when the board connects keeps it 3-button. |
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
[`components/tab5_ctrl/include/retropad_proto.h`](../../components/tab5_ctrl/include/retropad_proto.h).
The firmware patch carries a byte-identical copy.

All stock registers (0x00–0x67, 0xFD–0xFF) keep their M5Stack meaning. The RetroPad
firmware adds:

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
single transfer. The firmware still produces Normal-mode key events, which report each
button's bit number as `row = bit / 14, col = bit % 14`. Tools written for the stock keyboard
can therefore still see presses.

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
  whichever board is attached. The log prints `RetroPad attached: Genesis (fw 0x01)`.
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

```
cd hardware/tab5_controller/firmware
./build.sh            # needs arm-none-eabi-gcc and python3
./build.sh --flash    # also writes it with an ST-Link (st-flash)
```

`build.sh` does the following:

1. Clones M5Stack's firmware at the revision the patch was made against.
2. Applies `retropad-fw.patch`.
3. Builds with plain GCC.
4. Runs `pack_retropad_fw.py`, which merges M5Stack's IAP bootloader with the new application
   and writes the CRC-32 the bootloader checks before it will start the app.

The CRC scheme was checked by recomputing the CRC of M5Stack's own prebuilt image. You can
also open the patched tree in STM32CubeIDE, but its output still needs `pack_retropad_fw.py`.

## 7. Status and open items

* **Firmware:** the patch applies cleanly to upstream and builds (17 KB of the 52 KB app
  area). It has not yet run on real hardware.
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
