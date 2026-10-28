/*
 * USB HID Gamepad Host — ESP32-P4
 *
 * Reads wired USB gamepads via the board's USB 2.0 Type-A host port.
 * Auto-detects PS4 DualShock 4, PS5 DualSense, and generic HID gamepads.
 *
 * Three background tasks:
 *   1. usb_lib_task   — USB host library event loop (core 0)
 *   2. HID host task  — created by hid_host_install() (core 0)
 *   3. gamepad_task   — processes queued device events (core 1)
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"
#include "gamepad.h"

static const char *TAG = "gamepad";

/* ========================= Report Format Detection ========================= */

typedef enum {
    GP_FORMAT_UNKNOWN = 0,
    GP_FORMAT_PS3,      /* [id=0x01][00][btn1][btn2][ps][00][LX][LY][RX][RY][pressure...]  49 bytes */
    GP_FORMAT_PS4,      /* [id=0x01][LX][LY][RX][RY][hat+btn1][btn2][btn3][L2][R2]... */
    GP_FORMAT_PS5,      /* [id=0x01][LX][LY][RX][RY][L2][R2][cnt][hat+btn1][btn2][btn3]... */
    GP_FORMAT_GENERIC,  /* [LX][LY][RX][RY][hat+btn][btn]... or [id][LX][LY]... */
	GP_FORMAT_DUAL_PSX_ADAPTOR, /* [id=0x01][80][80][HatX][HatY][XABY|0][Sel,Start|LX,RX][00] 8 bytes */
    GP_FORMAT_DESCRIPTOR /* fields located by parsing the HID report descriptor */
} gp_report_format_t;

/* ========================= Report Descriptor Layout ========================= */

#define GP_MAX_BUTTONS 16

typedef struct {
    uint16_t bit_off;   /* bit offset within the report, after the report ID byte */
    uint8_t  bits;      /* 0 = field not present */
    bool     is_signed;
    int32_t  lmin, lmax;
} gp_field_t;

typedef struct {
    bool       valid;
    uint8_t    report_id;   /* 0 = descriptor declares no report IDs */
    gp_field_t x, y, z, rx, ry, rz, hat;
    gp_field_t btn[GP_MAX_BUTTONS];  /* Button usage 1..16 */
    int        n_buttons;
} gp_layout_t;

static gp_layout_t s_layout;
static hid_host_device_handle_t s_layout_dev = NULL;  /* interface the layout belongs to */

/* ========================= Internal State ========================= */

static gamepad_state_t s_state;
static SemaphoreHandle_t s_mutex = NULL;
static QueueHandle_t s_event_queue = NULL;
static TaskHandle_t s_usb_task = NULL;
static TaskHandle_t s_gp_task = NULL;
static volatile bool s_running = false;
static gp_report_format_t s_format = GP_FORMAT_UNKNOWN;
static int s_detect_count = 0;  /* number of reports used for format detection */
static volatile int s_format_log_pending = 0;  /* deferred format detection log */
static volatile int s_format_log_len = 0;      /* report len for deferred log */
static volatile int s_raw_dump_count = 0;      /* how many raw dumps have been emitted */
static uint8_t s_raw_dump_buf[64];             /* buffer for deferred raw hex dump */
static volatile int s_raw_dump_len = 0;
static volatile int s_raw_dump_pending = 0;    /* latest report differs from the previous one */
#define GP_RAW_DUMP_MAX 64                     /* changed reports logged per connection */

/* ========================= Device Identity ========================= */
static uint16_t s_vid = 0;
static uint16_t s_pid = 0;


/* ========================= Event Queue ========================= */

typedef enum {
    GP_EVT_DEVICE_CONNECTED,
} gp_evt_type_t;

typedef struct {
    gp_evt_type_t type;
    hid_host_device_handle_t hid_handle;
} gp_evt_t;

/* ========================= Hat Switch → D-pad ========================= */

static uint8_t hat_to_dpad(uint8_t hat)
{
    static const uint8_t map[] = {
        GAMEPAD_DPAD_UP,                                /* 0 = N  */
        GAMEPAD_DPAD_UP   | GAMEPAD_DPAD_RIGHT,         /* 1 = NE */
        GAMEPAD_DPAD_RIGHT,                             /* 2 = E  */
        GAMEPAD_DPAD_DOWN | GAMEPAD_DPAD_RIGHT,         /* 3 = SE */
        GAMEPAD_DPAD_DOWN,                              /* 4 = S  */
        GAMEPAD_DPAD_DOWN | GAMEPAD_DPAD_LEFT,          /* 5 = SW */
        GAMEPAD_DPAD_LEFT,                              /* 6 = W  */
        GAMEPAD_DPAD_UP   | GAMEPAD_DPAD_LEFT,          /* 7 = NW */
    };
    return (hat <= 7) ? map[hat] : 0;
}

static uint8_t data_to_dpad(uint8_t xaxis, uint8_t yaxis)
{
	uint8_t hat = 0;
	if (yaxis < 64) hat |= GAMEPAD_DPAD_UP;
	else if (yaxis > 192) hat |= GAMEPAD_DPAD_DOWN;
	if (xaxis < 64) hat |= GAMEPAD_DPAD_LEFT;
	else if (xaxis > 192) hat |= GAMEPAD_DPAD_RIGHT;
	return hat;
}

/* ========================= Format Detection ========================= */

/**
 * Detect report format from the first few idle reports.
 * PS4: byte[5] is hat+buttons byte (hat in lower nibble, 0-8 when idle = 0x08 centered)
 * PS5: byte[5] is L2 trigger (0 when idle), byte[8] is hat+buttons (0x08 when idle)
 */
