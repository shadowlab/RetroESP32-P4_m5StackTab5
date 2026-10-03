#!/usr/bin/env bash
# build_all_tab5.sh - Build the launcher + every emulator app for the M5Stack Tab5
# and merge them into one image (RetroESP32_P4_Tab5_v1.bin, flash at 0x0).
#
# Usage:  ./build_all_tab5.sh            # needs an activated ESP-IDF 5.5.x (idf.py on PATH)
#         ./build_all_tab5.sh launcher nes snes     # build only some projects, no merge
#
# Flash:  python -m esptool --chip esp32p4 -b 460800 write_flash 0x0 RetroESP32_P4_Tab5_v1.bin
#
# Everything is built with launcher/sdkconfig.tab5.defaults layered on top of the
# project's own defaults, and with a private build dir + sdkconfig per project so a
# Tab5 build never reuses (or clobbers) a handheld/HDMI build.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINS="$ROOT/firmware_tab5"
TAB5_DEFAULTS="$ROOT/launcher/sdkconfig.tab5.defaults"
OUT="$ROOT/RetroESP32_P4_Tab5_v1.bin"

command -v idf.py >/dev/null || { echo "idf.py not found - activate ESP-IDF 5.5.x first (. \$IDF_PATH/export.sh)"; exit 1; }

# name : project dir : output binary
ALL_APPS=(
  "nes:apps/nes:nes_app.bin"
  "gb:apps/gb:gb_app.bin"
  "sms:apps/sms:sms_app.bin"
  "spectrum:apps/spectrum:spectrum_app.bin"
  "stella:apps/stella:stella_app.bin"
  "prosystem:apps/prosystem:prosystem_app.bin"
  "handy:apps/handy:handy_app.bin"
  "pce:apps/pce:pce_app.bin"
  "atari800:apps/atari800:atari800_app.bin"
  "snes:apps/snes:snes_app.bin"
  "genesis:apps/genesis:genesis_app.bin"
  "neogeo:apps/neogeo:neogeo_app.bin"
)

want() { # is project $1 selected?
  [ "${#SELECTED[@]}" -eq 0 ] && return 0
  for s in "${SELECTED[@]}"; do [ "$s" = "$1" ] && return 0; done
  return 1
}
SELECTED=("$@")

mkdir -p "$BINS"

build() { # dir
  local dir="$1" bdir="$ROOT/$1/build_tab5"
  ( cd "$ROOT/$dir"
    rm -rf "$bdir"; mkdir -p "$bdir"
    local defaults="sdkconfig.defaults;$TAB5_DEFAULTS"
    # optional per-project Tab5 overrides (e.g. apps/nes: log strings out to fit its 576 KB slot)
    [ -f sdkconfig.tab5.defaults ] && defaults="$defaults;sdkconfig.tab5.defaults"
    idf.py -B "$bdir" -DSDKCONFIG="$bdir/sdkconfig" -DSDKCONFIG_DEFAULTS="$defaults" build )
}

if want launcher; then
  echo "=== Building launcher (Tab5) ==="
  build launcher
  B="$ROOT/launcher/build_tab5"
  cp "$B/launcher.bin"                      "$BINS/launcher.bin"
  cp "$B/bootloader/bootloader.bin"         "$BINS/bootloader.bin"
  cp "$B/partition_table/partition-table.bin" "$BINS/partition-table.bin"
  cp "$B/ota_data_initial.bin"              "$BINS/ota_data_initial.bin"
fi

for entry in "${ALL_APPS[@]}"; do
  IFS=: read -r name dir bin <<<"$entry"
  want "$name" || continue
  echo "=== Building $name (Tab5) ==="
  build "$dir"
  cp "$ROOT/$dir/build_tab5/$bin" "$BINS/$bin"
done

if [ "${#SELECTED[@]}" -ne 0 ]; then
  echo "Partial build - binaries in $BINS (no merge)."
  exit 0
fi

echo "=== Merging $OUT ==="
# Flash map: must match partitions_ota.csv (same offsets as generate_merged_bin.ps1)
python -m esptool --chip esp32p4 merge_bin -o "$OUT" --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x2000   "$BINS/bootloader.bin" \
  0x8000   "$BINS/partition-table.bin" \
  0xD000   "$BINS/ota_data_initial.bin" \
  0x10000  "$BINS/launcher.bin" \
  0x0D0000 "$BINS/nes_app.bin" \
  0x160000 "$BINS/gb_app.bin" \
  0x200000 "$BINS/sms_app.bin" \
  0x350000 "$BINS/spectrum_app.bin" \
  0x410000 "$BINS/stella_app.bin" \
  0x550000 "$BINS/prosystem_app.bin" \
  0x5F0000 "$BINS/handy_app.bin" \
  0x690000 "$BINS/pce_app.bin" \
  0x740000 "$BINS/atari800_app.bin" \
  0x8C0000 "$BINS/snes_app.bin" \
  0x9B0000 "$BINS/genesis_app.bin" \
  0xB00000 "$BINS/neogeo_app.bin"

echo "Done: $OUT"
