# Bug: MAP/misfire window planners double-subtract tdcPosition() (Variant A, recorded 2026-09-24)

Status: **FIXED in the working tree (2026-09-24, not committed)** - applied
later the same day as the record. Car validation pending (see section 7).
The fix moves the MAP sampling phase, so it belongs together with a tune
sanity pass (idle VE/AFR) on the next drive.

---

## TL;DR

`MapAveragingModule::scheduleNextCycleWindows()` (and the misfire planner)
computes `windowOffset = wrap(mapAveragingStart[i] - tdcPosition())` and fires
the window from the once-per-cycle TDC hook. That hook already fires at
**engine phase 0 (TDC1)**: `tdcMarkCallback` schedules it exactly
`tdcPosition()` crank degrees after the sync point, and engine phase is
defined as `E = wrap(phaseFromSyncPoint - tdcPosition())`, so the hook lands
at `E = 0`. `mapAveragingStart[]` is an absolute engine-phase angle, so
`tdcPosition` is subtracted **twice**.

Result on m74_9: every MAP window fires ~112 deg early; below ~2375 rpm
cylinder 1's window straddles the cycle boundary and its closing event is
cancelled by the next cycle's plan every time - the observed, perfectly
stable `endAveraging` 3/cyc vs `startAveraging` 4/cyc.

---

## 1. Symptom (car data)

Session 2026-09-24 10:55-10:59, first drive of the Variant A build
(firmware `maccan-tx-fix.2026.09.23`, ELF built 09-24 00:31), ~1146-1265 rpm,
five lockstats snapshots, ~2000 engine cycles.

Callbacks resolved with addr2line against the flashed image
(`firmware/build/rusefi.elf`):

| addr       | function        |
| ---------- | --------------- |
| 0x0805A441 | startAveraging  |
| 0x0805A275 | endAveraging    |
| 0x08049F45 | onTdcCallback   |

| snapshot | startAveraging n | endAveraging n | end/start |
| -------- | ---------------- | -------------- | --------- |
| 10:55:54 | 1448             | 1104           | 0.762     |
| 10:56:34 | 1626             | 1221           | 0.751     |
| 10:58:14 | 3813             | 2859           | 0.750     |
| 10:58:16 | 77               | 58             | 0.753     |
| 10:59:33 | 2962             | 2242           | 0.757     |

`startAveraging` fires exactly once per cylinder per engine cycle (== dwell
count); `endAveraging` fires exactly 3 per 4 starts, in every window of the
session. Baseline: the 2026-09-23 build (per-tooth `onEnginePhase` arms) ran
4/4 - this is a regression introduced by the once-per-cycle planner, not noise.

## 2. Evidence chain (code anchors)

Branch `maccan-tx-fix`, 2026-09-24:

1. **Phase convention.** `trigger_central.cpp` handleShaftSignal:
   `currentEngineDecodedPhase = wrap(currentPhaseFromSyncPoint - tdcPosition())`.
   `engine_math.h`: `tdcPosition() = triggerShape.tdcPosition +
   (camSyncOnSecondCrankRevolution ? globalTriggerAngleOffset + 360 :
   globalTriggerAngleOffset)`. m74_9: camSyncOnSecondCrankRevolution = first
   (no +360), `globalTriggerAngleOffset = 112` in 21129.msq -> tdcPosition()
   = 112. The sync sits 112 deg before TDC1; dwell/spark scheduling against
   this reference is the validated timing base of the whole car.
2. **When the hook fires.** `rpm_calculator.cpp` tdcMarkCallback:
   `scheduleByAngle(&tdcScheduler[revIndex2], nowNt, tdcPosition,
   onTdcCallback)` from the sync event (trgEventIndex == 0). At the sync
   phaseFromSyncPoint = 0, so E(sync) = wrap(-112) = 608; the hook fires 112
   crank degrees later at **E = 0** (TDC1). The in-file premise "the hook
   fires at engine phase tdcPosition()" (`map_averaging.cpp`,
   scheduleNextCycleWindows comment) is therefore wrong - this is the
   upstream mistake.
