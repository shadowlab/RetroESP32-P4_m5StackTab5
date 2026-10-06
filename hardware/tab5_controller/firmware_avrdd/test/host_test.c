/*
 * Host test for firmware_avrdd/main.c: builds the real firmware against fake
 * registers and replays what the Tab5 driver (components/tab5_ctrl) does over
 * I2C. Run with ./run_host_test.sh
 */
#include <stdio.h>
#include <string.h>

#define main firmware_main
#include "../main.c"
#undef main

PORT_t PORTA, PORTC, PORTD, PORTF;
VPORT_t VPORTC;
uint8_t fake_eeprom[256];
int fake_eeprom_writes;
TWI_t TWI0;
TCB_t TCB0;
ADC_t ADC0;
__typeof__(VREF) VREF;
__typeof__(PORTMUX) PORTMUX;
__typeof__(CLKCTRL) CLKCTRL;

static int fails;
static void check(const char *what, long got, long want)
{
    printf("  %-44s got %-6ld want %-6ld %s\n", what, got, want, got == want ? "ok" : "FAIL");
    fails += got != want;
}

/* One I2C client event: set SSTATUS, run the ISR, report the response command */
static uint8_t twi(uint8_t status)
{
    TWI0.SSTATUS = status;
    TWI0_TWIS_vect();
    return TWI0.SCTRLB;
}

/* Host write [reg, data...] then stop */
static void host_write(uint8_t reg, const uint8_t *data, int n)
{
    twi(TWI_APIF_bm | TWI_AP_bm);                       /* address + W */
    TWI0.SDATA = reg;  twi(TWI_DIF_bm);
    for (int i = 0; i < n; i++) { TWI0.SDATA = data[i]; twi(TWI_DIF_bm); }
    twi(TWI_APIF_bm);                                   /* stop */
}

/* Host read of n bytes (ACK all but the last), like i2c_master_transmit_receive */
static void host_read(uint8_t reg, uint8_t *out, int n)
{
    host_write(reg, NULL, 0);
    twi(TWI_APIF_bm | TWI_AP_bm | TWI_DIR_bm);          /* repeated start + R */
    for (int i = 0; i < n; i++) {
        uint8_t cmd = twi(TWI_DIF_bm | TWI_DIR_bm);     /* host ACKed the previous byte */
        if (cmd != TWI_SCMD_RESPONSE_gc) { printf("  read ended early at byte %d\n", i); fails++; }
        out[i] = TWI0.SDATA;
    }
    /* host NACKs the last byte: the client must complete, not send more */
    check("client completes after host NACK", twi(TWI_DIF_bm | TWI_DIR_bm | TWI_RXACK_bm), TWI_SCMD_COMPTRANS_gc);
    twi(TWI_APIF_bm);                                   /* stop */
}

static void press(PORT_t *port, int pin, int down)
{
    if (down) port->IN &= (uint8_t)~(1 << pin); else port->IN |= (uint8_t)(1 << pin);
}

/* n scans of the firmware's own sample + debounce, as its 1 ms main loop does */
static void scan(int n)
{
    for (int i = 0; i < n; i++)
        debounce_step(buttons_sample());
}

