/*
 * On-screen touch pad for the M5Stack Tab5 - see tab5_pad.h.
 *
 * Coordinates: all layout is in LANDSCAPE pixels (1280x720, origin top-left).  The panel is
 * portrait and the picture is rotated 270 deg onto it, so
 *      panel_x = 719 - landscape_y        panel_y = landscape_x
 * (the inverse is used to turn touch points back into landscape space).
 */
#include "sdkconfig.h"

#ifdef CONFIG_BOARD_M5STACK_TAB5

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "tab5_board.h"
#include "tab5_pad.h"

#define LAND_W   TAB5_PANEL_H      /* 1280 */
#define LAND_H   TAB5_PANEL_W      /* 720  */
#define BAR_W    TAB5_PAD_BAR_W    /* 140  */

#define COL_BORDER   0xC618
#define COL_NORMAL   0x2104
#define COL_PRESSED  0x7BEF
#define COL_TEXT     0xFFFF
#define COL_ARROW    0xFFFF

enum {
    B_UP, B_DOWN, B_LEFT, B_RIGHT,
    B_A, B_B, B_X, B_Y, B_L, B_R,
    B_START, B_SELECT, B_MENU, B_VOL,
    B_COUNT
};

typedef struct {
    uint8_t  right_bar;       /* 0 = landscape-left bar, 1 = right bar */
    int16_t  x, y, w, h;      /* landscape rect, relative to its bar */
    const char *label;        /* NULL for D-pad arms */
    int8_t   arrow;           /* D-pad arms: 0 up, 1 down, 2 left, 3 right; -1 none */
} pad_btn_t;

/* Layout (design width = BAR_W = 140).  The D-pad is hit-tested by vector, not by these rects. */
static const pad_btn_t s_btn[B_COUNT] = {
    [B_UP]     = {0,  49, 288, 42, 41, NULL, 0},
    [B_DOWN]   = {0,  49, 371, 42, 41, NULL, 1},
    [B_LEFT]   = {0,   8, 329, 41, 42, NULL, 2},
    [B_RIGHT]  = {0,  91, 329, 41, 42, NULL, 3},
    [B_L]      = {0,  10,  10, 120, 64, "L",      -1},
    [B_MENU]   = {0,  10,  84, 120, 46, "MENU",   -1},
    [B_SELECT] = {0,  10, 640, 120, 56, "SELECT", -1},
    [B_R]      = {1,  10,  10, 120, 64, "R",      -1},
    [B_VOL]    = {1,  10,  84, 120, 46, "VOL",    -1},
    [B_Y]      = {1,  10, 150,  56, 56, "Y",      -1},
    [B_X]      = {1,  74, 150,  56, 56, "X",      -1},
    [B_A]      = {1,  10, 240, 120, 110, "A",     -1},
    [B_B]      = {1,  10, 360, 120, 110, "B",     -1},
    [B_START]  = {1,  10, 640, 120, 56, "START",  -1},
};

#define DPAD_CX      70    /* relative to the left bar */
#define DPAD_CY      350
#define DPAD_RADIUS  95
#define DPAD_DEAD    20
#define HIT_SLOP     6

static const int s_input_idx[B_COUNT] = {
    [B_UP] = ODROID_INPUT_UP,     [B_DOWN] = ODROID_INPUT_DOWN,
    [B_LEFT] = ODROID_INPUT_LEFT, [B_RIGHT] = ODROID_INPUT_RIGHT,
    [B_A] = ODROID_INPUT_A,       [B_B] = ODROID_INPUT_B,
    [B_X] = ODROID_INPUT_X,       [B_Y] = ODROID_INPUT_Y,
    [B_L] = ODROID_INPUT_L,       [B_R] = ODROID_INPUT_R,
    [B_START] = ODROID_INPUT_START, [B_SELECT] = ODROID_INPUT_SELECT,
    [B_MENU] = ODROID_INPUT_MENU, [B_VOL] = ODROID_INPUT_VOLUME,
};

/* Touch is polled by a small background task so the I2C transfers never run on
 * an emulator's thread; tab5_pad_read() only reads the cached mask. */
#define POLL_PERIOD_MS   8          /* ~125 Hz */
#define POLL_TASK_PRIO   5          /* same as the video tasks; it sleeps between polls */
#define POLL_TASK_CORE   0          /* video tasks are pinned to core 1 */

