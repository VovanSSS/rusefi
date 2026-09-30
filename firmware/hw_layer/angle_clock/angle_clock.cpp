/**
 * @file angle_clock.cpp
 *
 * Single-timer hardware angle clock.  See angle_clock.h for architecture.
 *
 * Register notes (same TIM peripheral on STM32F4 and AT32F435):
 *  - OCxM = 1 with CCxE = 0: pure software comparator; CCxIF fires on
 *    CNT == CCR without driving any output pin.
 *  - CCxIF is rc_w0: write ~flag to clear.
 *  - TMR2 runs at 4 MHz (APB1, PSC = 71 @ 288 MHz TIMCLK1). The rate is
 *    MEASURED at init against TIM5 (NT domain, physics-validated) and the
 *    PSC is re-programmed from the measurement - prevents the 2026-08-30
 *    fuse incident (wrong assumed clock rate doubled every dwell).
 *  - rccEnableTIM2(true): lp=true keeps the timer clocked during WFI sleep.
 *    lp=false would clear APB1LPENR and stop the counter in idle - the root
 *    cause of the 2026-08-31 "2x period" saga for TMR7 and the earlier TMR2
 *    offset drift.
 *
 * Concurrency (see the header): the fires run at prio 3 above the handoff
 * (4). A fire preempting the handoff mid-arm touches a DIFFERENT channel -
 * disjoint SR/DIER bits and CCR registers. The ordered arm sequence (clear
 * stale CCxIF -> write future CCR -> store action -> enable CCxIE) is
 * load-bearing. The ISR releases a channel before calling action.execute()
 * so a re-arm from inside the action is safe (the dwell action arms the
 * spark, the open action arms the close - all on this same timer/vector).
 *
 * DURABLE INVARIANTS (hard-won, see the history):
 *  - never cancel armed events on an rpm flap or sync event - armed events
 *    fire by time regardless of sync/rpm exactly like the time-based build;
 *    the bounded ticks (angle-domain + MAX_TICKS_PER_DEGREE cap) and the
 *    charge-anchored overdwell rescue are what make late firing safe,
 *    cancellation is what made it destructive (2026-08-31 car defect);
 *  - DUE EVENTS FIRE, NEVER CANCEL: refresh arms a due dwell/injection for
 *    immediate firing (the time-based build fires them too - the first-cycle
 *    starvation of "late Start is dropped" is gone);
 *  - the PMEN bit must be set before the ARR/CNT writes and kept while the
 *    counter runs, or TMR2 silently truncates to 16 bits (2026-09-20).
 */

#include "pch.h"

#if EFI_ANGLE_CLOCK && EFI_PROD_CODE

#include "angle_clock.h"

// ── Timer alias ────────────────────────────────────────────────────────────
#define ANGLE_TIMER TIM2   // 32-bit APB1, VectorB0, IRQ 28

// AT32-specific: TMR2/TMR5 are 32-bit timers but only when the "plus mode" bit
// (CR1 bit 10, PMEN) is set. The STM32F4-derived fork header stops its TIM_CR1
// defines at CKD (bits 9:8) - bit 10 is AT32-only, so it has no name here.
// Without PMEN, TMR2 operates 16-bit and PR/CNT/CCR all truncate to 16 bits:
// the 2026-09-20 ARR=0xFFFF bug (the dwell counter wrapped at 65536 instead of
// 2^32, and every arm's tick-past check failed once the NT-domain target
// exceeded 2^31).
#define STM32_TIM_CR1_PMEN (1U << 10)

// ── Shared constants ──────────────────────────────────────────────────────
// Armed tick must be at least ARM_MARGIN_TICKS in the future so the CCR
// write and CCxIE enable complete before the match.  4 us mirrors TIM5.
static constexpr uint32_t ARM_MARGIN_TICKS = US2NT(4);

// Reject targets farther than MAX_LEAD_DEG degrees from s_currentPhase:
// the early windows arm at most 2 teeth ahead (~12.4 deg on 60-2); anything
// beyond this means the phase basis jumped (desync/re-sync) and the angle
// is no longer meaningful.
static constexpr float MAX_LEAD_DEG = 30.0f;

// Absolute ceiling on the angle->time basis fed by angleClockOnTooth.
// 2 ms/deg ~ 32 rpm; bounds the armed delay so a garbage basis (e.g., the
// 10 s decoder clamp at the first teeth) cannot stick a channel for the run.
static constexpr float MAX_TICKS_PER_DEGREE = US2NT(2000);

// Sentinel targetAngle for from-now armed channels: > cycleDeg(720) +
// MAX_LEAD_DEG(30) = 750, so remainingAngle(780) = 780 - currentPhase > 60 >
// MAX_LEAD_DEG for ANY currentPhase in [0, 720) - the per-tooth refresh
// always skips these channels. This prevents the refresh from re-anchoring
// time-domain (not angle-domain) CCRs.
static constexpr float FROM_NOW_SENTINEL = 780.0f;

