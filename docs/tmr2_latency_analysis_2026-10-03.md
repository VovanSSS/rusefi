# TMR2 latency spikes - full session analysis (m74_9, 2026-10-03 16:10-16:17)

Complete top-down analysis of the 2026-10-03 car session: 8 lockstats
windows, ~850 rpm idle, EFI_ANGLE_CLOCK build (single TMR2, 4 channels by
event type). Combines the measured numbers (full tables in
docs/lockstats_analysis_2026-10-03.md) with the code-level latency budget
for every level of the chain: EXTI (prio 0) -> trigger handoff (prio 4) ->
TMR2 angle clock (prio 3) -> TIM5 executor (prio 5). The user's take and
observations are in docs/tmr2_can_stall_user_take_2026-10-03.md.

## 1. What each metric actually measures (code semantics)

- triggerIsr (trigger_central.cpp:653-663): duration of ONE edge decode,
  from triggerHandlerEntryTime to the end of TriggerCentral::handleShaftSignal.
  INCLUDES everything that preempts the handoff inside (TMR2 fire bodies,
  EXTI captures); EXCLUDES the queue-pop wait and the outer gates
  (boardRawTriggerEdge, tooSoonToHandleSignal, isUsefulSignal).
- trgPreDecode / trgDecode / trgPostDecode (trigger_central.cpp:1120-1123):
  sub-phases of the same edge (t1-t0, t2-t1, t3-t2).
- triggerMaxDuration: max per-edge duration in NT ticks (1 tick = 0.25 us).
- maxTriggerReentrant: max nesting of the handoff (0 all session = no
  re-entrancy).
- angclk xsec late (angle_clock.cpp:229-230): TMR2->CNT - dueCcr measured at
  dispatch() entry, BEFORE the action body - pure ISR-entry delay.
- xsec handoff= (:243-244): fire entered while triggerReentrant > 0 (the
  handoff was running).
- xsec ser= (:245-249): dueCcr <= s_lastHandlerEndTick - the CCR matched
  while the PREVIOUS TMR2 entry was still executing its action bodies
  (same-priority serialization on the single shared vector).
- angclk maxLateUs: max over all channels of the entry delay.
- immediate=: refresh found a past-due event and armed it for immediate fire
  (angle_clock.cpp:591-605).
- maxLockedDuration / maxSchedulingPrecisionLoss / maxEventCallbackDuration:
  lock/precision/callback instrumentation, all 0 all session.

## 2. Priority ladder (verified in the session output and in code)

| prio | vector | role |
| --- | --- | --- |
| 0 | EXTI9_5 | raw crank-edge capture (fast IRQ) |
| 3 | TMR2 | angle-clock fires: dwell CC1 / spark CC2 / open CC3 / close CC4 |
| 4 | I2C1_EV | trigger decode + arming ("handoff") |
| 5 | TIM5 CC1 | executor (fallback events, MAP/knock windows, WDA-unrelated) |
| 6 | TMR7 | L9779 WDA feed (~100 us SPI burst, preemptable by fires) |
| 12 | CAN1 | CAN RX/TX |

nvic line in every window read exactly the want strings; angclk nvic T2=3;
tim2clk exact everywhere (ARR=FFFFFFFF - PMEN 32-bit holds; sleep/busy
T2d~NTd~400000; PSC=71; initRate 40001/40000).

Load-bearing fact from the ChibiOS port (os/common/ports/ARMv7-M/chcore.h:
PORT_IRQ_PROLOGUE() is EMPTY): kernel ISRs run UNLOCKED on ARMv7-M. A
prio-3 fire genuinely PREEMPTS a prio-4 handoff unless the handoff holds an
explicit BASEPRI (CriticalSectionLocker) or PRIMASK (__disable_irq) window.

## 3. Level 0 - EXTI (prio 0)

Body (digital_input_exti.cpp:242-267): extiGetAndClearGroup1 (EXTI PR RMW)
+ palReadLine (:254) + getTimeNowNt() (TIM5->CNT) + angleClockNow()
(TMR2->CNT, :260) + queue.push + NVIC->STIR (:265). Fixed ~2-3 us.

What it can do "in a moment":
- An edge burst (VRS noise storm): N captures back-to-back, each 2-3 us,
  each stealing from whatever was running (including a TMR2 fire) and each
  re-pending the handoff. Vector9C (:289-295) serves lines 5-9 in a loop;
  on m74_9 only the trigger input (F8 = line 8) is EXTI-enabled.
- Queue is 32 entries; push silently drops when full (:161-165).
- The handoff's pop takes __disable_irq around each dequeue (:210-212) - a
  sub-us full-PRIMASK window per edge.

Conclusion: EXTI cannot produce the 60-80 us class by itself; it is a
2-3 us/edge tax and the source of edge bursts.

