/**
 * @file    m74_9_vehicle_can.cpp
 *
 * Vehicle CAN for the m74_9 board: dashboard transmit + vehicle-speed
 * receive on CAN1 (500 kbit/s, G0 RX / G1 TX). Ported from the fw-m74.9
 * board repo (vehicle_can_tx.cpp / vehicle_can.cpp), merged with the
 * board's existing car-derived Largus encodings.
 *
 * The transmitter runs once per 5 ms CAN worker tick via the
 * custom_board_update_dash hook. Both profiles share the tick scheduler
 * and the frame tables; live fields are applied per frame ID.
 *
 * What was kept from the original m74_9_can.cpp stream (the profile
 * implementation does not carry these):
 *  - 0x186[0:1] multiplexed by the dash's own gate (the X throttle byte):
 *    fuel flow mL/h x16 at closed throttle (X == 0 - the dash BC L/h
 *    display reads value/16, recomputed constantly, DFCO-zeroed) and live
 *    rpm x8 while the throttle is open (X > 0 - the dash tach source,
 *    car-verified at exactly 1/8 slope)
 *  - live rpm x16 in 0x189[0:1] / 0x186[2:3] / 0x18A[0:1]
 *  - tach cross-check byte 0x186[4], throttle-X 0x186[5] with the
 *    closed-stop offset subtracted (X == 0 at a shut blade - the stock
 *    behavior; a nonzero closed-throttle X flips the dash into the rpm
 *    mode at idle and the needle starts reading the flow as rpm)
 *  - battery voltage 0x189[2:3] + 0x1F6[3] (volts x3.75, fitted from the
 *    stock captures), 0x189[4] rolling freshness counter,
 *    0x189[5] = 0xB9/0xBA
 *  - advance byte 0x18A[4], angle accumulator 0x18A[2:3]
 *  - fuel-gated engine-time counter 0x66A[3:4], 0x41A[1]/0x6FD[3] state
 *  - ignition gating (silence at key-off - the BCM drops the main relay
 *    on the 0x189 heartbeat timeout) and serial-session throttling
 *  - IMMO (0x713/0x714) stays in m74_9_can.cpp
 *
 * What the fw-m74.9 profiles add:
 *  - ladaCanbusProfile selector (Disabled / Largus / Niva / Granta)
 *  - indicated vehicle speed in 0x217[3:4] from the bus speed source
 *    (0x29A Largus with checksum+counter / 0x28C Niva/Granta)
 *  - CLT+IAT temperature encodings (0x5DA[0] = CLT+40, 0x65C[0] =
 *    (IAT+40)x2) with frame suppression on invalid sensors
 *  - idle target 0x5DA[1] and hard RPM limit 0x5DA[4]
 *  - the full Niva / Granta profile (0x180 rpm x8, 0x551 CLT+40, counters)
 *
 * @date    2026-10-08
 */

#include "pch.h"

#if EFI_CAN_SUPPORT

#include "m74_9_vehicle_can.h"
#include "can_msg_tx.h"
#include "ignition_controller.h"

// ---------------------------------------------------------------------------
// Vehicle speed receive
// ---------------------------------------------------------------------------

M749VehicleSpeedSensor::M749VehicleSpeedSensor()
    : CanSensorBase(0x29A, SensorType::VehicleSpeed, MS2NT(100)) {
}

bool M749VehicleSpeedSensor::acceptFrame(size_t busIndex, const CANRxFrame& frame) const {
	if (!useM749VehicleSpeed() || busIndex != 0 || CAN_ISX(frame) || CAN_ISRTR(frame)) {
		return false;
	}
	switch (config->ladaCanbusProfile) {
	case LadaCanbusProfile::Largus:
		return CAN_SID(frame) == 0x29A && frame.DLC == 8;
	case LadaCanbusProfile::NivaGranta:
		return CAN_SID(frame) == 0x28C && frame.DLC == 2;
	default:
		return false;
	}
}

