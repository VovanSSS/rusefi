/*
 * @file test_map_averaging.cpp
 *
 * @date: may 12, 2025
 * @author FDSoftware
 */

#include "pch.h"
#include "map_averaging.h"
#include "harley.h"
#include "linear_func.h"

#include <algorithm>
#include <array>
#include <vector>

namespace {
	auto const startAveragingAction{ action_s::make<startAveraging>((mapSampler*){}) };
}

TEST(MapAveragingAdc, BeforeConverterInitialization) {
	EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
	MapAverager sensor(SensorType::MapFast, MS2NT(200));

	// A healthy N52 key-on voltage arrives before initMap attaches the converter.
	sensor.onAdcSample(0.74f, true);
	EXPECT_EQ(0, eth.getWarningCounter());
	EXPECT_FALSE(engine->engineState.warnings.isWarningNow(ObdCode::CUSTOM_INSTANT_MAP_DECODING));
	EXPECT_FALSE(engine->outputChannels.isMapValid);
	EXPECT_FLOAT_EQ(0, engine->outputChannels.instantMAPValue);

	LinearFunc converter;
	converter.configure(4.5f, 0, 0.7f, 100, 5, 410);
	sensor.setFunction(converter);
	sensor.onAdcSample(0.74f, true);
	EXPECT_EQ(0, eth.getWarningCounter());
	EXPECT_TRUE(engine->outputChannels.isMapValid);
	EXPECT_NEAR(98.947f, engine->outputChannels.instantMAPValue, 0.04f);
}

TEST(MapAveragingAdc, BeforeChannelInitialization) {
	EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
	MapAverager sensor(SensorType::MapFast, MS2NT(200));
	LinearFunc converter;
	converter.configure(4.5f, 0, 0.7f, 100, 5, 410);
	sensor.setFunction(converter);

	// Neither a plausible voltage from an unselected channel nor an invalid
	// voltage should be treated as a MAP reading until its token is ready.
	sensor.onAdcSample(0.74f, false);
	EXPECT_FALSE(engine->outputChannels.isMapValid);
	EXPECT_FLOAT_EQ(0, engine->outputChannels.instantMAPValue);
	sensor.onAdcSample(4.5f, false);
	EXPECT_EQ(0, eth.getWarningCounter());
	EXPECT_FALSE(engine->engineState.warnings.isWarningNow(ObdCode::CUSTOM_INSTANT_MAP_DECODING));

	sensor.onAdcSample(0.74f, true);
	EXPECT_EQ(0, eth.getWarningCounter());
	EXPECT_TRUE(engine->outputChannels.isMapValid);
	EXPECT_NEAR(98.947f, engine->outputChannels.instantMAPValue, 0.04f);
}

TEST(MapAveragingAdc, InvalidVoltageAfterInitialization) {
	EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
	MapAverager sensor(SensorType::MapFast, MS2NT(200));
	LinearFunc converter;
	converter.configure(4.5f, 0, 0.7f, 100, 5, 410);
	sensor.setFunction(converter);

	// Readiness must not suppress real faults with a stopped engine or with
	// crank-synchronous averaging disabled.
	engineConfiguration->isMapAveragingEnabled = false;
	sensor.onAdcSample(4.5f, true);
	EXPECT_EQ(1, eth.getWarningCounter());
	EXPECT_TRUE(engine->engineState.warnings.isWarningNow(ObdCode::CUSTOM_INSTANT_MAP_DECODING));
	EXPECT_FALSE(engine->outputChannels.isMapValid);
	EXPECT_FLOAT_EQ(0, engine->outputChannels.instantMAPValue);
}

// ---- window angle math (200 Hz) - unchanged behavior ----

