/*
 * @file spark_logic.cpp
 *
 * @date Sep 15, 2016
 * @author Andrey Belomutskiy, (c) 2012-2020
 */

#include "pch.h"

#include "spark_logic.h"

#include "utlist.h"
#include "event_queue.h"
#include "angle_clock.h"
#include "trigger_central.h"

#include "knock_logic.h"

#if EFI_ENGINE_CONTROL

#if EFI_UNIT_TEST
extern bool verboseMode;
#endif /* EFI_UNIT_TEST */

#if EFI_PRINTF_FUEL_DETAILS || FUEL_MATH_EXTREME_LOGGING
	extern bool printFuelDebug;
#endif // EFI_PRINTF_FUEL_DETAILS

#if !EFI_UNIT_TEST
// only consumed by the skipped-spark detection in startDwellByTurningSparkPinHigh()
static const char *prevSparkName = nullptr;
#endif // EFI_UNIT_TEST

#if EFI_ANGLE_CLOCK
// ── Spark/injection schedule event ring (m74_9 C9009 double-schedule
// instrumentation) ─────────────────────────────────────────────────────────
// Records, with a kind tag:
//   0 = injection-open schedule (InjectionEvent::onTriggerTooth, handoff)
//   1 = dwell schedule (scheduleSparkEvent, handoff)
//   2 = spark fire (fireSparkAndPrepareNextSchedule, TMR2 ISR or TIM5)
// The dwell/injection entries carry the window kind, the angle offsets, the
// arm basis (tpd) and the phase-basis state, so the counter-fall==1 storm
// printed by the c9009diag block is replayable with 'sparkring', which
// decides between the two candidate root causes:
//   (a) garbage-basis arm: tpd huge (storm-flap oneDegreeUs) -> the dwell is
//       armed far in the future -> fires ~1 cycle late -> the next cycle's
//       current-window match lands right after the fire (counter-fall==1).
//       The fire entries prove it directly: the schedule->fire ntMs gap of a
//       late fire is ~1 cycle, not the normal dwell + arm lead.
//   (b) phase-basis jump: gapCorr stepped / the decoder re-synced between the
//       normal arm and the re-arm, moving the window onto the fixed dwell
//       angle (the fire is on time, the re-arm is a legitimate next-cycle
//       schedule shifted by the re-anchor).
// Writers: the prio-4 handoff (schedules) and the prio-3 TMR2 ISR / prio-5
// executor (fires) - the ring push therefore disables interrupts briefly.
struct SparkScheduleRingEntry {
	uint16_t counter;  // sparkCounter low 16 (inj: the element's own slot counter = 0)
	uint16_t fall;     // signalFallSparkId low 16 (dwell kind only)
	uint16_t dwell;    // target angle tenths of a degree
	uint16_t cur;      // currentPhase tenths
	uint16_t next;     // nextPhase tenths
	int16_t  off;      // target - currentPhase (wrapped), tenths, signed
	uint16_t tpd;      // angle-clock ticks-per-degree basis at this schedule
	int16_t  gapCorr;  // gapAnchorCorrectionDeg tenths (signed)
	uint16_t syncCnt;  // decoder synchronization counter low 16
	uint16_t rpm;
	uint16_t rev;      // revolution counter low 16
	uint16_t ntMs;     // getTimeNowMs() low 16
	uint16_t pw;       // injection PW in 0.1 ms (kind 0 only)
	uint8_t  kind;     // 0 = inj open schedule, 1 = dwell schedule, 2 = spark fire
	uint8_t  coil;     // firing-order slot 0..3
	uint8_t  window;   // 0 = current window, 1 = early window (unused for fire)
};

static SparkScheduleRingEntry sparkScheduleRing[256];
static uint16_t sparkScheduleRingHead = 0;
static uint16_t sparkScheduleRingCount = 0;
// Frozen after the counter-fall==1 anomaly (the C9009 double-schedule): the
// ring only holds ~0.5 s and a later 'sparkring' would see the storm roll out.
// Freezing keeps the pre-storm context + the anomalous schedule as the last
// entry; 'sparkring' unfreezes after the dump.
static bool sparkScheduleRingFrozen = false;

static void sparkRingPush(const SparkScheduleRingEntry& e) {
	if (sparkScheduleRingFrozen) {
		return;
	}
	// Multiple writers (handoff, TMR2 ISR, executor) - make the push atomic.
	__disable_irq();
	sparkScheduleRing[sparkScheduleRingHead] = e;
	sparkScheduleRingHead = (uint16_t)((sparkScheduleRingHead + 1) % efi::size(sparkScheduleRing));
	if (sparkScheduleRingCount < efi::size(sparkScheduleRing)) {
		sparkScheduleRingCount++;
	}
	__enable_irq();
}

TRIGGER_RAM_CODE void recordSparkSchedule(uint8_t coil, uint32_t counter, uint32_t fall,
		bool earlyWindow, float dwellAngle, float currentPhase, float nextPhase,
		float angleOffset) {
	SparkScheduleRingEntry e{};
	e.counter = (uint16_t)counter;
	e.fall = (uint16_t)fall;
	e.dwell = (uint16_t)(dwellAngle * 10.0f);
	e.cur = (uint16_t)(currentPhase * 10.0f);
	e.next = (uint16_t)(nextPhase * 10.0f);
	e.off = (int16_t)(angleOffset * 10.0f);
	uint32_t tpd = angleClockTicksPerDegree();
	e.tpd = (uint16_t)(tpd > 0xFFFF ? 0xFFFF : tpd);
	e.gapCorr = (int16_t)(getTriggerCentral()->gapAnchorCorrectionDeg * 10.0f);
	e.syncCnt = (uint16_t)engine->triggerCentral.triggerState.getSynchronizationCounter();
	e.rpm = (uint16_t)Sensor::getOrZero(SensorType::Rpm);
	e.rev = (uint16_t)getRevolutionCounter();
	e.ntMs = (uint16_t)getTimeNowMs();
	e.kind = 1;
	e.coil = coil;
	e.window = earlyWindow ? 1 : 0;
	sparkRingPush(e);
}

// Injection-open schedule entry: same fields, plus the commanded PW.
// Called from InjectionEvent::onTriggerTooth right after the arm.
TRIGGER_RAM_CODE void recordInjectionSchedule(uint8_t coil, float eventAngle,
		bool earlyWindow, float currentPhase, float nextPhase, float angleFromNow,
		float pwMs) {
	SparkScheduleRingEntry e{};
	e.dwell = (uint16_t)(eventAngle * 10.0f);
	e.cur = (uint16_t)(currentPhase * 10.0f);
	e.next = (uint16_t)(nextPhase * 10.0f);
	e.off = (int16_t)(angleFromNow * 10.0f);
	uint32_t tpd = angleClockTicksPerDegree();
	e.tpd = (uint16_t)(tpd > 0xFFFF ? 0xFFFF : tpd);
	e.gapCorr = (int16_t)(getTriggerCentral()->gapAnchorCorrectionDeg * 10.0f);
	e.syncCnt = (uint16_t)engine->triggerCentral.triggerState.getSynchronizationCounter();
	e.rpm = (uint16_t)Sensor::getOrZero(SensorType::Rpm);
	e.rev = (uint16_t)getRevolutionCounter();
	e.ntMs = (uint16_t)getTimeNowMs();
	e.pw = (uint16_t)(pwMs * 10.0f);
	e.kind = 0;
	e.coil = coil;
	e.window = earlyWindow ? 1 : 0;
	sparkRingPush(e);
}

