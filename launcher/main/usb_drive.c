/*
 * USB drive mode (M5Stack Tab5) - see usb_drive.h.
 *
 * The Tab5's USB-C port is wired to the ESP32-P4's first full-speed USB PHY (GPIO 24/25), which
 * USB-Serial-JTAG (console, flashing, tools/upload_papp.py) uses by default. The USB-OTG 1.1 controller
 * (USB_WRAP) normally sits on the second full-speed PHY (GPIO 26/27), but those pins are the ES8388's I2S
 * lines on the Tab5. The high-speed OTG controller drives the USB-A host port for gamepads, so it stays
 * where it is.
 *
 * So for USB drive mode the two full-speed PHYs are swapped (LP_SYS usb_ctrl): OTG 1.1 gets the USB-C pins
 * and runs TinyUSB's mass storage class on the raw SD card. USB-Serial-JTAG moves to the other PHY with its
 * pads switched off, so it never drives the I2S pins. The serial port is gone until USB drive mode ends.
 *
 * Only full speed (12 Mbit/s) is possible on these pins: expect roughly 0.8-1 MB/s.
 */
#include "sdkconfig.h"

#if CONFIG_BOARD_M5STACK_TAB5

#include "usb_drive.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "hal/usb_serial_jtag_ll.h"
#include "hal/usb_wrap_ll.h"
#include "soc/lp_system_struct.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tusb.h"
#include "odroid_sdcard.h"

static const char *TAG = "usb_drive";

static tinyusb_msc_storage_handle_t s_storage = NULL;
static bool s_tusb_installed = false;
static bool s_was_used = false;

/* Hand the USB-C pins (FSLS PHY 0) to USB-OTG 1.1 and park USB-Serial-JTAG, pads off, on PHY 1. */
static void phy_route_to_otg(void)
{
    usb_serial_jtag_ll_phy_enable_pad(false);   /* PC sees the serial port go away */
    vTaskDelay(pdMS_TO_TICKS(300));             /* ...long enough to notice before the drive appears */
    usb_wrap_ll_phy_select(&USB_WRAP, 0);
}

/* Back to the eFuse/default mapping: USB-Serial-JTAG on the USB-C pins. */
static void phy_route_to_serial_jtag(void)
{
    LP_SYS.usb_ctrl.sw_usb_phy_sel = 0;
    LP_SYS.usb_ctrl.sw_hw_usb_phy_sel = 0;
    usb_serial_jtag_ll_phy_enable_pad(true);
}

void usb_drive_boot_fixup(void)
{
    /* LP_SYS is not reset by a software or panic reset, so a swap made before one would still be in place. */
    if (LP_SYS.usb_ctrl.sw_hw_usb_phy_sel || !usb_serial_jtag_ll_phy_is_pad_enabled()) {
        phy_route_to_serial_jtag();
        ESP_LOGW(TAG, "USB pins were still swapped for USB drive mode; restored the serial port");
    }
}

esp_err_t usb_drive_start(void)
{
    sdmmc_card_t *card = odroid_sdcard_get_card();
    if (card == NULL) {
        ESP_LOGE(TAG, "No SD card mounted");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting USB drive mode; the serial port is off until it ends");

    /* The PC writes the card directly from now on, so the firmware must not have the FAT mounted. */
    esp_err_t ret = odroid_sdcard_detach_fs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Could not unmount the SD card (0x%x)", ret);
        return ret;
    }

    const tinyusb_msc_storage_config_t storage_cfg = {
        .medium.card = card,
        .fat_fs = {
            /* After the PC ejects the card it is mounted here again (the launcher restarts anyway). */
            .base_path = (char *)"/sd",
            .config.max_files = 5,
            .do_not_format = true,      /* never format somebody's card */
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    ret = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_storage);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MSC storage init failed (0x%x)", ret);
        return ret;
    }

    phy_route_to_otg();

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.port = TINYUSB_PORT_FULL_SPEED_0;  /* OTG 1.1, now on the USB-C pins */
    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB install failed (0x%x)", ret);
        phy_route_to_serial_jtag();
        return ret;
    }
    s_tusb_installed = true;
    s_was_used = false;
    return ESP_OK;
}

usb_drive_state_t usb_drive_state(void)
{
    tinyusb_msc_mount_point_t mp = TINYUSB_MSC_STORAGE_MOUNT_APP;
    if (s_storage) tinyusb_msc_get_storage_mount_point(s_storage, &mp);

    if (tud_mounted() && mp == TINYUSB_MSC_STORAGE_MOUNT_USB) {
        s_was_used = true;
        return USB_DRIVE_IN_USE;
    }
    return s_was_used ? USB_DRIVE_EJECTED : USB_DRIVE_WAITING;
}

void usb_drive_stop_and_restart(void)
{
    ESP_LOGI(TAG, "Leaving USB drive mode");
    if (s_tusb_installed) {
        tinyusb_driver_uninstall();             /* drops off the bus before the pins change hands */
        s_tusb_installed = false;
    }
    phy_route_to_serial_jtag();
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

#endif /* CONFIG_BOARD_M5STACK_TAB5 */