3. **The double subtraction.** `map_averaging.cpp`
   scheduleNextCycleWindows: `windowOffset = wrap(mapAveragingStart[i] -
   tdcPhase)` with `tdcPhase = tdcPosition()`, fired from E = 0 ->
   actual fire phase = `wrap(start[i] - tdcPosition())`.
   `mapAveragingStart[i]` is already engine-phase space
   (`onFastCallback`: `samplingAngle(rpm) + cylinders[i].getAngleOffset()`;
   the pre-Variant-A per-tooth code compared exactly these angles against
   engine phase via isPhaseInRange - same space, one subtraction, worked).
4. **The cancel that eats the close.** scheduleNextCycleWindows does
   `engine->scheduler.cancel(&samplers[i].timer)` before re-arming each slot;
   `startAveraging` re-arms the SAME node with its end
   (`scheduleByAngle(&s->timer, ..., mapAveragingDuration, endAveraging)`).
   A pending end that survives past the cycle boundary is therefore killed
   by the next plan.

## 3. Geometry (m74_9 numbers)

Tune values (21129.msq):

- sampling angle s(rpm), deg: 650->100, 800->104, 1700->109, 2600->113,
  3500->117, 4400->121, 5300->126, 7000->130.
- window duration: 50 deg at all bins; measureMapOnlyInOneCylinder = no.
- cylinder angle offsets (cylinders[i].getAngleOffset(), i = cylinder
  NUMBER): cyl1 = 0, cyl2 = 540, cyl3 = 180, cyl4 = 360 (firing 1-3-4-2).

At the session rpm (~1150) s ~= 106. Firing phases with the bug
(actual = wrap(start - 112)):

| slot i | cylinder | nominal start | actual start | actual end | result                     |
| ------ | -------- | ------------- | ------------ | ---------- | -------------------------- |
| 0      | 1        | ~106          | 714          | 764 = 44   | **cancelled by next plan** |
| 2      | 3        | ~286          | 174          | 224        | fires                      |
| 3      | 4        | ~466          | 354          | 404        | fires                      |
| 1      | 2        | ~646          | 534          | 584        | fires                      |

Exactly one closing event is lost per engine cycle - the measured 3/4. The
lost close belongs to **cylinder 1** (slot 0), and the broken Variant A chain
link is therefore `W1 -> mass of cylinder 3`
(firingOrderSuccessor(0) = 2 = cyl 3, minimal positive angle offset).

## 4. rpm dependence (a testable prediction)

Slot 0's end survives iff `wrap(s - 112) + 50 < 720`, i.e. iff s > 112.
s crosses 112 between the bins 1700 (109) and 2600 (113) -> at ~2375 rpm.

Prediction: the currently flashed build shows endAveraging **3/cyc below
~2375 rpm and 4/cyc above**; the window PLACEMENT error (112 deg early) is
present at every rpm regardless of the close count. One lockstats at ~3000
rpm showing 4/4 would confirm the mechanism without any code change - a
cheap verification step before touching the fix.

## 5. Impact

What actually breaks, ordered by severity:

1. **Window placement**: every window samples ~112 deg earlier than the tune
   asks - the 100..130 deg samplingAngle table is effectively shifted to
   -12..+18 deg ATDC. On a shared plenum at steady state MAP is nearly
   phase-independent, so the engine ran clean all session (the lockstats
   look healthy precisely because of this).
2. **Below ~2375 rpm, cylinder 1's window never closes**:
   - `mapPerCylinder[0]` freezes at its last value while running
     (checkable live-data channel - the on-car confirmation of this record);
   - the single averager keeps accumulating from ~714 deg until the next
     slot's start resets it (~174 deg of the next cycle): a ~190-deg smear
     whose data is discarded, so no BAD value enters, but the behavior is
     wrong and per-cylinder diagnostics for cyl 1 are dead;
   - the Variant A chain link "last window of the cycle feeds the first
     cylinder of the next one" never executes via endAveraging for
     cyl1 -> cyl3. The mass is still corrected by
     `refreshInjectionMassForCylinder` on the scheduling tooth
     (MapFast / basis) - one window older data. This is the designed
     redundancy that masks the bug and is the reason the mixture did not
     drift.
   - MapFast updates 3x/cyc instead of 4x; the sniffer MAP pin stays high
     across the boundary (merged ~238-deg pulse).
