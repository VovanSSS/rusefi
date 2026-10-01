#pragma once

#include <cstddef>
#include <cstdint>

namespace m749 {
constexpr uint32_t ProgrammingToken = 0x4DF9123B;
constexpr uint32_t ReturnToken = 0xF9C74A52;

// Only the two-byte UDS request fits the supported ISO-TP single-frame service.
// Suppressed positive responses are rejected: the OEM handoff requires a reply.
inline uint8_t sessionError(const uint8_t* data, size_t length, bool stopped) {
    if (length < 3 || length > 8 || data[0] != 2) {
        return 0x13;
    }
    if (data[2] != 2) {
        return 0x12;
    }
    return stopped ? 0 : 0x22;
}

template<class Transmit, class Reset>
bool enterProgramming(Transmit transmit, Reset reset) {
    // transmit must confirm physical completion, not just queue acceptance.
    if (!transmit()) {
        return false;
    }
    reset(ProgrammingToken);
    return true;
}

template<class Transmit, class Reset>
void handleRequest(size_t bus, uint32_t id, bool extended, bool remote,
                   const uint8_t* data, size_t length, bool stopped,
                   Transmit transmit, Reset reset) {
    if (bus != 0 || extended || remote || id != 0x7E0 || length < 2 ||
        data[1] != 0x10 || (data[0] & 0xF0) != 0) {
        return;
    }
    const auto error = sessionError(data, length, stopped);
    if (error) {
        const uint8_t response[8] = {3, 0x7F, 0x10, error, 0, 0, 0, 0};
        transmit(response);
        return;
    }
    // Session 02, P2 = 50 ms, P2* = 5000 ms (10 ms units).
    const uint8_t response[8] = {6, 0x50, 2, 0, 50, 1, 0xF4, 0};
    enterProgramming([&]() { return transmit(response); }, reset);
}
}

void initM749BootloaderHandoff();
