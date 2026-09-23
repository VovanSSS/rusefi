/**
 * @file injection_close_hw.h
 *
 * TIM5 CC2 mini-queue for hardware injection close timing.
 *
 * The injection OPEN fires from TMR3 (or TIM5 fallback) at the correct crank
 * angle. The injection CLOSE needs to fire exactly PW_ms after the ACTUAL open,
 * not after the SCHEDULED open (which differs by TIM5 batch latency ~20 us).
 *
 * TIM5 CC2 is a 32-bit output-compare channel on the same free-running counter
 * as the NT domain (getTimeNowLowerNt() == TIM5->CNT exactly). CCR2 = fireAt_NT
 * directly - no offset conversion needed.
 *
 * The mini-queue holds 4 slots (one per cylinder). When a close is scheduled,
 * CCR2 is set to the earliest pending fireAt. The CC2 callback dispatches all
 * due closes and re-arms CC2 for the next pending one.
 *
 * Priority: since the 2026-09-23 +1 ladder shift the TMR3 Open ISR (prio 4)
 * stands ABOVE the TIM5 CC2 ISR (prio 5) and can preempt it mid-queue-update,
 * so the pre-shift 'all injection ISRs at one priority, no lock' invariant is
 * GONE. Mutual exclusion is now:
 *  - hwInjectionCloseCallback holds CriticalSectionLocker (BASEPRI 3 masks
 *    the prio-4 open ISR) around its slot/CCR updates;
 *  - scheduleInjectionCloseHW needs no lock of its own: its callers are the
 *    prio-4 open ISR (the prio-5 CC2 ISR cannot preempt it) and the TIM5
 *    executor dispatch (prio 5, already inside the executor's critical
 *    section);
 *  - cancelInjectionCloseHW takes its own critical section (any context).
 */

#pragma once

#include "efitime.h"
#include "scheduler.h"

#if EFI_PROD_CODE && HAL_USE_PWM

// Schedule injection close for `cyl` at absolute NT time `nowNt + delayNt`.
// Called from turnInjectionPinHigh (TMR3 open ISR, prio 4, or the TIM5
// executor dispatch at prio 5 under its critical section).
// `action` is typically action_s::make<turnInjectionPinLow>(event).
void scheduleInjectionCloseHW(int cyl, efitick_t nowNt, uint32_t delayNt,
                                action_s action);
void cancelInjectionCloseHW(int cyl);
void hwInjectionCloseCallback(PWMDriver *pwmp);

// Telemetry: total CC2 ISR fires and scheduled closes.
uint32_t getInjectionCC2FiredCount();
uint32_t getInjectionCC2ScheduledCount();

#endif // EFI_PROD_CODE && HAL_USE_PWM