static gp_report_format_t detect_format(const uint8_t *data, int len)
{
	// Check for specific device signatures:
	// VID   PID	 Device
	// 0x054C 0x05C4 Sony DualShock 4 (PS4)
	// 0x054C 0x0CE6 Sony DualSense (PS5)
	// 0x0079 0x0006 PC TWIN SHOCK Gamepad/Generic USB Gamepad
	// 0x0810 0x0001 Dual PSX Adaptor

	// Step 1: Use the VID/PID combo to identify known controllers.
	if (len == 8 && s_vid == 0x0810 && s_pid == 0x0001) {
		return GP_FORMAT_DUAL_PSX_ADAPTOR;
	}

	/* Short report with ID 0x01: likely PS4/PS5 or clone (some clones use same format but generic VID/PID) */
	/* Longer report with ID 0x01 and PS4-like idle axes: likely PS4 or PS5 (some clones use same format but generic VID/PID) */


    /* Short report or no report ID → generic */
    if (len < 10 || data[0] != 0x01) {
        return GP_FORMAT_GENERIC;
    }

    /* PS3 DualShock 3: 49 bytes, id=0x01, byte[1]=0x00 (reserved),
     * axes at bytes [6..9] near 0x80, bytes [1..4] are NOT axes (buttons, usually low). */
    if (len >= 49) {
        bool ps3_axes_center = (data[6] >= 0x60 && data[6] <= 0xA0) &&
                               (data[7] >= 0x60 && data[7] <= 0xA0) &&
                               (data[8] >= 0x60 && data[8] <= 0xA0) &&
                               (data[9] >= 0x60 && data[9] <= 0xA0);
        bool ps4_axes_center = (data[1] >= 0x70 && data[1] <= 0x90) &&
                               (data[2] >= 0x70 && data[2] <= 0x90);
        if (ps3_axes_center && !ps4_axes_center && data[1] == 0x00) {
            return GP_FORMAT_PS3;
        }
    }

    /* Long report with ID 0x01: PS4 or PS5 */
    /* Check idle axes (bytes 1-4 should be near 0x80 = center) */
    bool axes_near_center = (data[1] >= 0x70 && data[1] <= 0x90) &&
                            (data[2] >= 0x70 && data[2] <= 0x90) &&
                            (data[3] >= 0x70 && data[3] <= 0x90) &&
                            (data[4] >= 0x70 && data[4] <= 0x90);

    if (axes_near_center) {
        /* Idle state pattern:
         * PS4: byte[5]=0x08 (hat centered), byte[8]=0x00 (L2 trigger)
         * PS5: byte[5]=0x00 (L2 trigger),   byte[8]=0x08 (hat centered)
         */
        if (data[5] == 0x08 && data[8] != 0x08) return GP_FORMAT_PS4;
        if (data[5] == 0x00 && data[8] == 0x08) return GP_FORMAT_PS5;
    }

    /* Non-idle heuristic: check which byte position has a valid hat (0-8) */
    uint8_t b5_hat = data[5] & 0x0F;
    uint8_t b8_hat = data[8] & 0x0F;

    if (b5_hat <= 8 && b8_hat > 8) return GP_FORMAT_PS4;
    if (b8_hat <= 8 && b5_hat > 8) return GP_FORMAT_PS5;

    /* Default: PS4 format (most common for wired USB gamepads) */
    return GP_FORMAT_PS4;
}

/* ========================= Report Descriptor Parsing ========================= */

/*
 * Minimal HID report descriptor walker: records where X/Y/Z/Rx/Ry/Rz, the hat switch and
 * Buttons 1..16 sit in the first input report that carries gamepad controls. Cheap pads
 * (e.g. SNES-style USB pads) put constant bytes and centred axes at positions the
 * fixed-offset formats above misread as held buttons, so the descriptor is the only
 * reliable source for unknown VID/PIDs.
 */
static void layout_set(gp_field_t *f, uint16_t off, uint8_t bits, int32_t lmin, int32_t lmax)
{
    /* Last occurrence wins: DragonRise SNES pads (0079:0011) declare X four times, and only
     * the last one moves (the first reads a constant 0x01, i.e. "d-pad left held"). */
    /* Some devices encode e.g. max=255 in one byte, which reads as -1 when sign-extended */
    if (lmax < lmin && bits < 32) lmax &= (int32_t)((1u << bits) - 1);
    f->bit_off = off;
    f->bits = bits;
    f->is_signed = lmin < 0;
    f->lmin = lmin;
    f->lmax = lmax;
}

static bool parse_report_descriptor(const uint8_t *d, size_t len, gp_layout_t *out)
{
    typedef struct { uint16_t page; int32_t lmin, lmax; uint32_t size, count; uint8_t id; } globals_t;
    globals_t g = {0}, stack[4];
    int sp = 0;
    uint32_t usages[GP_MAX_BUTTONS + 8];
    int n_usages = 0;
    uint32_t umin = 0, umax = 0;
    bool have_range = false;
    uint16_t offsets[256] = {0};  /* running input-report bit offset per report ID */
    int16_t chosen_id = -1;

    memset(out, 0, sizeof(*out));

    for (size_t i = 0; i < len; ) {
        uint8_t p = d[i++];
        if (p == 0xFE) {  /* long item: skip */
            if (i + 1 >= len) break;
            i += 2 + d[i];
            continue;
        }
        int sz = (p & 3) == 3 ? 4 : (p & 3);
        if (i + sz > len) break;
        uint32_t uval = 0;
        for (int b = 0; b < sz; b++) uval |= (uint32_t)d[i + b] << (8 * b);
        int32_t sval = (int32_t)uval;
        if (sz == 1) sval = (int8_t)uval;
        else if (sz == 2) sval = (int16_t)uval;
        i += sz;

        uint8_t type = (p >> 2) & 3, tag = p >> 4;
        if (type == 1) {  /* global */
            switch (tag) {
            case 0: g.page = (uint16_t)uval; break;
            case 1: g.lmin = sval; break;
            case 2: g.lmax = sval; break;
            case 7: g.size = uval; break;
            case 8: g.id = (uint8_t)uval; break;
            case 9: g.count = uval; break;
            case 10: if (sp < 4) stack[sp++] = g; break;
            case 11: if (sp > 0) g = stack[--sp]; break;
            }
        } else if (type == 2) {  /* local */
            uint32_t u = (sz == 4) ? uval : ((uint32_t)g.page << 16) | uval;
            if (tag == 0 && n_usages < (int)(sizeof(usages) / sizeof(usages[0]))) usages[n_usages++] = u;
            else if (tag == 1) { umin = u; have_range = true; }
            else if (tag == 2) { umax = u; have_range = true; }
        } else if (type == 0) {  /* main */
            if (tag == 8) {  /* Input */
                uint16_t *off = &offsets[g.id];
                bool constant = uval & 0x01;
                for (uint32_t k = 0; k < g.count && g.size > 0; k++) {
                    uint32_t u;
                    if (constant) u = 0;
                    else if (n_usages > 0) u = usages[(k < (uint32_t)n_usages) ? k : (uint32_t)n_usages - 1];
                    else if (have_range) u = (umin + k <= umax) ? umin + k : umax;
                    else u = 0;

                    uint16_t page = u >> 16, id = u & 0xFFFF;
                    gp_field_t *f = NULL;
                    if (page == 0x01) {
                        switch (id) {
                        case 0x30: f = &out->x;  break;
                        case 0x31: f = &out->y;  break;
                        case 0x32: f = &out->z;  break;
                        case 0x33: f = &out->rx; break;
                        case 0x34: f = &out->ry; break;
                        case 0x35: f = &out->rz; break;
                        case 0x39: f = &out->hat; break;
                        }
                    } else if (page == 0x09 && id >= 1 && id <= GP_MAX_BUTTONS) {
                        f = &out->btn[id - 1];
                    }
                    if (f && (chosen_id < 0 || chosen_id == g.id)) {
                        chosen_id = g.id;
                        layout_set(f, *off, (uint8_t)g.size, g.lmin, g.lmax);
                    }
                    *off += g.size;
                }
            }
            /* Input, Output, Feature, Collection, End Collection all clear locals */
            n_usages = 0;
            have_range = false;
            umin = umax = 0;
        }
    }

    out->report_id = (chosen_id > 0) ? (uint8_t)chosen_id : 0;
    for (int b = 0; b < GP_MAX_BUTTONS; b++) {
        if (out->btn[b].bits) out->n_buttons = b + 1;
    }
    out->valid = ((out->x.bits && out->y.bits) || out->hat.bits) && out->n_buttons > 0;
    return out->valid;
}

