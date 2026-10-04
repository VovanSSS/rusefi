# TMR2 body path: full flash inventory (m74_9, 2026-10-04)

What STILL executes from flash inside the TMR2 angle-clock ISR after the
2026-10-03 open/close relocation. Method: per-body disassembly call analysis
(`arm-none-eabi-objdump -d` on build/rusefi.elf, post-move build) - every
`bl`/`b.w` target classified by section; veneers (`___..._veneer` in
.fast_text) resolved to their flash targets. Sizes from `nm -S`.

## Context

Already in RAM (.fast_text, 20.7 KB of 64 KB budget):

- The four action bodies: turnSparkPinHighStartCharging (dwell),
  fireSparkAndPrepareNextSchedule (spark), turnInjectionPinHigh (open),
  turnInjectionPinLow (close).
- Their core helpers: prepareCylinderIgnitionSchedule,
  InjectionEvent::update/updateInjectionAngle, InjectorOutputPin::
  open/close/setHigh/setLow, NamedOutputPin::setHigh/setLow,
  OutputPin::setValue (both), SingleTimerExecutor::schedule/cancel,
  scheduleByAngle, recordSparkFire, wrapAngle, getCurrentIgnitionMode,
  getPerCylinderFiringOrderOffset, getCylinderNumberAtIndex,
  getTimeNowNt, Timer::reset/getElapsedSeconds, angleClock arm/refresh/
  dispatch internals. LogTriggerCoilState/InjectorState are inlined into
  the bodies.

## Remaining flash on TAKEN paths (per body)

| function | flash addr | size | called from | frequency |
| --- | --- | --- | --- | --- |
| Sensor::getOrZero | 0x080ab540 | 36 B | dwell body (C9009 rpm gate) | every dwell |
| LogTriggerTooth | 0x080e2d34 | 268 B | dwell + spark bodies (via LogTriggerCoilState; bails early, sniffer off) | every dwell + spark |
| assertPinAssigned | 0x080d0ed4 | 40 B | prepareCylinderIgnitionSchedule | every spark |
| scaled_channel<short,50,1>::scaled_channel(float) | 0x080ec6a4 | 32 B | prepare (getDwell input conversion) | every spark |
| SingleTimerExecutor::scheduleTimerCallback | 0x080d1dbc | 272 B | schedule() (overdwell + knock insert) | every dwell + spark |
| chSysGetStatusAndLockX | 0x08099fc4 | 56 B | every CriticalSectionLocker (kernel) | 1-3 per body |
| chSysRestoreStatusX | 0x080f47fe | 36 B | every CriticalSectionLocker (kernel) | 1-3 per body |
| OutputPin::isInitialized | 0x080f9eb4 | 16 B | InjectionEvent::update | every close |
| RpmCalculator::isCranking (+thunk) | 0x080fa80a/32 | 40+8 B | InjectionEvent::update | every close |
| Sensor::get | 0x080c0638 | 40 B | Sensor::getOrZero | every getOrZero |
| isBrainPinValid | 0x080f9d10 | 14 B | gpiochips_writePad <- OutputPin::setValue | every pin write |
| addEngineSnifferOutputPinEvent | 0x080deadc | 60 B | OutputPin::setValue (sniffer off -> inner bail via macro) | every pin write |
| memset (newlib) | 0x080fc3e8 | - | dispatch() due[] array init | once per ISR entry |
| action_s::trampoline instantiations | 0x080fa9xx | 4 B each | act.execute() -> body | every fire |

NOT on taken paths (left in flash deliberately): warning /
firmwareError (print paths - not executed when healthy).

## Special cases

1. chSysGetStatusAndLockX / chSysRestoreStatusX live in the ChibiOS fork
   (firmware/ChibiOS - a SEPARATE git repo). The bootloader linker script
   (m749_openblt.ld) has NO .fast_text section, so giving these functions
   the .fast_text attribute would create an orphan section in the
   bootloader build - must be gated per-build or done as a fork commit.
   Cost estimate: ~60 flash instructions per CS, 1-3 CS per body.
2. memset: newlib - one call per dispatch entry (~64 B init). Leave, or
   replace the array init; ~1 us.
3. The action_s trampoline is a template in controllers/system/timer/
   scheduler.h - marking it moves ALL instantiations (executor callbacks
   too, beneficial). Constexpr + noinline + section is fine.

## Planned move list (files)

1. firmware/controllers/sensors/core/sensor.h - Sensor::getOrZero (inline)
2. firmware/controllers/sensors/core/sensor.cpp - Sensor::get
3. firmware/controllers/engine_cycle/rpm_calculator.cpp - isCranking
4. firmware/controllers/engine_cycle/spark_logic.cpp - assertPinAssigned
5. firmware/controllers/system/timer/single_timer_executor.cpp -
   scheduleTimerCallback
6. firmware/console/binary/tooth_logger.cpp - LogTriggerTooth
7. firmware/hw_layer/pin_repository.cpp - isBrainPinValid
8. firmware/development/engine_sniffer.cpp -
   addEngineSnifferOutputPinEvent
9. firmware/libfirmware/util/include/rusefi/scaled_channel.h - float ctor
10. firmware/controllers/system/efi_gpio.cpp - OutputPin::isInitialized
11. firmware/controllers/system/timer/scheduler.h - action_s trampoline +
    trampoline_no_arg templates

