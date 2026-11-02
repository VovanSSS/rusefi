#include "pch.h"
#include "misfire_detection.h"
#include "idle_thread.h"
#include "malfunction_central.h"

#include <cmath>

// 2026-09-23: the detector moved out of the handoff. The per-cycle planner
// scheduleNextCycleSegments() schedules 2N executor events (segment start/end per
// cylinder); each end event evaluates the segment of the cylinder that already
// fired. Planning/event-structure is exercised through the real scheduler, the
// detection math through direct boundary-callback feeds.

static MisfireController& getMc() {
	return engine->module<MisfireController>().unmock();
}

static void setupMisfireConfig(EngineTestHelper& eth) {
	// The planner's sub-tooth guard reads the *configured* trigger: give it a
	// fine wheel so the configured tooth pitch allows realistic windows.
	engineConfiguration->trigger.customTotalToothCount = 60;
	engineConfiguration->trigger.customSkippedToothCount = 2;
	eth.setTriggerType(trigger_type_e::TT_TOOTHED_WHEEL);

	engineConfiguration->misfireDetectionEnabled = true;
	engineConfiguration->misfireK                = 3.0f; // threshold = baseline + 3 * wobble
	engineConfiguration->misfireSettleCycles     = 0;    // no settle delay in tests
	engineConfiguration->misfireConsecutiveCount = 2;    // need >=2 flagged firings within the window
	engineConfiguration->misfireWindowFirings    = 16;   // sliding window across all cylinders (~4 cycles)
	engineConfiguration->misfireCountThreshold   = 4;    // small so the test latches quickly
	engineConfiguration->misfireWindowStart      = 20;
	engineConfiguration->misfireWindowEnd        = 120;

	// The planner anchors on the TDC hook phase: reference it to 0.
	getTriggerCentral()->triggerShape.tdcPosition = 0;
	engineConfiguration->globalTriggerAngleOffset = 0;
	engine->rpmCalculator.setRpmValue(1500);
}

namespace {
// Drives synthetic segments straight into the boundary callbacks (the same math
// the executor events trigger), optionally inflating one cylinder's segment.
struct SegmentDriver {
	efitick_t now = 0;

	void feedSegment(uint8_t cyl, float factor) {
		float segUs = 20000.0f; // arbitrary healthy idle baseline
		getMc().captureSegmentStart(cyl, now);
		now += US2NT((efitick_t)(segUs * factor));
		getMc().finishSegment(cyl, now);
	}

	void feedCycle(int targetCyl, float factor) {
		for (uint8_t i = 0; i < engineConfiguration->cylindersCount; i++) {
			feedSegment(i, (targetCyl >= 0 && i == targetCyl) ? factor : 1.0f);
		}
	}

	void feedHealthyCycle() { feedCycle(-1, 1.0f); }
};
} // namespace

// ---- Disabled by default ----

TEST(MisfireDetection, disabledByDefault) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);

	ASSERT_FALSE(engineConfiguration->misfireDetectionEnabled);

	// Even forcing idle, the planner must schedule nothing while disabled.
	engine->module<IdleController>().unmock().isIdling = true;

	engine->module<MisfireController>()->scheduleNextCycleSegments();

	EXPECT_EQ(0, engine->scheduler.size());
	EXPECT_FALSE(getMc().misfireDetectionActive);
	EXPECT_EQ(0, getMc().misfireTotalCount);
	EXPECT_FALSE(getMc().misfireLatched);
}

// ---- Planner structure: 2 events per cylinder per cycle ----

TEST(MisfireDetection, plannerSchedulesTwoEventsPerCylinder) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	setupMisfireConfig(eth);
	engine->module<IdleController>().unmock().isIdling = true;

	engine->module<MisfireController>()->scheduleNextCycleSegments();

	EXPECT_EQ(engineConfiguration->cylindersCount * 2, engine->scheduler.size());
}

// ---- Planner anchor regression (the 2026-09-24 double subtraction) ----

