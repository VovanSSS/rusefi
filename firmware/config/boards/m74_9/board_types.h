#pragma once

#include <stdint.h>

// Vehicle CAN profile selector for the M74.9 dash CAN stream (CAN1).
// Keep the numeric values synchronized with the `custom LadaCanbusProfile`
// declaration in board_config.txt (TS labels: Disabled, Largus, Niva / Granta).
enum class LadaCanbusProfile : uint8_t {
	Disabled = 0,
	Largus = 1,
	NivaGranta = 2,
};

static_assert(sizeof(LadaCanbusProfile) == 1);
