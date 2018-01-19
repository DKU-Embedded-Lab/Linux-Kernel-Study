/*
 * kernel/locking/mutex.c
 *
 * Mutexes: blocking mutual exclusion locks
 *
 * Started by Ingo Molnar:
 *
 *  Copyright (C) 2004, 2005, 2006 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *
 * Many thanks to Arjan van de Ven, Thomas Gleixner, Steven Rostedt and
 * David Howells for suggestions and improvements.
 *
 *  - Adaptive spinning for mutexes by Peter Zijlstra. (Ported to mainline
 *    from the -rt tree, where it was originally implemented for rtmutexes
 *    by Steven Rostedt, based on work by Gregory Haskins, Peter Morreale
 *    and Sven Dietrich.
 *
 * Also see Documentation/locking/mutex-design.txt.
 */
#include <linux/mutex.h>
#include <linux/ww_mutex.h>
#include <linux/sched/signal.h>
#include <linux/sched/rt.h>
#include <linux/sched/wake_q.h>
#include <linux/sched/debug.h>
#include <linux/export.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/debug_locks.h>
#include <linux/osq_lock.h>

#ifdef CONFIG_DEBUG_MUTEXES
# include "mutex-debug.h"
#else
# include "mutex.h"
#endif

void
__mutex_init(struct mutex *lock, const char *name, struct lock_class_key *key)
{
	atomic_long_set(&lock->owner, 0);
	spin_lock_init(&lock->wait_lock);
	INIT_LIST_HEAD(&lock->wait_list);
#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
	osq_lock_init(&lock->osq);
#endif

	debug_mutex_init(lock, name, key);
}
EXPORT_SYMBOL(__mutex_init);

/*
 * @owner: contains: 'struct task_struct *' to the current lock owner,
 * NULL means not owned. Since task_struct pointers are aligned at
 * at least L1_CACHE_BYTES, we have low bits to store extra state.
 *
 * Bit0 indicates a non-empty waiter list; unlock must issue a wakeup.
 * Bit1 indicates unlock needs to hand the lock to the top-waiter
 * Bit2 indicates handoff has been done and we're waiting for pickup.
 */
#define MUTEX_FLAG_WAITERS	0x01
#define MUTEX_FLAG_HANDOFF	0x02
#define MUTEX_FLAG_PICKUP	0x04

#define MUTEX_FLAGS		0x07

static inline struct task_struct *__owner_task(unsigned long owner)
{
	return (struct task_struct *)(owner & ~MUTEX_FLAGS);
}

static inline unsigned long __owner_flags(unsigned long owner)
{
	return owner & MUTEX_FLAGS;
}

/*
 * Trylock variant that retuns the owning task on failure.
 */
static inline struct task_struct *__mutex_trylock_or_owner(struct mutex *lock)
{
	unsigned long owner, curr = (unsigned long)current;

	owner = atomic_long_read(&lock->owner);
	for (;;) { /* must loop, can race against a flag */
		unsigned long old, flags = __owner_flags(owner); 
        // owner 에서 0~2 bit 에 설정된 flag 값 가져옴
		unsigned long task = owner & ~MUTEX_FLAGS;
        // owner 에서 task_strut 의 주소값 가져옴

		if (task) {
            // owner 가 있는 상태라면 즉 LOCK 이 잡혀 있는 상태라면
			if (likely(task != curr))
				break;
            // 현재 lock 접근하는 task가 lock owner 와 
            // 다르면 lock 얻을 수 없는 일반적 경우로 owner 반환하고 종료
            // 같으면...
			if (likely(!(flags & MUTEX_FLAG_PICKUP)))
				break;
            // 같은 owner 가 같은 lock 을 얻으려 한는 경우라면 
            // MUTEX_FLAG_PICKUP 이 설정되어 있어야 함 
			flags &= ~MUTEX_FLAG_PICKUP; 
            // ok... 같은 lock 접근 인 경우, lock 획득 가능하기 위한 flag 를 
            // 확인하였으므로 이제 flag 에서 MUTEX_FLAG_PICKUP 삭제해 줌
		} else {
#ifdef CONFIG_DEBUG_MUTEXES
			DEBUG_LOCKS_WARN_ON(flags & MUTEX_FLAG_PICKUP);
#endif
		}

		/*
		 * We set the HANDOFF bit, we must make sure it doesn't live
		 * past the point where we acquire it. This would be possible
		 * if we (accidentally) set the bit on an unlocked mutex.
		 */ 
		flags &= ~MUTEX_FLAG_HANDOFF;
        // 이제 여기는 두가지 경우
        //  - lock owner 가 없는 경우 
        //  - lock owner 가 자기 자신이며 MUTEX_FLAG_PICKUP 이 설정되어 있던
        //    경우
		old = atomic_long_cmpxchg_acquire(&lock->owner, owner, curr | flags);
        // current thread 가 lock 잡도록 수행
		if (old == owner)
			return NULL;
        // lock 잡으면

		owner = old;
	}

	return __owner_task(owner);
    // lock->owner 에서 task_struct 만 반환
}

/*
 * Actual trylock that will work on any unlocked state.
 */
static inline bool __mutex_trylock(struct mutex *lock)
{
	return !__mutex_trylock_or_owner(lock);
}

#ifndef CONFIG_DEBUG_LOCK_ALLOC
/*
 * Lockdep annotations are contained to the slow paths for simplicity.
 * There is nothing that would stop spreading the lockdep annotations outwards
 * except more code.
 */

/*
 * Optimistic trylock that only works in the uncontended case. Make sure to
 * follow with a __mutex_trylock() before failing.
 */
static __always_inline bool __mutex_trylock_fast(struct mutex *lock)
{
	unsigned long curr = (unsigned long)current;

	if (!atomic_long_cmpxchg_acquire(&lock->owner, 0UL, curr))
		return true; 
    // cmpxchg 연산을 수행, lock owner 가 0 과 같다면 즉 mutex 에 대한 
    // 소유자가 없어서lock 을 잡을 수 있는 상황이므로 currrent task_struct 의 
    // 주소를 owner 에 설정 하고 빠르게 끝 
    // fastpath 에서 lock 잡기 성공!!

	return false;
}