static constexpr uint32_t CC_IF_MASK =
    STM32_TIM_SR_CC1IF | STM32_TIM_SR_CC2IF |
    STM32_TIM_SR_CC3IF | STM32_TIM_SR_CC4IF;

// Refresh only touches the two angle-domain channels; the from-now channels
// (spark, close) are skipped by their sentinel.
static constexpr uint8_t REFRESH_MASK =
    (1u << ANGLE_CLOCK_CH_DWELL) | (1u << ANGLE_CLOCK_CH_OPEN);

// ── Per-channel state (one channel per event type) ────────────────────────
struct AngleClockChannel {
    action_s action;
    float    targetAngle = 0.0f;
    uint32_t ccr = 0;   // shadow of the armed compare value
};

static AngleClockChannel s_ch[4];  // dwell / spark / open / close
static uint8_t s_armedMask = 0;    // bit per channel holding a pending action

// ── Per-tooth state (shared, written only by handoff prio 4) ─────────────
// s_edgeAcTick is the TMR2-DOMAIN edge timestamp captured by the fast EXTI
// IRQ at this tooth's edge (see digital_input_exti.cpp). TMR2 is its own time
// base: the arms never convert from NT and never read TIM5 - the offset
// conversion (angleClockTickForNt + s_timerOff) that lived here until
// 2026-09-29 is gone. The ticksPerDegree basis fed by angleClockOnTooth is
// numerically identical in both domains (the boot-time PSC measurement makes
// TMR2 tick at exactly the NT 4 MHz rate), so a delay computed in ticks from
// the angle is directly a TMR2 tick count.
static uint32_t s_edgeAcTick    = 0;
static float    s_currentPhase   = 0.0f;
static float    s_cycleDeg       = 720.0f;
static float    s_ticksPerDegree = 0.0f;

// ── Telemetry ─────────────────────────────────────────────────────────────
static uint32_t s_fired[4]       = {0, 0, 0, 0};
static uint32_t s_lateArm[4]     = {0, 0, 0, 0};
static uint32_t s_lateArmBusy[4] = {0, 0, 0, 0};
static uint32_t s_lateArmDwellGuard    = 0;  // dwell armGuard refusal (no basis / stale phase)
static uint32_t s_lateArmDwellTickPast = 0;  // dwell target tick already in the past
static uint32_t s_lateArmOpenGuard     = 0;  // open armGuard refusal (no basis / stale phase)
static uint32_t s_lateArmOpenTickPast  = 0;  // open target tick already in the past
static uint32_t s_maxLateTicks = 0;
static uint32_t s_immediate    = 0;
static uint32_t s_refreshSkip         = 0;  // refresh: armed CCR already optimal (steady-state no-op)
static uint32_t s_refreshReanchored   = 0;  // refresh: rewrote a CCR (pull > deadband = real correction)
static uint32_t s_refreshJitter       = 0;  // refresh: absorbed a sub-deadband pull (kept armed CCR)
static uint32_t s_refreshMaxPullTicks = 0;  // refresh: max |fresh - armed| seen in ticks (any non-skip visit)
static uint32_t s_refreshPending      = 0;  // refresh: left a channel with a pending CCxIF
static uint32_t s_lookaheadDwell      = 0;  // early-window dwell arm in [nextNext, nextNext+K) (2026-09-26)
static uint32_t s_lookaheadOpen       = 0;  // early-window open arm in [nextNext, nextNext+K)

// Most recent failed dwell arm (diagnostic snapshot, read by lockstats).
// branch = 0xFF is the "no refusal since last reset" sentinel, set in
// initAngleClock() - the struct must NOT carry a non-trivial aggregate
// initializer (the zero-initialized branch=0 would print a bogus all-zero
// refusal line on the FIRST lockstats after boot).
static DwellArmRefusal s_dwellRefusal;

static uint32_t s_initAcDelta = 0;
static uint32_t s_initNtDelta = 0;
static uint32_t s_initPsc     = 0;

// ── CCR register pointer ──────────────────────────────────────────────────
static volatile uint32_t* ccrReg(TIM_TypeDef* tmr, int ch) {
    switch (ch) {
    case 0:  return &tmr->CCR1;
    case 1:  return &tmr->CCR2;
    case 2:  return &tmr->CCR3;
    default: return &tmr->CCR4;
    }
}

// ── Tick conversions ──────────────────────────────────────────────────────
TRIGGER_RAM_CODE static float remainingAngle(float targetAngle) {
    float r = targetAngle - s_currentPhase;
    if (r < 0.0f) r += s_cycleDeg;
    return r;
}

TRIGGER_RAM_CODE uint32_t angleClockNow() {
    return ANGLE_TIMER->CNT;
}