// Spark fire entry: counter + coil + timestamp only - the schedule->fire
// ntMs gap of a late fire is the direct proof of the (a) mechanism.
TRIGGER_RAM_CODE void recordSparkFire(uint8_t coil, uint32_t counter) {
	SparkScheduleRingEntry e{};
	e.counter = (uint16_t)counter;
	e.rpm = (uint16_t)Sensor::getOrZero(SensorType::Rpm);
	e.rev = (uint16_t)getRevolutionCounter();
	e.ntMs = (uint16_t)getTimeNowMs();
	e.kind = 2;
	e.coil = coil;
	sparkRingPush(e);
}

// Console command 'sparkring [N]': dump the last N entries (newest last).
void printSparkScheduleRing(int count) {
	if (count <= 0 || count > (int)sparkScheduleRingCount) {
		count = sparkScheduleRingCount;
	}
	efiPrintf("sparkring: %d entries, newest last (kind i=inj-open d=dwell f=fire; angles in tenths of deg, tpd=ticks/deg, pw=0.1ms)", count);
	for (int i = 0; i < count; i++) {
		uint16_t idx = (uint16_t)((sparkScheduleRingHead + efi::size(sparkScheduleRing) - count + i) % efi::size(sparkScheduleRing));
		const SparkScheduleRingEntry& e = sparkScheduleRing[idx];
		char kind = e.kind == 0 ? 'i' : (e.kind == 1 ? 'd' : 'f');
		efiPrintf("sparkring #%u %c c=%u f=%u cyl=%u win=%u dwell=%d cur=%d next=%d off=%d tpd=%u gapC=%d sync=%u rpm=%u rev=%u ms=%u pw=%u",
			(unsigned)idx, kind, (unsigned)e.counter, (unsigned)e.fall, (unsigned)e.coil, (unsigned)e.window,
			(int)e.dwell, (int)e.cur, (int)e.next, (int)e.off,
			(unsigned)e.tpd, (int)e.gapCorr, (unsigned)e.syncCnt, (unsigned)e.rpm, (unsigned)e.rev, (unsigned)e.ntMs, (unsigned)e.pw);
	}
	// The dump consumed the frozen storm capture - resume recording.
	sparkScheduleRingFrozen = false;
}
#endif // EFI_ANGLE_CLOCK

static void fireSparkBySettingPinLow(IgnitionEvent *event, IgnitionOutputPin *output) {
#if SPARK_EXTREME_LOGGING
	efiPrintf("[%s] %d spark goes low revolution %d tick %d current value %d",
		event->getOutputForLoggins()->getName(), event->sparkCounter,
		getRevolutionCounter(),  time2print(getTimeNowUs()),
		output->currentLogicValue);
#endif /* SPARK_EXTREME_LOGGING */

	/**
	 * there are two kinds of 'out-of-order'
	 * 1) low goes before high, everything is fine afterwards
	 *
	 * 2) we have an un-matched low followed by legit pairs
	 */
	output->signalFallSparkId = event->sparkCounter;

	if (!output->currentLogicValue && !event->wasSparkLimited && !event->wasSparkCanceled) {
#if SPARK_EXTREME_LOGGING
		efiPrintf("out-of-order coil off %s", output->getName());
#endif /* SPARK_EXTREME_LOGGING */
		warning(ObdCode::CUSTOM_OUT_OF_ORDER_COIL, "out-of-order coil off %s", output->getName());
	}
	output->setLow();
}

static void assertPinAssigned(IgnitionOutputPin* output) {
	if (!output->isInitialized()) {
		warning(ObdCode::CUSTOM_OBD_COIL_PIN_NOT_ASSIGNED, "Pin Not Assigned check configuration #%s", output->getName()); \
	}
}

/**
 * @param cylinderIndex from 0 to cylinderCount, not cylinder number
 */
static int getIgnitionPinForIndex(int cylinderIndex, ignition_mode_e ignitionMode) {
	switch (ignitionMode) {
	case IM_ONE_COIL:
		return 0;
	case IM_WASTED_SPARK: {
		if (engineConfiguration->cylindersCount == 1) {
			// we do not want to divide by zero
			return 0;
		}
		return cylinderIndex % (engineConfiguration->cylindersCount / 2);
	}
	case IM_INDIVIDUAL_COILS:
		return cylinderIndex;
	case IM_TWO_COILS:
		return cylinderIndex % 2;

	default:
		firmwareError(ObdCode::CUSTOM_OBD_IGNITION_MODE, "Invalid ignition mode getIgnitionPinForIndex(): %d", engineConfiguration->ignitionMode);
		return 0;
	}
}

TRIGGER_RAM_CODE static void prepareCylinderIgnitionSchedule(angle_t dwellAngleDuration, floatms_t sparkDwell, IgnitionEvent *event) {
	// todo: clean up this implementation? does not look too nice as is.

	// let's save planned duration so that we can later compare it with reality
	event->sparkDwell = sparkDwell;

	// New angles for the next cycle: the dwell start is no longer armed.
	event->dwellStartArmed = false;

	auto ignitionMode = getCurrentIgnitionMode();

	// On an odd cylinder (or odd fire) wasted spark engine, map outputs as if in sequential.
	// During actual scheduling, the events just get scheduled every 360 deg instead
	// of every 720 deg.
	if (ignitionMode == IM_WASTED_SPARK && engine->engineState.useOddFireWastedSpark) {
		ignitionMode = IM_INDIVIDUAL_COILS;
	}

	const int index = getIgnitionPinForIndex(event->cylinderIndex, ignitionMode);
	const int coilIndex = getCylinderNumberAtIndex(index);
	angle_t finalIgnitionTiming =	getEngineState()->timingAdvance[coilIndex];
	// Stash which cylinder we're scheduling so that knock sensing knows which
	// cylinder just fired
	event->coilIndex = coilIndex;

	// 10 ATDC ends up as 710, convert it to -10 so we can log and clamp correctly
	if (finalIgnitionTiming > 360) {
		finalIgnitionTiming -= 720;
	}

	// Clamp the final ignition timing to the configured limits
	// finalIgnitionTiming is deg BTDC
	// minimumIgnitionTiming limits maximum retard
	// maximumIgnitionTiming limits maximum advance
	/*
	https://github.com/rusefi/rusefi/issues/5894 disabling feature for now
	finalIgnitionTiming = clampF(engineConfiguration->minimumIgnitionTiming, finalIgnitionTiming, engineConfiguration->maximumIgnitionTiming);
	*/

    engine->outputChannels.ignitionAdvanceCyl[event->cylinderIndex] = finalIgnitionTiming;

	angle_t sparkAngle =
		// Negate because timing *before* TDC, and we schedule *after* TDC
		- finalIgnitionTiming
		// Offset by this cylinder's position in the cycle
		+ getPerCylinderFiringOrderOffset(event->cylinderIndex, coilIndex);

	efiAssertVoid(ObdCode::CUSTOM_SPARK_ANGLE_1, !std::isnan(sparkAngle), "sparkAngle#1");
	wrapAngle(sparkAngle, "findAngle#2", ObdCode::CUSTOM_ERR_6550);
	event->sparkAngle = sparkAngle;

	engine->outputChannels.currentIgnitionMode = static_cast<uint8_t>(ignitionMode);

	IgnitionOutputPin *output = &enginePins.coils[coilIndex];
	event->outputs[0] = output;
	IgnitionOutputPin *secondOutput;

	// We need two outputs if:
	//  - we are running wasted spark, and have "two wire" mode enabled
	//  - We are running sequential mode, but we're cranking, so we should run in two wire wasted mode (not one wire wasted)
	bool isTwoWireWasted = engineConfiguration->twoWireBatchIgnition || (engineConfiguration->ignitionMode == IM_INDIVIDUAL_COILS);
	if (ignitionMode == IM_WASTED_SPARK && isTwoWireWasted) {
		int secondIndex = index + engineConfiguration->cylindersCount / 2;
		int secondCoilIndex = getCylinderNumberAtIndex(secondIndex);
		secondOutput = &enginePins.coils[secondCoilIndex];
		assertPinAssigned(secondOutput);
	} else {
		secondOutput = nullptr;
	}

	assertPinAssigned(output);

	event->outputs[1] = secondOutput;


	angle_t dwellStartAngle = sparkAngle - dwellAngleDuration;
	efiAssertVoid(ObdCode::CUSTOM_ERR_6590, !std::isnan(dwellStartAngle), "findAngle#5");

	assertAngleRange(dwellStartAngle, "findAngle dwellStartAngle", ObdCode::CUSTOM_ERR_6550);
	wrapAngle(dwellStartAngle, "findAngle#7", ObdCode::CUSTOM_ERR_6550);
	event->dwellAngle = dwellStartAngle;

#if FUEL_MATH_EXTREME_LOGGING
	if (printFuelDebug) {
		efiPrintf("addIgnitionEvent %s angle=%.1f", output->getName(), dwellStartAngle);
	}
	//	efiPrintf("addIgnitionEvent %s ind=%d", output->name, event->dwellPosition->eventIndex);
#endif /* FUEL_MATH_EXTREME_LOGGING */
}

