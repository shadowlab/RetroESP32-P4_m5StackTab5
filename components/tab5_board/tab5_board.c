/*
 * M5Stack Tab5 board support - see tab5_board.h.
 *
 * Panel/touch detection, DPI timings and IO-expander usage are derived from
 * Espressif's esp-bsp m5stack_tab5 BSP (Apache-2.0).
 */
#include "sdkconfig.h"

#ifdef CONFIG_BOARD_M5STACK_TAB5

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_cache.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/gpio.h"
#include "esp_io_expander.h"
#include "esp_io_expander_pi4ioe5v6408.h"
#include "esp_lcd_ili9881c.h"
#include "esp_lcd_st7123.h"

#include "tab5_board.h"
#include "tab5_init_ili9881c.h"
#include "tab5_init_st7123.h"
#include "tab5_init_st7121.h"

static const char *TAG = "tab5_board";

/* IO expander pins (expander @0x43 unless noted) */
#define EXP_SPEAKER_EN   IO_EXPANDER_PIN_NUM_1
#define EXP_LCD_EN       IO_EXPANDER_PIN_NUM_4
#define EXP_TOUCH_EN     IO_EXPANDER_PIN_NUM_5
#define EXP1_USB_EN      IO_EXPANDER_PIN_NUM_3   /* expander @0x44 */

#define TAB5_TOUCH_INT_GPIO     23
#define TAB5_DSI_PHY_LDO_CHAN   3
#define TAB5_DSI_PHY_LDO_MV     2500
#define TAB5_DSI_LANES          2
#define TAB5_DSI_BITRATE_MBPS   1000
#define TAB5_DSI_BITRATE_ST7121 965

#define ST7123_TOUCH_ADDR       0x55
#define GT911_TOUCH_ADDR_BACKUP 0x14

static i2c_master_bus_handle_t  s_i2c = NULL;
static esp_io_expander_handle_t s_exp0 = NULL;
static esp_io_expander_handle_t s_exp1 = NULL;

static tab5_revision_t           s_rev = TAB5_REV_UNKNOWN;
static esp_lcd_dsi_bus_handle_t  s_dsi_bus = NULL;
static esp_lcd_panel_io_handle_t s_dbi_io = NULL;
static esp_lcd_panel_handle_t    s_panel = NULL;
static void                     *s_fb = NULL;

/* ─── I2C + IO expanders ───────────────────────────────────────── */

esp_err_t tab5_board_init(void)
{
    if (s_exp1) return ESP_OK;

    if (!s_i2c) {
        const i2c_master_bus_config_t cfg = {
            .i2c_port = TAB5_I2C_PORT,
            .sda_io_num = TAB5_I2C_SDA,
            .scl_io_num = TAB5_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c), TAG, "I2C bus init failed");
        ESP_LOGI(TAG, "I2C bus ready (SDA=%d SCL=%d)", TAB5_I2C_SDA, TAB5_I2C_SCL);
    }
    if (!s_exp0) {
        ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_pi4ioe5v6408(
            s_i2c, ESP_IO_EXPANDER_I2C_PI4IOE5V6408_ADDRESS_LOW, &s_exp0), TAG, "IO expander 0 init failed");
    }
    ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_pi4ioe5v6408(
        s_i2c, ESP_IO_EXPANDER_I2C_PI4IOE5V6408_ADDRESS_HIGH, &s_exp1), TAG, "IO expander 1 init failed");
    return ESP_OK;
}

i2c_master_bus_handle_t tab5_board_i2c_bus(void)
{
    return s_i2c;
}

