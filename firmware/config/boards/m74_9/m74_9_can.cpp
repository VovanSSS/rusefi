/**
 * @file    m74_9_can.cpp
 *
 * BCM (Body Control Module) CAN protocol for the m74_9 board.
 *
 * Protocol overview
 * -----------------
 * BCM -> ECU (receive):
 *   0x0350 (100 ms) - BCM state machine; byte 4 encodes IGN/crank/relay/running state
 *   0x0303 (100 ms) - BCM relay status; byte 5 bit 1 = starter relay active
 *   0x0714           - IMMO challenge (see the IMMO section below)
 *
 * ECU -> BCM (transmit):
 *   0x0713           - IMMO trigger / response
 *
 * The dash/vehicle stream (0x01F6/0x0189/0x0186/0x018A/0x0217/0x02A9/0x02C6,
 * the 100 ms burst group and the 1 s keepalive, both LadaCanbusProfile
 * layouts) moved to m74_9_vehicle_can.cpp, driven by the
 * custom_board_update_dash hook. This file keeps the BCM RX, the IMMO
 * handshake and the ignition gating around the IMMO tick.
 *
 * Timing
 * ------
 * request() is called every 5 ms by CanWrite::PeriodicTask() and drives the
 * IMMO state machine (1 tick = 5 ms).
 *
 * @date    2026-08-12
 */

#include "pch.h"

#if EFI_CAN_SUPPORT

#include "m74_9_can.h"
#include "m74_9_vehicle_can.h"
#include "can_listener.h"
#include "can_msg_tx.h"
#include "ignition_controller.h"

// ---------------------------------------------------------------------------
// Frame IDs - BCM -> ECU (receive)
// ---------------------------------------------------------------------------
static constexpr uint32_t BCM_STATE_ID = 0x0350;   // BCM state machine
static constexpr uint32_t BCM_RELAY_ID = 0x0303;   // BCM relay status

// Byte 4 threshold for "starter relay active or engine running" in 0x0350
static constexpr uint8_t BCM_STARTER_ACTIVE_THRESHOLD = 0x80;

// ---------------------------------------------------------------------------
// IMMO challenge-response protocol (0x0713 / 0x0714)
// ---------------------------------------------------------------------------
// Protocol direction (confirmed from PCAN-View timing analysis):
//   ECU  -> 0x0713 [8 bytes]  : trigger / session request  (ECU initiates)
//   BCM  -> 0x0714 [8 bytes]  : challenge frame 1           }
//   BCM  -> 0x0714 [8 bytes]  : challenge frame 2  (0.2ms)  } 16-byte challenge
//   ECU  -> 0x0713 [8 bytes]  : computed response (15-20ms after challenge)
//   BCM  -> 0x0714 [8 bytes]  : periodic re-check (single frame, ~200ms)
//   ECU  -> 0x0713 [8 bytes]  : response           (<5ms)
//
// BCM physically closes the starter relay ONLY after a valid IMMO response.
// Without a correct 0x0713 response, BCM shows 0x84 in CAN but does not
// assert the relay GND signal.
//
// The response algorithm is at flash offset 0x203F0C in the original firmware
// (I865LB52_w2404b1 dump).  Load the dump in Ghidra (ARM Cortex-M LE Thumb,
// base 0x08000000) and decompile 0x08203F0C to extract the algorithm and key.
// Fill in computeImmoResponse() below once the algorithm is known.

static constexpr uint32_t IMMO_ECU_ID  = 0x0713;  // ECU sends (trigger + response)
static constexpr uint32_t IMMO_BCM_ID  = 0x0714;  // BCM sends (challenge)

// How long after first 0x0350 (IGN ON, byte4=0x04) before ECU sends the first trigger.
// Original firmware sends the trigger ~1500ms after ignition is first detected.
// rusEFI starts CAN earlier, so 1000ms should be enough for BCM to stabilise.
static constexpr uint32_t IMMO_TRIGGER_DELAY_MS = 1000u;
// Window within which two consecutive 0x0714 frames are treated as one 16-byte challenge
static constexpr uint32_t IMMO_CHALLENGE_WINDOW_MS = 5u;
// How long after receiving a challenge before sending the response
static constexpr uint32_t IMMO_RESPONSE_DELAY_MS = 15u;

// ---------------------------------------------------------------------------
// IMMO response computation
// ---------------------------------------------------------------------------