static void chargeTrailingSpark(IgnitionOutputPin* pin) {
#if SPARK_EXTREME_LOGGING
	efiPrintf("chargeTrailingSpark %s", pin->getName());
#endif /* SPARK_EXTREME_LOGGING */
	pin->setHigh();
}

static void fireTrailingSpark(IgnitionOutputPin* pin) {
#if SPARK_EXTREME_LOGGING
	efiPrintf("fireTrailingSpark %s", pin->getName());
#endif /* SPARK_EXTREME_LOGGING */
	pin->setLow();
}

TRIGGER_RAM_CODE void overFireSparkAndPrepareNextSchedule(IgnitionEvent *event) {
#if EFI_ANGLE_CLOCK
	// Redundant-rescue idempotency: the TMR2-armed fire already discharged
	// this charge. Acting again would double-fire the coil and double-run
	// prepareCylinderIgnitionSchedule (the single-writer contract of the
	// next cycle's dwell) - the fire and the rescue are two writers of the
	// same coil-off and only the first may act.
	if (event->sparkFiredSinceCharge) {
		return;
	}
#endif // EFI_ANGLE_CLOCK
#if SPARK_EXTREME_LOGGING
	efiPrintf("[%s] %d %s",
		event->getOutputForLoggins()->getName(), event->sparkCounter,
		__func__);
#endif /* SPARK_EXTREME_LOGGING */
	float actualDwellMs = event->actualDwellTimer.getElapsedSeconds() * 1e3;

	warning((ObdCode)((int)ObdCode::CUSTOM_Ignition_Coil_Overcharge_1 + event->cylinderIndex),
		"cylinder %d %s overcharge %f ms",
		event->cylinderIndex + 1, event->outputs[0]->getName(), actualDwellMs);

	// kill pending fire
	engine->module<TriggerScheduler>()->cancel(&event->sparkEvent);

#if EFI_ANGLE_CLOCK
	// Also kill the TMR2-armed from-now spark (the channel holds the fire this
	// rescue is about to replace) and a time-based fallback fire (armed when
	// the TMR2 arm failed): either would fire after this rescue and double-fire
	// an already-low coil (the bench C9012 out-of-order coil off).
	angleClockCancelSpark();
	engine->scheduler.cancel(&event->sparkEvent.eventScheduling);
#endif // EFI_ANGLE_CLOCK

	engine->engineState.overDwellCanceledCounter++;
	event->wasSparkCanceled = true;
	fireSparkAndPrepareNextSchedule(event);
}

/**
 * TL,DR: each IgnitionEvent is in charge of it's own scheduling forever, we plant next event while finishing handling of the current one
 */
TRIGGER_RAM_CODE void fireSparkAndPrepareNextSchedule(IgnitionEvent *event) {
#if EFI_ANGLE_CLOCK
	// The coil was already discharged for this charge (the overdwell rescue
	// fired first, or a redundant late fire arrived): do nothing - firing
	// again trips the out-of-order coil-off warning and a second
	// prepareCylinderIgnitionSchedule would double-arm the next cycle's
	// dwell (single-writer contract).
	// Also handles the ordering-bug no-op: at high rpm the TIM5 dwell fires
	// AFTER the TMR4 spark CCR, so sparkFiredSinceCharge=true (from the
	// previous cycle) makes this call a no-op on an uncharged coil. We record
	// that the no-op happened so turnSparkPinHighStartCharging can re-arm the
	// spark when the dwell finally starts (C9353 fix).
	if (event->sparkFiredSinceCharge) {
		// Coil not charged yet (angle-queue TMR4 arm fired before TMR2 dwell
		// during rapid acceleration). The dwell ISR will arm the spark via
		// angleClockArmSparkFromNow when the coil is actually charged.
		return;
	}
#endif // EFI_ANGLE_CLOCK

#if EFI_UNIT_TEST
	if (engine->onIgnitionEvent) {
		engine->onIgnitionEvent(event, false);
	}
#endif

	for (int i = 0; i< MAX_OUTPUTS_FOR_IGNITION;i++) {
		IgnitionOutputPin *output = event->outputs[i];

		if (output) {
			fireSparkBySettingPinLow(event, output);
		}
	}

#if EFI_ANGLE_CLOCK
	// Cancel the charge-anchored overdwell rescue: the fire just happened, the
	// rescue is a redundant writer (also guarded by sparkFiredSinceCharge) and
	// the multispark branch below reuses dwellStartTimer for the restrike.
	engine->scheduler.cancel(&event->dwellStartTimer);

	// The coil is discharged: a later overdwell rescue for this charge must
	// no-op (see overFireSparkAndPrepareNextSchedule).
	event->sparkFiredSinceCharge = true;

	// Spark schedule ring ('sparkring'): the fire entries prove the late-fire
	// mechanism directly - a schedule->fire ntMs gap of ~1 cycle (vs the normal
	// dwell + arm lead) names the garbage-basis arm as the storm root cause.
	recordSparkFire((uint8_t)event->cylinderIndex, event->sparkCounter);
#endif // EFI_ANGLE_CLOCK

	efitick_t nowNt = getTimeNowNt();

#if EFI_TOOTH_LOGGER
	LogTriggerCoilState(nowNt, event->coilIndex, false);
#endif // EFI_TOOTH_LOGGER
	if (!event->wasSparkLimited) {
		/**
		 * ratio of desired dwell duration to actual dwell duration gives us some idea of how good is input trigger jitter
		 */
		float actualDwellMs = event->actualDwellTimer.getElapsedSeconds(nowNt) * 1e3;
		float ratio = actualDwellMs / event->sparkDwell;

		if (ratio > 1.2) {
			engine->engineState.dwellOverChargeCounter++;
		} else if (ratio < 0.8) {
			engine->engineState.dwellUnderChargeCounter++;
		}
		engine->engineState.dwellActualRatio = ratio;
	}

	// now that we've just fired a coil let's prepare the new schedule for the next engine revolution

	angle_t dwellAngleDuration = engine->ignitionState.dwellDurationAngle;
	floatms_t sparkDwell = engine->ignitionState.getDwell();
	if (std::isnan(dwellAngleDuration) || std::isnan(sparkDwell)) {
		// we are here if engine has just stopped
		return;
	}

	// If there are more sparks to fire, schedule them
	if (event->sparksRemaining > 0) {
		event->sparksRemaining--;

		// each restrike is its own spark: grab a fresh id, otherwise the out-of-order
		// protection in startDwellByTurningSparkPinHigh bails restrike dwell since
		// signalFallSparkId already matches this event's sparkCounter
		event->sparkCounter = engine->engineState.globalSparkCounter++;

		efitick_t nextDwellStart = nowNt + engine->engineState.multispark.delay;
		efitick_t nextFiring = nextDwellStart + engine->engineState.multispark.dwell;
#if SPARK_EXTREME_LOGGING
		efiPrintf("schedule multispark");
#endif /* SPARK_EXTREME_LOGGING */

		// We can schedule both of these right away, since we're going for "asap" not "particular angle"
		engine->scheduler.schedule("dwell", &event->dwellStartTimer, nextDwellStart, action_s::make<turnSparkPinHighStartCharging>( event ));
		engine->scheduler.schedule("firing", &event->sparkEvent.eventScheduling, nextFiring, action_s::make<fireSparkAndPrepareNextSchedule>( event ));
	} else {
		if (engineConfiguration->enableTrailingSparks) {
#if SPARK_EXTREME_LOGGING
			efiPrintf("scheduleByAngle TrailingSparks");
#endif /* SPARK_EXTREME_LOGGING */

			// Trailing sparks are enabled - schedule an event for the corresponding trailing coil
			scheduleByAngle(
				&event->trailingSparkFire, nowNt, engine->ignitionState.trailingSparkAngle,
				action_s::make<fireTrailingSpark>( &enginePins.trailingCoils[event->coilIndex] )
			);
		}

		// If all events have been scheduled, prepare for next time.
		prepareCylinderIgnitionSchedule(dwellAngleDuration, sparkDwell, event);
	}

	engine->onSparkFireKnockSense(event->cylinderIndex, nowNt);
}

