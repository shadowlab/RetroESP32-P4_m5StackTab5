/*
 * Tab5 keyboard-port controller boards.
 *
 * Talks to whatever is plugged into the M5Stack Tab5 keyboard port:
 *   - a RetroPad console board (reports its console id and a live button mask)
 *   - the stock M5Stack Tab5 Keyboard (key events folded into the same mask)
 * A background task polls the board and re-probes it, so boards can be
 * swapped while the system is running.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "retropad_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool         present;       /**< a board answered at the configured address */
    bool         retropad;      /**< it carries the RetroPad extension block */
    rp_console_t console;       /**< RP_CONSOLE_KEYBOARD for a stock keyboard */
    uint8_t      fw_version;    /**< stock FIRMWARE_VERSION register */
    uint8_t      analog_count;  /**< 0 or 2 */
} tab5_ctrl_info_t;

/**
 * @brief Create the I2C bus, probe the port and start the poll task.
 *        Safe to call more than once. Returns ESP_ERR_NOT_SUPPORTED when
 *        CONFIG_TAB5_CTRL_ENABLE is off; an empty port is not an error.
 */
esp_err_t tab5_ctrl_init(void);

/** @brief Snapshot of what is currently plugged in. */
void tab5_ctrl_get_info(tab5_ctrl_info_t *info);

/** @brief Live canonical button mask (RP_BIT(RP_BTN_*)), 0 when nothing is attached. */
uint32_t tab5_ctrl_get_buttons(void);

/** @brief Analog channel 0..1 (0-255), or -1 when the board has none. */
int tab5_ctrl_get_analog(int channel);

/** @brief Human-readable console name ("NES", "Genesis", ...). */
const char *tab5_ctrl_console_name(rp_console_t console);

#ifdef __cplusplus
}
#endif