TEST(EngineModules, MapAveragingModule_onFastCallback) {
    EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
    engineConfiguration->isMapAveragingEnabled = true;

    MapAveragingModule mapModule;

    // trigger events at crank speed
    for (size_t i = 0; i < 9; i++) {
        eth.fireTriggerEventsWithDuration(200);
        eth.executeActions();
    }
    ASSERT_EQ(150,  Sensor::getOrZero(SensorType::Rpm));

    mapModule.onFastCallback();

    // we expect here than the map start angles correspond to the phase of the cylinder + 100 of samplingAngle
   	EXPECT_EQ(engine->engineState.mapAveragingStart[0], 100);
    EXPECT_EQ(engine->engineState.mapAveragingStart[1], 640);
    EXPECT_EQ(engine->engineState.mapAveragingStart[2], 280);
    EXPECT_EQ(engine->engineState.mapAveragingStart[3], 460);

    EXPECT_EQ(engine->engineState.mapAveragingDuration, 50);
}

TEST(EngineModules, MapAveragingModule_onFastCallbackCustomSampleWindow) {
    EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
    engineConfiguration->isMapAveragingEnabled = true;
	setArrayValues(engineConfiguration->map.samplingAngle, 75);

    MapAveragingModule mapModule;

    // trigger events at crank speed
    for (size_t i = 0; i < 9; i++) {
        eth.fireTriggerEventsWithDuration(200);
        eth.executeActions();
    }
    ASSERT_EQ(150,  Sensor::getOrZero(SensorType::Rpm));

    mapModule.onFastCallback();

    // we expect here than the map start angles correspond to the phase of the cylinder + 75 of samplingAngle
    EXPECT_EQ(engine->engineState.mapAveragingStart[0], 75);  // 0 + 75
    EXPECT_EQ(engine->engineState.mapAveragingStart[1], 615); // 540 + 75
    EXPECT_EQ(engine->engineState.mapAveragingStart[2], 255); // 180 + 75
    EXPECT_EQ(engine->engineState.mapAveragingStart[3], 435); // 360 + 75

    EXPECT_EQ(engine->engineState.mapAveragingDuration, 50);
}

TEST(EngineModules, MapAveragingModule_onFastCallbackOddFire) {
	EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
	setHarley();
	// Apply the new cylinder count, firing order, and trigger before calculating
	// offsets. Otherwise cylinder 2 retains the four-cylinder setup's 540 degrees.
	// Update geometry without notifying unrelated global hardware controllers.
	engine->updateTriggerConfiguration();
	setArrayValues(engineConfiguration->map.samplingAngle, 75);
	engine->rpmCalculator.setRpmValue(150);

	engine->module<MapAveragingModule>()->onFastCallback();

	ASSERT_EQ(2, engineConfiguration->cylindersCount);
	EXPECT_FLOAT_EQ(52.5f, engine->engineState.mapAveragingStart[0]);  // 0 + 75 - 22.5
	EXPECT_FLOAT_EQ(457.5f, engine->engineState.mapAveragingStart[1]); // 360 + 75 + 22.5
	EXPECT_FLOAT_EQ(50, engine->engineState.mapAveragingDuration);
}

// ---- once-per-cycle planner (replaces the per-tooth onEnginePhase scan) ----

TEST(EngineModules, MapAveragingModule_scheduleNextCycleWindows) {
    EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
    engineConfiguration->isMapAveragingEnabled = true;
    engineConfiguration->measureMapOnlyInOneCylinder = true;
	engine->rpmCalculator.setRpmValue(200);

	// Zero the trigger phase so scheduled offsets equal the absolute window
	// angles: the TDC executor hook fires at ENGINE PHASE 0 (TDC1), so with
	// tdcPosition() = 0 the offset == mapAveragingStart[i] exactly.
	getTriggerCentral()->triggerShape.tdcPosition = 0;
	engineConfiguration->globalTriggerAngleOffset = 0;

	engine->module<MapAveragingModule>()->onFastCallback();
	engine->module<MapAveragingModule>()->scheduleNextCycleWindows();

	// samplingAngle default is 100 deg, cylinder 0 offset 0 -> start at 100 deg
    bool averageDone = eth.assertEventExistsAtEnginePhase("startMapAveraging callback", startAveragingAction, static_cast<angle_t>(100));
    EXPECT_TRUE(averageDone);

    // move forward, we expect that the startAveraging is called and we are currently running the averaging code
	eth.moveTimeForwardMs(100);
	eth.executeActions();
	EXPECT_TRUE(engine->outputChannels.isMapAveraging);

    // move forward, here we expect the averaging is done and endAveraging was called
   	eth.moveTimeForwardMs(100);
	eth.executeActions();
    EXPECT_FALSE(engine->outputChannels.isMapAveraging);
}

