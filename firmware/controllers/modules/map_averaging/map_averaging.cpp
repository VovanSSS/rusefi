/**
 * @file	map_averaging.cpp
 *
 * In order to have best MAP estimate possible, we real MAP value at a relatively high frequency
 * and average the value within a specified angle position window for each cylinder
 *
 * @date Dec 11, 2013
 * @author Andrey Belomutskiy, (c) 2012-2020
 *
 * This file is part of rusEfi - see http://rusefi.com
 *
 * rusEfi is free software; you can redistribute it and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * rusEfi is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include "pch.h"
#include "exp_average.h"


#if EFI_MAP_AVERAGING && defined (MODULE_MAP_AVERAGING)
#if !EFI_SHAFT_POSITION_INPUT
	fail("EFI_SHAFT_POSITION_INPUT required to have EFI_EMULATE_POSITION_SENSORS")
#endif // EFI_SHAFT_POSITION_INPUT

#include "map_averaging.h"
#include "trigger_central.h"
#include "engine_math.h"


/**
 * this instance does not have a real physical pin - it's only used for engine sniffer
 *
 * todo: we can kind of add real physical pin just for a very narrow case of troubleshooting but only if we ever need it :)
 */
static NamedOutputPin mapAveragingPin("map");

// allow smoothing up to number of cylinders
#define MAX_MAP_BUFFER_LENGTH (MAX_CYLINDER_COUNT)
// in MAP units, not voltage!
static float averagedMapRunningBuffer[MAX_MAP_BUFFER_LENGTH];
static int mapMinBufferLength = 0;
static int averagedMapBufIdx = 0;
// True once the MAP sensor converted a sample successfully: gates the
// C6899 warning so the startup init race does not alarm (see
// mapAveragingAdcCallback).
#if HAL_USE_ADC
static bool hasEverDecodedMap = false;
#endif // HAL_USE_ADC


static void endAveraging(MapAverager* arg);


void startAveraging(mapSampler* s) {
    if (!engine->engineState.mapAveragingDuration) {
        // Zero duration means the engine wasn't spinning or something, abort
        return;
    }
	efiAssertVoid(ObdCode::CUSTOM_ERR_6649, hasLotsOfRemainingStack(), "lowstck#9");

	// Variant A single window: every cylinder phase region feeds ONE averager.
	// The window is attributed to the cylinder whose region it samples - the
	// PREVIOUS cylinder, relative to the next injection - and its close feeds
	// the firing-order successor's mass (see endAveraging).
	auto& averager = getMapAvg(0);
	averager.start(s->cylinderNumber);

	mapAveragingPin.setHigh();
	engine->outputChannels.isMapAveraging = true;

	scheduleByAngle(&s->timer, getTimeNowNt(), engine->engineState.mapAveragingDuration,
		action_s::make<endAveraging>(&averager));
}

void MapAverager::start(uint8_t cylinderNumber) {
	chibios_rt::CriticalSectionLocker csl;

	m_counter = 0;
	m_sum = 0;
	m_isAveraging = true;
	m_cylinderNumber = cylinderNumber;
}

SensorResult MapAverager::submit(float volts) {
	auto result = m_function ? m_function->convert(volts) : unexpected;

	if (m_isAveraging && result) {
		chibios_rt::CriticalSectionLocker csl;

		m_counter++;
		m_sum += result.Value;
	}

	return result;
}

static ExpAverage expAverage;

// huh? why is this killing unit tests _linking_ only on WINDOWS?! PUBLIC_API_WEAK
float filterMapValue(float value) {
  expAverage.setSmoothingFactor(engineConfiguration->mapExpAverageAlpha);
  return expAverage.initOrAverage(value);
}

float MapAverager::stop() {
	chibios_rt::CriticalSectionLocker csl;

	m_isAveraging = false;

	if (m_counter > 0) {
		float averageMap = m_sum / m_counter;
		m_lastCounter = m_counter;

		// TODO: this should be per-sensor, not one for all MAP sensors
		averagedMapRunningBuffer[averagedMapBufIdx] = averageMap;
		// increment circular running buffer index
		averagedMapBufIdx = (averagedMapBufIdx + 1) % mapMinBufferLength;
		// find min. value (only works for pressure values, not raw voltages!)
		float minPressure = averagedMapRunningBuffer[0];
		for (int i = 1; i < mapMinBufferLength; i++) {
			if (averagedMapRunningBuffer[i] < minPressure)
				minPressure = averagedMapRunningBuffer[i];
		}

		engine->outputChannels.mapPerCylinder[m_cylinderNumber] = minPressure;
		setValidValue(filterMapValue(minPressure), getTimeNowNt());

		return minPressure;
	} else {
#if EFI_PROD_CODE
		warning(ObdCode::CUSTOM_UNEXPECTED_MAP_VALUE, "No MAP values to average");
#endif
	}

	return 0;
}

