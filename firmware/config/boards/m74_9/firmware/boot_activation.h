#pragma once

#include <cstddef>
#include <cstdint>

namespace m749 {
constexpr uint32_t MarkerAddress = 0x08200000;
constexpr uint32_t NormalMarker = 0x43A0C212;
constexpr uint32_t ProgrammingMarker = 0x2548A4D2;
constexpr uint32_t I865BootCrc = 0xD7B6B894;
constexpr uint32_t I812BootCrc = 0x4F256CD9;
constexpr uint32_t ActivationDescriptor = 0x0805FFE0;
constexpr size_t MarkerPageSize = 4096;

struct ImageChecks {
    uint32_t software;
    uint32_t calibration;
    uint32_t boot;
    bool valid;
};

template<class Read>
uint32_t readWord(Read read, uint32_t address) {
    uint32_t word = 0;
    for (unsigned i = 0; i < 4; i++) {
        word |= uint32_t(read(address + i)) << (i * 8);
    }
    return word;
}

template<class Read, class Heartbeat>
uint32_t crcRange(Read read, Heartbeat heartbeat, uint32_t address, uint32_t length, uint32_t crc) {
    for (uint32_t i = 0; i < length; i++) {
        if ((i & 4095) == 0) {
            heartbeat();
        }
        crc ^= uint32_t(read(address + i)) << 24;
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc << 1) ^ ((crc & 0x80000000) ? 0x04C11DB7 : 0);
        }
    }
    return crc;
}

template<class Read, class Heartbeat>
ImageChecks checkImages(Read read, Heartbeat heartbeat) {
    auto software = crcRange(read, heartbeat, 0x08001000, 0x5F000, 0xFFFFFFFF);
    software = crcRange(read, heartbeat, 0x08080000, 0x7FFFC, software);
    auto boot = crcRange(read, heartbeat, 0x08000000, 0x1000, 0xFFFFFFFF);
    boot = crcRange(read, heartbeat, 0x08201000, 0x2CFFC, boot);
    const uint32_t calibrationStart = boot == I812BootCrc ? 0x08069000 : 0x08060000;
    auto calibration = crcRange(read, heartbeat, calibrationStart, 0x0807FFFC - calibrationStart, 0xFFFFFFFF);
    return {software, calibration, boot,
        software == readWord(read, 0x080FFFFC) && calibration == readWord(read, 0x0807FFFC) &&
        boot == readWord(read, 0x0822DFFC) && (boot == I865BootCrc || boot == I812BootCrc) &&
        readWord(read, 0x08001000) == 0x20020000 && readWord(read, 0x08001004) == 0x08080001};
}

// Flash operations are restricted to this one metadata page. Preserve every
// other byte, verify the restored body, and publish the normal marker last.
template<class Flash>
bool activate(Flash& flash, const ImageChecks& checks, uint32_t token, uint8_t* page) {
    if (!checks.valid) {
        return false;
    }
    auto read = [&](uint32_t address) { return flash.read(address); };
    const uint32_t marker = readWord(read, MarkerAddress);
    if (marker == NormalMarker) {
        return true;
    }
    if (token != 0xF9C74A52 || (marker != ProgrammingMarker && marker != 0xFFFFFFFF)) {
        return false;
    }
    for (size_t i = 0; i < MarkerPageSize; i++) {
        page[i] = read(MarkerAddress + i);
    }
    for (unsigned i = 0; i < 4; i++) {
        page[i] = uint8_t(NormalMarker >> (i * 8));
    }
    if (!flash.eraseMarkerPage() || !flash.programMarkerPage(4, page + 4, MarkerPageSize - 4)) {
        return false;
    }
    for (size_t i = 4; i < MarkerPageSize; i++) {
        if (read(MarkerAddress + i) != page[i]) {
            return false;
        }
    }
    if (!flash.programMarkerPage(0, page, 4)) {
        return false;
    }
    return readWord(read, MarkerAddress) == NormalMarker;
}
}

void initM749BootActivation();
bool m749ActivationReady();
uint32_t m749SoftwareCrc();
uint32_t m749CalibrationCrc();