uint32_t angleClockTicksPerDegree() {
    // NaN casts to 0 in hardware (vcvtr saturates) - informative (no basis).
    return static_cast<uint32_t>(s_ticksPerDegree);
}

// ── ISR dispatch ──────────────────────────────────────────────────────────
TRIGGER_RAM_CODE static void dispatch() {
    // Pass 1: release every due channel. ALL channels are released BEFORE any
    // action executes, so a re-arm from inside an action (dwell arms spark,
    // open arms close) can never hit a busy channel - including the
    // exactly-100%-duty corner where the previous close (CC4) and the next
    // open (CC3) match in the same ISR entry.
    uint32_t sr = ANGLE_TIMER->SR & CC_IF_MASK;
    action_s due[4];
    uint32_t dueCcr[4];
    uint8_t dueMask = 0;

    for (int ch = 0; ch < 4; ch++) {
        const uint32_t flag = STM32_TIM_SR_CC1IF << ch;
        if (!(sr & flag)) continue;

        ANGLE_TIMER->SR    = ~flag;
        ANGLE_TIMER->DIER &= ~(STM32_TIM_DIER_CC1IE << ch);

        auto& c = s_ch[ch];
        due[ch]    = c.action;
        dueCcr[ch] = c.ccr;
        c.action = {};
        dueMask |= static_cast<uint8_t>(1u << ch);
    }
    s_armedMask &= static_cast<uint8_t>(~dueMask);

    // Pass 2: execute the released actions in channel order (dwell -> spark ->
    // open -> close).
    for (int ch = 0; ch < 4; ch++) {
        if (!(dueMask & (1u << ch))) continue;
        action_s act = due[ch];
        if (act) {
            const int32_t late =
                static_cast<int32_t>(static_cast<uint32_t>(ANGLE_TIMER->CNT) - dueCcr[ch]);
            s_fired[ch]++;
            if (late > 0 && static_cast<uint32_t>(late) > s_maxLateTicks)
                s_maxLateTicks = static_cast<uint32_t>(late);
            act.execute();
        }
    }
}

extern "C" {

TRIGGER_RAM_CODE void STM32_TIM2_HANDLER(void) {
    OSAL_IRQ_PROLOGUE();
    dispatch();
    assertInterruptPriority(__func__, EFI_IRQ_ANGLE_CLOCK_PRIORITY);
    OSAL_IRQ_EPILOGUE();
}

} // extern "C"

// ── Initialization ────────────────────────────────────────────────────────
static void initTimerRegs(TIM_TypeDef* tmr, uint32_t psc, uint32_t cr1Mode = 0) {
    // cr1Mode carries the AT32-specific CR1 control bits (STM32_TIM_CR1_PMEN
    // for the 32-bit TMR2) that must be present BEFORE the ARR/CNT writes and
    // kept while the counter runs - otherwise the 32-bit registers truncate
    // to 16 bits (see the PMEN comment above).
    tmr->CR1   = cr1Mode;
    tmr->PSC   = psc;
    tmr->ARR   = 0xFFFFFFFFU;
    tmr->CR1   = cr1Mode;
    tmr->CCMR1 = STM32_TIM_CCMR1_OC1M(1) | STM32_TIM_CCMR1_OC2M(1);
    tmr->CCMR2 = STM32_TIM_CCMR1_OC1M(1) | STM32_TIM_CCMR1_OC2M(1);
    tmr->CCER  = 0;
    tmr->DIER  = 0;
    tmr->EGR   = STM32_TIM_EGR_UG;   // latch PSC/ARR, clear CNT
    tmr->SR    = 0;
    tmr->CR1   = STM32_TIM_CR1_CEN | cr1Mode;
}