static bool startDwellByTurningSparkPinHigh(IgnitionEvent *event, IgnitionOutputPin *output) {
	// Out-of-order bail (spark already fired for this counter) computed BEFORE the
	// warning so the warning can report which case it is: bail=yes = the safe path
	// (dwell refused, coil keeps its charge until the spark), bail=no + charged=1 =
	// the coil is still charging from the previous dwell and this one started on
	// top of it - the dangerous double-charge case worth chasing.
	bool bail = output->signalFallSparkId >= event->sparkCounter;

	// todo: no reason for this to be disabled in unit_test mode?!
#if ! EFI_UNIT_TEST

	if (Sensor::getOrZero(SensorType::Rpm) > 2 * engineConfiguration->cranking.rpm) {
		const char *outputName = output->getName();
		if (prevSparkName == outputName && getCurrentIgnitionMode() != IM_ONE_COIL) {
			warning(ObdCode::CUSTOM_OBD_SKIPPED_SPARK,
				"looks like skipped spark event revolution=%d [%s] rpm=%d charged=%d bail=%s fall=%u counter=%u",
				getRevolutionCounter(), outputName,
				(int)Sensor::getOrZero(SensorType::Rpm),
				output->currentLogicValue ? 1 : 0,
				bail ? "yes" : "no",
				(unsigned)output->signalFallSparkId,
				(unsigned)event->sparkCounter);
		}
		prevSparkName = outputName;
	}
#endif /* EFI_UNIT_TEST */


#if SPARK_EXTREME_LOGGING
	efiPrintf("[%s] %d spark goes high revolution %d tick %d current value %d",
		event->getOutputForLoggins()->getName(), event->sparkCounter,
		getRevolutionCounter(), time2print(getTimeNowUs()),
		output->currentLogicValue, event->sparkCounter);
#endif /* SPARK_EXTREME_LOGGING */

	// Reset error flag(s)
	event->wasSparkCanceled = false;

	if (bail) {
	  /**
	   * fact: we schedule both start of dwell and spark firing using a combination of time and trigger event domain
	   * in case of bad/noisy signal we can get unexpected trigger events and a small time delay for spark firing before
	   * we even start dwell if it scheduled with a longer time-only delay with fewer trigger events
	   *
	   * here we are detecting such out-of-order processing and choose the safer route of not even starting dwell
	   * [tag] #6349
	   */

#if SPARK_EXTREME_LOGGING
		efiPrintf("[%s] bail spark dwell\n", output->getName());
#endif /* SPARK_EXTREME_LOGGING */
		// let's save this coil if things do not look right
		engine->engineState.sparkOutOfOrderCounter++;
		return true;
	}

	output->setHigh();
	return false;
}

TRIGGER_RAM_CODE void turnSparkPinHighStartCharging(IgnitionEvent *event) {
	efitick_t nowNt = getTimeNowNt();

	event->actualDwellTimer.reset(nowNt);

  bool skippedDwellDueToTriggerNoised = false;
	for (int i = 0; i< MAX_OUTPUTS_FOR_IGNITION;i++) {
		IgnitionOutputPin *output = event->outputs[i];
		if (output != NULL) {
		  // at the moment we have a funny xor as if outputs could have different destiny. That's probably an over exaggeration,
		  // realistically it should be enough to check the sequencing of only the first output but that would be less elegant
		  //
		  // maybe it would have need nicer if instead of an array of outputs we had a linked list of outputs? but that's just daydreaming.
			skippedDwellDueToTriggerNoised |= startDwellByTurningSparkPinHigh(event, output);
		}
	}

#if EFI_UNIT_TEST
	engine->incrementBailedOnDwellCount();
#endif


  if (!skippedDwellDueToTriggerNoised) {

#if EFI_ANGLE_CLOCK
	// New charge: clear the spark-fired-since-charge flag.
	event->sparkFiredSinceCharge = false;

	// Anchor overdwell rescue at ACTUAL charge moment (2.5x planned dwell).
	if (event->sparksRemaining == 0) {
		efitick_t fireTime = sumTickAndFloat(nowNt, MSF2NT(2.5f * event->sparkDwell));
		engine->scheduler.schedule("overdwell", &event->dwellStartTimer, fireTime, action_s::make<overFireSparkAndPrepareNextSchedule>( event ));
	}

	// Arm the spark from the ACTUAL charge moment on TMR2 (or TIM5 fallback).
	// Root cause of wasNoOp cycles: during rapid acceleration the spark was
	// armed at tooth T+k with a high-rpm basis while the dwell CCR was set at
	// tooth T-1 with a low-rpm basis. TMR2 fired first (coil uncharged) ->
	// no-op -> dwell fired -> TIM5 re-arm. The fix: always arm the spark HERE
	// (after TMR2 has fired the dwell), with delay = sparkDwell from now.
	// This guarantees the coil is always charged before the spark fires.
	// The anchor is TMR2's OWN counter (angleClockNow() - the same counter
	// this dwell ISR just fired from, no TIM5 read); the TIM5 fallback below
	// stays NT-based by design (it is the fallback, not the primary path).
	// Cancel any stale spark arm first (it may carry the wrong basis);
	// if it already fired as a no-op the channel is already free.
	angleClockCancelSpark();
	engine->scheduler.cancel(&event->sparkEvent.eventScheduling);
	{
		const uint32_t delayNt = static_cast<uint32_t>(MSF2NT(event->sparkDwell));
		const efitick_t sparkTime = sumTickAndFloat(nowNt, static_cast<float>(delayNt));
		if (!angleClockArmSparkFromNow(angleClockNow(), delayNt,
										  action_s::make<fireSparkAndPrepareNextSchedule>(event))) {
			engine->scheduler.schedule("spark", &event->sparkEvent.eventScheduling,
									   sparkTime, action_s::make<fireSparkAndPrepareNextSchedule>(event));
		}
	}
#endif // EFI_ANGLE_CLOCK

#if EFI_UNIT_TEST
  	if (engine->onIgnitionEvent) {
  		engine->onIgnitionEvent(event, true);
  	}
#endif

#if EFI_TOOTH_LOGGER
		LogTriggerCoilState(nowNt, event->coilIndex, true);
#endif // EFI_TOOTH_LOGGER
  }


	if (engineConfiguration->enableTrailingSparks) {
		IgnitionOutputPin *output = &enginePins.trailingCoils[event->coilIndex];
		// Trailing sparks are enabled - schedule an event for the corresponding trailing coil
		scheduleByAngle(
			&event->trailingSparkCharge, nowNt, engine->ignitionState.trailingSparkAngle,
			action_s::make<chargeTrailingSpark>( output )
		);
	}
}