## 4. Level 1 - trigger handoff (prio 4)

Entry structure: CH_IRQ_HANDLER(STM32_I2C1_EVENT_HANDLER)
(digital_input_exti.cpp:204-236) runs a while(true) loop that decodes ALL
queued edges back-to-back in ONE ISR entry. One entry can therefore span
N x per-edge cost when the queue accumulated N edges (during CS windows,
fire preemption, or a storm burst).

Per-edge chain (trigger_central.cpp:930-1124):
- preDecode (t0->t1): debounce gate (EFI_TRIGGER_DEBOUNCE=FALSE, compiled
  out), shape-error check, noise filter (noiseless decoder OFF on m74_9),
  isToothExpectedNow (EFI_TRIGGER_CHECK_TOOTH_TIMING), hwEventCounters++.
  Measured avg 1.56 us.
- decode (t1->t2): decodeTriggerEvent - gap ratio [1.6,3.75], sync
  validation, phase updates. Measured avg 13.0 us. The rare 100-250 us
  bucket (38 of 894k events) is the printGaps/warning path from decode
  errors (error_handling.cpp:708-740: chvsnprintf with %f runs UNLOCKED, so
  it does not mask fires but stretches the edge).
- postDecode (t2->t3), the dominant phase (avg 43.5 us, 73% of triggerIsr):
  1. CriticalSectionLocker around m_lastToothTimer (trigger_central.cpp:
     1028-1034) - sub-us mask window on EVERY tooth.
  2. rpmShaftPositionCallback (rpm_calculator.cpp:309-413): cycle RPM
     update, revolution-counter rate-limit, updateInstantRpm (binary search
     over the 116 eventAngles + float division).
  3. tdcMarkCallback (:1044) - one executor insert per cycle when the
     sniffer is enabled.
  4. findNextTriggerToothAngle (:1050, :1098) - +2 scan for the early-window
     upper bound.
  5. mainTriggerCallback (main_trigger_callback.cpp:395-532):
     - angleClockOnTooth (:452-455) - basis feed, 2-3 us.
     - scheduleDwellEarlyIfDue (spark_logic.cpp:1098-1166) - 4x per-cylinder
       window scan on EVERY tooth; on a hit runs scheduleSparkEvent:
       angleClockArmDwell + sparkRingPush (__disable_irq, spark_logic.cpp:
       84-94).
     - rpm==0 gate (main_trigger_callback.cpp:462-480) - storm-flap early
       return (with angleClockRefresh still called).
     - scheduleEventsUntilNextTriggerTooth (trigger_scheduler.cpp:97-164):
       CS head-steal (:110-115) + CS re-attach (:157-163) every tooth; the
       angle queue is empty for spark under EFI_ANGLE_CLOCK (only
       VVT/AuxActor), so the flush is ~5 us.
     - handleFuel -> InjectionEvent::onTriggerTooth x4
       (main_trigger_callback.cpp:96-359): cheap early-outs via
       injectionStartArmed/angleClockIsArmed; on a hit: wallFuel.adjust,
       getInjectionDuration x2 (injector model), angleClockArmInjection or
       TIM5 fallback (executor insertTask CS), recordInjectionSchedule
       (sparkRingPush __disable_irq).
     - EFI_TRIGGER_CONFIG_RUNTIME_RELOAD block compiled out on m74_9
       (:494-511).
     - engineModules.apply_all REMOVED (2026-09-23) - no per-tooth module
       work remains.
     - onTriggerEventSparkLogic (spark_logic.cpp:894-1078): limp check,
       getDwell (table interp), 4x loop with dwellStartArmed early-out;
       on a miss: two isPhaseInRange tests + scheduleSparkEvent. NOTE the
       prepareIgnitionSchedule() call at :916-918 when !isReady - a full
       ignition-schedule rebuild IN the handoff; the prime suspect for the
       single ~1.2 ms postDecode outlier in w1.
     - angleClockRefresh (angle_clock.cpp:630-638): armedMask early-out;
       when armed: refreshChannel x2 (SR read, skip-write compare, maybe
       CCR write + SR re-read race check).
  6. decodeMapCam (:1110) - cheap on m74_9 (no MAP cam).
  7. boardTriggerCallback (:1112) - learner compiled out on m74_9.

CS/PRIMASK window inventory inside the handoff (each masks the prio-3
fires for its duration):

| window | where | size |
| --- | --- | --- |
| m_lastToothTimer CS | trigger_central.cpp:1028-1034 | sub-us, every tooth |
| angle-queue head-steal / re-attach CS | trigger_scheduler.cpp:110-115, 157-163 | sub-us, every tooth |
| executor insertTask CS | single_timer_executor.cpp:106-109 | sub-us, on fallback schedules |
| executor queue.remove CS | single_timer_executor.cpp:136-141 | sub-us, on cancels |
| sparkRingPush __disable_irq (full PRIMASK) | spark_logic.cpp:84-94 | ~0.5-1 us, on EVERY dwell/injection schedule and every fire |
| queue-pop __disable_irq | digital_input_exti.cpp:210-212 | ~0.1 us, per edge |

