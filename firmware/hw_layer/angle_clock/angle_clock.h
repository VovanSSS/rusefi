/**
 * @file angle_clock.h
 *
 * Hardware angle clock: three APB1 timers as pure software comparators,
 * one timer per event type, one channel per cylinder.
 *
 *   TMR2 (32-bit, VectorB0, prio 3) -> dwell starts   [cylinder 0..3 = ch 0..3]
 *   TMR4 (16-bit, VectorB8, prio 3) -> spark fires     [cylinder 0..3 = ch 0..3]
 *   TMR3 (16-bit, VectorB4, prio 3) -> injection starts[cylinder 0..3 = ch 0..3]
 *
 * FIXED CHANNEL ASSIGNMENT eliminates the "no channel available" arm failure
 * of the old single-pool design: each cylinder owns a dedicated channel in
 * each timer. A channel is always free at arm time because the previous event
 * on that channel fired within one engine cycle (~150 ms @ 800 rpm) and was
 * released by the ISR immediately. No search, no conflict possible.
 *
 * 16-bit timers (TMR3/TMR4): same APB1 4 MHz clock as TMR2/TIM5.
 * The CCR is a 16-bit value; the arm check uses signed 16-bit arithmetic so
 * delays > ~8 ms (half the 16.384 ms counter period) return false and the
 * caller falls back to TIM5 - this degrades gracefully below ~200 rpm where
 * the 2-tooth scheduling lead exceeds 8 ms. The angle clock is not needed
 * below ~200 rpm anyway.
 *
 * Timing identity with the FALSE build: at steady-state rpm the tick computed
 * from oneDegreeUs on tooth T-1 plus the per-tooth refresh on tooth T gives
 * the same absolute compare moment as TIM5 would schedule on tooth T. The
 * advantage is that the TMR ISR fires independently of the handoff latency
 * at tooth T - a 1 ms handoff tail no longer delays the event.
 */

#pragma once

#include "scheduler.h"
#include "efitime.h"

// Pure tick math, host-testable.
static inline uint32_t angleClockDelayTicks(float angleFromNow, float ticksPerDegree) {
    return static_cast<uint32_t>(angleFromNow * ticksPerDegree);
}

#if EFI_ANGLE_CLOCK

// LOOKAHEAD (2026-09-26): the one-tooth-ahead early window at tooth T-1 spans
// [nextPhase, nextNextPhase). A dwell/inj START target that lands just past
// nextNextPhase - the m74_9 START angles sit AT tooth boundaries by
// construction (advance + dwell relationships) - is excluded by that window,
// so it falls to the current-window arm at remaining ~ 0, and any handoff
// latency (~100-250 us on scheduling teeth) makes it past-due at refresh ->
// immediate fire (the residual ~2% over a drive that Option B's fresh basis
// could not remove - it is a window/arming artifact, not a basis lag).
// Extending the upper bound by ANGLE_CLOCK_LOOKAHEAD_DEG arms those boundary
// events at T-1 with a full tooth of lead; the per-tooth angleClockRefresh
// keeps the CCR accurate, and dwellStartArmed / injectionStartArmed guard off
// double arming. The charge-anchored overdwell rescue bounds a late start.
//
// The bound is consumed as isPhaseInRange(target, nextPhase,
// nextNextPhase + ANGLE_CLOCK_LOOKAHEAD_DEG). When nextNextPhase + K crosses
// 720 the wrap branch of isPhaseInRange extends the window to the cycle end
// exactly the boundary events near the wrap, no degenerate match-everything
// (that only happens when next <= current, impossible with nextNextPhase >
// nextPhase and K > 0). K = 6 deg = one nominal 60-2 tooth edge (720/116 ~=
// 6.2); an armed target at T-1 sits up to ~2 edges + K = ~18.4 deg past the
// phase, well inside MAX_LEAD_DEG (30).
static constexpr float ANGLE_CLOCK_LOOKAHEAD_DEG = 6.0f;

// Re-anchor deadband (2026-09-26): refresh only rewrites a CCR when the fresh
// prediction is MEANINGFULLY earlier than the armed tick. The only-earlier
// anchor means a "pull" is always oldCcr - newTick, and the oneDegreeUs
// (90-deg rpm average) basis re-derived on every tooth jitters by a few ticks
// (float rounding, the rpm average updating), so the
// armed-tick-vs-fresh-prediction gap is almost never exactly zero - refresh
// would rewrite the CCR (and pay the peripheral write + SR re-read) just to
// absorb sub-µs noise. Keeping the armed CCR for pulls <= DEADBAND fires the
// event at most DEADBAND later than the freshest prediction: at 7000 rpm 2 us
// = 0.08 deg, far below the VRS conditioner constant 0.43 deg and the
// ms-class scheduling precision loss - harmless for a START edge (the
// injection close is anchored to the actual open; spark is re-armed from the
// charge moment). Real accel/decel corrections exceed the deadband and still
// re-anchor. Watch `angclk refresh ... maxPullUs` to size this: if
// maxPullUs stays large, the arm basis is genuinely stale and the deadband
// must not absorb that class.
static constexpr uint32_t ANGLE_CLOCK_REANCHOR_DEADBAND_TICKS = US2NT(2);

