#!/usr/bin/env bash
# DEFAULT build for m74_9: the three-stage OEM image.
#
# Build the M74.9 three-stage image: OpenBLT second-stage + main app, merged
# into one addressed HEX/SREC with the OEM CRC-32/MPEG-2 at 0x080FFFFC.
#
# The main app `gmake` step still emits a (wrong) hex2dfu-based HEX/SREC; the
# final merge step below overwrites build/rusefi.hex and build/rusefi.srec with
# the correct three-stage image. Flash those (not rusefi.bin) through the M749
# CLI (UDS/I865 OEM loader).
#
# NOTE: do NOT build this board with `compile.sh -b` (bundle): the bundle flow
# merges bootloader+app with hex2dfu, whose layout/CRC assumptions do not match
# the OEM three-stage layout (it hangs/CRC-clobbers on the relocated OpenBLT
# sections - learned 2026-10-02).
# NOTE: no `set -u`: common_script_read_meta_env.inc references BOARD_DIR/
# PROJECT_BOARD unguarded (${VAR} in [ -z ] and echo) and exits on nounset.
set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # .../config/boards/m74_9
BOARD_DIR_REL=config/boards/m74_9
META=$BOARD_DIR_REL/meta-info.env

cd "$SCRIPT_DIR/../../.."   # firmware/
export PATH="/opt/arm-gnu-toolchain/bin:$PATH"

# 1. Main application (regenerates the shared config/ generated headers).
. config/boards/common_script_read_meta_env.inc "$META" >/dev/null 2>&1
gmake -j12 "$@"

# 1b. ZW-coverage guard: the .m749_fast_text patterns are name globs, a
#     rename can silently evict hot code from the zero-wait window (the
#     link ASSERT only bounds the size). Fails the build when a listed
#     hot symbol resolves outside ZW/RAM.
"$SCRIPT_DIR/bin/check_zw_coverage.sh" build/rusefi.elf

# 2. OpenBLT second-stage bootloader.
BOARD_DIR=../$BOARD_DIR_REL BOARD_META_PATH=../$META gmake -C bootloader -r SUBMAKE=yes -j12

# 3. Merge + CRC.
python3 "$SCRIPT_DIR/bin/m749_image.py" \
  --openblt bootloader/blbuild/openblt_m74_9.elf \
  --app build/rusefi.elf \
  --format hex build/rusefi.hex
python3 "$SCRIPT_DIR/bin/m749_image.py" \
  --openblt bootloader/blbuild/openblt_m74_9.elf \
  --app build/rusefi.elf \
  --format srec build/rusefi.srec

# 4. App-only SREC for the OpenBLT-over-CAN update path (no bootloader, no CRC
#    word: the bootloader's FlashWriteChecksum erases the 0x080FF000 CRC sector
#    and writes the OEM CRC itself). Two segments:
#    0x08010000..0x0804F17C + 0x08090000..0x080FD398.
arm-none-eabi-objcopy -O srec build/rusefi.elf build/rusefi_app.srec

echo "M74.9 three-stage image written: build/rusefi.hex build/rusefi.srec"
echo "OpenBLT app-update image written: build/rusefi_app.srec"
