/**
 * @file	map_averaging.h
 *
 * @date Dec 11, 2013
 * @author Andrey Belomutskiy, (c) 2012-2020
 */

#pragma once
#include "engine_module.h"
#include "stored_value_sensor.h"
#include "sensor_converter_func.h"
#include "scheduler.h"

/**
* A single scheduling slot for one MAP sampling window: the ONE window (single
* averager, see Variant A 2026-09-23) passes through the cylinder phase regions
* in firing order - up to 4 starts are planned per engine cycle from the TDC
* executor hook (MapAveragingModule::scheduleNextCycleWindows), one window open
* at a time.
*
* The one-shot startAveraging event node is reused serially for the window's
* endAveraging follow-up (the dwellStartTimer pattern): by the time either event
* runs, its executor queue node has been popped.
*/
struct mapSampler {
	scheduling_s timer;
	uint8_t cylinderNumber;
};

#if EFI_MAP_AVERAGING

#if HAL_USE_ADC
void mapAveragingAdcCallback(float instantVoltage, bool channelReady);
#endif

// allow smoothing up to number of cylinders
#define MAX_MAP_BUFFER_LENGTH (MAX_CYLINDER_COUNT)
#endif /* EFI_MAP_AVERAGING */

class MapAverager : public StoredValueSensor {
public:
	MapAverager(SensorType type, efidur_t timeout)
		: StoredValueSensor(type, timeout)
	{
	}

	void start(uint8_t cylinderNumber);

	/**
	 * @return the fresh window-averaged MAP value (min-of-running-buffer), or 0
	 * when the window collected no samples
	 */
	float stop();

	uint8_t getCylinderNumber() const {
		return m_cylinderNumber;
	}

	SensorResult submit(float sensorVolts);
	void onAdcSample(float instantVoltage, bool channelReady);

	/**
	 * Variant B: read-only conversion of the raw fast-ADC latch - no stored
	 * state is written, so this is safe to call from the handoff (the handoff
	 * must never publish into MapFast: the 200 Hz tick owns that).
	 */
	SensorResult convertRaw(float volts) const {
		return m_function ? m_function->convert(volts) : unexpected;
	}

	/**
	 * Variant B: convert the raw fast-ADC latch and publish it as the current
	 * MapFast value (no angle window). On a failed conversion the previous
	 * published value is kept (it expires via the sensor timeout).
	 */
	SensorResult publishInstant(float volts);

	void setFunction(SensorConverter& func) {
		m_function = &func;
	}

	void showInfo(const char* sensorName) const override;

private:
	SensorConverter* m_function = nullptr;

	bool m_isAveraging = false;
	size_t m_counter = 0;
	size_t m_lastCounter = 0;
	float m_sum = 0;
	uint8_t m_cylinderNumber = 0;

	// Set once the MAP sensor has produced at least one successful conversion:
	// suppresses the one-shot C6899 that the first boot-time samples otherwise
	// raise before the curve/divider config is applied.
	bool hasEverDecodedMap = false;
};

// The ONE window averager: every cylinder phase region feeds this instance
// (single-window Variant A). The index parameter is kept for API compatibility.
MapAverager& getMapAvg(size_t idx);
float filterMapValue(float value);
void startAveraging(struct mapSampler* s);

class MapAveragingModule : public EngineModule {
public:
	void onConfigurationChange(engine_configuration_s const * previousConfig) override;

	void onFastCallback() override;

	// Once-per-engine-cycle planner, called from the TDC executor hook: schedules
	// the sampling window starts of the coming cycle in time (scheduleByAngle).
	// Replaces the per-tooth onEnginePhase window scan - zero per-tooth handoff
	// work. Each window is attributed to its cylinder, and the value it produces
	// belongs to that cylinder ("the window is filled by the previous cylinder").
	void scheduleNextCycleWindows();

	// The cylinder whose phase region follows `cylinderNumber` in firing order
	// (minimal positive angle offset). The just-closed window of `cylinderNumber`
	// feeds this cylinder's injection mass; wraps across the cycle boundary.
	static uint8_t firingOrderSuccessor(uint8_t cylinderNumber);

	void triggerCallback(uint32_t index, efitick_t edgeTimestamp);
	void init();
	void submitSample(float voltsMap1, float voltsMap2);
	mapSampler samplers[MAX_CYLINDER_COUNT];
};

#if EFI_MAP_AVERAGING && defined(MODULE_MAP_AVERAGING)
// Variant B (instant-at-arm) selector: the tune gate AND the board hook
// (custom_board_mapInstantAtArm, m74_9 - shared plenum) both hold. True means:
// MapFast is published once per 200 Hz tick as the instant conversion of the
// raw fast-ADC latch (no angle windows, no planner/executor events), and the
// mass refresh below rescales each cylinder by the raw latch converted at its
// own arm time: mass *= convert(rawMapFast) / referenceMap.
bool isMapInstantAtArm();

// Per-cylinder mass refresh, consumed at injection scheduling time in the
// handoff.
// Variant A (windows): mass *= MapFast / injectionMassMapBasis[cyl]. MapFast
// holds the freshest closed window value; the basis equals referenceMap after
// the 200 Hz pipeline refresh and the just-closed window value after
// endAveraging rescaled the firing-order successor - so the multiply stays
// composable and MAP can never enter the mass twice.
// Variant B (instant-at-arm): mass *= convert(rawMapFast now) /
// referenceMap - phase-locked, one converter call + one multiply, composable
// with the 200 Hz tick because mass and referenceMap share the value
// published by the PREVIOUS tick.
// Returns `mass` unchanged for non-SD modes.
float refreshInjectionMassForCylinder(uint8_t cylinderNumber, float mass);
#else
static inline bool isMapInstantAtArm() {
	return false;
}
static inline float refreshInjectionMassForCylinder(uint8_t, float mass) {
	return mass;
}
#endif