static int32_t field_read(const gp_field_t *f, const uint8_t *data, int len)
{
    uint32_t v = 0;
    for (int b = 0; b < f->bits && b < 32; b++) {
        int bit = f->bit_off + b;
        if ((bit >> 3) >= len) break;
        if (data[bit >> 3] & (1u << (bit & 7))) v |= 1u << b;
    }
    if (f->is_signed && f->bits < 32 && (v & (1u << (f->bits - 1)))) {
        v |= ~((1u << f->bits) - 1);
    }
    return (int32_t)v;
}

/* Scale a logical axis value to -128..127 (centre of a 0..255 axis lands on -1/0) */
static int16_t field_axis(const gp_field_t *f, const uint8_t *data, int len)
{
    if (!f->bits || f->lmax <= f->lmin) return 0;
    int32_t v = field_read(f, data, len);
    if (v < f->lmin) v = f->lmin;
    if (v > f->lmax) v = f->lmax;
    return (int16_t)(((int64_t)(v - f->lmin) * 255) / (f->lmax - f->lmin) - 128);
}

/* Button usage n (1-based) → GAMEPAD_BTN_*, in the common DirectInput order
 * (matches DS4 and DragonRise-style SNES pads: 1=X/Square 2=A/Cross 3=B/Circle 4=Y/Triangle). */
static const uint32_t s_desc_btn_map[GP_MAX_BUTTONS] = {
    GAMEPAD_BTN_X, GAMEPAD_BTN_A, GAMEPAD_BTN_B, GAMEPAD_BTN_Y,
    GAMEPAD_BTN_L1, GAMEPAD_BTN_R1, GAMEPAD_BTN_L2, GAMEPAD_BTN_R2,
    GAMEPAD_BTN_SELECT, GAMEPAD_BTN_START, GAMEPAD_BTN_L3, GAMEPAD_BTN_R3,
    GAMEPAD_BTN_HOME, GAMEPAD_BTN_MISC, 0, 0,
};

static void parse_with_layout(const gp_layout_t *l, const uint8_t *data, int len, gamepad_state_t *gs)
{
    gs->axis_lx = field_axis(&l->x, data, len);
    gs->axis_ly = field_axis(&l->y, data, len);
    /* Right stick: Z/Rz on DS4-style descriptors, Rx/Ry elsewhere */
    gs->axis_rx = l->z.bits  ? field_axis(&l->z, data, len)  : field_axis(&l->rx, data, len);
    gs->axis_ry = l->rz.bits ? field_axis(&l->rz, data, len) : field_axis(&l->ry, data, len);

    /* Digital-only pads report the d-pad on X/Y at the logical extremes */
    if (l->x.bits && l->y.bits) {
        if (gs->axis_ly < -64) gs->dpad |= GAMEPAD_DPAD_UP;
        if (gs->axis_ly >  64) gs->dpad |= GAMEPAD_DPAD_DOWN;
        if (gs->axis_lx < -64) gs->dpad |= GAMEPAD_DPAD_LEFT;
        if (gs->axis_lx >  64) gs->dpad |= GAMEPAD_DPAD_RIGHT;
    }
    if (l->hat.bits) {
        int32_t v = field_read(&l->hat, data, len);
        int32_t range = l->hat.lmax - l->hat.lmin;
        if (v >= l->hat.lmin && v <= l->hat.lmax) {  /* out of range = centred (null state) */
            int32_t idx = v - l->hat.lmin;
            if (range == 3) idx *= 2;  /* 4-way hat */
            gs->dpad |= hat_to_dpad((uint8_t)idx);
        }
    }
    for (int b = 0; b < l->n_buttons; b++) {
        if (l->btn[b].bits && field_read(&l->btn[b], data, len) > 0) gs->buttons |= s_desc_btn_map[b];
    }
}

/* ========================= Button Parsing Helpers ========================= */

