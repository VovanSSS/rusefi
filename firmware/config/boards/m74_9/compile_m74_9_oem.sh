#!/usr/bin/env bash
# Build the M74.9 three-stage image: OpenBLT second-stage + main app, merged
# into one addressed HEX/SREC with the OEM CRC-32/MPEG-2 at 0x080FFFFC.
#
# The main app `gmake` step still emits a (wrong) hex2dfu-based HEX/SREC; the
# final merge step below overwrites build/rusefi.hex and build/rusefi.srec with
# the correct three-stage image. Flash those (not rusefi.bin) through the M749
# CLI (UDS/I865 OEM loader).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # .../config/boards/m74_9
BOARD_DIR_REL=config/boards/m74_9
META=$BOARD_DIR_REL/meta-info.env

cd "$SCRIPT_DIR/../../.."   # firmware/
export PATH="/opt/arm-gnu-toolchain/bin:$PATH"

# 1. Main application (regenerates the shared config/ generated headers).
. config/boards/common_script_read_meta_env.inc "$META" >/dev/null 2>&1
gmake -j12 "$@"

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

echo "M74.9 three-stage image written: build/rusefi.hex build/rusefi.srec"
