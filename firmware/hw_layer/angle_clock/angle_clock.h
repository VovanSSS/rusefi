/**
 * @file angle_clock.h
 *
 * Hardware angle clock: ONE 32-bit timer (TMR2 on AT32F435, VectorB0, prio 3)
 * with four compare channels, one per engine event TYPE:
 *
 *   CC1 = dwell start       (angle-domain, armed by the handoff, refreshed)
 *   CC2 = spark fire        (from-now, armed at dwell fire)
 *   CC3 = injection open    (angle-domain, armed by the handoff, refreshed)
 *   CC4 = injection close   (from-now, armed at open fire)
 *
 * Fixed channel-by-type assignment: at most one event of each type is ever
 * pending in sequential/individual-coils operation (the m74_9 mode), so a
 * channel is always free at arm time - no search, no per-cylinder slots.
 * The single-pool "no channel available" failure class of the old
 * per-cylinder pool design is gone for good: an arm that finds its channel
 * busy (wasted spark / batch / a storm over-arm) fails and the caller falls
 * back to TIM5 - counted as lateArm busy.
 *
 * This is a consolidation of the 2026-09-01 three-timer design
 * (TMR2 dwell / TMR4 spark / TMR3 injection + TIM5 CC2 injection-close
 * mini-queue) onto one timer. All channels are 32-bit now, so the 16-bit
 * PW > 8 ms limit that exiled the injection close to TIM5 CC2 is gone, the
 * TMR3/TMR4 < 200 rpm degradation is gone, and the injection close runs at
 * the same prio 3 as the other three types (it was the most blocked path
 * at prio 5, behind the handoff and the TMR fires).
 *
 * The per-tooth angleClockRefresh re-anchors only the two ANGLE-domain
 * channels (dwell, open) from the freshest tooth basis; the from-now
 * channels (spark, close) carry the 780 deg sentinel targetAngle so the
 * refresh skips them (remainingAngle(780) > MAX_LEAD_DEG for any phase in
 * [0, 720)). The armedMask early-out makes the refresh a single bit test
 * on the ~93% of teeth that have nothing armed.
 *
 * Concurrency (same analysis as the three-timer design, see the
 * 2026-09-28 priority ladder): the TMR2 ISR runs at prio 3, above the
 * handoff (4) and the executor (5). A fire preempting the handoff mid-arm
 * touches a DIFFERENT channel (disjoint SR/DIER bits and CCR registers) -
 * trivially safe; same-channel re-entry at arm time is impossible (the
 * channel was released by the previous fire and the caller only re-arms
 * for the next cycle 180+ deg later). The ordered arm sequence (clear
 * stale CCxIF -> write future CCR -> store action -> enable CCxIE) is
 * load-bearing. The ISR releases a channel before executing the action,
 * so a re-arm from inside the action (spark armed from the dwell action,
 * close armed from the open action) is safe even though both run on the
 * same vector.
 */

#pragma once

#include "scheduler.h"
#include "efitime.h"

// Pure tick math, host-testable.
static inline uint32_t angleClockDelayTicks(float angleFromNow, float ticksPerDegree) {
    return static_cast<uint32_t>(angleFromNow * ticksPerDegree);
}

#if EFI_ANGLE_CLOCK

// Channel numbers by event type (fixed assignment, == TMR2 CC1..CC4).
static constexpr int ANGLE_CLOCK_CH_DWELL = 0;  // CC1: dwell start (angle)
static constexpr int ANGLE_CLOCK_CH_SPARK = 1;  // CC2: spark fire (from-now)
static constexpr int ANGLE_CLOCK_CH_OPEN  = 2;  // CC3: injection open (angle)
static constexpr int ANGLE_CLOCK_CH_CLOSE = 3;  // CC4: injection close (from-now)

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

// Configure TMR2, measure rate, enable NVIC. Call once at boot, after the
// TIM5 executor is running (the init-time rate measurement uses the NT domain
// as the reference - see the initAngleClock() comment in angle_clock.cpp).
void initAngleClock();

// Current TMR2 free-running counter value (the angle clock's OWN time base -
// captured in the fast EXTI IRQ and read by the from-now arms).
uint32_t angleClockNow();

// Current angle->time basis (ticks per degree) used by the angle arms.
// Diagnostics: the spark schedule ring (spark_logic.cpp) records it per
// schedule to tell a garbage-basis storm arm (tpd huge -> armed far in the
// future -> fires ~1 cycle late) from a phase-basis jump (gapCorr/sync).
// 0 = no valid basis yet.
uint32_t angleClockTicksPerDegree();