// Configure TMR2/TMR3/TMR4, measure rate, enable NVIC. Call once at boot,
// after the TIM5 executor is running.
void initAngleClock();

// Convert an NT timestamp to an absolute 32-bit angle-clock tick (TMR2 domain).
uint32_t angleClockTickForNt(efitick_t nt);

// Current TMR2 free-running counter value (for diagnostics).
uint32_t angleClockNow();

// Feed the per-tooth state used by all three timers. Call once per tooth
// (including rpm==0 storm-flap teeth - the refresh must track true speed).
// A not-positive ticksPerDegree (NaN on rpm==0 flap) keeps the last good
// basis without touching the phase or timestamp.
void angleClockOnTooth(efitick_t edgeTimestamp, float currentPhase,
                        float cycleDeg, float ticksPerDegree);

// Diagnostic snapshot of the most recent failed dwell arm.
//   branch: 0 = no angle->time basis (s_ticksPerDegree <= 0)
//           1 = stale phase (remaining > MAX_LEAD_DEG)
//           2 = target tick already in the past (tick-past)
//   window: 0 = early (scheduleDwellEarlyIfDue), 1 = current (onTriggerEventSparkLogic)
struct DwellArmRefusal {
    uint8_t  branch;
    uint8_t  window;
    uint8_t  cyl;
    float    targetAngle;      // dwellAngle passed by the caller
    float    callerPhase;      // currentPhase at the call site
    float    callerNextPhase;  // nextPhase at the call site
    float    currentPhase;     // angle clock s_currentPhase
    float    cycleDeg;         // angle clock s_cycleDeg (720 for 4-stroke)
    float    ticksPerDegree;   // angle clock s_ticksPerDegree
    float    remaining;        // remainingAngle(targetAngle) in angle-clock phase frame
    uint32_t atTick;           // computed target tick (tick-past branch only)
    uint32_t ccrCnt;           // DWELL_TIMER->CNT at refusal
    uint32_t lateArmTotal;     // s_lateArmDwell at refusal
};

// Arm the dwell-start channel for `cylinderIndex` on TMR2.
// `earlyWindow` records which caller armed it (true = early window, false =
// current window) - diagnostic only, the arm logic is identical either way.
// Returns false when the delay would exceed the scheduler lead window or when
// the target tick is already in the past - caller falls back to TIM5.
bool angleClockArmDwell(int cylinderIndex, float targetAngle, action_s action,
                         float callerPhase, float callerNextPhase, bool earlyWindow);

// Arm the spark-fire channel for `cylinderIndex` on TMR4 (16-bit).
// Falls back at delays > ~8 ms (< ~200 rpm on 60-2, 2-tooth lead).
bool angleClockArmSpark(int cylinderIndex, float targetAngle, action_s action,
                         float callerPhase, float callerNextPhase);

// Arm the spark-fire channel for `cylinderIndex` on TMR4 directly from an
// NT timestamp + delay. Used by turnSparkPinHighStartCharging to guarantee
// the spark fires AFTER the coil is charged, regardless of the rpm basis
// used when the dwell was armed. The targetAngle sentinel (780 deg) makes
// the per-tooth refresh skip this channel (780 > cycleDeg + MAX_LEAD_DEG).
// Returns false (caller falls back to TIM5) when the delay is too small.
bool angleClockArmSparkFromNow(int cylinderIndex, efitick_t nowNt,
                                uint32_t delayNt, action_s action);

// Arm the injection-start channel for `cylinderIndex` on TMR3 (16-bit).
bool angleClockArmInjection(int cylinderIndex, float targetAngle, action_s action,
                              float callerPhase, float callerNextPhase);

// Arm the injection-END channel for `cylinderIndex` on TMR3 directly from an
// NT timestamp + delay. Called from turnInjectionPinHigh (the actual injector-
// open callback) to anchor the closing pulse to the REAL opening moment,
// regardless of any TMR3/TIM5 START timing uncertainty. Analogue of
// angleClockArmSparkFromNow for injection close. targetAngle sentinel 780 deg
// prevents the per-tooth refresh from touching this channel.
bool angleClockArmInjectionFromNow(int cylinderIndex, efitick_t nowNt,
                                    uint32_t delayNt, action_s action);

// Re-anchor all armed channels from the freshest tooth data. Due events
// (angle already passed) are armed for immediate ISR firing - both dwell
// and spark/injection, matching the time-based build's due-tooth behavior.
// Call at the end of every trigger handoff (including rpm==0 flap teeth,
// before the rpm gate return).
void angleClockRefresh();

// Cancel one channel by cylinder index. Safe when the channel is free.
void angleClockCancelDwell(int cylinderIndex);
void angleClockCancelSpark(int cylinderIndex);
void angleClockCancelInjection(int cylinderIndex);