None of these is >=10 us individually. Because the ISR is otherwise
unlocked, a TMR2 fire matching during a handoff PREEMPTS it and its full
action-body time is charged to that edge's triggerIsr/postDecode histogram
(this is how the triggerIsr 100-250 us bucket grows without any print).

## 5. Level 2 - TMR2 angle clock (prio 3)

dispatch() (angle_clock.cpp:197-256):
- Pass 1 reads SR ONCE (:203) and releases every due channel before any
  action executes (:208-220) - a re-arm from inside an action can never hit
  a busy channel (incl. the 100%-duty close/open same-tick corner).
- Pass 2 executes the released bodies in channel order dwell -> spark ->
  open -> close (:225-254).
- late = CNT - dueCcr is measured at entry (:229-230); a channel whose CCR
  matched AFTER the pass-1 SR read pends a SECOND entry that waits for the
  first entry to finish (same priority, one vector) - the ser= class.
- s_lastHandlerEndTick is stamped at handler exit (:267).

The four action bodies (print-free inventory, all verified):

| body | file:line | content | ~cost |
| --- | --- | --- | --- |
| dwell | spark_logic.cpp:557-636 | getTimeNowNt + 4x startDwellByTurningSparkPinHigh (Rpm cache read + bail check + GPIO setHigh) + overdwell executor insert (CS) + angleClockCancelSpark + scheduler.cancel (CS) + angleClockArmSparkFromNow | 10-20 us |
| spark | spark_logic.cpp:380-495 | fireSparkBySettingPinLow + cancel overdwell (CS) + recordSparkFire (ring push, __disable_irq) + getDwell + prepareCylinderIgnitionSchedule (float math) + onSparkFireKnockSense | 10-15 us |
| open | fuel_schedule.cpp:19-57 | getTimeNowNt + output->open + angleClockArmInjectionCloseFromNow or TIM5 fallback (CS) | 5-10 us |
| close | main_trigger_callback.cpp:73-83 | output->close + InjectionEvent::update (one division) | 3-5 us |

A co-due entry of 2-4 bodies therefore runs ~20-50 us; a channel matching
during it waits that remainder (ser= tail, no print needed). The idle
cluster is tight by tune: close(N) = TDC(N)-400 deg and dwell start of the
next firing cylinder sit ~1 deg apart, so co-due entries are common.

Arm refusal branches (angleClockArmDwell :430-478 / angleClockArmInjection
:480-510): guard (basis<=0 or target > MAX_LEAD_DEG 30), tickPast
(atTick - CNT < ARM_MARGIN 4 us), busy (channel still armed). In the
session: dwell guard=0 tickPast=0 (perfect); open tickPast 67/42/36 in
w3/w4/w6 = late decodes during the storm; close busy 1016/1134/117 in
w3/w4/w6 = the previous close still pending when the next open fired
(the injection analog of the double-schedule); every busy/lateArm close
falls back to TIM5 1:1 and the injector still closes (accounting exact:
sched fuel n == open lateArm + close lateArm in every window).

## 6. Level 3 - TIM5 executor (prio 5)

- popDue holds a CS spin of up to US2NT(8) per event
  (single_timer_executor.cpp:77-80) - the only ~8 us-class mask window in
  the executor; everything else (insertTask, remove, timer re-arm) is
  sub-us since the 2026-09-29 pop-under-short-CS refactor.
- schedule() never executes due events inline from ISR context (:110-126),
  so executor callbacks do not run inside the handoff.
- schedulerIsr avg 70-86 us per window (the batch); sched exec mode sits in
  the 4-16 us bucket; late>=10us ~95-100% is a threshold artifact cutting
  that bucket. The 64-256 us exec bucket grows to 8.5-9.7% only in the
  storm windows (w3/w4).

## 7. Measured numbers (condensed; full tables in the companion doc)

Per-window averages (us): triggerIsr 52.1-68.7 (pre ~1.56 / decode
11.5-15.0 / post 37.7-50.8), schedulerIsr 70.3-86.0.

Global maxima: triggerIsr 1247 us (w1, 1 event) else 158-214 us; decode
100-250 us = 38 events of 894k; postDecode >1000 us (w1, 1 event) +
100-250 us = 3877 events (0.43%); schedulerIsr 250-500 us = 7 events;
sched exec maxLateUs 249 (w3); angclk maxLateUs 95 (w3/w4); xsec maxUs
81-95.

