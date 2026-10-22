/*
 * Odroid SD Card Compatibility Layer — ESP32-P4 Implementation
 *
 * Mounts the SD card via SDMMC 4-bit mode with on-chip LDO power control.
 * Uses the same GPIO assignments as the ESP32-P4 base project.
 */

#include "sdkconfig.h"
#include "odroid_sdcard.h"
#include "pins_config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_idf_version.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#ifdef CONFIG_BOARD_M5STACK_TAB5
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#define SDCARD_SPI_HOST SPI3_HOST
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "odroid_sdcard";
static bool s_mounted = false;

/*
 * The P4 has one SDMMC controller with two slots: the SD card is on slot 0, and ESP-Hosted (Wi-Fi via the
 * ESP32-C6, slot 1) also brings the controller up. On ESP-IDF 6 the controller must be initialised only once
 * when the two share it (esp-idf#16233; ESP-Hosted's mcu_hosted_sdio_sdmmc_combined example works around it
 * the same way): initialise it here explicitly - a no-op if ESP-Hosted or an earlier attempt already did -
 * and give the mount a no-op init so it doesn't do it again. Unmount / a failed mount still only remove
 * slot 0 (SDMMC_HOST_FLAG_DEINIT_ARG -> sdmmc_host_deinit_slot), so the C6's slot keeps running.
 */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define SDCARD_SHARED_HOST_INIT 1
static esp_err_t sdmmc_host_init_noop(void)
{
    return ESP_OK;
}
#else
#define SDCARD_SHARED_HOST_INIT 0
#endif

/* Global SD card base path — referenced by emulator components */
const char* SD_BASE_PATH = "/sd";

esp_err_t odroid_sdcard_open(const char* base_path)
{
    if (s_mounted) {
        ESP_LOGW(TAG, "SD card already mounted");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing SD card (SDMMC mode with on-chip LDO)");

    /* Create on-chip LDO power control (channel 4, 3.3 V) */
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = 4,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    esp_err_t ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create SD power control LDO (0x%x)", ret);
        return ret;
    }

    /*
     * Mount attempts, from fastest to most forgiving.  If 4-bit high speed gets no answer from the card
     * (e.g. "send_op_cond returned 0x107", seen on a Tab5), slow down, drop to 1-bit and finally try SPI
     * mode on the same pins.  Each attempt power-cycles the card through the LDO first, and the log
     * names the mode that worked.
     */
    static const struct {
        const char *name;
        int width;              /* 4 or 1: SDMMC bus width; 0: SPI mode */
        int freq_khz;
    } modes[] = {
        { "SDMMC 4-bit, 40 MHz", 4, SDMMC_FREQ_HIGHSPEED },
        { "SDMMC 4-bit, 20 MHz", 4, SDMMC_FREQ_DEFAULT },
        { "SDMMC 1-bit, 20 MHz", 1, SDMMC_FREQ_DEFAULT },
        { "SDMMC 1-bit, 400 kHz", 1, SDMMC_FREQ_PROBING },
#ifdef CONFIG_BOARD_M5STACK_TAB5
        { "SPI, 20 MHz", 0, SDMMC_FREQ_DEFAULT },
#endif
    };
    const int max_attempts = sizeof(modes) / sizeof(modes[0]);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 10,
        .allocation_unit_size = 16 * 1024
    };

    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        const int width = modes[attempt - 1].width;
        ESP_LOGI(TAG, "SD mount attempt %d/%d: %s", attempt, max_attempts, modes[attempt - 1].name);

        sdmmc_card_t *card = NULL;
        if (width > 0) {
            sdmmc_host_t host = SDMMC_HOST_DEFAULT();
            host.slot = SDMMC_HOST_SLOT_0;
            host.max_freq_khz = modes[attempt - 1].freq_khz;
            host.pwr_ctrl_handle = pwr_ctrl_handle;

            sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
            slot_config.width = width;
            slot_config.clk = (gpio_num_t)SD_MMC_CLK;
            slot_config.cmd = (gpio_num_t)SD_MMC_CMD;
            slot_config.d0  = (gpio_num_t)SD_MMC_D0;
            if (width == 4) {
                slot_config.d1 = (gpio_num_t)SD_MMC_D1;
                slot_config.d2 = (gpio_num_t)SD_MMC_D2;
                slot_config.d3 = (gpio_num_t)SD_MMC_D3;
            } else {
                /* D3 is not part of a 1-bit bus, but a card that sees it low during CMD0 switches
                 * to SPI mode and then ignores SDMMC until it loses power: keep it pulled up. */
                gpio_reset_pin((gpio_num_t)SD_MMC_D3);
                gpio_set_direction((gpio_num_t)SD_MMC_D3, GPIO_MODE_INPUT);
                gpio_pullup_en((gpio_num_t)SD_MMC_D3);
            }
            slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

#if SDCARD_SHARED_HOST_INIT
            ret = sdmmc_host_init();
            if (ret == ESP_OK) {
                host.init = &sdmmc_host_init_noop;
                ret = esp_vfs_fat_sdmmc_mount(base_path, &host, &slot_config,
                                               &mount_config, &card);
            } else {
                ESP_LOGW(TAG, "SDMMC host init failed (0x%x)", ret);
            }
#else
            ret = esp_vfs_fat_sdmmc_mount(base_path, &host, &slot_config,
                                           &mount_config, &card);
#endif
        }