void initAngleClock() {
    // lp=true: keep the timer clocked during WFI sleep (APB1LPENR).
    // lp=false would gate the clock and drift the NT<->timer offset on
    // every idle period - the 2026-08-31 "2x period" root cause.
    rccEnableTIM2(true);

    // Freeze alongside TIM5 on debugger halt so the NT<->timer offset
    // stays constant across core halts.
#if defined(STM32F4XX) || defined(STM32F7XX)
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_TIM2_STOP;
#endif

    // Start at the provisional PSC = 71 (assumes TIMCLK1 = 288 MHz -> 4 MHz).
    // The measured rate below corrects it.
    initTimerRegs(ANGLE_TIMER, 71, STM32_TIM_CR1_PMEN);  // TMR2: 32-bit plus mode

    // Measure the real TMR2 rate against NT (TIM5). This is load-bearing: a
    // 2x-wrong PSC doubled every dwell and blew the 15A fuse on 2026-08-30.
    // NOTE: this is the ONE deliberate TIM5 touch in the whole angle clock - a
    // boot-time, one-shot measurement. It is what makes the ticksPerDegree
    // basis (computed by the trigger decode in NT units) numerically identical
    // to TMR2 ticks, so the angle-domain arms can add tick delays directly to
    // the TMR2-domain edge timestamp with no runtime conversion and no runtime
    // TIM5 dependency whatsoever.
    const uint32_t ac0 = ANGLE_TIMER->CNT;
    const uint32_t nt0 = getTimeNowLowerNt();
    do { } while (getTimeNowLowerNt() - nt0 < US2NT(10000));  // 10 ms
    const uint32_t acDelta = ANGLE_TIMER->CNT - ac0;
    const uint32_t ntDelta = getTimeNowLowerNt() - nt0;

    // Round-half-up to nearest integer: (72*ac + nt/2) / nt - 1.
    // Plain truncating division (72*ac/nt - 1) rounds DOWN, which when
    // acDelta < ntDelta by even 1 tick (measurement noise) gives PSC=70
    // instead of 71. With PSC=70, TMR2 runs 1.4% faster than NT (TIM5),
    // the NT<->TMR2 offset drifts ~56 k ticks/s, and all arm checks fail
    // after ~10 ms -> the car stops starting (angclk psc=70 was observed
    // in the field, 2026-09-01). TMR2 shares the same APB1 TIMCLK1 as
    // TIM5, so with the same PSC the rates are identical; the measurement
    // is only a safety net and must never overshoot by rounding error.
    uint32_t newPsc = (72U * acDelta + ntDelta / 2U) / ntDelta - 1U;
    if (newPsc > 0xFFFFU) newPsc = 0xFFFFU;

    // Re-program with the measured PSC. Keep PMEN across the stop/start
    // (writing CR1=0 would drop it and make TMR2 16-bit again).
    ANGLE_TIMER->CR1 = STM32_TIM_CR1_PMEN; ANGLE_TIMER->PSC = newPsc;
    ANGLE_TIMER->CNT = 0;
    ANGLE_TIMER->EGR = STM32_TIM_EGR_UG;
    ANGLE_TIMER->SR  = 0;
    ANGLE_TIMER->CR1 = STM32_TIM_CR1_CEN | STM32_TIM_CR1_PMEN;

    nvicEnableVector(STM32_TIM2_NUMBER, EFI_IRQ_ANGLE_CLOCK_PRIORITY);

    // "No refusal since boot" sentinel - a zero-initialized branch=0 would
    // print a bogus all-zero refusal line on the first lockstats.
    s_dwellRefusal.branch = 0xFF;

    s_initAcDelta = acDelta;
    s_initNtDelta = ntDelta;
    s_initPsc     = newPsc;

    efiPrintf("angle clock: TMR2 init PSC 71->%lu (measured %lu/%lu ticks)",
        static_cast<unsigned long>(newPsc),
        static_cast<unsigned long>(acDelta),
        static_cast<unsigned long>(ntDelta));
}

// ── Per-tooth state update ────────────────────────────────────────────────
TRIGGER_RAM_CODE void angleClockOnTooth(uint32_t edgeAcTick,
                                         float currentPhase,
                                         float cycleDeg,
                                         float ticksPerDegree) {
    s_edgeAcTick    = edgeAcTick;
    s_currentPhase  = currentPhase;
    s_cycleDeg      = cycleDeg;

    // Keep the last good basis when the feed is NaN/zero (rpm==0 storm flap).
    // Zeroing the basis would collapse every armed tick onto the tooth edge
    // and fire everything immediately at the worst moment.
    if (!(ticksPerDegree > 0.0f)) {
        return;
    }
    if (ticksPerDegree > MAX_TICKS_PER_DEGREE) {
        s_ticksPerDegree = MAX_TICKS_PER_DEGREE;
    } else {
        s_ticksPerDegree = ticksPerDegree;
    }
}

// ── Low-level channel writer ──────────────────────────────────────────────
// Ordered arm sequence, race-proof against the prio-3 ISR preempting mid-arm:
// clear stale CCxIF -> write future CCR -> store shadow -> store action ->
// enable CCxIE -> set armed bit. The ISR never executes a channel with an
// empty action, and the CNT cannot reach the future CCR within this window.
TRIGGER_RAM_CODE static void writeChannel(int ch, uint32_t atTick,
                                          float targetAngle, action_s action) {
    auto& c = s_ch[ch];
    const uint32_t flag = STM32_TIM_SR_CC1IF << ch;
    ANGLE_TIMER->SR    = ~flag;
    *ccrReg(ANGLE_TIMER, ch) = atTick;
    c.ccr         = atTick;
    c.targetAngle = targetAngle;
    c.action      = action;
    ANGLE_TIMER->DIER |= STM32_TIM_DIER_CC1IE << ch;
    s_armedMask |= static_cast<uint8_t>(1u << ch);
}

