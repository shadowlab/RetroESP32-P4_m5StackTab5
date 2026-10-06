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
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "gwenesis_io.h"
#include "gwenesis_savestate.h"
#include "m68k.h"

unsigned char button_state[3]= {0xff,0xff,0xff};

/* 6-button pads: X/Y/Z/MODE (active low, bit 0 = Z, 1 = Y, 2 = X, 3 = MODE).
 * Kept apart from button_state so the save-state layout is unchanged. */
static unsigned char button_state_ext[3] = {0xff, 0xff, 0xff};
static unsigned char pad_type[3];          /* enum gwenesis_pad_type */
static unsigned char pad_th_falls[3];      /* TH 1->0 edges since the last timeout */
static int pad_th_time[3];                 /* master clock of the last TH edge */

/* The pad's counter resets ~1.5 ms after the last TH edge (master clock). */
#define SIX_BUTTON_TIMEOUT 80000

/* Button mapping 
    7 6 5 4 3 2 1 0
Start A B C R L D U 
*/
static unsigned char gwenesis_io_pad_state[3] = {0x33,0x33,0x33};

#define GWENESIS_IO_VERSION 0x81 /* oversea NTSC model version 81 */
/*
$A10003	:	MODE 	VMOD 	DISK 	RSV 	VER3 	VER2 	VER1 	VER0
MODE (R) 	0: Domestic Model
  	        1: Overseas Model
VMOD (R) 	0: NTSC CPU clock 7.67 MHz
  	        1: PAL CPU clock 7.60 MHz
RSV (R) 	Currently not used
VER3-0 (R)	MEGA DRIVE version is indicated by $0-$F. The present hardware version is indicated by $0. 
$A10003	: DATA 1 ( CTRL1 )
$A10005	: DATA 2 ( CTRL2 )
$A10007	: DATA 3 ( EXP )
$A10009	: CTRL 1
$A1000B	: CTRL 2
$A1000D	: CTRL 3
$A1000F	: TxDATA 1
$A10011	: RxDATA 1
$A10013	: S-CTRL 1
$A10015	: TxDATA 2
$A10017	: RxDATA 2
$A10019	: S-CTRL 2
$A1001B	: TxDATA 3
$A1001D	: RxDATA 3
$A1001F	: S-CTRL 3 

DATA shows the status of each port. The I/O direction of each bit is set by CTRL and S-CTRL.
DATA =	PD7 	PD6 	PD5 	PD4 	PD3 	PD2 	PD1 	PD0

PD7 (RW)
PD6 (RW) TH
PD5 (RW) TR
PD4 (RW) TL
PD3 (RW) RIGHT
PD2 (RW) LEFT
PD1 (RW) DOWN
PD0 (RW) UP 

TH = 0
PD7 0
PD6 TH=0
PD5 Start
PD4 A
PD3 0
PD2 0
PD1 DOWN
PD0 UP    

TH = 1
PD7 0
PD6 TH=1
PD5 B
PD4 C
PD3 RIGHT
PD2 LEFT
PD1 DOWN
PD0 UP   


CTRL designates the I/O direction of each port and the INTERRUPT CONTROL of TH.
CTRL = 	INT 	PC6 	PC5 	PC4 	PC3 	PC2 	PC1 	PC0

INT (RW)	0: TH-INT PROHIBITED
	1: TH-INT ALLOWED
PC6 (RW)	0: PD6 INPUT MODE
	1: OUTPUT MODE
PC5 (RW)	0: PD5 INPUT MODE
	1: OUTPUT MODE
PC4 (RW)	0: PD4 INPUT MODE
	1: OUTPUT MODE
PC3 (RW)	0: PD3 INPUT MODE
	1: OUTPUT MODE
PC2 (RW)	0: PD2 INPUT MODE
	1: OUTPUT MODE
PC1 (RW)	0: PD1 INPUT MODE
	1: OUTPUT MODE
PC0 (RW)	0: PD0 INPUT MODE
	1: OUTPUT MODE  

S-CTRL is for the status, etc. of each port's mode change, baud rate and SERIAL.

S-CTRL = BPS1 	BPS0 	SIN 	SOUT 	RINT 	RERR 	RRDY 	TFUL

SIN (RW) 	0: TR-PARALLEL MODE
	1: SERIAL IN
SOUT (RW)	0: TL-PARALLEL MODE
	1: SERIAL OUT
RINT (RW)	0: Rxd READY INTERRUPT PROHIBITED
	1: Rxd READY INTERRUPT ALLOWED
RERR (R) 	0:
	1: Rxd ERROR
RRDY (R) 	0:
	1: Rxd READY
TFUL (R) 	0:
	1: Txd FULL 
*/

unsigned char io_reg[16] = {GWENESIS_IO_VERSION, /* 0x1 Version */
                             0x7f, 0x7f, 0x7f,   /* 0x3-5-7 JOYPAD DATA 1 2 & EXT */
                             0x00, 0x00, 0x00,   /* 0x9-A-C JOYPAD CTRL 1 2 & EXT */
                             0xff,    0,    0,   /* PORT 1 */
                             0xff,    0,    0,   /* PORT 2 */
                             0xff,    0,    0};  /* PORT 3 */

void gwenesis_io_pad_release_button(int pad, int button)
{
    if (button >= PAD_Z)
        button_state_ext[pad] |= (1 << (button - PAD_Z));
    else
        button_state[pad] |= (1 << button);
}