// One window at a time, but all four starts are planned for the coming cycle:
// 4 starts per cycle, one per cylinder phase region.
TEST(EngineModules, MapAveragingModule_scheduleAllFourWindows) {
    EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
    engineConfiguration->isMapAveragingEnabled = true;
    engineConfiguration->measureMapOnlyInOneCylinder = false;
	engine->rpmCalculator.setRpmValue(200);
	getTriggerCentral()->triggerShape.tdcPosition = 0;
	engineConfiguration->globalTriggerAngleOffset = 0;
	setArrayValues(engineConfiguration->map.samplingAngle, 75);

	engine->module<MapAveragingModule>()->onFastCallback();
	engine->module<MapAveragingModule>()->scheduleNextCycleWindows();

	EXPECT_EQ(4, engine->scheduler.size());

	// All four starts share one callback, so match by offset: walk the queue
	// and check the scheduled engine phases against the expected set.
	float usPerDeg = engine->rpmCalculator.oneDegreeUs;
	std::vector<float> scheduledOffsets;
	for (int i = 0; i < engine->scheduler.size(); i++) {
		auto event = engine->scheduler.getForUnitTest(i);
		if (event->action.getCallback() == startAveragingAction.getCallback()) {
			scheduledOffsets.push_back((event->getMomentUs() - getTimeNowUs()) / usPerDeg);
		}
	}
	ASSERT_EQ(4, scheduledOffsets.size());

	auto offsetExpected = [&](angle_t expected) {
		return std::any_of(scheduledOffsets.begin(), scheduledOffsets.end(), [&](angle_t actual) {
			return std::abs(actual - expected) < 1.0f;
		});
	};
	EXPECT_TRUE(offsetExpected(75));
	EXPECT_TRUE(offsetExpected(255));
	EXPECT_TRUE(offsetExpected(435));
	EXPECT_TRUE(offsetExpected(615));
}

// Regression for the 2026-09-24 double-subtraction bug (see
// docs/bug_map_planner_tdc_offset.md): with a nonzero tdcPosition() - the real
// m74_9 runs 112 - the windows must STILL be planned at their absolute
// engine-phase angles, because the TDC hook fires at engine phase 0. Before the
// fix the subtraction scheduled every window tdcPosition() degrees early.
TEST(EngineModules, MapAveragingModule_scheduleWindowsWithNonzeroTdcOffset) {
    EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
    engineConfiguration->isMapAveragingEnabled = true;
    engineConfiguration->measureMapOnlyInOneCylinder = false;
	engine->rpmCalculator.setRpmValue(200);

	// m74_9 configuration: shape tdcPosition 0 + tunable offset 112
	getTriggerCentral()->triggerShape.tdcPosition = 0;
	engineConfiguration->globalTriggerAngleOffset = 112;

	// sampling angle BELOW the offset (like the car below ~2375 rpm): with the
	// bug slot 0 wrapped 105 - 112 -> 713 deg
	setArrayValues(engineConfiguration->map.samplingAngle, 105);

	engine->module<MapAveragingModule>()->onFastCallback();
	engine->module<MapAveragingModule>()->scheduleNextCycleWindows();

	EXPECT_EQ(4, engine->scheduler.size());

	float usPerDeg = engine->rpmCalculator.oneDegreeUs;
	std::vector<float> scheduledOffsets;
	for (int i = 0; i < engine->scheduler.size(); i++) {
		auto event = engine->scheduler.getForUnitTest(i);
		if (event->action.getCallback() == startAveragingAction.getCallback()) {
			scheduledOffsets.push_back((event->getMomentUs() - getTimeNowUs()) / usPerDeg);
		}
	}
	ASSERT_EQ(4, scheduledOffsets.size());

	auto offsetExpected = [&](angle_t expected) {
		return std::any_of(scheduledOffsets.begin(), scheduledOffsets.end(), [&](angle_t actual) {
			return std::abs(actual - expected) < 1.0f;
		});
	};
	EXPECT_TRUE(offsetExpected(105));
	EXPECT_TRUE(offsetExpected(285));
	EXPECT_TRUE(offsetExpected(465));
	EXPECT_TRUE(offsetExpected(645));
}