static uint32_t parse_ps_buttons(uint8_t hat_btn_byte, uint8_t btn2_byte, uint8_t btn3_byte)
{
    uint32_t btns = 0;

    /* hat_btn_byte upper nibble: face buttons */
    if (hat_btn_byte & 0x10) btns |= GAMEPAD_BTN_X;       /* Square */
    if (hat_btn_byte & 0x20) btns |= GAMEPAD_BTN_A;       /* Cross  */
    if (hat_btn_byte & 0x40) btns |= GAMEPAD_BTN_B;       /* Circle */
    if (hat_btn_byte & 0x80) btns |= GAMEPAD_BTN_Y;       /* Triangle */

    /* btn2_byte: shoulder + system */
    if (btn2_byte & 0x01) btns |= GAMEPAD_BTN_L1;
    if (btn2_byte & 0x02) btns |= GAMEPAD_BTN_R1;
    if (btn2_byte & 0x04) btns |= GAMEPAD_BTN_L2;
    if (btn2_byte & 0x08) btns |= GAMEPAD_BTN_R2;
    if (btn2_byte & 0x10) btns |= GAMEPAD_BTN_SELECT;     /* Share/Create */
    if (btn2_byte & 0x20) btns |= GAMEPAD_BTN_START;      /* Options */
    if (btn2_byte & 0x40) btns |= GAMEPAD_BTN_L3;
    if (btn2_byte & 0x80) btns |= GAMEPAD_BTN_R3;

    /* btn3_byte: PS + touchpad */
    if (btn3_byte & 0x01) btns |= GAMEPAD_BTN_HOME;
    if (btn3_byte & 0x02) btns |= GAMEPAD_BTN_MISC;       /* Touchpad click */

    return btns;
}

/* ========================= Report Parsing ========================= */

