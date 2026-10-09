/**
 * @file injection_close_hw.h
 *
 * TIM5 CC2 mini-queue for hardware injection close timing.
 *
 * ALL PROD BUILDS: on the angle-clock build (m74_9) the injection OPEN fires
 * from the TMR2 angle clock (CC3, prio 3) and the CLOSE is queued here; on
 * time-based builds both fire from the executor. The queue absorbs
 * overlapping closes (duty overlap of adjacent cylinders, or a double-open
 * re-arm): 4 slots, one per cylinder - a slot's pendingCount>1 means a close
 * arrived while the previous one of the SAME cylinder is still pending.
 *
 * The injection OPEN fires at the correct crank angle. The injection CLOSE
 * needs to fire exactly PW_ms after the ACTUAL open, not after the SCHEDULED
 * open (which differs by TIM5 batch latency ~20 us).
 *
 * TIM5 CC2 is a 32-bit output-compare channel on the same free-running counter
 * as the NT domain (getTimeNowLowerNt() == TIM5->CNT exactly). CCR2 = fireAt_NT
 * directly - no offset conversion needed.
 *
 * The mini-queue holds 4 slots (one per cylinder). When a close is scheduled,
 * CCR2 is set to the earliest pending fireAt. The CC2 callback dispatches all
 * due closes and re-arms CC2 for the next pending one.
 *
 * Priority: the TIM5 CC2 ISR (prio 5) stands BELOW the injection-open ISR
 * (TMR2, prio 3, on the angle-clock build) that calls scheduleInjectionCloseHW,
 * so mutual exclusion is:
 *  - hwInjectionCloseCallback holds CriticalSectionLocker (BASEPRI 3 masks
 *    the higher-priority writers) around its slot/CCR updates;
 *  - scheduleInjectionCloseHW needs no lock of its own: its callers run
 *    above the CC2 ISR (the prio-3 TMR2 open ISR), or inside the executor's
 *    own critical section on time-based builds;
 *  - cancelInjectionCloseHW takes its own critical section (any context).
 */

#pragma once

#include "efitime.h"
#include "scheduler.h"

#if EFI_PROD_CODE && HAL_USE_PWM

// Schedule injection close for `cyl` at absolute NT time `nowNt + delayNt`.
// Called from turnInjectionPinHigh (the actual open callback).
// `action` is typically action_s::make<turnInjectionPinLow>(event).
void scheduleInjectionCloseHW(int cyl, efitick_t nowNt, uint32_t delayNt,
                                action_s action);
void cancelInjectionCloseHW(int cyl);
void hwInjectionCloseCallback(PWMDriver *pwmp);

// Telemetry: total CC2 ISR fires and scheduled closes.
uint32_t getInjectionCC2FiredCount();
uint32_t getInjectionCC2ScheduledCount();
// Reset the telemetry counters (m74_9 lockstats resets them so every
// snapshot is a window delta, matching the angclk counters).
void resetInjectionCC2Counters();

#endif // EFI_PROD_CODE && HAL_USE_PWM