TRIGGER_RAM_CODE void scheduleSparkEvent(bool limitedSpark, IgnitionEvent *event,
		float rpm, float dwellMs, float dwellAngle, float sparkAngle, efitick_t edgeTimestamp, float currentPhase, float nextPhase, bool earlyWindow) {
	UNUSED(rpm);
#if EFI_ANGLE_CLOCK
	// The overdwell rescue is anchored at the actual charge moment in the
	// angle-clock build, not at schedule time - dwellMs is only used here by
	// the time-based build's schedule-time rescue anchor.
	UNUSED(dwellMs);
#else
	UNUSED(earlyWindow);
#endif // EFI_ANGLE_CLOCK

	float angleOffset = dwellAngle - currentPhase;
	if (angleOffset < 0) {
		angleOffset += engine->engineState.engineCycle;
	}

	// For single-tooth triggers (currentPhase == nextPhase), all dwell angles map to the
	// same trigger tooth. When the dwell angle is just below the current phase, the offset
	// wraps to nearly a full engine cycle, scheduling the dwell far in the future
	// Clamp to 0 so the spark starts immediately.
	if (currentPhase == nextPhase && angleOffset > engine->engineState.engineCycle / 2) {
		angleOffset = 0;
#if SPARK_EXTREME_LOGGING
	    efiPrintf("Clamping spark dwell to current phase due to single-tooth trigger");
#endif /* SPARK_EXTREME_LOGGING */
	}

	/**
	 * By the way 32-bit value should hold at least 400 hours of events at 6K RPM x 12 events per revolution
	 * [tag:duration_limit]
	 */
	event->sparkCounter = engine->engineState.globalSparkCounter++;
	event->wasSparkLimited = limitedSpark;

#if !EFI_UNIT_TEST
	// C9009 root-cause instrumentation (m74_9): detect the anomalous re-schedule
	// of the SAME coil one global slot after its own fire (counter - fall == 1,
	// vs the normal counter - fall == cylinderCount). This is the scheduling-side
	// twin of the dwell-start C9009 warning in startDwellByTurningSparkPinHigh;
	// unlike that warning (which runs in the dwell ISR with no phase context),
	// this logs at SCHEDULE time where the angle/phase relationship that caused
	// the double-schedule is still visible. Multispark is disabled on m74_9, so
	// a counter - fall == 1 here is always the anomaly, never a restrike.
	{
		IgnitionOutputPin *outputForDiag = event->outputs[0];
		if (outputForDiag) {
			uint32_t fall = (uint32_t)outputForDiag->signalFallSparkId;
#if EFI_ANGLE_CLOCK
			// Record FIRST so the anomalous schedule itself becomes the last ring
			// entry when the freeze below fires - 'sparkring' then shows the
			// pre-storm context + the anomaly.
			recordSparkSchedule((uint8_t)event->cylinderIndex, event->sparkCounter, fall,
				earlyWindow, dwellAngle, currentPhase, nextPhase, angleOffset);
#endif // EFI_ANGLE_CLOCK
			// fall == 0 is the never-fired sentinel: the very first schedule of
		// each coil (counter 1..4) would otherwise read as counter - fall == 1
		// at startup (observed: Coil 1 counter=1 fall=0 rev=0). Skip it.
			if (fall != 0 && event->sparkCounter == fall + 1) {
				// Print in tenths-of-a-degree integers: %f formatting (chprintf) is
				// slow (~100 us per conversion) and this runs in the handoff ISR -
				// 6x %f here would itself be a ~600 us stall (the exact 'затык'
				// class that feeds inj lateArm). Integer scaling is ~1 cycle.
				efiPrintf("c9009diag coil=%s counter=%u fall=%u dwell=%d spark=%d cur=%d next=%d off=%d early=%d rpm=%d rev=%u gapCorr=%d",
					outputForDiag->getName(),
					(unsigned)event->sparkCounter,
					(unsigned)fall,
					(int)(dwellAngle * 10.0f), (int)(sparkAngle * 10.0f),
					(int)(currentPhase * 10.0f), (int)(nextPhase * 10.0f), (int)(angleOffset * 10.0f),
					earlyWindow ? 1 : 0,
					(int)Sensor::getOrZero(SensorType::Rpm),
					(unsigned)getRevolutionCounter(),
					(int)(getTriggerCentral()->gapAnchorCorrectionDeg * 10.0f));
#if EFI_ANGLE_CLOCK
				// Freeze the ring at the anomaly: a later 'sparkring' then shows the
				// pre-storm entries (the ring only holds ~0.5 s and the storm data
				// rolls out within seconds). 'sparkring' unfreezes after the dump.
				sparkScheduleRingFrozen = true;
#endif // EFI_ANGLE_CLOCK
			}
		}
	}
#endif // !EFI_UNIT_TEST

	// This cycle's dwell is now scheduled: the current-tooth window of the
	// NEXT tooth must not schedule it a second time. Cleared in
	// prepareCylinderIgnitionSchedule at spark fire.
	event->dwellStartArmed = true;

	efitick_t chargeTime = 0;

	/**
	 * The start of charge is always within the current trigger event range, so just plain time-based scheduling
	 */
	if (!limitedSpark) {
#if SPARK_EXTREME_LOGGING
		efiPrintf("[%s] %d sparkUp scheduling revolution %d angle %.1f (+%.1f) later",
			event->getOutputForLoggins()->getName(), event->sparkCounter,
			getRevolutionCounter(), dwellAngle, angleOffset);
#endif /* SPARK_EXTREME_LOGGING */

		/**
		 * Note how we do not check if spark is limited or not while scheduling 'spark down'
		 * This way we make sure that coil dwell started while spark was enabled would fire and not burn
		 * the coil.
		 */
		// The dwell start (coil charge) is armed in the ANGLE domain with the
		// freshest tooth data; the per-tooth refresh keeps the tick accurate
		// until it fires at any rpm. chargeTime anchors the overdwell rescue
		// and the TIM5 fallback - on the fresh basis in the angle-clock build,
		// on the 90-degree average otherwise (bit-identical to the proven
		// time-based build).
#if EFI_ANGLE_CLOCK
		// The dwell start (coil charge) is armed in the ANGLE domain with the
		// 90-degree-window rpm average (oneDegreeUs) - the SAME basis the
		// proven time-based path converts angles with (see
		// mainTriggerCallback's angleClockOnTooth feed). The per-tooth basis
		// was rejected: at cranking a stretched compression tooth armed the
		// charge late and the fire late, and the charge-anchored rescue
		// discharged first (the C935x cluster). chargeTime anchors the TIM5
		// fallback and is basis-consistent with the FALSE build's
		// oneDegreeUs-based chargeTime.
		chargeTime = sumTickAndFloat(edgeTimestamp, angleOffset * US2NT(engine->rpmCalculator.oneDegreeUs));
#else
		float delayUs = engine->rpmCalculator.oneDegreeUs * angleOffset;
		chargeTime = sumTickAndFloat(edgeTimestamp, USF2NT(delayUs));
#endif // EFI_ANGLE_CLOCK

#if EFI_ANGLE_CLOCK
		if (!angleClockArmDwell(dwellAngle, action_s::make<turnSparkPinHighStartCharging>( event ), currentPhase, nextPhase, earlyWindow))
#endif // EFI_ANGLE_CLOCK
		{
			engine->scheduler.schedule("dwell", &event->dwellStartTimer, chargeTime, action_s::make<turnSparkPinHighStartCharging>( event ));
		}

#if EFI_UNIT_TEST
		engine->onScheduleTurnSparkPinHighStartCharging(*event, edgeTimestamp, angleOffset, chargeTime);
#endif

#if SPARK_EXTREME_LOGGING
		efitimeus_t chargeTimeUs = NT2US(chargeTime);
		efiPrintf("[%s] %d sparkUp scheduled at %d ticks (%d.%06d)",
			event->getOutputForLoggins()->getName(), event->sparkCounter,
			time2print(chargeTime), time2print(chargeTimeUs / (1000 * 1000)), time2print(chargeTimeUs % (1000 * 1000)));
#endif /* SPARK_EXTREME_LOGGING */

		event->sparksRemaining = engine->engineState.multispark.count;
	} else {
		// don't fire multispark if spark is cut completely!
		event->sparksRemaining = 0;

#if SPARK_EXTREME_LOGGING
		efiPrintf("[%s] %d sparkUp NOT scheduled because of limitedSpark",
			event->getOutputForLoggins()->getName(), event->sparkCounter);
#endif /* SPARK_EXTREME_LOGGING */
	}

	/**
	 * Spark event is often happening during a later trigger event timeframe
	 */

	efiAssertVoid(ObdCode::CUSTOM_ERR_6591, !std::isnan(sparkAngle), "findAngle#4");
	assertAngleRange(sparkAngle, "findAngle#a5", ObdCode::CUSTOM_ERR_6549);

#if SPARK_EXTREME_LOGGING
	efiPrintf("[%s] %d sparkDown scheduling revolution %d angle %.1f",
		event->getOutputForLoggins()->getName(), event->sparkCounter,
		getRevolutionCounter(), sparkAngle);
#endif /* FUEL_MATH_EXTREME_LOGGING */

	#if !EFI_ANGLE_CLOCK
		// FALSE build: the spark is queued in the angle queue and fired when the
		// tooth arrives. An overdwell rescue protects the coil if the tooth never
		// comes.
		bool isTimeScheduled = engine->module<TriggerScheduler>()->scheduleOrQueue(
			"spark",
			&event->sparkEvent, edgeTimestamp, sparkAngle,
			action_s::make<fireSparkAndPrepareNextSchedule>( event ),
			currentPhase, nextPhase);

		if (!isTimeScheduled) {
			if (!limitedSpark) {
				efitick_t fireTime = sumTickAndFloat(chargeTime, MSF2NT(1.5f * dwellMs));
				// [tag:overdwell]
				engine->scheduler.schedule("overdwell", &event->sparkEvent.eventScheduling, fireTime, action_s::make<overFireSparkAndPrepareNextSchedule>( event ));
	#if EFI_UNIT_TEST
				engine->onScheduleOverFireSparkAndPrepareNextSchedule(*event, fireTime);
	#endif
			} else {
				engine->engineState.overDwellNotScheduledCounter++;
			}
		}
	#else
		// Angle-clock build: spark is armed from turnSparkPinHighStartCharging
		// (angleClockArmSparkFromNow) AFTER the coil is charged. The angle queue
		// is NOT used for spark - no scheduleOrQueue, no overdwell pre-schedule.
		// The overdwell rescue is anchored at the actual charge moment at 2.5x dwell.
		if (limitedSpark) {
			engine->engineState.overDwellNotScheduledCounter++;
		}
	#endif // !EFI_ANGLE_CLOCK

#if EFI_UNIT_TEST
	if (verboseMode) {
		efiPrintf("spark dwell@ %.1f spark@ %.2f id=%d sparkCounter=%d", event->dwellAngle,
			event->sparkEvent.getAngle(),
			event->coilIndex,
			event->sparkCounter);
	}
#endif
}

