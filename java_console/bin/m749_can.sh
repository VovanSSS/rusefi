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

cd "$(dirname "$0")/.." || exit 1

JAR=m749/build/libs/m749-all.jar
(cd .. && ./gradlew :m749:fatJar) || exit 1

# Resolve relative file arguments against the CALLER's cwd BEFORE the cd below.
resolved_args=()
for arg in "$@"; do
  case "$arg" in
    -*) resolved_args+=("$arg") ;;
    *)
      if [ -e "$arg" ]; then
        resolved_args+=("$(cd "$(dirname "$arg")" && pwd)/$(basename "$arg")")
      else
        resolved_args+=("$arg")
      fi
      ;;
  esac
done

# libpcanbasic_jni.dylib / PCANBasic_JNI.dll live in java_console/.
# --enable-native-access silences the Java 24+ warning from PCANBasic's System.loadLibrary.
exec java --enable-native-access=ALL-UNNAMED -Djava.library.path=. -cp "$JAR" com.rusefi.m749.M749Cli "${resolved_args[@]}"
