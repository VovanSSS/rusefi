/**
 * @file	microsecond_timer_stm32.cpp
 *
 * A single upcounting timer (currently TIM5) is used as a single timebase both for time
 * measurement and event scheduling.  This helps reduce jitter by not making another time
 * measurement at the time of scheduling.
 * 
 * This implementation only works on stm32 because it sets hardware registers directly.
 * ChibiOS doesn't support using timers in output compare mode, only PMW, so we have to 
 * manually configure the timer in outupt compare mode.
 * 
 * @date Dec 1, 2020
 * @author Matthew Kennedy, (c) 2012-2020
 */

#include "global.h"

#if EFI_PROD_CODE && HAL_USE_PWM

#include "port_microsecond_timer.h"
#include "injection_close_hw.h"

void portSetHardwareSchedulerTimer(efitick_t nowNt, efitick_t setTimeNt) {
	// This implementation doesn't need the current time, only the target time
	UNUSED(nowNt);

	// The hardware counter is free-running and monotonic. Write the compare exactly
	// at the requested target so in-flight events fire on schedule (no artificial
	// margin, as opposed to the old 4 us clamp which pushed every near/past target
	// a full 4 us into the future - i.e. up to 4 us late). If the target has already
	// passed by the time the channel is armed (the scheduler can go stale under
	// load), the free-running counter will never hit the written CCR - the CNT ==
	// CCR1 equality is already missed and no further interrupt would fire until the
	// 32-bit counter wraps (~18 min at 4 MHz), silently killing the whole event
	// scheduler. Force the due event immediately with a software compare (EGR CC1G)
	// instead.
	//
	// The EGR must be written AFTER pwmEnableChannelNotificationI(): that call
	// clears CC1IF (SR = ~CC1IF) when re-enabling CC1IE on the re-arm path, so a
	// forced flag written before it would be erased and the event lost. The check
	// also covers the near-future race where the counter crosses the target between
	// the CCR write and that SR-clear: by the time the check runs the target is
	// behind the counter and the EGR fires anyway, so the missed-event hang is
	// impossible regardless of how wide the arm window was or whether the target
	// was already stale on entry.
	const uint32_t compare = static_cast<uint32_t>(setTimeNt);

	pwm_lld_enable_channel(&SCHEDULER_PWM_DEVICE, 0, compare);
	pwmEnableChannelNotificationI(&SCHEDULER_PWM_DEVICE, 0);

	if (static_cast<int32_t>(static_cast<uint32_t>(setTimeNt) - getTimeNowLowerNt()) <= 0) {
		SCHEDULER_TIMER_DEVICE->EGR = STM32_TIM_EGR_CC1G;
	}
}

static void hwTimerCallback(PWMDriver*) {
	pwmDisableChannelNotificationI(&SCHEDULER_PWM_DEVICE, 0);
	portMicrosecondTimerCallback();

	assertInterruptPriority(__func__, EFI_IRQ_SCHEDULING_TIMER_PRIORITY);
}

static const PWMConfig timerConfig = {
	.frequency = SCHEDULER_TIMER_FREQ,
	/* wanted timer period = 2^32 counts,
	 * but driver set (period - 1) value to register
	 * also period is uint32_t
	 * So set it to zero so it will overlap to 0xffffffff when writen to register */
	.period = 0,
	.callback = nullptr,		// No update callback
	.channels = {
		{PWM_OUTPUT_DISABLED, hwTimerCallback},		   // CC1: NT executor
		{PWM_OUTPUT_DISABLED, hwInjectionCloseCallback},  // CC2: injection close mini-queue
		{PWM_OUTPUT_DISABLED, nullptr},
		{PWM_OUTPUT_DISABLED, nullptr}
	},
	.cr2 = 0,
	.bdtr = 0,
	.dier = 0
};

void portInitMicrosecondTimer() {
	pwmStart(&SCHEDULER_PWM_DEVICE, &timerConfig);

	// ChibiOS doesn't let you configure timers in output compare mode, only PWM mode.
	// We want to be able to set the compare register without waiting for an update event
	// (which would take 358 seconds at 12mhz timer speed), so we have to use normal upcounting
	// output compare mode instead.
	// CC1: executor (OC1M=1 = set-on-match, software comparator, CCxE=0 no pin output)
	// CC2: injection close mini-queue (OC2M=1, same mode) - on the angle-clock
	// build the OPEN fires from TMR2 and the CLOSE is queued here (2026-10-09).
	SCHEDULER_TIMER_DEVICE->CCMR1 = STM32_TIM_CCMR1_OC1M(1)
	                               | STM32_TIM_CCMR1_OC2M(1)
		;

	/* TODO: implement for all possible TIMs */
	if (SCHEDULER_TIMER_DEVICE == TIM5) {
		/* stop timers clock when core is halted */
	#if defined(STM32F4XX) || defined (STM32F7XX)
		DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_TIM5_STOP;
	#endif
	#if defined(STM32H7XX)
		DBGMCU->APB1LFZ1 |= DBGMCU_APB1LFZ1_DBG_TIM5;
	#endif
	}
}

uint32_t getTimeNowLowerNt() {
	// Using the same timer for measurement and scheduling improves
	// precision and reduces jitter.
	return SCHEDULER_TIMER_DEVICE->CNT;
}

#endif // EFI_PROD_CODE