// Regression: every started window must also close. With the old tdcPosition()
// double subtraction and samplingAngle < tdcPosition, slot 0's 50-deg window
// straddled the cycle boundary and the next cycle's plan (cancel-before-re-arm)
// killed its pending close every cycle - endAveraging 3/cyc on the car. Step a
// full cycle, re-plan at the next mark (engine phase 0), and check that nothing
// is still averaging shortly after the boundary.
TEST(EngineModules, MapAveragingModule_allWindowsCloseAcrossCycleBoundary) {
    EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
    engineConfiguration->isMapAveragingEnabled = true;
    engineConfiguration->measureMapOnlyInOneCylinder = false;
	engine->rpmCalculator.setRpmValue(200);

	getTriggerCentral()->triggerShape.tdcPosition = 0;
	engineConfiguration->globalTriggerAngleOffset = 112;
	setArrayValues(engineConfiguration->map.samplingAngle, 105);

	engine->module<MapAveragingModule>()->onFastCallback();
	engine->module<MapAveragingModule>()->scheduleNextCycleWindows();

	// 200 rpm: 720 deg = 600 ms; 1 ms steps so executor events fire at
	// distinct instants. With the fix the last window closes at 695 deg
	// (579 ms); with the bug slot 0 latches 'true' at 713 deg and its close
	// (763 deg) is cancelled by the re-plan.
	for (int i = 0; i < 595; i++) {
		eth.moveTimeForwardMs(1);
		eth.executeActions();
	}

	// the next engine cycle's TDC mark: re-plans and (before the fix)
	// cancels slot 0's still-pending close
	engine->module<MapAveragingModule>()->scheduleNextCycleWindows();

	// 30 ms past the boundary: all four closes fired -> idle flag
	eth.moveTimeForwardMs(30);
	eth.executeActions();

	EXPECT_FALSE(engine->outputChannels.isMapAveraging);
}

TEST(EngineModules, MapAveragingModule_scheduleSkipsWhenDisabledOrStopped) {
    EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
    engineConfiguration->isMapAveragingEnabled = false;
    engine->rpmCalculator.setRpmValue(200);
    engine->module<MapAveragingModule>()->scheduleNextCycleWindows();
    EXPECT_EQ(0, engine->scheduler.size());

    engineConfiguration->isMapAveragingEnabled = true;
    engine->rpmCalculator.setRpmValue(0);
    engine->module<MapAveragingModule>()->scheduleNextCycleWindows();
    EXPECT_EQ(0, engine->scheduler.size());
}

// ---- firing order successor: each window feeds the successor's injection mass ----

TEST(EngineModules, MapAveraging_firingOrderSuccessor) {
    EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);

	// firing order 1-3-4-2: angle offsets 0 / 180 / 360 / 540 for cylinders 0..3
	EXPECT_EQ(2, engine->module<MapAveragingModule>()->firingOrderSuccessor(0));
	EXPECT_EQ(3, engine->module<MapAveragingModule>()->firingOrderSuccessor(2));
	EXPECT_EQ(1, engine->module<MapAveragingModule>()->firingOrderSuccessor(3));
	// wraps across the cycle boundary: the last window feeds the first cylinder
	// of the NEXT cycle
	EXPECT_EQ(0, engine->module<MapAveragingModule>()->firingOrderSuccessor(1));
}

