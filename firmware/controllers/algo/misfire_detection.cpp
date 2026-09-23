/**
 * @file misfire_detection.cpp
 * @brief Misfire detection.
 *
 * Detects combustion misfires by tracking per-cycle crankshaft acceleration and
 * comparing it against an adaptive, noise-aware (EMA + wobble) threshold. Detected
 * misfires are reported to the malfunction-indicator subsystem.
 */

#include "pch.h"
#include "misfire_detection.h"

#if EFI_ENGINE_CONTROL && EFI_MISFIRE_DETECTION

#include "idle_thread.h"
#include "malfunction_central.h"
#include "efilib.h"
#include "engine_math.h"
#include "rpm_calculator.h"
#include "trigger_central.h"

// Fallbacks when config fields are zero or out of range.
static constexpr float MISFIRE_EMA_ALPHA_RISE_DEFAULT    = 0.05f;
static constexpr float MISFIRE_EMA_ALPHA_FALL_DEFAULT    = 0.005f;
static constexpr float MISFIRE_K_DEFAULT                 = 3.0f;
static constexpr float MISFIRE_WOBBLE_ALPHA_RISE_DEFAULT = 0.1f;
static constexpr float MISFIRE_WOBBLE_ALPHA_FALL_DEFAULT = 0.01f;
// Minimum wobble floor (µs): prevents the threshold from collapsing on zero-variation signals
// (e.g. bench tests with perfectly uniform synthetic teeth). With K=3, 50µs gives a 150µs
// minimum margin -- roughly 1° at 1500 RPM, comfortably below any real misfire signature.
static constexpr float MISFIRE_WOBBLE_FLOOR_US           = 50.0f;

namespace {
// Executor actions for the per-cylinder segment boundary events.
void misfireSegmentStartAction(uint8_t cylinderNumber) {
	engine->module<MisfireController>()->captureSegmentStart(cylinderNumber, getTimeNowNt());
}

void misfireSegmentEndAction(uint8_t cylinderNumber) {
	engine->module<MisfireController>()->finishSegment(cylinderNumber, getTimeNowNt());
}
} // namespace

void MisfireController::resetDetectionState() {
	for (size_t i = 0; i < MAX_CYLINDER_COUNT; i++) {
		m_segActive[i] = false;
	}
	m_emaSeeded = false;
	m_emaSeg = 0;
	m_wobbleSeeded = false;
	m_emaWobble = 0;
	m_settleCount = 0;
	m_windowHead = 0;
	m_windowFill = 0;
	m_windowTooNarrow = false;
}

bool MisfireController::isSettled() const {
	uint16_t settleCycles = engineConfiguration->misfireSettleCycles;
	return (settleCycles == 0) || (m_settleCount >= settleCycles);
}

void MisfireController::onEngineStop() {
	// Prepare for the next start: drop the baseline, rate-test window and segment timers.
	// misfireTotalCount and the MIL latch persist until power cycle.
	resetDetectionState();
}

void MisfireController::captureSegmentStart(uint8_t cylinderNumber, efitick_t nowNt) {
	// The start of cylinder N's expansion-stroke segment (fired on the executor
	// at the segment's scheduled start angle).
	m_segStart[cylinderNumber] = nowNt;
	m_segActive[cylinderNumber] = true;
}

void MisfireController::finishSegment(uint8_t cylinderNumber, efitick_t nowNt) {
	// Evaluate the segment of the cylinder that ALREADY fired: its expansion
	// window just closed. Segment time is executor-based (both boundaries fire
	// on the TIM5 executor) - the documented precision tradeoff of moving the
	// detector out of the handoff.
	if (!m_segActive[cylinderNumber]) {
		return;
	}

	m_segActive[cylinderNumber] = false;
	float segUs = NT2US((float)(nowNt - m_segStart[cylinderNumber]));
	evaluateSegment(segUs);
}

void MisfireController::recordFiring(bool flagged) {
	m_window[m_windowHead] = flagged;
	m_windowHead = (m_windowHead + 1) % MISFIRE_WINDOW_MAX;
	if (m_windowFill < MISFIRE_WINDOW_MAX) {
		m_windowFill++;
	}
}

uint8_t MisfireController::flaggedInWindow(uint8_t windowSize) const {
	uint8_t n = windowSize < m_windowFill ? windowSize : m_windowFill;
	uint8_t flagged = 0;
	uint8_t idx = m_windowHead; // one past the newest entry
	for (uint8_t k = 0; k < n; k++) {
		idx = (idx == 0) ? (MISFIRE_WINDOW_MAX - 1) : (idx - 1);
		if (m_window[idx]) {
			flagged++;
		}
	}
	return flagged;
}

void MisfireController::registerMisfire() {
	misfireTotalCount++;

	uint16_t threshold = engineConfiguration->misfireCountThreshold;
	if (!misfireLatched && threshold > 0 && misfireTotalCount >= threshold) {
		misfireLatched = true;
		// Engine-wide detection => generic random/multiple-cylinder misfire code (P0300).
		warning(ObdCode::OBD_Random_Misfire, "Misfire detected");
	}
}