static __always_inline bool __mutex_unlock_fast(struct mutex *lock)
{
	unsigned long curr = (unsigned long)current;

	if (atomic_long_cmpxchg_release(&lock->owner, curr, 0UL) == curr)
		return true;
    // 현재 mutex_unlock 호출한 thread 가 lock holder 이며 mutex flag 따로 
    // 설정이 안되어 있다면 owner 에 0 을 씀 
    //  - MUTEX_FLAG_WAITERS 설정 안되어 있어야 함
    //    => 있다면 wakeup 해주어야..
    //  - MUTEX_FLAG_HANDOFF 설정 안되어 있어야 함
    //    => 있다면 diret 로 wait_list 의 첫번째 놈에게 전달해 주어야..

	return false;
}
#endif

static inline void __mutex_set_flag(struct mutex *lock, unsigned long flag)
{
	atomic_long_or(flag, &lock->owner);
}

static inline void __mutex_clear_flag(struct mutex *lock, unsigned long flag)
{
	atomic_long_andnot(flag, &lock->owner);
}

// mutex_waiter 가 지금 wait_list  의 첫번째 entry인가? 
static inline bool __mutex_waiter_is_first(struct mutex *lock, struct mutex_waiter *waiter)
{
	return list_first_entry(&lock->wait_list, struct mutex_waiter, list) == waiter;
}

/*
 * Give up ownership to a specific task, when @task = NULL, this is equivalent
 * to a regular unlock. Sets PICKUP on a handoff, clears HANDOF, preserves
 * WAITERS. Provides RELEASE semantics like a regular unlock, the
 * __mutex_trylock() provides a matching ACQUIRE semantics for the handoff.
 */
static void __mutex_handoff(struct mutex *lock, struct task_struct *task)
{
	unsigned long owner = atomic_long_read(&lock->owner);

	for (;;) {
		unsigned long old, new;

#ifdef CONFIG_DEBUG_MUTEXES
		DEBUG_LOCKS_WARN_ON(__owner_task(owner) != current);
		DEBUG_LOCKS_WARN_ON(owner & MUTEX_FLAG_PICKUP);
#endif

		new = (owner & MUTEX_FLAG_WAITERS);
		new |= (unsigned long)task;
		if (task)
			new |= MUTEX_FLAG_PICKUP;
            // MUTEX_FLAG_PICKUP 설정 추가

		old = atomic_long_cmpxchg_release(&lock->owner, owner, new);
		if (old == owner)
			break;

		owner = old;
	}
}

#ifndef CONFIG_DEBUG_LOCK_ALLOC
/*
 * We split the mutex lock/unlock logic into separate fastpath and
 * slowpath functions, to reduce the register pressure on the fastpath.
 * We also put the fastpath first in the kernel image, to make sure the
 * branch is predicted by the CPU as default-untaken.
 */
static void __sched __mutex_lock_slowpath(struct mutex *lock);

/**
 * mutex_lock - acquire the mutex
 * @lock: the mutex to be acquired
 *
 * Lock the mutex exclusively for this task. If the mutex is not
 * available right now, it will sleep until it can get it.
 *
 * The mutex must later on be released by the same task that
 * acquired it. Recursive locking is not allowed. The task
 * may not exit without first unlocking the mutex. Also, kernel
 * memory where the mutex resides must not be freed with
 * the mutex still locked. The mutex must first be initialized
 * (or statically defined) before it can be locked. memset()-ing
 * the mutex to 0 is not allowed.
 *
 * ( The CONFIG_DEBUG_MUTEXES .config option turns on debugging
 *   checks that will enforce the restrictions and will also do
 *   deadlock debugging. )
 *
 * This function is similar to (but not equivalent to) down().
 */
void __sched mutex_lock(struct mutex *lock)
{
	might_sleep();
    // debugging 목적 설정, 선점가능하도록 실행도중 scheduling 가능하도록 설정
	if (!__mutex_trylock_fast(lock)) // fast path 로 lock 을 얻으려 시도
		__mutex_lock_slowpath(lock);
    // 바로 lock 획득 불가능 한 경우, midpath/slowpath 로..
}
EXPORT_SYMBOL(mutex_lock);
#endif

static __always_inline void
ww_mutex_lock_acquired(struct ww_mutex *ww, struct ww_acquire_ctx *ww_ctx)
{
#ifdef CONFIG_DEBUG_MUTEXES
	/*
	 * If this WARN_ON triggers, you used ww_mutex_lock to acquire,
	 * but released with a normal mutex_unlock in this call.
	 *
	 * This should never happen, always use ww_mutex_unlock.
	 */
	DEBUG_LOCKS_WARN_ON(ww->ctx);

	/*
	 * Not quite done after calling ww_acquire_done() ?
	 */
	DEBUG_LOCKS_WARN_ON(ww_ctx->done_acquire);

	if (ww_ctx->contending_lock) {
		/*
		 * After -EDEADLK you tried to
		 * acquire a different ww_mutex? Bad!
		 */
		DEBUG_LOCKS_WARN_ON(ww_ctx->contending_lock != ww);

		/*
		 * You called ww_mutex_lock after receiving -EDEADLK,
		 * but 'forgot' to unlock everything else first?
		 */
		DEBUG_LOCKS_WARN_ON(ww_ctx->acquired > 0);
		ww_ctx->contending_lock = NULL;
	}

	/*
	 * Naughty, using a different class will lead to undefined behavior!
	 */
	DEBUG_LOCKS_WARN_ON(ww_ctx->ww_class != ww->ww_class);
#endif
	ww_ctx->acquired++;
}

static inline bool __sched
__ww_ctx_stamp_after(struct ww_acquire_ctx *a, struct ww_acquire_ctx *b)
{
	return a->stamp - b->stamp <= LONG_MAX &&
	       (a->stamp != b->stamp || a > b);
}

