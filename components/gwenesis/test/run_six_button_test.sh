#!/bin/sh
# Build and run the 6-button pad protocol test on the host (needs gcc).
set -e
cd "$(dirname "$0")"
SRC=../src
gcc -Wall -Wextra -Wno-unused-parameter -I$SRC/io -I$SRC/savestate -I$SRC/cpus/M68K -I$SRC/bus \
    -o /tmp/gwenesis_six_button_test six_button_test.c six_button_stubs.c $SRC/io/gwenesis_io.c
/tmp/gwenesis_six_button_test
