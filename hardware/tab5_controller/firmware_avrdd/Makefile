# RetroPad firmware for AVR DD boards (AVR32DD28).
#
#   make                 # needs an avr-gcc with AVR DD support (GCC 13+ / avr-libc 2.2+)
#   make flash PORT=/dev/ttyUSB0      # SerialUPDI (USB-serial adapter + resistor)
#
# AVR_GCC_DIR can point at an unpacked toolchain, e.g. Zak Kemble's avr-gcc-14 build.

MCU      ?= avr32dd28
AVRDUDE_PART ?= avr32dd28
PORT     ?= /dev/ttyUSB0
AVR_GCC_DIR ?=
CC       := $(if $(AVR_GCC_DIR),$(AVR_GCC_DIR)/bin/,)avr-gcc
OBJCOPY  := $(if $(AVR_GCC_DIR),$(AVR_GCC_DIR)/bin/,)avr-objcopy
SIZE     := $(if $(AVR_GCC_DIR),$(AVR_GCC_DIR)/bin/,)avr-size

PROTO    := ../../../components/tab5_ctrl/include
CFLAGS   := -mmcu=$(MCU) -Os -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -I$(PROTO) -I.
LDFLAGS  := -mmcu=$(MCU) -Wl,--gc-sections

all: retropad_avrdd.hex

retropad_avrdd.elf: main.c pinmap.h $(PROTO)/retropad_proto.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ main.c
	$(SIZE) $@

retropad_avrdd.hex: retropad_avrdd.elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

pinmap.h: ../boards/layout.py ../boards/kicad_gen/gen_avrdd_pinmap.py
	python3 ../boards/kicad_gen/gen_avrdd_pinmap.py

flash: retropad_avrdd.hex
	avrdude -c serialupdi -P $(PORT) -p $(AVRDUDE_PART) -U flash:w:$<:i

clean:
	rm -f retropad_avrdd.elf retropad_avrdd.hex

.PHONY: all flash clean
