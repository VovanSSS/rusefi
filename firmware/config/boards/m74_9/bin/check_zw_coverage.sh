#!/usr/bin/env bash
# ZW-coverage check for the m74_9 main app.
#
# The .m749_fast_text section of m749.ld places hot code (matched by
# function-section name patterns) into the AT32 zero-wait flash window
# 0x08011000..0x0803FFFF (the app's share of the 256 KiB ZW region of the
# 384 KiB SRAM option). The patterns are name globs, so a class/file rename
# can silently evict code from ZW - the link-time ASSERT only bounds the
# section size, it cannot detect lost coverage. This script is the coverage
# guard: the listed symbols must resolve inside the ZW window or in the
# .fast_text RAM copy (0x20050000+). Add any new hot-path symbol here.
#
# Usage: check_zw_coverage.sh <elf>
set -eu

ELF="$1"
NM="${NM:-$(command -v arm-none-eabi-nm || true)}"
if [ -z "$NM" ] && [ -x /opt/arm-gnu-toolchain/bin/arm-none-eabi-nm ]; then
  NM=/opt/arm-gnu-toolchain/bin/arm-none-eabi-nm
fi
if [ -z "$NM" ]; then
  echo "ZW-COVERAGE: arm-none-eabi-nm not found" >&2
  exit 1
fi

# ZW window and RAM fast-text base (see m749.ld; keep in sync).
ZW_START=$((16#08011000))
ZW_END=$((16#08040000))
RAM_FAST_START=$((16#20050000))

fail=0
check() {
  local sym="$1"
  local escaped line addr
  # Escape regex specials in the symbol (handles '::' as literals).
  escaped=$(printf '%s' "$sym" | sed 's/[][().*+?^${\\|}]/\\&/g')
  line=$("$NM" -C "$ELF" | grep -E "[[:space:]]${escaped}($|[^[:alnum:]_])" | head -1)
  if [ -z "$line" ]; then
    echo "ZW-COVERAGE: symbol '$sym' not found in the ELF" >&2
    fail=1
    return
  fi
  addr=$((16#$(echo "$line" | awk '{print $1}')))
  if [ "$addr" -lt "$ZW_START" ] || [ "$addr" -ge "$ZW_END" ]; then
    if [ "$addr" -lt "$RAM_FAST_START" ]; then
      printf 'ZW-COVERAGE: symbol %-48s at 0x%08x is in NZW flash (slow)\n' "$sym" "$addr" >&2
      fail=1
    fi
  fi
}

# Per-tooth trigger/decode chain (RAM-marked is fine, ZW is fine).
check 'mainTriggerCallback'
check 'handleShaftSignal'
check 'hwHandleShaftSignal'
check 'TriggerCentral::handleShaftSignal'
check 'TriggerDecoderBase::decodeTriggerEvent'
check 'TriggerScheduler::scheduleEventsUntilNextTriggerTooth'
check 'RpmCalculator::assignRpmValue'
check 'Sensor::getOrZero'
check 'getTimeNowNt'
check 'handleExtiIsr'
check 'hwTimerCallback'
check 'InjectionEvent::onTriggerTooth'
check 'startInjection'
check 'turnSparkPinHighStartCharging'
check 'SingleTimerExecutor::scheduleTimerCallback'
check 'SingleTimerExecutor::executeAllPendingActions'
check 'EventQueue::tryReturnScheduling'
check 'chSysGetStatusAndLockX'
check 'chSysLockFromISR'
check 'interpolate2d'
check 'getRunningAdvance'
check 'SpeedDensityAirmass::getAirmass'
check 'gpiochips_writePad(Gpio, int)'
check 'memset'
check 'memcpy'

if [ "$fail" -ne 0 ]; then
  echo "ZW-COVERAGE: FAILED - hot symbols resolved outside the ZW window." >&2
  echo "Check the .m749_fast_text patterns in firmware/m749.ld." >&2
  exit 1
fi

echo "ZW-COVERAGE: OK - all checked symbols are in ZW flash or the RAM fast copy."