void initializeIgnitionActions() {
	IgnitionEventList *list = &engine->ignitionEvents;
	angle_t dwellAngle = engine->ignitionState.dwellDurationAngle;
	floatms_t sparkDwell = engine->ignitionState.getDwell();
	if (std::isnan(engine->engineState.timingAdvance[0]) || std::isnan(dwellAngle)) {
		// error should already be reported
		// need to invalidate previous ignition schedule
		list->isReady = false;
		return;
	}
	efiAssertVoid(ObdCode::CUSTOM_ERR_6592, engineConfiguration->cylindersCount > 0, "cylindersCount");

	for (size_t cylinderIndex = 0; cylinderIndex < engineConfiguration->cylindersCount; cylinderIndex++) {
		list->elements[cylinderIndex].cylinderIndex = cylinderIndex;
		prepareCylinderIgnitionSchedule(dwellAngle, sparkDwell, &list->elements[cylinderIndex]);
	}
	list->isReady = true;
}

static void prepareIgnitionSchedule() {
	ScopePerf perf(PE::PrepareIgnitionSchedule);

	operation_mode_e operationMode = getEngineRotationState()->getOperationMode();
	float maxAllowedDwellAngle;

	if (getCurrentIgnitionMode() == IM_ONE_COIL) {
		maxAllowedDwellAngle = getEngineCycle(operationMode) / engineConfiguration->cylindersCount / 1.1;
	} else {
	  maxAllowedDwellAngle = (int) (getEngineCycle(operationMode) / 2); // the cast is about making Coverity happy
	}

	if (engine->ignitionState.dwellDurationAngle == 0) {
		warning(ObdCode::CUSTOM_ZERO_DWELL, "dwell is zero?");
	}
	if (engine->ignitionState.dwellDurationAngle > maxAllowedDwellAngle) {
		warning(ObdCode::CUSTOM_DWELL_TOO_LONG, "dwell angle too long: %.2f", engine->ignitionState.dwellDurationAngle);
	}

	// todo: add some check for dwell overflow? like 4 times 6 ms while engine cycle is less then that

	initializeIgnitionActions();
}

