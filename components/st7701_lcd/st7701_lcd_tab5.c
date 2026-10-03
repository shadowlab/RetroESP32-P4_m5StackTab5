/*
 * st7701_lcd API on the M5Stack Tab5.
 *
 * The launcher and the emulator sidebar code were written against the original
 * 480x800 portrait panel.  On the Tab5 (720x1280) this shim keeps that
 * coordinate space ("legacy" coordinates) and maps it onto the DSI frame
 * buffer with a 1.5x PPA scale, letterboxed 40 px top and bottom:
 *
 *      panel_x = legacy_x * 1.5
 *      panel_y = legacy_y * 1.5 + 40
 *
 * Game frames do not go through here - odroid_display renders them directly
 * (rotate + scale in one PPA pass).  This path serves the launcher's
 * full-screen draws and the small sidebar buttons.
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
#define SCALE           1.5f
#define Y_LETTERBOX     ((TAB5_PANEL_H - (int)(LEGACY_H * SCALE)) / 2)   /* 40 */

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

    uint32_t dx = (uint32_t)(x * SCALE + 0.5f);
    uint32_t dy = (uint32_t)(y * SCALE + 0.5f) + Y_LETTERBOX;
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
    return st7701_lcd_draw_rgb_bitmap(x, y, w, h, data);
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