/**
 * Compute the 8-byte 0x0713 response from the 16-byte 0x0714 challenge.
 *
 * TODO: reverse-engineer the algorithm from the original firmware. The immo
 * module lives in the second flash bank around 0x08203F0C - 0x08204D00
 * (Ghidra: base 0x08000000, ARM Cortex-M LE Thumb). Relevant functions:
 *   0x08203FFC - immo dispatcher / state machine
 *   0x082047D0 - 0x0713/0x0714 frame staging helper
 *   0x082048E8 - 0x0714 receive handler / buffer copy
 *   0x08204B00 - 0x08204C00 - key constants and registration area
 *
 * Captured challenge/response pairs (full 16-byte challenge):
 *   trigger 0841994c0b81a14c
 *     challenge 4a4f2a204fad58fd273622b70c2b559e -> response fda32d94ae77c121
 *   trigger d081cc982708d806
 *     challenge 66aaeef37037dee0cdeefb22bde96807 -> response efaa66f0caa6f0cd
 *   trigger 5283dba64769c00d
 *     challenge 660be1e2a34b8140b45633a0499a01ec -> response 9cb7f8ca31431bb6
 *   trigger a5ea8a93265530dd
 *     challenge cfcfbbf3cdc0f75ce9efe2eb23b62a25 -> response bff99205ed4ab7a8
 *   trigger 45d4a5c0f6a1ea72
 *     challenge cd4c9070196bbdebb425cb7c4350082c -> response 96739be6b1299f77
 *
 * @param challenge  16 bytes received in two consecutive 0x0714 CAN frames
 *                   (frame1[0..7] || frame2[0..7])
 * @param response   8-byte output buffer, filled by this function
 * @return true if the computation succeeded (key loaded, algorithm known)
 */
static bool computeImmoResponse(const uint8_t challenge[16], uint8_t response[8]) {
    (void)challenge;
    (void)response;
    return false;
}

/**
 * Compute the 8-byte 0x0713 response to a SINGLE-frame (8-byte) 0x0714 challenge.
 * This is the "quick re-check" exchange that BCM sends periodically.
 *
 * Captured quick pairs:
 *   challenge 596e3885f2460f4a -> response 2cdf61de56551b86
 *   challenge a00059b79f90a8ae -> response a225de4e470324b6
 *   challenge 0eabfe9d351af837 -> response b15156d14683cd15
 *   challenge 4a90250e855128bf -> response 14a267bec8d8c3dc
 *   challenge 63b95b1a3fbf41fe -> response 2fa71d4722835ea2
 */
static bool computeImmoQuickResponse(const uint8_t challenge[8], uint8_t response[8]) {
    (void)challenge;
    (void)response;
    return false;
}

// ---------------------------------------------------------------------------
// IMMO logging helpers
// ---------------------------------------------------------------------------

static void immoLog8(const char* label, const uint8_t* data) {
    efiPrintf("IMMO: %s %02x %02x %02x %02x %02x %02x %02x %02x",
        label,
        data[0], data[1], data[2], data[3],
        data[4], data[5], data[6], data[7]);
}

static void immoLog16(const char* label, const uint8_t* data) {
    efiPrintf("IMMO: %s %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x",
        label,
        data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
        data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15]);
}

// ---------------------------------------------------------------------------
// Listener class
// ---------------------------------------------------------------------------

class M74_9BcmCanListener : public CanListener {
public:
    M74_9BcmCanListener()
        : CanListener(BCM_STATE_ID)
    {
    }

    bool isStarterActive() const {
        return m_starterActive;
    }

    bool isImmoAuthenticated() const {
        // With the IMMO force-off bit (physical bypass installed) the BCM does
        // not need our responses - report authenticated so no future gating logic
        // sees a failed handshake.
        return config->m74_9ImmoOff || (m_immoState == ImmoState::ResponseSent);
    }

    // Accept BCM state/relay frames AND IMMO challenge frames.
    // With m74_9ImmoOff set the 0x0714 challenge frames are rejected here, so
    // they never enter the RX path or the IMMO state machine at all.
    bool acceptFrame(const size_t busIndex, const CANRxFrame& frame) const override {
        (void)busIndex;
        uint32_t id = CAN_ID(frame);
        return (id == BCM_STATE_ID) || (id == BCM_RELAY_ID)
            || ((id == IMMO_BCM_ID) && !config->m74_9ImmoOff);
    }

