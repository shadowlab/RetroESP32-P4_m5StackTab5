# hdmi_fpga.bin

FPGA image for the M5Stack Module Display 13.2 (Gowin GW1NR-9). Copy this file to `/system/hdmi_fpga.bin` on
the SD card. The firmware loads it into the module over JTAG the first time after the module is powered up
(about 2 s); after that the FPGA keeps it until power is removed, so the file is only read when needed.

It is the RLE-compressed bitstream from M5GFX (`Panel_M5HDMI_FS.h`, converted to a binary file), built from
https://github.com/ciniml/atom_display_fpga (branch `feature/veryl-port`, commit 93d419c) with GOWIN EDA 1.9.12.
That design is published under the GNU GPL v3; the source is in the repository above.
