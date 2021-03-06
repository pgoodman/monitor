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
#include "devices/timer.h"
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
sema_init (struct semaphore *sema, unsigned value) 
{
  ASSERT (sema != NULL);

  sema->value = value;
  list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. */
void
sema_down (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0) 
    {
      list_push_back (&sema->waiters, &thread_current ()->elem);
      thread_block ();
    }
  sema->value--;
  intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) 
{
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
sema_up (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  sema->value++;
  if (!list_empty (&sema->waiters)) 
    {
      /* Find highest-priority waiting thread. */
      struct thread *max = list_entry (list_max (&sema->waiters,
                                                 thread_lower_priority, NULL),
                                       struct thread, elem);

      /* Remove `max' from wait list and unblock. */
      list_remove (&max->elem);
      thread_unblock (max);

      /* Yield to a higher-priority thread, if we're running in a
         context where it makes sense to do so.
         
         Kind of a funny interaction with donation here.
         We only support donation for locks, and locks turn off
         interrupts before calling us, so we automatically don't
         do the yield here, delegating to lock_release(). */
      if (!intr_context () && old_level == INTR_ON)
        thread_yield_to_higher_priority ();
    }
  intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) 
{
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
sema_test_helper (void *sema_) 
{
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
lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);

  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock)
{
  enum intr_level old_level;

  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  old_level = intr_disable ();

  if (lock->holder != NULL) 
    {
      /* Donate our priority to the thread holding the lock.
         First, update the data structures. */
      struct thread *donor = thread_current ();
      donor->want_lock = lock;
      donor->donee = lock->holder;
      list_push_back (&lock->holder->donors, &donor->donor_elem);
      
      /* Now implement the priority donation itself
         by recomputing the donee's priority
         and cascading the donation as far as necessary. */
      if (donor->donee != NULL)
        thread_recompute_priority (donor->donee);
    }

  sema_down (&lock->semaphore);
  lock->holder = thread_current ();
  intr_set_level (old_level);
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock)
{
  bool success;

  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));

  success = sema_try_down (&lock->semaphore);
  if (success)
    lock->holder = thread_current ();
  return success;
}

/* Releases LOCK, which must be owned by the current thread.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) 
{
  enum intr_level old_level;
  struct thread *t = thread_current ();
  struct list_elem *e;

  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  old_level = intr_disable ();

  /* Return donations to threads that want this lock. */
  for (e = list_begin (&t->donors); e != list_end (&t->donors); ) 
    {
      struct thread *donor = list_entry (e, struct thread, donor_elem);
      if (donor->want_lock == lock) 
        {
          donor->donee = NULL;
          e = list_remove (e);
        }
      else
        e = list_next (e);
    }

  /* Release lock. */
  lock->holder = NULL;
  sema_up (&lock->semaphore);

  /* Recompute our priority based on our remaining donations,
     then yield to a higher-priority ready thread if one now
     exists. */
  thread_recompute_priority (t);
  thread_yield_to_higher_priority ();

  intr_set_level (old_level);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) 
{
  ASSERT (lock != NULL);

  return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem 
  {
    struct list_elem elem;              /* List element. */
    struct list_elem t_elem;            /* List element for timed semaphores. */
    struct semaphore semaphore;         /* This semaphore. */
    struct thread *thread;              /* Thread. */
		int64_t remaining;									/* Time remaining (in nanoseconds).
																					 -1 if never times out. */
  };

static struct list timed_semaphores;

void
synch_init(void)
{
	list_init(&timed_semaphores);
}

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);

  list_init (&cond->waiters);
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
cond_wait (struct condition *cond, struct lock *lock) 
{
  struct semaphore_elem waiter;

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));
  
  sema_init (&waiter.semaphore, 0);
	waiter.remaining = -1;
  waiter.thread = thread_current ();
  list_push_back (&cond->waiters, &waiter.elem);
  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