TRIGGER_RAM_CODE void onTriggerEventSparkLogic(float rpm, efitick_t edgeTimestamp, float currentPhase, float nextPhase, float nextNextPhase) {
#if !EFI_ANGLE_CLOCK
	UNUSED(nextNextPhase);
#endif // !EFI_ANGLE_CLOCK
	ScopePerf perf(PE::OnTriggerEventSparkLogic);

	if (!engineConfiguration->isIgnitionEnabled) {
		return;
	}

	LimpState limitedSparkState = getLimpManager()->allowIgnition();

	// todo: eliminate state copy logic by giving limpManager it's owm limp_manager.txt and leveraging LiveData
	engine->outputChannels.sparkCutReason = (int8_t)limitedSparkState.reason;
	bool limitedSpark = !limitedSparkState.value;

	const floatms_t dwellMs = engine->ignitionState.getDwell();
	if (std::isnan(dwellMs) || dwellMs <= 0) {
		warning(ObdCode::CUSTOM_DWELL, "invalid dwell to handle: %.2f", dwellMs);
		return;
	}

	if (!engine->ignitionEvents.isReady) {
		prepareIgnitionSchedule();
	}

#if EFI_ANGLE_CLOCK
	// Per-tooth invariant: the current-window [currentPhase, nextPhase) of this
	// tooth is a SUBSET of the previous tooth's EARLY window
	// [nextPhase, nextNextPhase + ANGLE_CLOCK_LOOKAHEAD_DEG) (scheduleDwellEarlyIfDue),
	// so in steady state every reachable event was already armed one tooth earlier
	// and the loop below only needs the current-window for the T-1-miss fallback
	// (first cycle after a (re)sync, or isReady was false). The per-cylinder
	// dwellStartArmed guard therefore lives at the TOP of the non-wasted branch -
	// an armed cylinder is skipped with a single field read, before any window
	// math or limiter re-checks.
	//
	// (The old allDwellArmed whole-loop exit was dead code: in sequential mode
	// at most one cylinder is charging at a time, so all four can never be armed
	// simultaneously.)
#endif // EFI_ANGLE_CLOCK

	/**
	 * Ignition schedule is defined once per revolution
	 * See initializeIgnitionActions()
	 */


	// Only apply odd cylinder count wasted logic if:
	// - odd cyl count
	// - current mode is wasted spark
	// - four stroke
	bool enableOddCylinderWastedSpark =
		engine->engineState.useOddFireWastedSpark
		&& getCurrentIgnitionMode() == IM_WASTED_SPARK;

	if (engine->ignitionEvents.isReady) {
		for (size_t i = 0; i < engineConfiguration->cylindersCount; i++) {
			IgnitionEvent *event = &engine->ignitionEvents.elements[i];

			angle_t dwellAngle = event->dwellAngle;

			angle_t sparkAngle = event->sparkAngle;
			if (std::isnan(sparkAngle)) {
				warning(ObdCode::CUSTOM_ADVANCE_SPARK, "NaN advance");
				continue;
			}

			bool isOddCylWastedEvent = false;
			if (enableOddCylinderWastedSpark) {
				auto dwellAngleWastedEvent = dwellAngle + 360;
				if (dwellAngleWastedEvent > 720) {
					dwellAngleWastedEvent -= 720;
				}

				// Check whether this event hits 360 degrees out from now (ie, wasted spark),
				// and if so, twiddle the dwell and spark angles so it happens now instead
				isOddCylWastedEvent = isPhaseInRange(dwellAngleWastedEvent, currentPhase, nextPhase);

				if (isOddCylWastedEvent) {
					dwellAngle = dwellAngleWastedEvent;

					sparkAngle += 360;
					if (sparkAngle > 720) {
						sparkAngle -= 720;
					}
				}
			}

			if (!isOddCylWastedEvent) {
				// Skip what's already armed this cycle with a single field read,
				// BEFORE the window math below - an armed cylinder never needs
				// the scheduleEarly/scheduleNow range tests or the limiter
				// re-checks. Behavior-identical to the old post-window `continue`
				// for the armed case (the early branch of scheduleDwellEarlyIfDue,
				// or an earlier current-window arm, already scheduled it; the
				// guard also blocks a same-tooth double schedule).
				if (event->dwellStartArmed) {
					continue;
				}

				// The dwell is scheduled one tooth ahead (EFI_ANGLE_CLOCK): the
				// early window [nextPhase, nextNextPhase) gives the handoff a
				// full tooth of slack instead of the old 0-1 tooth lead. The
				// current-tooth window is kept for the first cycle after a
				// (re)sync, when the dwell angle can already be inside the
				// current tooth. dwellStartArmed prevents double scheduling:
				// the early branch of this tooth covers the current-tooth
				// window of the NEXT tooth.
				bool scheduleEarly = false;
#if EFI_ANGLE_CLOCK
				// The early window is only meaningful when a distinct next-next
				// tooth exists: useOnlyRisingEdges wheels store the falling edge at
				// the preceding rise's angle, so findNextTriggerToothAngle(i+1)
				// returns the same phase as nextPhase and isPhaseInRange
				// (next <= current) then matches EVERY angle - the dwell would be
				// armed every tooth and refused, exactly like the fire arm's guard
				// below. Fall back to the current-tooth window (FALSE-build
				// behavior) in that case.
				//
				// LOOKAHEAD (2026-09-26): the upper bound extends past
				// nextNextPhase by ANGLE_CLOCK_LOOKAHEAD_DEG so a dwell that sits at
				// the next next-tooth boundary is armed at T-1 with a full tooth of
				// lead instead of the current-window rem ~ 0 arm (immediate class).
				scheduleEarly = nextNextPhase != nextPhase && isPhaseInRange(dwellAngle, nextPhase, nextNextPhase + ANGLE_CLOCK_LOOKAHEAD_DEG);
#endif // EFI_ANGLE_CLOCK
				bool scheduleNow = isPhaseInRange(dwellAngle, currentPhase, nextPhase);
				if (!scheduleEarly && !scheduleNow) {
					continue;
				}
#if EFI_ANGLE_CLOCK
				// Count ONLY arms that actually happened here (past the dwellStartArmed
				// guard). scheduleDwellEarlyIfDue already counted + armed this dwell
				// earlier in the same tooth, so counting before the guard double-counted
				// it (measured lookahead dwell == 2x dwell fired); counting after keeps
				// lookahead dwell ~1:1 with fired, matching inj.
				if (scheduleEarly) {
					angleClockNoteLookaheadDwell(dwellAngle, nextNextPhase);
				}
#endif // EFI_ANGLE_CLOCK
			}

			if (i == 0 && engineConfiguration->artificialTestMisfire && (getRevolutionCounter() % ((int)engineConfiguration->scriptSetting[5]) == 0)) {
				// artificial misfire on cylinder #1 for testing purposes
				// enable artificialMisfire
				warning(ObdCode::CUSTOM_ARTIFICIAL_MISFIRE, "artificial misfire on cylinder #1 for testing purposes %d", engine->engineState.globalSparkCounter);
				continue;
			}
#if ROTATIONAL_IDLE_CONTROLLER
		if (engine->rotationalIdleController.shouldSkipSparkRotationalIdle()) {
			continue;
		}
#endif // ROTATIONAL_IDLE_CONTROLLER

#if EFI_LAUNCH_CONTROL
            bool sparkLimited = engine->softSparkLimiter.shouldSkip() || engine->hardSparkLimiter.shouldSkip();
            engine->ignitionState.luaIgnitionSkip = sparkLimited;
			if (sparkLimited) {
				continue;
			}
#endif // EFI_LAUNCH_CONTROL

#if EFI_ANTILAG_SYSTEM && EFI_LAUNCH_CONTROL
/*
       if (engine->antilagController.isAntilagCondition) {
			if (engine->ALSsoftSparkLimiter.shouldSkip()) {
				continue;
			}
		}
		float throttleIntent = Sensor::getOrZero(SensorType::DriverThrottleIntent);
		engine->antilagController.timingALSSkip = interpolate3d(
			config->ALSIgnSkipTable,
			config->alsIgnSkipLoadBins, throttleIntent,
			config->alsIgnSkiprpmBins, rpm
		);

			auto ALSSkipRatio = engine->antilagController.timingALSSkip;
            engine->ALSsoftSparkLimiter.setTargetSkipRatio(ALSSkipRatio/100);
*/
#endif // EFI_ANTILAG_SYSTEM

			scheduleSparkEvent(limitedSpark, event, rpm, dwellMs, dwellAngle, sparkAngle, edgeTimestamp, currentPhase, nextPhase, false);
		}
	}
}

