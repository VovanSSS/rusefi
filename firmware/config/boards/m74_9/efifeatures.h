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

/* Per-tooth trigger input debounce gate (custom_board_triggerDebounceUs) is
 * compiled out: on m74_9 no board override is installed (the assignment is
 * commented out in board_configuration.cpp) yet the gate still executed a
 * get_board_override_result dispatch on EVERY decoded tooth. The gate is also
 * not wanted at high rpm (the 100 us per-polarity floor collides with the
 * 68 us half-tooth at 7000 rpm - documented in CLAUDE.md). Shared default is
 * TRUE in trigger_central.cpp; unit tests install the override at runtime and
 * keep the gate. */
#define EFI_TRIGGER_DEBOUNCE FALSE

/* Per-tooth tooth-timing plausibility check (isToothExpectedNow in
 * trigger_central.cpp, C9007/C9008) is compiled out: the check estimates the
 * current engine phase from the last tooth and rejects teeth deviating >10 deg
 * from the expected next phase, on EVERY decoded tooth (~5 us of the
 * trgPreDecode budget). m74_9 runs the same class of handoff-trim work as
 * EFI_TRIGGER_DEBOUNCE / EFI_TRIGGER_CONFIG_RUNTIME_RELOAD above; the decoder
 * already validates gaps and teeth harder than a 10 deg phase check
 * (gap-ratio windows + custom_board_syncAcceptToothLoss / syncEarlyGapWhileCranking
 * + the tooth-loss tolerance), and the L9779 VRS time filter shifts the decode
 * edge, which the check does not model. Shared default is TRUE in
 * trigger_central.cpp - other boards keep the check. */
#define EFI_TRIGGER_CHECK_TOOTH_TIMING FALSE

/* TMR2 hardware angle clock (OC channels as software comparators, events
 * programmed one tooth ahead, fired by a priority-3 ISR): removes the
 * trigger-decode/handoff latency from the spark/injection timing chain.
 * Must be defined BEFORE the stm32f4ems include - that header defaults the
 * flag to FALSE. See hw_layer/angle_clock/angle_clock.h.
 *
 * HISTORY: disabled 2026-08-30 after the first car start blew the 15A fuse
 * (C9012 out-of-order coil off + C9351-4 overcharge 4.5-8.2 ms at the
 * catch). Re-enabled 2026-08-31 on the sleep-gate theory (rccEnableTIM2
 * false->true) - the fuse blew AGAIN at the first crank attempt, so the
 * sleep gate was NOT the cause (or not the only one). Re-enabled again
 * 2026-08-31 after the redesign (angle-domain arming + per-tooth refresh,
 * charge-anchored overdwell rescue) and the car defect fix (no
 * angleClockCancelAll on the rpm==0 storm flap - see CLAUDE.md "THE CAR
 * DEFECT (2026-08-31 evening)"). Disabled again 2026-08-31 (the car
 * caught and stalled: the per-tooth basis armed the catch fires late,
 * the rescue discharged first). RE-ENABLED 2026-08-31 with the FINAL
 * basis: the angle clock now arms with the 90-degree-window rpm average
 * (oneDegreeUs - the same basis the proven time-based path uses), and
 * the overdwell rescue margin is 2.5x dwell so it never steals a fire
 * that is merely late. The per-tooth basis and its clamp are gone. With
 * the flag FALSE the early windows compile out and ignition/fuel return
 * to the proven time-based paths. */
/* DISABLED 2026-11-04: back to the standard time-based TIM5 executor path.
 * The three-timer angle clock (TMR2/TMR3/TMR4) and its .fast_text RAM code
 * are compiled out; ignition/fuel scheduling runs on TIM5 as in the stock
 * rusEFI build. ZW placement (.m749_fast_text) is kept for hot code. */
#define EFI_ANGLE_CLOCK FALSE

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
