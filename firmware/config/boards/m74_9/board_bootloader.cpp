/**
 * @file	board_bootloader.cpp
 * @brief	Minimal board hooks for the OpenBLT bootloader build (m74_9)
 *
 * The full board_configuration.cpp is excluded from the bootloader build:
 * it drags in the whole engine runtime (sensors, trigger decoding, MFS
 * storage) which the bootloader does not need. The bootloader only needs:
 *   - boardInit(), called from halInit() via the ChibiOS board mechanism
 *     (GPIOs/clocks are already set up by board.c __early_init);
 *   - the LED pins used by the OpenBLT LED driver;
 *   - boardCanListener(): the M749 UDS handoff (identity DIDs + 10 02), see
 *     below.
 *
 * @date Aug 19, 2026
 */

#include "pch.h"

#ifdef BOOTLOADER_CAN_LISTENER

#include "hal.h"
#include "can.h"
#include "boot_activation.h"
#include "bootloader_handoff.h"

/**
 * The OpenBLT CAN driver calls this hook for every received frame that does
 * NOT match the XCP receive ID (openblt_can.cpp CanReceivePacket "wrong:"
 * path, BOOTLOADER_CAN_LISTENER). While the ECU sits in OpenBLT (XCP mode -
 * the backdoor window, a failed app-update or the reset-loop latch) the M749
 * CLI's UDS traffic on 7E0 was previously dropped, so `m749_can.sh --identify`
 * and the OEM-loader rollback were unreachable from this state.
 *
 * Two services are answered, mirroring the running app's handoff
 * (bootloader_handoff.cpp):
 *   - 22 F1A0..F1A4 identity DIDs: the CLI identifies the ECU as rusEFI
 *     (M749_READY / M749_NOT_READY by the activation word, or RUSEFI by the
 *     F1A4 'rEFI' identity). The bootloader build computes the CRCs in
 *     initM749BootActivation(), so the values are real.
 *   - 10 02 programming session: acknowledge, then reboot with the SRAM
 *     boot-intent token - the OEM resident loader enters programming mode and
 *     the CLI continues with loader authentication and flash access. This is
 *     the rollback entry: --upload <OEM fullflash.bin> restores the stock
 *     application from the OpenBLT state.
 */

static bool m749Transmit(const uint8_t payload[8]) {
	CANTxFrame response = {};
	response.SID = 0x7E8;
	response.DLC = 8;
	memcpy(response.data8, payload, 8);
	return canTransmitTimeout(&CAND1, CAN_ANY_MAILBOX, &response, TIME_MS2I(100)) == MSG_OK;
}

void boardCanListener(CANRxFrame* frame) {
	if (CAN_ISX(*frame) || CAN_ISRTR(*frame) || CAN_SID(*frame) != 0x7E0 || frame->DLC < 3) {
		return;
	}
	const uint8_t* data = frame->data8;
	const uint8_t length = data[0];

	// ReadDataByIdentifier 0x22 F1 xx (ISO-TP single frame).
	if (length == 3 && data[1] == 0x22 && data[2] == 0xF1 && data[3] >= 0xA0 && data[3] <= 0xA4) {
		const uint8_t did = data[3];
		uint32_t value;
		switch (did) {
			case 0xA0: value = m749ActivationReady() ? 0x4D740101 : 0x4D740100; break;
			case 0xA1: value = m749SoftwareCrc(); break;
			case 0xA2: value = m749CalibrationCrc(); break;
			case 0xA3: value = *reinterpret_cast<const volatile uint32_t*>(m749::MarkerAddress); break;
			default:   value = 0x72454649; break; // F1A4: 'r','E','F','I' - the rusEFI identity
		}
		const uint8_t payload[8] = {7, 0x62, 0xF1, did, uint8_t(value >> 24), uint8_t(value >> 16),
			uint8_t(value >> 8), uint8_t(value)};
		m749Transmit(payload);
		return;
	}

	// DiagnosticSessionControl 0x10 02: acknowledge, then hand the boot to the
	// OEM resident loader. The loader alone serves the authentication, the
	// FF01 flash access and the 34/36/37 download that the CLI runs next.
	if (length == 2 && data[1] == 0x10 && data[2] == 0x02) {
		// Session 02, P2 = 50 ms, P2* = 5000 ms (10 ms units).
		const uint8_t payload[8] = {6, 0x50, 2, 0, 50, 1, 0xF4, 0};
		if (!m749Transmit(payload)) {
			// The CLI waits for the 50 02 reply before loader authentication;
			// without it on the wire the reset would just stall the session.
			return;
		}
		// Give the frame time to physically leave the bus, then reboot into
		// the OEM loader (the same entry the running app uses).
		chThdSleepMilliseconds(10);
		__disable_irq();
		m749BootIntent = m749::ProgrammingToken;
		__DSB();
		NVIC_SystemReset();
	}
}

#endif // BOOTLOADER_CAN_LISTENER

void boardInit() {
	// Nothing to do: board.c (__early_init) already set up GPIOs and clocks,
	// and the drivers initialize their own pins (see CanInit in
	// bootloader/openblt_chibios/openblt_can.cpp).
}

Gpio getCommsLedPin() {
	return Gpio::Unassigned;
}

Gpio getWarningLedPin() {
	return Gpio::Unassigned;
}

Gpio getRunningLedPin() {
	return Gpio::Unassigned;
}