TEST(MisfireDetection, plannerSegmentOffsetsIgnoreTdcPosition) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	setupMisfireConfig(eth);
	engine->module<IdleController>().unmock().isIdling = true;

	// Nonzero tunable offset (m74_9 runs 112): the TDC hook fires at engine
	// phase 0, so segment offsets are the absolute window angles - NOT
	// windowStart - tdcPosition() (see docs/bug_map_planner_tdc_offset.md).
	engineConfiguration->globalTriggerAngleOffset = 112;

	engine->module<MisfireController>()->scheduleNextCycleSegments();
	EXPECT_EQ(engineConfiguration->cylindersCount * 2, engine->scheduler.size());

	// cylinder 0's segment must start TDC(0) + misfireWindowStart(20) = 20 deg
	// after the plan; before the fix it wrapped 20 - 112 -> 628 deg.
	auto const segStartAction{ action_s::make<misfireSegmentStartAction>(uint8_t{0}) };
	float usPerDeg = engine->rpmCalculator.oneDegreeUs;
	bool foundStartAt20 = false;
	for (int i = 0; i < engine->scheduler.size(); i++) {
		auto event = engine->scheduler.getForUnitTest(i);
		if (event->action.getCallback() == segStartAction.getCallback()) {
			float offsetDeg = (event->getMomentUs() - getTimeNowUs()) / usPerDeg;
			if (std::abs(offsetDeg - 20.0f) < 1.0f) {
				foundStartAt20 = true;
			}
		}
	}
	EXPECT_TRUE(foundStartAt20);
}

// ---- Executor-driven segments seed the baseline and arm detection ----

TEST(MisfireDetection, plannerSegmentsSeedBaseline) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	setupMisfireConfig(eth);
	engine->module<IdleController>().unmock().isIdling = true;

	// One planned cycle; step the mock clock so the boundary events fire at
	// different instants (the real executor fires them in separate batches as
	// the clock advances - one frozen 'now' would collapse every segment to 0).
	engine->module<MisfireController>()->scheduleNextCycleSegments();
	for (int i = 0; i < 100; i++) {
		eth.moveTimeForwardMs(1);
		eth.executeActions();
	}

	// The completed segments seeded the shared baseline...
	EXPECT_GT(getMc().misfireLastSegUs, 0.0f);

	// ...and the next planner pass arms detection with the seeded baseline.
	engine->module<MisfireController>()->scheduleNextCycleSegments();
	EXPECT_TRUE(getMc().misfireDetectionActive);
}

// ---- Healthy idle produces no misfires ----

TEST(MisfireDetection, healthyIdleNoMisfire) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	setupMisfireConfig(eth);
	engine->module<IdleController>().unmock().isIdling = true;

	for (int i = 0; i < 20; i++) {
		engine->module<MisfireController>()->scheduleNextCycleSegments();
		// one full engine cycle at 1500 rpm = 80 ms; step the clock so each of the
		// 8 events fires at its own instant - all fire before the next re-plan
		// (max offset ~660 deg = ~73 ms) so no segment gets cancelled mid-flight
		for (int step = 0; step < 90; step++) {
			eth.moveTimeForwardMs(1);
			eth.executeActions();
		}
	}

	// last planner pass armed detection on the seeded baseline
	EXPECT_TRUE(getMc().misfireDetectionActive);
	EXPECT_EQ(0, getMc().misfireTotalCount);
	EXPECT_FALSE(getMc().misfireLatched);
}

// ---- Repeated misfires on one cylinder latch the MIL ----

