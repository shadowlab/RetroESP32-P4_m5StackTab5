/*
 * st7701_lcd API on the M5Stack Tab5.
 *
 * The launcher and the emulator sidebar code were written against the original
 * 480x800 portrait panel.  On the Tab5 (720x1280) this shim keeps that
 * coordinate space ("legacy" coordinates) and maps it onto the DSI frame
 * buffer with the same 1.25x UI rect that odroid_display uses (TAB5_UI_*):
 *
 *      panel_x = 60  + legacy_x * 1.25
 *      panel_y = 140 + legacy_y * 1.25
 *
 * Game frames do not go through here - odroid_display renders them directly
 * (rotate + scale in one PPA pass).  This path serves the launcher's
 * full-screen draws.  The MENU/VOL sidebar buttons that SNES / Genesis /
 * Neo Geo draw through st7701_lcd_draw_to_fb() are ignored: the touch pad
 * (tab5_pad.c) owns the side bars and provides MENU/VOL itself.
 */
#include "sdkconfig.h"

#ifdef CONFIG_BOARD_M5STACK_TAB5

#include <string.h>
#include "esp_log.h"
#include "st7701_lcd.h"
#include "tab5_board.h"
#include "ppa_engine.h"

#define LEGACY_W        480
#define LEGACY_H        800
#define SCALE           TAB5_UI_SCALE

static const char *TAG = "st7701_tab5";

esp_err_t st7701_lcd_init(void)
{
    return tab5_display_init();
}

esp_err_t st7701_lcd_draw_rgb_bitmap(uint16_t x, uint16_t y,
                                      uint16_t w, uint16_t h,
                                      const uint16_t *data)
{
    void *fb = tab5_display_fb();
    if (!fb) return ESP_ERR_INVALID_STATE;
    if (!data || w == 0 || h == 0) return ESP_ERR_INVALID_ARG;
    if (x + w > LEGACY_W || y + h > LEGACY_H) return ESP_ERR_INVALID_SIZE;

    uint32_t dx = (uint32_t)(x * SCALE + 0.5f) + TAB5_UI_X_OFF;
    uint32_t dy = (uint32_t)(y * SCALE + 0.5f) + TAB5_UI_Y_OFF;
    esp_err_t ret = ppa_rotate_scale_rgb565_to_rect(
        data, w, h, 0, SCALE, SCALE,
        fb, tab5_display_fb_size(), TAB5_PANEL_W, TAB5_PANEL_H,
        dx, dy, NULL, NULL, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "draw %ux%u @(%u,%u) failed: %s", w, h, x, y, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t st7701_lcd_draw_bitmap(uint16_t x_start, uint16_t y_start,
                                  uint16_t x_end, uint16_t y_end,
                                  const uint16_t *color_data)
{
    return st7701_lcd_draw_rgb_bitmap(x_start, y_start,
                                      x_end - x_start, y_end - y_start, color_data);
}

esp_err_t st7701_lcd_draw_to_fb(uint16_t x, uint16_t y,
                                uint16_t w, uint16_t h,
                                const uint16_t *data)
{
    /* Legacy emulator sidebar buttons: superseded by the Tab5 touch pad. */
    (void)x; (void)y; (void)w; (void)h; (void)data;
    return ESP_OK;
}

esp_err_t st7701_lcd_fill_screen(uint16_t color)
{
    if (!tab5_display_fb()) return ESP_ERR_INVALID_STATE;
    tab5_display_fill(color);
    return ESP_OK;
}

void st7701_lcd_get_handles(bsp_lcd_handles_t *ret_handles)
{
    if (ret_handles) memset(ret_handles, 0, sizeof(*ret_handles));
}

uint16_t st7701_lcd_width(void)  { return LEGACY_W; }
uint16_t st7701_lcd_height(void) { return LEGACY_H; }

#endif /* CONFIG_BOARD_M5STACK_TAB5 */
