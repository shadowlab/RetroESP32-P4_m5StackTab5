/*
 * M5Stack Tab5 board support
 *
 * Hardware bring-up for the Tab5 (ESP32-P4, 5" 720x1280 MIPI-DSI panel,
 * PI4IOE5V6408 IO expanders, ES8388 codec, USB-A host).  Pin assignments and
 * panel init sequences follow Espressif's esp-bsp `m5stack_tab5` BSP
 * (Apache-2.0, see LICENSE.espressif-bsp), reduced to what the emulator HAL needs
 * and with no LVGL / esp_video / sensor dependencies.
 *
 * Three hardware revisions exist and are auto-detected at runtime:
 *   v1  ILI9881C panel + GT911  touch
 *   v2  ST7123   panel + ST7123 touch (fw 3)
 *   v3  ST7121   panel + ST712x touch (fw 1)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native panel geometry (portrait). The emulator UI is landscape and is rotated by PPA. */
#define TAB5_PANEL_W   720
#define TAB5_PANEL_H   1280

/* System I2C bus (touch, IO expanders, ES8388, ...) */
#define TAB5_I2C_PORT  1
#define TAB5_I2C_SDA   31
#define TAB5_I2C_SCL   32

/* Audio (ES8388 over I2S) */
#define TAB5_I2S_MCLK  30
#define TAB5_I2S_BCLK  27
#define TAB5_I2S_WS    29
#define TAB5_I2S_DOUT  26
#define TAB5_I2S_DIN   28

/* Backlight PWM (driven by odroid_display's LEDC channel) */
#define TAB5_LCD_BACKLIGHT_GPIO 22

/*
 * Presentation geometry.  The firmware draws a landscape image that is rotated 270 deg onto the
 * portrait panel.  Landscape view = 1280x720; landscape X = panel Y, landscape Y = 719 - panel X.
 *   launcher UI / PAPP apps : 800x480 logical  x 1.25  -> 1000x600, centred
 *   emulators               : 320x240 (legacy x2) x 1.5 -> 960x720 (3x), centred
 * Both leave >= 140 px side bars (landscape left/right) that hold the touch pad.
 */
#define TAB5_UI_SCALE    1.25f
#define TAB5_UI_X_OFF    60     /* panel X of the UI rect (landscape: 60 px top margin)  */
#define TAB5_UI_Y_OFF    140    /* panel Y of the UI rect (landscape: 140 px left margin) */
#define TAB5_EMU_SCALE   1.5f
#define TAB5_PAD_BAR_W   140    /* landscape px reserved at each side for the touch pad */

typedef enum {
    TAB5_REV_UNKNOWN = 0,
    TAB5_REV_ILI9881C_GT911,
    TAB5_REV_ST7123,
    TAB5_REV_ST7121,
} tab5_revision_t;

#define TAB5_MAX_TOUCH_POINTS 5

typedef struct {
    uint16_t x;   /* native portrait panel coordinates: 0..719  */
    uint16_t y;   /* native portrait panel coordinates: 0..1279 */
} tab5_touch_point_t;

/** Create the I2C bus and bring up the IO expanders (idempotent). */
esp_err_t tab5_board_init(void);

/** Shared I2C bus handle (NULL before tab5_board_init()). */
i2c_master_bus_handle_t tab5_board_i2c_bus(void);

/** Speaker amplifier enable (IO expander). */
esp_err_t tab5_board_speaker_enable(bool enable);

/** USB-A host port 5V rail (IO expander). Call before usb_host_install(). */
esp_err_t tab5_board_usb_power_enable(bool enable);

/**
 * Detect the hardware revision, power the DSI PHY and bring up the panel.
 * The DPI frame buffer(s) (RGB565, 720x1280, PSRAM) are cleared to black.  With
 * CONFIG_TAB5_DOUBLE_BUFFER there are two, swapped at the frame boundary (tear-free).
 * The backlight is NOT turned on (odroid_display owns the LEDC channel).
 */
esp_err_t tab5_display_init(void);

/** Detected hardware revision (valid after tab5_display_init()). */
tab5_revision_t tab5_board_revision(void);

/**
 * Frame presentation.  begin returns the buffer to draw the next full picture into (RGB565,
 * 720x1280, row stride 1440 bytes; PPA can render straight into it).  end queues it for
 * scan-out at the next frame boundary.
 * With double buffering, begin waits for the previous frame to reach the screen; if
 * `may_drop` is set it does not wait and returns NULL instead (drop this frame), so an
 * emulator is never slowed down to the panel's refresh rate.  Single-buffered: begin always
 * returns the one buffer and end does nothing.
 */
void *tab5_display_begin_frame(bool may_drop);
void  tab5_display_end_frame(void);

/**
 * All frame buffers, for content that must be identical in every buffer (the touch pad,
 * one-off full-screen draws): write it into each of them.
 */
int    tab5_display_fb_count(void);
void  *tab5_display_fb_at(int index);
size_t tab5_display_fb_size(void);

/**
 * Fill every frame buffer with an RGB565 colour (CPU, cache-synced).
 * If a fill hook is registered it runs after the fill, before the cache sync, so persistent
 * decorations (the touch pad) survive every clear.
 */
void tab5_display_fill(uint16_t color);

typedef void (*tab5_fill_hook_t)(uint16_t *fb);
void tab5_display_set_fill_hook(tab5_fill_hook_t hook);

/** Touch controller (GT911 or ST712x depending on revision). Idempotent. */
esp_err_t tab5_touch_init(void);

/**
 * Read currently-pressed touch points, native portrait coordinates.
 * @return number of points written to pts (0 when nothing is touched).
 */
int tab5_touch_read(tab5_touch_point_t *pts, int max_points);

/**
 * Battery monitor (INA226 @0x41, 5 mOhm shunt, NP-F550 2S pack).  Idempotent.
 * Needs tab5_board_init() first.
 */
esp_err_t tab5_battery_init(void);

/**
 * Read the pack voltage and current.
 * @param mv  pack voltage in millivolts (INA226 bus voltage), may be NULL
 * @param ma  signed shunt current in milliamps, may be NULL.  The sign follows the INA226
 *            convention; TAB5_BATT_CHARGE_CURRENT_POSITIVE says which sign means charging.
 */
esp_err_t tab5_battery_read(int *mv, int *ma);

/** 1 if positive INA226 current means "charging" (unverified on hardware - flip if the icon is inverted). */
#define TAB5_BATT_CHARGE_CURRENT_POSITIVE 1

#ifdef __cplusplus
}
#endif