void M749VehicleSpeedSensor::decodeFrame(const CANRxFrame& frame, efitick_t nowNt) {
	unsigned offset = 0;
	if (config->ladaCanbusProfile == LadaCanbusProfile::Largus) {
		// Checksum = one's complement of the modulo-256 sum of bytes 0..6.
		uint8_t sum = 0;
		for (unsigned i = 0; i < 7; i++) {
			sum += frame.data8[i];
		}
		if (frame.data8[7] != static_cast<uint8_t>(~sum)) {
			return;
		}

		// Alive counter in byte 6 bits 3:0; any change is accepted
		// (including skip/backward/wrap). Counter state persists across
		// timeouts so a stuck stream cannot refresh the reading.
		const uint8_t counter = frame.data8[6] & 0x0F;
		if (m_haveCounter && counter == m_counter) {
			return;
		}
		m_haveCounter = true;
		m_counter = counter;
		offset = 4;
	}

	// Niva/Granta's two-byte speed frame has no counter or checksum.
	const uint16_t raw = (uint16_t(frame.data8[offset]) << 8) | frame.data8[offset + 1];
	if (raw == 0xFFFF) {
		invalidate();
		return;
	}

	const uint16_t limited = raw > 50000 ? 50000 : raw;
	setValidValue(limited * 0.01f, nowNt);
}

bool useM749VehicleSpeed() {
	const bool supported = config->ladaCanbusProfile == LadaCanbusProfile::Largus
		|| config->ladaCanbusProfile == LadaCanbusProfile::NivaGranta;
	return supported && !engineConfiguration->enableCanVss
		&& !isBrainPinValid(engineConfiguration->vehicleSpeedSensorInputPin);
}

void M749VehicleSpeedSensor::reset() {
	invalidate();
	m_haveCounter = false;
	m_counter = 0;
}

static M749VehicleSpeedSensor speed;

void initM749VehicleCan() {
	// Keep the listener registered for live profile changes; RX checks selection.
	registerCanListener(speed);
	startM749VehicleCan();
}

void startM749VehicleCan() {
	// Explicitly configured sources take precedence, even before initialization.
	if (useM749VehicleSpeed() && !Sensor::hasSensor(SensorType::VehicleSpeed)) {
		speed.Register();
	}
}

void stopM749VehicleCan() {
	// Release only our slot before configuration applies another speed source.
	speed.unregister();
	speed.reset();
}

// ---------------------------------------------------------------------------
// Encode helpers
// ---------------------------------------------------------------------------

