/*
 * RetroPad protocol — shared by the ESP32-P4 host driver (tab5_ctrl) and the
 * AVR32DD28 controller-board firmware (hardware/tab5_controller/firmware_avrdd),
 * which includes this header directly.
 *
 * A RetroPad board is electrically and mechanically a drop-in replacement for
 * the M5Stack Tab5 Keyboard (SKU A164): same 2x5 Ext.Port1 header (G0 = SDA,
 * G1 = SCL), same I2C address (0x6D) and the same register map.  The RetroPad
 * firmware only ADDS the block below; every stock register (0x00-0x67,
 * 0xFD-0xFF) keeps its M5Stack meaning, so the host can talk to a stock
 * keyboard and a RetroPad with one driver.
 *
 * This is the single copy; do not fork it into the firmware tree.
 */
#ifndef RETROPAD_PROTO_H
#define RETROPAD_PROTO_H

/* ── Stock M5Stack Tab5 Keyboard registers (subset the host uses) ───────── */
#define RP_I2C_ADDR_DEFAULT     0x6D
#define RP_REG_INT_CFG          0x00
#define RP_REG_INT_STAT         0x01
#define RP_REG_EVENT_NUM        0x02
#define RP_REG_BRIGHTNESS       0x03
#define RP_REG_KB_MODE          0x10    /* 0 = Normal (row/col events) */
#define RP_REG_KEY_EVENT        0x20    /* [7]=press [6:4]=row [3:0]=col, 0xFF=empty */
#define RP_REG_FW_VERSION       0xFE

/* ── RetroPad extension block (read-only, one contiguous burst) ─────────── */
#define RP_REG_INFO             0x70    /* 8 bytes, see below */
#define RP_REG_BUTTONS          0x80    /* uint32 little-endian, live debounced */
#define RP_REG_ANALOG           0x84    /* 4 x uint8 (AN0, AN1, reserved, reserved) */
#define RP_REG_EXT_END          0x87
#define RP_EXT_BLOCK_LEN        (RP_REG_EXT_END - RP_REG_INFO + 1)  /* 24 */

/* INFO layout (offsets from RP_REG_INFO) */
#define RP_INFO_MAGIC0          0       /* 'R' */
#define RP_INFO_MAGIC1          1       /* 'P' */
#define RP_INFO_MAGIC2          2       /* 'A' */
#define RP_INFO_MAGIC3          3       /* 'D' */
#define RP_INFO_PROTO_VER       4       /* RP_PROTO_VERSION */
#define RP_INFO_CONSOLE_ID      5       /* rp_console_t, from the ID straps */
#define RP_INFO_ANALOG_COUNT    6       /* 0 or 2 (AN-fitted strap) */
#define RP_INFO_FLAGS           7       /* reserved, 0 */

#define RP_PROTO_VERSION        1

/* ── Console IDs (4-bit strap value on the board) ───────────────────────── */
typedef enum {
    RP_CONSOLE_GENERIC   = 0,   /* no straps fitted */
    RP_CONSOLE_NES       = 1,   /* the shared NES / Game Boy board */
    RP_CONSOLE_GB        = 2,   /* Game Boy / Color; mapped the same as NES */
    RP_CONSOLE_SNES      = 3,
    RP_CONSOLE_SMS       = 4,   /* Master System / Game Gear */
    RP_CONSOLE_GENESIS   = 5,
    RP_CONSOLE_PCE       = 6,
    RP_CONSOLE_A2600     = 7,
    RP_CONSOLE_A7800     = 8,
    RP_CONSOLE_LYNX      = 9,
    RP_CONSOLE_A5200     = 10,  /* 800XL / 5200 */
    RP_CONSOLE_COLECO    = 11,
    RP_CONSOLE_NEOGEO    = 12,
    RP_CONSOLE_SPECTRUM  = 13,
    RP_CONSOLE_RESERVED  = 14,
    RP_CONSOLE_KEYBOARD  = 15,  /* never strapped: host-side id for a stock M5 keyboard */
    RP_CONSOLE_COUNT
} rp_console_t;

/* ── Canonical button bits (RP_REG_BUTTONS) ─────────────────────────────── *
 * The bit meaning is fixed across boards; the console id only says which
 * subset is populated and how the host should map it for the running
 * emulator.  The board firmware translates its per-console pin wiring
 * (firmware_avrdd/pinmap.h, one GPIO per button) into these bits.          */
enum {
    RP_BTN_UP = 0,  RP_BTN_DOWN,   RP_BTN_LEFT,  RP_BTN_RIGHT,
    RP_BTN_A,       RP_BTN_B,      RP_BTN_C,     RP_BTN_X,
    RP_BTN_Y,       RP_BTN_Z,      RP_BTN_L,     RP_BTN_R,
    RP_BTN_L2,      RP_BTN_R2,     RP_BTN_START, RP_BTN_SELECT,
    RP_BTN_MENU,    RP_BTN_VOLUME, RP_BTN_OPT1,  RP_BTN_OPT2,
    RP_BTN_KP1,     RP_BTN_KP2,    RP_BTN_KP3,   RP_BTN_KP4,
    RP_BTN_KP5,     RP_BTN_KP6,    RP_BTN_KP7,   RP_BTN_KP8,
    RP_BTN_KP9,     RP_BTN_KPSTAR, RP_BTN_KP0,   RP_BTN_KPHASH,
    RP_BTN_COUNT
};

#define RP_BIT(b)               (1UL << (b))

#endif /* RETROPAD_PROTO_H */
