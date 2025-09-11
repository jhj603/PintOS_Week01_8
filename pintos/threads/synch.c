/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);

	sema->holder = NULL;
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. */
/* 세마포어의 value가 양수가 될 때까지 스레드 대기시키는 함수 */
void
sema_down (struct semaphore *sema) {
	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	/* 인터럽트 비활성화 */
	enum intr_level old_level = intr_disable ();

	struct thread* cur = thread_current();

	/* While the semaphore is unavailable, wait. */
	while (sema->value == 0) {
		/* Donate priority to the semaphore holder. */
		/* Do not donate again if we are already waiting on a lock. */
		if (sema->holder && cur->wait_on_lock == NULL) {
			cur->wait_on_lock = (struct lock *) sema;
			list_insert_ordered(&sema->holder->donations, &cur->donation_elem, priority_greater, NULL);
			donate_priority(cur);
		}

		list_insert_ordered(&sema->waiters, &cur->elem, priority_greater, NULL);
		thread_block ();
	}

	/* Acquired the semaphore. Clear wait_on_lock. */
	/* This cleanup is for donations made directly by sema_down. */
	if (cur->wait_on_lock == (struct lock *) sema) {
		cur->wait_on_lock = NULL;
	}

	sema->value--;
	sema->holder = cur;

	/* 인터럽트 재활성화 */
	intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) {
	ASSERT (sema != NULL);

	enum intr_level old_level = intr_disable ();

	struct thread *cur = thread_current();

	/* If the current thread is the holder, release donations. */
	if (sema->holder == cur) {
		remove_donations((struct lock *) sema);
		refresh_priority(cur);
		sema->holder = NULL; /* Release ownership. */
	}

	/* Standard semaphore V operation. */
	sema->value++;
	if (!list_empty(&sema->waiters)) {
		/* The woken thread will become the new holder inside its sema_down(). */
		thread_unblock(list_entry(list_pop_front(&sema->waiters), struct thread, elem));
	}

	check_preemption();

	intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
	}
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
}

/* */

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));

	struct thread* cur = thread_current();

	enum intr_level	old_level = intr_disable();

	/* 락 소유자가 있을 경우에 */
	if (NULL != lock->holder)
	{
		/* 현재 스레드가 어떤 락을 기다리는지 기록 */
		cur->wait_on_lock = lock;

		/* 소유자의 donations 리스트에 현재 스레드를 추가 */
		/* release에서 현재 스레드를 찾아 지울 수 있음 */
		list_insert_ordered(&lock->holder->donations, &cur->donation_elem, priority_greater, NULL);

		donate_priority(cur);	
	}
	
	/* 락을 얻기 위해 대기 (세마포어 down). 호출 전에 우선순위 기부 로직이 수행되어야 함 */
	sema_down (&lock->semaphore);
	
	/* 락을 획득했으므로 기다리는 락 정보 해제 */
	cur->wait_on_lock = NULL;
	
	/* 락의 새 소유자를 현재 스레드로 설정 */
	lock->holder = cur;

	intr_set_level(old_level);
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success)
		lock->holder = thread_current ();
	return success;
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));

	/* 인터럽트 비활성화 */
	enum intr_level old_level = intr_disable();
	
	/* 현재 스레드의 donations 리스트에서 락과 관련된 기부 제거 */
	remove_donations(lock);

	/* 우선순위 재계산 */
	refresh_priority(thread_current());

	/* 락 해제. sema_up은 대기 중인 스레드 중 가장 높은 우선순위의 스레드를 깨움 */
	lock->holder = NULL;
	sema_up (&lock->semaphore);

	/* 락 해제 후 우선순위 변화를 반영하여 스케줄링을 재개 */
    check_preemption();

	/* 인터럽트 재활성화 */
	intr_set_level(old_level);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem {
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
	/* 세마포어를 소유하는 스레드 */
	struct thread* owner;
};

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/* semaphore_elem 삽입 시, 우선순위 순으로 정렬하면서 삽입하기 위한 비교 함수 */
static bool sema_priority_greater(const struct list_elem* a, const struct list_elem* b, void* aux UNUSED)
{
	/* a, b는 cond->waiters 리스트에 있는 semaphore_elem의 list_elem */
	struct semaphore_elem* se_a = list_entry(a, struct semaphore_elem, elem);
	struct semaphore_elem* se_b = list_entry(b, struct semaphore_elem, elem);

	/* 우선순위는 semaphore_elem 내부 semaphore와 연결된 스레드를 통해 얻을 수 있음 */
	/* semaphore_elem의 우선순위는 semaphore와 연결된 스레드의 우선순위 */
	return se_a->owner->priority > se_b->owner->priority;
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);

	/* 세마포어 소유자 설정 */
	waiter.owner = thread_current();

	//list_push_back (&cond->waiters, &waiter.elem);
	
	/* 우선순위 순으로 정렬하면서 삽입 */
	list_insert_ordered(&cond->waiters, &waiter.elem, sema_priority_greater, NULL);

	lock_release (lock);
	sema_down (&waiter.semaphore);
	lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	if (!list_empty (&cond->waiters))
		sema_up (&list_entry (list_pop_front (&cond->waiters),
					struct semaphore_elem, elem)->semaphore);
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}
