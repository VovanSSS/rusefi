#include "pch.h"
#include "boot_activation.h"

static_assert(STM32_PWM_TIM5_IRQ_PRIORITY == EFI_IRQ_SCHEDULING_TIMER_PRIORITY,
    "M74.9 TIM5 interrupt priority must match the scheduler contract");

// Boot intent survives across reset (SRAM 0x20000000, .boot_intent NOLOAD).
// Defined here so both the OpenBLT and main-app builds own the symbol; the
// main app's bootloader_handoff.cpp references it extern.
__attribute__((section(".boot_intent"), used)) volatile uint32_t m749BootIntent;

// Address and bytes are part of the CLI/firmware ABI. The linker reserves them.
__attribute__((section(".m749_activation"), used))
const uint32_t m749ActivationAbi[8] = {
    0x3934374D, 0x32544341, 2, 1,
    m749::I865BootCrc, 0x08060000, m749::I812BootCrc, 0x08069000
};

static m749::ImageChecks checks;
static bool ready;
static uint8_t savedMarkerPage[m749::MarkerPageSize];

static void feedWatchdog() {
    // Reload only: does not start or reconfigure an inactive watchdog.
    IWDG->KR = 0xAAAA;
}

// Bank 1 executes throughout. No generic flash/MFS API can reach loader/NVM.
// AT32F435/437 RM section 5; bank-2 operation sequence matches Artery's SDK.
class MarkerFlash {
    static constexpr uint32_t errors = FLASH_STS_PRGMERR | FLASH_STS_EPPERR;

    bool wait() {
        const auto start = chVTGetSystemTimeX();
        while (FLASH2->STS & FLASH_STS_OBF) {
            if (chVTTimeElapsedSinceX(start) >= TIME_MS2I(2'000)) {
                return false;
            }
            feedWatchdog();
            chThdSleepMilliseconds(1);
        }
        return (FLASH2->STS & errors) == 0;
    }

public:
    uint8_t read(uint32_t address) const {
        return *reinterpret_cast<const volatile uint8_t*>(address);
    }

    bool eraseMarkerPage() {
        if (!wait()) {
            return false;
        }
        FLASH2->KEYR = 0x45670123;
        FLASH2->KEYR = 0xCDEF89AB;
        if (FLASH2->CTRL & FLASH_CTRL_LOCK) {
            return false;
        }
        FLASH2->STS = errors | FLASH_STS_ODF;
        FLASH2->CTRL = FLASH_CTRL_SECERS;
        FLASH2->ADDR = m749::MarkerAddress;
        FLASH2->CTRL |= FLASH_CTRL_ERSTR;
        const bool ok = wait();
        FLASH2->CTRL = FLASH_CTRL_LOCK;
        if (!ok) {
            return false;
        }
        for (size_t i = 0; i < m749::MarkerPageSize; i++) {
            if (read(m749::MarkerAddress + i) != 0xFF) {
                return false;
            }
        }
        return true;
    }

    bool programMarkerPage(size_t offset, const uint8_t* data, size_t length) {
        if ((offset & 1) || (length & 1) || offset > m749::MarkerPageSize ||
            length > m749::MarkerPageSize - offset || !wait()) {
            return false;
        }
        FLASH2->KEYR = 0x45670123;
        FLASH2->KEYR = 0xCDEF89AB;
        if (FLASH2->CTRL & FLASH_CTRL_LOCK) {
            return false;
        }
        FLASH2->STS = errors | FLASH_STS_ODF;
        FLASH2->CTRL = FLASH_CTRL_PRGM;
        bool ok = true;
        for (size_t i = 0; i < length; i += 2) {
            const uint16_t value = uint16_t(data[i]) | uint16_t(data[i + 1]) << 8;
            if (value == 0xFFFF) {
                continue;
            }
            auto address = reinterpret_cast<volatile uint16_t*>(m749::MarkerAddress + offset + i);
            *address = value;
            if (!wait() || *address != value) {
                ok = false;
                break;
            }
            feedWatchdog();
        }
        FLASH2->CTRL = FLASH_CTRL_LOCK;
        return ok;
    }
};

void initM749BootActivation() {
    // Called after HAL/RT init, before board actuator initialization.
    MarkerFlash flash;
    checks = m749::checkImages([&](uint32_t address) { return flash.read(address); }, feedWatchdog);
    ready = m749::activate(flash, checks, m749BootIntent, savedMarkerPage);
    if (!ready) {
#if EFI_BOOTLOADER
        // OpenBLT stays in XCP mode on an invalid image: CpuStartUserProgram()
        // keeps it there via NvmVerifyChecksum(), and the host can re-flash.
#else
        // An incomplete/corrupt image never starts engine control or publishes validity.
        __disable_irq();
        m749BootIntent = 0x4DF9123B;
        __DSB();
        NVIC_SystemReset();
#endif
    }
    m749BootIntent = 0;
}

bool m749ActivationReady() { return ready; }
uint32_t m749SoftwareCrc() { return checks.software; }
uint32_t m749CalibrationCrc() { return checks.calibration; }