    // Called every 5 ms by CanWrite::PeriodicTask(). Only the IMMO state
    // machine lives here; the dash/vehicle stream moved to
    // m74_9_vehicle_can.cpp (custom_board_update_dash hook). The ignition
    // gate mirrors the old sendPeriodic() behavior: no IMMO processing while
    // the key is off (the BCM sends nothing then anyway).
    CanListener* request() override {
        if (isIgnVoltage()) {
            tickImmo();
        }
        return getNext();
    }

protected:
    // Decode received BCM frames and update local state flags
    void decodeFrame(const CANRxFrame& frame, efitick_t nowNt) override {
        uint32_t id = CAN_ID(frame);

        if (id == BCM_STATE_ID) {
            // byte 4: 0x04 = IGN on/no start, 0x44 = crank switch pressed,
            //         0x84 = starter relay active, 0xC4 = engine running.
            uint8_t byte4 = frame.data8[4];
            m_starterActive = (byte4 >= BCM_STARTER_ACTIVE_THRESHOLD);

            // Arm IMMO trigger on ignition-on (byte4 == 0x04), NOT on crank switch.
            //
            // BCM does NOT assert crank-switch (byte4=0x44) until the IMMO
            // handshake is successfully completed.  Waiting for byte4=0x44
            // creates a dead-lock: rusEFI never sends the trigger, BCM never
            // sends the challenge, IMMO is never cleared, engine never starts.
            //
            // The original ECU sends the 0x0713 trigger ~1.5 s after the first
            // 0x0350 frame is received (IGN ON, byte4=0x04).  We mirror that:
            // arm the IMMO timer on any BCM active state so that the trigger
            // fires well before the user presses the crank switch.
            bool ignitionActive = (byte4 != 0x00);
            // With m74_9ImmoOff the IMMO state machine stays in Idle: no trigger,
            // no response, no 0x0713 flood (the physical bypass answers the BCM).
            if (ignitionActive && (m_immoState == ImmoState::Idle) && !config->m74_9ImmoOff) {
                m_immoState        = ImmoState::WaitingToTrigger;
                m_immoTimerTicks   = 0;
            }
        } else if (id == BCM_RELAY_ID) {
            // byte 5 bit 1 (0x02): starter relay active
            m_relayActive = ((frame.data8[5] & 0x02) != 0);
        } else if (id == IMMO_BCM_ID) {
            handleImmoChallengeFrame(frame, nowNt);
        }
    }

private:
    // -----------------------------------------------------------------------
    // IMMO state machine
    // -----------------------------------------------------------------------

    enum class ImmoState : uint8_t {
        Idle,             // no crank request active
        WaitingToTrigger, // crank switch seen; waiting IMMO_TRIGGER_DELAY_MS
        TriggerSent,      // sent 0x0713 trigger; waiting for 0x0714 challenge
        Challenge1Rx,     // received first 0x0714 frame; waiting for second
        ChallengeComplete, // both 0x0714 frames received; compute + send response
        ResponseSent,     // 0x0713 response sent; protocol complete for this session
    };

    /**
     * Process an incoming 0x0714 (IMMO challenge) frame.
     * BCM sends the 16-byte challenge as two consecutive 8-byte CAN frames.
     */
    void handleImmoChallengeFrame(const CANRxFrame& frame, efitick_t /*nowNt*/) {
        if (m_immoState == ImmoState::TriggerSent) {
            // First challenge frame
            for (int i = 0; i < 8; i++) {
                m_immoChallenge[i] = frame.data8[i];
            }
            immoLog8("challenge frame 1", m_immoChallenge);
            m_immoState      = ImmoState::Challenge1Rx;
            m_immoTimerTicks = 0;
        } else if (m_immoState == ImmoState::Challenge1Rx) {
            // Second challenge frame — 16-byte challenge now complete
            for (int i = 0; i < 8; i++) {
                m_immoChallenge[8 + i] = frame.data8[i];
            }
            immoLog16("challenge complete", m_immoChallenge);
            m_immoState      = ImmoState::ChallengeComplete;
            m_immoTimerTicks = 0;
        } else if (m_immoState == ImmoState::ResponseSent) {
            // BCM sent a single-frame quick re-check challenge
            immoLog8("quick challenge", frame.data8);
            uint8_t response[8] = {};
            if (computeImmoQuickResponse(frame.data8, response)) {
                sendImmoResponse(response);
            } else {
                efiPrintf("IMMO: quick challenge received but response algorithm not implemented");
            }
        }
    }

