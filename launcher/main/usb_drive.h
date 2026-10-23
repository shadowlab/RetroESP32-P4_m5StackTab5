/*
 * USB drive mode (M5Stack Tab5): the SD card is offered to a PC over the USB-C port as a USB mass
 * storage device, so ROMs and saves can be copied without taking the card out.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USB_DRIVE_WAITING,      /* exposed, no PC has configured the device yet */
    USB_DRIVE_IN_USE,       /* a PC has the card */
    USB_DRIVE_EJECTED,      /* the PC ejected the card (or the cable was pulled): safe to leave */
} usb_drive_state_t;

/* Call once early at boot. Undoes the USB pin swap if the board restarted (crash, reset button) while in
 * USB drive mode, so the serial port comes back. */
void usb_drive_boot_fixup(void);

/* Take the file system off the SD card and offer the card on the USB-C port. The serial port
 * (USB-Serial-JTAG) is off while this runs: both use the same USB pins. */
esp_err_t usb_drive_start(void);

usb_drive_state_t usb_drive_state(void);

/* Leave USB drive mode: give the USB pins back to the serial port and restart, which mounts the card again
 * and re-reads the ROM lists. */
void usb_drive_stop_and_restart(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