static SemaphoreHandle_t s_lock = NULL;          /* serialises pad drawing (poll task vs fill hook) */
static volatile uint32_t s_mask = 0;             /* currently pressed buttons (bit = B_xxx) */

/* ─── Drawing primitives (landscape coordinates -> portrait panel) ─ */

static void fill_rect_land(uint16_t *fb, int lx, int ly, int w, int h, uint16_t color)
{
    int px0 = (LAND_H - 1) - (ly + h - 1);
    int px1 = px0 + h;                       /* exclusive */
    if (px0 < 0) px0 = 0;
    if (px1 > TAB5_PANEL_W) px1 = TAB5_PANEL_W;
    if (px1 <= px0) return;
    for (int i = 0; i < w; i++) {
        int py = lx + i;
        if (py < 0 || py >= TAB5_PANEL_H) continue;
        uint16_t *row = fb + (size_t)py * TAB5_PANEL_W;
        for (int px = px0; px < px1; px++) row[px] = color;
    }
}

/* 5x7 glyphs, one byte per row, bit 4 = leftmost pixel. Only the letters the labels use. */
static const uint8_t *glyph(char c)
{
    static const uint8_t g_A[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t g_B[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E};
    static const uint8_t g_C[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E};
    static const uint8_t g_E[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F};
    static const uint8_t g_L[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F};
    static const uint8_t g_M[7] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11};
    static const uint8_t g_N[7] = {0x11,0x19,0x15,0x15,0x13,0x11,0x11};
    static const uint8_t g_O[7] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t g_R[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11};
    static const uint8_t g_S[7] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E};
    static const uint8_t g_T[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04};
    static const uint8_t g_U[7] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t g_V[7] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04};
    static const uint8_t g_X[7] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11};
    static const uint8_t g_Y[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04};
    switch (c) {
    case 'A': return g_A; case 'B': return g_B; case 'C': return g_C; case 'E': return g_E;
    case 'L': return g_L; case 'M': return g_M; case 'N': return g_N; case 'O': return g_O;
    case 'R': return g_R; case 'S': return g_S; case 'T': return g_T; case 'U': return g_U;
    case 'V': return g_V; case 'X': return g_X; case 'Y': return g_Y;
    default:  return NULL;
    }
}

static void draw_text_centered(uint16_t *fb, int cx, int cy, const char *s, int scale, uint16_t color)
{
    int n = (int)strlen(s);
    int tw = n * 6 * scale - scale, th = 7 * scale;
    int x0 = cx - tw / 2, y0 = cy - th / 2;
    for (int ci = 0; ci < n; ci++) {
        const uint8_t *g = glyph(s[ci]);
        if (!g) continue;
        for (int gy = 0; gy < 7; gy++)
            for (int gx = 0; gx < 5; gx++)
                if (g[gy] & (0x10 >> gx))
                    fill_rect_land(fb, x0 + ci * 6 * scale + gx * scale, y0 + gy * scale, scale, scale, color);
    }
}

static void draw_arrow(uint16_t *fb, int cx, int cy, int dir, uint16_t color)
{
    const int t = 14;
    for (int k = 0; k < t; k++) {
        switch (dir) {
        case 0: fill_rect_land(fb, cx - k, cy - t / 2 + k, 2 * k + 1, 1, color); break;
        case 1: fill_rect_land(fb, cx - k, cy + t / 2 - k, 2 * k + 1, 1, color); break;
        case 2: fill_rect_land(fb, cx - t / 2 + k, cy - k, 1, 2 * k + 1, color); break;
        default: fill_rect_land(fb, cx + t / 2 - k, cy - k, 1, 2 * k + 1, color); break;
        }
    }
}

static inline int bar_x0(const pad_btn_t *b) { return b->right_bar ? (LAND_W - BAR_W) : 0; }

static void draw_button(uint16_t *fb, int i, bool pressed)
{
    const pad_btn_t *b = &s_btn[i];
    int lx = bar_x0(b) + b->x, ly = b->y;
    fill_rect_land(fb, lx, ly, b->w, b->h, COL_BORDER);
    fill_rect_land(fb, lx + 2, ly + 2, b->w - 4, b->h - 4, pressed ? COL_PRESSED : COL_NORMAL);
    int cx = lx + b->w / 2, cy = ly + b->h / 2;
    if (b->label) {
        int scale = (b->h >= 100) ? 6 : (b->w < 80 ? 4 : 3);
        draw_text_centered(fb, cx, cy, b->label, scale, COL_TEXT);
    } else if (b->arrow >= 0) {
        draw_arrow(fb, cx, cy, b->arrow, COL_ARROW);
    }
}

