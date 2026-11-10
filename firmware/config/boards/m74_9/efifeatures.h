/**
 * @file boards/m74_9/efifeatures.h
 *
 * @brief In this header we can override efifeatures.h.
 *
 * @date Jan 01, 2023
 * @author Andrey Gusakov, 2023
 */

/* Override some settings */
#define HAL_USE_MMC_SPI FALSE
#define EFI_FILE_LOGGING FALSE

/* The application runs on top of the OpenBLT bootloader (USE_OPENBLT=yes in
 * meta-info.env). Without this define the app-side OpenBLT machinery is
 * compiled out entirely: the canOpenBLT CAN trigger, the reboot_openblt
 * console command and show_blt_version. Must be defined BEFORE the
 * stm32f4ems include - that header defaults the flag to FALSE. */
#define EFI_USE_OPENBLT TRUE

/* The .fast_text -> SRAM copy is DISABLED (2026-11-04): the hot path runs
 * from the zero-wait ZW flash window via the .m749_fast_text linker section
 * instead. ZW and SRAM are both 0-WS, so this is speed-neutral; the trade-off
 * is that ZW flash fetch stalls during any MFS write/erase (no RWW on AT32),
 * which m74_9 already avoids by deferring storage writes to engine-off. */
#define EFI_TRIGGER_IN_RAM FALSE

/* Runtime trigger-config re-read in the handoff (checkIfTriggerConfigChanged
 * on the sync tooth -> schedule rebuild + periodicFastCallback) is compiled
 * out: on m74_9 a configuration burn can only happen while the engine is
 * stopped (custom_board_allowTsBurn gates on isStopped), the trigger geometry
 * is re-read synchronously at burn time via onConfigurationChangeTriggerCallback
 * -> updateTriggerConfiguration (which also invalidates the fuel schedule),
 * and the ignition schedule is rebuilt after engine stop / at the first
 * running tooth. The trigger handoff therefore never touches the
 * configuration at runtime. See main_trigger_callback.cpp (the shared default
 * is TRUE - other boards keep the old behavior). */
#define EFI_TRIGGER_CONFIG_RUNTIME_RELOAD FALSE

/* Per-tooth tooth-timing plausibility check (isToothExpectedNow in
 * trigger_central.cpp, C9007/C9008) is compiled out: the check estimates the
 * current engine phase from the last tooth and rejects teeth deviating >10 deg
 * from the expected next phase, on EVERY decoded tooth (~5 us of the
 * trgPreDecode budget). m74_9 runs the same class of handoff-trim work as
 * EFI_TRIGGER_CONFIG_RUNTIME_RELOAD above; the decoder
 * already validates gaps and teeth harder than a 10 deg phase check
 * (gap-ratio windows + custom_board_syncAcceptToothLoss / syncEarlyGapWhileCranking
 * + the tooth-loss tolerance), and the L9779 VRS time filter shifts the decode
 * edge, which the check does not model. Shared default is TRUE in
 * trigger_central.cpp - other boards keep the check. */
#define EFI_TRIGGER_CHECK_TOOTH_TIMING FALSE

/* Console log buffer (double-buffered, 2x this, static). The stm32f4ems
 * default of 6500 truncates the 'pins' output mid-line: the pin report +
 * L9779 SPI frame history exceed it and LogBuffer::writeInternal writes
 * partial lines when full. 16384 x 2 = 32 KB static - fine on the 384 KB
 * AT32F435 (the memcore pool still has ~200 KB left). */
#define DL_OUTPUT_BUFFER 16384

#include "../../stm32f4ems/efifeatures.h"

/* This board has no USB wired out */
#undef EFI_USB_SERIAL
#define EFI_USB_SERIAL              FALSE

/* Fast ADC (TIM6 -> ADC2, MAP sampling) is enabled (default TRUE from
 * stm32f4ems) and functional: the AT32 DMA1/DMA2 DMAMUX (TBL_SEL flexible
 * mapping) is fixed in the ChibiOS fork - see
 * ChibiOS/os/hal/ports/AT32/AT32F4xx/hal_lld.c (DMA clocks before dmaInit)
 * and at32_registry.h (STM32_DMA_HAS_DMAMUXSEL). */

/* CLT/IAT (and O2/AC-pressure) live on ADC3-only pins (PF5/PF6/F10/F3) - the
 * slow ADC (ADC1) cannot see them. Sample ADC3 channels EFI_ADC_32..39 in the
 * slow loop (stm32_adc_v2.cpp convGroupSlowAdc3). */
#define EFI_ADC3_SLOW                TRUE