void MisfireController::evaluateSegment(float segDurationUs) {
	if (segDurationUs <= 0) {
		// Degenerate timing (phase wrap / first sample) — ignore.
		return;
	}

	if (!m_emaSeeded) {
		// Warm-up: seed the shared baseline with the first valid segment, don't judge it.
		m_emaSeg = segDurationUs;
		m_emaSeeded = true;
		return;
	}

	// Adaptive wobble-based threshold: baseline + K * wobble.
	// Flagging is suppressed until both the wobble is seeded and the settle period has elapsed.
	// During settle the baseline and wobble still update so they are warm when detection starts.
	float k = engineConfiguration->misfireK;
	if (k <= 0.0f) {
		k = MISFIRE_K_DEFAULT;
	}
	float kWobble = k * m_emaWobble;
	bool settled = isSettled();
	float thresh = (settled && m_wobbleSeeded) ? (m_emaSeg + kWobble) : 0.0f;
	bool flagged = settled && m_wobbleSeeded && (segDurationUs > thresh);

	// Update live data so the log always shows current segment vs. baseline/threshold/wobble.
	misfireLastSegUs      = segDurationUs;
	misfireEmaUs          = m_emaSeg;
	misfireThreshUs       = thresh;
	misfireWobbleThreshUs = (settled && m_wobbleSeeded) ? kWobble : 0.0f;

	// Slot this firing into the engine-wide rate-test window.
	recordFiring(flagged);

	if (flagged) {
		// "N of last M": a flagged firing only counts once at least misfireConsecutiveCount
		// flagged firings (any cylinder) fall within the last misfireWindowFirings firings.
		// Baseline and wobble are frozen on flagged firings so they cannot drift upward.
		uint8_t windowSize = engineConfiguration->misfireWindowFirings;
		if (windowSize < 1) {
			windowSize = 1;
		}
		if (windowSize > MISFIRE_WINDOW_MAX) {
			windowSize = MISFIRE_WINDOW_MAX;
		}
		uint8_t need = engineConfiguration->misfireConsecutiveCount;
		if (need < 1) {
			need = 1;
		}
		if (flaggedInWindow(windowSize) >= need) {
			registerMisfire();
		}
	} else {
		// Advance the settle counter while we are still in the settle period.
		if (!settled && m_settleCount < 65535u) {
			m_settleCount++;
		}

		// Clean firing: compute deviation from current baseline before updating it, so both
		// the baseline and wobble use the same reference point for this firing.
		float deviation = fabsf(segDurationUs - m_emaSeg);

		// Update baseline EMA (asymmetric alpha).
		// Positive delta (segment slower = engine decelerating): higher alpha so genuine RPM
		// drops are tracked quickly and don't cause false positives.
		// Negative delta (segment faster = recovering): lower alpha resists upward drift from
		// borderline-slow clean firings that occur between misfires.
		float diff = segDurationUs - m_emaSeg;
		float alpha;
		if (diff > 0.0f) {
			alpha = engineConfiguration->misfireEmaAlphaDecel;
			if (alpha <= 0.0f || alpha > 1.0f) {
				alpha = MISFIRE_EMA_ALPHA_RISE_DEFAULT;
			}
		} else {
			alpha = engineConfiguration->misfireEmaAlphaAccel;
			if (alpha <= 0.0f || alpha > 1.0f) {
				alpha = MISFIRE_EMA_ALPHA_FALL_DEFAULT;
			}
		}
		m_emaSeg += alpha * diff;

		// Update wobble EMA (asymmetric alpha).
		// Rising wobble (engine getting rougher): track fast so the threshold rises quickly
		// and doesn't false-trip during a load step, cam lope onset, etc.
		// Falling wobble (engine smoothing out): relax slowly — stay conservative.
		// A floor prevents the threshold from collapsing to zero on zero-variation signals.
		if (!m_wobbleSeeded) {
			m_emaWobble = std::max(deviation, MISFIRE_WOBBLE_FLOOR_US);
			m_wobbleSeeded = true;
		} else {
			float wobbleDiff = deviation - m_emaWobble;
			float wobbleAlpha;
			if (wobbleDiff > 0.0f) {
				wobbleAlpha = engineConfiguration->misfireWobbleAlphaRise;
				if (wobbleAlpha <= 0.0f || wobbleAlpha > 1.0f) {
					wobbleAlpha = MISFIRE_WOBBLE_ALPHA_RISE_DEFAULT;
				}
			} else {
				wobbleAlpha = engineConfiguration->misfireWobbleAlphaFall;
				if (wobbleAlpha <= 0.0f || wobbleAlpha > 1.0f) {
					wobbleAlpha = MISFIRE_WOBBLE_ALPHA_FALL_DEFAULT;
				}
			}
			m_emaWobble += wobbleAlpha * wobbleDiff;
			m_emaWobble = std::max(m_emaWobble, MISFIRE_WOBBLE_FLOOR_US);
		}

		misfireWobbleUs = m_emaWobble;
	}
}