// ---- Variant A: endAveraging rescales the successor's mass with the fresh window ----

TEST(EngineModules, MapAveraging_endAveragingRescalesSuccessorMass) {
    EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
    engineConfiguration->fuelAlgorithm = engine_load_mode_e::LM_SPEED_DENSITY;
    engineConfiguration->isMapAveragingEnabled = true;
    engineConfiguration->measureMapOnlyInOneCylinder = true;
	engine->rpmCalculator.setRpmValue(200);
	getTriggerCentral()->triggerShape.tdcPosition = 0;
	engineConfiguration->globalTriggerAngleOffset = 0;

	// state as the 200 Hz pipeline leaves it
	engine->engineState.referenceMap = 80;
	for (size_t i = 0; i < 4; i++) {
		engine->engineState.injectionMass[i] = 10;
		engine->engineState.injectionMassMapBasis[i] = 80;
	}

	// linear converter: volts -> kPa (0..5V => 0..100 kPa, out-of-range bounds wide open)
	static LinearFunc testConverter;
	testConverter.configure(0, 0, 5, 100, -100, 1000);
	getMapAvg(0).setFunction(testConverter);

	engine->module<MapAveragingModule>()->onFastCallback();
	engine->module<MapAveragingModule>()->scheduleNextCycleWindows();

	// window opens (offset 100 deg at 200 rpm ~= 83 ms)
	eth.moveTimeForwardMs(100);
	eth.executeActions();
	ASSERT_TRUE(engine->outputChannels.isMapAveraging);

	// one sample inside the window: 2.5V -> 50 kPa
	getMapAvg(0).submit(2.5f);

	// window closes (start + duration)
	eth.moveTimeForwardMs(100);
	eth.executeActions();
	ASSERT_FALSE(engine->outputChannels.isMapAveraging);

	// cylinder 0's window closed with 50 kPa -> successor (cylinder 2) rescaled:
	// 10 * 50 / 80 = 6.25, with the basis advanced to 50
	EXPECT_NEAR(engine->engineState.injectionMass[2], 10.0f * 50.0f / 80.0f, 1e-3);
	EXPECT_NEAR(engine->engineState.injectionMassMapBasis[2], 50.0f, 1e-3);

	// the other cylinders' masses stayed reference-based
	EXPECT_NEAR(engine->engineState.injectionMass[0], 10.0f, 1e-3);
}

// ---- Variant A: the injection-scheduling-time single multiply refresh ----

TEST(EngineModules, MapAveraging_refreshInjectionMass) {
    EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
    engineConfiguration->fuelAlgorithm = engine_load_mode_e::LM_SPEED_DENSITY;
    engine->engineState.referenceMap = 80;
	engine->engineState.injectionMassMapBasis[2] = 80; // the 200 Hz tick wrote the reference basis
	Sensor::setMockValue(SensorType::MapFast, 96);

	// refresh against the single window: mass *= MapFast / basis
    EXPECT_NEAR(refreshInjectionMassForCylinder(2, 10.0f), 12.0f, 1e-3);

	// endAveraging already applied the same window value: the refresh must no-op
	// (no double application of the single shared window)
	engine->engineState.injectionMassMapBasis[2] = 96;
    EXPECT_NEAR(refreshInjectionMassForCylinder(2, 12.0f), 12.0f, 1e-3);

	// invalid window value: pass through
	Sensor::resetMockValue(SensorType::MapFast);
	engine->engineState.injectionMassMapBasis[2] = 80;
    EXPECT_NEAR(refreshInjectionMassForCylinder(2, 10.0f), 10.0f, 1e-3);

	// alpha-N / MAF: pass through (mass is not MAP-proportional there)
	engineConfiguration->fuelAlgorithm = engine_load_mode_e::LM_ALPHA_N;
	Sensor::setMockValue(SensorType::MapFast, 96);
	engine->engineState.injectionMassMapBasis[2] = 80;
    EXPECT_NEAR(refreshInjectionMassForCylinder(2, 10.0f), 10.0f, 1e-3);
}