3. **Misfire planner** (`scheduleNextCycleSegments`,
   misfire_detection.cpp) computes the identical offset - latent while
   misfireDetectionEnabled = disabled (it is, in 21129.msq).

Not affected (validated in the same session): dwell/spark/injection angle
clock (lateArm 0 everywhere), tim2clk, injCC2, executor lateness profile.
The bug is contained in the MAP-window / misfire planning layer.

## 6. Why the tests did not catch it

The planner tests (`unit_tests/.../test_map_averaging.cpp`) set
`triggerShape.tdcPosition = 0` (per the plan doc: "triggerShape.tdcPosition
= 0 в тесте, offset == абсолютный угол окна"). With tdcPosition() = 0 the
subtraction is a no-op and the offset coincides with the correct answer; the
nonzero-tdcPosition path (112 on the real car) is never exercised, and no
test asserts that the close count equals the start count for a window whose
end crosses the cycle boundary.

## 7. Fix

APPLIED (working tree, 2026-09-24):

- `map_averaging.cpp` scheduleNextCycleWindows: `windowOffset =
  wrapAngle(mapAveragingStart[i])` - the `tdcPhase` subtraction and the
  misleading comment are gone (the comment now documents that the TDC hook
  fires at engine phase 0).
- `misfire_detection.cpp` scheduleNextCycleSegments: `startOffset =
  windowStart` (same removal; `segmentWidth`/`endOffset` untouched - angle
  differences are reference-independent).
- `misfire_detection.h` now declares the two segment trampolines (moved out
  of the .cpp's anonymous namespace - startAveraging-style) so the tests can
  identify the scheduled events by callback. Gotcha found while doing this:
  a namespace-scope prototype for a function DEFINED inside an anonymous
  namespace creates two same-signature entities, the name becomes an
  overload set and `action_s::make<Func>` (an `auto` NTTP) rejects it -
  keep the definition and the declaration at the same scope.

Regression tests added (all three green):

- `MapAveragingModule_scheduleWindowsWithNonzeroTdcOffset` - with
  triggerShape.tdcPosition = 0 and globalTriggerAngleOffset = 112 (the m74_9
  values), the four planned starts sit at 105/285/465/645 deg (before the
  fix: 713/173/353/533).
- `MapAveragingModule_allWindowsCloseAcrossCycleBoundary` - samplingAngle
  105 < tdcPosition: after a full cycle plus a re-plan at the next mark,
  all four windows closed (isMapAveraging = false past the boundary).
- `MisfireDetection.plannerSegmentOffsetsIgnoreTdcPosition` - cylinder 0's
  segment starts at 20 deg (not wrap(20-112) = 628).

Validation: unit tests 1163 pass / 12 fail (the same 12 pre-existing
injection/trigger failures, none new); m74_9 board build BUILD SUCCESSFUL.

Expected car signature after the fix: endAveraging 4/cyc, live
mapPerCylinder[0], sniffer shows 4 clean 50-deg pulses. The not-yet-run
pre-fix prediction test (4/4 above ~2375 rpm on the unfixed build) is now
moot - validate directly on the fixed build instead.

Cautions:

- the fix moves the MAP sampling phase by 112 deg - it changes when the
  VE-table MAP value is sampled, so re-validate idle VE/AFR after it lands
  (the samplingAngle table should then mean what it says);
- the old per-tooth code (the 09-23 build) sampled at the correct phase -
  any tune mapping done against 09-23 sessions remains valid after the fix;
  mapping done on TODAY's Variant A build sampled 112 deg early.

## 8. Related records

- docs/report.md, dated bug entry (2026-09-24)
- CLAUDE.md, "MAP/misfire out of the handoff" section: planner invariant
  correction bullet