/*
 * Wake up any waiters that may have to back off when the lock is held by the
 * given context.
 *
 * Due to the invariants on the wait list, this can only affect the first
 * waiter with a context.
 *
 * The current task must not be on the wait list.
 */
static void __sched
__ww_mutex_wakeup_for_backoff(struct mutex *lock, struct ww_acquire_ctx *ww_ctx)
{
	struct mutex_waiter *cur;

	lockdep_assert_held(&lock->wait_lock);

	list_for_each_entry(cur, &lock->wait_list, list) {
		if (!cur->ww_ctx)
			continue;

		if (cur->ww_ctx->acquired > 0 &&
		    __ww_ctx_stamp_after(cur->ww_ctx, ww_ctx)) {
			debug_mutex_wake_waiter(lock, cur);
			wake_up_process(cur->task);
		}

		break;
	}
}

/*
 * After acquiring lock with fastpath or when we lost out in contested
 * slowpath, set ctx and wake up any waiters so they can recheck.
 */
static __always_inline void
ww_mutex_set_context_fastpath(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	ww_mutex_lock_acquired(lock, ctx);

	lock->ctx = ctx;

	/*
	 * The lock->ctx update should be visible on all cores before
	 * the atomic read is done, otherwise contended waiters might be
	 * missed. The contended waiters will either see ww_ctx == NULL
	 * and keep spinning, or it will acquire wait_lock, add itself
	 * to waiter list and sleep.
	 */
	smp_mb(); /* ^^^ */

	/*
	 * Check if lock is contended, if not there is nobody to wake up
	 */
	if (likely(!(atomic_long_read(&lock->base.owner) & MUTEX_FLAG_WAITERS)))
		return;

	/*
	 * Uh oh, we raced in fastpath, wake up everyone in this case,
	 * so they can see the new lock->ctx.
	 */
	spin_lock(&lock->base.wait_lock);
	__ww_mutex_wakeup_for_backoff(&lock->base, ctx);
	spin_unlock(&lock->base.wait_lock);
}

/*
 * After acquiring lock in the slowpath set ctx.
 *
 * Unlike for the fast path, the caller ensures that waiters are woken up where
 * necessary.
 *
 * Callers must hold the mutex wait_lock.
 */
static __always_inline void
ww_mutex_set_context_slowpath(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	ww_mutex_lock_acquired(lock, ctx);
	lock->ctx = ctx;
}

#ifdef CONFIG_MUTEX_SPIN_ON_OWNER

static inline
bool ww_mutex_spin_on_owner(struct mutex *lock, struct ww_acquire_ctx *ww_ctx,
			    struct mutex_waiter *waiter)
{
	struct ww_mutex *ww;

	ww = container_of(lock, struct ww_mutex, base);

	/*
	 * If ww->ctx is set the contents are undefined, only
	 * by acquiring wait_lock there is a guarantee that
	 * they are not invalid when reading.
	 *
	 * As such, when deadlock detection needs to be
	 * performed the optimistic spinning cannot be done.
	 *
	 * Check this in every inner iteration because we may
	 * be racing against another thread's ww_mutex_lock.
	 */
	if (ww_ctx->acquired > 0 && READ_ONCE(ww->ctx))
		return false;

	/*
	 * If we aren't on the wait list yet, cancel the spin
	 * if there are waiters. We want  to avoid stealing the
	 * lock from a waiter with an earlier stamp, since the
	 * other thread may already own a lock that we also
	 * need.
	 */
	if (!waiter && (atomic_long_read(&lock->owner) & MUTEX_FLAG_WAITERS))
		return false;

	/*
	 * Similarly, stop spinning if we are no longer the
	 * first waiter.
	 */
	if (waiter && !__mutex_waiter_is_first(lock, waiter))
		return false;

	return true;
}

/*
 * Look out! "owner" is an entirely speculative pointer access and not
 * reliable.
 *
 * "noinline" so that this function shows up on perf profiles.
 */
static noinline
bool mutex_spin_on_owner(struct mutex *lock, struct task_struct *owner,
			 struct ww_acquire_ctx *ww_ctx, struct mutex_waiter *waiter)
{
	bool ret = true;

	rcu_read_lock();
	while (__mutex_owner(lock) == owner) {
		/*
		 * Ensure we emit the owner->on_cpu, dereference _after_
		 * checking lock->owner still matches owner. If that fails,
		 * owner might point to freed memory. If it still matches,
		 * the rcu_read_lock() ensures the memory stays valid.
		 */
		barrier();

		/*
		 * Use vcpu_is_preempted to detect lock holder preemption issue.
		 */
		if (!owner->on_cpu || need_resched() ||
				vcpu_is_preempted(task_cpu(owner))) {
			ret = false;
			break;
		}

		if (ww_ctx && !ww_mutex_spin_on_owner(lock, ww_ctx, waiter)) {
			ret = false;
			break;
		}

		cpu_relax();
	}
	rcu_read_unlock();

	return ret;
}

/*
 * Initial check for entering the mutex spinning loop
 */
static inline int mutex_can_spin_on_owner(struct mutex *lock)
{
	struct task_struct *owner;
	int retval = 1;

	if (need_resched())
		return 0;
    // 현재 task 가 CPU yield 요청 즉 preemption 요청이 설정되 있지 않아야 
    // midpath 가 실행될 수 있음 
    // 즉 현재 task 보다 우선순위가 높은 task 에 의해 
    // CPU 요청이 있는 상태라면 midpath 포기하고, slowpath 로 전환
	rcu_read_lock();
	owner = __mutex_owner(lock);

	/*
	 * As lock holder preemption issue, we both skip spinning if task is not
	 * on cpu or its cpu is preempted
	 */
	if (owner)
		retval = owner->on_cpu && !vcpu_is_preempted(task_cpu(owner)); 
    // 아래 조건을 만족해야
    //  - lock 이 잡혀 있는 경우, lock 잡고있는 놈이 돌고 있는 thread 가 
    //    running 상태여야 함

	rcu_read_unlock();

	/*
	 * If lock->owner is not set, the mutex has been released. Return true
	 * such that we'll trylock in the spin path, which is a faster option
	 * than the blocking slow path.
	 */
	return retval;
}