static void parse_gamepad_report(const uint8_t *data, int len)
{
    if (len < 4) return;

    /* Log first 16 bytes of raw report at DEBUG level */
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, (len > 16) ? 16 : len, ESP_LOG_DEBUG);

    /* Descriptor layout with report IDs: ignore reports for other collections */
    if (s_format == GP_FORMAT_DESCRIPTOR && s_layout.report_id && data[0] != s_layout.report_id) return;

    /* Always capture latest raw report for gamepad_get_raw_report() API;
     * flag changed reports so gamepad_task can log them */
    {
        int copy_len = (len > 64) ? 64 : len;
        if (copy_len != s_raw_dump_len || memcmp(s_raw_dump_buf, data, copy_len) != 0) {
            s_raw_dump_pending = 1;
        }
        memcpy(s_raw_dump_buf, data, copy_len);
        s_raw_dump_len = copy_len;
    }

    /* Auto-detect format from first few reports — NO LOGGING here (USB callback context) */
    if (s_format == GP_FORMAT_UNKNOWN && s_detect_count < 5) {
        gp_report_format_t detected = detect_format(data, len);
        if (detected != GP_FORMAT_UNKNOWN) {
            s_format = detected;
            s_format_log_len = len;
            s_format_log_pending = 1;  /* defer logging to gamepad_task */
        }
        s_detect_count++;
        if (s_detect_count >= 5 && s_format == GP_FORMAT_UNKNOWN) {
            s_format = (len >= 10 && data[0] == 0x01) ? GP_FORMAT_PS4 : GP_FORMAT_GENERIC;
            s_format_log_len = len;
            s_format_log_pending = 1;
        }
        if (s_format == GP_FORMAT_UNKNOWN) return;
    }

    gamepad_state_t gs;
    memset(&gs, 0, sizeof(gs));
    gs.connected = 1;

    switch (s_format) {
    case GP_FORMAT_DESCRIPTOR: {
        int off = s_layout.report_id ? 1 : 0;
        parse_with_layout(&s_layout, data + off, len - off, &gs);
        break;
    }

    case GP_FORMAT_PS3:
        /* PS3 DualShock 3: [0x01][00][btn1][btn2][ps_btn][00][LX][LY][RX][RY]...
         * btn1 byte[2]: bit0=Sel bit1=L3 bit2=R3 bit3=Start bit4=Up bit5=Right bit6=Down bit7=Left
         * btn2 byte[3]: bit0=L2 bit1=R2 bit2=L1 bit3=R1 bit4=Tri bit5=Cir bit6=Cross bit7=Sq
         */
        if (len < 10) break;
        gs.axis_lx  = (int16_t)data[6] - 128;
        gs.axis_ly  = (int16_t)data[7] - 128;
        gs.axis_rx  = (int16_t)data[8] - 128;
        gs.axis_ry  = (int16_t)data[9] - 128;
        /* D-pad from byte 2 digital bits */
        if (data[2] & 0x10) gs.dpad |= GAMEPAD_DPAD_UP;
        if (data[2] & 0x20) gs.dpad |= GAMEPAD_DPAD_RIGHT;
        if (data[2] & 0x40) gs.dpad |= GAMEPAD_DPAD_DOWN;
        if (data[2] & 0x80) gs.dpad |= GAMEPAD_DPAD_LEFT;
        /* Face buttons from byte 3 */
        if (data[3] & 0x40) gs.buttons |= GAMEPAD_BTN_A;       /* Cross  */
        if (data[3] & 0x20) gs.buttons |= GAMEPAD_BTN_B;       /* Circle */
        if (data[3] & 0x80) gs.buttons |= GAMEPAD_BTN_X;       /* Square */
        if (data[3] & 0x10) gs.buttons |= GAMEPAD_BTN_Y;       /* Triangle */
        if (data[3] & 0x04) gs.buttons |= GAMEPAD_BTN_L1;
        if (data[3] & 0x08) gs.buttons |= GAMEPAD_BTN_R1;
        if (data[3] & 0x01) gs.buttons |= GAMEPAD_BTN_L2;
        if (data[3] & 0x02) gs.buttons |= GAMEPAD_BTN_R2;
        /* System buttons from byte 2 */
        if (data[2] & 0x01) gs.buttons |= GAMEPAD_BTN_SELECT;
        if (data[2] & 0x08) gs.buttons |= GAMEPAD_BTN_START;
        if (data[2] & 0x02) gs.buttons |= GAMEPAD_BTN_L3;
        if (data[2] & 0x04) gs.buttons |= GAMEPAD_BTN_R3;
        /* PS button from byte 4 */
        if (data[4] & 0x01) gs.buttons |= GAMEPAD_BTN_HOME;
        /* Analog triggers (if report long enough) */
        if (len >= 20) {
            gs.brake    = ((uint16_t)data[18]) << 2;   /* L2 pressure */
            gs.throttle = ((uint16_t)data[19]) << 2;   /* R2 pressure */
        }
        break;

    case GP_FORMAT_PS4:
        /* PS4: [0x01][LX][LY][RX][RY][hat+btn1][btn2][btn3][L2][R2]... */
        if (len < 10) break;
        gs.axis_lx  = (int16_t)data[1] - 128;
        gs.axis_ly  = (int16_t)data[2] - 128;
        gs.axis_rx  = (int16_t)data[3] - 128;
        gs.axis_ry  = (int16_t)data[4] - 128;
        gs.dpad     = hat_to_dpad(data[5] & 0x0F);
        gs.buttons  = parse_ps_buttons(data[5], data[6], data[7]);
        gs.brake    = ((uint16_t)data[8]) << 2;   /* L2: 0-255 → 0-1020 */
        gs.throttle = ((uint16_t)data[9]) << 2;   /* R2: 0-255 → 0-1020 */
        break;

    case GP_FORMAT_PS5:
        /* PS5: [0x01][LX][LY][RX][RY][L2][R2][cnt][hat+btn1][btn2][btn3]... */
        if (len < 11) break;
        gs.axis_lx  = (int16_t)data[1] - 128;
        gs.axis_ly  = (int16_t)data[2] - 128;
        gs.axis_rx  = (int16_t)data[3] - 128;
        gs.axis_ry  = (int16_t)data[4] - 128;
        gs.brake    = ((uint16_t)data[5]) << 2;
        gs.throttle = ((uint16_t)data[6]) << 2;
        gs.dpad     = hat_to_dpad(data[8] & 0x0F);
        gs.buttons  = parse_ps_buttons(data[8], data[9], data[10]);
        break;

	case GP_FORMAT_DUAL_PSX_ADAPTOR:
		/* [0x01][80][80][HatX][HatY][XABY|0][Sel,Start|LX,RX][00] 8 bytes */
		if (len < 8) break;
		gs.axis_lx  = (int16_t)data[1] - 128;
        gs.axis_ly  = (int16_t)data[2] - 128;
        gs.axis_rx  = (int16_t)data[3] - 128;
        gs.axis_ry  = (int16_t)data[4] - 128;
        gs.brake    = 0; 
        gs.throttle = 0;
        gs.dpad     = data_to_dpad(data[3], data[4]);
        gs.buttons  = parse_ps_buttons(data[5], data[6], data[7]);
		break;

    case GP_FORMAT_GENERIC: {
        /*
         * Generic USB gamepad layout (8-byte, NO report ID):
         *
         *   [0]=LX  [1]=LY  [2]=RX  [3]=RY  [4]=Z-axis(0x80)  [5]=hat_lo|btns_hi  [6]=btns_lo  [7]=unused
         *
         *   Byte 5 low nibble:  hat switch (0x0F = centered, 0-7 = directions)
         *   Byte 5 high nibble: bit4=X, bit5=A, bit6=B, bit7=Y
         *   Byte 6:             bit0=L1, bit1=R1, bit4=Select, bit5=Start
         *   Byte 7:             unused on this pad
         *
         *   D-pad:  mapped to Left-stick axes (byte0=X, byte1=Y):
         *           Up=LY(0x00), Down=LY(0xFF), Left=LX(0x00), Right=LX(0xFF)
         *           Center = 0x7F
         */
        int off = 0;
        if (len > 8 && data[0] >= 0x01 && data[0] <= 0x0F) {
            off = 1;  /* Skip report ID */
        }
        if (len - off < 4) break;

        gs.axis_lx = (int16_t)data[off + 0] - 128;
        gs.axis_ly = (int16_t)data[off + 1] - 128;
        if (len - off >= 4) {
            gs.axis_rx = (int16_t)data[off + 2] - 128;
            gs.axis_ry = (int16_t)data[off + 3] - 128;
        }

        /* D-pad from LX/LY axes (this gamepad uses axes, not hat, for d-pad) */
        {
            uint8_t lx = data[off + 0];
            uint8_t ly = data[off + 1];
            /* Threshold: <0x20 = min, >0xE0 = max, else center */
            if (ly < 0x20) gs.dpad |= GAMEPAD_DPAD_UP;
            if (ly > 0xE0) gs.dpad |= GAMEPAD_DPAD_DOWN;
            if (lx < 0x20) gs.dpad |= GAMEPAD_DPAD_LEFT;
            if (lx > 0xE0) gs.dpad |= GAMEPAD_DPAD_RIGHT;
        }

        /* Also check hat in case another gamepad uses it: byte 5 low nibble */
        if (len - off >= 6) {
            uint8_t hat_val = data[off + 5] & 0x0F;
            if (hat_val <= 7) {
                gs.dpad |= hat_to_dpad(hat_val);
            }
        }

        /* Face buttons: byte 5 high nibble */
        if (len - off >= 6) {
            uint8_t bh5 = data[off + 5];
            if (bh5 & 0x10) gs.buttons |= GAMEPAD_BTN_X;
            if (bh5 & 0x20) gs.buttons |= GAMEPAD_BTN_A;
            if (bh5 & 0x40) gs.buttons |= GAMEPAD_BTN_B;
            if (bh5 & 0x80) gs.buttons |= GAMEPAD_BTN_Y;
        }

        /* Shoulder + meta buttons: byte 6 */
        if (len - off >= 7) {
            uint8_t bl = data[off + 6];
            if (bl & 0x01) gs.buttons |= GAMEPAD_BTN_L1;
            if (bl & 0x02) gs.buttons |= GAMEPAD_BTN_R1;
            if (bl & 0x10) gs.buttons |= GAMEPAD_BTN_SELECT;
            if (bl & 0x20) gs.buttons |= GAMEPAD_BTN_START;
        }
        break;
    }
    default:
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(&s_state, &gs, sizeof(gs));
    xSemaphoreGive(s_mutex);
}

/* ========================= HID Host Callbacks ========================= */

/**
 * Interface-level callback: receives input reports and disconnect events
 */