static esp_err_t exp_push_pull(esp_io_expander_handle_t h, uint32_t pin, bool level)
{
    esp_err_t ret = esp_io_expander_set_dir(h, pin, IO_EXPANDER_OUTPUT);
    ret |= esp_io_expander_set_level(h, pin, level);
    ret |= esp_io_expander_set_output_mode(h, pin, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    return ret;
}

esp_err_t tab5_board_speaker_enable(bool enable)
{
    ESP_RETURN_ON_ERROR(tab5_board_init(), TAG, "board init");
    return exp_push_pull(s_exp0, EXP_SPEAKER_EN, enable);
}

esp_err_t tab5_board_usb_power_enable(bool enable)
{
    ESP_RETURN_ON_ERROR(tab5_board_init(), TAG, "board init");
    return exp_push_pull(s_exp1, EXP1_USB_EN, enable);
}

static esp_err_t lcd_enable(bool enable)
{
    /* LCD_EN is released (input + pull-up + open-drain) to power the panel,
     * and driven low to cut it - matches the BSP's BSP_FEATURE_LCD. */
    esp_err_t ret = ESP_OK;
    if (enable) {
        ret |= esp_io_expander_set_pullupdown(s_exp0, EXP_LCD_EN, IO_EXPANDER_PULL_UP);
        ret |= esp_io_expander_set_dir(s_exp0, EXP_LCD_EN, IO_EXPANDER_INPUT);
        ret |= esp_io_expander_set_output_mode(s_exp0, EXP_LCD_EN, IO_EXPANDER_OUTPUT_MODE_OPEN_DRAIN);
    } else {
        ret |= esp_io_expander_set_dir(s_exp0, EXP_LCD_EN, IO_EXPANDER_OUTPUT);
        ret |= esp_io_expander_set_level(s_exp0, EXP_LCD_EN, 0);
        ret |= esp_io_expander_set_output_mode(s_exp0, EXP_LCD_EN, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    }
    return ret;
}

/* ─── Revision detection ───────────────────────────────────────── */

/* 16-bit-register I2C IO used by both touch controllers */
static esp_err_t touch_io_new(uint8_t addr, esp_lcd_panel_io_handle_t *io)
{
    const esp_lcd_panel_io_i2c_config_t cfg = {
        .scl_speed_hz = 100000,
        .dev_addr = addr,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 16,
        .flags = { .disable_control_phase = 1 },
    };
    return esp_lcd_new_panel_io_i2c(s_i2c, &cfg, io);
}

static tab5_revision_t detect_revision(void)
{
    if (s_rev != TAB5_REV_UNKNOWN) return s_rev;

    /* Touch rail must be up before the controllers answer on I2C. */
    if (exp_push_pull(s_exp0, EXP_TOUCH_EN, true) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable touch rail");
        return TAB5_REV_UNKNOWN;
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    if (i2c_master_probe(s_i2c, ST7123_TOUCH_ADDR, 100) == ESP_OK) {
        esp_lcd_panel_io_handle_t io = NULL;
        uint8_t fw = 0;
        if (touch_io_new(ST7123_TOUCH_ADDR, &io) != ESP_OK) return TAB5_REV_UNKNOWN;
        esp_err_t ret = esp_lcd_panel_io_rx_param(io, 0x0000, &fw, 1);
        esp_lcd_panel_io_del(io);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ST712x firmware version read failed: %s", esp_err_to_name(ret));
            return TAB5_REV_UNKNOWN;
        }
        if (fw == 1)      s_rev = TAB5_REV_ST7121;
        else if (fw == 3) s_rev = TAB5_REV_ST7123;
        else {
            ESP_LOGW(TAG, "Unsupported ST712x firmware version %u", fw);
            return TAB5_REV_UNKNOWN;
        }
    } else if (i2c_master_probe(s_i2c, GT911_TOUCH_ADDR_BACKUP, 100) == ESP_OK) {
        s_rev = TAB5_REV_ILI9881C_GT911;
    } else {
        ESP_LOGE(TAG, "No known touch controller found - unsupported Tab5 revision");
        return TAB5_REV_UNKNOWN;
    }

    static const char *names[] = { "?", "v1 (ILI9881C + GT911)", "v2 (ST7123)", "v3 (ST7121)" };
    ESP_LOGI(TAG, "Detected Tab5 hardware %s", names[s_rev]);
    return s_rev;
}

tab5_revision_t tab5_board_revision(void)
{
    return s_rev;
}

/* ─── Display ──────────────────────────────────────────────────── */

esp_err_t tab5_display_init(void)
{
    if (s_panel) return ESP_OK;

    ESP_RETURN_ON_ERROR(tab5_board_init(), TAG, "board init");
    ESP_RETURN_ON_ERROR(lcd_enable(true), TAG, "LCD enable failed");

    esp_ldo_channel_handle_t ldo = NULL;
    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = TAB5_DSI_PHY_LDO_CHAN,
        .voltage_mv = TAB5_DSI_PHY_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "DSI PHY LDO failed");

    tab5_revision_t rev = detect_revision();
    ESP_RETURN_ON_FALSE(rev != TAB5_REV_UNKNOWN, ESP_ERR_NOT_SUPPORTED, TAG, "unsupported Tab5 revision");

    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = TAB5_DSI_LANES,
        .phy_clk_src = 0,
        .lane_bit_rate_mbps = (rev == TAB5_REV_ST7121) ? TAB5_DSI_BITRATE_ST7121 : TAB5_DSI_BITRATE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &s_dsi_bus), TAG, "DSI bus failed");

    const esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &s_dbi_io), TAG, "DBI IO failed");

    /* Video timings per panel (from the BSP).  One frame buffer only: PPA renders
     * straight into it and several callers update sub-rectangles of it, which a
     * ping-pong scheme would break. */
    const esp_lcd_dpi_panel_config_t dpi_ili9881c = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 60,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = TAB5_PANEL_W, .v_size = TAB5_PANEL_H,
            .hsync_back_porch = 140, .hsync_pulse_width = 40, .hsync_front_porch = 40,
            .vsync_back_porch = 20,  .vsync_pulse_width = 4,  .vsync_front_porch = 20,
        },
        .flags.use_dma2d = true,
    };
    const esp_lcd_dpi_panel_config_t dpi_st7123 = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 70,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = TAB5_PANEL_W, .v_size = TAB5_PANEL_H,
            .hsync_back_porch = 40, .hsync_pulse_width = 2, .hsync_front_porch = 40,
            .vsync_back_porch = 8,  .vsync_pulse_width = 2, .vsync_front_porch = 220,
        },
        .flags.use_dma2d = true,
    };
    const esp_lcd_dpi_panel_config_t dpi_st7121 = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 70,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = TAB5_PANEL_W, .v_size = TAB5_PANEL_H,
            .hsync_back_porch = 40, .hsync_pulse_width = 2,  .hsync_front_porch = 40,
            .vsync_back_porch = 24, .vsync_pulse_width = 20, .vsync_front_porch = 200,
        },
        .flags.use_dma2d = true,
    };

    const ili9881c_vendor_config_t vc_ili9881c = {
        .init_cmds = disp_init_data_ili9881c,
        .init_cmds_size = sizeof(disp_init_data_ili9881c) / sizeof(disp_init_data_ili9881c[0]),
        .mipi_config = { .dsi_bus = s_dsi_bus, .dpi_config = &dpi_ili9881c, .lane_num = TAB5_DSI_LANES },
    };
    const st7123_vendor_config_t vc_st7123 = {
        .init_cmds = disp_init_data_st7123,
        .init_cmds_size = sizeof(disp_init_data_st7123) / sizeof(disp_init_data_st7123[0]),
        .mipi_config = { .dsi_bus = s_dsi_bus, .dpi_config = &dpi_st7123 },
    };
    const st7123_vendor_config_t vc_st7121 = {
        .init_cmds = disp_init_data_st7121,
        .init_cmds_size = sizeof(disp_init_data_st7121) / sizeof(disp_init_data_st7121[0]),
        .mipi_config = { .dsi_bus = s_dsi_bus, .dpi_config = &dpi_st7121 },
    };

    const void *vendor = (rev == TAB5_REV_ILI9881C_GT911) ? (const void *)&vc_ili9881c
                       : (rev == TAB5_REV_ST7123)         ? (const void *)&vc_st7123
                                                          : (const void *)&vc_st7121;
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,      /* reset is handled via the LCD_EN rail */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)vendor,
    };
    if (rev == TAB5_REV_ILI9881C_GT911) {
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9881c(s_dbi_io, &panel_cfg, &s_panel), TAG, "ILI9881C new failed");
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7123(s_dbi_io, &panel_cfg, &s_panel), TAG, "ST7123 new failed");
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    esp_lcd_panel_invert_color(s_panel, false);
    esp_lcd_panel_mirror(s_panel, false, false);

    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &s_fb), TAG, "get frame buffer");
    ESP_RETURN_ON_FALSE(s_fb, ESP_ERR_INVALID_STATE, TAG, "no frame buffer");

    /* Black before the caller turns the backlight on (avoids a white flash). */
    tab5_display_fill(0x0000);
    esp_lcd_panel_disp_on_off(s_panel, true);

    ESP_LOGI(TAG, "Display ready: %dx%d RGB565, fb=%p", TAB5_PANEL_W, TAB5_PANEL_H, s_fb);
    return ESP_OK;
}

