/*
 * HDMI output through the M5Stack Module Display 13.2 - see hdmi_out.h.
 *
 * The module protocol (FPGA JTAG loader, SPI command set, video timing maths and the LT8618
 * register tables) follows M5GFX's Panel_M5HDMI (MIT, (c) M5Stack, based on LovyanGFX) and the
 * module's FPGA design (https://github.com/ciniml/atom_display_fpga).  It is rewritten here on top of
 * plain ESP-IDF drivers so the firmware does not need the whole of M5GFX; see LICENSE.m5gfx.
 *
 * Tab5 M-Bus wiring (from M5Unified / M5GFX):
 *   SPI  SCLK 5, MOSI 18, MISO 19, CS 48   (the FPGA's JTAG uses the same pins: TCK/TDI/TDO/TMS)
 *   I2C  SDA 31, SCL 32                    (system bus; LT8618 HDMI transmitter at 0x39)
 *   I2S  BCK 2, DATA 47, LRCK 35           (48 kHz stereo into the transmitter)
 */

#include "hdmi_out.h"
#include "lt8618_tables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_rom_gpio.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2s_std.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_sig_map.h"
#include "soc/i2c_periph.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "hdmi_out";

#define FPGA_IMAGE_PATH  "/sd/system/hdmi_fpga.bin"

#define PIN_SCLK   5
#define PIN_MOSI   18
#define PIN_MISO   19
#define PIN_CS     48
#define PIN_SDA    31
#define PIN_SCL    32
#define I2C_PORT   1
#define PIN_I2S_BCK   2
#define PIN_I2S_DOUT  47
#define PIN_I2S_WS    35

#define LT_ADDR    0x39
#define SPI_HOST_ID SPI2_HOST
#define SPI_CHUNK  32704   /* multiple of 64, and < 2^18 bits: the P4 DMA limit is 262143 bits */

/* FPGA command set */
#define CMD_NOP           0x00
#define CMD_READ_ID       0x04
#define CMD_SCREEN_SCALING 0x18
#define CMD_CASET         0x2A
#define CMD_RASET         0x2B
#define CMD_WRITE_RAW_16  0x42
#define CMD_FILLRECT_8    0x69
#define CMD_VIDEO_TIMING_V 0xB0
#define CMD_VIDEO_TIMING_H 0xB1
#define CMD_VIDEO_CLOCK   0xB2

#define OUT_W 1280
#define OUT_H 720

/* ───────────────────────── state ───────────────────────── */
static volatile bool s_started;
static volatile bool s_ready;
static volatile bool s_audio_ok;
static i2c_master_bus_handle_t s_bus;

/* ───────────────────────── I2C to the LT8618 ─────────────────────────
 * The module has series resistors in SDA/SCL, so on the Tab5 the transmitter's ACK does not reach
 * 0 V and the I2C peripheral can read it as a NACK (M5GFX documents this).  The normal driver is
 * tried first; when its reads fail the registers are accessed by bit-banging the same two pins
 * with the ACK ignored. */
typedef enum { I2C_NONE, I2C_DRIVER, I2C_BITBANG } lt_bus_t;
static lt_bus_t s_i2c_mode;
static i2c_master_dev_handle_t s_lt_dev;

static inline void bb_delay(void) { esp_rom_delay_us(3); }
static inline void bb_sda(int v) { gpio_set_level(PIN_SDA, v); }
static inline void bb_scl(int v) { gpio_set_level(PIN_SCL, v); }