int main(void)
{
    /* SNES board: ID straps on PA0 + PA1 pulled low (console 3), all buttons up */
    PORTA.IN = 0xFF & (uint8_t)~0x03;
    PORTC.IN = PORTD.IN = PORTF.IN = 0xFF;
    s_console = read_console_id();
    buttons_init();
    analog_pins_init();
    twi_init();
    printf("boot\n");
    check("console id from straps", s_console, RP_CONSOLE_SNES);
    check("TWI address", TWI0.SADDR, 0x6D << 1);
    check("pull-up on PD1 (UP)", PORTD.PIN1CTRL & PORT_PULLUPEN_bm, PORT_PULLUPEN_bm);
    printf("unused pins don't float\n");
    check("fitted strap PA0: pull-up off", PORTA.PIN0CTRL & PORT_PULLUPEN_bm, 0);
    check("fitted strap PA1: pull-up off", PORTA.PIN1CTRL & PORT_PULLUPEN_bm, 0);
    check("unfitted strap PA6: pull-up kept", PORTA.PIN6CTRL & PORT_PULLUPEN_bm, PORT_PULLUPEN_bm);
    check("unfitted strap PA7: pull-up kept", PORTA.PIN7CTRL & PORT_PULLUPEN_bm, PORT_PULLUPEN_bm);
    check("AN0 PA4: input buffer off", PORTA.PIN4CTRL, PORT_ISC_INPUT_DISABLE_gc);
    check("AN1 PA5: input buffer off", PORTA.PIN5CTRL, PORT_ISC_INPUT_DISABLE_gc);

    printf("probe (tab5_ctrl.c probe())\n");
    uint8_t v[RP_EXT_BLOCK_LEN];
    host_read(RP_REG_FW_VERSION, v, 1);
    check("FW_VERSION", v[0], FW_VERSION);
    host_read(RP_REG_INFO, v, 8);
    check("signature RPAD", memcmp(v, "RPAD", 4), 0);
    check("protocol version", v[RP_INFO_PROTO_VER], RP_PROTO_VERSION);
    check("console id", v[RP_INFO_CONSOLE_ID], RP_CONSOLE_SNES);
    check("analog count (SNES has none)", v[RP_INFO_ANALOG_COUNT], 0);
    uint8_t zero = 0;
    host_write(RP_REG_INT_CFG, &zero, 1);
    host_write(RP_REG_EVENT_NUM, &zero, 1);
    host_read(RP_REG_INT_CFG, v, 1);
    check("INT_CFG written 0 reads back 0", v[0], 0);

    printf("poll (tab5_ctrl.c poll_retropad())\n");
    press(&PORTD, 1, 1);       /* UP  = slot 0, PD1 */
    press(&PORTC, 0, 1);       /* A   = slot 7, PC0 */
    press(&PORTF, 1, 1);       /* R   = slot 12, PF1 */
    scan(2);
    host_read(RP_REG_BUTTONS, v, 8);
    check("not reported before debounce", v[0] | v[1] | v[2] | v[3], 0);
    scan(DEBOUNCE_SCANS + 1);
    host_read(RP_REG_BUTTONS, v, 8);
    uint32_t mask = v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
    check("UP|A|R after debounce", mask, RP_BIT(RP_BTN_UP) | RP_BIT(RP_BTN_A) | RP_BIT(RP_BTN_R));
    press(&PORTD, 1, 0);
    scan(DEBOUNCE_SCANS + 1);
    host_read(RP_REG_BUTTONS, v, 4);
    mask = v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
    check("UP released", mask, RP_BIT(RP_BTN_A) | RP_BIT(RP_BTN_R));

    printf("NES, Game Boy, Master System and Genesis boards (same firmware, map chosen by the ID straps)\n");
    PORTA.IN = 0xFF & (uint8_t)~0x01;                   /* ID0 only -> console 1 (NES / GB) */
    check("NES console id from straps", read_console_id(), RP_CONSOLE_NES);
    s_console = RP_CONSOLE_NES;
    PORTC.IN = PORTD.IN = PORTF.IN = 0xFF;
    press(&PORTD, 6, 1);                                /* slot 5 = A on NES */
    press(&PORTC, 1, 1);                                /* slot 8 = MENU on NES */
    check("NES PD6 -> A, PC1 -> MENU", buttons_sample(), RP_BIT(RP_BTN_A) | RP_BIT(RP_BTN_MENU));
    press(&PORTF, 0, 1);                                /* slot 11 not fitted on NES */
    check("NES unused slot PF0 ignored", buttons_sample(), RP_BIT(RP_BTN_A) | RP_BIT(RP_BTN_MENU));
    PORTA.IN = 0xFF & (uint8_t)~(1 << 1);               /* ID1 only -> console 2 */
    check("Game Boy console id from straps", read_console_id(), RP_CONSOLE_GB);
    s_console = RP_CONSOLE_GB;
    PORTC.IN = PORTD.IN = PORTF.IN = 0xFF;
    press(&PORTD, 7, 1);                                /* slot 6 = SELECT on GB */
    press(&PORTC, 0, 1);                                /* slot 7 = START on GB */
    check("GB PD7 -> SELECT, PC0 -> START", buttons_sample(),
          RP_BIT(RP_BTN_SELECT) | RP_BIT(RP_BTN_START));
    PORTA.IN = 0xFF & (uint8_t)~(1 << 6);               /* ID2 only -> console 4 */
    check("Master System console id from straps", read_console_id(), RP_CONSOLE_SMS);
    s_console = RP_CONSOLE_SMS;
    PORTC.IN = PORTD.IN = PORTF.IN = 0xFF;
    press(&PORTD, 5, 1);                                /* slot 4 = button 1 (B bit) */
    press(&PORTC, 0, 1);                                /* slot 7 = START (Pause) */
    check("SMS PD5 -> 1 (B), PC0 -> START", buttons_sample(),
          RP_BIT(RP_BTN_B) | RP_BIT(RP_BTN_START));
    PORTA.IN = 0xFF & (uint8_t)~((1 << 0) | (1 << 6));  /* ID0 + ID2 -> console 5 */
    check("Genesis console id from straps", read_console_id(), RP_CONSOLE_GENESIS);
    s_console = RP_CONSOLE_GENESIS;
    PORTC.IN = PORTD.IN = PORTF.IN = 0xFF;
    press(&PORTD, 6, 1);                                /* slot 5 = X */
    press(&PORTC, 2, 1);                                /* slot 9 = Z */
    press(&PORTC, 3, 1);                                /* slot 10 = MODE (SELECT bit) */
    check("Genesis PD6 -> X, PC2 -> Z, PC3 -> MODE", buttons_sample(),
          RP_BIT(RP_BTN_X) | RP_BIT(RP_BTN_Z) | RP_BIT(RP_BTN_SELECT));

    printf("console-select board: straps read SELECT_STRAP_ID\n");
    memset(fake_eeprom, 0xFF, sizeof fake_eeprom);       /* blank EEPROM */
    PORTA.IN = 0xFF & (uint8_t)~((SELECT_STRAP_ID & 0x03) | (SELECT_STRAP_ID & 0x0C) << 4);
    check("straps read the select code", read_console_id(), SELECT_STRAP_ID);
    PORTC.PIN2CTRL = PORT_PULLUPEN_bm;                   /* as buttons_init() left it */
    select_init();
    check("blank EEPROM starts at the first console", s_console, select_console[0]);
    check("  LED shows it", s_led_shown, 0);
    check("  PC2 output, low, no pull-up", (VPORTC.DIR & PIN2_bm) | (VPORTC.OUT & PIN2_bm) | PORTC.PIN2CTRL, PIN2_bm);
    host_read(RP_REG_INFO, v, 8);
    check("  info block reports NES, never the select code", v[RP_INFO_CONSOLE_ID], RP_CONSOLE_NES);
    int presses = 0;
    for (int ms = 0; ms < SEL_DEBOUNCE_MS - 1; ms++)
        presses += select_step(true);
    check("bounce shorter than the debounce: ignored", presses, 0);
    check("  ignored release resets the count", select_step(false), 0);
    for (int ms = 0; ms < SEL_DEBOUNCE_MS; ms++)
        presses += select_step(true);
    check("held press steps once", presses, 1);
    check("  console now Game Boy", s_console, RP_CONSOLE_GB);
    check("  saved to EEPROM", fake_eeprom[0], 1);
    check("  LED shows it", s_led_shown, 1);
    for (int ms = 0; ms < 500; ms++)
        presses += select_step(true);
    check("holding does not repeat", presses, 1);
    for (int ms = 0; ms < SEL_DEBOUNCE_MS; ms++)
        presses += select_step(false);
    for (int ms = 0; ms < SEL_DEBOUNCE_MS; ms++)
        presses += select_step(true);
    check("next press: Master System", s_console, RP_CONSOLE_SMS);
    host_read(RP_REG_INFO, v, 8);
    check("  info block reports SMS to the Tab5", v[RP_INFO_CONSOLE_ID], RP_CONSOLE_SMS);
    for (int ms = 0; ms < SEL_DEBOUNCE_MS; ms++)
        select_step(false);
    for (int ms = 0; ms < SEL_DEBOUNCE_MS; ms++)
        select_step(true);
    check("wraps back to NES", s_console, RP_CONSOLE_NES);
    check("  EEPROM written once per press", fake_eeprom_writes, 3);
    fake_eeprom[0] = 2;                                  /* power cycle with SMS saved */
    select_init();
    check("boot restores the saved console", s_console, RP_CONSOLE_SMS);
    fake_eeprom[0] = 7;                                  /* corrupt index */
    select_init();
    check("out-of-range EEPROM falls back to the first", s_console, select_console[0]);
    s_select = false;

    printf("strap boards: the ID is re-read while running\n");
    set_console(RP_CONSOLE_NES);
    check("one reading of a new ID is not enough", console_id_step(RP_CONSOLE_SMS), 0);
    check("  console unchanged", s_console, RP_CONSOLE_NES);
    check("second equal reading switches", console_id_step(RP_CONSOLE_SMS), 1);
    check("  console now SMS", s_console, RP_CONSOLE_SMS);
    host_read(RP_REG_INFO, v, 8);
    check("  info block reports SMS to the Tab5", v[RP_INFO_CONSOLE_ID], RP_CONSOLE_SMS);
    check("a single GB glitch: no change", console_id_step(RP_CONSOLE_GB), 0);
    check("  back at SMS resets the candidate", console_id_step(RP_CONSOLE_SMS), 0);
    check("  console still SMS", s_console, RP_CONSOLE_SMS);
    PORTA.IN = 0xFF & (uint8_t)~(1 << 1);               /* ID1 grounded only */
    check("strap read after a move: Game Boy", read_console_id(), RP_CONSOLE_GB);
    PORTA.IN = 0xFF & (uint8_t)~(1 << 0);               /* back to ID0 */
    check("previously grounded pin gets its pull-up back", read_console_id(), RP_CONSOLE_NES);
    check("  ID1 pull-up re-enabled", PORTA.PIN1CTRL & PORT_PULLUPEN_bm, PORT_PULLUPEN_bm);

    printf("unknown console id falls back to the generic order, never all-UP\n");
    s_console = RP_CONSOLE_NEOGEO;
    PORTC.IN = PORTD.IN = PORTF.IN = 0xFF;
    press(&PORTF, 1, 1);       /* slot 12 */
    check("slot 12 -> MENU (generic)", buttons_sample(), RP_BIT(RP_BTN_MENU));

    printf("%s\n", fails ? "FAILED" : "ALL PASSED");
    return fails != 0;
}
