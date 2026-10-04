#!/usr/bin/env bash
# build_all_tab5.sh - Build the launcher + every emulator app for the M5Stack Tab5
# and merge them into one image (RetroESP32_P4_Tab5_v1.bin, flash at 0x0).
#
# Usage:  ./build_all_tab5.sh            # needs an activated ESP-IDF 5.5.x or 6.x (idf.py on PATH)
#         ./build_all_tab5.sh launcher nes snes     # build only some projects, no merge
#         ./build_all_tab5.sh --merge-only          # merge the binaries already in firmware_tab5/
#
# Flash:  python -m esptool --chip esp32p4 -b 460800 write_flash 0x0 RetroESP32_P4_Tab5_v1.bin
#
# Everything is built with launcher/sdkconfig.tab5.defaults layered on top of the
# project's own defaults, and with a private build dir + sdkconfig per project so a
# Tab5 build never reuses (or clobbers) a handheld build.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINS="$ROOT/firmware_tab5"
TAB5_DEFAULTS="$ROOT/launcher/sdkconfig.tab5.defaults"
OUT="$ROOT/RetroESP32_P4_Tab5_v1.bin"

PARTITIONS="$ROOT/partitions_ota.csv"

# name : project dir : output binary : partition (partitions_ota.csv)
ALL_APPS=(
  "nes:apps/nes:nes_app.bin:ota_0"
  "gb:apps/gb:gb_app.bin:ota_1"
  "sms:apps/sms:sms_app.bin:ota_2"
  "spectrum:apps/spectrum:spectrum_app.bin:ota_3"
  "stella:apps/stella:stella_app.bin:ota_4"
  "prosystem:apps/prosystem:prosystem_app.bin:ota_5"
  "handy:apps/handy:handy_app.bin:ota_6"
  "pce:apps/pce:pce_app.bin:ota_7"
  "atari800:apps/atari800:atari800_app.bin:ota_8"
  "snes:apps/snes:snes_app.bin:ota_10"
  "genesis:apps/genesis:genesis_app.bin:ota_11"
  "neogeo:apps/neogeo:neogeo_app.bin:ota_12"
)

part_field() { # partition name, field (4 = offset, 5 = size)
  awk -F, -v n="$1" -v f="$2" '!/^#/ { gsub(/[ \t]/, ""); if ($1 == n) { print $f; exit } }' "$PARTITIONS"
}

check_fit() { # binary, partition - the per-app OTA slots differ in size, so check each one
  local bin="$1" part="$2" size slot
  size=$(stat -c %s "$bin")
  slot=$(( $(part_field "$part" 5) ))
  printf '%-22s %5d KB / %5d KB  (%s)\n' "$(basename "$bin")" $((size / 1024)) $((slot / 1024)) "$part"
  [ "$size" -le "$slot" ] || { echo "ERROR: $(basename "$bin") does not fit in $part"; return 1; }
}

want() { # is project $1 selected?
  [ "${#SELECTED[@]}" -eq 0 ] && return 0
  for s in "${SELECTED[@]}"; do [ "$s" = "$1" ] && return 0; done
  return 1
}
MERGE_ONLY=0
[ "${1:-}" = "--merge-only" ] && { MERGE_ONLY=1; shift; }
SELECTED=("$@")

command -v python >/dev/null || { echo "python not found - activate ESP-IDF first (. \$IDF_PATH/export.sh)"; exit 1; }
[ "$MERGE_ONLY" = 1 ] || command -v idf.py >/dev/null || { echo "idf.py not found - activate ESP-IDF 5.5.x or 6.x first (. \$IDF_PATH/export.sh)"; exit 1; }

mkdir -p "$BINS"

build() { # dir
  local dir="$1" bdir="$ROOT/$1/build_tab5"
  ( cd "$ROOT/$dir"
    rm -rf "$bdir"; mkdir -p "$bdir"
    local defaults="sdkconfig.defaults;$TAB5_DEFAULTS"
    # optional per-project Tab5 overrides (apps/<app>/sdkconfig.tab5.defaults)
    [ -f sdkconfig.tab5.defaults ] && defaults="$defaults;sdkconfig.tab5.defaults"
    idf.py -B "$bdir" -DSDKCONFIG="$bdir/sdkconfig" -DSDKCONFIG_DEFAULTS="$defaults" build )
}

if [ "$MERGE_ONLY" = 0 ] && want launcher; then
  echo "=== Building launcher (Tab5) ==="
  build launcher
  B="$ROOT/launcher/build_tab5"
  cp "$B/launcher.bin"                      "$BINS/launcher.bin"
  cp "$B/bootloader/bootloader.bin"         "$BINS/bootloader.bin"
  cp "$B/partition_table/partition-table.bin" "$BINS/partition-table.bin"
  cp "$B/ota_data_initial.bin"              "$BINS/ota_data_initial.bin"
  check_fit "$BINS/launcher.bin" factory
fi

for entry in "${ALL_APPS[@]}"; do
  [ "$MERGE_ONLY" = 0 ] || break
  IFS=: read -r name dir bin part <<<"$entry"
  want "$name" || continue
  echo "=== Building $name (Tab5) ==="
  build "$dir"
  cp "$ROOT/$dir/build_tab5/$bin" "$BINS/$bin"
  check_fit "$BINS/$bin" "$part"
done

if [ "$MERGE_ONLY" = 0 ] && [ "${#SELECTED[@]}" -ne 0 ]; then
  echo "Partial build - binaries in $BINS (no merge)."
  exit 0
fi

echo "=== Merging $OUT ==="
# App offsets come from partitions_ota.csv; the bootloader/partition table offsets are fixed.
check_fit "$BINS/launcher.bin" factory
ARGS=(0x2000 "$BINS/bootloader.bin"
      0x8000 "$BINS/partition-table.bin"
      "$(part_field otadata 4)" "$BINS/ota_data_initial.bin"
      "$(part_field factory 4)" "$BINS/launcher.bin")
for entry in "${ALL_APPS[@]}"; do
  IFS=: read -r name dir bin part <<<"$entry"
  check_fit "$BINS/$bin" "$part"
  ARGS+=("$(part_field "$part" 4)" "$BINS/$bin")
done
python -m esptool --chip esp32p4 merge_bin -o "$OUT" --flash_mode qio --flash_size 16MB --flash_freq 80m "${ARGS[@]}"

echo "Done: $OUT"