// Cancel every channel on every timer. Not called from hot paths.
void angleClockCancelAll();

// --- Telemetry ---
uint32_t angleClockFiredDwell();
uint32_t angleClockFiredSpark();
uint32_t angleClockFiredInj();
uint32_t angleClockLateArmDwell();
uint32_t angleClockLateArmSpark();
uint32_t angleClockLateArmInj();

// Split of angleClockLateArmDwell() by failure reason - tells which of the two
// arm-failure branches fires when dwell falls back to TIM5.
uint32_t angleClockLateArmDwellGuard();     // armGuard: no basis OR stale phase
uint32_t angleClockLateArmDwellTickPast();  // target tick already in the past

// Same split for injection (TMR3): guard = armGuard refusal (no basis OR stale
// phase = the resync/phase-jump class), tickPast = the target tick already
// passed (the handoff-stall class). This discriminates the two hypotheses for
// the growing angclk inj lateArm.
uint32_t angleClockLateArmInjGuard();
uint32_t angleClockLateArmInjTickPast();

// Snapshot of the most recent failed dwell arm (see DwellArmRefusal).
const DwellArmRefusal& angleClockDwellRefusal();
uint32_t angleClockMaxLateTicks();
uint32_t angleClockImmediateFireCount();

// LOOKAHEAD telemetry (2026-09-26): count of early-window arms whose target
// landed in the extended band [nextNextPhase, nextNextPhase + LOOKAHEAD) - the
// boundary events that previously fell to the current-window arm at rem ~ 0
// and fired past-due (immediate). > 0 proves the extension is engaged. Dwell
// vs inj split for attribution. The band check is in the inline helpers below.
uint32_t angleClockLookaheadDwell();
uint32_t angleClockLookaheadInj();
void angleClockRecordLookaheadDwell();
void angleClockRecordLookaheadInj();

// Refresh write-split telemetry (2026-09-26): deadband absorbs sub-deadband
// pulls (per-tooth basis jitter) so `write` counts only MEANINGFUL re-anchors
// (real accel/decel corrections). jitter = the absorbed small pulls (the
// "armed tick was slightly late" population); maxPullTicks = worst prediction
// gap seen in ticks - /4 = us. If maxPullUs stays large the arm basis is
// genuinely stale and the deadband must not absorb that class.
uint32_t angleClockRefreshJitter();
uint32_t angleClockRefreshMaxPullTicks();

// Classify an early-window arm into the LOOKAHEAD band and bump the counter.
// Call ONLY when the extended early window matched (scheduleEarly == true);
// the band check is precise across the 720 wrap (target >= nextNextPhase &&
// target < nextNextPhase + K) so a wrap-case target in the normal [nextPhase,
// 720) part is NOT misattributed.
static inline void angleClockNoteLookaheadDwell(float targetAngle, float nextNextPhase) {
    if (targetAngle >= nextNextPhase &&
        targetAngle < nextNextPhase + ANGLE_CLOCK_LOOKAHEAD_DEG) {
        angleClockRecordLookaheadDwell();
    }
}
static inline void angleClockNoteLookaheadInj(float targetAngle, float nextNextPhase) {
    if (targetAngle >= nextNextPhase &&
        targetAngle < nextNextPhase + ANGLE_CLOCK_LOOKAHEAD_DEG) {
        angleClockRecordLookaheadInj();
    }
}

// Refresh telemetry (2026-09-25 skip-write): per-window counts of how the
// every-tooth angleClockRefresh() disposed of the 12 armed channels (4x
// dwell/spark/inj). skip dominates in steady rpm (armed CCR already optimal - the
// no-op the optimization removes); write marks accel/decel teeth where the
// only-earlier anchor actually moved a CCR; pending = a channel whose CCxIF was
// already set (the TMR ISR is about to fire the standing CCR).
uint32_t angleClockRefreshSkip();       // skip-write: no CCR write, no SR re-read
uint32_t angleClockRefreshReanchored(); // actually rewrote a (changed) CCR
uint32_t angleClockRefreshPending();    // left a channel with a pending CCxIF

// Init-time measurement (printed in lockstats for fuse-incident detection).
uint32_t angleClockInitAcDelta();
uint32_t angleClockInitNtDelta();
uint32_t angleClockInitPsc();

void angleClockResetStats();

// Runtime clock sanity for TMR2 (dwell). Reads the APB1 clock-enable, sleep-
// enable and reset bits, CR1 (CEN), CR2, SMCR and PSC, then measures TIM2->CNT
// vs NT over BOTH a 100 ms sleep (WFI) and a 100 ms busy-wait. The sleep-vs-
// busy split tells whether the 2026-09-20 TMR2 crawl is a sleep gate (sleep
// T2d << busy T2d) or an always-on slow clock (both low).
void angleClockTimerDiag();

#endif // EFI_ANGLE_CLOCK