/*
 * Optimistic spinning.
 *
 * We try to spin for acquisition when we find that the lock owner
 * is currently running on a (different) CPU and while we don't
 * need to reschedule. The rationale is that if the lock owner is
 * running, it is likely to release the lock soon.
 *
 * The mutex spinners are queued up using MCS lock so that only one
 * spinner can compete for the mutex. However, if mutex spinning isn't
 * going to happen, there is no point in going through the lock/unlock
 * overhead.
 *
 * Returns true when the lock was taken, otherwise false, indicating
 * that we need to jump to the slowpath and sleep.
 *
 * The waiter flag is set to true if the spinner is a waiter in the wait
 * queue. The waiter-spinner will spin on the lock directly and concurrently
 * with the spinner at the head of the OSQ, if present, until the owner is
 * changed to itself.
 */
static __always_inline bool
mutex_optimistic_spin(struct mutex *lock, struct ww_acquire_ctx *ww_ctx,
		      const bool use_ww_ctx, struct mutex_waiter *waiter)
{
	if (!waiter) {
		/*
		 * The purpose of the mutex_can_spin_on_owner() function is
		 * to eliminate the overhead of osq_lock() and osq_unlock()
		 * in case spinning isn't possible. As a waiter-spinner
		 * is not going to take OSQ lock anyway, there is no need
		 * to call mutex_can_spin_on_owner().
		 */
		if (!mutex_can_spin_on_owner(lock))
			goto fail;
        // 아래 조건을 만족하는 상황만 midpath 가능
        //  - 현재 process 가 CPU 를 yield 하겠다는 선점요청을 한 
        //    상태가 아니여야 함
        //  - 이미 lock 잡고있는 thread 가 다른 CPU 에서 돌고 있는 중이어야함 
        //
        //  위에 해당되지 않는 놈을 미리 거름 
		/*
		 * In order to avoid a stampede of mutex spinners trying to
		 * acquire the mutex all at once, the spinners need to take a
		 * MCS (queued) lock first before spinning on the owner field.
		 */
		if (!osq_lock(&lock->osq))
			goto fail;
        // mutex 의 osq lock 변수인 lock->osq 를 통해 midpath 수행  
        //
        // midpath 에서 lock 잡은 경우 !true => 계속 실행
        // midpath 에서 slowpath 로 넘어가야 하는 경우 !false => fail 로 이동
	}

    // <midpath 성공한 경우!!>
    // case 1. waiter 가 NULL 이 아니거나 즉 wait-wound mutex 사용하는 경우 
    //         FIXME
    // case 2. waiter 가 NULL 이며, osq 의 선두에 있는 놈이 
    //         midpath 에서 lock 잡기 성공!!
    //         => 이제 owner 설정해 주어야 함
	for (;;) {
		struct task_struct *owner;

		/* Try to acquire the mutex... */
		owner = __mutex_trylock_or_owner(lock);
		if (!owner)
			break; 
        // case 2.
        // osq 에서 spinning 하다가 자기 차례에서 lock 을 잡은 놈의 
        // task_struct 를 owner 에 설정 해주고 break
        //
        // case 1. 
        // wait-wound mutex 사용하는 경우
        // fastpath 한번 더 확인하고, midpath 과정 시작하기 위헤 코드계속 
        // FIXME
        // 

		/*
		 * There's an owner, wait for it to either
		 * release the lock or go to sleep.
		 */
		if (!mutex_spin_on_owner(lock, owner, ww_ctx, waiter))
			goto fail_unlock;

		/*
		 * The cpu_relax() call is a compiler barrier which forces
		 * everything in this loop to be re-loaded. We don't need
		 * memory barriers as we'll eventually observe the right
		 * values at the cost of a few extra spins.
		 */
		cpu_relax();
	}

	if (!waiter)
		osq_unlock(&lock->osq);
        // case2. 
        // osq lock 도 spinning 정상적으로 하다가 lock 잡고 끝났으므로 
        // osq 에서 제거 및 spinning 하고 있는 다음 optimistic_spin_node 
        // per-CPU 의 locked 에 1 을 write 하여 너차례라고 알림
	return true;
    // osq spinning 의 결과 성공적으로 osq lock 을 얻고, 
    // mutex 의 owner 를 설정하였으니 true 로 함수 종료

fail_unlock:
	if (!waiter)
		osq_unlock(&lock->osq);

fail:
	/*
	 * If we fell out of the spin path because of need_resched(),
	 * reschedule now, before we try-lock the mutex. This avoids getting
	 * scheduled out right after we obtained the mutex.
	 */
	if (need_resched()) {
		/*
		 * We _should_ have TASK_RUNNING here, but just in case
		 * we do not, make it so, otherwise we might get stuck.
		 */
		__set_current_state(TASK_RUNNING);
		schedule_preempt_disabled();
	}

	return false;
}
#else
static __always_inline bool
mutex_optimistic_spin(struct mutex *lock, struct ww_acquire_ctx *ww_ctx,
		      const bool use_ww_ctx, struct mutex_waiter *waiter)
{
	return false;
}
#endif

static noinline void __sched __mutex_unlock_slowpath(struct mutex *lock, unsigned long ip);

/**
 * mutex_unlock - release the mutex
 * @lock: the mutex to be released
 *
 * Unlock a mutex that has been locked by this task previously.
 *
 * This function must not be used in interrupt context. Unlocking
 * of a not locked mutex is not allowed.
 *
 * This function is similar to (but not equivalent to) up().
 */
void __sched mutex_unlock(struct mutex *lock)
{
#ifndef CONFIG_DEBUG_LOCK_ALLOC
	if (__mutex_unlock_fast(lock))
		return;
    // owner 에 task_struct 만 있고 그게 current 라면 바로 unlock
#endif
	__mutex_unlock_slowpath(lock, _RET_IP_);
    // midpath, slowpath 관련
}
EXPORT_SYMBOL(mutex_unlock);