void gwenesis_io_pad_press_button(int pad, int button)
{
    if (button >= PAD_Z)
        button_state_ext[pad] &= ~(1 << (button - PAD_Z));
    else
        button_state[pad] &= ~(1 << button);
}

void gwenesis_io_set_pad_type(int pad, int type)
{
    if (pad < 0 || pad > 2 || pad_type[pad] == type)
        return;
    pad_type[pad] = type;
    pad_th_falls[pad] = 0;
}

void gwenesis_io_frame_end(int system_clock)
{
    for (int pad = 0; pad < 3; pad++)
        pad_th_time[pad] -= system_clock;
}

/* Forget the TH sequence once the pad has been idle past its timeout. A
 * negative age means the frame clock was rebased without frame_end(). */
static inline void six_button_timeout(int pad)
{
    int age = m68k_cycles_master() - pad_th_time[pad];
    if (age < 0 || age > SIX_BUTTON_TIMEOUT)
        pad_th_falls[pad] = 0;
}

static inline void gwenesis_io_pad_write(int pad, int value)
{
    unsigned char mask = io_reg[pad + 4];
    unsigned char old_th = gwenesis_io_pad_state[pad] & 0x40;

     gwenesis_io_pad_state[pad] &= ~mask;
    gwenesis_io_pad_state[pad] |= value & mask;

    if (pad_type[pad] == GWENESIS_PAD_6BUTTON && old_th != (gwenesis_io_pad_state[pad] & 0x40)) {
        six_button_timeout(pad);
        if (old_th && pad_th_falls[pad] < 7)
            pad_th_falls[pad]++;              /* TH 1 -> 0 */
        pad_th_time[pad] = m68k_cycles_master();
    }
}

static inline  unsigned char gwenesis_io_pad_read(int pad)
{
    unsigned char value;
 
    /* get host button */
    gwenesis_io_get_buttons();

    value = gwenesis_io_pad_state[pad] & 0x40;
    value |= 0x3f;

    /* 6-button pad, counting TH 1->0 edges since the counter last reset:
     *   TH=0 after the 3rd edge:  ?0SA0000   (low nibble 0 identifies the pad)
     *   TH=1 after the 3rd edge:  ?1CBMXYZ
     *   TH=0 after the 4th edge:  ?0SA1111
     *   otherwise the 3-button responses below. */
    if (pad_type[pad] == GWENESIS_PAD_6BUTTON)
    {
        six_button_timeout(pad);
        unsigned char falls = pad_th_falls[pad];
        if ((value & 0x40) && falls == 3)
            return value & ((button_state[pad] & 0x30) | (button_state_ext[pad] & 0x0f));
        if (!(value & 0x40) && falls == 3)
            return value & ((button_state[pad] >> 2) & 0x30);
        if (!(value & 0x40) && falls == 4)
            return value & (((button_state[pad] >> 2) & 0x30) | 0x0f);
    }

    if (value & 0x40)
    {
        value &= (button_state[pad] & 0x3f);
    }
    else
    {
        value &= ((button_state[pad] & 3) | ((button_state[pad] >> 2) & 0x30));
    }
    return value;
}

void gwenesis_io_write_ctrl(unsigned int address, unsigned int value)
{

    address >>= 1;
 
    // JOYPAD DATA
    if (address >= 0x1 && address <= 0x3)
    {

        io_reg[address] = value;
        gwenesis_io_pad_write(address - 1, value);
        return;
    }
    // JOYPAD CTRL
    else if (address >= 0x4 && address <= 0x6)
    {
        if (io_reg[address] != value)
        {
            io_reg[address] = value;
            gwenesis_io_pad_write(address - 4, io_reg[address - 3]);
        }
        return;
    }

    return;
}

unsigned int gwenesis_io_read_ctrl(unsigned int address)
{
    address >>= 1;
    if (address >= 0x1 && address < 0x4)
    {
        unsigned char mask = 0x80 | io_reg[address + 3];
        unsigned char value;
        value = io_reg[address] & mask;
        value |= gwenesis_io_pad_read(address - 1) & ~mask;

        return value;
    }
    else
    {
        return io_reg[address];
    }
}

void gwenesis_io_set_reg(unsigned int reg, unsigned int value) {
    io_reg[reg] = value;
    return;
}

void gwenesis_io_save_state() {
    SaveState* state;
    state = saveGwenesisStateOpenForWrite("io");
    saveGwenesisStateSetBuffer(state, "button_state", button_state, sizeof(button_state));
    saveGwenesisStateSetBuffer(state, "gwenesis_io_pad_state", gwenesis_io_pad_state, sizeof(gwenesis_io_pad_state));
    saveGwenesisStateSetBuffer(state, "io_reg", io_reg, sizeof(io_reg));
}

void gwenesis_io_load_state() {
    SaveState* state = saveGwenesisStateOpenForRead("io");
    saveGwenesisStateGetBuffer(state, "button_state", button_state, sizeof(button_state));
    saveGwenesisStateGetBuffer(state, "gwenesis_io_pad_state", gwenesis_io_pad_state, sizeof(gwenesis_io_pad_state));
    saveGwenesisStateGetBuffer(state, "io_reg", io_reg, sizeof(io_reg));
}
