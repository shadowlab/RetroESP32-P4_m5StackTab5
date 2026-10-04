# M5Stack Tab5 port

The Tab5 is a board target alongside the Guition 4.3″ handheld this firmware was written for.
It is selected with **`CONFIG_BOARD_M5STACK_TAB5=y`** (see `launcher/sdkconfig.tab5.defaults`).

> **Status:** the launcher and all 12 emulator apps compile, link and fit their flash slots for the Tab5
> with ESP-IDF 5.5.2; the handheld build still builds too.
> The port has **not been run on hardware yet** — expect to iterate on the first boot. Items that most
> need a look on a real device are listed under [First-boot checklist](#first-boot-checklist).

## Hardware map

| Function | Tab5 | Where it lives |
|---|---|---|
| SoC | ESP32-P4, 16 MB flash, 32 MB HEX PSRAM | unchanged `sdkconfig.defaults` |
| Display | 5″ 720×1280 MIPI-DSI, 2 lanes. **ILI9881C**, **ST7123** or **ST7121** depending on hardware revision — auto-detected | `components/tab5_board` |
| Touch | **GT911** (v1) or **ST712x** (v2/v3), up to 5 points, auto-detected | `components/tab5_board` |
| Power rails | PI4IOE5V6408 IO expanders @ 0x43 / 0x44 (LCD, touch, speaker amp, USB-A 5 V) | `components/tab5_board` |
| I2C | SDA 31 / SCL 32 (system bus, shared by everything above + codec) | `pins_config.h` |
| Audio | **ES8388** over I2S: MCLK 30, BCLK 27, WS 29, DOUT 26, DIN 28 | `components/audio` |
| Backlight | PWM on GPIO 22 | `components/odroid` |
| SD card | SDMMC slot 0, GPIO 39–44 (same pins as the handheld) | unchanged |
| Controllers | USB-A host port (powered via the expander) | `components/gamepad` |

Pin numbers and the panel init sequences come from Espressif's `esp-bsp` `m5stack_tab5` BSP
(Apache-2.0, license in `components/tab5_board/LICENSE.espressif-bsp`). The BSP itself is **not** a
dependency: it drags in LVGL and esp_video, and the launcher partition is already ~94 % full. Only the
small `esp_lcd_ili9881c`, `esp_lcd_st7123` and `esp_io_expander_pi4ioe5v6408` components are fetched from
the component registry (first build needs network access).

## Display pipeline

All the firmware renders for a landscape 800×480 / 320×240 image that is rotated 270° onto a 480×800
portrait panel. The Tab5 panel is 720×1280, so the original "legacy" geometry is kept and every
presentation gets an extra scale factor chosen so that **≥ 140 px side bars** stay free for the
[touch pad](#on-screen-touch-pad):

| Content | Source | Result on the 1280×720 landscape view |
|---|---|---|
| Launcher UI / PAPP apps | 800×480 | 1000×600 (**1.25×**), 140 px bars left/right |
| Emulators (Atari, PCE, Spectrum, …) | 320×240 | 960×720 (**3×**, integer, 4:3), 160 px bars |
| Game Boy / Game Gear | 160×144 | 960×720 (6× / 5×), one PPA pass |
| Master System | 256×192 | 960×720 (3.75×), one PPA pass |
| NES | 256×224 | 960×714 (3.75× / 3.1875×), one PPA pass |
| Lynx | 160×102 | 960×612 (6×, true aspect), one PPA pass |
| SNES / Genesis / Neo Geo | native size × 2 (legacy) | native size × 3 |

**Optimization — one PPA pass, no staging buffer.** The handheld path ran PPA into a staging buffer and
then `esp_lcd_panel_draw_bitmap()` copied that into the DSI frame buffer. On the Tab5 the PPA's
rotate + scale writes **straight into the DSI scan-out buffer** at the right offset
(`ppa_rotate_scale_rgb565_to_rect()`), removing a whole extra pass over 0.8–1.4 MB per frame — the
dominant cost at 60 FPS. Scale factors are rounded to the PPA's 1/16 resolution so output sizes are
deterministic. When the presented geometry changes (launcher ↔ emulator ↔ PAPP app) the frame buffer is
cleared once so no stale pixels remain in the bars.

**Optimization — native frames in one pass.** On the handheld, NES, Game Boy, Master System / Game Gear
scale to a 320×240 buffer first, then again onto the panel, and Lynx goes through the whole 800×480 UI
buffer. On the Tab5 these go straight from their native resolution to the panel in a single PPA pass,
with factors the PPA represents exactly (multiples of 1/16), keeping the same 4:3 picture.

**Tear-free double buffering** (`CONFIG_TAB5_DOUBLE_BUFFER`, default on). The panel driver gets two
720×1280 frame buffers (+1.8 MB PSRAM). Each frame is drawn into the hidden one and swapped in at the
panel's frame boundary, so a picture is never shown half-drawn. The swap needs no copy: handing the driver
a pointer inside its own buffer just switches the scan-out buffer. Before redrawing the old buffer the
next frame waits for VSYNC. **Emulator frames never wait**: if the previous frame is not on screen yet the
new one is dropped, so emulation speed (and audio) never depends on the panel's refresh rate. UI frames
(launcher, menus, PAPP apps) do wait, so the last state is always shown. The touch pad, frame-buffer
clears and the launcher's one-off draws are written into both buffers.

**Panel refresh rate.** With the esp-bsp timings the panels refresh at about **48 Hz (v1, ILI9881C, 60 MHz
pixel clock)** and **58 Hz (v2/v3, ST712x, 70 MHz)**, below the emulators' 60 FPS, so some frames are
dropped (judder). Both clocks are Kconfig options; `sdkconfig.tab5.defaults` has the lines to uncomment for
~60 Hz (75 MHz / 73 MHz). They are opt-in because they are outside the tested timings; the boot log prints
the resulting refresh rate (`Display ready: … MHz pixel clock -> ~NN.N Hz`).

**Optimization — code runs from PSRAM.** `sdkconfig.tab5.defaults` sets QIO flash and
`SPIRAM_XIP_FROM_PSRAM`, as M5Stack's own Tab5 firmware does: code and constants are copied into the
200 MHz HEX PSRAM at boot, so cache misses no longer go to 80 MHz flash. This costs about the app's size in
PSRAM (≤ 1.3 MB of 32 MB); Neo Geo sizes its caches from the remaining free PSRAM at runtime.

Code that still talks to the old `st7701_lcd` API (the launcher's full-screen draws) goes through
`st7701_lcd_tab5.c`, which keeps the 480×800 coordinate space and maps it onto the same 1.25× UI rect.
The MENU/VOL sidebar buttons that SNES / Genesis / Neo Geo draw through that API are ignored — the touch
pad provides them — so those cores did not need any changes.

Touch input is mapped back to the same legacy 480×800 space in `gt911_touch.c` (through the UI rect), so
the launcher's touch keyboard and the PAPP loader work unchanged. Touches in the side bars belong to the
pad and are not reported there, so pressing a pad button never clicks a UI element.

**Faster emulator launch / return.** Starting an emulator and going back to the launcher are both
reboots, and detecting the hardware revision costs a 500 ms wait for the touch controller. The detected
revision is kept in RTC no-init memory (it survives a software reset; same address in every image), so only
the first boot after power-on probes; warm boots log `cached across reboot, probe skipped` and save
~450 ms each way. The cache is used only after a software reset and only if its magic and check word match;
otherwise the board is probed as before.

## Input

* **USB gamepads work as on the other boards** (the 5 V rail of the USB-A port is switched on at boot).
* The handheld's **GPIO pad is disabled** on the Tab5 — its pins (28/29/30) are the codec's I2S lines.
  The paddle / battery ADC code is disabled for the same reason.
* **Battery:** read from the Tab5's **INA226** power monitor (I2C 0x41, 5 mΩ shunt) in
  `tab5_battery_read()`. The pack is a 2S NP-F550, so the voltage is converted to a percentage per cell
  (3.0–4.2 V) and fed through the existing `odroid_input_battery_level_read()`, i.e. the launcher's
  battery icon works unchanged. The standard Tab5 has no pack and runs from 5 V: a bus voltage under
  5.5 V is treated as "no battery" and reported as 100 %. "Charging" means current above ±50 mA in the
  direction set by `TAB5_BATT_CHARGE_CURRENT_POSITIVE`.
* **No USB pad needed:** an on-screen touch pad is built in, see below. USB pads and the touch pad can be
  used together (their buttons are OR-ed).

### On-screen touch pad

`components/odroid/tab5_pad.c`. It lives in the two black side bars next to the picture (landscape view):

```
 left bar                          right bar
 [   L   ]                         [   R   ]
 [ MENU  ]                         [  VOL  ]
                                   [Y]  [X]
     ▲                             [   A   ]
   ◀ ■ ▶                           [   B   ]
     ▼
 [SELECT ]                         [ START ]
```

* Drawn **into the DSI frame buffer** and redrawn from a hook after every frame-buffer clear
  (`tab5_display_set_fill_hook`). The emulator/UI picture never covers the bars, so it costs nothing per
  frame; pressed buttons are re-drawn highlighted (only that button, cache-synced).
* **Multi-touch** (up to 5 points): hold the D-pad and A/B together; the D-pad is hit-tested by direction
  from its centre, so diagonals work. Touch I2C runs at 400 kHz.
* **Polled off the emulator threads:** a background task on core 0 reads the touch controller every 8 ms
  and redraws changed buttons; `odroid_input_gamepad_read()` only reads the cached button mask.
* The same pad is shown in the launcher, in-game menus and PAPP apps, so touch alone can drive everything.
  X/Y keep their existing "X → MENU, Y → VOLUME" behaviour on cores without native X/Y.
* The layout is one table (`s_btn_base[]`) in landscape pixels — resize or move buttons there.
* **Per-system layouts** (`s_layouts[]`, picked from the app's project name): buttons a core ignores are
  hidden (not drawn, not touchable) and the rest carry the console's names, following each core's mapping:

  | System | Buttons shown |
  |---|---|
  | NES, Game Boy / Color, ZX Spectrum | A, B, Start, Select |
  | Master System / Game Gear | 2, 1, Start, Pause |
  | Atari 2600 | Fire, Select, Reset |
  | Atari 7800 | A, B, Select, Pause |
  | Atari Lynx | A, B, Opt1, Pause |
  | PC Engine | I, II, Select, Run |
  | Atari 800 / 5200 | Fire, KBD (on-screen keyboard), Select, Start |
  | Genesis | A (full width), B, C, Start |
  | Neo Geo | A, B, C, D, Coin, Start |
  | SNES, launcher, PAPP apps | full pad |

  The D-pad, MENU and VOL are always there.

## Building

```bash
. $IDF_PATH/export.sh            # ESP-IDF 5.5.x
./build_all_tab5.sh              # launcher + 12 emulators -> firmware_tab5/ and RetroESP32_P4_Tab5_v1.bin
./build_all_tab5.sh launcher snes   # or just some projects
python -m esptool --chip esp32p4 -b 460800 write_flash 0x0 RetroESP32_P4_Tab5_v1.bin
```

A single project by hand:

```bash
cd launcher
idf.py -B build_tab5 -DSDKCONFIG=build_tab5/sdkconfig \
       -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.tab5.defaults" build
```

(Use a separate build dir + sdkconfig: a Tab5 `sdkconfig` reused for a handheld build, or the other way
round, drives the wrong display and pins.)

The SD card layout, ROM folders and Neo Geo cache generation are identical to the other targets.

**Slot sizes:** all 12 apps and the launcher fit their OTA slots in the Tab5 build. The NES slot (`ota_0`)
was grown from 576 KB to 640 KB with 64 KB from the unused `ota_9` slot (`ota_1`…`ota_8` moved up by
0x10000, sizes unchanged), so NES builds with full logging at ~590 KB. The tightest slots are now Atari 800
(~757 / 768 KB) and the launcher (~735 / 768 KB). The partition table is shared with the other targets: after
updating, flash the full image (or the partition table plus every app) once.

## First-boot checklist

1. **Serial log** — look for `Detected Tab5 hardware v1/v2/v3` from `tab5_board`. If it prints
   *"No known touch controller found"* the revision probing needs a look (it follows the BSP: ST7123 touch
   answers at 0x55, GT911 at 0x14).
2. **Orientation** — the UI is rotated 270° like the handheld. If the picture and touch are upside down,
   the fix is the rotation angle in `tab5_present()` / `st7701_lcd_tab5.c` plus the mirrored mapping in
   `gt911_touch.c` and `tab5_pad.c`. The pad's drawing and hit-testing were checked on the host
   (rendered upright, every button hits itself); whether touch coordinates line up with the panel's
   orientation is **not** verified on hardware: tap each pad button once and watch it highlight.
3. **Audio** — ES8388 init happens in `components/audio/audio.c`; the speaker amp is enabled through the
   expander (`tab5_board_speaker_enable`).
4. **USB gamepad** — needs the expander's USB rail (`tab5_board_usb_power_enable`).
5. **Battery** — the serial log shows `INA226 battery monitor ready`. With a pack fitted the icon should
   track charge; if "charging" looks inverted, flip `TAB5_BATT_CHARGE_CURRENT_POSITIVE` (the shunt's
   current sign is not verified on hardware).
6. **Frame rate** — `DISP TIMING` lines print the PPA time per frame every 60 frames.
7. **Refresh / tearing** — the boot log prints the panel refresh rate. For ~60 Hz uncomment the clock lines
   in `sdkconfig.tab5.defaults` and rebuild; if the panel then shows noise, a black screen or rolls, go back
   to the defaults. If double buffering misbehaves (frozen or flickering picture), build with
   `CONFIG_TAB5_DOUBLE_BUFFER=n` to compare.

## Ideas not done yet

* Touch pad polish: "dead zone" tuning and a size setting once it has been tried on the device.
* Tab5 extras: IMU (BMI270) tilt controls, RTC, the ESP32-C6 (Wi-Fi) co-processor.
