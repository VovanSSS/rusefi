/*
 * @file test_map_instant_arm.cpp
 *
 * Variant B (2026-09-29): per-cylinder instant MAP at arm time for
 * shared-plenum boards (m74_9) - MapFast is published once per 200 Hz tick as
 * the instant conversion of the raw fast-ADC latch, and each cylinder's
 * injection mass is rescaled by the raw latch converted at its own arm time.
 * See isMapInstantAtArm() / custom_board_mapInstantAtArm.
 */

#include "pch.h"
#include "map_averaging.h"
#include "board_overrides.h"
#include "linear_func.h"

// RAII toggle for the custom_board_mapInstantAtArm board hook: the unit-test
// environment is not m74_9, so install the hook directly instead of building
// the board. Restores nullopt (all other boards' default) on scope exit.
class MapInstantAtArmScope {
public:
	MapInstantAtArmScope(bool on) {
		custom_board_mapInstantAtArm = on ? []() { return true; } : []() { return false; };
	}

	~MapInstantAtArmScope() {
		custom_board_mapInstantAtArm.reset();
	}
};

namespace {
	// linear converter: 0..5V => 0..100 kPa, bounds wide open (-100..1000)
	void setTestConverter() {
		static LinearFunc testConverter;
		testConverter.configure(0, 0, 5, 100, -100, 1000);
		getMapAvg(0).setFunction(testConverter);
	}
}

// ---- arm-time refresh: per-cylinder instant MAP from the raw latch ----

TEST(EngineModules, MapInstantArm_refreshRescalesMassByInstantMap) {
	EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
	MapInstantAtArmScope scope(true);
	engineConfiguration->fuelAlgorithm = engine_load_mode_e::LM_SPEED_DENSITY;
	engineConfiguration->isMapAveragingEnabled = true;
	setTestConverter();
	engine->engineState.referenceMap = 80;
	engine->outputChannels.rawMapFast = 2.5f; // -> 50 kPa

	// mass = 10 * 50 / 80 = 6.25 - and the refresh reads the RAW LATCH, not
	// the MapFast registry value (a stale registry entry must not matter)
	Sensor::setMockValue(SensorType::MapFast, 96);
	EXPECT_NEAR(refreshInjectionMassForCylinder(2, 10.0f), 6.25f, 1e-3);
	Sensor::resetMockValue(SensorType::MapFast);

	// out-of-range raw (60V -> 1200 kPa > 1000 bound): pass through
	engine->outputChannels.rawMapFast = 60;
	EXPECT_NEAR(refreshInjectionMassForCylinder(2, 10.0f), 10.0f, 1e-3);

	// zero reference (engine not started / pipeline never ran): pass through
	engine->outputChannels.rawMapFast = 2.5f;
	engine->engineState.referenceMap = 0;
	EXPECT_NEAR(refreshInjectionMassForCylinder(2, 10.0f), 10.0f, 1e-3);

	// alpha-N / MAF: pass through (mass is not MAP-proportional there)
	engine->engineState.referenceMap = 80;
	engineConfiguration->fuelAlgorithm = engine_load_mode_e::LM_ALPHA_N;
	EXPECT_NEAR(refreshInjectionMassForCylinder(2, 10.0f), 10.0f, 1e-3);
}

// The hook off (the default) keeps the variant A refresh path:
// mass *= MapFast / injectionMassMapBasis.
TEST(EngineModules, MapInstantArm_refreshUsesWindowPathWhenHookOff) {
	EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
	engineConfiguration->fuelAlgorithm = engine_load_mode_e::LM_SPEED_DENSITY;
	engine->engineState.referenceMap = 80;
	engine->engineState.injectionMassMapBasis[2] = 80;
	Sensor::setMockValue(SensorType::MapFast, 96);
	EXPECT_NEAR(refreshInjectionMassForCylinder(2, 10.0f), 12.0f, 1e-3);
	Sensor::resetMockValue(SensorType::MapFast);
}

// ---- 200 Hz publish: MapFast = convert(raw latch) ----

