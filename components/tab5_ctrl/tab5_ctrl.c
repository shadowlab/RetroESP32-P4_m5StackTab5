/*
 * Tab5 keyboard-port controller boards — ESP32-P4 host driver.
 *
 * Protocol: components/tab5_ctrl/include/retropad_proto.h
 * Hardware: hardware/tab5_controller/README.md
 *
 * One task owns the I2C device. While a board is attached it polls every
 * CONFIG_TAB5_CTRL_POLL_MS; after repeated I2C errors the board is treated as
 * unplugged and the port is re-probed once a second, so a different console
 * board can be swapped in without rebooting.
 */

#include "tab5_ctrl.h"

#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"

#if CONFIG_TAB5_CTRL_ENABLE

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_TIMEOUT_MS      20
#define I2C_FREQ_HZ         400000
#define FAIL_LIMIT          5       /* consecutive errors before "unplugged" */
#define REPROBE_MS          1000
#define KB_KEYS             70      /* stock keyboard: 5 rows x 14 columns */

static const char *TAG = "tab5_ctrl";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static TaskHandle_t            s_task;
static portMUX_TYPE            s_lock = portMUX_INITIALIZER_UNLOCKED;

static tab5_ctrl_info_t  s_info;
static volatile uint32_t s_buttons;
static volatile uint8_t  s_analog[2];

/* Stock keyboard: which keys are held, folded into canonical bits. */
static uint8_t s_kb_down[KB_KEYS];

/* Stock M5Stack keyboard key id (row * 14 + col) → canonical button. */
static const struct { uint8_t key; uint8_t btn; } s_kb_map[] = {
    { 53, RP_BTN_UP },     { 67, RP_BTN_DOWN },   { 66, RP_BTN_LEFT },  { 68, RP_BTN_RIGHT },
    { 59, RP_BTN_A },      /* x */               { 58, RP_BTN_B },     /* z */
    { 60, RP_BTN_C },      /* c */               { 45, RP_BTN_X },     /* s */
    { 44, RP_BTN_Y },      /* a */               { 46, RP_BTN_Z },     /* d */
    { 29, RP_BTN_L },      /* q */               { 30, RP_BTN_R },     /* w */
    { 31, RP_BTN_L2 },     /* e */               { 32, RP_BTN_R2 },    /* r */
    { 55, RP_BTN_START },  /* enter */           { 69, RP_BTN_SELECT },/* space */
    {  0, RP_BTN_MENU },   /* esc */             { 13, RP_BTN_VOLUME },/* del */
    { 28, RP_BTN_OPT1 },   /* tab */             { 41, RP_BTN_OPT2 },  /* backspace */
    {  1, RP_BTN_KP1 },    {  2, RP_BTN_KP2 },   {  3, RP_BTN_KP3 },   {  4, RP_BTN_KP4 },
    {  5, RP_BTN_KP5 },    {  6, RP_BTN_KP6 },   {  7, RP_BTN_KP7 },   {  8, RP_BTN_KP8 },
    {  9, RP_BTN_KP9 },    { 10, RP_BTN_KP0 },   { 22, RP_BTN_KPSTAR },/* * */
    { 17, RP_BTN_KPHASH }, /* # */
};

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static void set_detached(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(&s_info, 0, sizeof(s_info));
    s_buttons = 0;
    portEXIT_CRITICAL(&s_lock);
}

/* Identify what is on the port. Returns false if nothing answers. */
static bool probe(void)
{
    if (i2c_master_probe(s_bus, CONFIG_TAB5_CTRL_I2C_ADDR, I2C_TIMEOUT_MS) != ESP_OK)
        return false;

    tab5_ctrl_info_t info = { .present = true };
    reg_read(RP_REG_FW_VERSION, &info.fw_version, 1);

    /* A stock keyboard does not answer 0x70 with fresh data, so the 4-byte
     * signature plus a known protocol version is what marks a RetroPad. */
    uint8_t blk[8] = { 0 };
    if (reg_read(RP_REG_INFO, blk, sizeof(blk)) == ESP_OK &&
        memcmp(blk, "RPAD", 4) == 0 && blk[RP_INFO_PROTO_VER] == RP_PROTO_VERSION &&
        blk[RP_INFO_CONSOLE_ID] < RP_CONSOLE_KEYBOARD) {
        info.retropad     = true;
        info.console      = (rp_console_t)blk[RP_INFO_CONSOLE_ID];
        info.analog_count = blk[RP_INFO_ANALOG_COUNT];
        /* Live state is polled; the event queue and INT line are not needed. */
        reg_write(RP_REG_INT_CFG, 0x00);
    } else {
        info.console = RP_CONSOLE_KEYBOARD;
        reg_write(RP_REG_KB_MODE, 0);       /* Normal mode: row/col events */
        reg_write(RP_REG_INT_CFG, 0x00);
    }
    reg_write(RP_REG_EVENT_NUM, 0);         /* drop anything queued before we came up */
    memset(s_kb_down, 0, sizeof(s_kb_down));

    portENTER_CRITICAL(&s_lock);
    s_info    = info;
    s_buttons = 0;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "%s attached: %s (fw 0x%02X%s)",
             info.retropad ? "RetroPad" : "Tab5 Keyboard",
             tab5_ctrl_console_name(info.console), info.fw_version,
             info.analog_count ? ", analog" : "");
    return true;
}

