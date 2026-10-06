/*
 * RetroPad firmware for the console controller boards (AVR32DD28, SOIC-28).
 * SPDX-License-Identifier: MIT
 *
 * Speaks the RetroPad I2C protocol (address 0x6D, M5Stack keyboard register
 * map plus the block in components/tab5_ctrl/include/retropad_proto.h). Only
 * the registers the Tab5 host driver touches are implemented; the stock M5Stack
 * keyboard's event/HID/char modes are not.
 *
 * Pins (see boards/layout.py AVRDD_SLOTS and pinmap.h):
 *   PD1-PD7, PC0-PC3, PF0-PF1  13 button slots, switch to GND, internal pull-up
 *   PA2 SDA, PA3 SCL           I2C client to the Tab5 (TWI0 default pins)
 *   PA0, PA1, PA6, PA7         console-ID straps ID0..ID3 (0R to GND = bit set)
 *   PA4, PA5                   AN0 / AN1 (AIN24 / AIN25), Atari paddle/analog boards
 *   PC3, PC2                   console-select boards: select button, SK6812 status LED
 *   PF7 UPDI, PF6 RESET        programming
 */
#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <stdbool.h>
#include <stdint.h>

#include "pinmap.h"
#include "retropad_proto.h"

#define F_CPU_HZ        24000000UL
#define FW_VERSION      0x22            /* 0x2x = AVR DD build */
#define SCAN_US         1000            /* one button scan per millisecond */
#define DEBOUNCE_SCANS  5               /* stable for 5 ms before a change counts */

/* Consoles whose boards carry potentiometers on AN0/AN1 */
#define HAS_ANALOG(id)  ((id) == RP_CONSOLE_A2600 || (id) == RP_CONSOLE_A5200)

static uint8_t s_console;
static uint8_t s_analog_count;
static volatile uint32_t s_buttons;    /* debounced canonical mask */
static volatile uint8_t s_analog[2];

/* Stock registers the host writes during probe; kept so reads return them */
static volatile uint8_t s_int_cfg = 0x07;
static volatile uint8_t s_kb_mode;

/* ── clocks and timing ─────────────────────────────────────────────────── */
static void clock_init(void)
{
    _PROTECTED_WRITE(CLKCTRL.OSCHFCTRLA, CLKCTRL_FRQSEL_24M_gc);
    _PROTECTED_WRITE(CLKCTRL.MCLKCTRLB, 0);                 /* no prescaler */
}

static void timer_init(void)
{
    /* TCB0 periodic flag every SCAN_US: CLK_PER / 2 = 12 MHz -> 12 ticks/us */
    TCB0.CCMP = (uint16_t)(F_CPU_HZ / 2 / 1000000UL * SCAN_US - 1);
    TCB0.CTRLB = TCB_CNTMODE_INT_gc;
    TCB0.CTRLA = TCB_CLKSEL_DIV2_gc | TCB_ENABLE_bm;
}

static bool timer_tick(void)
{
    if (!(TCB0.INTFLAGS & TCB_CAPT_bm))
        return false;
    TCB0.INTFLAGS = TCB_CAPT_bm;
    return true;
}

/* ── straps, buttons, analog ───────────────────────────────────────────── */
static void pullup(PORT_t *port, uint8_t pin)
{
    port->DIRCLR = (uint8_t)(1 << pin);
    (&port->PIN0CTRL)[pin] = PORT_PULLUPEN_bm;
}

static uint8_t read_console_id(void)
{
    static const uint8_t strap_pin[4] = {0, 1, 6, 7};     /* PA0, PA1, PA6, PA7 */
    uint8_t id = 0;
    for (uint8_t i = 0; i < 4; i++)
        pullup(&PORTA, strap_pin[i]);
    for (volatile uint16_t d = 0; d < 2000; d++) {         /* let the pull-ups settle */
    }
    for (uint8_t i = 0; i < 4; i++) {
        if (!(PORTA.IN & (1 << strap_pin[i]))) {
            id |= (uint8_t)(1 << i);
            /* Fitted strap: the pin is tied to GND, so drop the pull-up
             * (it would draw current) and the input buffer. */
            (&PORTA.PIN0CTRL)[strap_pin[i]] = PORT_ISC_INPUT_DISABLE_gc;
        }
        /* Unfitted strap: keep the pull-up so the pin doesn't float */
    }
    return id;
}

static void buttons_init(void)
{
    for (uint8_t s = 0; s < SLOT_COUNT; s++)
        pullup(slot_port[s], slot_pin[s]);
}