// ── Arm functions ─────────────────────────────────────────────────────────
// Common angle-domain guard: check the basis and the lead window, return the
// delay in ticks. Returns false when the target is too far ahead (stale
// phase) or the basis is not yet valid.
TRIGGER_RAM_CODE static bool armGuard(float targetAngle,
                                       float& outRemaining,
                                       uint32_t& outDelay) {
    if (s_ticksPerDegree <= 0.0f) return false;
    outRemaining = remainingAngle(targetAngle);
    if (outRemaining > MAX_LEAD_DEG) return false;
    outDelay = angleClockDelayTicks(outRemaining, s_ticksPerDegree);
    return true;
}

// Record the most recent dwell arm refusal for lockstats diagnostics. Runs
// only on the failure path, so the flash-resident body is not a hot-path cost.
static void recordDwellRefusal(uint8_t branch, bool earlyWindow,
                               float targetAngle, float callerPhase, float callerNextPhase,
                               float remaining, uint32_t atTick, uint32_t ccrCnt) {
    s_dwellRefusal.branch = branch;
    s_dwellRefusal.window = earlyWindow ? 0 : 1;
    s_dwellRefusal.targetAngle = targetAngle;
    s_dwellRefusal.callerPhase = callerPhase;
    s_dwellRefusal.callerNextPhase = callerNextPhase;
    s_dwellRefusal.currentPhase = s_currentPhase;
    s_dwellRefusal.cycleDeg = s_cycleDeg;
    s_dwellRefusal.ticksPerDegree = s_ticksPerDegree;
    s_dwellRefusal.remaining = remaining;
    s_dwellRefusal.atTick = atTick;
    s_dwellRefusal.ccrCnt = ccrCnt;
    s_dwellRefusal.lateArmTotal = s_lateArm[ANGLE_CLOCK_CH_DWELL];
}

TRIGGER_RAM_CODE bool angleClockArmDwell(float targetAngle, action_s action,
                                          float callerPhase, float callerNextPhase,
                                          bool earlyWindow) {
    const float remaining = remainingAngle(targetAngle);

    // armGuard, inlined so each refusal is attributed to its exact branch in
    // the snapshot (the shared armGuard() is kept for the open arm).
    if (s_ticksPerDegree <= 0.0f) {
        s_lateArm[ANGLE_CLOCK_CH_DWELL]++;
        s_lateArmDwellGuard++;
        recordDwellRefusal(0, earlyWindow, targetAngle,
                           callerPhase, callerNextPhase, remaining, 0, ANGLE_TIMER->CNT);
        return false;
    }
    if (remaining > MAX_LEAD_DEG) {
        s_lateArm[ANGLE_CLOCK_CH_DWELL]++;
        s_lateArmDwellGuard++;
        recordDwellRefusal(1, earlyWindow, targetAngle,
                           callerPhase, callerNextPhase, remaining, 0, ANGLE_TIMER->CNT);
        return false;
    }

    const uint32_t delay  = angleClockDelayTicks(remaining, s_ticksPerDegree);
    const uint32_t atTick = s_edgeAcTick + delay;
    const uint32_t cnt    = ANGLE_TIMER->CNT;

    if (static_cast<int32_t>(atTick - cnt) <
        static_cast<int32_t>(ARM_MARGIN_TICKS)) {
        s_lateArm[ANGLE_CLOCK_CH_DWELL]++;
        s_lateArmDwellTickPast++;
        recordDwellRefusal(2, earlyWindow, targetAngle,
                           callerPhase, callerNextPhase, remaining, atTick, cnt);
        return false;
    }

    // Busy: a previous dwell is still armed (wasted spark charging two coils
    // at once, or a storm over-arm). Never overwrite a pending event - the
    // caller falls back to TIM5 and the charge-anchored rescue bounds it.
    if (s_ch[ANGLE_CLOCK_CH_DWELL].action) {
        s_lateArm[ANGLE_CLOCK_CH_DWELL]++;
        s_lateArmBusy[ANGLE_CLOCK_CH_DWELL]++;
        recordDwellRefusal(3, earlyWindow, targetAngle,
                           callerPhase, callerNextPhase, remaining, atTick, cnt);
        return false;
    }

    writeChannel(ANGLE_CLOCK_CH_DWELL, atTick, targetAngle, action);
    return true;
}

TRIGGER_RAM_CODE bool angleClockArmInjection(float targetAngle, action_s action,
                                              float callerPhase, float callerNextPhase) {
    UNUSED(callerPhase);
    UNUSED(callerNextPhase);

    float remaining; uint32_t delay;
    if (!armGuard(targetAngle, remaining, delay)) {
        s_lateArm[ANGLE_CLOCK_CH_OPEN]++;
        s_lateArmOpenGuard++;
        return false;
    }

    const uint32_t atTick = s_edgeAcTick + delay;
    const uint32_t cnt    = ANGLE_TIMER->CNT;

    if (static_cast<int32_t>(atTick - cnt) <
        static_cast<int32_t>(ARM_MARGIN_TICKS)) {
        s_lateArm[ANGLE_CLOCK_CH_OPEN]++;
        s_lateArmOpenTickPast++;
        return false;
    }

    if (s_ch[ANGLE_CLOCK_CH_OPEN].action) {
        s_lateArm[ANGLE_CLOCK_CH_OPEN]++;
        s_lateArmBusy[ANGLE_CLOCK_CH_OPEN]++;
        return false;
    }

    writeChannel(ANGLE_CLOCK_CH_OPEN, atTick, targetAngle, action);
    return true;
}