/**
 * ww_mutex_unlock - release the w/w mutex
 * @lock: the mutex to be released
 *
 * Unlock a mutex that has been locked by this task previously with any of the
 * ww_mutex_lock* functions (with or without an acquire context). It is
 * forbidden to release the locks after releasing the acquire context.
 *
 * This function must not be used in interrupt context. Unlocking
 * of a unlocked mutex is not allowed.
 */
void __sched ww_mutex_unlock(struct ww_mutex *lock)
{
	/*
	 * The unlocking fastpath is the 0->1 transition from 'locked'
	 * into 'unlocked' state:
	 */
	if (lock->ctx) {
#ifdef CONFIG_DEBUG_MUTEXES
		DEBUG_LOCKS_WARN_ON(!lock->ctx->acquired);
#endif
		if (lock->ctx->acquired > 0)
			lock->ctx->acquired--;
		lock->ctx = NULL;
	}

	mutex_unlock(&lock->base);
}
EXPORT_SYMBOL(ww_mutex_unlock);

static inline int __sched
__ww_mutex_lock_check_stamp(struct mutex *lock, struct mutex_waiter *waiter,
			    struct ww_acquire_ctx *ctx)
{
	struct ww_mutex *ww = container_of(lock, struct ww_mutex, base);
	struct ww_acquire_ctx *hold_ctx = READ_ONCE(ww->ctx);
	struct mutex_waiter *cur;

	if (hold_ctx && __ww_ctx_stamp_after(ctx, hold_ctx))
		goto deadlock;

	/*
	 * If there is a waiter in front of us that has a context, then its
	 * stamp is earlier than ours and we must back off.
	 */
	cur = waiter;
	list_for_each_entry_continue_reverse(cur, &lock->wait_list, list) {
		if (cur->ww_ctx)
			goto deadlock;
	}

	return 0;

deadlock:
#ifdef CONFIG_DEBUG_MUTEXES
	DEBUG_LOCKS_WARN_ON(ctx->contending_lock);
	ctx->contending_lock = ww;
#endif
	return -EDEADLK;
}

static inline int __sched
__ww_mutex_add_waiter(struct mutex_waiter *waiter,
		      struct mutex *lock,
		      struct ww_acquire_ctx *ww_ctx)
{
	struct mutex_waiter *cur;
	struct list_head *pos;

	if (!ww_ctx) {
		list_add_tail(&waiter->list, &lock->wait_list);
		return 0;
	}

	/*
	 * Add the waiter before the first waiter with a higher stamp.
	 * Waiters without a context are skipped to avoid starving
	 * them.
	 */
	pos = &lock->wait_list;
	list_for_each_entry_reverse(cur, &lock->wait_list, list) {
		if (!cur->ww_ctx)
			continue;

		if (__ww_ctx_stamp_after(ww_ctx, cur->ww_ctx)) {
			/* Back off immediately if necessary. */
			if (ww_ctx->acquired > 0) {
#ifdef CONFIG_DEBUG_MUTEXES
				struct ww_mutex *ww;

				ww = container_of(lock, struct ww_mutex, base);
				DEBUG_LOCKS_WARN_ON(ww_ctx->contending_lock);
				ww_ctx->contending_lock = ww;
#endif
				return -EDEADLK;
			}

			break;
		}

		pos = &cur->list;

		/*
		 * Wake up the waiter so that it gets a chance to back
		 * off.
		 */
		if (cur->ww_ctx->acquired > 0) {
			debug_mutex_wake_waiter(lock, cur);
			wake_up_process(cur->task);
		}
	}

	list_add_tail(&waiter->list, pos);
	return 0;
}

/*
 * Lock a mutex (possibly interruptible), slowpath:
 */
static __always_inline int __sched
__mutex_lock_common(struct mutex *lock, long state, unsigned int subclass,
		    struct lockdep_map *nest_lock, unsigned long ip,
		    struct ww_acquire_ctx *ww_ctx, const bool use_ww_ctx)
{
	struct mutex_waiter waiter;
	bool first = false;
	struct ww_mutex *ww;
	int ret;
    // ip 는 debugging 용도
	might_sleep();

	ww = container_of(lock, struct ww_mutex, base);
	if (use_ww_ctx && ww_ctx) {
		if (unlikely(ww_ctx == READ_ONCE(ww->ctx)))
			return -EALREADY;
        // 
	}
    // ww_mutex 를 사용하는 경우 lock 으로부터 ww_mutex 가져옴
	preempt_disable();
	mutex_acquire_nest(&lock->dep_map, subclass, 0, nest_lock, ip); 
    // lock 추적 debugging 용도 정보 초기화

	if (__mutex_trylock(lock) ||
	    mutex_optimistic_spin(lock, ww_ctx, use_ww_ctx, NULL)) { 
        // __mutex_trylock 으로 fastpath 한번 더 확인 후, 
        //   - 성공이면 !NULL => true
        //   - 실패면 !owner 주소 => false => midpath 수행
        // 
        // mutex_optimistic_spin 함수를 통해 mid path 수행 
        //
        // => fastpath 또는 midpath 에서 lock 잡기 성공한다면 아래 코드 실행
        //
		/* got the lock, yay! */
		lock_acquired(&lock->dep_map, ip); 
        // lock debugging 정보 기록
		if (use_ww_ctx && ww_ctx)
			ww_mutex_set_context_fastpath(ww, ww_ctx);
        // wait-wound mutex 관련 
        // FIXME
		preempt_enable();
		return 0;
	}
    // midpath 로 진입을 아얘 못하거나, midpath 도중 CPU yield 해야하는 
    // 상황이어서 osq 구성한거 되돌리고 여기로 오게되면...
    //
    // slowpath 로 lock 을 대기해야 하는 상황
    //
	spin_lock(&lock->wait_lock);
	/*
	 * After waiting to acquire the wait_lock, try again.
	 */
	if (__mutex_trylock(lock)) { 
        // fastpath 가능한지 한번 더 확인        
		if (use_ww_ctx && ww_ctx)
			__ww_mutex_wakeup_for_backoff(lock, ww_ctx);
            // wait-wound mutex 관련 
            // FIXME
		goto skip_wait;
	}