static void bb_take(void)
{
    gpio_set_direction(PIN_SDA, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_direction(PIN_SCL, GPIO_MODE_INPUT_OUTPUT_OD);
    esp_rom_gpio_connect_out_signal(PIN_SDA, SIG_GPIO_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(PIN_SCL, SIG_GPIO_OUT_IDX, false, false);
    bb_sda(1);
    bb_scl(1);
}

static void bb_release(void)
{
    esp_rom_gpio_connect_out_signal(PIN_SDA, i2c_periph_signal[I2C_PORT].sda_out_sig, false, false);
    esp_rom_gpio_connect_out_signal(PIN_SCL, i2c_periph_signal[I2C_PORT].scl_out_sig, false, false);
}

static void bb_start(void)
{
    bb_sda(1); bb_scl(1); bb_delay();
    bb_sda(0); bb_delay();
    bb_scl(0); bb_delay();
}

static void bb_stop(void)
{
    bb_sda(0); bb_delay();
    bb_scl(1); bb_delay();
    bb_sda(1); bb_delay();
}

/* returns the ACK bit as read (ignored by callers except for diagnostics) */
static bool bb_write_byte(uint8_t b)
{
    for (int i = 0; i < 8; ++i) {
        bb_sda((b & 0x80) ? 1 : 0);
        b <<= 1;
        bb_delay();
        bb_scl(1); bb_delay();
        bb_scl(0);
    }
    bb_sda(1); bb_delay();
    bb_scl(1); bb_delay();
    bool ack = gpio_get_level(PIN_SDA) == 0;
    bb_scl(0); bb_delay();
    return ack;
}

static uint8_t bb_read_byte(bool ack)
{
    uint8_t v = 0;
    bb_sda(1);
    for (int i = 0; i < 8; ++i) {
        bb_delay();
        bb_scl(1); bb_delay();
        v = (uint8_t)((v << 1) | (gpio_get_level(PIN_SDA) ? 1 : 0));
        bb_scl(0);
    }
    bb_sda(ack ? 0 : 1); bb_delay();
    bb_scl(1); bb_delay();
    bb_scl(0); bb_delay();
    bb_sda(1);
    return v;
}

static void lt_wr(uint8_t reg, uint8_t val)
{
    if (s_i2c_mode == I2C_DRIVER) {
        uint8_t b[2] = { reg, val };
        i2c_master_transmit(s_lt_dev, b, 2, 50);   /* result ignored on purpose (see above) */
    } else if (s_i2c_mode == I2C_BITBANG) {
        bb_take();
        bb_start();
        bb_write_byte(LT_ADDR << 1);
        bb_write_byte(reg);
        bb_write_byte(val);
        bb_stop();
        bb_release();
    }
}

static bool lt_rd(uint8_t reg, uint8_t *val)
{
    *val = 0;
    if (s_i2c_mode == I2C_DRIVER) {
        return i2c_master_transmit_receive(s_lt_dev, &reg, 1, val, 1, 50) == ESP_OK;
    } else if (s_i2c_mode == I2C_BITBANG) {
        bb_take();
        bb_start();
        bb_write_byte(LT_ADDR << 1);
        bb_write_byte(reg);
        bb_start();
        bb_write_byte((LT_ADDR << 1) | 1);
        *val = bb_read_byte(false);
        bb_stop();
        bb_release();
        return true;
    }
    return false;
}

static uint8_t lt_rd8(uint8_t reg)
{
    uint8_t v;
    lt_rd(reg, &v);
    return v;
}

static void lt_wr_set(const uint8_t *pairs, size_t len)
{
    for (size_t i = 0; i + 1 < len; i += 2) lt_wr(pairs[i], pairs[i + 1]);
}

static bool lt_read_chip_id(uint8_t id[3])
{
    lt_wr(0xff, 0x80);
    lt_wr(0xee, 0x01);
    for (uint8_t i = 0; i < 3; ++i) {
        if (!lt_rd(i, &id[i])) { id[0] = id[1] = id[2] = 0; return false; }
    }
    return true;
}

static bool id_valid(const uint8_t id[3])
{
    return !(id[0] == id[1] && id[0] == id[2]);
}

/* Finds the transmitter; returns false when no module is attached. */
static bool lt_detect(uint8_t id[3])
{
    if (s_bus) {
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = LT_ADDR,
            .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(s_bus, &dc, &s_lt_dev) == ESP_OK) {
            s_i2c_mode = I2C_DRIVER;
            if (lt_read_chip_id(id) && id_valid(id)) {
                ESP_LOGI(TAG, "LT8618 chip ID %02x %02x %02x (I2C driver)", id[0], id[1], id[2]);
                return true;
            }
            i2c_master_bus_rm_device(s_lt_dev);
            s_lt_dev = NULL;
        }
    }
    s_i2c_mode = I2C_BITBANG;
    if (lt_read_chip_id(id) && id_valid(id)) {
        ESP_LOGI(TAG, "LT8618 chip ID %02x %02x %02x (bit-banged I2C, ACK ignored)", id[0], id[1], id[2]);
        return true;
    }
    s_i2c_mode = I2C_NONE;
    return false;
}

static bool lt_init(void)
{
    uint8_t id[3];
    lt_read_chip_id(id);

    lt_wr_set(lt_audio48k, sizeof(lt_audio48k));
    lt_wr(0x2b, lt_rd8(0x2b) & 0xfd);
    lt_wr(0x2e, lt_rd8(0x2e) & 0xfe);
    if (id[2] == 0xE2) lt_wr_set(lt_u3, sizeof(lt_u3));

    for (int i = 0; i < 8; ++i) {
        lt_wr_set(lt_pll, sizeof(lt_pll));
        bool locked = (lt_rd8(0x15) & 0x80) != 0;
        uint8_t value = lt_rd8(0xea);
        bool done = (lt_rd8(0xeb) & 0x80) != 0;
        if (locked && done && value != 0xff) {
            lt_wr_set(lt_post_pll, sizeof(lt_post_pll));
            return true;
        }
    }
    ESP_LOGE(TAG, "HDMI transmitter PLL did not lock");
    return false;
}

/* ───────────────────────── SPI to the FPGA ───────────────────────── */
static spi_device_handle_t s_spi_w;
static spi_device_handle_t s_spi_r;
static bool s_spi_bus_up;
static uint8_t *s_small;          /* 64-byte DMA-capable bounce buffer for short transfers */

static inline void cs(int level) { gpio_set_level(PIN_CS, level); }

static void spi_devices_remove(void)
{
    if (s_spi_w) { spi_bus_remove_device(s_spi_w); s_spi_w = NULL; }
    if (s_spi_r) { spi_bus_remove_device(s_spi_r); s_spi_r = NULL; }
}

static esp_err_t spi_up(uint32_t wr_hz, uint32_t rd_hz)
{
    if (!s_spi_bus_up) {
        const spi_bus_config_t bc = {
            .mosi_io_num = PIN_MOSI,
            .miso_io_num = PIN_MISO,
            .sclk_io_num = PIN_SCLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = SPI_CHUNK,
        };
        esp_err_t e = spi_bus_initialize(SPI_HOST_ID, &bc, SPI_DMA_CH_AUTO);
        if (e != ESP_OK) return e;
        gpio_set_pull_mode(PIN_MISO, GPIO_PULLUP_ONLY);
        s_spi_bus_up = true;
    }
    spi_devices_remove();

    const gpio_config_t cc = {
        .pin_bit_mask = 1ULL << PIN_CS,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cc);
    cs(1);

    spi_device_interface_config_t w = {
        .clock_speed_hz = (int)wr_hz,
        .mode = 3,
        .spics_io_num = -1,
        .queue_size = 2,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    esp_err_t e = spi_bus_add_device(SPI_HOST_ID, &w, &s_spi_w);
    if (e != ESP_OK) return e;
    spi_device_interface_config_t r = {
        .clock_speed_hz = (int)rd_hz,
        .mode = 3,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    return spi_bus_add_device(SPI_HOST_ID, &r, &s_spi_r);
}

static void spi_down(void)
{
    spi_devices_remove();
    if (s_spi_bus_up) {
        spi_bus_free(SPI_HOST_ID);
        s_spi_bus_up = false;
    }
}

static void spi_write(const void *p, size_t n)
{
    spi_transaction_t t = { .length = n * 8, .tx_buffer = p };
    spi_device_transmit(s_spi_w, &t);
}

/* short writes go through the internal bounce buffer (n <= 64) */
static void spi_write_small(const void *p, size_t n)
{
    memcpy(s_small, p, n);
    spi_write(s_small, n);
}

static uint8_t spi_read8(void)
{
    spi_transaction_t t = { .length = 8, .rxlength = 8, .flags = SPI_TRANS_USE_RXDATA };
    spi_device_polling_transmit(s_spi_r, &t);
    return t.rx_data[0];
}

/* four bytes, first received byte in the low byte (as M5GFX's readData(32)) */
static uint32_t spi_read32(void)
{
    spi_transaction_t t = { .length = 32, .rxlength = 32, .flags = SPI_TRANS_USE_RXDATA };
    spi_device_polling_transmit(s_spi_r, &t);
    return (uint32_t)t.rx_data[0] | ((uint32_t)t.rx_data[1] << 8) |
           ((uint32_t)t.rx_data[2] << 16) | ((uint32_t)t.rx_data[3] << 24);
}

static uint32_t fpga_read_id(void)
{
    static const uint8_t nop4[4] = { CMD_NOP, CMD_NOP, CMD_NOP, CMD_NOP };
    cs(0);
    spi_write_small(nop4, 4);
    cs(1);
    spi_write_small(nop4, 4);
    cs(0);
    const uint8_t rid = CMD_READ_ID;
    spi_write_small(&rid, 1);
    uint32_t retry = 16;
    while (spi_read8() == 0xFF && --retry) {}
    spi_read8();
    uint32_t id = spi_read32();
    cs(1);
    return id;
}

static bool fpga_id_is_hd(uint32_t id)
{
    return (id & 0xFFFF) == ('H' | ('D' << 8));
}

/* The FPGA answers 0x00 while it is busy with a command / pixel stream. */
static bool wait_idle(uint32_t timeout_ms)
{
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    do {
        cs(0);
        uint8_t b = spi_read8();
        cs(1);
        if (b != 0x00) return true;
        esp_rom_delay_us(50);
    } while (esp_timer_get_time() < end);
    return false;
}

static void send_cmd(const uint8_t *cmd, size_t n)
{
    wait_idle(100);
    cs(0);
    spi_write_small(cmd, n);
    cs(1);
}

/* ───────────────────────── FPGA image over JTAG ───────────────────────── */
#define JT_TCK PIN_SCLK
#define JT_TDI PIN_MOSI
#define JT_TDO PIN_MISO
#define JT_TMS PIN_CS

enum {
    ISC_NOOP = 0x02, ISC_ERASE = 0x05, ERASE_DONE = 0x09, ISC_ENABLE = 0x15, FAST_PROGRAM = 0x17,
    STATUS_CODE = 0x41, ISC_DISABLE = 0x3A,
};
typedef enum { TAP_UNKNOWN, TAP_IDLE, TAP_DRSHIFT, TAP_DREXIT1, TAP_IRSHIFT, TAP_IREXIT1 } tap_t;

static inline void jt_set(int pin, int v) { gpio_ll_set_level(&GPIO, pin, v); }
static inline void jt_tck(void) { jt_set(JT_TCK, 0); jt_set(JT_TCK, 1); }

static void jt_tap_inner(bool tms, size_t clocks)
{
    jt_set(JT_TMS, tms);
    do { jt_tck(); } while (--clocks);
}

static void jt_move(tap_t from, tap_t to)
{
    int high = 1, low = 2;
    if (from == TAP_UNKNOWN && to == TAP_IDLE)        { high = 8; }
    else if (from == TAP_IDLE && to == TAP_IDLE)      { high = 0; low = 3; }
    else if (from == TAP_IDLE && to == TAP_IRSHIFT)   { high = 2; }
    else if (from == TAP_IDLE && to == TAP_DRSHIFT)   { }
    else if (from == TAP_IREXIT1 && to == TAP_IDLE)   { low = 11; }
    else if (from == TAP_DREXIT1 && to == TAP_IDLE)   { }
    else { ESP_LOGW(TAG, "JTAG: bad tap walk"); return; }
    if (high) jt_tap_inner(true, high);
    jt_tap_inner(false, low);
}

static void jt_write(uint8_t din, bool tms, bool lsb, size_t len)
{
    if (lsb && din) {
        din = (uint8_t)(((din & 0x55) << 1) + ((din >> 1) & 0x55));
        din = (uint8_t)(((din & 0x33) << 2) + ((din >> 2) & 0x33));
        din = (uint8_t)(((din & 0x0F) << 4) + ((din >> 4) & 0x0F));
    }
    jt_set(JT_TMS, 0);
    int prev = -1;
    do {
        size_t i = 8;
        do {
            if (!--i && tms && len == 1) jt_set(JT_TMS, 1);
            int lv = (din >> i) & 1;
            if (prev != lv) { prev = lv; jt_set(JT_TDI, lv); }
            jt_tck();
        } while (i);
    } while (--len);
}

static void jt_inst(uint8_t inst)
{
    jt_move(TAP_IDLE, TAP_IRSHIFT);
    jt_write(inst, true, true, 1);
    jt_move(TAP_IREXIT1, TAP_IDLE);
}

static void jt_dummy_clock(uint32_t msec)
{
    int64_t end = esp_timer_get_time() + (int64_t)msec * 1000;
    do { jt_tck(); jt_tck(); } while (esp_timer_get_time() < end);
}

static uint32_t jt_status(void)
{
    uint32_t out = 0;
    jt_inst(STATUS_CODE);
    jt_move(TAP_IDLE, TAP_DRSHIFT);
    jt_set(JT_TMS, 0);
    for (int i = 0; i < 32; ++i) {
        if (i == 31) jt_set(JT_TMS, 1);
        jt_tck();
        if (gpio_ll_get_level(&GPIO, JT_TDO)) out |= 1u << i;
    }
    jt_move(TAP_DREXIT1, TAP_IDLE);
    return out;
}

/* The SPI bus must be down: the four pins are bit-banged as JTAG. */
static void fpga_load(const uint8_t *img, size_t len)
{
    const int outs[] = { JT_TCK, JT_TDI, JT_TMS };
    for (size_t i = 0; i < 3; ++i) {
        gpio_reset_pin(outs[i]);
        gpio_set_direction(outs[i], GPIO_MODE_OUTPUT);
        jt_set(outs[i], 0);
    }
    gpio_reset_pin(JT_TDO);
    gpio_set_direction(JT_TDO, GPIO_MODE_INPUT);

    int retry = 128;
    do {   /* the wait differs between FPGA lots: poll the status register */
        if ((jt_status() & 0x200) == 0) break;
        vTaskDelay(1);
    } while (--retry);

    jt_move(TAP_UNKNOWN, TAP_IDLE);
    ESP_LOGI(TAG, "Erasing FPGA SRAM");
    jt_inst(ISC_ENABLE); jt_inst(ISC_ERASE); jt_inst(ISC_NOOP);
    jt_dummy_clock(4);
    jt_inst(ERASE_DONE); jt_inst(ISC_NOOP);
    jt_inst(ISC_DISABLE); jt_inst(ISC_NOOP);
    jt_dummy_clock(4);

    ESP_LOGI(TAG, "Writing FPGA image (%u bytes)", (unsigned)len);
    jt_inst(ISC_ENABLE);
    jt_inst(FAST_PROGRAM);
    jt_move(TAP_IDLE, TAP_DRSHIFT);

    int32_t rle_len = -1, direct_len = -1;
    for (size_t i = 0; i < len; ++i) {
        bool tx_end = (i == len - 1);
        if (rle_len < 0) {
            rle_len = img[i];
            direct_len = -1;
        } else if (rle_len == 0) {            /* direct mode */
            if (direct_len == -1) {
                direct_len = img[i];
            } else {
                jt_write(img[i], tx_end, false, 1);
                if (0 == --direct_len) rle_len = -1;
            }
        } else {                              /* repeat mode */
            jt_write(img[i], tx_end, false, (size_t)rle_len);
            rle_len = -1;
        }
    }

    jt_move(TAP_DREXIT1, TAP_IDLE);
    jt_inst(ISC_DISABLE);
    jt_inst(ISC_NOOP);
    ESP_LOGI(TAG, "FPGA image written, JTAG status 0x%08x", (unsigned)jt_status());
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (n > 0) ? heap_caps_malloc((size_t)n, MALLOC_CAP_SPIRAM) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) *len = (size_t)n;
    return buf;
}

/* ───────────────────────── video mode ───────────────────────── */
static int s_scale;               /* current integer scale, 0 = not set */
static int s_mem_w, s_mem_h, s_canvas_w, s_canvas_h;
static int s_last_w, s_last_h;

static uint8_t sum_not(const uint8_t *p, size_t n)
{
    uint8_t s = 0;
    for (size_t i = 0; i < n; ++i) s += p[i];
    return (uint8_t)~s;
}

static void send_timing(uint8_t cmd, int sync, int back, int active, int front)
{
    uint8_t b[10] = { cmd, (uint8_t)(sync >> 8), (uint8_t)sync, (uint8_t)(back >> 8), (uint8_t)back,
                      (uint8_t)(active >> 8), (uint8_t)active, (uint8_t)(front >> 8), (uint8_t)front, 0 };
    b[9] = sum_not(b, 9);
    send_cmd(b, sizeof(b));
}

static void send_scaling(int sx, int sy, int mem_w, int mem_h)
{
    int w = mem_w / sx;
    int h = mem_h / sy;
    uint8_t b[8] = { CMD_SCREEN_SCALING, (uint8_t)sx, (uint8_t)sy, (uint8_t)(w >> 8), (uint8_t)w,
                     (uint8_t)(h >> 8), (uint8_t)h, 0 };
    b[7] = sum_not(b, 7);
    send_cmd(b, sizeof(b));
}

/* Pixel clock 74.25 MHz: M5GFX's PLL search gives feedback 1, input 1, output 8 (VCO 594 MHz). */
static void send_clock(void)
{
    uint8_t b[9] = { CMD_VIDEO_CLOCK, 0, 1, 0, 1, 0, 8, 0, 0 };
    b[8] = sum_not(b, 8);
    send_cmd(b, sizeof(b));
}

/* Same timing search as M5GFX Panel_M5HDMI::_init_resolution at 60 Hz / 74.25 MHz. */
static void init_resolution(int mem_w, int mem_h, int scale)
{
    const int32_t total = 74250000 / 60;
    int vert_total = mem_h + 9;
    int hori_total = total / vert_total;
    int hori_tmp = hori_total, vert_tmp = vert_total;
    int hori_min = mem_w + (32 + (mem_w >> (1 + scale)));
    int hori_max = mem_w + (768 + (mem_w >> 3));
    if (hori_tmp > hori_max) hori_tmp = hori_max;

    uint32_t diff = ~0u;
    for (;;) {
        int d1 = total - hori_tmp * vert_tmp;
        uint32_t da = (uint32_t)abs(d1);
        if (diff > da) {
            diff = da;
            hori_total = hori_tmp;
            vert_total = vert_tmp;
            if (diff == 0) break;
        }
        if (d1 >= 0) ++vert_tmp;
        else if (--hori_tmp < hori_min) break;
    }
    if (hori_total <= hori_min) {
        hori_total = hori_min;
        ESP_LOGW(TAG, "video timing out of range for %dx%d", mem_w, mem_h);
    }

    int remain = vert_total - mem_h;
    int sync = 1;
    remain -= sync;
    int porch = remain >> 1;
    remain -= porch;
    int v_front = porch, v_sync = sync, v_back = remain;

    remain = hori_total - mem_w;
    sync = 24 + (remain >> 8);
    remain -= sync;
    porch = (remain * 131) >> 8;
    remain -= porch;
    int h_front = porch, h_sync = sync, h_back = remain;

    send_timing(CMD_VIDEO_TIMING_V, v_sync, v_back, mem_h, v_front);
    send_timing(CMD_VIDEO_TIMING_H, h_sync, h_back, mem_w, h_front);
    send_scaling(scale, scale, mem_w, mem_h);
    send_clock();
}

static void fpga_clear(int mem_w, int mem_h)
{
    int xe = mem_w - 1, ye = mem_h - 1;
    uint8_t b[10] = { CMD_FILLRECT_8, 0, 0, 0, 0, (uint8_t)(xe >> 8), (uint8_t)xe,
                      (uint8_t)(ye >> 8), (uint8_t)ye, 0x00 };
    send_cmd(b, sizeof(b));
}

/* Largest integer scale whose canvas (1280/s x 720/s) still holds the frame. */
static int pick_scale(uint32_t w, uint32_t h)
{
    for (int s = 8; s > 1; --s) {
        if (w <= (uint32_t)(OUT_W / s) && h <= (uint32_t)(OUT_H / s)) return s;
    }
    return 1;
}

static void apply_mode(int scale)
{
    s_canvas_w = OUT_W / scale;
    s_canvas_h = OUT_H / scale;
    s_mem_w = s_canvas_w * scale;
    s_mem_h = s_canvas_h * scale;
    init_resolution(s_mem_w, s_mem_h, scale);
    fpga_clear(s_mem_w, s_mem_h);
    s_scale = scale;
    s_last_w = s_last_h = 0;
    ESP_LOGI(TAG, "video mode: %dx%d output, %dx scale, %dx%d canvas", s_mem_w, s_mem_h, scale, s_canvas_w, s_canvas_h);
}

/* One window write: CASET + RASET + WRITE_RAW_16, then w*h big-endian RGB565 pixels. */
static bool send_frame(const uint8_t *px, int w, int h)
{
    int xs = (s_canvas_w - w) / 2, ys = (s_canvas_h - h) / 2;
    int xe = xs + w - 1, ye = ys + h - 1;
    uint8_t hdr[11] = {
        CMD_CASET, (uint8_t)(xs >> 8), (uint8_t)xs, (uint8_t)(xe >> 8), (uint8_t)xe,
        CMD_RASET, (uint8_t)(ys >> 8), (uint8_t)ys, (uint8_t)(ye >> 8), (uint8_t)ye,
        CMD_WRITE_RAW_16,
    };

    if (!wait_idle(50)) return false;
    cs(0);
    spi_write_small(hdr, sizeof(hdr));

    size_t total = (size_t)w * h * 2;
    size_t body = total & ~(size_t)63;
    for (size_t off = 0; off < body; off += SPI_CHUNK) {
        size_t n = body - off;
        if (n > SPI_CHUNK) n = SPI_CHUNK;
        spi_write(px + off, n);
    }
    if (total > body) spi_write_small(px + body, total - body);

    /* wait until the FPGA has taken all pixels (it answers 0x00 while busy) */
    bool ok = false;
    int64_t end = esp_timer_get_time() + 50000;
    do {
        if (spi_read8() != 0x00) { ok = true; break; }
    } while (esp_timer_get_time() < end);
    cs(1);
    return ok;
}

/* ───────────────────────── frame hand-over ─────────────────────────
 * Three slots: one being sent, one holding the newest unsent frame, one free.  A new frame
 * replaces an unsent one, so the caller is never held up by the SPI link. */
#define SLOTS 3
static uint8_t *s_slot_buf[SLOTS];
static size_t s_slot_cap[SLOTS];
static uint16_t s_slot_w[SLOTS], s_slot_h[SLOTS];
static int s_pending = -1, s_inflight = -1;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;

static bool slot_reserve(int i, size_t bytes)
{
    if (s_slot_cap[i] >= bytes) return true;
    heap_caps_free(s_slot_buf[i]);
    s_slot_buf[i] = heap_caps_aligned_alloc(64, (bytes + 63) & ~(size_t)63, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    s_slot_cap[i] = s_slot_buf[i] ? ((bytes + 63) & ~(size_t)63) : 0;
    return s_slot_buf[i] != NULL;
}

void hdmi_out_present(const uint16_t *src, uint32_t w, uint32_t h, bool src_is_be)
{
    if (!s_ready || !src || w == 0 || h == 0 || w > OUT_W || h > OUT_H) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < SLOTS; ++i) {
        if (i != s_pending && i != s_inflight) { idx = i; break; }
    }
    xSemaphoreGive(s_lock);
    if (idx < 0) return;

    size_t bytes = (size_t)w * h * 2;
    if (!slot_reserve(idx, bytes)) return;

    uint8_t *dst = s_slot_buf[idx];
    if (src_is_be) {
        memcpy(dst, src, bytes);
    } else if ((((uintptr_t)src) & 3) == 0) {
        const uint32_t *s32 = (const uint32_t *)src;
        uint32_t *d32 = (uint32_t *)dst;
        size_t n32 = bytes / 4;
        for (size_t i = 0; i < n32; ++i) {
            uint32_t x = s32[i];
            d32[i] = ((x & 0x00FF00FFu) << 8) | ((x >> 8) & 0x00FF00FFu);
        }
        if (bytes & 2) ((uint16_t *)dst)[bytes / 2 - 1] = __builtin_bswap16(src[bytes / 2 - 1]);
    } else {
        uint16_t *d16 = (uint16_t *)dst;
        for (size_t i = 0; i < bytes / 2; ++i) d16[i] = __builtin_bswap16(src[i]);
    }
    s_slot_w[idx] = (uint16_t)w;
    s_slot_h[idx] = (uint16_t)h;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pending = idx;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

/* ───────────────────────── audio ───────────────────────── */
static i2s_chan_handle_t s_i2s;
static uint32_t s_rs_pos = 1u << 16;     /* resampler position, biased by one input sample */
static int16_t s_rs_prev[2];

static esp_err_t audio_init(void)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    esp_err_t e = i2s_new_channel(&cc, &s_i2s, NULL);
    if (e != ESP_OK) return e;

    i2s_std_config_t sc = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCK,
            .ws   = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    e = i2s_channel_init_std_mode(s_i2s, &sc);
    if (e == ESP_OK) e = i2s_channel_enable(s_i2s);
    if (e != ESP_OK) {
        i2s_del_channel(s_i2s);
        s_i2s = NULL;
    }
    return e;
}

bool hdmi_out_audio_active(void)
{
    return s_ready && s_audio_ok;
}

void hdmi_out_audio_write(const int16_t *in, int frames, int rate)
{
    if (!hdmi_out_audio_active() || !in || frames <= 0 || rate <= 0) return;

    /* PSRAM, not a static array: the SNES app has no spare internal RAM (it links by a few KB) */
    enum { OUT_FRAMES = 1024 };
    static int16_t *out;
    size_t written;
    if (!out) {
        out = heap_caps_malloc(OUT_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!out) return;
    }

    if (rate == 48000) {
        for (int off = 0; off < frames; off += OUT_FRAMES) {
            int n = frames - off;
            if (n > OUT_FRAMES) n = OUT_FRAMES;
            i2s_channel_write(s_i2s, in + off * 2, (size_t)n * 4, &written, portMAX_DELAY);
        }
        return;
    }

    /* linear interpolation to 48 kHz; position kept across calls so blocks join seamlessly */
    const uint32_t step = (uint32_t)(((uint64_t)rate << 16) / 48000);
    uint32_t u = s_rs_pos;
    int produced = 0;
    for (;;) {
        int i = (int)(u >> 16) - 1;            /* sample(i) and sample(i+1) bracket the position */
        if (i + 1 >= frames) break;
        uint32_t frac = u & 0xFFFF;
        for (int c = 0; c < 2; ++c) {
            int a = (i < 0) ? s_rs_prev[c] : in[i * 2 + c];
            int b = in[(i + 1) * 2 + c];
            out[produced * 2 + c] = (int16_t)(a + (((b - a) * (int)frac) >> 16));
        }
        u += step;
        if (++produced == OUT_FRAMES) {
            i2s_channel_write(s_i2s, out, (size_t)produced * 4, &written, portMAX_DELAY);
            produced = 0;
        }
    }
    if (produced) i2s_channel_write(s_i2s, out, (size_t)produced * 4, &written, portMAX_DELAY);
    s_rs_pos = u - ((uint32_t)frames << 16);
    s_rs_prev[0] = in[(frames - 1) * 2];
    s_rs_prev[1] = in[(frames - 1) * 2 + 1];
}

/* ───────────────────────── bring-up and sender task ───────────────────────── */
static bool bring_up(void)
{
    uint8_t id[3];
    if (!lt_detect(id)) {
        ESP_LOGI(TAG, "no Module Display found on the M-Bus (HDMI off)");
        return false;
    }

    /* reset the transmitter (same table as M5GFX HDMI_Trans::reset) */
    lt_wr_set(lt_reset, sizeof(lt_reset));

    s_small = heap_caps_aligned_alloc(64, 64, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_small) return false;

    static const uint32_t wr_hz[] = { 80000000, 66000000, 53000000, 40000000, 33000000, 26000000, 20000000, 13000000 };
    static const uint32_t rd_hz[] = { 16000000, 13000000, 10000000, 8000000, 6000000, 5000000, 4000000, 3000000 };
    int wi = 0, ri = 0;
    if (spi_up(wr_hz[wi], rd_hz[ri]) != ESP_OK) { ESP_LOGE(TAG, "SPI init failed"); return false; }

    uint32_t fid = fpga_read_id();
    ESP_LOGI(TAG, "FPGA ID before load: 0x%08x", (unsigned)fid);
    if (!fpga_id_is_hd(fid)) {
        size_t len = 0;
        uint8_t *img = read_file(FPGA_IMAGE_PATH, &len);
        if (!img) {
            ESP_LOGW(TAG, "FPGA is not configured and %s is missing: copy SDcard/system/hdmi_fpga.bin there", FPGA_IMAGE_PATH);
            spi_down();
            return false;
        }
        /* The first load after a firmware flash has been seen to leave the FPGA mute: load again. */
        bool alive = false;
        for (int attempt = 1; attempt <= 3 && !alive; ++attempt) {
            spi_down();
            fpga_load(img, len);
            if (spi_up(wr_hz[wi], rd_hz[ri]) != ESP_OK) { ESP_LOGE(TAG, "SPI re-init failed"); free(img); return false; }

            /* wait for the FPGA to come up: it reads 0xFFFFFFFF when idle */
            int retry = 200;   /* ~2 s */
            cs(0);
            spi_read32();
            uint32_t v;
            do {
                vTaskDelay(pdMS_TO_TICKS(10));
                v = spi_read32();
            } while (v != 0xFFFFFFFFu && --retry);
            cs(1);
            if (retry == 0) ESP_LOGW(TAG, "FPGA did not become idle (last read 0x%08x)", (unsigned)v);
            alive = fpga_id_is_hd(fpga_read_id());
            if (!alive) ESP_LOGW(TAG, "FPGA silent after load attempt %d", attempt);
        }
        free(img);
    }

    int tries = 8;
    for (;;) {
        fid = fpga_read_id();
        if (fpga_id_is_hd(fid)) break;
        ESP_LOGW(TAG, "FPGA ID read 0x%08x (SPI write %u / read %u MHz)", (unsigned)fid,
                 (unsigned)(wr_hz[wi] / 1000000), (unsigned)(rd_hz[ri] / 1000000));
        if (--tries == 0) { ESP_LOGW(TAG, "FPGA ID read failed"); return false; }
        if (fid == 0 || fid == 0xFFFFFFFFu) {       /* MISO does not change: assume the command was lost */
            if (wi + 1 < (int)(sizeof(wr_hz) / sizeof(wr_hz[0]))) ++wi;
        } else {
            if (ri + 1 < (int)(sizeof(rd_hz) / sizeof(rd_hz[0]))) ++ri;
        }
        spi_up(wr_hz[wi], rd_hz[ri]);
    }
    ESP_LOGI(TAG, "FPGA ID 'HD' v%u.%u, SPI write %u MHz / read %u MHz",
             (unsigned)((fid >> 16) & 0xFF), (unsigned)((fid >> 24) & 0xFF),
             (unsigned)(wr_hz[wi] / 1000000), (unsigned)(rd_hz[ri] / 1000000));

    apply_mode(1);
    if (!lt_init()) return false;
    return true;
}

static void hdmi_task(void *arg)
{
    (void)arg;
    s_lock = xSemaphoreCreateMutex();
    if (!bring_up()) {
        spi_down();
        if (s_lt_dev) { i2c_master_bus_rm_device(s_lt_dev); s_lt_dev = NULL; }
        s_i2c_mode = I2C_NONE;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_err_t ae = audio_init();
    s_audio_ok = (ae == ESP_OK);
    if (!s_audio_ok) ESP_LOGW(TAG, "HDMI audio init failed (0x%x), video only", ae);

    s_ready = true;
    ESP_LOGI(TAG, "HDMI output ready%s", s_audio_ok ? " (audio on HDMI)" : "");

    int fails = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            int idx = s_pending;
            s_pending = -1;
            s_inflight = idx;
            xSemaphoreGive(s_lock);
            if (idx < 0) break;

            int w = s_slot_w[idx], h = s_slot_h[idx];
            int scale = pick_scale(w, h);
            if (scale != s_scale) apply_mode(scale);
            if (w != s_last_w || h != s_last_h) {
                fpga_clear(s_mem_w, s_mem_h);
                s_last_w = w;
                s_last_h = h;
            }
            if (send_frame(s_slot_buf[idx], w, h)) {
                fails = 0;
            } else if (++fails == 30) {
                ESP_LOGE(TAG, "FPGA stopped answering, HDMI off");
                s_ready = false;
            }

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_inflight = -1;
            xSemaphoreGive(s_lock);
        }
    }
}

void hdmi_out_start(i2c_master_bus_handle_t i2c)
{
    if (s_started) return;
    s_started = true;
    s_bus = i2c;
    if (xTaskCreate(hdmi_task, "hdmi_out", 8192, NULL, 4, &s_task) != pdPASS) {
        s_started = false;
        ESP_LOGE(TAG, "task create failed");
    }
}

bool hdmi_out_ready(void)
{
    return s_ready;
}
