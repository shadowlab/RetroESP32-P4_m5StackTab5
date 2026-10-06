/* Host stand-in for <avr/eeprom.h> (see host_test.c): a 256-byte array. */
#ifndef FAKE_AVR_EEPROM_H
#define FAKE_AVR_EEPROM_H
#include <stdint.h>

extern uint8_t fake_eeprom[256];
extern int fake_eeprom_writes;
static inline uint8_t eeprom_read_byte(const uint8_t *addr) { return fake_eeprom[(uintptr_t)addr]; }
static inline void eeprom_update_byte(uint8_t *addr, uint8_t v)
{
    if (fake_eeprom[(uintptr_t)addr] != v) {
        fake_eeprom[(uintptr_t)addr] = v;
        fake_eeprom_writes++;
    }
}
#endif