void MisfireController::scheduleNextCycleSegments() {
	bool enabled = engineConfiguration->misfireDetectionEnabled;
#if EFI_IDLE_CONTROL
	bool idle = engine->module<IdleController>()->isIdling;
#else
	bool idle = true;
#endif
	bool monitoring = enabled && idle;

	misfireDetectionActive = monitoring && m_emaSeeded && isSettled();

	if (!monitoring) {
		// Drop the boundary events still pending from the last plan: a segment
		// must not fire (and evaluate) after leaving idle.
		for (size_t i = 0; i < MAX_CYLINDER_COUNT; i++) {
			engine->scheduler.cancel(&m_segStartEvent[i]);
			engine->scheduler.cancel(&m_segEndEvent[i]);
		}

		// Reset transient detection state once when leaving idle so a later re-idle
		// re-seeds baselines instead of judging against stale norms.
		if (m_wasMonitoring) {
			resetDetectionState();
		}
		m_wasMonitoring = false;
		return;
	}
	m_wasMonitoring = true;

	float windowStartOffset = engineConfiguration->misfireWindowStart;
	float windowEndOffset = engineConfiguration->misfireWindowEnd;

	// The segment is timed between the trigger edges that bracket windowStart and windowEnd,
	// so the crank only provides a real velocity sample at each tooth. If the whole window
	// fits inside one tooth interval, both ends land on the same edge and segUs collapses to
	// zero — evaluateSegment() then bails on every firing and all live data freezes. Refuse to
	// run (and tell the user) when the window is narrower than the average tooth spacing rather
	// than measuring silently-degenerate segments. getLength() is events per engine cycle, so
	// engineCycle / getLength() is the mean tooth pitch in the same degree space as the offsets.
	float windowWidth = windowEndOffset - windowStartOffset;
	size_t triggerEvents = getTriggerCentral()->triggerShape.getLength();
	float toothSpacing = (triggerEvents > 0) ? (getEngineState()->engineCycle / triggerEvents) : 0.0f;
	if (windowWidth < toothSpacing) {
		misfireDetectionActive = false;
		if (!m_windowTooNarrow) {
			m_windowTooNarrow = true;
			configError("misfire window %.0f deg too narrow for trigger, need >= %.0f deg",
						windowWidth, toothSpacing);
		}
		return;
	}
	m_windowTooNarrow = false;

	float rpm = Sensor::getOrZero(SensorType::Rpm);
	if (rpm == 0) {
		return;
	}

	// Schedule the segment events relative to the TDC mark (engine phase =
	// tdcPosition()) once per engine cycle, like the MAP windows. Both boundary
	// events fire on the TIM5 executor - the segment is timed between executor
	// fire times, and each end event evaluates the segment of the cylinder that
	// already fired.
	efitick_t nowNt = getTimeNowNt();
	angle_t tdcPhase = tdcPosition();
	size_t cylCount = engineConfiguration->cylindersCount;

	for (size_t i = 0; i < cylCount; i++) {
		// getAngleOffset() gives each cylinder's TDC reference within the engine cycle
		// (handles firing order + odd-fire). The window sits in the expansion stroke.
		angle_t tdc = engine->cylinders[i].getAngleOffset();

		angle_t windowStart = tdc + windowStartOffset;
		wrapAngle(windowStart, "misfireStart", ObdCode::CUSTOM_ERR_6562);
		angle_t windowEnd = tdc + windowEndOffset;
		wrapAngle(windowEnd, "misfireEnd", ObdCode::CUSTOM_ERR_6562);

		angle_t startOffset = windowStart - tdcPhase;
		wrapAngle(startOffset, "misfireStartOffset", ObdCode::CUSTOM_ERR_6562);
		angle_t segmentWidth = windowEnd - windowStart;
		if (segmentWidth < 0) {
			segmentWidth += getEngineState()->engineCycle;
		}
		angle_t endOffset = startOffset + segmentWidth;

		// Re-planning (a new cycle) can overtake events still pending from the
		// previous plan under hard acceleration: cancel before re-arming the slot.
		engine->scheduler.cancel(&m_segStartEvent[i]);
		engine->scheduler.cancel(&m_segEndEvent[i]);

		scheduleByAngle(&m_segStartEvent[i], nowNt, startOffset, action_s::make<misfireSegmentStartAction>(static_cast<uint8_t>(i)));
		scheduleByAngle(&m_segEndEvent[i], nowNt, endOffset, action_s::make<misfireSegmentEndAction>(static_cast<uint8_t>(i)));
	}
}

#else // !EFI_MISFIRE_DETECTION

void MisfireController::scheduleNextCycleSegments() {}
void MisfireController::captureSegmentStart(uint8_t, efitick_t) {}
void MisfireController::finishSegment(uint8_t, efitick_t) {}
void MisfireController::onEngineStop() {}
void MisfireController::evaluateSegment(float) {}
void MisfireController::registerMisfire() {}
void MisfireController::resetDetectionState() {}
void MisfireController::recordFiring(bool) {}
uint8_t MisfireController::flaggedInWindow(uint8_t) const { return 0; }

#endif // EFI_MISFIRE_DETECTION