static void hid_interface_cb(hid_host_device_handle_t hid_dev,
                              const hid_host_interface_event_t event,
                              void *arg)
{
    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t data[64];
        size_t data_len = 0;
        esp_err_t err = hid_host_device_get_raw_input_report_data(hid_dev, data, sizeof(data), &data_len);
        if (err == ESP_OK && data_len > 0) {
            hid_host_dev_params_t params;
            hid_host_device_get_params(hid_dev, &params);
            /* Only parse non-keyboard, non-mouse devices; with a descriptor layout, only
             * the interface it was parsed from (receivers can expose several) */
            if (params.proto != HID_PROTOCOL_KEYBOARD && params.proto != HID_PROTOCOL_MOUSE &&
                (s_format != GP_FORMAT_DESCRIPTOR || hid_dev == s_layout_dev)) {
                parse_gamepad_report(data, (int)data_len);
            }
        }
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "USB HID device disconnected");
        hid_host_device_close(hid_dev);
        /* Clear state */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        memset(&s_state, 0, sizeof(s_state));
        xSemaphoreGive(s_mutex);
        s_vid = 0;
        s_pid = 0;
        /* Reset format detection for next device */
        s_format = GP_FORMAT_UNKNOWN;
        s_detect_count = 0;
        s_layout_dev = NULL;
        s_layout.valid = false;
        s_raw_dump_count = 0;
        s_raw_dump_len = 0;
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        /* Transfer error — most likely device disconnecting.
         * Do NOT close device here! The USB Host Library will fire DEV_GONE → DISCONNECTED
         * through the normal disconnect flow once it finishes cleaning up pending URBs.
         * Closing here would race the USB core and leave interfaces unreleased. */
        ESP_LOGW(TAG, "HID transfer error — waiting for USB core disconnect flow");
        break;
    default:
        break;
    }
}

/**
 * Driver-level callback: new HID device detected → queue for gamepad_task
 */
static void hid_device_cb(hid_host_device_handle_t hid_dev,
                           const hid_host_driver_event_t event,
                           void *arg)
{
    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        gp_evt_t evt = {
            .type = GP_EVT_DEVICE_CONNECTED,
            .hid_handle = hid_dev,
        };
        if (s_event_queue) {
            xQueueSend(s_event_queue, &evt, 0);
        }
    }
}

/* ========================= USB Host Library Task ========================= */

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL3,  /* Higher priority for USB interrupt */
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB host library installed");
    xTaskNotifyGive((TaskHandle_t)arg);

    while (s_running) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
            if (!s_running) break;
        }
    }

    ESP_LOGI(TAG, "USB host library shutting down");
    vTaskDelay(pdMS_TO_TICKS(10));
    usb_host_uninstall();
    vTaskDelete(NULL);
}

/* ========================= Sony DualShock 3 Operational-Mode Enable ========================= */

/*
 * Sony DualShock 3 / Sixaxis pads (VID 0x054C, PID 0x0268) do NOT stream input reports
 * over USB after enumeration — they sit silent with all four LEDs blinking until the
 * host puts them into "operational mode" via a HID Feature request. Clone pads auto-
 * stream (and use a different VID/PID), so this is gated to the genuine Sony VID/PID and
 * every step is best-effort (a STALL is non-fatal — input arrives on the interrupt IN
 * endpoint, not EP0).
 *
 * NOTE: on the units seen so far the operational-mode enable + LED report are necessary
 * but NOT sufficient — the pad lights player LED 1 but its input subsystem stays asleep
 * until a physical button is pressed once. Nothing host-side (periodic re-send of the
 * enable/LED report, SET_IDLE) was found to wake it; those were tried and removed. So a
 * single button press after connect is expected before the pad responds.
 */

/* DS3 LED/rumble OUTPUT report (report id 0x01). byte[9] is the LED bitmask
 * (0x02 = player LED 1); the four 5-byte blocks that follow are the standard
 * per-LED on/off/duty config. Module-scope (non-const: the HID API takes a
 * non-const pointer) and only touched from gamepad_task, so no locking needed. */
static uint8_t s_ds3_led_report[] = {
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02,   /* byte[9] = LED1 */
    0xff, 0x27, 0x10, 0x00, 0x32,   /* LED4 config */
    0xff, 0x27, 0x10, 0x00, 0x32,   /* LED3 config */
    0xff, 0x27, 0x10, 0x00, 0x32,   /* LED2 config */
    0xff, 0x27, 0x10, 0x00, 0x32,   /* LED1 config */
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00
};

static void ds3_enable_operational(hid_host_device_handle_t hid_dev)
{
    /* Step 1: GET_REPORT (Feature) 0xF2 — reads the controller's Bluetooth MAC and
     * primes some genuine units to begin reporting. */
    uint8_t f2[17] = {0};
    size_t  f2_len = sizeof(f2);
    esp_err_t err = hid_class_request_get_report(hid_dev, HID_REPORT_TYPE_FEATURE,
                                                 0xF2, f2, &f2_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS3: GET_REPORT 0xF2 failed: %s (continuing)", esp_err_to_name(err));
    }

    /* Step 2: SET_REPORT (Feature) 0xF4 = 42 0C 00 00 — enable operational mode. */
    uint8_t enable[4] = {0x42, 0x0C, 0x00, 0x00};
    err = hid_class_request_set_report(hid_dev, HID_REPORT_TYPE_FEATURE,
                                       0xF4, enable, sizeof(enable));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS3: SET_REPORT 0xF4 (enable) failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "DS3: operational-mode enable sent (0xF4 42 0C 00 00)");
    }

    /* Step 3: SET_REPORT (Output) report 0x01 — LED/rumble control report.
     * Assigns player LED 1 (cosmetic recognition indicator). Best-effort. */
    err = hid_class_request_set_report(hid_dev, HID_REPORT_TYPE_OUTPUT,
                                       0x01, s_ds3_led_report, sizeof(s_ds3_led_report));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DS3: SET_REPORT 0x01 (LED/activate) failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "DS3: LED/activate output report sent (%d bytes)",
                 (int)sizeof(s_ds3_led_report));
    }
}

/* ========================= Gamepad Processing Task ========================= */

static const char *proto_names[] = { "NONE", "KEYBOARD", "MOUSE" };