Average vs maximum: triggerIsr 59.3 avg vs 1247/214 max = 21x/3.6x;
postDecode 43.5 vs >1000/250 = 23x/5.7x; sched exec ~10 mode vs 249 =
~25x in w3; angclk entry <10 typical vs 95 = ~10x. Large ratios = rare
tails, not distribution shifts.

## 8. The print-free refutation and the xsec three-way split

User-refuted by log absence (durable rule in CLAUDE.md, 2026-10-03): the
whole session has exactly ONE warning (C9009 at 16:12:07, fall+4 = benign
rpm-flap prevSparkName desync). The xsec >=10us counts are hundreds per
window (w3: dwell 327 + spark 475 + open 362 + close 435 = 1599), so
formatted ISR prints mathematically cannot drive the 60-80 us class.

w3 three-way split (angle_clock.cpp:238-251):

| type | n>=10us | handoff= | ser= | unclassified |
| --- | --- | --- | --- | --- |
| dwell | 327 | 6 | 52 | 269 |
| spark | 475 | 14 | 179 | 282 |
| open | 362 | 5 | 39 | 318 |
| close | 435 | 13 | 142 | 280 |

- ser= is fully explained print-free: same-prio serialization behind the
  previous entry's 2-4 co-due action bodies (20-50 us).
- handoff= (small) is explained by the explicit mask windows of section 4
  plus the executor popDue spin - all sub-us..8us, which matches the small
  counts.
- unclassified is the MAJORITY and is NOT pinned by static reading: with
  the verified ladder and the unlocked-ISR fact, a >=10us entry delay with
  no running handoff and no previous-entry overlap requires a >=10us
  BASEPRI/PRIMASK window OUTSIDE the handoff; the in-code inventory of such
  windows is thin (the executor 8 us spin is the largest). This class is
  exactly the "5-10 us baseline that occasionally jumps to 60-80 us" the
  user describes - the jump source needs capture-at-entry data to be named.

## 9. CAN/dash impact (user observation)

Dash frames are sent from a THREAD, not an ISR: CanWrite::PeriodicTask()
every 5 ms -> M74_9BcmCanListener::request() -> sendPeriodic()
(m74_9_can.cpp:396-400, :901+). The console ISO-TP assembly also runs in
thread context. CAN ISRs sit at prio 12 (below all engine timing).

Honest code-level conclusion: a single ~1.2 ms stall cannot kill the 10 ms
0x0189 heartbeat by itself; the measured 60-80 us TMR2 bursts do not mask
the CAN ISR (prio 12 is below, and the bursts are prio 3). For the observed
dash stall (check light, dead gauges) the candidates in code order are:
(a) a sustained storm starving the CanWrite/ISO-TP threads (w3/w4 had the
close-busy storm), (b) CAN bus errors (TEC/TER/bus-off) - NOT captured at
the stall instant, (c) a longer wedge (the frozen-NT-clock class) - NOT
observed this session (maxTriggerReentrant=0, no gap-in-time errors). The
causal chain to CAN is therefore NOT proven; it needs TER/TEC + a
send-interval ring at the stall instant.

## 10. Proven vs open

PROVEN (code + measurements):
- Averages stable; maxima are rare tails (max/avg 3.6-25x).
- The 60-80 us TMR2 entry delays are real (xsec maxUs 81-95) and mostly
  NOT the handoff and NOT previous-body serialization - the unclassified
  majority needs a mask window that static code reading cannot pin.
- Prints cannot be the driver (log absence; durable rule).
- close-busy storm in w3/w4 = injection double-schedule analog; all closes
  still executed via TIM5 1:1; accounting exact.
- Timer/hardware layer clean: nvic, tim2clk, PLL, PSC, initRate all exact.

OPEN:
- The exact masker of the unclassified >=10us class (needs capture-at-entry
  instrumentation).
- The single ~1.2 ms postDecode stall (w1) - prime suspect
  prepareIgnitionSchedule() at spark_logic.cpp:916-918, unconfirmed.
- The CAN stall causal chain (needs TER/TEC + send-interval ring).

## 11. Next steps (instrumentation, in priority order)

1. Capture-at-entry instrumentation in angle_clock dispatch(): for every
   late>=10us fire record {channel, lateUs, immediate-origin flag, PRIMASK,
   BASEPRI, triggerReentrant, dueCcr-vs-stamp delta, current phase,
   ticksPerDegree}; plus per-type action-body max durations around
   act.execute(). One lockstats then names the unclassified majority.
2. Add a counter for prepareIgnitionSchedule() calls from the handoff
   (spark_logic.cpp:916-918) to confirm/refute the w1 1.2 ms suspect.
3. CAN: read TER/TEC + record the last N CanWrite send intervals
   immediately after a dash/console stall; timestamp cross-check the
   check-light event against the trigger histograms.
