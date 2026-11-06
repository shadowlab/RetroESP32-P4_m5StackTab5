# hdmi_fpga.bin

FPGA image for the M5Stack Module Display 13.2 (Gowin GW1NR-9). Copy this file to `/system/hdmi_fpga.bin` on
the SD card. The firmware loads it into the module over JTAG the first time after the module is powered up
(about 2 s); after that the FPGA keeps it until power is removed, so the file is only read when needed.

It is the RLE-compressed bitstream from M5GFX (`Panel_M5HDMI_FS.h`, converted to a binary file), built from
https://github.com/ciniml/atom_display_fpga (branch `feature/veryl-port`, commit 93d419c) with GOWIN EDA 1.9.12.
That design is published under the GNU GPL v3; the source is in the repository above.

# hdmi_mode.txt (optional)

Output timing for the HDMI module. Put one mode name on the first line of `/system/hdmi_mode.txt` on the SD card:
`1280x720` (default), `1280x720@50`, `1280x800`, `1024x768`, `800x600`, `640x480`, and the half-clock modes `1440x900`, `1680x1050` and `1920x1080` (experimental: only even scales, so the 800x480 launcher does not fit `1440x900`). Use one your display lists
if it shows no picture at 1280x720 (older PC monitors on a DVI adapter often do not accept 720p). Games are scaled by whole
numbers to fit the chosen mode; a frame bigger than the mode (the 800x480 launcher in `640x480`) is not shown.

Experiment options after the mode name on the same line: `dvi` (clear the HDMI bit of the transmitter, for DVI monitors), `d6=hh` / `d7=hh` (write LT8618 registers 82:d6 / 82:d7 directly). Example: `1024x768 dvi`.
