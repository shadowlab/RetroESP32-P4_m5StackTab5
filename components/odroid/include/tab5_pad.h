/*
 * On-screen touch pad for the M5Stack Tab5 (no physical buttons).
 *
 * The pad lives in the two black side bars next to the picture (landscape left/right,
 * TAB5_PAD_BAR_W px each) and is drawn straight into the DSI frame buffer - it is part of
 * the "background" that survives every frame because the emulator/UI image never covers
 * the bars.  It is redrawn after each frame-buffer clear via tab5_display_set_fill_hook().
 *
 *   left bar : L, MENU, D-pad, SELECT          right bar: R, VOL, Y X, A, B, START
 */
#pragma once

#include "sdkconfig.h"
#include <stdbool.h>
#include "odroid_input.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_BOARD_M5STACK_TAB5

/** Register the redraw hook and start the touch poll task (idempotent). Call once the display is up. */
void tab5_pad_init(void);

/** OR the currently-touched pad buttons into `state` (cached by a ~125 Hz background poll task; no I2C here). */
void tab5_pad_read(odroid_gamepad_state *state);

#endif

#ifdef __cplusplus
}
#endif
