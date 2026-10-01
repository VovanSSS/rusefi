#!/bin/bash
# Flash the connected ECU through its OEM resident loader (I812/I865) over CAN.
# This is the M74.9 original-bootloader uploader (com.rusefi.m749.M749Cli), not
# OpenBLT. Requires a PCAN-USB adapter (MacCAN/libPCBUSB on macOS, PEAK driver on
# Windows/Linux), an SLCAN adapter, or a Linux SocketCAN interface, at 500 kbit/s
# on diagnostic IDs 7E0/7E8.
#
# Usage: m749_can.sh [options] [firmware.hex|.srec]
#   see com.rusefi.m749.M749Cli --help
#   e.g. m749_can.sh --upload rusefi.hex --channel PCAN_USBBUS1

# Remember the caller's working directory before cd, so relative file arguments
# (upload input, read-flash output, pair files) resolve against it, not java_console/.
CALLER_PWD="$(pwd)"

cd "$(dirname "$0")/.." || exit 1

JAR=m749/build/libs/m749-all.jar
(cd .. && ./gradlew :m749:fatJar) || exit 1

# Flags whose VALUE is a file/output path and must be anchored to the caller's
# cwd. Transport/flow-control flags (--channel/--slcan/--socketcan/--serial-baud/
# --slcan-bus/--block-size/--stmin) and --read-byte (a hex address) are left alone.
resolve_file_arg() {
  case "$1" in
    /*) printf '%s' "$1" ;;
    *)  printf '%s' "$CALLER_PWD/$1" ;;
  esac
}

resolved_args=()
prev=""
for arg in "$@"; do
  case "$arg" in
    -*) resolved_args+=("$arg") ;;
    *)
      case "$prev" in
        --upload|--write-flash|--check-target|--read-flash|--read-pair|--export-pair|--pair-file|--immo-backup)
          resolved_args+=("$(resolve_file_arg "$arg")") ;;
        *) resolved_args+=("$arg") ;;
      esac
      ;;
  esac
  prev="$arg"
done

# libpcanbasic_jni.dylib / PCANBasic_JNI.dll live in java_console/.
# --enable-native-access silences the Java 24+ warning from PCANBasic's System.loadLibrary.
exec java --enable-native-access=ALL-UNNAMED -Djava.library.path=. -cp "$JAR" com.rusefi.m749.M749Cli "${resolved_args[@]}"