static uint32_t buttons_sample(void)
{
    uint32_t mask = 0;
    const uint8_t *bits = slot_bit[s_console];
    for (uint8_t s = 0; s < SLOT_COUNT; s++) {
        if (bits[s] == SLOT_NONE)
            continue;
        if (!(slot_port[s]->IN & (1 << slot_pin[s])))     /* pressed = low */
            mask |= RP_BIT(bits[s]);
    }
    return mask;
}

/* AN0 / AN1 (PA4 / PA5) are analog-only. On boards without pots they are
 * unconnected, so their digital input buffers stay off on every board rather
 * than leaving floating inputs. */
static void analog_pins_init(void)
{
    PORTA.PIN4CTRL = PORT_ISC_INPUT_DISABLE_gc;
    PORTA.PIN5CTRL = PORT_ISC_INPUT_DISABLE_gc;
}

static void analog_init(void)
{
    VREF.ADC0REF = VREF_REFSEL_VDD_gc;
    ADC0.CTRLC = ADC_PRESC_DIV16_gc;                         /* 1.5 MHz ADC clock */
    ADC0.CTRLA = ADC_ENABLE_bm | ADC_RESSEL_12BIT_gc;
}

static uint8_t analog_read(uint8_t muxpos)
{
    ADC0.MUXPOS = muxpos;
    ADC0.COMMAND = ADC_STCONV_bm;
    while (!(ADC0.INTFLAGS & ADC_RESRDY_bm)) {
    }
    return (uint8_t)(ADC0.RES >> 4);                         /* 12-bit -> 8-bit */
}

/* ── console ID: re-read while running ─────────────────────────────────── *
 * The straps are re-read every ID_POLL_MS (not on console-select boards,
 * whose console comes from the select button). A new value takes effect after
 * two equal readings in a row, so a strap being reworked can't flicker it. */
#define ID_POLL_MS 100

static uint8_t s_id_candidate = 0xFF;

static void set_console(uint8_t id)
{
    if (id >= RP_CONSOLE_COUNT)
        id = RP_CONSOLE_GENERIC;
    if (HAS_ANALOG(id) && !s_analog_count)
        analog_init();
    uint8_t sreg = SREG;                    /* also called at boot, before sei() */
    cli();
    s_console = id;
    s_analog_count = HAS_ANALOG(id) ? 2 : 0;
    SREG = sreg;
}

/* One ID reading; returns true when the console changed. */
static bool console_id_step(uint8_t raw)
{
    if (raw == s_console) {
        s_id_candidate = 0xFF;
        return false;
    }
    if (raw != s_id_candidate) {
        s_id_candidate = raw;
        return false;
    }
    s_id_candidate = 0xFF;
    set_console(raw);
    return true;
}

/* ── console-select boards (pinmap.h SELECT_*) ─────────────────────────── *
 * Straps reading SELECT_STRAP_ID mark a board shared by several consoles. Each
 * press of the select button (PC3) steps to the next entry of select_console[],
 * which is saved to EEPROM and shown on the SK6812MINI-E on PC2 in that
 * console's colour. The Tab5 re-reads the console ID, so the launcher follows. */
#define SEL_DEBOUNCE_MS 20
#define LED_REFRESH_MS  1000            /* re-send: the LED's 5 V may come up late */
#define EE_SELECT       ((uint8_t *)0)

static bool s_select;
static uint8_t s_sel_index;
static bool s_sel_down;
static uint8_t s_sel_count;

#ifdef __AVR__
/* One byte to the LED, MSB first, on VPORTC.2. At 24 MHz: 0 = 7 cycles high
 * (0.29 us), 1 = 15 cycles high (0.63 us), 30 cycles (1.25 us) per bit;
 * SK6812: T0H 0.3, T1H 0.6, period 1.25 +-0.15 us. sbi/cbi on a VPORT take
 * one cycle. Interrupts are off (I2C clock-stretches meanwhile). */
static void led_byte(uint8_t byte)
{
    uint8_t bits;
    __asm__ volatile(
        "ldi  %[bits], 8\n"
        "1:\n\t"
        "sbi  %[port], 2\n\t"
        "nop\n\tnop\n\tnop\n\tnop\n\tnop\n\t"
        "sbrs %[byte], 7\n\t"
        "cbi  %[port], 2\n\t"                     /* 0 bit ends here */
        "lsl  %[byte]\n\t"
        "nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\t"
        "cbi  %[port], 2\n\t"                     /* 1 bit ends here */
        "nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\t"
        "nop\n\tnop\n\tnop\n\tnop\n\tnop\n\t"
        "dec  %[bits]\n\t"
        "brne 1b\n"
        : [bits] "=&d"(bits), [byte] "+r"(byte)
        : [port] "I"(_SFR_IO_ADDR(VPORTC.OUT)));
}

