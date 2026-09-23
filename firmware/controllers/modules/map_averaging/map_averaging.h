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
void mapAveragingAdcCallback(float instantVoltage);
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
// Variant A single-multiply mass refresh, consumed at injection scheduling time
// in the handoff: mass *= MapFast / injectionMassMapBasis[cyl]. MapFast holds
// the freshest closed window value; the basis equals referenceMap after the
// 200 Hz pipeline refresh and the just-closed window value after endAveraging
// rescaled the firing-order successor - so the multiply stays composable and
// MAP can never enter the mass twice. Returns `mass` unchanged for non-SD modes.
float refreshInjectionMassForCylinder(uint8_t cylinderNumber, float mass);
#else
static inline float refreshInjectionMassForCylinder(uint8_t, float mass) {
	return mass;
}
#endif