// ── From-now arms (time domain, refresh-skipped via the sentinel) ────────
// nowAc is a TMR2-domain timestamp (angleClockNow() read by the caller in the
// TMR2 ISR that just fired) - the delay is added directly, no NT conversion.
TRIGGER_RAM_CODE static bool armFromNow(int ch, uint32_t nowAc, uint32_t delayNt,
                                        action_s action) {
    auto& c = s_ch[ch];
    if (c.action) {
        s_lateArm[ch]++;
        s_lateArmBusy[ch]++;
        return false;
    }

    const uint32_t atTick = nowAc + delayNt;
    const uint32_t cnt    = ANGLE_TIMER->CNT;

    if (static_cast<int32_t>(atTick - cnt) <
        static_cast<int32_t>(ARM_MARGIN_TICKS)) {
        s_lateArm[ch]++;
        return false;
    }

    writeChannel(ch, atTick, FROM_NOW_SENTINEL, action);
    return true;
}

TRIGGER_RAM_CODE bool angleClockArmSparkFromNow(uint32_t nowAc,
                                                 uint32_t delayNt, action_s action) {
    return armFromNow(ANGLE_CLOCK_CH_SPARK, nowAc, delayNt, action);
}

TRIGGER_RAM_CODE bool angleClockArmInjectionCloseFromNow(uint32_t nowAc,
                                                          uint32_t delayNt, action_s action) {
    return armFromNow(ANGLE_CLOCK_CH_CLOSE, nowAc, delayNt, action);
}

// ── Refresh ───────────────────────────────────────────────────────────────
// Re-anchor one angle-domain channel from the freshest tooth data.
TRIGGER_RAM_CODE static void refreshChannel(int ch) {
    auto& c = s_ch[ch];
    if (!c.action) return;

    const float remaining = remainingAngle(c.targetAngle);
    // Phase basis jumped (desync): leave armed to fire by time, exactly
    // like the FALSE build which fires time-scheduled events regardless of sync.
    if (remaining > MAX_LEAD_DEG) return;

    const uint32_t oldCcr = c.ccr;
    const uint32_t newTick = s_edgeAcTick +
        angleClockDelayTicks(remaining, s_ticksPerDegree);

    // Only re-anchor EARLIER: the armed tick is the best prediction from the
    // arm tooth; a later-tooth basis cannot push it out (that was the pile-up
    // root cause at the catch with the stale ~250 rpm basis).
    const uint32_t anchor = (static_cast<int32_t>(newTick - oldCcr) > 0)
                            ? oldCcr : newTick;

    const uint32_t cnt = static_cast<uint32_t>(ANGLE_TIMER->CNT);

    // Skip-write (2026-09-25): when the anchor equals the already-armed CCR
    // and the event is not yet due, refresh would rewrite the SAME value (and
    // run the race-check) for nothing - in steady rpm the only-earlier anchor
    // gives newTick ~= oldCcr, so this is the common case on every tooth,
    // including rpm==0 flap teeth where the basis is unchanged. Returning here
    // BEFORE the CCR write and SR re-read removes the per-tooth peripheral
    // traffic on armed channels; the TMR ISR just fires the standing CCR.
    if (anchor == oldCcr &&
        static_cast<int32_t>(oldCcr - cnt) >= static_cast<int32_t>(ARM_MARGIN_TICKS)) {
        s_refreshSkip++;
        return;
    }

    const uint32_t flag = STM32_TIM_SR_CC1IF << ch;
    // If CCxIF is pending the ISR is about to fire with the current CCR -
    // don't touch it (re-anchoring would give a ~2^32 late reading).
    if (ANGLE_TIMER->SR & flag) {
        s_refreshPending++;
        return;
    }

    if (static_cast<int32_t>(anchor - cnt) <
        static_cast<int32_t>(ARM_MARGIN_TICKS)) {
        // Due now: arm for immediate ISR fire.
        const uint32_t nowTick = cnt + ARM_MARGIN_TICKS;
        *ccrReg(ANGLE_TIMER, ch) = nowTick;
        // Race: old CCR may have matched in the few cycles between the flag
        // check above and this write.  If so, the pending ISR fires the action
        // with the old CCR - restore it so the lateness measurement is correct.
        if (ANGLE_TIMER->SR & flag) {
            *ccrReg(ANGLE_TIMER, ch) = oldCcr;
        } else {
            c.ccr = nowTick;
            s_immediate++;
        }
        return;
    }

    // Normal re-anchor with deadband (2026-09-26): the only-earlier anchor
    // means a non-zero pull is always oldCcr - anchor (fresh wants it EARLIER).
    // Absorb sub-deadband pulls - per-tooth basis jitter produces a few-ticks
    // gap on nearly every armed channel, and firing at the armed tick leaves
    // the event <deadband (0.08 deg @7000) late vs the freshest basis:
    // harmless for START edges, saves a pointless CCR write + SR re-read per
    // tooth. Real accel/decel corrections exceed the deadband and still re-anchor.
    const uint32_t pull = oldCcr - anchor;   // anchor <= oldCcr (only-earlier)
    if (pull > s_refreshMaxPullTicks) s_refreshMaxPullTicks = pull;
    if (pull <= ANGLE_CLOCK_REANCHOR_DEADBAND_TICKS) {
        s_refreshJitter++;
        return;
    }
    *ccrReg(ANGLE_TIMER, ch) = anchor;
    if (ANGLE_TIMER->SR & flag) {
        *ccrReg(ANGLE_TIMER, ch) = oldCcr;
    } else {
        c.ccr = anchor;
        s_refreshReanchored++;
    }
}