namespace {
struct PeriodicFrame {
	uint16_t id;
	uint8_t length;
	uint8_t periodTicks;
	uint8_t phaseTicks;
	uint8_t baseline[8];
};

// Periods and phases are in 5 ms worker ticks. Stagger the slow frames to
// leave queue space for diagnostics and other enabled CAN services.
// Unassigned fields retain fixed profile baselines; they are not live sensors.
constexpr PeriodicFrame largusFrames[] = {
	{0x186, 7,   2,  0, {0x00, 0x00, 0x32, 0x03, 0x20, 0x00, 0x20, 0x00}},
	{0x189, 8,   2,  1, {0x32, 0x03, 0x20, 0x32, 0x00, 0xB9, 0x00, 0x00}},
	{0x18A, 6,   2,  0, {0x32, 0x00, 0x00, 0x06, 0xFE, 0x00, 0x00, 0x00}},
	{0x1F6, 8,   2,  1, {0x00, 0x00, 0x02, 0x2E, 0x00, 0x00, 0x00, 0x00}},
	{0x217, 8,   4,  0, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88}},
	{0x2A9, 1,   4,  1, {0xF4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{0x2C6, 6,   4,  2, {0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00}},
	{0x36E, 4,  20,  3, {0x09, 0x99, 0x09, 0x99, 0x00, 0x00, 0x00, 0x00}},
	{0x3A5, 3,  20,  4, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{0x41A, 8,  20,  5, {0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{0x41D, 4,  20,  6, {0x48, 0xFE, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{0x511, 7,  20,  7, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{0x522, 8,  20,  8, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{0x5DA, 8,  20,  9, {0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00}},
	{0x648, 8,  20, 10, {0xC3, 0xFF, 0xF0, 0x00, 0x00, 0x7F, 0xFE, 0x18}},
	{0x65C, 3,  20, 11, {0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{0x66A, 8,  20, 12, {0x08, 0xFF, 0x00, 0x01, 0x01, 0xC0, 0x00, 0x00}},
	{0x6FD, 4,  20, 13, {0x00, 0x00, 0x99, 0x20, 0x00, 0x00, 0x00, 0x00}},
	{0x5E2, 2, 200, 19, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
};

// Shared Niva/Granta layout. Unassigned fields use fixed compatibility values;
// they do not describe live torque, warning lamps or other vehicle state.
constexpr PeriodicFrame nivaGrantaFrames[] = {
	{0x1F9, 8,  2,  0, {0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{0x180, 8,  2,  0, {0x00, 0x00, 0x30, 0x23, 0x86, 0x11, 0x40, 0x00}},
	{0x160, 7,  2,  0, {0x2D, 0xA3, 0x02, 0x00, 0x00, 0xC8, 0x00, 0x00}},
	{0x182, 8,  2,  1, {0x00, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00}},
	{0x186, 7,  2,  1, {0x00, 0x00, 0x30, 0x20, 0x00, 0x00, 0x00, 0x00}},
	{0x18A, 6,  2,  1, {0x2D, 0xA8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{0x189, 8,  2,  1, {0x2D, 0xA3, 0x20, 0x38, 0x60, 0xD9, 0x00, 0x00}},
	{0x35D, 8, 20,  0, {0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00}},
	{0x551, 8, 20,  2, {0x00, 0x00, 0x01, 0x80, 0x01, 0x00, 0x00, 0x00}},
	{0x6E2, 6, 20,  4, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{0x5DA, 8, 20,  6, {0x00, 0x76, 0x00, 0x00, 0xBE, 0x00, 0x00, 0x00}},
	{0x65C, 2, 20,  8, {0xC8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{0x314, 8, 20, 10, {0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{0x68E, 8, 20, 12, {0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAA}},
};

float bounded(float value, float low, float high) {
	if (!std::isfinite(value)) {
		return low;
	}
	return value < low ? low : value > high ? high : value;
}

uint16_t indicatedSpeed(float speed) {
	// Keep the fixed-point rounding of the indicated-speed conversion.
	// Zero or unavailable speed stays zero; positive speed gets gain/offset.
	const uint32_t q6 = bounded(speed, 0, 500) * 64;
	const uint32_t indicated = q6 ? ((q6 * 16712) >> 14) + 125 : 0;
	const uint32_t raw = (indicated * 5) / 32;
	return raw > 4095 ? 4095 : raw;
}

/**
 * Encode RPM as RPM*16 big-endian uint16_t (per BCM protocol).
 * When RPM is zero (engine not rotating) the original ECU still broadcasts
 * ~0x3200 (800 RPM * 16).  BCM treats the non-zero baseline as "ECU
 * calibration loaded" confirmation; 0x0000 triggers BCM error state CF and
 * prevents the starter relay from physically closing.
 */
uint16_t encodeRpmOrBaseline(float rpm) {
	if (rpm < 1.0f) {
		return 0x3200u;
	}
	return (uint16_t)(rpm * 16.0f);
}

/**
 * Instantaneous fuel flow for the dash BC (бортовой компьютер), mL/h x16
 * (0.0625 mL/h per unit). 0x186 bytes 0-1, sent while the throttle is CLOSED
 * (X == 0) - the dash reads this field as the fuel flow then: its idle L/h
 * display = value / 16 (the stock's idle 0.9-1.3 L/h = 0x3A4C..0x4E00 / 16,
 * recomputed constantly). While the throttle is open the same field carries
 * rpm x8 for the tach (see the 0x186 case).
 *
 * The stock sends the RAW per-event rate (spiky at idle - the dash does its
 * own smoothing). Do not pre-smooth: a pre-filtered value makes the dash's
 * display update slower than the stock's.
 */
uint16_t encodeFuelFlowMlPerHour() {
#ifdef MODULE_ODOMETER
	float gPerSecond = engine->module<TripOdometer>()->getConsumptionGramPerSecond();
#else
	float gPerSecond = 0;
#endif // MODULE_ODOMETER

	// Fuel cut: zero flow. The original ECU does exactly this - the field
	// drops to 0x0000 during DFCO even while rpm is still climbing, and the
	// dash shows zero consumption.
	if (engine->module<DfcoController>()->cutFuel()) {
		gPerSecond = 0;
	}

	// g/s -> mL/h: *3600 (s->h), /0.745 g/mL (gasoline density), *16 (stock
	// field scale: mL/h in 1/16 units).
	float mlPerHour = gPerSecond * 3600.0f / 0.745f * 16.0f;
	return (uint16_t)clampF(0.0f, mlPerHour, 65535.0f);
}

/**
 * Dash tach byte - 0x0186 byte 4. Stock fit (orig_1/2/3): tach = rpm - 768
 * EXACTLY (slope 1.0 across every captured point, 702-912 rpm): 775 -> 0x07,
 * 800 -> 0x20, 889 -> 0x79; 0x20 at rest (800 rpm baseline).
 *
 * The dash needle has two rpm sources: the 16-bit rpm fields (0x0189[0:1]
 * etc. = rpm x16) and 0x0186[0:1] = rpm x8 (the field this board's old
 * fuel-flow encoding broke). Byte 4 is the plausibility cross-check: the
 * cluster compares it against its own rpm estimate and ignores/holds the
 * needle when it diverges. Mode 4 = the stock formula with u8 wrap-around
 * ((uint8_t)(int32_t)(rpm - 768)) - at 2500 rpm it wraps to 196, matching
 * what a C-compiled stock ECU computes. Modes 0-3 are the old on-car A/B
 * candidates, kept behind 'cantach' until mode 4 is validated on the road.
 */
int s_tachEncoding = 4;

const char* tachEncodingDescription(int mode) {
	switch (mode) {
		case 1: return "rpm - 768 clamped (caps at 1023, old)";
		case 2: return "rpm/8 - 68 (caps at 2584)";
		case 3: return "rpm/32 + 7 (covers ~7900)";
		case 4: return "rpm - 768, u8 wrap (stock formula)";
		default: return "rpm/16 - 18 (capture fit, caps at ~4368)";
	}
}

uint8_t encodeTachByte(float rpm) {
	if (rpm < 1.0f) {
		// rest baseline - all modes agree at the 800 rpm point (0x20 = 32)
		return 0x20;
	}

	switch (s_tachEncoding) {
		case 1: return (uint8_t)clampF(0.0f, rpm - 768.0f, 255.0f);
		case 2: return (uint8_t)clampF(0.0f, rpm / 8.0f - 68.0f, 255.0f);
		case 3: return (uint8_t)clampF(0.0f, rpm / 32.0f + 7.0f, 255.0f);
		case 4: return (uint8_t)(int32_t)(rpm - 768.0f); // u8 wrap, stock formula
		default: return (uint8_t)clampF(0.0f, rpm / 16.0f - 18.0f, 255.0f);
	}
}

/**
 * Battery voltage, little-endian millivolts - 0x0189 bytes 2-3.
 * orig traces: 0x3220 LE = 12832 mV = 12.83 V at rest, 0x38CD..0x3CE9 LE
 * (14.5..15.6 V) while the alternator/bench supply charges.
 */
uint16_t encodeBatteryMillivoltsLE() {
	float volts = Sensor::getOrZero(SensorType::BatteryVoltage);
	return (uint16_t)clampF(0.0f, volts * 1000.0f, 65535.0f);
}

/**
 * Throttle-position byte X - 0x0186 byte 5. 0.1 % units: 0x09..0x0E
 * (0.9..1.4 %) at warm idle, 0x4C (7.6 %) during a rev blip, 0 when the
 * throttle is closed (also during the catch - the stock holds it shut).
 *
 * The board reads ~0.4% at the closed stop (rusEFI autozero offset vs the
 * stock: real closed stop = -0.4% in rusEFI). The dash gates its tach
 * source selection and its consumption integrators on X > 0 - a nonzero
 * closed-throttle X flips the dash into the "fuel flowing" mode at idle
 * and the tach starts reading 0x186[0:1] as rpm x8 (the flow value, i.e.
 * the user-visible "2000 rpm at idle / 7000+ at light throttle" bug).
 * Subtract the closed-stop offset so a shut blade reads exactly 0.
 */
uint8_t encodeThrottleXByte() {
	float tps = Sensor::getOrZero(SensorType::Tps1);
	// Closed-throttle offset measured on the board (see above).
	float x = (tps - 0.4f) * 10.0f;
	return (uint8_t)clampF(0.0f, x, 255.0f);
}

/**
 * Ignition-advance byte - 0x018A byte 4: 256 - 2*advance while running,
 * 0x02 cranking and 0x00 at IGN-on.
 */
uint8_t encodeIgnitionAdvanceByte(float timingAdvanceDeg) {
	// 256 - 2*advance; clamp so the byte stays in [0x02, 0xFE]
	float encoded = 256.0f - 2.0f * timingAdvanceDeg;
	return (uint8_t)clampF(2.0f, encoded, 254.0f);
}
} // namespace

void setM749TachEncoding(const char* arg) {
	if (arg == nullptr || arg[0] == 0) {
		efiPrintf("cantach: current=%d (%s). 0=rpm/16-18 1=rpm-768clamp 2=rpm/8-68 3=rpm/32+7 4=rpm-768wrap",
			s_tachEncoding, tachEncodingDescription(s_tachEncoding));
		return;
	}

	int mode = atoi(arg);
	if (mode < 0 || mode > 4) {
		efiPrintf("cantach: bad mode %d (0..4)", mode);
		return;
	}

	s_tachEncoding = mode;
	efiPrintf("cantach: mode=%d (%s)", mode, tachEncodingDescription(mode));
}

// ---------------------------------------------------------------------------
// Transmitter
// ---------------------------------------------------------------------------

void M749VehicleCanTransmitter::update() {
	const auto profile = config->ladaCanbusProfile;
	if (profile != m_profile || !engineConfiguration->canWriteEnabled) {
		m_tick = 0;
		m_counter1F9 = m_counter35D = m_counter551 = 0;
		m_profile = profile;
	}
	const PeriodicFrame* frames;
	size_t frameCount;
	switch (profile) {
	case LadaCanbusProfile::Largus:
		frames = largusFrames;
		frameCount = efi::size(largusFrames);
		break;
	case LadaCanbusProfile::NivaGranta:
		frames = nivaGrantaFrames;
		frameCount = efi::size(nivaGrantaFrames);
		break;
	default:
		m_tick = 0;
		return;
	}
	if (!engineConfiguration->canWriteEnabled) {
		m_tick = 0;
		return;
	}
	const bool largus = profile == LadaCanbusProfile::Largus;

	const auto rpm = bounded(Sensor::getOrZero(SensorType::Rpm), 0, 8191.875f);
	const auto clt = Sensor::get(SensorType::Clt);
	const auto iat = Sensor::get(SensorType::Iat);
	const bool isRunning = engine->rpmCalculator.isRunning();
	const auto state = rpm == 0 ? 0 : isRunning ? 2 : 1;

	for (size_t index = 0; index < frameCount; index++) {
		const auto& frame = frames[index];
		if (m_tick % frame.periodTicks != frame.phaseTicks) {
			continue;
		}
		// No temperature fault encoding is defined by this profile. Stop the
		// affected frame instead of transmitting a fabricated healthy reading.
		if ((frame.id == (largus ? 0x5DA : 0x551) && (!clt || !std::isfinite(clt.Value)))
			|| (largus && frame.id == 0x65C && (!iat || !std::isfinite(iat.Value)))) {
			continue;
		}

		CanTxMessage msg(CanCategory::NBC, frame.id, frame.length, 0, false);
		for (unsigned i = 0; i < frame.length; i++) {
			msg[i] = frame.baseline[i];
		}
		switch (frame.id) {
		case 0x180: {
			// Niva / Granta: big-endian RPM * 8, including cranking RPM.
			const uint16_t raw = rpm * 8;
			msg[0] = raw >> 8;
			msg[1] = raw;
			break;
		}
		case 0x186: {
			// Largus: merged layout. [0:1] is multiplexed by the SAME gate the
			// dash uses (the X throttle byte, 0x186[5]):
			//  - X == 0 (closed throttle): the dash reads this field as the
			//    instantaneous FUEL FLOW, mL/h x16 - the BC L/h display =
			//    value/16 (stock idle 0.9-1.3 L/h, recomputed constantly;
			//    DFCO zeroes it). The needle follows the rpm x16 fields.
			//  - X > 0 (throttle open): the dash reads this field as RPM x8 -
			//    the tach needle source (car-verified at exactly 1/8 slope:
			//    the old flow-always encoding showed flow/8 - 2000 rpm at idle,
			//    7000+ at 1900 - and a nonzero closed-throttle X flipped the
			//    dash into this mode at idle).
			// [2:3] = rpm x16, [4] = tach cross-check, [5] = throttle-X.
			// Niva / Granta keeps the profile baseline (0x186[0:1] stays zero).
			if (!largus) {
				break;
			}
			const uint8_t throttleX = encodeThrottleXByte();
			const uint16_t b01 = (state == 2 && throttleX > 0)
				? (uint16_t)(rpm * 8.0f)
				: encodeFuelFlowMlPerHour();
			const uint16_t rpmEncoded = encodeRpmOrBaseline(rpm);
			msg[0] = (uint8_t)(b01 >> 8);
			msg[1] = (uint8_t)(b01 & 0xFF);
			msg[2] = (uint8_t)(rpmEncoded >> 8);
			msg[3] = (uint8_t)(rpmEncoded & 0xFF);
			msg[4] = encodeTachByte(rpm);
			msg[5] = state == 2 ? throttleX : 0x00;
			break;
		}
		case 0x189: {
			// Largus: live rpm x16, battery mV LE, rolling freshness counter,
			// 0xB9/0xBA state byte (all kept from the original stream).
			if (!largus) {
				break;
			}
			const uint16_t rpmEncoded = encodeRpmOrBaseline(rpm);
			const uint16_t vbattMv = encodeBatteryMillivoltsLE();
			msg[0] = (uint8_t)(rpmEncoded >> 8);
			msg[1] = (uint8_t)(rpmEncoded & 0xFF);
			msg[2] = (uint8_t)(vbattMv & 0xFF);
			msg[3] = (uint8_t)(vbattMv >> 8);
			// byte 4: rolling counter while running (the cluster treats a frozen
			// value as stale), 0x00 at rest. The stock steps it by 0x10.
			msg[4] = state == 2 ? m_dashCounter : 0x00;
			m_dashCounter += 0x10;
			if (m_dashCounter == 0) {
				m_dashCounter = 0x10;
			}
			msg[5] = state == 2 ? 0xBA : 0xB9;
			break;
		}
		case 0x18A: {
			// Largus: live rpm x16, throttle-gated angle accumulator (2 deg/unit
			// quantized to 128-deg steps, 3 x rpm units/s), advance byte.
			if (!largus) {
				break;
			}
			const uint16_t rpmEncoded = encodeRpmOrBaseline(rpm);
			if (encodeThrottleXByte() > 0) {
				m_angleCounter2 += rpm * 0.03f; // rpm * 3 units/s over 10 ms
				if (m_angleCounter2 >= 65536.0f) {
					m_angleCounter2 -= 65536.0f;
				}
			}
			const uint16_t angleCounter = (uint16_t)(((uint32_t)m_angleCounter2 & 0xFFC0u) + 6u);
			msg[0] = (uint8_t)(rpmEncoded >> 8);
			msg[1] = (uint8_t)(rpmEncoded & 0xFF);
			msg[2] = (uint8_t)(angleCounter >> 8);
			msg[3] = (uint8_t)(angleCounter & 0xFF);
			msg[4] = state == 2 ? encodeIgnitionAdvanceByte(engine->engineState.timingAdvance[0])
				: (state == 1 ? 0x02 : 0x00);
			break;
		}
		case 0x1F6:
			// Engine state bits 7:6 = 0 stopped / 1 cranking / 2 running.
			// Byte 1 (0x20 running) and byte 3 (battery volts x3.75: the stock
			// reads 0x30 at 12.83 V, 0x2D at 12.0 V - fitted from the captures)
			// are the car-derived fields kept from the original stream.
			msg[2] = (msg[2] & 0x3F) | (state << 6);
			msg[1] = state == 2 ? 0x20 : 0x00;
			msg[3] = (uint8_t)clampF(0.0f, Sensor::getOrZero(SensorType::BatteryVoltage) * 3.75f, 255.0f);
			break;
		case 0x217: {
			// Indicated speed, 0.1 km/h, 12 bits: byte 3 = bits 11:4, byte 4
			// high nibble = bits 3:0. 0x70 in byte 2 while the engine rotates.
			const auto raw = indicatedSpeed(Sensor::getOrZero(SensorType::VehicleSpeed));
			msg[2] = rpm > 0 ? 0x70 : 0;
			msg[3] = raw >> 4;
			msg[4] = (msg[4] & 0x0F) | ((raw & 0x0F) << 4);
			break;
		}
		case 0x5DA:
			// Largus: dash coolant temperature (CLT+40, -40..215 degC), current
			// idle target / 8, configured hard RPM limit / 32. Niva / Granta
			// keeps the profile baseline (0x5DA[0] stays zero by contract).
			if (!largus) {
				break;
			}
			msg[0] = bounded(clt.Value, -40, 215) + 40;
			msg[1] = bounded(engine->module<IdleController>().unmock().idleTarget, 0, 2040) / 8;
			msg[4] = bounded(engineConfiguration->rpmHardLimit, 0, 8160) / 32;
			break;
		case 0x65C:
			// Largus: intake temperature, bits 7:1 = (IAT+40), -40..87 degC.
			if (largus) {
				msg[0] = static_cast<uint8_t>(bounded(iat.Value, -40, 87) + 40) << 1;
			}
			break;
		case 0x1F9:
			msg[0] = (msg[0] & 0x0F) | (m_counter1F9 << 4);
			m_counter1F9 = (m_counter1F9 + 1) & 0x0F;
			break;
		case 0x35D:
			msg[6] = (msg[6] & 0x0F) | (m_counter35D << 4);
			m_counter35D = (m_counter35D + 1) & 0x0F;
			break;
		case 0x551:
			// Niva / Granta: dash coolant temperature (CLT+40).
			msg[1] = bounded(clt.Value, -40, 215) + 40;
			msg[5] = (msg[5] & 0x0F) | (m_counter551 << 4);
			m_counter551 = (m_counter551 + 1) & 0x0F;
			break;
		case 0x41A:
			// ECU init status: IGN on {0x00, 0x03, ...}, running all zeros
			// (kept from the original stream).
			if (largus) {
				msg[1] = state == 2 ? 0x00 : 0x03;
			}
			break;
		case 0x66A:
			// ECU operating state + fuel-gated 100 ms counter. Bytes 3-4
			// advance 1 per 100 ms while fuel flows at up to ~820 rpm and 2
			// per 100 ms above, paused at closed throttle (the stock's
			// cumulative hour-meter). Kept from the original stream.
			if (largus) {
				if (encodeThrottleXByte() > 0) {
					m_engineTimeCounter += (rpm > 820.0f) ? 2 : 1;
				}
				msg[0] = state == 2 ? 0x00 : 0x08;
				msg[3] = m_engineTimeCounter;
				msg[4] = m_engineTimeCounter;
			}
			break;
		case 0x6FD:
			// ECU mode indicator: byte 3 = 0x20 (IGN on) / 0x00 (running).
			if (largus) {
				msg[3] = state == 2 ? 0x00 : 0x20;
			}
			break;
		default:
			break;
		}
	}
	m_tick = (m_tick + 1) % 200;
}

// ---------------------------------------------------------------------------
// Board hook: custom_board_update_dash
// ---------------------------------------------------------------------------

void updateM749VehicleCan(CanCycle cycle) {
	UNUSED(cycle);
	static M749VehicleCanTransmitter transmitter;

	// Stock behavior: with ignition off the ECU's CAN node is COMPLETELY
	// silent (verified in ignoff.trc - zero ECU frames of any kind; the
	// remaining traffic is dash/cluster chatter). The BCM drops the main
	// relay when the 0x0189 heartbeat dies - if we keep streaming with
	// the key off, the BCM treats the ECU as alive and the relay never
	// drops. So the whole dash stream stops until ignition returns.
	// isIgnVoltage() reads the L9779 KEY_ON cache (DIA_REG9), false for the
	// first ~100 ms after boot - safe default.
	if (!isIgnVoltage()) {
		return;
	}

	// While a serial (ISO-TP/TS) session is active:
	//   - engine off: mute the vehicle stream entirely (it contends with TS
	//     responses for CAN mailboxes).
	//   - engine running: keep sending at 1/4 rate - the dash/IMMO/pump logic
	//     tolerates it for the 10 s pause window, the serial bursts need the
	//     mailboxes.
	bool serialSessionActive = (engine->pauseCANdueToSerialUntil > getTimeNowNt());
	if (serialSessionActive) {
		if (Sensor::getOrZero(SensorType::Rpm) <= 0) {
			return;
		}
		static uint8_t divider = 0;
		if ((divider++ & 3) != 0) {
			return;
		}
	}

	transmitter.update();
}

#endif // EFI_CAN_SUPPORT
