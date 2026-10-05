/* Host stand-in for <avr/interrupt.h> (see host_test.c). */
#ifndef FAKE_AVR_INTERRUPT_H
#define FAKE_AVR_INTERRUPT_H
#define ISR(vec) void vec(void)
#define sei() ((void)0)
#define cli() ((void)0)
#endif