static void draw_all(uint16_t *fb, uint32_t mask)
{
    /* D-pad centre block (not interactive) */
    fill_rect_land(fb, DPAD_CX - 21, DPAD_CY - 21, 42, 42, COL_BORDER);
    fill_rect_land(fb, DPAD_CX - 19, DPAD_CY - 19, 38, 38, COL_NORMAL);
    for (int i = 0; i < B_COUNT; i++) draw_button(fb, i, (mask >> i) & 1);
}

static void sync_button(int i)
{
    uint16_t *fb = (uint16_t *)tab5_display_fb();
    if (!fb) return;
    const pad_btn_t *b = &s_btn[i];
    int py0 = bar_x0(b) + b->x;
    int rows = b->w;
    esp_cache_msync(fb + (size_t)py0 * TAB5_PANEL_W, (size_t)rows * TAB5_PANEL_W * sizeof(uint16_t),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

/* ─── Hit testing ──────────────────────────────────────────────── */

static uint32_t hit_test(int lx, int ly)
{
    uint32_t m = 0;

    int dx = lx - DPAD_CX, dy = ly - DPAD_CY;
    if (dx >= -DPAD_RADIUS && dx <= DPAD_RADIUS && dy >= -DPAD_RADIUS && dy <= DPAD_RADIUS) {
        if (dy < -DPAD_DEAD) m |= 1u << B_UP;
        if (dy >  DPAD_DEAD) m |= 1u << B_DOWN;
        if (dx < -DPAD_DEAD) m |= 1u << B_LEFT;
        if (dx >  DPAD_DEAD) m |= 1u << B_RIGHT;
        return m;
    }

    for (int i = B_A; i < B_COUNT; i++) {
        const pad_btn_t *b = &s_btn[i];
        int x0 = bar_x0(b) + b->x - HIT_SLOP, y0 = b->y - HIT_SLOP;
        if (lx >= x0 && lx < x0 + b->w + 2 * HIT_SLOP && ly >= y0 && ly < y0 + b->h + 2 * HIT_SLOP)
            m |= 1u << i;
    }
    return m;
}

/* ─── Public API ───────────────────────────────────────────────── */

static void pad_fill_hook(uint16_t *fb)
{
    /* Runs inside tab5_display_fill() on whichever task cleared the frame buffer. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    draw_all(fb, s_mask);
    xSemaphoreGive(s_lock);
}

/* One poll: read touch, hit-test, redraw buttons whose state changed. */
static void pad_poll_once(void)
{
    tab5_touch_point_t pts[TAB5_MAX_TOUCH_POINTS];
    int n = tab5_touch_read(pts, TAB5_MAX_TOUCH_POINTS);
    uint32_t mask = 0;
    for (int i = 0; i < n; i++) {
        /* panel (portrait) -> landscape */
        int lx = pts[i].y;
        int ly = (LAND_H - 1) - (int)pts[i].x;
        mask |= hit_test(lx, ly);
    }

    uint32_t changed = mask ^ s_mask;
    if (!changed) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint16_t *fb = (uint16_t *)tab5_display_fb();
    if (fb) {
        for (int i = 0; i < B_COUNT; i++) {
            if (!((changed >> i) & 1)) continue;
            draw_button(fb, i, (mask >> i) & 1);
            sync_button(i);
        }
    }
    s_mask = mask;
    xSemaphoreGive(s_lock);
}

static void pad_poll_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        pad_poll_once();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(POLL_PERIOD_MS));
    }
}

void tab5_pad_init(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return;
    tab5_display_set_fill_hook(pad_fill_hook);
    if (xTaskCreatePinnedToCore(pad_poll_task, "tab5_pad", 3072, NULL,
                                POLL_TASK_PRIO, NULL, POLL_TASK_CORE) != pdPASS) {
        ESP_LOGE("tab5_pad", "touch pad poll task could not be created");
    }
}

void tab5_pad_read(odroid_gamepad_state *state)
{
    uint32_t mask = s_mask;     /* single aligned word: no lock needed */
    for (int i = 0; i < B_COUNT; i++) {
        if ((mask >> i) & 1) state->values[s_input_idx[i]] = 1;
    }
}

#endif /* CONFIG_BOARD_M5STACK_TAB5 */