static void gamepad_task(void *arg)
{
    gp_evt_t evt;

    ESP_LOGI(TAG, "Waiting for USB HID gamepad...");

    while (s_running) {
        /* Deferred format detection log (avoid logging in USB callback context) */
        if (s_format_log_pending) {
            s_format_log_pending = 0;
            const char *names[] = {"unknown", "PS3", "PS4", "PS5", "Generic", "Dual PSX Adaptor", "Descriptor"};
            ESP_LOGI(TAG, "Detected report format: %s (len=%d)", names[s_format], s_format_log_len);
        }

        /* Deferred raw hex dump of changed reports (first GP_RAW_DUMP_MAX per connection) */
        if (s_raw_dump_pending && s_raw_dump_count < GP_RAW_DUMP_MAX) {
            s_raw_dump_pending = 0;
            s_raw_dump_count++;
            gamepad_state_t gs;
            char btns[64];
            gamepad_get_state(&gs);
            gamepad_buttons_to_str(gs.buttons, btns, sizeof(btns));
            ESP_LOGI(TAG, "Raw report #%d (%d bytes): dpad=0x%X btns=%s L=(%d,%d) R=(%d,%d)",
                     s_raw_dump_count, s_raw_dump_len, gs.dpad, btns,
                     gs.axis_lx, gs.axis_ly, gs.axis_rx, gs.axis_ry);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, s_raw_dump_buf, s_raw_dump_len, ESP_LOG_INFO);
        }

        if (xQueueReceive(s_event_queue, &evt, pdMS_TO_TICKS(50))) {
            if (evt.type == GP_EVT_DEVICE_CONNECTED) {
                hid_host_dev_params_t params;
                ESP_ERROR_CHECK(hid_host_device_get_params(evt.hid_handle, &params));

                int proto_idx = (params.proto <= HID_PROTOCOL_MOUSE) ? params.proto : 0;
                ESP_LOGI(TAG, "HID device connected: sub_class=%d, proto=%s",
                         params.sub_class, proto_names[proto_idx]);

                if (params.proto != HID_PROTOCOL_KEYBOARD && params.proto != HID_PROTOCOL_MOUSE) {
                    ESP_LOGI(TAG, "==> Gamepad detected!");

                    /* Retrieve VID/PID for controller identification */
                    hid_host_dev_info_t dev_info;
                    if (hid_host_get_device_info(evt.hid_handle, &dev_info) == ESP_OK) {
                        s_vid = dev_info.VID;
                        s_pid = dev_info.PID;
                        ESP_LOGI(TAG, "Gamepad VID=0x%04X PID=0x%04X", s_vid, s_pid);
                    }
                }

                /* Open device and configure interface callback */
                const hid_host_device_config_t dev_config = {
                    .callback = hid_interface_cb,
                    .callback_arg = NULL,
                };

                esp_err_t err = hid_host_device_open(evt.hid_handle, &dev_config);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "hid_host_device_open failed: %s", esp_err_to_name(err));
                    continue;
                }

                /* Dump the report descriptor and, unless a hand-written format covers this
                 * pad (Sony, Dual PSX adaptor), locate its controls from the descriptor. */
                if (params.proto != HID_PROTOCOL_KEYBOARD && params.proto != HID_PROTOCOL_MOUSE &&
                    s_layout_dev == NULL) {
                    size_t rd_len = 0;
                    const uint8_t *rd = hid_host_get_report_descriptor(evt.hid_handle, &rd_len);
                    if (rd && rd_len > 0) {
                        ESP_LOGI(TAG, "Report descriptor (%d bytes):", (int)rd_len);
                        ESP_LOG_BUFFER_HEX_LEVEL(TAG, rd, rd_len, ESP_LOG_INFO);
                        bool known = (s_vid == 0x054C) || (s_vid == 0x0810 && s_pid == 0x0001);
                        gp_layout_t layout;
                        if (!known && parse_report_descriptor(rd, rd_len, &layout)) {
                            ESP_LOGI(TAG, "Descriptor layout: id=%u X@%u/%u[%ld..%ld] Y@%u/%u hat@%u/%u[%ld..%ld] buttons=%d (btn1@%u)",
                                     layout.report_id, layout.x.bit_off, layout.x.bits,
                                     (long)layout.x.lmin, (long)layout.x.lmax,
                                     layout.y.bit_off, layout.y.bits,
                                     layout.hat.bit_off, layout.hat.bits,
                                     (long)layout.hat.lmin, (long)layout.hat.lmax,
                                     layout.n_buttons, layout.btn[0].bit_off);
                            s_layout = layout;
                            s_layout_dev = evt.hid_handle;
                            s_format = GP_FORMAT_DESCRIPTOR;
                            s_format_log_len = 0;
                            s_format_log_pending = 1;
                        } else if (!known) {
                            ESP_LOGW(TAG, "No gamepad controls found in descriptor; using byte-offset heuristics");
                        }
                    } else {
                        ESP_LOGW(TAG, "Could not read report descriptor");
                    }
                }

                /* Boot-interface devices: set protocol */
                if (HID_SUBCLASS_BOOT_INTERFACE == params.sub_class) {
                    hid_class_request_set_protocol(evt.hid_handle, HID_REPORT_PROTOCOL_BOOT);
                }

                /* NOTE: Removed SET_IDLE — PS4/PS5 gamepads may STALL this request,
                 * and the HID host library doesn't recover EP0 from STALL properly.
                 * SET_IDLE is only required for boot keyboards. */

                /* Let device settle after SET_CONFIGURATION. PS4 controllers may
                 * draw high current that causes VBUS droop → brief disconnect.
                 * Long delay gives power supply time to stabilize. */
                ESP_LOGI(TAG, "Waiting 1000ms for device to stabilize...");
                vTaskDelay(pdMS_TO_TICKS(1000));

                /* Try to start with retry logic */
                err = ESP_FAIL;
                for (int attempt = 0; attempt < 3; attempt++) {
                    err = hid_host_device_start(evt.hid_handle);
                    if (err == ESP_OK) {
                        ESP_LOGI(TAG, "hid_host_device_start OK (attempt %d)", attempt + 1);
                        break;
                    }
                    ESP_LOGW(TAG, "hid_host_device_start failed (attempt %d): %s",
                             attempt + 1, esp_err_to_name(err));
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "hid_host_device_start giving up after 3 attempts");
                    hid_host_device_close(evt.hid_handle);
                    continue;
                }

                /* Genuine Sony DualShock 3 (Sixaxis) needs the operational-mode enable
                 * before it can stream input over USB. Sent AFTER device_start so the
                 * host is already polling the interrupt-IN endpoint. (Even so, this pad
                 * still needs one physical button press to actually begin streaming —
                 * see ds3_enable_operational.) */
                if (s_vid == 0x054C && s_pid == 0x0268) {
                    ESP_LOGI(TAG, "DS3 detected (054C:0268) — sending operational-mode enable");
                    ds3_enable_operational(evt.hid_handle);
                }

                /* Mark connected for gamepad devices */
                if (params.proto != HID_PROTOCOL_KEYBOARD && params.proto != HID_PROTOCOL_MOUSE) {
                    xSemaphoreTake(s_mutex, portMAX_DELAY);
                    s_state.connected = 1;
                    xSemaphoreGive(s_mutex);
                }
            }
        }
    }

    vTaskDelete(NULL);
}

