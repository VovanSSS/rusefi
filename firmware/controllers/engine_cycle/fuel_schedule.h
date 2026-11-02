/**
 * @file fuel_schedule.h
 */

#pragma once

#include "global.h"
#include "efi_gpio.h"
#include "scheduler.h"
#include "fl_stack.h"
#include "trigger_structure.h"
#include "wall_fuel.h"

#define MAX_WIRES_COUNT 2

class InjectionEvent {
public:
	InjectionEvent() = default;

	bool update();

	// Call this every decoded trigger tooth.  It will schedule any relevant events for this injector.
	void onTriggerTooth(efitick_t nowNt, float currentPhase, float nextPhase, float nextNextPhase);

	// [tag:wall_fuel_telemetry] SD log field addresses must be constant expressions in production.
	constexpr WallFuel& getWallFuel() {
		return wallFuel;
	}

	void setIndex(uint8_t index) {
		ownIndex = index;
	}

	CylinderMask calculateInjectorOutputMask() const;
	uint8_t getIndex() const { return ownIndex; }

private:
	// Update the injection start angle
	bool updateInjectionAngle();

	// Compute the injection start angle, compensating for injection duration and injection phase settings.
	expected<float> computeInjectionAngle() const;

	/**
	 * This is a performance optimization for IM_SIMULTANEOUS fuel strategy.
	 * It's more efficient to handle all injectors together if that's the case
	 */
	bool isSimultaneous = false;
	uint8_t ownIndex = 0xF;
	uint8_t cylinderNumber = 0;
	injection_mode_e m_injectionMode = IM_SEQUENTIAL;

public:
	uint8_t getCylinderNumber() const { return cylinderNumber; }
private:

	WallFuel wallFuel{};

public:
	// TODO: this should be private
	InjectorOutputPin *outputs[MAX_WIRES_COUNT]{};
	InjectorOutputPin *outputsStage2[MAX_WIRES_COUNT]{};
	float injectionStartAngle = 0;

	// True once this cycle's injection start has been scheduled, together
	// with the absolute time it was armed for. A window match is suppressed
	// only while the armed target is still in the future (the one-tooth-ahead
	// arm covering the following tooth's window); once the target has passed,
	// a window match is the NEXT cycle's scheduling - at high duty the next
	// window can arrive before the injection end recomputes the angle, and
	// the pre-existing behavior schedules with the stale angle.
	bool injectionStartArmed = false;
	efitick_t injectionStartArmedAt = 0;

	// Injection close delay in NT ticks (= US2NT(durationUsStage1)), stored
	// by onTriggerTooth for use in turnInjectionPinHigh which arms the close
	// at the ACTUAL open moment: on the TMR2 angle clock CC4 (EFI_ANGLE_CLOCK,
	// 32-bit hardware compare) or the TIM5 CC2 mini-queue (time-based builds).
	uint32_t injectionEndDelayNt = 0;
};

#if !EFI_ANGLE_CLOCK
// Schedule injection close on TIM5 CC2 mini-queue (time-based builds only;
// the angle-clock build arms the close on TMR2 CC4).
// Called from turnInjectionPinHigh after output->open(). cyl = cylinder 0..3.
void scheduleInjectionCloseHW(int cyl, efitick_t nowNt, uint32_t delayNt, action_s action);
void cancelInjectionCloseHW(int cyl);
#endif // !EFI_ANGLE_CLOCK

void turnInjectionPinHigh(scheduler_arg_t arg);
void turnInjectionPinLow(InjectionEvent *event);

// [tag:injection_action_snapshot] Every action carries the output selection made
// when the pulse was scheduled. A mode change can rebuild InjectionEvent before
// either close action runs.
struct InjectorContext {
	scheduler_arg_t raw = 0;

	static constexpr CylinderMask OutputMask = (1u << MAX_CYLINDER_COUNT) - 1;
	static constexpr scheduler_arg_t EventIndexShift = MAX_CYLINDER_COUNT;
	static constexpr scheduler_arg_t Stage2Flag = 1u << (MAX_CYLINDER_COUNT + 4);

	constexpr InjectorContext(CylinderMask mask, uint8_t eventIndex, bool stage2Active)
		: raw((mask & OutputMask) | ((eventIndex & 0xF) << EventIndexShift) | (stage2Active ? Stage2Flag : 0)) {}

	constexpr explicit InjectorContext(scheduler_arg_t value) : raw(value) {}

	constexpr CylinderMask outputsMask() const { return raw & OutputMask; }
	constexpr uint8_t eventIndex() const { return (raw >> EventIndexShift) & 0xF; }
	constexpr bool stage2Active() const { return raw & Stage2Flag; }
};

static_assert(MAX_CYLINDER_COUNT <= sizeof(CylinderMask) * 8,
	"CylinderMask must have one bit per cylinder");
static_assert(MAX_CYLINDER_COUNT <= 12);

void startInjection(scheduler_arg_t arg);
void endInjection(scheduler_arg_t arg);
void endInjectionStage2(scheduler_arg_t arg);


/**
 * This class knows about when to inject fuel
 */
class FuelSchedule {
public:
	FuelSchedule();

	// Call this function if something happens that requires a rebuild, like a change to the trigger pattern
	void invalidate();

	// Call this every trigger tooth.  It will schedule all required injector events.
	void onTriggerTooth(efitick_t nowNt, float currentPhase, float nextPhase, float nextNextPhase);

	/**
	 * this method schedules all fuel events for an engine cycle
	 * Calculate injector opening angle, pins, and mode for all injectors
	 */
	void addFuelEvents();

	/**
	 * injection events, per cylinder
	 */
	InjectionEvent elements[MAX_CYLINDER_COUNT];
	bool isReady = false;
};

FuelSchedule * getFuelSchedule();