#if EFI_ANGLE_CLOCK
/**
 * Schedules the FULL spark event (dwell arm on TMR2 + spark queued for TMR4)
 * for cylinders whose dwellAngle falls in [nextPhase, nextNextPhase). Called
 * at the BEGINNING of mainTriggerCallback (~30 µs elapsed), BEFORE handleFuel
 * and onTriggerEventSparkLogic.
 *
 * WHY full scheduleSparkEvent, not just angleClockArmDwell:
 * Setting dwellStartArmed=true without queueing the spark makes
 * onTriggerEventSparkLogic skip the cylinder entirely - the spark never
 * enters the angle queue, TMR4 is never armed, rescue fires -> C9352/C9353
 * and the engine stops starting (field bug 2026-09-01, psc=70 session).
 * Calling scheduleSparkEvent does both: arms dwell on TMR2 AND queues the
 * spark so scheduleEventsUntilNextTriggerTooth can arm it on TMR4 later.
 *
 * dwellStartArmed=true is set inside scheduleSparkEvent, causing
 * onTriggerEventSparkLogic to correctly skip the cylinder on this tooth.
 */
TRIGGER_RAM_CODE void scheduleDwellEarlyIfDue(float rpm, efitick_t edgeTimestamp,
                                               float currentPhase,
                                               float nextPhase,
                                               float nextNextPhase) {
    if (!engine->ignitionEvents.isReady) {
        return;
    }
    if (nextNextPhase == nextPhase) {
        return;  // useOnlyRisingEdges guard (same as onTriggerEventSparkLogic)
    }

    const floatms_t dwellMs = engine->ignitionState.getDwell();
    if (std::isnan(dwellMs) || dwellMs <= 0) {
        return;
    }

    // Mirror the limiter check from onTriggerEventSparkLogic.
    const bool limitedSpark = !getLimpManager()->allowIgnition().value;

    for (size_t i = 0; i < engineConfiguration->cylindersCount; i++) {
        IgnitionEvent* event = &engine->ignitionEvents.elements[i];
        if (event->dwellStartArmed) {
            continue;  // already scheduled this cycle
        }
        const angle_t dwellAngle = event->dwellAngle;
        const angle_t sparkAngle = event->sparkAngle;
        if (std::isnan(dwellAngle) || std::isnan(sparkAngle)) {
            continue;
        }
        // Only the EARLY window [nextPhase, nextNextPhase) is checked here.
        // The current window [currentPhase, nextPhase) is intentionally left to
        // onTriggerEventSparkLogic (called after the rpm==0 gate on real teeth only).
        //
        // WHY: scheduleDwellEarlyIfDue runs on ALL teeth, including spurious noise
        // teeth during the catch storm (8400+ events/sec vs 2300/sec normal). At 3.3%
        // probability a noise tooth's currentPhase accidentally matches the current
        // window for some cylinder -> false arm -> TMR2 fires early -> spark fires ->
        // prepareCylinderIgnitionSchedule clears dwellStartArmed=false -> the REAL
        // scheduling tooth re-arms -> double scheduling -> 2x lateArm rate.
        // Measured on 2026-09-04: fired+lateArm = 17956 / 233s = 77/s, expected 38.3/s
        // = exactly 2x -> storm double-scheduling confirmed.
        //
        // With fix A (this function before rpm==0 gate), the early arm at tooth T-1
        // runs even on rpm==0 flap teeth -> dwellStartArmed=true at tooth T -> the
        // current-window fallback in onTriggerEventSparkLogic is blocked -> no double.
        // The current window in onTriggerEventSparkLogic is the rare fallback for
        // first-cycle-after-sync or isReady=false cases.
        //
        // LOOKAHEAD (2026-09-26): the upper bound extends past nextNextPhase by
        // ANGLE_CLOCK_LOOKAHEAD_DEG. The START angles sit AT tooth boundaries by
        // construction (advance + dwell), so a target just past nextNextPhase used to
        // fall to the current-window arm at rem ~ 0 -> past-due at refresh ->
        // immediate (the residual ~2% over a drive). The extension arms it here at
        // T-1 with a full tooth of lead; dwellStartArmed blocks re-arming and the
        // charge-anchored rescue bounds a late charge start.
        if (!isPhaseInRange(dwellAngle, nextPhase, nextNextPhase + ANGLE_CLOCK_LOOKAHEAD_DEG)) {
            continue;  // dwell not in the (extended) early window this tooth
        }
        angleClockNoteLookaheadDwell(dwellAngle, nextNextPhase);
        // Schedule the full event: arms TMR2 for dwell AND queues spark for
        // TMR4 arming at the next tooth via scheduleEventsUntilNextTriggerTooth.
        // scheduleSparkEvent sets dwellStartArmed=true so onTriggerEventSparkLogic
        // skips this cylinder. The TMR2 arm margin at ~5 µs elapsed:
        //   7000 rpm: delay(6°)=571 ticks=143 µs >> 20 ticks elapsed. OK.
        scheduleSparkEvent(limitedSpark, event, rpm, dwellMs,
                           dwellAngle, sparkAngle, edgeTimestamp,
                           currentPhase, nextPhase, true);
    }
}
#endif // EFI_ANGLE_CLOCK

/**
 * Number of sparks per physical coil
 * @see getNumberOfInjections
 */
int getNumberOfSparks(ignition_mode_e mode) {
	switch (mode) {
	case IM_ONE_COIL:
		return engineConfiguration->cylindersCount;
	case IM_TWO_COILS:
		return engineConfiguration->cylindersCount / 2;
	case IM_INDIVIDUAL_COILS:
		return 1;
	case IM_WASTED_SPARK:
		return 2;
	default:
		firmwareError(ObdCode::CUSTOM_ERR_IGNITION_MODE, "Unexpected ignition_mode_e %d", mode);
		return 1;
	}
}

/**
 * @see getInjectorDutyCycle
 */
percent_t getCoilDutyCycle(float rpm) {
	floatms_t totalPerCycle = engine->ignitionState.getDwell() * getNumberOfSparks(getCurrentIgnitionMode());
	floatms_t engineCycleDuration = getCrankshaftRevolutionTimeMs(rpm) * (getEngineRotationState()->getOperationMode() == TWO_STROKE ? 1 : 2);
	return 100 * totalPerCycle / engineCycleDuration;
}

#endif // EFI_ENGINE_CONTROL