## IMPLEMENTED (2026-10-04, commit)

Items 1-8, 10, 11 moved. .fast_text 20.2 -> 22.3 KB (41.7 KB free).
Per-body call re-analysis of the new ELF: every taken-path target of all
four bodies + prepare + update is now at 0x2005xxxx (RAM), including the
direct calls that used to go through veneers (Sensor::getOrZero,
LogTriggerTooth, assertPinAssigned, isCranking, isInitialized).

Left in flash (documented):
- scaled_channel float ctor (32 B, one call per spark): item 9 DROPPED -
  GCC emits the constexpr ctor as an alias to a clone, and the section
  attribute on the alias does not match the target ("section of alias must
  match section of its target", reproduced for every instantiation in
  engine TUs; the explicit-instantiation workaround fails the same way).
- The 4-byte action_s trampoline stubs (LTO reduced them to b.w veneers
  into RAM bodies - ~2 flash fetches per fire).
- Kernel CS functions chSysGetStatusAndLockX/chSysRestoreStatusX (separate
  ChibiOS repo; the bootloader linker has no .fast_text).
- newlib memset in dispatch (one call per entry).
- warning/firmwareError print paths (not taken).

Header-order notes (learned while doing it):
- scheduler.h is included from global.h BEFORE the TRIGGER_RAM_CODE define
  - the define block in firmware/global.h was moved above the
  single_timer_executor.h include (efifeatures.h is already available via
  common_headers.h) and wrapped in #ifndef (scaled_channel.h-free now, but
  keep the guard for future header users).
- unit_tests/global.h defines the empty TRIGGER_RAM_CODE AFTER
  test_executor.h (which pulls in scheduler.h) - moved above it.
- sensor.h is included by small TUs without global.h, so the macro cannot
  be used in sensor.h at all: getOrZero moved from an inline header body to
  an out-of-line TRIGGER_RAM_CODE definition in sensor.cpp.

Validation: compile_m74_9.sh BUILD SUCCESSFUL; unit tests 1169/15 (the
15 documented pre-existing injection/trigger scheduling failures, nothing
new); AngleClock tests pass.

## 2026-10-04 (19:25-19:28) - m74_9: full flash removal CAR-VERIFIED - the second wave cut dwell/spark ~20 us each

Six clean windows (~1000-1400 rpm, no storm, accounting exact).

| body (maxUs) | original | after move 1 (bodies) | after move 2 (callees) |
| --- | --- | --- | --- |
| dwell | 62-92 | 67-82 | 44-53 |
| spark | 85-111 | 85-107 | 65-75 |
| open | 55-74 | 34-57 | 33-44 |
| close | 76-89 | 44-56 | 41-47 |

The callee move cut dwell/spark ~20 us each (they were the control group
of move 1 but still carried the flash callees); open/close shed a few more.
Total across both waves: dwell -25, spark -28, open -25, close -35 us.

Derived metrics - the best of the whole investigation:
- angclk maxLateUs in clean windows 25-35 us (was 56-106 originally,
  33-41 after move 1).
- ser= ~0 in all six windows (the shortened bodies no longer overlap the
  next entry); captures 10-27 us, no masks, no handoff, TMR2-only.
- Bonus: the executor callback trampolines (startKnockSampling/onTdc/
  watchdog) also landed in RAM (action_s template section) - their
  maxDurUs dropped (startKnock 128 -> 79-99).

The residual floor is now clean: ~33-35 us FIXED + ~6 cycles/instruction,
plus spark carries an extra ~15-20 us (its remaining flash tail: the
scaled_channel ctor + the longest chain). The floor is NOT flash, NOT CS,
NOT prints - the moved open body (250 RAM instructions, no CS, no
executor) still runs 33-44 us. The mechanism is still open; the next
diagnostic is per-callee timing inside the bodies.

## Expected effect

~400-500 flash instructions remain on the taken paths at the measured flash
cost (~6.5 cycles/instruction + prefetch invalidation on calls), i.e. on
the order of a few us per body, plus the per-CS kernel cost (~1-3 us).
This removes the LAST flash fetches from the TMR2 fire path; the
24-30 us fixed floor measured on the car (2026-10-04 session) is expected
to remain - it is not explained by any of these targets (the moved open
body has no CS, no executor ops, ~55 flash instructions left, and still
runs 34-57 us).

## RAM budget

.fast_text today: 0x50f8 = 20.2 KB of the 64 KB ram_fast region (free
43.3 KB). The move list adds (real ELF sizes):

| group | bytes |
| --- | --- |
| fixed functions (getOrZero 36, Sensor::get 40, isCranking 40, assertPinAssigned 40, scheduleTimerCallback 272, LogTriggerTooth 268, isBrainPinValid 14, addEngineSnifferOutputPinEvent 60, isInitialized 16) | 786 |
| scaled_channel float ctor: 22 instantiations (0x1a-0x24 each) | 692 |
| action_s trampolines: 23 instantiations | 2254 |
| total | ~3732 (3.6 KB) |

Minor offsets the other way: the per-target veneers disappear (-8 B x ~10)
and noinline removes the inline copies from callers (flash savings, not
RAM). After the move: ~24 KB used, ~40 KB free (~62% headroom). The kernel
CS functions would add ~92 B if ever moved. RAM is not the constraint.
