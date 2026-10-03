/*
 * GT911 touch wrapper - ported from Arduino C++ to ESP-IDF C
 */
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch_gt911.h"
#include "gt911_touch.h"

#ifdef CONFIG_BOARD_M5STACK_TAB5
/*
 * M5Stack Tab5: the touch controller is GT911 or ST712x depending on the
 * hardware revision and reports in the native 720x1280 portrait space.  All
 * callers (launcher, safe-boot check, virtual buttons, PAPP loader) work in
 * the original 480x800 portrait space, so coordinates are mapped back:
 *   legacy_x = x / 1.5,   legacy_y = (y - 40) / 1.5   (40 px letterbox)
 * The args of gt911_touch_init() are ignored - the board owns the pins.
 */
#include "tab5_board.h"

esp_err_t gt911_touch_init(int8_t sda_pin, int8_t scl_pin, int8_t rst_pin, int8_t int_pin)
{
    (void)sda_pin; (void)scl_pin; (void)rst_pin; (void)int_pin;
    return tab5_touch_init();   /* idempotent */
}

bool gt911_touch_get_xy(uint16_t *x, uint16_t *y)
{
    tab5_touch_point_t p;
    if (tab5_touch_read(&p, 1) < 1) return false;

    int lx = (int)p.x * 2 / 3;
    int ly = ((int)p.y - 40) * 2 / 3;
    if (lx < 0) lx = 0;
    if (lx > 479) lx = 479;
    if (ly < 0) ly = 0;
    if (ly > 799) ly = 799;
    if (x) *x = (uint16_t)lx;
    if (y) *y = (uint16_t)ly;
    return true;
}

#else  /* original RetroESP32-P4 board: GT911 on I2C1 */

#define CONFIG_LCD_HRES 480
#define CONFIG_LCD_VRES 800

static const char *TAG = "gt911_touch";

static esp_lcd_touch_handle_t s_tp = NULL;
static esp_lcd_panel_io_handle_t s_tp_io_handle = NULL;

esp_err_t gt911_touch_init(int8_t sda_pin, int8_t scl_pin, int8_t rst_pin, int8_t int_pin)
{
    // Get the I2C bus handle (bus must be initialized beforehand)
    i2c_master_bus_handle_t i2c_handle = NULL;
    ESP_ERROR_CHECK(i2c_master_get_bus_handle(1, &i2c_handle));

    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_config.scl_speed_hz = 100000;
    ESP_LOGI(TAG, "Initialize touch IO (I2C)");
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &s_tp_io_handle));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = CONFIG_LCD_HRES,
        .y_max = CONFIG_LCD_VRES,
        .rst_gpio_num = (gpio_num_t)rst_pin,
        .int_gpio_num = (gpio_num_t)int_pin,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ESP_LOGI(TAG, "Initialize touch controller GT911");
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(s_tp_io_handle, &tp_cfg, &s_tp));

    return ESP_OK;
}

bool gt911_touch_get_xy(uint16_t *x, uint16_t *y)
{
    if (!s_tp) return false;

    uint16_t touch_strength[1];
    uint8_t touch_cnt = 0;

    esp_lcd_touch_read_data(s_tp);
    bool touched = esp_lcd_touch_get_coordinates(s_tp, x, y, touch_strength, &touch_cnt, 1);

    return touched;
}

#endif /* CONFIG_BOARD_M5STACK_TAB5 */
