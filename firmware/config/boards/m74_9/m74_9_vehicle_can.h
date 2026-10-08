/**
 * @file    m74_9_vehicle_can.h
 *
 * Vehicle CAN for the m74_9 board: dashboard transmit + vehicle-speed
 * receive on CAN1. Ported from the fw-m74.9 board repo (vehicle_can.h /
 * vehicle_can_tx.cpp), merged with the board's existing car-derived Largus
 * encodings (fuel flow, tach byte, battery voltage, throttle-X, advance,
 * hour-meter) which the profile implementation there does not carry.
 *
 * The transmitter is driven once per 5 ms CAN worker tick via the
 * custom_board_update_dash hook (updateDash -> CanWrite::PeriodicTask).
 * Periods/phases below are in those 5 ms ticks.
 *
 * @date    2026-10-08
 */

#pragma once

#include "can_sensor.h"

// CAN1 vehicle speed, in km/h. Integrity failures never refresh the sensor.
class M749VehicleSpeedSensor : public CanSensorBase {
public:
	M749VehicleSpeedSensor();
	void reset();
	bool acceptFrame(size_t busIndex, const CANRxFrame& frame) const override;

protected:
	void decodeFrame(const CANRxFrame& frame, efitick_t nowNt) override;

private:
	bool m_haveCounter = false;
	uint8_t m_counter = 0;
};

bool useM749VehicleSpeed();
void initM749VehicleCan();
void stopM749VehicleCan();
void startM749VehicleCan();

// Called once per 5 ms CAN worker tick; no blocking and no catch-up bursts.
class M749VehicleCanTransmitter {
public:
	void update();

private:
	uint16_t m_tick = 0;
	LadaCanbusProfile m_profile = LadaCanbusProfile::Disabled;
	uint8_t m_counter1F9 = 0;
	uint8_t m_counter35D = 0;
	uint8_t m_counter551 = 0;

	// Car-derived state kept from the original m74_9 dash stream (the profile
	// tables do not carry these live fields).
	float m_angleCounter2 = 0;
	uint8_t m_engineTimeCounter = 0;
	uint8_t m_dashCounter = 0;
};

// Board hook for custom_board_update_dash: ignition gate + serial-session
// throttling (kept from the original m74_9 stream) around the transmitter.
void updateM749VehicleCan(CanCycle cycle);

// Tach-byte encoding mode ('cantach' console action); mode 4 = the stock
// formula (rpm - 768 with u8 wrap).
void setM749TachEncoding(const char* arg);