#if HAL_USE_ADC

/**
 * This method is invoked from ADC callback.
 * @note This method is invoked OFTEN, this method is a potential bottleneck - the implementation should be
 * as fast as possible
 */
void mapAveragingAdcCallback(float instantVoltage) {
	efiAssertVoid(ObdCode::CUSTOM_ERR_6650, hasLotsOfRemainingStack(), "lowstck#9a");

	SensorResult mapResult = getMapAvg(0).submit(instantVoltage);

	if (!mapResult) {
		// The very first fast-ADC samples can arrive before the MAP sensor
		// configuration (curve points, divider) is applied at boot, so their
		// conversion fails and would raise a misleading one-shot C6899 right
		// after power-on. Suppress the warning until the sensor has proven it
		// can convert at least once - a genuinely broken sensor still gets
		// reported on every sample after that first success (and by the
		// sensor checker).
		if (hasEverDecodedMap) {
			// hopefully this warning is not too much CPU consumption for fast ADC callback
			warning(ObdCode::CUSTOM_INSTANT_MAP_DECODING, "Invalid MAP at %f", instantVoltage);
		}
		engine->outputChannels.isMapValid = false;
	} else {
		hasEverDecodedMap = true;
		engine->outputChannels.isMapValid = true;
	}

#if EFI_TUNER_STUDIO
	float instantMap = mapResult.value_or(0);
	engine->outputChannels.instantMAPValue = instantMap;
#endif // EFI_TUNER_STUDIO
}
#endif

// Scale the stored injectionMass of one cylinder against a fresh MAP window
// value, relative to the basis the stored mass was last based on. The basis
// bookkeeping keeps this composable with the 200 Hz pipeline (which rewrites
// injectionMass[c] in reference units every tick): any interleaving of
// ticks and window closes applies each window value exactly once - MAP can
// never enter the mass twice (the otherwise-systematic double-application of
// the single shared window).
static void rescaleInjectionMass(uint8_t cylinderNumber, float freshMap) {
	if (engineConfiguration->fuelAlgorithm != engine_load_mode_e::LM_SPEED_DENSITY) {
		return;
	}

	if (freshMap <= 0) {
		return;
	}

	float basis = engine->engineState.injectionMassMapBasis[cylinderNumber];
	if (basis <= 0) {
		return;
	}

	engine->engineState.injectionMass[cylinderNumber] *= freshMap / basis;
	engine->engineState.injectionMassMapBasis[cylinderNumber] = freshMap;
}

static void endAveraging(MapAverager* arg) {
	float freshWindowMap = arg->stop();

	// Variant A: the just-closed window fixed the freshest pressure. The window
	// belongs to the previous cylinder (arg->getCylinderNumber()); recompute the
	// mass of the FIRING-ORDER SUCCESSOR - the next cylinder whose injection is
	// armed - from it. Runs on the TIM5 executor (prio 5): zero handoff cost.
	// The chain is continuous across the cycle boundary: the last window of a
	// cycle feeds the first cylinder of the next one.
	rescaleInjectionMass(MapAveragingModule::firingOrderSuccessor(arg->getCylinderNumber()), freshWindowMap);

	engine->outputChannels.isMapAveraging = false;
	mapAveragingPin.setLow();
}

float refreshInjectionMassForCylinder(uint8_t cylinderNumber, float mass) {
	if (engineConfiguration->fuelAlgorithm != engine_load_mode_e::LM_SPEED_DENSITY) {
		return mass;
	}

	// The ONE window: MapFast holds the freshest closed window value (the
	// previous cylinder's average per the 180-degree update chain) and
	// injectionMassMapBasis[cyl] is the MAP value the stored mass was last
	// based on - referenceMap after the 200 Hz refresh, the just-closed window
	// value after endAveraging rescaled this cylinder.
	const float freshMap = Sensor::getOrZero(SensorType::MapFast);
	const float basis = engine->engineState.injectionMassMapBasis[cylinderNumber];

	if (freshMap > 0 && basis > 0) {
		mass *= freshMap / basis;
	}

	return mass;
}

uint8_t MapAveragingModule::firingOrderSuccessor(uint8_t cylinderNumber) {
	size_t count = engineConfiguration->cylindersCount;
	if (count <= 1) {
		return cylinderNumber;
	}

	angle_t tdc = engine->cylinders[cylinderNumber].getAngleOffset();

	uint8_t best = cylinderNumber;
	float bestDelta = 720.0f + 1.0f;

	for (size_t i = 0; i < count; i++) {
		if (i == cylinderNumber) {
			continue;
		}

		float delta = engine->cylinders[i].getAngleOffset() - tdc;
		if (delta <= 0) {
			delta += 720.0f;
		}

		if (delta < bestDelta) {
			bestDelta = delta;
			best = i;
		}
	}

	return best;
}