TRIGGER_RAM_CODE void angleClockRefresh() {
    // armedMask early-out: on the ~93% of teeth that arm nothing this is a
    // single bit test (the old design visited 12 channels per tooth).
    if (!(s_armedMask & REFRESH_MASK)) {
        return;
    }
    refreshChannel(ANGLE_CLOCK_CH_DWELL);
    refreshChannel(ANGLE_CLOCK_CH_OPEN);
}

// ── Cancel ────────────────────────────────────────────────────────────────
TRIGGER_RAM_CODE static void cancelChannel(int ch) {
    s_ch[ch].action = {};
    ANGLE_TIMER->DIER &= ~(STM32_TIM_DIER_CC1IE << ch);
    s_armedMask &= static_cast<uint8_t>(~(1u << ch));
}

TRIGGER_RAM_CODE void angleClockCancelDwell()     { cancelChannel(ANGLE_CLOCK_CH_DWELL); }
TRIGGER_RAM_CODE void angleClockCancelSpark()     { cancelChannel(ANGLE_CLOCK_CH_SPARK); }
TRIGGER_RAM_CODE void angleClockCancelInjection() { cancelChannel(ANGLE_CLOCK_CH_OPEN); }
TRIGGER_RAM_CODE void angleClockCancelClose()     { cancelChannel(ANGLE_CLOCK_CH_CLOSE); }

// ── Telemetry accessors ───────────────────────────────────────────────────
uint32_t angleClockFiredCount(int ch)       { return s_fired[ch]; }
uint32_t angleClockLateArmCount(int ch)     { return s_lateArm[ch]; }
uint32_t angleClockBusyCount(int ch)        { return s_lateArmBusy[ch]; }
uint32_t angleClockLateArmDwellGuard()      { return s_lateArmDwellGuard; }
uint32_t angleClockLateArmDwellTickPast()   { return s_lateArmDwellTickPast; }
uint32_t angleClockLateArmOpenGuard()       { return s_lateArmOpenGuard; }
uint32_t angleClockLateArmOpenTickPast()    { return s_lateArmOpenTickPast; }
const DwellArmRefusal& angleClockDwellRefusal() { return s_dwellRefusal; }
uint32_t angleClockMaxLateTicks()    { return s_maxLateTicks; }
uint32_t angleClockImmediateFireCount() { return s_immediate; }
uint32_t angleClockRefreshSkip()      { return s_refreshSkip; }
uint32_t angleClockRefreshReanchored(){ return s_refreshReanchored; }
uint32_t angleClockRefreshJitter()    { return s_refreshJitter; }
uint32_t angleClockRefreshMaxPullTicks() { return s_refreshMaxPullTicks; }
uint32_t angleClockRefreshPending()   { return s_refreshPending; }
uint32_t angleClockLookaheadDwell()   { return s_lookaheadDwell; }
uint32_t angleClockLookaheadOpen()    { return s_lookaheadOpen; }
void angleClockRecordLookaheadDwell() { s_lookaheadDwell++; }
void angleClockRecordLookaheadOpen()  { s_lookaheadOpen++; }
uint32_t angleClockInitAcDelta()     { return s_initAcDelta; }
uint32_t angleClockInitNtDelta()     { return s_initNtDelta; }
uint32_t angleClockInitPsc()         { return s_initPsc; }

void angleClockResetStats() {
    for (int ch = 0; ch < 4; ch++) {
        s_fired[ch] = s_lateArm[ch] = s_lateArmBusy[ch] = 0;
    }
    s_lateArmDwellGuard = s_lateArmDwellTickPast = 0;
    s_lateArmOpenGuard = s_lateArmOpenTickPast = 0;
    s_maxLateTicks = s_immediate = 0;
    s_refreshSkip = s_refreshReanchored = s_refreshJitter = 0;
    s_refreshMaxPullTicks = 0;
    s_refreshPending = 0;
    s_lookaheadDwell = s_lookaheadOpen = 0;
    s_dwellRefusal.branch = 0xFF;  // sentinel: no refusal since last reset
}

