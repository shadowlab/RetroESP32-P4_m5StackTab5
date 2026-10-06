/*
Gwenesis : Genesis & megadrive Emulator.

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.
This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
You should have received a copy of the GNU General Public License along with
this program. If not, see <http://www.gnu.org/licenses/>.

__author__ = "bzhxx"
__contact__ = "https://github.com/bzhxx"
__license__ = "GPLv3"

*/
#ifndef _gwenesis_io_H_
#define _gwenesis_io_H_

#pragma once

/* Pad type per port. 3-button is the default; a 6-button pad answers the
 * TH-counter sequence games use to detect it and to read X/Y/Z/MODE. */
enum gwenesis_pad_type {
    GWENESIS_PAD_3BUTTON = 0,
    GWENESIS_PAD_6BUTTON = 1,
};

/* Extra 6-button pad buttons, numbered after PAD_S (gwenesis_bus.h). */
enum gwenesis_pad_button_ext {
    PAD_Z = 8,
    PAD_Y,
    PAD_X,
    PAD_MODE,
};

void gwenesis_io_pad_press_button(int pad, int button);
void gwenesis_io_pad_release_button(int pad, int button);
void gwenesis_io_set_pad_type(int pad, int type);

/* Call once per frame with the master-clock cycles the frame consumed, after
 * the M68K cycle counter has been rebased (keeps the 6-button timeout exact
 * across frame boundaries). */
void gwenesis_io_frame_end(int system_clock);

void gwenesis_io_write_ctrl(unsigned int address, unsigned int value);
unsigned int gwenesis_io_read_ctrl(unsigned int address);

void gwenesis_io_set_reg(unsigned int reg, unsigned int value);
void gwenesis_io_get_buttons();

void gwenesis_io_save_state();
void gwenesis_io_load_state();

#endif