namespace {

class MapSamplingAngle : public testing::Test {
protected:
	EngineTestHelper eth{engine_type_e::TEST_CRANK_ENGINE};
	MapAveragingModule& module = *engine->module<MapAveragingModule>();

	void SetUp() override {
		engineConfiguration->isMapAveragingEnabled = true;
		engineConfiguration->measureMapOnlyInOneCylinder = true;
		engineConfiguration->isIgnitionEnabled = false;
		engineConfiguration->isInjectionEnabled = false;
		engine->rpmCalculator.setRpmValue(1000);
		module.init();
		setWindow(100, 50);
		engine->scheduler.clear();
	}

	void TearDown() override {
		engine->scheduler.clear();
		getMapAvg(0).stop();
	}

	void setWindow(float start, float duration) {
		setArrayValues(engineConfiguration->map.samplingAngle, start);
		setArrayValues(engineConfiguration->map.samplingWindow, duration);
		module.onFastCallback();
	}

	void expectStart(int queueIndex, size_t cylinder, efitimeus_t edgeUs, float delayUs) {
		auto event = engine->scheduler.getForUnitTest(queueIndex);
		ASSERT_NE(nullptr, event);
		EXPECT_EQ(startAveragingAction.getCallback(), event->action.getCallback());
		EXPECT_EQ(&module.samplers[cylinder], event->action.getArgument<mapSampler*>());
		EXPECT_EQ(cylinder, module.samplers[cylinder].cylinderNumber);
		EXPECT_NEAR(edgeUs + delayUs, event->getMomentUs(), 1);
	}
};

TEST_F(MapSamplingAngle, InterpolatesIndependentAngleAndWindowRpmCurves) {
	auto& map = engineConfiguration->map;
	for (size_t i = 0; i < efi::size(map.samplingAngle); i++) {
		map.samplingAngleBins[i] = 1000 + 1000 * i;
		map.samplingAngle[i] = 40 + 20 * i;
	}
	for (size_t i = 0; i < efi::size(map.samplingWindow); i++) {
		map.samplingWindowBins[i] = 1000 + 2000 * i;
		map.samplingWindow[i] = 20 + 10 * i;
	}
	struct Sample { float rpm; float angle; float duration; };
	for (const auto& sample : {Sample{0, 40, 20}, Sample{1000, 40, 20},
		Sample{1500, 50, 22.5f}, Sample{2000, 60, 25}, Sample{16000, 180, 90}}) {
		SCOPED_TRACE(sample.rpm);
		engine->rpmCalculator.setRpmValue(sample.rpm);
		module.onFastCallback();
		EXPECT_FLOAT_EQ(sample.angle, engine->engineState.mapAveragingStart[0]);
		EXPECT_FLOAT_EQ(sample.duration, engine->engineState.mapAveragingDuration);
	}
}

TEST_F(MapSamplingAngle, WrapsNegativeAndOverflowAnglesForEveryCylinder) {
	struct Sample { float angle; std::array<float, 4> starts; };
	for (const auto& sample : {Sample{-100, {620, 440, 80, 260}},
		Sample{300, {300, 120, 480, 660}}, Sample{720, {0, 540, 180, 360}}}) {
		SCOPED_TRACE(sample.angle);
		setWindow(sample.angle, 50);
		for (size_t cylinder = 0; cylinder < sample.starts.size(); cylinder++) {
			EXPECT_FLOAT_EQ(sample.starts[cylinder], engine->engineState.mapAveragingStart[cylinder]);
		}
	}
}

TEST_F(MapSamplingAngle, ClampsWindowToTenDegreesAndCylinderPeriodMinusTen) {
	struct Sample { float window; float expected; };
	for (const auto& sample : {Sample{-50, 10}, Sample{0, 10}, Sample{5, 10},
		Sample{10, 10}, Sample{50, 50}, Sample{170, 170}, Sample{180, 170}, Sample{720, 170}}) {
		SCOPED_TRACE(sample.window);
		setWindow(100, sample.window);
		EXPECT_FLOAT_EQ(sample.expected, engine->engineState.mapAveragingDuration);
	}

	setHarley();
	// applyTriggerWaveform also broadcasts a configuration change to static ETB
	// controllers, whose PID pointers may outlive a previous EngineTestHelper.
	// Only trigger/cylinder geometry is relevant to these MAP tests.
	engine->updateTriggerConfiguration();
	setWindow(100, 720);
	EXPECT_FLOAT_EQ(350, engine->engineState.mapAveragingDuration);
}

TEST_F(MapSamplingAngle, UsesTwoStrokeCycleForWrappingAndWindowLimit) {
	engineConfiguration->twoStroke = true;
	engine->updateTriggerConfiguration();
	ASSERT_FLOAT_EQ(360, engine->engineState.engineCycle);
	setWindow(-20, 200);
	const float expected[] = {340, 250, 70, 160};
	for (size_t cylinder = 0; cylinder < efi::size(expected); cylinder++) {
		EXPECT_FLOAT_EQ(expected[cylinder], engine->engineState.mapAveragingStart[cylinder]);
	}
	EXPECT_FLOAT_EQ(80, engine->engineState.mapAveragingDuration);
}

TEST_F(MapSamplingAngle, ZeroDurationDoesNotStartAveraging) {
	engine->engineState.mapAveragingDuration = 0;
	startAveraging(&module.samplers[0]);
	EXPECT_EQ(0, engine->scheduler.size());
	EXPECT_FALSE(engine->outputChannels.isMapAveraging);
}

TEST_F(MapSamplingAngle, TriggerEdgesStartAndStopWindowAtConfiguredAngles) {
	setWindow(75, 30);
	engineConfiguration->globalTriggerAngleOffset = 0;
	engine->updateTriggerConfiguration();
	// The test engine's half-moon crank wheel has an edge every 180 degrees.
	// Feed real edges through the decoder to establish 150 RPM and engine phase.
	eth.smartFireTriggerEvents2(10, 200);
	ASSERT_FLOAT_EQ(150, Sensor::getOrZero(SensorType::Rpm));
	for (int edge = 0; edge < 4 && engine->triggerCentral.currentEngineDecodedPhase != 0; edge++) {
		if (edge % 2 == 0) {
			eth.smartFireRise(200);
		} else {
			eth.smartFireFall(200);
		}
	}
	ASSERT_FLOAT_EQ(0, engine->triggerCentral.currentEngineDecodedPhase);
	const auto edgeUs = getTimeNowUs();
	// Drain the TDC marker queued at this edge, before the future MAP window.
	eth.executeActions();
	EXPECT_FALSE(engine->outputChannels.isMapAveraging);
	ASSERT_EQ(1, engine->scheduler.size());
	expectStart(0, 0, edgeUs, 83333.333f); // 75 degrees at 150 RPM
	const auto startNt = engine->scheduler.getHead()->getMomentNt();
	eth.setTimeAndInvokeEventsNt(startNt - US2NT(1));
	EXPECT_FALSE(engine->outputChannels.isMapAveraging);
	eth.setTimeAndInvokeEventsNt(startNt);
	EXPECT_TRUE(engine->outputChannels.isMapAveraging);
	ASSERT_EQ(1, engine->scheduler.size());
	const auto endNt = engine->scheduler.getHead()->getMomentNt();
	EXPECT_NEAR(33333.333f, NT2USF(endNt - startNt), 1); // 30-degree window
	eth.setTimeAndInvokeEventsNt(endNt - US2NT(1));
	EXPECT_TRUE(engine->outputChannels.isMapAveraging);
	eth.setTimeAndInvokeEventsNt(endNt);
	EXPECT_FALSE(engine->outputChannels.isMapAveraging);
	EXPECT_EQ(0, engine->scheduler.size());
}

} // namespace