static void led_show(void)
{
    const uint8_t *grb = select_grb[s_sel_index];
    uint8_t sreg = SREG;
    cli();
    led_byte(grb[0]);
    led_byte(grb[1]);
    led_byte(grb[2]);
    SREG = sreg;
}
#else
static uint8_t s_led_shown = 0xFF;      /* host test: index last sent to the LED */
static void led_show(void) { s_led_shown = s_sel_index; }
#endif

static void select_init(void)
{
    PORTC.PIN2CTRL = 0;                   /* LED data: output, low, no pull-up */
    VPORTC.OUT &= (uint8_t)~PIN2_bm;
    VPORTC.DIR |= PIN2_bm;
    uint8_t i = eeprom_read_byte(EE_SELECT);
    s_sel_index = i < SELECT_COUNT ? i : 0;   /* blank EEPROM reads 0xFF */
    s_select = true;
    set_console(select_console[s_sel_index]);
    led_show();
}

/* One 1 ms sample of the select button (true = pressed); true on a new press. */
static bool select_step(bool pressed)
{
    if (pressed == s_sel_down) {
        s_sel_count = 0;
        return false;
    }
    if (++s_sel_count < SEL_DEBOUNCE_MS)
        return false;
    s_sel_count = 0;
    s_sel_down = pressed;
    if (!pressed)
        return false;
    s_sel_index = (uint8_t)((s_sel_index + 1) % SELECT_COUNT);
    set_console(select_console[s_sel_index]);
    eeprom_update_byte(EE_SELECT, s_sel_index);
    led_show();
    return true;
}

/* ── I2C client: RetroPad register window ─────────────────────────────── */
static uint8_t s_reg;                  /* register pointer */
static bool s_reg_set;                 /* first byte of a write sets the pointer */
static bool s_first_read;              /* no byte sent yet in this read */
static uint8_t s_snapshot[RP_EXT_BLOCK_LEN];

static void snapshot(void)
{
    uint32_t b = s_buttons;
    s_snapshot[RP_INFO_MAGIC0] = 'R';
    s_snapshot[RP_INFO_MAGIC1] = 'P';
    s_snapshot[RP_INFO_MAGIC2] = 'A';
    s_snapshot[RP_INFO_MAGIC3] = 'D';
    s_snapshot[RP_INFO_PROTO_VER] = RP_PROTO_VERSION;
    s_snapshot[RP_INFO_CONSOLE_ID] = s_console;
    s_snapshot[RP_INFO_ANALOG_COUNT] = s_analog_count;
    s_snapshot[RP_INFO_FLAGS] = 0;
    for (uint8_t i = 0; i < 8; i++)
        s_snapshot[8 + i] = 0;                               /* 0x78-0x7F reserved */
    s_snapshot[RP_REG_BUTTONS - RP_REG_INFO + 0] = (uint8_t)b;
    s_snapshot[RP_REG_BUTTONS - RP_REG_INFO + 1] = (uint8_t)(b >> 8);
    s_snapshot[RP_REG_BUTTONS - RP_REG_INFO + 2] = (uint8_t)(b >> 16);
    s_snapshot[RP_REG_BUTTONS - RP_REG_INFO + 3] = (uint8_t)(b >> 24);
    s_snapshot[RP_REG_ANALOG - RP_REG_INFO + 0] = s_analog[0];
    s_snapshot[RP_REG_ANALOG - RP_REG_INFO + 1] = s_analog[1];
    s_snapshot[RP_REG_ANALOG - RP_REG_INFO + 2] = 0;
    s_snapshot[RP_REG_ANALOG - RP_REG_INFO + 3] = 0;
}

static uint8_t reg_read(uint8_t reg)
{
    if (reg >= RP_REG_INFO && reg <= RP_REG_EXT_END)
        return s_snapshot[reg - RP_REG_INFO];
    switch (reg) {
        case RP_REG_INT_CFG:     return s_int_cfg;
        case RP_REG_INT_STAT:    return 0;                   /* no event queue */
        case RP_REG_EVENT_NUM:   return 0;
        case RP_REG_KB_MODE:     return s_kb_mode;
        case RP_REG_KEY_EVENT:   return 0xFF;                /* queue always empty */
        case RP_REG_FW_VERSION:  return FW_VERSION;
        case 0xFF:               return RP_I2C_ADDR_DEFAULT;
        default:                 return 0xFF;
    }
}