    /**
     * Periodic IMMO state-machine tick, called from request().
     * Manages delays and sends 0x0713 trigger/response at the right time.
     */
    void tickImmo() {
        // m74_9ImmoOff force-disables the whole handshake: no trigger, no
        // retries, no challenge processing (0x0714 frames are rejected in
        // acceptFrame() as well). The physical bypass answers the BCM instead.
        if (config->m74_9ImmoOff) {
            m_immoState = ImmoState::Idle;
            m_immoTimerTicks = 0;
            return;
        }

        m_immoTimerTicks++;
        // Convert 5ms ticks to ms
        uint32_t elapsedMs = m_immoTimerTicks * 5u;

        switch (m_immoState) {
        case ImmoState::WaitingToTrigger:
            if (elapsedMs >= IMMO_TRIGGER_DELAY_MS) {
                sendImmoTrigger();
                // log the session only on the initial send - retries below
                // would otherwise repeat this line every 500 ms forever
                efiPrintf("IMMO: sent 0x0713 trigger (session 0x%04x)", (unsigned)(m_immoSessionCounter - 1));
                m_immoState      = ImmoState::TriggerSent;
                m_immoTimerTicks = 0;
            }
            break;

        case ImmoState::TriggerSent:
            // If no challenge received within 500ms, retry trigger
            if (elapsedMs >= 500u) {
                sendImmoTrigger();
                m_immoTimerTicks = 0;
            }
            break;

        case ImmoState::Challenge1Rx:
            // If second frame does not arrive within the window, discard and retry
            if (elapsedMs >= IMMO_CHALLENGE_WINDOW_MS * 2u) {
                m_immoState      = ImmoState::TriggerSent;
                m_immoTimerTicks = 0;
            }
            break;

        case ImmoState::ChallengeComplete:
            if (elapsedMs >= IMMO_RESPONSE_DELAY_MS) {
                uint8_t response[8] = {};
                if (computeImmoResponse(m_immoChallenge, response)) {
                    sendImmoResponse(response);
                    m_immoState = ImmoState::ResponseSent;
                    efiPrintf("IMMO: authenticated");
                } else {
                    // Algorithm not yet implemented — log and stay in this state
                    // so it can be observed with the CAN sniffer.
                    efiPrintf("IMMO: challenge received but response algorithm not implemented");
                    m_immoState = ImmoState::TriggerSent;
                }
                m_immoTimerTicks = 0;
            }
            break;

        case ImmoState::Idle:
        case ImmoState::ResponseSent:
        default:
            break;
        }
    }

    /**
     * Send 0x0713 trigger frame.
     * The first frame ECU sends to initiate the IMMO session.  The exact
     * content is session-specific in original firmware (a counter + session
     * data); we use a simple rolling 16-bit counter that matches the
     * observed byte-6/byte-7 pattern from captures.
     */
    void sendImmoTrigger() {
        CanTxMessage msg(CanCategory::NBC, IMMO_ECU_ID, 8, DEFAULT_BUS_INDEX);
        // Rolling session counter in bytes 0-1 (observed pattern from captures)
        msg[0] = (uint8_t)(m_immoSessionCounter >> 8);
        msg[1] = (uint8_t)(m_immoSessionCounter & 0xFF);
        // Bytes 2-7: TODO — fill with real session data once algorithm is known.
        // For now use observed typical values from captures to look "alive".
        msg[2] = 0x00;
        msg[3] = 0x00;
        msg[4] = 0x00;
        msg[5] = 0x00;
        msg[6] = 0xC0;
        msg[7] = 0x00;
        m_immoSessionCounter++;
    }

    /**
     * Send 0x0713 response frame with the computed 8-byte answer.
     */
    void sendImmoResponse(const uint8_t response[8]) {
        CanTxMessage msg(CanCategory::NBC, IMMO_ECU_ID, 8, DEFAULT_BUS_INDEX);
        for (int i = 0; i < 8; i++) {
            msg[i] = response[i];
        }
        immoLog8("sent 0x0713 response", response);
    }

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    // True when BCM signals starter relay active (0x0350 byte 4 >= 0x80)
    bool m_starterActive = false;
    // True when BCM relay status frame confirms relay active (0x0303 byte 5 bit 1)
    bool m_relayActive   = false;

    // -----------------------------------------------------------------------
    // IMMO state
    // -----------------------------------------------------------------------
    ImmoState m_immoState        = ImmoState::Idle;
    // Ticks elapsed in current IMMO state (1 tick = 5ms)
    uint32_t  m_immoTimerTicks   = 0;
    // Rolling session counter embedded in 0x0713 trigger frames
    uint16_t  m_immoSessionCounter = 0;
    // Buffer for the 16-byte BCM challenge (two consecutive 0x0714 frames)
    uint8_t   m_immoChallenge[16] = {};
};

// ---------------------------------------------------------------------------
// Module-level static instance (no heap allocation)
// ---------------------------------------------------------------------------

static M74_9BcmCanListener m74_9BcmListener;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool m74_9_bcmStarterActive() {
    return m74_9BcmListener.isStarterActive();
}

bool m74_9_immoAuthenticated() {
    return m74_9BcmListener.isImmoAuthenticated();
}

bool m74_9_isImmobilizerBlocking() {
    // The ECU never blocks fuel/ignition on IMMO today: with m74_9ImmoOff the
    // handshake is disabled entirely (physical bypass answers the BCM), and even
    // with it enabled computeImmoResponse() is not implemented yet, so blocking
    // here would prevent every start.
    //
    // When the algorithm is ready, restore:
    //   return engineConfiguration->m74_9ImmoEnabled
    //          && !m74_9BcmListener.isImmoAuthenticated();
    return false;
}

void initM74_9Can() {
    registerCanListener(m74_9BcmListener);
    // Vehicle-speed receive + dash transmit live in m74_9_vehicle_can.cpp.
    initM749VehicleCan();
    addConsoleActionS("cantach", setM749TachEncoding);
}

#endif // EFI_CAN_SUPPORT