static bool
semaphore_elem_lower_priority (const struct list_elem *a_,
                               const struct list_elem *b_,
                               void *aux UNUSED) 
{
  const struct semaphore_elem *a
    = list_entry (a_, struct semaphore_elem, elem);
  const struct semaphore_elem *b
    = list_entry (b_, struct semaphore_elem, elem);

  return a->thread->priority < b->thread->priority;
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  if (!list_empty (&cond->waiters)) 
    {
      struct list_elem *max
        = list_max (&cond->waiters, semaphore_elem_lower_priority, NULL);
      list_remove (max);
      sema_up (&list_entry (max, struct semaphore_elem, elem)->semaphore); 
    }
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);

  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}

/* Same as cond_wait; except that instead of using a lock to ensure mutual
 * exclusion, we ensure that this function is called only with interrupts
 * disabled. This function must not be called from within an interrupt handler.
 *
 * This function must be called with interrupts disabled. If it goes to sleep,
 * it will enable interrupts before sleeping.
 */
void
cond_wait_intr (struct condition *cond) 
{
  struct semaphore_elem waiter;

  ASSERT (cond != NULL);
  ASSERT (intr_get_level() == INTR_OFF);
  ASSERT (!intr_context());
  
  sema_init (&waiter.semaphore, 0);
  waiter.remaining = -1;
  list_push_back (&cond->waiters, &waiter.elem);
  printf("%s() %d:\n", __func__, __LINE__);
  intr_enable();
  sema_down (&waiter.semaphore);
  intr_disable();
  printf("%s() %d:\n", __func__, __LINE__);
}

/* Same as cond_timed_wait; except that it uses disabled interrupts for
 * mutual exclusion, instead of a lock. */
bool
cond_timed_wait_intr (struct condition *cond, int64_t nanoseconds)
{
  struct semaphore_elem waiter;

  ASSERT (cond != NULL);
  ASSERT (!intr_context ());
  ASSERT (intr_get_level() == INTR_OFF);
  ASSERT (nanoseconds > 0);
  
  sema_init (&waiter.semaphore, 0);
  list_push_back (&cond->waiters, &waiter.elem);

  /* timed_semaphore_mutex protexts timed_semaphores list and the "remaining"
   * field of the waiter. */
  //lock_acquire(&timed_semaphores_mutex);
  //waiter.remaining = nanoseconds;
  waiter.remaining = nanoseconds /* XXX */ * 1000000ULL;
  list_push_back(&timed_semaphores, &waiter.t_elem);

  intr_enable();
  sema_down (&waiter.semaphore);
  intr_disable();
  if (waiter.remaining > 0) {
    //lock_release(&timed_semaphores_mutex);
    return false;
  } else {
    /* timed out. remove from cond's waiter list. */
    ASSERT(waiter.remaining == 0);
    list_remove(&waiter.elem);
    //lock_release(&timed_semaphores_mutex);
    return true;
  }
}


/* Same as cond_signal; but instead of using a lock to ensure mutual exclusion,
 * this function must always be called with interrupts disabled.
 */
void
cond_signal_intr (struct condition *cond) 
{
  enum intr_level old_level;

  ASSERT (cond != NULL);
  ASSERT (intr_get_level() == INTR_OFF);

  if (!list_empty (&cond->waiters)) {
    struct semaphore_elem *waiter;
    waiter = list_entry (list_pop_front (&cond->waiters),
                         struct semaphore_elem, elem);
    sema_up (&waiter->semaphore);
    /* timed_semaphore_mutex protects both the timed_semaphore list and the
     * "remaining" field of the waiter. */
    //lock_acquire(&timed_semaphores_mutex);
    old_level = intr_disable();
    if (waiter->remaining > 0) {
      /* It has not timed out. */
      list_remove(&waiter->t_elem);
    }
    //lock_release(&timed_semaphores_mutex);
    intr_set_level(old_level);
  }
}

void
timed_semaphores_tick(void)
{
  struct list_elem *e;
  static int64_t nanoseconds = 1e+9/TIMER_FREQ;

  for (e = list_begin(&timed_semaphores);
      e != list_end(&timed_semaphores);
      e = list_next(e)) {
    struct semaphore_elem *se;
    se = list_entry(e, struct semaphore_elem, t_elem);
    if (se->remaining > nanoseconds) {
      se->remaining -= nanoseconds;
      ASSERT(se->remaining > 0);
    } else {
      se->remaining = 0;
      list_remove(&se->t_elem);
      sema_up (&se->semaphore);
    }
  }
}