TEST(EngineModules, MapInstantArm_onFastCallbackPublishesInstantMap) {
	EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
	MapInstantAtArmScope scope(true);
	engineConfiguration->isMapAveragingEnabled = true;
	setTestConverter();
	engine->outputChannels.rawMapFast = 3.0f; // -> 60 kPa

	engine->module<MapAveragingModule>()->onFastCallback();

	// read back via the sensor member (bypasses the registry)
	EXPECT_NEAR(getMapAvg(0).get().value_or(0), 60.0f, 1e-3);
	EXPECT_TRUE(engine->outputChannels.isMapValid);
	// one global value for every slot (shared plenum)
	EXPECT_NEAR(engine->outputChannels.mapPerCylinder[0], 60.0f, 1e-3);
	EXPECT_NEAR(engine->outputChannels.mapPerCylinder[3], 60.0f, 1e-3);

	// a bad raw does not publish: the old value stays (it expires via the
	// sensor timeout) and isMapValid goes false
	engine->outputChannels.rawMapFast = 60;
	engine->module<MapAveragingModule>()->onFastCallback();
	EXPECT_FALSE(engine->outputChannels.isMapValid);
	EXPECT_NEAR(getMapAvg(0).get().value_or(0), 60.0f, 1e-3);

	// after the 200 ms sensor timeout the stale value expires
	eth.moveTimeForwardMs(300);
	EXPECT_FALSE(getMapAvg(0).get().Valid);
}

// ---- the angle-window planner is off in variant B ----

TEST(EngineModules, MapInstantArm_scheduleNextCycleWindowsSkips) {
	EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
	MapInstantAtArmScope scope(true);
	engineConfiguration->isMapAveragingEnabled = true;
	engine->rpmCalculator.setRpmValue(200);
	getTriggerCentral()->triggerShape.tdcPosition = 0;
	engineConfiguration->globalTriggerAngleOffset = 0;

	engine->module<MapAveragingModule>()->onFastCallback();
	engine->module<MapAveragingModule>()->scheduleNextCycleWindows();

	EXPECT_EQ(0, engine->scheduler.size());
}

// ---- composition: 200 Hz tick (reference) then arm-time refresh ----

TEST(EngineModules, MapInstantArm_composition200HzTickThenArm) {
	EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
	MapInstantAtArmScope scope(true);
	engineConfiguration->fuelAlgorithm = engine_load_mode_e::LM_SPEED_DENSITY;
	engineConfiguration->isMapAveragingEnabled = true;
	setTestConverter();

	// steady-state 200 Hz tick: the pipeline computed the stored mass against
	// the value published by the PREVIOUS tick - here both are 80 kPa
	engine->outputChannels.rawMapFast = 4.0f; // -> 80 kPa
	engine->module<MapAveragingModule>()->onFastCallback();
	engine->engineState.referenceMap = getMapAvg(0).get().value_or(0);
	ASSERT_NEAR(engine->engineState.referenceMap, 80.0f, 1e-3);

	// arm time: the raw latch moved to 50 kPa -> mass rescaled by 50 / 80
	engine->outputChannels.rawMapFast = 2.5f;
	EXPECT_NEAR(refreshInjectionMassForCylinder(0, 10.0f), 6.25f, 1e-3);
}

// The tune gate still applies: isMapAveragingEnabled off means the hook alone
// does nothing.
TEST(EngineModules, MapInstantArm_tuneGateOffIsInert) {
	EngineTestHelper eth(engine_type_e::TEST_CRANK_ENGINE);
	MapInstantAtArmScope scope(true);
	engineConfiguration->isMapAveragingEnabled = false;
	engine->rpmCalculator.setRpmValue(200);
	setTestConverter();
	engine->outputChannels.rawMapFast = 3.0f;

	EXPECT_FALSE(isMapInstantAtArm());
	engine->module<MapAveragingModule>()->onFastCallback();
	EXPECT_EQ(0, engine->scheduler.size());
}