// Runtime clock sanity for TMR2. The 2026-09-20 car session showed TIM2->CNT
// advancing ~30 ticks/s instead of 4 MHz, so every angleClockArmDwell
// tick-past check failed. Reads the APB1 clock-enable / sleep-enable bits
// plus CR1/PSC and measures the live counter rate against NT (TIM5) so one
// lockstats dump names the gate that killed the clock.
void angleClockTimerDiag() {
    const uint32_t apb1enr   = RCC->APB1ENR;
    const uint32_t apb1lpenr = RCC->APB1LPENR;
    const uint32_t apb1rstr  = RCC->APB1RSTR;
    const uint32_t cr1       = ANGLE_TIMER->CR1;
    const uint32_t cr2       = ANGLE_TIMER->CR2;
    const uint32_t smcr      = ANGLE_TIMER->SMCR;
    const uint32_t psc       = ANGLE_TIMER->PSC;
    const uint32_t arr       = ANGLE_TIMER->ARR;
    const uint32_t dier      = ANGLE_TIMER->DIER;
    const uint32_t ccer      = ANGLE_TIMER->CCER;

    // Clock tree at runtime: proves the PLL is still locked at 288 MHz (SWS=
    // PLL) and HSE/HEXT is still healthy (HSERDY) - rules out a PLL/HSE
    // fallback to HICK as the TMR2 slowdown cause. PLLCFGR shows the
    // multiplier/divider if a corrupt write changed it (TIM5 = 4 MHz already
    // proves the tree is correct, but this names the register if it drifted).
    const uint32_t rcc_cr   = RCC->CR;
    const uint32_t rcc_cfgr = RCC->CFGR;
    const uint32_t rcc_pll  = RCC->PLLCFGR;

    // Sleep-based measurement: the CPU is in WFI between ISRs, so a timer whose
    // clock is gated in sleep (the 2026-09-20 TMR2 failure) crawls here.
    const uint32_t nt0s = getTimeNowLowerNt();
    const uint32_t t20s = ANGLE_TIMER->CNT;
    chThdSleepMilliseconds(100);
    const uint32_t nt1s = getTimeNowLowerNt();
    const uint32_t t21s = ANGLE_TIMER->CNT;

    // Busy-wait measurement: the CPU never sleeps, so every APB1 timer clock
    // is alive the whole window. Comparing the two tells whether the crawl is
    // a sleep gate (sleep T2d << busy T2d) or an always-on slow clock (both
    // low). A 100 ms spin in the lockstats thread is safe: the WDG window is
    // seconds and ISRs (handoff/TMR2) still preempt.
    const uint32_t nt0b = getTimeNowLowerNt();
    const uint32_t t20b = ANGLE_TIMER->CNT;
    do { } while (getTimeNowLowerNt() - nt0b < US2NT(100000));
    const uint32_t nt1b = getTimeNowLowerNt();
    const uint32_t t21b = ANGLE_TIMER->CNT;

    efiPrintf("tim2clk EN=%u LPEN=%u RST=%u CEN=%u PSC=%u ARR=%08x DIER=%08x CCER=%08x SMCR=%08x CR2=%08x",
        (unsigned)((apb1enr & RCC_APB1ENR_TIM2EN) ? 1 : 0),
        (unsigned)((apb1lpenr & RCC_APB1LPENR_TIM2LPEN) ? 1 : 0),
        (unsigned)((apb1rstr & RCC_APB1RSTR_TIM2RST) ? 1 : 0),
        (unsigned)((cr1 & STM32_TIM_CR1_CEN) ? 1 : 0),
        (unsigned)psc,
        (unsigned)arr,
        (unsigned)dier,
        (unsigned)ccer,
        (unsigned)smcr,
        (unsigned)cr2);
    efiPrintf("tim2clk CR=%08x CFGR=%08x PLLCFGR=%08x (want PLLON HSEON PLLRDY HSERDY, SWS=PLL, PLLMS=1 PLLNS=144 PLLFR=4)",
        (unsigned)rcc_cr,
        (unsigned)rcc_cfgr,
        (unsigned)rcc_pll);
    efiPrintf("tim2clk sleep T2d=%u NTd=%u busy T2d=%u NTd=%u/100ms (want EN=1 LPEN=1 RST=0 CEN=1 PSC~71, both T2d~NTd~400000)",
        (unsigned)(t21s - t20s),
        (unsigned)(nt1s - nt0s),
        (unsigned)(t21b - t20b),
        (unsigned)(nt1b - nt0b));
}

#endif // EFI_ANGLE_CLOCK && EFI_PROD_CODE