	debug_mutex_lock_common(lock, &waiter);
	debug_mutex_add_waiter(lock, &waiter, current);

	lock_contended(&lock->dep_map, ip);
    // lock validator 관련 초기화
	if (!use_ww_ctx) {
		/* add waiting tasks to the end of the waitqueue (FIFO): */
		list_add_tail(&waiter.list, &lock->wait_list);
        // mutex 의 wait_list 에 mutex_waiter 추가
#ifdef CONFIG_DEBUG_MUTEXES
		waiter.ww_ctx = MUTEX_POISON_WW_CTX;
#endif
	} else { 
        // wait-wound mutex 관련 
        // FIXME
		/* Add in stamp order, waking up waiters that must back off. */
		ret = __ww_mutex_add_waiter(&waiter, lock, ww_ctx);
		if (ret)
			goto err_early_backoff;

		waiter.ww_ctx = ww_ctx;
	}

	waiter.task = current;
    // current task_struct 를 mutex_waiter 에 설정

	if (__mutex_waiter_is_first(lock, &waiter))
		__mutex_set_flag(lock, MUTEX_FLAG_WAITERS);
    // mutex_waiter 가 wait_list 의 유일한 waier 라면, 
    // MUTEX_FLAG_WAITERS 를 설정하여 현재 lock 을 
    // 기다리며 잠들어 있는 놈이 있다는 것을 알림
    // => unlock 시, wakeup 해주어야함을 알리기 위해

	set_current_state(state);
    // 현재 task 의 state 를 TASK_UNINTERRUPTIBLE, TASK_INTERRUPTIBLE 등 매개
    // 변수로 받은 값으로 설정. 
    // mutex_lock 에서는 TASK_UNINTERRUPTIBLE
	for (;;) {
        // 이제 sleep 했다가... 깨서 lock 획득 가능하나 확인했다가...
        // 다시 sleep 했다가 반복
		/*
		 * Once we hold wait_lock, we're serialized against
		 * mutex_unlock() handing the lock off to us, do a trylock
		 * before testing the error conditions to make sure we pick up
		 * the handoff.
		 */
		if (__mutex_trylock(lock))
			goto acquired;
        // mutex 얻을 수 있는지 확인

		if (unlikely(signal_pending_state(state, current))) {
            // mutex_lock 으로 호출시, state 가 TASK_UNINTERRUPTIBLE 이므로 
            // 관련 없지만. 
            // TASK_INTERRUPTIBLE 로 설정시, mutex 를 기다리던 thread 가 
            // signal 을 받아 mutex 획득을 중단 할 수 있음
			ret = -EINTR;
			goto err;
		}

		if (use_ww_ctx && ww_ctx && ww_ctx->acquired > 0) {
            // FIXME
            // wait-wound mutex 관련
			ret = __ww_mutex_lock_check_stamp(lock, &waiter, ww_ctx);
			if (ret)
				goto err;
		}
        // 이제 현재 thread 를 sleep 시키고schedule 함수를 통해 다음 
        // 수행해야 될 task 를 선택해야 하므로 wait_queue 를 지키고 있던 
        // spinlock 을 풀음
		spin_unlock(&lock->wait_lock);
		schedule_preempt_disabled();
        // 다시 수행되게 되면 여기부터 수행됨
		/*
		 * ww_mutex needs to always recheck its position since its waiter
		 * list is not FIFO ordered.
		 */
		if ((use_ww_ctx && ww_ctx) || !first) {
            // FIXME
            // wait-wound mutex 관련
			first = __mutex_waiter_is_first(lock, &waiter);
			if (first)
				__mutex_set_flag(lock, MUTEX_FLAG_HANDOFF);
                // 현재 thread 가 wait_list 의 첫번째 놈이라면
                // MUTEX_FLAG_HANDOFF 설정하여 lock 잡고있던 놈이lock 을
                // 바로전달 할 수 있게 함 
		}

		set_current_state(state); 
        // task 상태를 다시 설정
		/*
		 * Here we order against unlock; we must either see it change
		 * state back to RUNNING and fall through the next schedule(),
		 * or we must see its unlock and acquire.
		 */
		if (__mutex_trylock(lock) ||
		    (first && mutex_optimistic_spin(lock, ww_ctx, use_ww_ctx, &waiter)))
			break;
        // fastpath, midpath 로 lock acquire 시도
		spin_lock(&lock->wait_lock);
	}
	spin_lock(&lock->wait_lock);
acquired: 
    // lock 을 얻은 상태 
	__set_current_state(TASK_RUNNING);
    // lock 을 얻었으므로 실행 재개함. 
    // 현재 task 상태를 TASK_RUNNING 으로 변경
	mutex_remove_waiter(lock, &waiter, current);
    // mutex->wait_list 에서 현재 task_struct 에 해당하는 mutex_waiter 삭제
	if (likely(list_empty(&lock->wait_list)))
		__mutex_clear_flag(lock, MUTEX_FLAGS);
        // 삭제 결과 list 가 하나도 안남게 된다면 lock 잡을라고 sleep 하는놈이
        // 없으므로 다 flag 다 clear
	debug_mutex_free_waiter(&waiter);

skip_wait:
	/* got the lock - cleanup and rejoice! */
	lock_acquired(&lock->dep_map, ip);
    // lock debugging 정보 기록
	if (use_ww_ctx && ww_ctx)
		ww_mutex_set_context_slowpath(ww, ww_ctx);
        // FIXME
        // wait-wound mutex 관련 
	spin_unlock(&lock->wait_lock);
	preempt_enable();
	return 0;
    // 성공적으로 종료

err:
	__set_current_state(TASK_RUNNING);    
	mutex_remove_waiter(lock, &waiter, current);
err_early_backoff:
	spin_unlock(&lock->wait_lock);
	debug_mutex_free_waiter(&waiter);
	mutex_release(&lock->dep_map, 1, ip);
	preempt_enable();
	return ret;
}