// Feed the per-tooth state used by the angle channels. Call once per tooth
// (including rpm==0 storm-flap teeth - the refresh must track true speed).
// edgeAcTick is the TMR2-domain edge timestamp captured by the fast EXTI IRQ
// at this tooth's edge (NOT an NT timestamp - the angle clock never reads
// TIM5). A not-positive ticksPerDegree (NaN on rpm==0 flap) keeps the last
// good basis without touching the phase or timestamp.
void angleClockOnTooth(uint32_t edgeAcTick, float currentPhase,
                        float cycleDeg, float ticksPerDegree);

// Diagnostic snapshot of the most recent failed dwell arm.
//   branch: 0 = no angle->time basis (s_ticksPerDegree <= 0)
//           1 = stale phase (remaining > MAX_LEAD_DEG)
//           2 = target tick already in the past (tick-past)
//           3 = channel busy (a previous dwell still armed)
//   window: 0 = early (scheduleDwellEarlyIfDue), 1 = current (onTriggerEventSparkLogic)
struct DwellArmRefusal {
    uint8_t  branch;
    uint8_t  window;
    float    targetAngle;      // dwellAngle passed by the caller
    float    callerPhase;      // currentPhase at the call site
    float    callerNextPhase;  // nextPhase at the call site
    float    currentPhase;     // angle clock s_currentPhase
    float    cycleDeg;         // angle clock s_cycleDeg (720 for 4-stroke)
    float    ticksPerDegree;   // angle clock s_ticksPerDegree
    float    remaining;        // remainingAngle(targetAngle) in angle-clock phase frame
    uint32_t atTick;           // computed target tick (tick-past branch only)
    uint32_t ccrCnt;           // ANGLE_TIMER->CNT at refusal
    uint32_t lateArmTotal;     // s_lateArm[CH_DWELL] at refusal
};

// Arm the dwell-start channel (CC1) by engine angle.
// `earlyWindow` records which caller armed it (true = early window, false =
// current window) - diagnostic only, the arm logic is identical either way.
// Returns false when the delay would exceed the scheduler lead window, when
// the target tick is already in the past, or when the channel still holds the
// previous dwell (wasted spark / storm over-arm) - caller falls back to TIM5.
bool angleClockArmDwell(float targetAngle, action_s action,
                        float callerPhase, float callerNextPhase, bool earlyWindow);

// Arm the injection-open channel (CC3) by engine angle.
bool angleClockArmInjection(float targetAngle, action_s action,
                            float callerPhase, float callerNextPhase);

// Arm the spark-fire channel (CC2) directly from a TMR2-domain timestamp +
// delay (nowAc = angleClockNow() read in the dwell-fire ISR - the same
// counter the ISR itself fired from, no NT conversion). Used by
// turnSparkPinHighStartCharging to guarantee the spark fires AFTER the coil
// is charged, regardless of the rpm basis used when the dwell was armed.
// The targetAngle sentinel (780 deg) makes the per-tooth refresh skip this
// channel (780 > cycleDeg + MAX_LEAD_DEG). Returns false (caller falls back
// to TIM5) when the delay is too small or the channel is busy.
bool angleClockArmSparkFromNow(uint32_t nowAc, uint32_t delayNt, action_s action);

// Arm the injection-close channel (CC4) directly from a TMR2-domain timestamp
// + delay. Called from turnInjectionPinHigh (the actual injector-open
// callback) to anchor the closing pulse to the REAL opening moment,
// regardless of any TIM5/TMR2 START timing uncertainty. Analogue of
// angleClockArmSparkFromNow for injection close; 32-bit now, so PW up to
// ~65 ms is fine (the 16-bit PW > 8 ms limit of the old TMR3 arm is gone).
// Returns false (caller falls back to TIM5) when the delay is too small or
// the channel is busy.
bool angleClockArmInjectionCloseFromNow(uint32_t nowAc, uint32_t delayNt, action_s action);

// Re-anchor the armed ANGLE channels (dwell, open) from the freshest tooth
// data. Due events (angle already passed) are armed for immediate ISR
// firing - both dwell and injection, matching the time-based build's
// due-tooth behavior. The from-now channels (spark, close) are skipped by
// their 780 deg sentinel. Call at the end of every trigger handoff
// (including rpm==0 flap teeth, before the rpm gate return). The armedMask
// early-out makes this a single bit test when nothing is armed.
void angleClockRefresh();

// Cancel one channel by type. Safe when the channel is free.
void angleClockCancelDwell();
void angleClockCancelSpark();
void angleClockCancelInjection();
void angleClockCancelClose();

// --- Telemetry ---
// Fired / failed-arm / busy-channel counters per event type (channel index).
uint32_t angleClockFiredCount(int ch);
uint32_t angleClockLateArmCount(int ch);
uint32_t angleClockBusyCount(int ch);