static void reg_write(uint8_t reg, uint8_t value)
{
    if (reg == RP_REG_INT_CFG)
        s_int_cfg = value;
    else if (reg == RP_REG_KB_MODE)
        s_kb_mode = value;
    /* everything else (INT_STAT/EVENT_NUM clears, RGB, address) is accepted and ignored */
}

static void twi_init(void)
{
    PORTMUX.TWIROUTEA = PORTMUX_TWI0_DEFAULT_gc;             /* SDA PA2, SCL PA3 */
    TWI0.SADDR = RP_I2C_ADDR_DEFAULT << 1;
    TWI0.SCTRLA = TWI_DIEN_bm | TWI_APIEN_bm | TWI_PIEN_bm | TWI_ENABLE_bm;
}

ISR(TWI0_TWIS_vect)
{
    uint8_t st = TWI0.SSTATUS;

    if (st & (TWI_COLL_bm | TWI_BUSERR_bm)) {
        TWI0.SSTATUS = TWI_COLL_bm | TWI_BUSERR_bm;
        TWI0.SCTRLB = TWI_SCMD_COMPTRANS_gc;
        return;
    }
    if (st & TWI_APIF_bm) {
        if (st & TWI_AP_bm) {                                /* address match */
            if (st & TWI_DIR_bm) {
                snapshot();                                  /* coherent multi-byte read */
                s_first_read = true;
            } else {
                s_reg_set = false;
            }
            TWI0.SCTRLB = TWI_SCMD_RESPONSE_gc;              /* ACK */
        } else {                                             /* stop */
            TWI0.SCTRLB = TWI_SCMD_COMPTRANS_gc;
        }
        return;
    }
    if (st & TWI_DIF_bm) {
        if (st & TWI_DIR_bm) {                               /* host reads */
            if (!s_first_read && (st & TWI_RXACK_bm)) {
                TWI0.SCTRLB = TWI_SCMD_COMPTRANS_gc;         /* host NACKed: done */
            } else {
                s_first_read = false;
                TWI0.SDATA = reg_read(s_reg++);
                TWI0.SCTRLB = TWI_SCMD_RESPONSE_gc;
            }
        } else {                                             /* host writes */
            uint8_t v = TWI0.SDATA;
            if (!s_reg_set) {
                s_reg = v;
                s_reg_set = true;
            } else {
                reg_write(s_reg++, v);
            }
            TWI0.SCTRLB = TWI_SCMD_RESPONSE_gc;
        }
    }
}

/* ── debounce: a change counts once it is stable for DEBOUNCE_SCANS scans ── */
static uint32_t s_candidate;
static uint8_t s_stable;

static void debounce_step(uint32_t now)
{
    if (now != s_candidate) {
        s_candidate = now;
        s_stable = 0;
    } else if (s_stable < DEBOUNCE_SCANS && ++s_stable == DEBOUNCE_SCANS) {
        cli();
        s_buttons = s_candidate;
        sei();
    }
}

/* ── main loop ─────────────────────────────────────────────────────────── */
int main(void)
{
    clock_init();
    buttons_init();
    analog_pins_init();
    uint8_t straps = read_console_id();
    if (straps == SELECT_STRAP_ID)
        select_init();
    else
        set_console(straps);
    snapshot();
    timer_init();
    twi_init();
    sei();

    uint16_t id_ms = 0;
    for (;;) {
        if (!timer_tick())
            continue;
        debounce_step(buttons_sample());
        ++id_ms;
        if (s_select) {
            select_step(!(VPORTC.IN & PIN3_bm));
            if (id_ms >= LED_REFRESH_MS) {
                id_ms = 0;
                led_show();
            }
        } else if (id_ms >= ID_POLL_MS) {
            id_ms = 0;
            console_id_step(read_console_id());
        }
        if (s_analog_count) {
            uint8_t a0 = analog_read(ADC_MUXPOS_AIN24_gc);
            uint8_t a1 = analog_read(ADC_MUXPOS_AIN25_gc);
            cli();
            s_analog[0] = a0;
            s_analog[1] = a1;
            sei();
        }
    }
}