static void applyMapMinBufferLength() {
	// check range
	mapMinBufferLength = maxI(minI(engineConfiguration->mapMinBufferLength, MAX_MAP_BUFFER_LENGTH), 1);
	// reset index
	averagedMapBufIdx = 0;
	// fill with maximum values
	for (int i = 0; i < mapMinBufferLength; i++) {
		averagedMapRunningBuffer[i] = FLT_MAX;
	}
}

void MapAveragingModule::onFastCallback() {
	float rpm = Sensor::getOrZero(SensorType::Rpm);
    MAP_sensor_config_s * c = &engineConfiguration->map;
    angle_t start = interpolate2d(rpm, c->samplingAngleBins, c->samplingAngle);
    efiAssertVoid(ObdCode::CUSTOM_ERR_MAP_START_ASSERT, !std::isnan(start), "start");

	for (size_t i = 0; i < engineConfiguration->cylindersCount; i++) {
        float cylinderStart = start + engine->cylinders[i].getAngleOffset();
        wrapAngle(cylinderStart, "cylinderStart", ObdCode::CUSTOM_ERR_6562);
        engine->engineState.mapAveragingStart[i] = cylinderStart;
    }

    angle_t duration = interpolate2d(rpm, c->samplingWindowBins, c->samplingWindow);
    assertAngleRange(duration, "samplingDuration", ObdCode::CUSTOM_ERR_6563);

    // Clamp the duration to slightly less than one cylinder period
    float cylinderPeriod = engine->engineState.engineCycle / engineConfiguration->cylindersCount;
    engine->engineState.mapAveragingDuration = clampF(10, duration, cylinderPeriod - 10);
}

void MapAveragingModule::scheduleNextCycleWindows() {
	if (!engineConfiguration->isMapAveragingEnabled) {
		return;
	}

	float rpm = Sensor::getOrZero(SensorType::Rpm);
	if (rpm == 0) {
		return;
	}

	// Runs from the TDC executor hook once per engine cycle (the hook fires at
	// engine phase tdcPosition()): plan every window start of the coming cycle
	// in time. The angle->time conversion happens up to a full cycle ahead with
	// the current oneDegreeUs - the documented option-B accel-drift tradeoff -
	// and the events execute on the TIM5 executor. Zero per-tooth handoff work
	// (this is what replaced the per-tooth onEnginePhase window scan).
	efitick_t nowNt = getTimeNowNt();
	angle_t tdcPhase = tdcPosition();

	int samplingCount = engineConfiguration->measureMapOnlyInOneCylinder ? 1 : engineConfiguration->cylindersCount;

	for (int i = 0; i < samplingCount; i++) {
		angle_t windowOffset = engine->engineState.mapAveragingStart[i] - tdcPhase;
		wrapAngle(windowOffset, "mapWindowOffset", ObdCode::CUSTOM_ERR_6562);

		// Re-planning (a new cycle) can overtake events still pending from the
		// previous plan under hard acceleration: cancel before re-arming the slot.
		engine->scheduler.cancel(&samplers[i].timer);

		scheduleByAngle(&samplers[i].timer, nowNt, windowOffset, action_s::make<startAveraging>(&samplers[i]));
	}
}

void MapAveragingModule::onConfigurationChange(engine_configuration_s const * previousConfig) {
	if (!previousConfig || engineConfiguration->mapMinBufferLength != previousConfig->mapMinBufferLength) {
		applyMapMinBufferLength();
	}
}

void MapAveragingModule::init() {
	  for (size_t cylinderIndex = 0; cylinderIndex < MAX_CYLINDER_COUNT; cylinderIndex++) {
  		samplers[cylinderIndex].cylinderNumber = cylinderIndex;
	  }

	if (engineConfiguration->isMapAveragingEnabled) {
		efiPrintf("initMapAveraging...");
		applyMapMinBufferLength();
	} else {
		efiPrintf("Running without MapAveraging...");
	}
}

#else
void MapAveragingModule::onFastCallback(){}
void MapAveragingModule::onConfigurationChange(engine_configuration_s const *){}
void MapAveragingModule::init() {}
void MapAveragingModule::scheduleNextCycleWindows() {}
uint8_t MapAveragingModule::firingOrderSuccessor(uint8_t cylinderNumber) { return cylinderNumber; }
#endif /* EFI_MAP_AVERAGING */