// Per-channel ISR-ENTRY lateness attribution (the pooled angleClockMaxLateTicks
// cannot say which channel floats): count of fires >= 10 us late, the max, how
// many waited for the running trigger handoff (fires-above-handoff trade), and
// how many waited for the previous TMR2 entry's action bodies (single-vector
// serialization - the 60-76 us class, the injection-close action recomputes
// the model in the prio-3 ISR).
uint32_t angleClockLateGt10usCount(int ch);
uint32_t angleClockLateMaxTicks(int ch);
uint32_t angleClockLateHandoffCount(int ch);
uint32_t angleClockLateSerializedCount(int ch);

// Split of the dwell arm failure by reason - tells which of the failure
// branches fires when dwell falls back to TIM5.
uint32_t angleClockLateArmDwellGuard();     // armGuard: no basis OR stale phase
uint32_t angleClockLateArmDwellTickPast();  // target tick already in the past

// Same split for injection open (CC3): guard = armGuard refusal (no basis OR
// stale phase = the resync/phase-jump class), tickPast = the target tick
// already passed (the handoff-stall class).
uint32_t angleClockLateArmOpenGuard();
uint32_t angleClockLateArmOpenTickPast();

// Snapshot of the most recent failed dwell arm (see DwellArmRefusal).
const DwellArmRefusal& angleClockDwellRefusal();
uint32_t angleClockMaxLateTicks();
uint32_t angleClockImmediateFireCount();

// LOOKAHEAD telemetry (2026-09-26): count of early-window arms whose target
// landed in the extended band [nextNextPhase, nextNextPhase + LOOKAHEAD) - the
// boundary events that previously fell to the current-window arm at rem ~ 0
// and fired past-due (immediate). > 0 proves the extension is engaged. Dwell
// vs open split for attribution. The band check is in the inline helpers below.
uint32_t angleClockLookaheadDwell();
uint32_t angleClockLookaheadOpen();
void angleClockRecordLookaheadDwell();
void angleClockRecordLookaheadOpen();

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
static inline void angleClockNoteLookaheadOpen(float targetAngle, float nextNextPhase) {
    if (targetAngle >= nextNextPhase &&
        targetAngle < nextNextPhase + ANGLE_CLOCK_LOOKAHEAD_DEG) {
        angleClockRecordLookaheadOpen();
    }
}

// Refresh telemetry (2026-09-25 skip-write): per-window counts of how the
// every-tooth angleClockRefresh() disposed of the 2 armed ANGLE channels.
// skip dominates in steady rpm (armed CCR already optimal - the no-op the
// optimization removes); write marks accel/decel teeth where the only-earlier
// anchor actually moved a CCR; pending = a channel whose CCxIF was already
// set (the TMR ISR is about to fire the standing CCR).
uint32_t angleClockRefreshSkip();       // skip-write: no CCR write, no SR re-read
uint32_t angleClockRefreshReanchored(); // actually rewrote a (changed) CCR
uint32_t angleClockRefreshPending();    // left a channel with a pending CCxIF
uint32_t angleClockRefreshJitter();     // absorbed a sub-deadband pull (kept armed CCR)
uint32_t angleClockRefreshMaxPullTicks(); // max |fresh - armed| seen in ticks

// Init-time measurement (printed in lockstats for fuse-incident detection).
uint32_t angleClockInitAcDelta();
uint32_t angleClockInitNtDelta();
uint32_t angleClockInitPsc();

void angleClockResetStats();

// Runtime clock sanity for TMR2. Reads the APB1 clock-enable, sleep-enable
// and reset bits, CR1 (CEN), CR2, SMCR and PSC, then measures TIM2->CNT
// vs NT over BOTH a 100 ms sleep (WFI) and a 100 ms busy-wait. The sleep-vs-
// busy split tells whether the 2026-09-20 TMR2 crawl is a sleep gate (sleep
// T2d << busy T2d) or an always-on slow clock (both low).
void angleClockTimerDiag();

#endif // EFI_ANGLE_CLOCK

// TMR2-domain "now" for callers that synthesize trigger edges WITHOUT the
// fast-EXTI timestamp capture (trigger emulator self-stim, comparator/ADC
// trigger inputs, unit tests). On the angle-clock build this reads TMR2's own
// free-running counter - the SAME counter the hardware compare fires from, so
// the synthesized edge time needs no NT<->TMR2 conversion and no TIM5 read.
// Elsewhere the angle clock is compiled out and this returns 0 (unused).
static inline uint32_t angleClockSyntheticEdgeTick() {
#if EFI_ANGLE_CLOCK
    return angleClockNow();
#else
    return 0;
#endif
}
