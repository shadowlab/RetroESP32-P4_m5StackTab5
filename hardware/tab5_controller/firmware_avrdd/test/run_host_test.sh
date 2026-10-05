#!/bin/sh
# Build firmware_avrdd/main.c on the host against fake registers and run the I2C protocol test.
set -e
cd "$(dirname "$0")"
gcc -std=c11 -Wall -Wextra -Wno-unused-function -Ifake -I.. -I../../../../components/tab5_ctrl/include \
    -o /tmp/retropad_avrdd_host_test host_test.c
/tmp/retropad_avrdd_host_test
