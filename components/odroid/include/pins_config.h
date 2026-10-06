#pragma once

#include "sdkconfig.h"

/* LCD Resolution: logical (legacy 480x800 portrait) UI space.  On the Tab5 the
 * physical panel is 720x1280; odroid_display / st7701_lcd scale this space by
 * 1.5x onto it (see tab5_board.h for the native geometry). */
#define LCD_H_RES 480
#define LCD_V_RES 800

/* LCD Reset Pin */
#define LCD_RST   -1

#ifdef CONFIG_BOARD_M5STACK_TAB5
/* ── M5Stack Tab5 ─────────────────────────────────────────────── */
#define LCD_BK_LIGHT_GPIO  22

/* System I2C (touch, IO expanders, ES8388) */
#define TP_I2C_SDA  31
#define TP_I2C_SCL  32
#define TP_RST      -1
#define TP_INT      -1

/* I2S / ES8388 Audio Codec Pins (amp enable is on an IO expander) */
#define I2S_MCLK_IO   30
#define I2S_BCLK_IO   27
#define I2S_WS_IO     29
#define I2S_DOUT_IO   26   /* ESP32 -> codec SDIN */
#define I2S_DIN_IO    28   /* codec DOUT -> ESP32 */
#define AUDIO_PA_IO   -1
#else
/* ── Original RetroESP32-P4 board ─────────────────────────────── */
/* LCD Backlight Pin */
#define LCD_BK_LIGHT_GPIO  23

/* Touch I2C Pins */
#define TP_I2C_SDA  7
#define TP_I2C_SCL  8
#define TP_RST      -1
#define TP_INT      -1

/* I2S / ES8311 Audio Codec Pins */
#define I2S_MCLK_IO   13
#define I2S_BCLK_IO   12
#define I2S_WS_IO     10
#define I2S_DOUT_IO    9   /* ESP32 -> ES8311 SDIN */
#define I2S_DIN_IO    48   /* ES8311 DOUT -> ESP32 */
#define AUDIO_PA_IO   11   /* Power Amplifier enable (active high) */
#endif

/* SD MMC Pins (identical on both boards: SDMMC slot 0 IO-MUX pins) */
#define SD_MMC_CLK  43
#define SD_MMC_CMD  44
#define SD_MMC_D0   39
#define SD_MMC_D1   40
#define SD_MMC_D2   41
#define SD_MMC_D3   42
