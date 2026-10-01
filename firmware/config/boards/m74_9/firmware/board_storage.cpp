#include "pch.h"

#if EFI_STORAGE_MFS == TRUE
#include "hal_mfs.h"

namespace {
constexpr uint32_t FlashBankBase = 0x08200000;
constexpr uint32_t SectorSize = 4096;
constexpr uint32_t StorageBankSize = 256 * 1024;
constexpr uint32_t StorageStart = 0x08300000;
constexpr uint32_t StorageEnd = 0x08380000; // Exclusive.
constexpr uint32_t FirstSector = (StorageStart - FlashBankBase) / SectorSize;
constexpr uint32_t BankSectors = StorageBankSize / SectorSize;

static_assert(StorageStart % SectorSize == 0);
static_assert(StorageStart + 2 * StorageBankSize == StorageEnd);

// Both settings records live in MFS; these banks alternate during collection.
const MFSConfig mfsConfig = {
    .flashp = (BaseFlash *)&EFLD2,
    .erased = 0xFFFFFFFFU,
    .bank_size = StorageBankSize,
    .bank0_start = FirstSector,
    .bank0_sectors = BankSectors,
    .bank1_start = FirstSector + BankSectors,
    .bank1_sectors = BankSectors
};
}

bool boardInitMfs() {
    // Check before starting MFS: mounting blank/invalid banks can erase them.
    const auto* descriptor = flashGetDescriptor(mfsConfig.flashp);
    if (descriptor == nullptr ||
        reinterpret_cast<uintptr_t>(descriptor->address) != FlashBankBase ||
        descriptor->sectors != nullptr || descriptor->sectors_size != SectorSize ||
        descriptor->sectors_count < FirstSector + 2 * BankSectors ||
        descriptor->size < StorageEnd - FlashBankBase) {
        efiPrintf("M74.9 MFS: unsupported flash geometry");
        return false;
    }

    eflStart(&EFLD2, nullptr);
    return true;
}

const MFSConfig* boardGetMfsConfig() {
    return &mfsConfig;
}
#endif