void *tab5_display_fb(void)
{
    return s_fb;
}

size_t tab5_display_fb_size(void)
{
    return (size_t)TAB5_PANEL_W * TAB5_PANEL_H * sizeof(uint16_t);
}

void tab5_display_fill(uint16_t color)
{
    if (!s_fb) return;
    uint32_t v = ((uint32_t)color << 16) | color;
    uint32_t *p = (uint32_t *)s_fb;
    size_t n = tab5_display_fb_size() / sizeof(uint32_t);
    for (size_t i = 0; i < n; i++) p[i] = v;
    esp_cache_msync(s_fb, tab5_display_fb_size(),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

/* ─── Touch ────────────────────────────────────────────────────── */

#define GT911_REG_STATUS  0x814E
#define GT911_REG_POINTS  0x814F

static esp_lcd_panel_io_handle_t s_touch_io = NULL;
static SemaphoreHandle_t         s_touch_lock = NULL;
static uint8_t                   s_st_max_touches = 0;
static tab5_touch_point_t        s_last_pts[TAB5_MAX_TOUCH_POINTS];
static int                       s_last_cnt = 0;

esp_err_t tab5_touch_init(void)
{
    if (s_touch_io) return ESP_OK;

    ESP_RETURN_ON_ERROR(tab5_board_init(), TAG, "board init");
    tab5_revision_t rev = detect_revision();
    ESP_RETURN_ON_FALSE(rev != TAB5_REV_UNKNOWN, ESP_ERR_NOT_SUPPORTED, TAG, "unsupported Tab5 revision");

    if (!s_touch_lock) s_touch_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_touch_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    if (rev == TAB5_REV_ILI9881C_GT911) {
        /* The INT line has a pull-up to 3V3 that blocks the GT911; the BSP
         * holds it low so the controller comes up at its backup address 0x14. */
        const gpio_config_t int_cfg = {
            .mode = GPIO_MODE_OUTPUT,
            .intr_type = GPIO_INTR_DISABLE,
            .pull_up_en = 1,
            .pin_bit_mask = BIT64(TAB5_TOUCH_INT_GPIO),
        };
        gpio_config(&int_cfg);
        gpio_set_level(TAB5_TOUCH_INT_GPIO, 0);
        ESP_RETURN_ON_ERROR(touch_io_new(GT911_TOUCH_ADDR_BACKUP, &s_touch_io), TAG, "GT911 IO");
        ESP_LOGI(TAG, "GT911 touch ready");
    } else {
        ESP_RETURN_ON_ERROR(touch_io_new(ST7123_TOUCH_ADDR, &s_touch_io), TAG, "ST712x IO");
        uint8_t max = 0;
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_rx_param(s_touch_io, 0x0009, &max, 1), TAG, "ST712x info");
        s_st_max_touches = (max == 0 || max > TAB5_MAX_TOUCH_POINTS) ? TAB5_MAX_TOUCH_POINTS : max;
        ESP_LOGI(TAG, "ST712x touch ready (%u points)", s_st_max_touches);
    }
    return ESP_OK;
}

static int read_gt911(tab5_touch_point_t *pts, int max_points)
{
    uint8_t status = 0;
    if (esp_lcd_panel_io_rx_param(s_touch_io, GT911_REG_STATUS, &status, 1) != ESP_OK) return s_last_cnt;
    if (!(status & 0x80)) return -1;            /* no new report: keep previous state */

    int cnt = status & 0x0F;
    if (cnt > TAB5_MAX_TOUCH_POINTS) cnt = TAB5_MAX_TOUCH_POINTS;
    int out = 0;
    if (cnt > 0) {
        uint8_t raw[8 * TAB5_MAX_TOUCH_POINTS];
        if (esp_lcd_panel_io_rx_param(s_touch_io, GT911_REG_POINTS, raw, 8 * cnt) == ESP_OK) {
            for (int i = 0; i < cnt && out < max_points; i++) {
                const uint8_t *r = &raw[i * 8];
                pts[out].x = (uint16_t)(r[1] | (r[2] << 8));
                pts[out].y = (uint16_t)(r[3] | (r[4] << 8));
                out++;
            }
        }
    }
    uint8_t zero = 0;
    esp_lcd_panel_io_tx_param(s_touch_io, GT911_REG_STATUS, &zero, 1);   /* ack */
    return out;
}

static int read_st712x(tab5_touch_point_t *pts, int max_points)
{
    uint8_t adv = 0;
    if (esp_lcd_panel_io_rx_param(s_touch_io, 0x0010, &adv, 1) != ESP_OK) return s_last_cnt;
    if (!(adv & 0x08)) return 0;                /* with_coord clear: nothing touching */

    uint8_t raw[7 * TAB5_MAX_TOUCH_POINTS];
    if (esp_lcd_panel_io_rx_param(s_touch_io, 0x0014, raw, 7 * s_st_max_touches) != ESP_OK) return s_last_cnt;
    int out = 0;
    for (int i = 0; i < s_st_max_touches && out < max_points; i++) {
        const uint8_t *r = &raw[i * 7];
        if (!(r[0] & 0x80)) continue;           /* valid flag */
        pts[out].x = (uint16_t)(((r[0] & 0x3F) << 8) | r[1]);
        pts[out].y = (uint16_t)((r[2] << 8) | r[3]);
        out++;
    }
    return out;
}

int tab5_touch_read(tab5_touch_point_t *pts, int max_points)
{
    if (!s_touch_io || !pts || max_points <= 0) return 0;
    if (max_points > TAB5_MAX_TOUCH_POINTS) max_points = TAB5_MAX_TOUCH_POINTS;

    xSemaphoreTake(s_touch_lock, portMAX_DELAY);
    tab5_touch_point_t tmp[TAB5_MAX_TOUCH_POINTS];
    int n = (s_rev == TAB5_REV_ILI9881C_GT911) ? read_gt911(tmp, TAB5_MAX_TOUCH_POINTS)
                                                : read_st712x(tmp, TAB5_MAX_TOUCH_POINTS);
    if (n >= 0) {
        memcpy(s_last_pts, tmp, sizeof(tmp));
        s_last_cnt = n;
    }
    int out = s_last_cnt < max_points ? s_last_cnt : max_points;
    memcpy(pts, s_last_pts, out * sizeof(tab5_touch_point_t));
    xSemaphoreGive(s_touch_lock);
    return out;
}

/* ─── Battery (INA226) ─────────────────────────────────────────── */

#define INA226_ADDR        0x41
#define INA226_REG_CONFIG  0x00
#define INA226_REG_SHUNT   0x01
#define INA226_REG_BUS     0x02
/* 16 averages, 1.1 ms bus + shunt conversion, shunt+bus continuous (matches M5's demo) */
#define INA226_CONFIG      0x4527

static i2c_master_dev_handle_t s_ina = NULL;

static esp_err_t ina_read16(uint8_t reg, uint16_t *out)
{
    uint8_t b[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_ina, &reg, 1, b, 2, 50), TAG, "INA226 read");
    *out = (uint16_t)((b[0] << 8) | b[1]);
    return ESP_OK;
}