TEST(MisfireDetection, repeatedMisfireLatchesMil) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	setupMisfireConfig(eth);
	engine->module<IdleController>().unmock().isIdling = true;

	SegmentDriver d;
	// Seed the shared engine-wide baseline with healthy cycles first.
	for (int i = 0; i < 5; i++) {
		d.feedHealthyCycle();
	}
	ASSERT_EQ(0, getMc().misfireTotalCount);

	// Misfire cylinder 0 every cycle (1.6x slower segment). A single dead cylinder
	// puts >=2 flagged firings in the 16-firing window, so the engine-wide rate
	// test counts it and the MIL latches.
	for (int i = 0; i < 12; i++) {
		d.feedCycle(0, 1.6f);
	}

	EXPECT_GE(getMc().misfireTotalCount, engineConfiguration->misfireCountThreshold);
	EXPECT_TRUE(getMc().misfireLatched);

	getMc().onSlowCallback();
	error_codes_set_s errors;
	getErrorCodes(&errors);
	ASSERT_EQ(1, errors.count);
	EXPECT_EQ(ObdCode::OBD_Random_Misfire, errors.error_codes[0]);

	clearWarnings();
	getMc().onSlowCallback();
	EXPECT_TRUE(hasErrorCodes());
}

// ---- threshold == 0: monitor-only, never latches ----

TEST(MisfireDetection, thresholdZeroMonitorOnly) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	setupMisfireConfig(eth);
	engineConfiguration->misfireCountThreshold = 0; // monitor-only
	engine->module<IdleController>().unmock().isIdling = true;

	SegmentDriver d;
	for (int i = 0; i < 5; i++) {
		d.feedHealthyCycle();
	}
	for (int i = 0; i < 20; i++) {
		d.feedCycle(0, 1.6f); // misfire cylinder 0 every cycle
	}

	EXPECT_GT(getMc().misfireTotalCount, 0u); // counter accumulated
	EXPECT_FALSE(getMc().misfireLatched);     // never latched
}

// ---- Leaving idle stops monitoring ----

TEST(MisfireDetection, leavingIdleStopsMonitoring) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	setupMisfireConfig(eth);
	auto& idleCtrl = engine->module<IdleController>().unmock();
	idleCtrl.isIdling = true;

	engine->module<MisfireController>()->scheduleNextCycleSegments();
	for (int i = 0; i < 90; i++) {
		eth.moveTimeForwardMs(1);
		eth.executeActions();
	}
	// the next planner pass arms detection on the seeded baseline
	engine->module<MisfireController>()->scheduleNextCycleSegments();
	EXPECT_TRUE(getMc().misfireDetectionActive);

	// Drop out of idle - the planner must stop scheduling and reset the gate.
	idleCtrl.isIdling = false;
	engine->module<MisfireController>()->scheduleNextCycleSegments();
	EXPECT_FALSE(getMc().misfireDetectionActive);
	EXPECT_EQ(0, engine->scheduler.size());
}

// ---- Window narrower than one trigger tooth: refuse + configError, never freeze ----

TEST(MisfireDetection, narrowWindowBelowToothSpacingRefuses) {
	EngineTestHelper eth(engine_type_e::TEST_ENGINE);
	setupMisfireConfig(eth);
	engine->module<IdleController>().unmock().isIdling = true;

	// A sub-tooth window: both ends fall inside one trigger interval, so the timed
	// segment would collapse to zero and every live-data field would silently
	// freeze. The planner must refuse to schedule and raise a configError.
	clearConfigErrorMessage();
	engineConfiguration->misfireWindowStart = 20;
	engineConfiguration->misfireWindowEnd   = 21; // 1 deg wide, far below any real tooth pitch

	engine->module<MisfireController>()->scheduleNextCycleSegments();

	EXPECT_FALSE(getMc().misfireDetectionActive);
	EXPECT_EQ(0, engine->scheduler.size());
	EXPECT_TRUE(hasConfigError());

	// Widening back to a valid window must clear the refusal and resume scheduling.
	clearConfigErrorMessage();
	engineConfiguration->misfireWindowEnd = 120;
	engine->module<MisfireController>()->scheduleNextCycleSegments();

	EXPECT_FALSE(hasConfigError());
	EXPECT_EQ(engineConfiguration->cylindersCount * 2, engine->scheduler.size());
}
