/*
 * HDMI output through the M5Stack Module Display 13.2 (M-Bus module on the Tab5).
 *
 * The module has an FPGA with its own frame buffer and an LT8618 HDMI transmitter.  The firmware
 * sends it a native-resolution RGB565 frame over SPI and the FPGA scales it up by an integer
 * factor (so a NES frame goes out as 3x, a Game Boy frame as 5x, on a 1280x720 signal).  Audio goes
 * to the module as I2S and the transmitter embeds it in the HDMI stream.
 *
 * Everything here is optional: without the module (or without the FPGA image on the SD card, when
 * the FPGA has not been loaded yet) hdmi_out_start() logs a line and the other calls do nothing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Detect the module and bring it up in a background task (FPGA load takes ~2 s the first time
 * after power-up).  Safe to call more than once.  Needs the SD card mounted: the FPGA image is read
 * from /sd/system/hdmi_fpga.bin when the FPGA is not configured yet.
 *
 * @param i2c  the system I2C master bus (SDA 31 / SCL 32), shared with the touch and codec
 */
void hdmi_out_start(i2c_master_bus_handle_t i2c);

/** true once the module is up and accepting frames / audio */
bool hdmi_out_ready(void);

/**
 * Queue a frame for the module.  Returns immediately: the frame is copied, and a newer frame
 * replaces one that has not been sent yet, so the caller is never paced by the HDMI link.
 *
 * @param src          RGB565 pixels, w*h
 * @param src_is_be    true when each pixel is stored high byte first (the "byte-swapped" LCD layout)
 */
void hdmi_out_present(const uint16_t *src, uint32_t w, uint32_t h, bool src_is_be);

/** true when audio should go to HDMI instead of the Tab5 speaker */
bool hdmi_out_audio_active(void);

/** Play 16-bit stereo PCM on HDMI (resampled to 48 kHz).  Blocks at the audio rate. */
void hdmi_out_audio_write(const int16_t *stereo, int frames, int sample_rate);

#ifdef __cplusplus
}
#endif
