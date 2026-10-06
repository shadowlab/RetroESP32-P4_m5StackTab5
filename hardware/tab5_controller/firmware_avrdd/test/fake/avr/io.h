/* Minimal stand-in for <avr/io.h> so main.c builds on the host (see host_test.c).
 * Only the registers and bit names main.c uses; values match ioavr32dd28.h. */
#ifndef FAKE_AVR_IO_H
#define FAKE_AVR_IO_H
#include <stdint.h>

typedef struct { volatile uint8_t DIR, DIRSET, DIRCLR, IN; volatile uint8_t PIN0CTRL, PIN1CTRL, PIN2CTRL, PIN3CTRL,
                 PIN4CTRL, PIN5CTRL, PIN6CTRL, PIN7CTRL; } PORT_t;
typedef struct { volatile uint8_t SCTRLA, SCTRLB, SSTATUS, SADDR, SDATA; } TWI_t;
typedef struct { volatile uint8_t CTRLA, CTRLB, INTFLAGS; volatile uint16_t CCMP; } TCB_t;
typedef struct { volatile uint8_t CTRLA, CTRLC, MUXPOS, COMMAND, INTFLAGS; volatile uint16_t RES; } ADC_t;

extern PORT_t PORTA, PORTC, PORTD, PORTF;
extern TWI_t TWI0;
extern TCB_t TCB0;
extern ADC_t ADC0;
extern struct { volatile uint8_t ADC0REF; } VREF;
extern struct { volatile uint8_t TWIROUTEA; } PORTMUX;
extern struct { volatile uint8_t OSCHFCTRLA, MCLKCTRLB; } CLKCTRL;

#define _PROTECTED_WRITE(reg, v) ((reg) = (v))
#define CLKCTRL_FRQSEL_24M_gc (0x09 << 2)
#define TCB_CNTMODE_INT_gc 0x00
#define TCB_CLKSEL_DIV2_gc (0x01 << 1)
#define TCB_ENABLE_bm 0x01
#define TCB_CAPT_bm 0x01
static volatile uint8_t SREG;
#define PORT_PULLUPEN_bm 0x08
#define PORT_ISC_INPUT_DISABLE_gc 0x04
#define VREF_REFSEL_VDD_gc 0x05
#define ADC_PRESC_DIV16_gc 0x04
#define ADC_ENABLE_bm 0x01
#define ADC_RESSEL_12BIT_gc 0x00
#define ADC_STCONV_bm 0x01
#define ADC_RESRDY_bm 0x01
#define ADC_MUXPOS_AIN24_gc 0x18
#define ADC_MUXPOS_AIN25_gc 0x19
#define PORTMUX_TWI0_DEFAULT_gc 0x00
#define TWI_DIEN_bm 0x80
#define TWI_APIEN_bm 0x40
#define TWI_PIEN_bm 0x20
#define TWI_ENABLE_bm 0x01
#define TWI_AP_bm 0x01
#define TWI_DIR_bm 0x02
#define TWI_COLL_bm 0x08
#define TWI_BUSERR_bm 0x04
#define TWI_RXACK_bm 0x10
#define TWI_APIF_bm 0x40
#define TWI_DIF_bm 0x80
#define TWI_SCMD_RESPONSE_gc 0x03
#define TWI_SCMD_COMPTRANS_gc 0x02
#endif
