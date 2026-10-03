# M5Stack Tab5 port

The Tab5 is a third board target next to the Guition 4.3″ handheld and the HDMI console.
It is selected with **`CONFIG_BOARD_M5STACK_TAB5=y`** (see `launcher/sdkconfig.tab5.defaults`) and is
mutually exclusive with `CONFIG_HDMI_OUTPUT`.

> **Status:** the launcher and all 12 emulator apps compile and link for the Tab5 with ESP-IDF 5.5.2.
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
portrait panel. The Tab5 panel is exactly **1.5×** that in both axes (720×1280 vs 480×800 — with 40 spare
rows top and bottom for the 800 → 1200 stretch), so the original "legacy" geometry is kept and every
presentation gets one extra 1.5× factor:

| Content | Source | Result on the 1280×720 landscape view |
|---|---|---|
| Launcher UI / PAPP apps | 800×480 | 1200×720 (1.5×), 40 px bars left/right |
| Emulators (NES, GB, SMS, Atari, PCE, …) | 320×240 | 960×720 (**3×**, integer, 4:3), 160 px bars |
| SNES / Genesis / Neo Geo | native size × 2 (legacy) | native size × 3 |

**Optimization — one PPA pass, no staging buffer.** The handheld path ran PPA into a staging buffer and
then `esp_lcd_panel_draw_bitmap()` copied that into the DSI frame buffer. On the Tab5 the PPA's
rotate + scale writes **straight into the DSI scan-out buffer** at the right offset
(`ppa_rotate_scale_rgb565_to_rect()`), removing a whole extra pass over 0.8–1.4 MB per frame — the
dominant cost at 60 FPS. Scale factors are rounded to the PPA's 1/16 resolution so output sizes are
deterministic. When the presented geometry changes (launcher ↔ emulator ↔ PAPP app) the frame buffer is
cleared once so no stale pixels remain in the bars.

Code that still talks to the old `st7701_lcd` API (launcher full-screen draws, the MENU/VOL sidebar
buttons in SNES / Genesis / Neo Geo) goes through `st7701_lcd_tab5.c`, which keeps the 480×800 coordinate
space and maps it onto the panel with a 1.5× PPA scale — those cores did not need any changes.

Touch input is mapped back to the same legacy 480×800 space in `gt911_touch.c`, so the launcher, the
touch keyboard, the PAPP loader and the MENU/VOL zones all work unchanged.

## Input

* **USB gamepads work as on the other boards** (the 5 V rail of the USB-A port is switched on at boot).
* The handheld's **GPIO pad is disabled** on the Tab5 — its pins (28/29/30) are the codec's I2S lines.
  The paddle / battery ADC code is disabled for the same reason; the battery reads as full for now.
* Touch still provides the **MENU** (touch the first ~170 legacy px, i.e. the left end of the landscape
  view) and **VOLUME** (the right end) buttons plus the launcher UI. There is **no on-screen D-pad / buttons
  yet** — play with a USB controller.

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

## First-boot checklist

1. **Serial log** — look for `Detected Tab5 hardware v1/v2/v3` from `tab5_board`. If it prints
   *"No known touch controller found"* the revision probing needs a look (it follows the BSP: ST7123 touch
   answers at 0x55, GT911 at 0x14).
2. **Orientation** — the UI is rotated 270° like the handheld. If the picture and touch are upside down,
   the fix is the rotation angle in `tab5_present()` / `st7701_lcd_tab5.c` plus the mirrored mapping in
   `gt911_touch.c`.
3. **Audio** — ES8388 init happens in `components/audio/audio.c`; the speaker amp is enabled through the
   expander (`tab5_board_speaker_enable`).
4. **USB gamepad** — needs the expander's USB rail (`tab5_board_usb_power_enable`).
5. **Frame rate** — `DISP TIMING` lines print the PPA time per frame every 60 frames.

## Ideas not done yet

* On-screen touch controller (multi-touch is already available from `tab5_touch_read()`; the 160 px
  bars next to a 3× emulator image are the natural place for it).
* Battery level from the Tab5's power monitor (I2C) instead of the handheld's ADC divider.
* NES / GB / SMS: scale the source straight to the panel in one PPA pass instead of going through the
  320×240 intermediate.
* Tab5 extras: IMU (BMI270) tilt controls, RTC, the ESP32-C6 (Wi-Fi) co-processor.
