#pragma once

/*
 * AT32-port NVIC priority ladder (m74_9), effective 2026-09-28:
 * the angle-clock fires are ABOVE the handoff (fires 3 > handoff 4), the
 * opposite of the 2026-09-23 +1 shift (handoff 3 > fires 4), restoring
 * the pre-09-23 intent that the fire ISRs are the top kernel priority.
 *
 * 4 priority bits = 16 levels total: 0 = highest, 15 = lowest. Levels 0-2
 * are NEVER masked by the kernel (CORTEX_BASEPRI_KERNEL masks 3..15), so
 * only the ~2-3 us fast capture may live there - code that uses kernel
 * primitives (CriticalSectionLocker etc.) must stay at >= 3.
 *
 *   0  EXTI9_5   raw crank-edge capture (fast IRQ, ~2-3 us)
 *   3  TMR2      angle-clock fires: dwell start / spark / injection open+close
 *   4  I2C1_EV   trigger decode + angle-clock arming ("handoff")
 *   5  TIM5      executor CC1: overdwell rescue, MAP/knock windows,
 *                soft PWM, watchdog buddy (CC2 unused on the angle-clock build)
 *   6  TMR7      L9779 WDA feed (~100 us SPI burst every ~28 ms)
 *   7  ADC1/2/3  slow ADC (CLT/IAT/lambda/VBATT)
 *   8  TIM6      fast-ADC trigger (GPT)
 *   9  SysTick   system tick (core SysTick, SYSTICKv1 ST driver)
 *  10  SDIO      SD card
 *  11  SPI1      L9779 SPI (thread-side polled)
 *  12  CAN1      dash CAN + ISO-TP console + OpenBLT
 *  13  USART     LIN master (USART3)
 *  15  USB OTG   CDC console + USB MSD (composite)
 *
 * Only the EXTI/handoff/TIM5/angle-clock/WDA defines below are consumed by
 * rusEFI code on this port. ADC/UART/SysTick/SDMMC/SPI/CAN/USB are programmed
 * by the HARDCODED STM32_*_IRQ_PRIORITY values in the AT32 mcuconf
 * (at32/cfg/mcuconf.h) - the defines here are kept in sync for reference
 * only. The bootloader builds with the SAME headers and mcuconf (board.mk
 * CONFDIR), so ladder changes apply to it too (its EXTI stays at 6 via the
 * EFI_BOOTLOADER guard below).
 */

#ifdef EFI_BOOTLOADER
#define EFI_IRQ_EXTI_PRIORITY             6
#else
#define EFI_IRQ_EXTI_PRIORITY             CORTEX_MAXIMUM_PRIORITY
#endif

// Trigger decode + angle-clock arming (handoff). BELOW the TMR2 fires
// (prio 3) since 2026-09-28 - a fire preempts the handoff mid-decode, so the
// fire itself fires with only the ~1-2 us NVIC entry latency, never waiting
// for the decode (the fires are the real-time product: dwell/spark/injection
// pin edges; the decode is a producer that tolerates a short preemption).
// THE TRADE (vs the 2026-09-23 +1 shift): the handoff pays the fire duration
// (~10-25 us) for every collision - an arm landing past its tick (the
// tick-past / lateArm class), worst on scheduling teeth where the arm runs in
// the first ~33 us and a fire can overlap it. The fires are anchored 'from
// now' at arm time, so the dwell->spark interval is preserved, and the
// overdwell rescue + charge-anchored spark re-arm bound the consequence.
// CORTEX_MAX_KERNEL_PRIORITY is the ceiling line: both the fires (3) and the
// handoff (4) use kernel primitives (CriticalSectionLocker) so neither can
// move into the fast 0-2 levels.
#define EFI_IRQ_EXTI_HANDOFF_PRIORITY     4

// The scheduling timer (TIM5 CC1) - event executor for everything that is NOT
// on the angle clock: overdwell rescue, MAP widows, knock windows, soft PWM,
// watchdog buddy. BELOW the handoff AND below the TMR2 fires: the fires
// preempt its run-to-completion batches, so a fire's lateness is never the
// executor's queuing problem, and the executor can no longer stall the decode.
// What remains on TIM5 tolerates a delay bound by one fire + one handoff:
// the 2.5x-dwell rescue and MAP/knock windows carry millisecond margins.
// Historical: the AT32 mcuconf defines no STM32_PWM_TIM5_IRQ_PRIORITY, so the
// TIMv1 LLD defaults it to 7 - the define here is what keeps it a kernel
// priority at all.
#define EFI_IRQ_SCHEDULING_TIMER_PRIORITY 5
#define STM32_PWM_TIM5_IRQ_PRIORITY       EFI_IRQ_SCHEDULING_TIMER_PRIORITY

// The L9779 WDA feed (free-running auto-reload TMR7): BELOW the executor. Its
// ~100 us polled-SPI burst runs once per ~28 ms and must never delay the
// angle-clock fires or the executor's injection closes: at 6 the burst is
// preempted by both (and by the handoff - harmless, the SPI clock merely
// pauses mid-frame; the chip has no max tlead/tcsn, only minimums). The
// answer window is ~12.6 ms wide, so even a 1 ms preemption cannot push
// RESP_BYTE0 out of it. It stays ISR-context (never a thread): a thread
// wakeup under cranking load was the original wd_timing_miss/EC>4 kill mode.
#define EFI_IRQ_L9779_WDA_PRIORITY        6

// The following are NOT consumed on this port - the AT32 mcuconf hardcodes
// its own STM32_* values matching the ladder above (see at32/cfg/mcuconf.h);
// kept in sync for reference and for the (no-op on AT32) assertInterruptPriority.
#define EFI_IRQ_ADC_PRIORITY 7
#define EFI_IRQ_UART_PRIORITY 7
#define EFI_IRQ_SYSTICK_PRIORITY 9
#define EFI_IRQ_SDMMC_PRIORITY 11
#define EFI_IRQ_SPI_PRIORITY 11
#define EFI_IRQ_CAN_PRIORITY 12
#define EFI_IRQ_USB_PRIORITY 15