/* ========================= Public API ========================= */

esp_err_t gamepad_init(const gamepad_config_t *config)
{
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_state, 0, sizeof(s_state));
    s_format = GP_FORMAT_UNKNOWN;
    s_detect_count = 0;
    s_format_log_pending = 0;
    s_raw_dump_count = 0;
    s_raw_dump_len = 0;
    s_raw_dump_pending = 0;
    s_layout_dev = NULL;
    s_layout.valid = false;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_event_queue = xQueueCreate(10, sizeof(gp_evt_t));
    if (!s_event_queue) {
        vSemaphoreDelete(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    s_running = true;

    /* Start USB host library task */
    BaseType_t ret = xTaskCreatePinnedToCore(usb_lib_task, "usb_host",
                                              config->usb_task_stack,
                                              xTaskGetCurrentTaskHandle(),
                                              config->usb_task_priority,
                                              &s_usb_task, 0);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create USB host task");
        s_running = false;
        vQueueDelete(s_event_queue);
        vSemaphoreDelete(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    /* Wait for USB host to be ready */
    ulTaskNotifyTake(false, pdMS_TO_TICKS(3000));

    /* Install HID host driver (creates its own background task) */
    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .task_priority = config->hid_task_priority,
        .stack_size = config->hid_task_stack,
        .core_id = 0,
        .callback = hid_device_cb,
        .callback_arg = NULL,
    };

    esp_err_t err = hid_host_install(&hid_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install HID host driver: %s", esp_err_to_name(err));
        s_running = false;
        /* USB host will shut down in its task when s_running becomes false */
        vQueueDelete(s_event_queue);
        vSemaphoreDelete(s_mutex);
        return err;
    }

    /* Start gamepad event processing task */
    ret = xTaskCreatePinnedToCore(gamepad_task, "gamepad", 4096, NULL,
                                   config->hid_task_priority, &s_gp_task, 1);
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create gamepad task");
        hid_host_uninstall();
        s_running = false;
        vQueueDelete(s_event_queue);
        vSemaphoreDelete(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB HID gamepad host initialized — plug in a USB gamepad!");
    return ESP_OK;
}

void gamepad_deinit(void)
{
    if (!s_running) return;

    s_running = false;

    /* Wait for tasks to finish */
    vTaskDelay(pdMS_TO_TICKS(500));

    hid_host_uninstall();

    if (s_event_queue) {
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
    }
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    s_usb_task = NULL;
    s_gp_task = NULL;
    memset(&s_state, 0, sizeof(s_state));
}

void gamepad_get_state(gamepad_state_t *state)
{
    if (!state || !s_mutex) {
        if (state) memset(state, 0, sizeof(*state));
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(state, &s_state, sizeof(gamepad_state_t));
    xSemaphoreGive(s_mutex);
}

bool gamepad_is_connected(void)
{
    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool conn = (s_state.connected != 0);
    xSemaphoreGive(s_mutex);
    return conn;
}

void gamepad_buttons_to_str(uint32_t buttons, char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) return;
    buf[0] = '\0';

    static const struct { uint32_t mask; const char *name; } btn_names[] = {
        { GAMEPAD_BTN_A,      "A"      },
        { GAMEPAD_BTN_B,      "B"      },
        { GAMEPAD_BTN_X,      "X"      },
        { GAMEPAD_BTN_Y,      "Y"      },
        { GAMEPAD_BTN_L1,     "L1"     },
        { GAMEPAD_BTN_R1,     "R1"     },
        { GAMEPAD_BTN_L2,     "L2"     },
        { GAMEPAD_BTN_R2,     "R2"     },
        { GAMEPAD_BTN_SELECT, "SEL"    },
        { GAMEPAD_BTN_START,  "START"  },
        { GAMEPAD_BTN_L3,     "L3"     },
        { GAMEPAD_BTN_R3,     "R3"     },
        { GAMEPAD_BTN_HOME,   "HOME"   },
        { GAMEPAD_BTN_MISC,   "MISC"   },
    };

    size_t pos = 0;
    for (int i = 0; i < sizeof(btn_names) / sizeof(btn_names[0]); i++) {
        if (buttons & btn_names[i].mask) {
            int written = snprintf(buf + pos, buf_size - pos,
                                   "%s%s", (pos > 0) ? " " : "", btn_names[i].name);
            if (written > 0) pos += written;
            if (pos >= buf_size - 1) break;
        }
    }

    if (pos == 0) {
        snprintf(buf, buf_size, "(none)");
    }
}

int gamepad_get_raw_report(uint8_t *buf, size_t buf_size)
{
    if (!buf || buf_size == 0 || s_raw_dump_len == 0) return 0;
    int copy = (s_raw_dump_len < (int)buf_size) ? s_raw_dump_len : (int)buf_size;
    memcpy(buf, s_raw_dump_buf, copy);
    return copy;
}

void gamepad_get_vid_pid(uint16_t *vid, uint16_t *pid)
{
    if (vid) *vid = s_vid;
    if (pid) *pid = s_pid;
}
