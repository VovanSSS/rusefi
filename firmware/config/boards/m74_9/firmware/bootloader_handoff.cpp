#include "pch.h"
#include "bootloader_handoff.h"
#include "boot_activation.h"
#include "board_overrides.h"
#include "can.h"

// Own CAN1 while awaiting this exact frame's physical completion. The normal
// HAL return means only "mailbox loaded". Suppress its TX ISR temporarily so
// TXOK cannot be cleared before we inspect it; RX/error interrupts stay enabled.
static bool transmitCompleted(const CANTxFrame& frame) {
    CanTxMessage::removeDevice(0);
    const auto start = chVTGetSystemTimeX();
    constexpr auto timeout = TIME_MS2I(50);
    const uint32_t empty = CAN_TSR_TME0 | CAN_TSR_TME1 | CAN_TSR_TME2;
    while ((CAND1.can->TSR & empty) != empty) {
        if (chVTTimeElapsedSinceX(start) >= timeout) {
            CanTxMessage::setDevice(0, &CAND1);
            return false;
        }
        chThdSleepMilliseconds(1);
    }

    chSysLock();
    constexpr auto txIrq = static_cast<IRQn_Type>(STM32_CAN1_TX_NUMBER);
    const auto txInterrupt = NVIC_GetEnableIRQ(txIrq);
    // Mask at the NVIC, including an interrupt already pending from the drain.
    NVIC_DisableIRQ(txIrq);
    __DSB();
    __ISB();
    CAND1.can->TSR = CAN_TSR_RQCP0;
    can_lld_transmit(&CAND1, 1, &frame);
    chSysUnlock();

    uint32_t status;
    do {
        status = CAND1.can->TSR;
        if (status & CAN_TSR_RQCP0) {
            break;
        }
        chThdSleepMilliseconds(1);
    } while (chVTTimeElapsedSinceX(start) < timeout);

    const bool completed = (status & (CAN_TSR_RQCP0 | CAN_TSR_TXOK0)) ==
                           (CAN_TSR_RQCP0 | CAN_TSR_TXOK0);
    chSysLock();
    if (!completed) {
        CAND1.can->TSR = CAN_TSR_ABRQ0;
    }
    if (txInterrupt) {
        NVIC_EnableIRQ(txIrq);
    }
    chSysUnlock();
    CanTxMessage::setDevice(0, &CAND1);
    return completed;
}

static void processDiagnosticRequest(size_t bus, const CANRxFrame& frame, efitick_t) {
    if (bus == 0 && CAN_SID(frame) == 0x7E0 && !CAN_ISX(frame) && !CAN_ISRTR(frame) && frame.DLC >= 4 &&
        frame.data8[0] == 3 && frame.data8[1] == 0x22 && frame.data8[2] == 0xF1 &&
        frame.data8[3] >= 0xA0 && frame.data8[3] <= 0xA4) {
        const auto did = frame.data8[3];
        const uint32_t value = did == 0xA0 ? (m749ActivationReady() ? 0x4D740101 : 0x4D740100) :
            did == 0xA1 ? m749SoftwareCrc() : did == 0xA2 ? m749CalibrationCrc() :
            did == 0xA3 ? *reinterpret_cast<const volatile uint32_t*>(m749::MarkerAddress) :
            0x72454649;  // F1A4: 'r','E','F','I' - the rusEFI identity DID
        CANTxFrame response = {};
        response.SID = 0x7E8;
        response.DLC = 8;
        const uint8_t data[8] = {7, 0x62, 0xF1, did, uint8_t(value >> 24), uint8_t(value >> 16),
                                uint8_t(value >> 8), uint8_t(value)};
        memcpy(response.data8, data, sizeof(data));
        transmitCompleted(response);
        return;
    }
    if (bus == 0 && CAN_SID(frame) == 0x7E0 && !CAN_ISX(frame) && !CAN_ISRTR(frame) && frame.DLC >= 3 &&
        frame.data8[0] == 2 && frame.data8[1] == 0x11 && frame.data8[2] == 1) {
        CANTxFrame response = {};
        response.SID = 0x7E8;
        response.DLC = 8;
        const bool stopped = engine->rpmCalculator.isStopped();
        const uint8_t data[8] = {uint8_t(stopped ? 2 : 3), uint8_t(stopped ? 0x51 : 0x7F),
                                uint8_t(stopped ? 1 : 0x11), uint8_t(stopped ? 0 : 0x22), 0, 0, 0, 0};
        memcpy(response.data8, data, sizeof(data));
        if (transmitCompleted(response) && stopped) {
            __disable_irq();
            m749BootIntent = 0; // Prove normal-marker boot, independent of the return token.
            __DSB();
            NVIC_SystemReset();
        }
        return;
    }
    m749::handleRequest(bus, CAN_SID(frame), CAN_ISX(frame), CAN_ISRTR(frame),
        frame.data8, frame.DLC, engine->rpmCalculator.isStopped(),
        [](const uint8_t* payload) {
            CANTxFrame response = {};
            response.SID = 0x7E8;
            response.DLC = 8;
            memcpy(response.data8, payload, 8);
            return transmitCompleted(response);
        },
        [](uint32_t token) {
            __disable_irq();
            m749BootIntent = token;
            __DSB();
            NVIC_SystemReset();
        });
}

void initM749BootloaderHandoff() {
    custom_board_can_rx = processDiagnosticRequest;
}