static esp_err_t poll_retropad(void)
{
    uint8_t buf[8];
    esp_err_t err = reg_read(RP_REG_BUTTONS, buf, sizeof(buf));
    if (err != ESP_OK) return err;

    s_buttons   = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                  ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    s_analog[0] = buf[4];
    s_analog[1] = buf[5];
    return ESP_OK;
}

static esp_err_t poll_keyboard(void)
{
    uint8_t n = 0;
    esp_err_t err = reg_read(RP_REG_EVENT_NUM, &n, 1);
    if (err != ESP_OK) return err;

    for (; n > 0; n--) {
        uint8_t ev;
        err = reg_read(RP_REG_KEY_EVENT, &ev, 1);
        if (err != ESP_OK) return err;
        if (ev == 0xFF) break;
        uint8_t key = ((ev >> 4) & 0x07) * 14 + (ev & 0x0F);
        if (key < KB_KEYS) s_kb_down[key] = (ev & 0x80) ? 1 : 0;
    }

    uint32_t mask = 0;
    for (size_t i = 0; i < sizeof(s_kb_map) / sizeof(s_kb_map[0]); i++) {
        if (s_kb_down[s_kb_map[i].key]) mask |= RP_BIT(s_kb_map[i].btn);
    }
    s_buttons = mask;
    return ESP_OK;
}

static void poll_task(void *arg)
{
    int fails = 0;
    bool attached = probe();

    for (;;) {
        if (!attached) {
            vTaskDelay(pdMS_TO_TICKS(REPROBE_MS));
            attached = probe();
            fails = 0;
            continue;
        }

        esp_err_t err = s_info.retropad ? poll_retropad() : poll_keyboard();
        if (err == ESP_OK) {
            fails = 0;
        } else if (++fails >= FAIL_LIMIT) {
            ESP_LOGI(TAG, "board removed");
            set_detached();
            attached = false;
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_TAB5_CTRL_POLL_MS));
    }
}

esp_err_t tab5_ctrl_init(void)
{
    if (s_task) return ESP_OK;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = -1,    /* any free controller */
        .sda_io_num        = CONFIG_TAB5_CTRL_I2C_SDA,
        .scl_io_num        = CONFIG_TAB5_CTRL_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags             = { .enable_internal_pullup = 1 },
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed (%s)", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = CONFIG_TAB5_CTRL_I2C_ADDR,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
        return err;
    }

    if (xTaskCreate(poll_task, "tab5_ctrl", 3072, NULL, 3, &s_task) != pdPASS) {
        i2c_master_bus_rm_device(s_dev);
        i2c_del_master_bus(s_bus);
        s_dev = NULL;
        s_bus = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "keyboard port on SDA=%d SCL=%d addr=0x%02X",
             CONFIG_TAB5_CTRL_I2C_SDA, CONFIG_TAB5_CTRL_I2C_SCL, CONFIG_TAB5_CTRL_I2C_ADDR);
    return ESP_OK;
}

void tab5_ctrl_get_info(tab5_ctrl_info_t *info)
{
    if (!info) return;
    portENTER_CRITICAL(&s_lock);
    *info = s_info;
    portEXIT_CRITICAL(&s_lock);
}

uint32_t tab5_ctrl_get_buttons(void)
{
    return s_buttons;
}

int tab5_ctrl_get_analog(int channel)
{
    if (channel < 0 || channel >= s_info.analog_count || channel >= 2) return -1;
    return s_analog[channel];
}

#else /* !CONFIG_TAB5_CTRL_ENABLE */

esp_err_t tab5_ctrl_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void tab5_ctrl_get_info(tab5_ctrl_info_t *info)
{
    if (info) memset(info, 0, sizeof(*info));
}

uint32_t tab5_ctrl_get_buttons(void)
{
    return 0;
}

int tab5_ctrl_get_analog(int channel)
{
    (void)channel;
    return -1;
}

#endif /* CONFIG_TAB5_CTRL_ENABLE */

const char *tab5_ctrl_console_name(rp_console_t console)
{
    static const char *const names[RP_CONSOLE_COUNT] = {
        [RP_CONSOLE_GENERIC]  = "Generic",
        [RP_CONSOLE_NES]      = "NES",
        [RP_CONSOLE_GB]       = "Game Boy",
        [RP_CONSOLE_SNES]     = "SNES",
        [RP_CONSOLE_SMS]      = "Master System",
        [RP_CONSOLE_GENESIS]  = "Genesis",
        [RP_CONSOLE_PCE]      = "PC Engine",
        [RP_CONSOLE_A2600]    = "Atari 2600",
        [RP_CONSOLE_A7800]    = "Atari 7800",
        [RP_CONSOLE_LYNX]     = "Atari Lynx",
        [RP_CONSOLE_A5200]    = "Atari 800/5200",
        [RP_CONSOLE_COLECO]   = "ColecoVision",
        [RP_CONSOLE_NEOGEO]   = "Neo Geo",
        [RP_CONSOLE_SPECTRUM] = "ZX Spectrum",
        [RP_CONSOLE_RESERVED] = "Reserved",
        [RP_CONSOLE_KEYBOARD] = "Tab5 Keyboard",
    };
    return ((unsigned)console < RP_CONSOLE_COUNT) ? names[console] : "Unknown";
}