static int __sched
__mutex_lock(struct mutex *lock, long state, unsigned int subclass,
	     struct lockdep_map *nest_lock, unsigned long ip)
{
	return __mutex_lock_common(lock, state, subclass, nest_lock, ip, NULL, false);
}

static int __sched
__ww_mutex_lock(struct mutex *lock, long state, unsigned int subclass,
		struct lockdep_map *nest_lock, unsigned long ip,
		struct ww_acquire_ctx *ww_ctx)
{
	return __mutex_lock_common(lock, state, subclass, nest_lock, ip, ww_ctx, true);
}

#ifdef CONFIG_DEBUG_LOCK_ALLOC
void __sched
mutex_lock_nested(struct mutex *lock, unsigned int subclass)
{
	__mutex_lock(lock, TASK_UNINTERRUPTIBLE, subclass, NULL, _RET_IP_);
}

EXPORT_SYMBOL_GPL(mutex_lock_nested);

void __sched
_mutex_lock_nest_lock(struct mutex *lock, struct lockdep_map *nest)
{
	__mutex_lock(lock, TASK_UNINTERRUPTIBLE, 0, nest, _RET_IP_);
}
EXPORT_SYMBOL_GPL(_mutex_lock_nest_lock);

int __sched
mutex_lock_killable_nested(struct mutex *lock, unsigned int subclass)
{
	return __mutex_lock(lock, TASK_KILLABLE, subclass, NULL, _RET_IP_);
}
EXPORT_SYMBOL_GPL(mutex_lock_killable_nested);

int __sched
mutex_lock_interruptible_nested(struct mutex *lock, unsigned int subclass)
{
	return __mutex_lock(lock, TASK_INTERRUPTIBLE, subclass, NULL, _RET_IP_);
}
EXPORT_SYMBOL_GPL(mutex_lock_interruptible_nested);

void __sched
mutex_lock_io_nested(struct mutex *lock, unsigned int subclass)
{
	int token;

	might_sleep();

	token = io_schedule_prepare();
	__mutex_lock_common(lock, TASK_UNINTERRUPTIBLE,
			    subclass, NULL, _RET_IP_, NULL, 0);
	io_schedule_finish(token);
}
EXPORT_SYMBOL_GPL(mutex_lock_io_nested);

static inline int
ww_mutex_deadlock_injection(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
#ifdef CONFIG_DEBUG_WW_MUTEX_SLOWPATH
	unsigned tmp;

	if (ctx->deadlock_inject_countdown-- == 0) {
		tmp = ctx->deadlock_inject_interval;
		if (tmp > UINT_MAX/4)
			tmp = UINT_MAX;
		else
			tmp = tmp*2 + tmp + tmp/2;

		ctx->deadlock_inject_interval = tmp;
		ctx->deadlock_inject_countdown = tmp;
		ctx->contending_lock = lock;

		ww_mutex_unlock(lock);

		return -EDEADLK;
	}
#endif

	return 0;
}

int __sched
ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	int ret;

	might_sleep();
	ret =  __ww_mutex_lock(&lock->base, TASK_UNINTERRUPTIBLE,
			       0, ctx ? &ctx->dep_map : NULL, _RET_IP_,
			       ctx);
	if (!ret && ctx && ctx->acquired > 1)
		return ww_mutex_deadlock_injection(lock, ctx);

	return ret;
}
EXPORT_SYMBOL_GPL(ww_mutex_lock);

int __sched
ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	int ret;

	might_sleep();
	ret = __ww_mutex_lock(&lock->base, TASK_INTERRUPTIBLE,
			      0, ctx ? &ctx->dep_map : NULL, _RET_IP_,
			      ctx);

	if (!ret && ctx && ctx->acquired > 1)
		return ww_mutex_deadlock_injection(lock, ctx);

	return ret;
}
EXPORT_SYMBOL_GPL(ww_mutex_lock_interruptible);

#endif

/*
 * Release the lock, slowpath:
 */
static noinline void __sched __mutex_unlock_slowpath(struct mutex *lock, unsigned long ip)
{
	struct task_struct *next = NULL;
	DEFINE_WAKE_Q(wake_q);
	unsigned long owner;

	mutex_release(&lock->dep_map, 1, ip);
    // lock debugging 관련
	/*
	 * Release the lock before (potentially) taking the spinlock such that
	 * other contenders can get on with things ASAP.
	 *
	 * Except when HANDOFF, in that case we must not clear the owner field,
	 * but instead set it to the top waiter.
	 */
	owner = atomic_long_read(&lock->owner);
	for (;;) {
		unsigned long old;

#ifdef CONFIG_DEBUG_MUTEXES
		DEBUG_LOCKS_WARN_ON(__owner_task(owner) != current);
		DEBUG_LOCKS_WARN_ON(owner & MUTEX_FLAG_PICKUP);
#endif

		if (owner & MUTEX_FLAG_HANDOFF)
			break;
        // MUTEX_FLAG_HANDOFF 가 설정되어 있다면, 단순 owner 에 대한 clear 가 
        // 아니라 direct 로 handoff 해주어야 하므로 break
		old = atomic_long_cmpxchg_release(&lock->owner, owner,
						  __owner_flags(owner));
        // lock->owner 에 flag 만 남기고 다 clear
		if (old == owner) {
			if (owner & MUTEX_FLAG_WAITERS)
				break;
                // MUTEX_FLAG_WAITERS 가 설정되어 있다면 
                // wakeup 해주어야 하므로 break

			return;
		}

		owner = old;
	}

	spin_lock(&lock->wait_lock);
	debug_mutex_unlock(lock);
	if (!list_empty(&lock->wait_list)) {
        // list 가 비어있지 않은 경우 wait_list 에서
        // 첫번째 놈 가져와 바로 깨울 list에 추가
		/* get the first entry from the wait-list: */
		struct mutex_waiter *waiter =
			list_first_entry(&lock->wait_list,
					 struct mutex_waiter, list);        

		next = waiter->task;

		debug_mutex_wake_waiter(lock, waiter);
		wake_q_add(&wake_q, next);
	}

	if (owner & MUTEX_FLAG_HANDOFF)
		__mutex_handoff(lock, next);
        // MUTEX_FLAG_HANDOFF 설정 시 아직 lock 을 잡지 못하고
        // 여러번 잠든 놈이 있는 경우이므로 fairness 를 위해 
        // wait_list 의 첫번째 놈에게 lock 직접전달

	spin_unlock(&lock->wait_lock);

	wake_up_q(&wake_q);
    // 깨움
}

