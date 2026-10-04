# M5Stack Tab5 port

The Tab5 is a third board target next to the Guition 4.3″ handheld and the HDMI console.
It is selected with **`CONFIG_BOARD_M5STACK_TAB5=y`** (see `launcher/sdkconfig.tab5.defaults`) and is
mutually exclusive with `CONFIG_HDMI_OUTPUT`.

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
| Emulators (NES, GB, SMS, Atari, PCE, …) | 320×240 | 960×720 (**3×**, integer, 4:3), 160 px bars |
| SNES / Genesis / Neo Geo | native size × 2 (legacy) | native size × 3 |

**Optimization — one PPA pass, no staging buffer.** The handheld path ran PPA into a staging buffer and
then `esp_lcd_panel_draw_bitmap()` copied that into the DSI frame buffer. On the Tab5 the PPA's
rotate + scale writes **straight into the DSI scan-out buffer** at the right offset
(`ppa_rotate_scale_rgb565_to_rect()`), removing a whole extra pass over 0.8–1.4 MB per frame — the
dominant cost at 60 FPS. Scale factors are rounded to the PPA's 1/16 resolution so output sizes are
deterministic. When the presented geometry changes (launcher ↔ emulator ↔ PAPP app) the frame buffer is
cleared once so no stale pixels remain in the bars.

Code that still talks to the old `st7701_lcd` API (the launcher's full-screen draws) goes through
`st7701_lcd_tab5.c`, which keeps the 480×800 coordinate space and maps it onto the same 1.25× UI rect.
The MENU/VOL sidebar buttons that SNES / Genesis / Neo Geo draw through that API are ignored — the touch
pad provides them — so those cores did not need any changes.

Touch input is mapped back to the same legacy 480×800 space in `gt911_touch.c` (through the UI rect), so
the launcher's touch keyboard and the PAPP loader work unchanged. Touches in the side bars belong to the
pad and are not reported there, so pressing a pad button never clicks a UI element.

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
  from its centre, so diagonals work. Polled at ≤ 125 Hz; touch I2C runs at 400 kHz.
* The same pad is shown in the launcher, in-game menus and PAPP apps, so touch alone can drive everything.
  X/Y keep their existing "X → MENU, Y → VOLUME" behaviour on cores without native X/Y.
* The layout is one table (`s_btn[]`) in landscape pixels — resize or move buttons there.

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

(Use a separate build dir + sdkconfig: a stale `CONFIG_BOARD_M5STACK_TAB5` / `CONFIG_HDMI_OUTPUT` in a reused
`sdkconfig` gives a black screen, same as the HDMI note in `ARCHITECTURE.md`.)

The SD card layout, ROM folders and Neo Geo cache generation are identical to the other targets.

**Slot sizes:** all 12 apps and the launcher fit their OTA slots in the Tab5 build. NES is the tightest
(`ota_0`, 576 KB): the Tab5 board code made it ~5 KB too big, so `apps/nes/sdkconfig.tab5.defaults` compiles
INFO log strings out of that one app (580 KB, 9 KB spare). Launcher: 725 KB of 768 KB.

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

## Ideas not done yet

* Touch pad polish: per-system layouts (hide X/Y on NES, show C/D on Neo Geo), haptic-free "dead zone"
  tuning, a transparency/size setting.
* NES / GB / SMS: scale the source straight to the panel in one PPA pass instead of going through the
  320×240 intermediate.
* Tab5 extras: IMU (BMI270) tilt controls, RTC, the ESP32-C6 (Wi-Fi) co-processor.
