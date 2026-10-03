/*
 * Odroid System Compatibility Layer — ESP32-P4 Implementation
 *
 * Initializes all ESP32-P4 hardware:
 *   I2C → PPA → USB Gamepad → Audio → LCD → Touch
 *
 * Also provides OTA application switching stubs.
 */

#include "odroid_system.h"
#include "pins_config.h"
#ifdef CONFIG_BOARD_M5STACK_TAB5
#include "tab5_board.h"
#endif
#ifndef CONFIG_HDMI_OUTPUT
#include "st7701_lcd.h"
#include "gt911_touch.h"
#endif
#include "ppa_engine.h"
#include "gamepad.h"
#include "audio.h"

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "odroid_system";
static bool s_initialized = false;
static i2c_master_bus_handle_t s_i2c_handle = NULL;

/* ─── I2C master bus (shared by touch + audio codec) ─────────── */
static void init_i2c(void)
{
#ifdef CONFIG_BOARD_M5STACK_TAB5
    /* Tab5: tab5_board owns the system I2C bus (also used by the IO expanders). */
    ESP_ERROR_CHECK(tab5_board_init());
    s_i2c_handle = tab5_board_i2c_bus();
    ESP_LOGI(TAG, "I2C master bus initialized (Tab5 system bus)");
    return;
#endif
    i2c_master_bus_config_t i2c_bus_conf = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = (gpio_num_t)TP_I2C_SDA,
        .scl_io_num = (gpio_num_t)TP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = { .enable_internal_pullup = 1, },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_conf, &s_i2c_handle));
    ESP_LOGI(TAG, "I2C master bus initialized (SDA=%d, SCL=%d)", TP_I2C_SDA, TP_I2C_SCL);
}

/* ─── System Init ─────────────────────────────────────────────── */
void odroid_system_init(void)
{
    if (s_initialized) return;

    ESP_LOGI(TAG, "=== RetroESP32-P4 System Init ===");

    /* 1. I2C bus */
    init_i2c();

    /* 2. PPA 2D Engine */
    ESP_LOGI(TAG, "Initializing PPA 2D engine...");
    ESP_ERROR_CHECK(ppa_engine_init());

    /* 3. USB HID Gamepad */
    ESP_LOGI(TAG, "Initializing USB HID gamepad host...");
#ifdef CONFIG_BOARD_M5STACK_TAB5
    /* USB-A port 5V is switched by an IO expander */
    if (tab5_board_usb_power_enable(true) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable USB-A power rail");
    }
#endif
    gamepad_config_t gp_cfg = GAMEPAD_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(gamepad_init(&gp_cfg));

    /* 4. Audio (ES8311 codec) */
#ifdef CONFIG_BOARD_M5STACK_TAB5
    ESP_LOGI(TAG, "Initializing audio (ES8388 codec)...");
    if (tab5_board_speaker_enable(true) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable speaker amplifier");
    }
#else
    ESP_LOGI(TAG, "Initializing audio (ES8311 codec)...");
#endif
    audio_config_t audio_cfg = AUDIO_CONFIG_DEFAULT();
    audio_cfg.mclk_io    = I2S_MCLK_IO;
    audio_cfg.bclk_io    = I2S_BCLK_IO;
    audio_cfg.ws_io      = I2S_WS_IO;
    audio_cfg.dout_io    = I2S_DOUT_IO;
    audio_cfg.din_io     = I2S_DIN_IO;
    audio_cfg.pa_ctrl_io = AUDIO_PA_IO;
    audio_cfg.i2c_handle = s_i2c_handle;
    audio_cfg.sample_rate = 16000;
    audio_cfg.volume     = 60;
    esp_err_t audio_ret = audio_init(&audio_cfg);
    if (audio_ret != ESP_OK) {
        ESP_LOGW(TAG, "Audio init failed (0x%x), continuing without audio", audio_ret);
    }

    /* 5. LCD / HDMI display */
#ifndef CONFIG_HDMI_OUTPUT
#ifdef CONFIG_BOARD_M5STACK_TAB5
    ESP_LOGI(TAG, "Initializing LCD (Tab5 MIPI DSI, auto-detected panel)...");
#else
    ESP_LOGI(TAG, "Initializing LCD (ST7701 MIPI DSI)...");
#endif
    ESP_ERROR_CHECK(st7701_lcd_init());

    /* 6. Touch (GT911 / ST712x on the Tab5) */
    ESP_LOGI(TAG, "Initializing touch...");
    ESP_ERROR_CHECK(gt911_touch_init(TP_I2C_SDA, TP_I2C_SCL, TP_RST, TP_INT));

    /* 7. Clear physical LCD to black */
    st7701_lcd_fill_screen(0x0000);
    vTaskDelay(pdMS_TO_TICKS(100));
#else
    /* HDMI: display init happens in ili9341_init() via hdmi_display_init() */
    ESP_LOGI(TAG, "HDMI mode — LCD and touch skipped (HDMI init in ili9341_init)");
#endif

    s_initialized = true;
    ESP_LOGI(TAG, "=== System Init Complete ===");
}

/* ─── I2C bus accessor ────────────────────────────────────────── */
void *odroid_system_get_i2c_bus(void)
{
    return (void *)s_i2c_handle;
}

/* ─── OTA Application Switching (stub) ────────────────────────── */
void odroid_system_application_set(int slot)
{
    /*
     * On the original Odroid Go, this sets an OTA partition slot
     * so the next esp_restart() boots the specified emulator.
     *
     * On the ESP32-P4, emulators will be loaded differently (TBD).
     * For now, just log the request.
     */
    ESP_LOGW(TAG, "Application set to slot %d (stub — emulator launch not yet implemented)", slot);
}

void odroid_system_sleep(void)
{
    ESP_LOGW(TAG, "odroid_system_sleep() called — no deep sleep on P4, doing nothing");
}

void odroid_system_led_set(int value)
{
    (void)value;
    /* No indicator LED on P4 Guiton board */
}