esp_err_t tab5_battery_init(void)
{
    if (s_ina) return ESP_OK;
    ESP_RETURN_ON_ERROR(tab5_board_init(), TAG, "board init");

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = INA226_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t dev = NULL;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &cfg, &dev), TAG, "INA226 add device");

    const uint8_t conf[3] = { INA226_REG_CONFIG, INA226_CONFIG >> 8, INA226_CONFIG & 0xFF };
    esp_err_t ret = i2c_master_transmit(dev, conf, sizeof(conf), 50);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "INA226 not responding (%s) - battery will read as full", esp_err_to_name(ret));
        i2c_master_bus_rm_device(dev);
        return ret;
    }
    s_ina = dev;
    ESP_LOGI(TAG, "INA226 battery monitor ready");
    return ESP_OK;
}

esp_err_t tab5_battery_read(int *mv, int *ma)
{
    ESP_RETURN_ON_FALSE(s_ina, ESP_ERR_INVALID_STATE, TAG, "battery monitor not initialised");
    uint16_t bus = 0, shunt = 0;
    ESP_RETURN_ON_ERROR(ina_read16(INA226_REG_BUS, &bus), TAG, "bus");
    ESP_RETURN_ON_ERROR(ina_read16(INA226_REG_SHUNT, &shunt), TAG, "shunt");
    if (mv) *mv = (int)bus * 5 / 4;                 /* 1.25 mV / LSB */
    if (ma) *ma = (int)(int16_t)shunt / 2;          /* 2.5 uV / LSB over 5 mOhm = 0.5 mA / LSB */
    return ESP_OK;
}

#endif /* CONFIG_BOARD_M5STACK_TAB5 */