#ifdef CONFIG_BOARD_M5STACK_TAB5
        else {
            /* SPI mode on the SD pins: CLK = CLK, MOSI = CMD, MISO = D0, CS = D3 (as in esp-bsp) */
            sdmmc_host_t host = SDSPI_HOST_DEFAULT();
            host.slot = SDCARD_SPI_HOST;
            host.max_freq_khz = modes[attempt - 1].freq_khz;
            host.pwr_ctrl_handle = pwr_ctrl_handle;
            const spi_bus_config_t bus_cfg = {
                .sclk_io_num = SD_MMC_CLK,
                .mosi_io_num = SD_MMC_CMD,
                .miso_io_num = SD_MMC_D0,
                .quadwp_io_num = -1,
                .quadhd_io_num = -1,
                .max_transfer_sz = 4000,
            };
            ret = spi_bus_initialize(SDCARD_SPI_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
            if (ret == ESP_OK) {
                sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
                dev_cfg.gpio_cs = SD_MMC_D3;
                dev_cfg.host_id = SDCARD_SPI_HOST;
                ret = esp_vfs_fat_sdspi_mount(base_path, &host, &dev_cfg, &mount_config, &card);
                if (ret != ESP_OK) spi_bus_free(SDCARD_SPI_HOST);
            } else {
                ESP_LOGW(TAG, "SPI bus init failed (0x%x)", ret);
            }
        }
#endif
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SD Card mounted at %s (attempt %d, %s)", base_path, attempt, modes[attempt - 1].name);
            sdmmc_card_print_info(stdout, card);
            s_mounted = true;
            return ESP_OK;
        }

        ESP_LOGW(TAG, "SD mount failed (0x%x, %s) on attempt %d", ret, esp_err_to_name(ret), attempt);

        if (attempt < max_attempts) {
            ESP_LOGI(TAG, "Power-cycling SD card via LDO...");
            sd_pwr_ctrl_del_on_chip_ldo(pwr_ctrl_handle);
            pwr_ctrl_handle = NULL;
            vTaskDelay(pdMS_TO_TICKS(500));
            ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to recreate LDO (0x%x)", ret);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    ESP_LOGE(TAG, "Failed to mount SD card after %d attempts", max_attempts);
    return ESP_FAIL;
}

bool odroid_sdcard_is_mounted(void)
{
    return s_mounted;
}

esp_err_t odroid_sdcard_close(void)
{
    if (!s_mounted) return ESP_OK;
    /* Note: on P4, we keep the SD card mounted persistently */
    ESP_LOGI(TAG, "odroid_sdcard_close() called (keeping mounted for P4)");
    return ESP_OK;
}

size_t odroid_sdcard_get_filesize(const char* path)
{
    if (!path) return 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fclose(f);
    return size;
}

size_t odroid_sdcard_copy_file_to_memory(const char* path, void* ptr)
{
    if (!path || !ptr) return 0;

    FILE* f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return 0;
    }

    fseek(f, 0, SEEK_END);
    size_t filesize = ftell(f);
    fseek(f, 0, SEEK_SET);

    size_t total = 0;
    const size_t block = 4096;
    while (total < filesize) {
        size_t to_read = filesize - total;
        if (to_read > block) to_read = block;
        size_t read = fread((uint8_t*)ptr + total, 1, to_read, f);
        if (read == 0) break;
        total += read;
    }

    fclose(f);
    ESP_LOGI(TAG, "Copied %d bytes from %s", (int)total, path);
    return total;
}

char* odroid_sdcard_create_savefile_path(const char* base_path, const char* fileName)
{
    if (!base_path || !fileName) return NULL;

    /* Create path: <base_path>/odroid/data/<fileName> */
    size_t len = strlen(base_path) + strlen("/odroid/data/") + strlen(fileName) + 1;
    char* path = malloc(len);
    if (!path) return NULL;

    snprintf(path, len, "%s/odroid/data/%s", base_path, fileName);
    return path;
}
