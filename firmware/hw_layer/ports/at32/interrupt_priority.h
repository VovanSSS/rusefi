#pragma once

#ifdef EFI_BOOTLOADER
#define EFI_IRQ_EXTI_PRIORITY             6
#else
#define EFI_IRQ_EXTI_PRIORITY             CORTEX_MAXIMUM_PRIORITY
#endif

// Trigger decode + angle-clock arming (handoff) at the HIGHEST kernel IRQ
// priority (CORTEX_MAX_KERNEL_PRIORITY = 3 on ARMv7-M), co-equal with the
// angle-clock fire ISRs. WHY (2026-09-22, supersedes the 08-23 "executor
// above decode" inverse invariant): the handoff arms the TMR2/3/4 compare
// channels from `s_edgeTimestamp + angle delay`; if a prio-3 executor batch
// (MAP windows, knock, soft PWM, watchdog, TIM5 CC2) delays the handoff past
// the armed tick, the arm fails 'tick-past' and the event falls back to TIM5
// time-based scheduling (the angclk inj/dwell lateArm stall class, proven by
// the guard=0/tickPast=100% split - up to 640 fallbacks per driving window).
// At prio 3 the handoff can only be delayed by a RUNNING prio-3 ISR body
// (angle-clock fires, ~10-25 us), never by the executor.
// CORTEX_MAX_KERNEL_PRIORITY is the ceiling: the handoff uses kernel
// primitives (CriticalSectionLocker) so it cannot move into the fast 0-2
// levels.
#define EFI_IRQ_EXTI_HANDOFF_PRIORITY     3

// The scheduling timer (TIM5 CC1/CC2) - event executor for everything that is
// NOT on the angle clock: injection close (CC2), overdwell rescue, MAP widows,
// knock windows, soft PWM, watchdog buddy. It was the highest-priority kernel
// IRQ (3) since 2026-08-23, when dwell/spark/injection themselves fired from
// TIM5 and a decode-delayed dispatch floated every command (11-30% >=10 us
// late). With the angle clock those commands moved to TMR2/3/4, so what
// remains on TIM5 tolerates a decode-duration delay: close PW error is bound
// by one handoff (~44-250 us on 2-20 ms PWs), the 2.5x-dwell rescue and MAP/
// knock windows carry millisecond margins. Dropped to BELOW the handoff
// (2026-09-22) so its run-to-completion batches no longer stall the decode.
// Historical: the AT32 mcuconf defines no STM32_PWM_TIM5_IRQ_PRIORITY, so the
// TIMv1 LLD defaults it to 7 - the define here is what keeps it a kernel
// priority at all.
#define EFI_IRQ_SCHEDULING_TIMER_PRIORITY 4
#define STM32_PWM_TIM5_IRQ_PRIORITY       EFI_IRQ_SCHEDULING_TIMER_PRIORITY

// Three hardware angle-clock timers, at the top kernel priority co-equal with
// the handoff (prio 3): their ISRs execute dwell/spark/injection actions
// directly. Same priority with the handoff means a fire pending during the
// decode waits for it (lateness up to the decode duration) - the documented
// trade of the handoff-up change; the overdwell rescue and the
// charge-anchored spark re-arm bound the consequence.
//   TMR2 (32-bit, VectorB0, IRQ 28) -> dwell starts
//   TMR4 (16-bit, VectorB8, IRQ 30) -> spark fires
//   TMR3 (16-bit, VectorB4, IRQ 29) -> injection starts
#define EFI_IRQ_ANGLE_CLOCK_PRIORITY      3

// The L9779 WDA feed (one-shot TMR10, direct registers): BELOW the trigger
// handoff on purpose - its ~100 us polled-SPI burst used to run inside the
// priority-3 executor ISR, which preempted the handoff and added up to
// 100 us to the decode/scheduling chain (the "WDA processed events at the
// wrong time" distortion). The answer window is ~12.6 ms wide, so even a
// 1 ms handoff tail preempting the burst cannot push RESP_BYTE0 out of it.
// It stays ISR-context (never a thread): a thread wakeup under cranking
// load was the original wd_timing_miss/EC>4 kill mode.
#define EFI_IRQ_L9779_WDA_PRIORITY        5
#define EFI_IRQ_ADC_PRIORITY 5
#define EFI_IRQ_UART_PRIORITY 6
#define EFI_IRQ_SYSTICK_PRIORITY 8
#define EFI_IRQ_SDMMC_PRIORITY 10
#define EFI_IRQ_SPI_PRIORITY 10
#define EFI_IRQ_CAN_PRIORITY 11
#define EFI_IRQ_USB_PRIORITY 14