#ifndef CONFIG_DEBUG_LOCK_ALLOC
/*
 * Here come the less common (and hence less performance-critical) APIs:
 * mutex_lock_interruptible() and mutex_trylock().
 */
static noinline int __sched
__mutex_lock_killable_slowpath(struct mutex *lock);

static noinline int __sched
__mutex_lock_interruptible_slowpath(struct mutex *lock);

/**
 * mutex_lock_interruptible - acquire the mutex, interruptible
 * @lock: the mutex to be acquired
 *
 * Lock the mutex like mutex_lock(), and return 0 if the mutex has
 * been acquired or sleep until the mutex becomes available. If a
 * signal arrives while waiting for the lock then this function
 * returns -EINTR.
 *
 * This function is similar to (but not equivalent to) down_interruptible().
 */
int __sched mutex_lock_interruptible(struct mutex *lock)
{
	might_sleep();

	if (__mutex_trylock_fast(lock))
		return 0;

	return __mutex_lock_interruptible_slowpath(lock);
}

EXPORT_SYMBOL(mutex_lock_interruptible);

int __sched mutex_lock_killable(struct mutex *lock)
{
	might_sleep();

	if (__mutex_trylock_fast(lock))
		return 0;

	return __mutex_lock_killable_slowpath(lock);
}
EXPORT_SYMBOL(mutex_lock_killable);

void __sched mutex_lock_io(struct mutex *lock)
{
	int token;

	token = io_schedule_prepare();
	mutex_lock(lock);
	io_schedule_finish(token);
}
EXPORT_SYMBOL_GPL(mutex_lock_io);

static noinline void __sched
__mutex_lock_slowpath(struct mutex *lock)
{
	__mutex_lock(lock, TASK_UNINTERRUPTIBLE, 0, NULL, _RET_IP_);
}

static noinline int __sched
__mutex_lock_killable_slowpath(struct mutex *lock)
{
	return __mutex_lock(lock, TASK_KILLABLE, 0, NULL, _RET_IP_);
}

static noinline int __sched
__mutex_lock_interruptible_slowpath(struct mutex *lock)
{
	return __mutex_lock(lock, TASK_INTERRUPTIBLE, 0, NULL, _RET_IP_);
}

static noinline int __sched
__ww_mutex_lock_slowpath(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	return __ww_mutex_lock(&lock->base, TASK_UNINTERRUPTIBLE, 0, NULL,
			       _RET_IP_, ctx);
}

static noinline int __sched
__ww_mutex_lock_interruptible_slowpath(struct ww_mutex *lock,
					    struct ww_acquire_ctx *ctx)
{
	return __ww_mutex_lock(&lock->base, TASK_INTERRUPTIBLE, 0, NULL,
			       _RET_IP_, ctx);
}

#endif

/**
 * mutex_trylock - try to acquire the mutex, without waiting
 * @lock: the mutex to be acquired
 *
 * Try to acquire the mutex atomically. Returns 1 if the mutex
 * has been acquired successfully, and 0 on contention.
 *
 * NOTE: this function follows the spin_trylock() convention, so
 * it is negated from the down_trylock() return values! Be careful
 * about this when converting semaphore users to mutexes.
 *
 * This function must not be used in interrupt context. The
 * mutex must be released by the same task that acquired it.
 */
int __sched mutex_trylock(struct mutex *lock)
{
	bool locked = __mutex_trylock(lock);

	if (locked)
		mutex_acquire(&lock->dep_map, 0, 1, _RET_IP_);

	return locked;
}
EXPORT_SYMBOL(mutex_trylock);

#ifndef CONFIG_DEBUG_LOCK_ALLOC
int __sched
ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	might_sleep();

	if (__mutex_trylock_fast(&lock->base)) {
		if (ctx)
			ww_mutex_set_context_fastpath(lock, ctx);
		return 0;
	}

	return __ww_mutex_lock_slowpath(lock, ctx);
}
EXPORT_SYMBOL(ww_mutex_lock);

int __sched
ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	might_sleep();

	if (__mutex_trylock_fast(&lock->base)) {
		if (ctx)
			ww_mutex_set_context_fastpath(lock, ctx);
		return 0;
	}

	return __ww_mutex_lock_interruptible_slowpath(lock, ctx);
}
EXPORT_SYMBOL(ww_mutex_lock_interruptible);

#endif

/**
 * atomic_dec_and_mutex_lock - return holding mutex if we dec to 0
 * @cnt: the atomic which we are to dec
 * @lock: the mutex to return holding if we dec to 0
 *
 * return true and hold lock if we dec to 0, return false otherwise
 */
int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock)
{
	/* dec if we can't possibly hit 0 */
	if (atomic_add_unless(cnt, -1, 1))
		return 0;
	/* we might hit 0, so take the lock */
	mutex_lock(lock);
	if (!atomic_dec_and_test(cnt)) {
		/* when we actually did the dec, we didn't hit 0 */
		mutex_unlock(lock);
		return 0;
	}
	/* we hit 0, and we hold the lock */
	return 1;
}
EXPORT_SYMBOL(atomic_dec_and_mutex_lock);
