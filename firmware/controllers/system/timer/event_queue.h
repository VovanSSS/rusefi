/**
 * @file event_queue.h
 *
 * @date Apr 17, 2014
 * @author Andrey Belomutskiy, (c) 2012-2020
 */

#pragma once

#include "scheduler.h"
#include "utlist.h"
#include <rusefi/expected.h>

#define QUEUE_LENGTH_LIMIT 1000

/**
 * Execution sorted linked list
 */
class EventQueue {
public:
	// See comment in EventQueue::executeAll for info about lateDelay - it sets the
	// time gap between events for which we will wait instead of rescheduling the next
	// event in a group of events near one another.
	explicit EventQueue(efidur_t lateDelay = 0);

	/**
	 * O(size) - linear search in sorted linked list
	 */
	bool insertTask(scheduling_s *scheduling, efitick_t timeX, action_s const& action);
	void remove(scheduling_s* scheduling);

	int executeAll(efitick_t now);
	/**
	 * Execute the head event if it is due. When executedMomentNt/executedAtNt
	 * are provided they receive the event's scheduled moment and the time at
	 * which it actually started running (after the busy-wait), so callers can
	 * measure dispatch lateness: executedAtNt - executedMomentNt >= 0 is how
	 * late the command went out.
	 */
	bool executeOne(efitick_t now, efitick_t* executedMomentNt = nullptr, efitick_t* executedAtNt = nullptr, efitick_t* executedDurationNt = nullptr);

	/**
	 * Pop the head event WITHOUT executing it: due-check + busy-wait + unlink,
	 * the action is moved out into `outAction`. Used by the firmware executor
	 * (single_timer_executor.cpp) so the queue edits (the unlink) can run under
	 * a SHORT critical section while the callback body executes OUTSIDE the
	 * lock - the prio-3 angle-clock fires and the prio-4 trigger handoff are
	 * not masked by the prio-5 executor batch (the 2026-09-29 "fire mask" fix).
	 * Must be called under the caller's critical section; returns false when
	 * the head is absent or farther than m_lateDelay into the future.
	 */
	bool popDue(efitick_t now, action_s& outAction, efitick_t* executedMomentNt = nullptr, efitick_t* executedAtNt = nullptr);

	expected<efitick_t> getNextEventTime(efitick_t nowUs) const;
	void clear();
	int size() const;
	scheduling_s *getElementAtIndexForUnitText(int index);
	scheduling_s * getHead();

	scheduling_s* getFreeScheduling();
	void tryReturnScheduling(scheduling_s* sched);

	// Diagnostics for the fixed scheduling pool: how many pool slots are
	// currently handed out, and the worst case seen since construction.
	int getUsedPoolSlots() const { return m_usedPoolSlots; }
	int getMaxUsedPoolSlots() const { return m_maxUsedPoolSlots; }
private:
	void assertListIsSorted() const;
	/**
	 * this list is sorted
	 */
	scheduling_s *m_head = nullptr;
	const efidur_t m_lateDelay;

	scheduling_s* m_freelist = nullptr;
	int m_usedPoolSlots = 0;
	int m_maxUsedPoolSlots = 0;
	scheduling_s m_pool[64];
};

