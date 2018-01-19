/*
 * Mutexes: blocking mutual exclusion locks
 *
 * started by Ingo Molnar:
 *
 *  Copyright (C) 2004, 2005, 2006 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *
 * This file contains the main data structure and API definitions.
 */
#ifndef __LINUX_MUTEX_H
#define __LINUX_MUTEX_H

#include <asm/current.h>
#include <linux/list.h>
#include <linux/spinlock_types.h>
#include <linux/linkage.h>
#include <linux/lockdep.h>
#include <linux/atomic.h>
#include <asm/processor.h>
#include <linux/osq_lock.h>
#include <linux/debug_locks.h>

struct ww_acquire_ctx;

/*
 * Simple, straightforward mutexes with strict semantics:
 *
 * - only one task can hold the mutex at a time
 * - only the owner can unlock the mutex
 * - multiple unlocks are not permitted
 * - recursive locking is not permitted
 * - a mutex object must be initialized via the API
 * - a mutex object must not be initialized via memset or copying
 * - task may not exit with mutex held
 * - memory areas where held locks reside must not be freed
 * - held mutexes must not be reinitialized
 * - mutexes may not be used in hardware or software interrupt
 *   contexts such as tasklets and timers
 *
 * These semantics are fully enforced when DEBUG_MUTEXES is
 * enabled. Furthermore, besides enforcing the above rules, the mutex
 * debugging code also implements a number of additional features
 * that make lock debugging easier and faster:
 *
 * - uses symbolic names of mutexes, whenever they are printed in debug output
 * - point-of-acquire tracking, symbolic lookup of function names
 * - list of all locks held in the system, printout of them
 * - owner tracking
 * - detects self-recursing locks and prints out all relevant info
 * - detects multi-task circular deadlocks and prints out all affected
 *   locks and tasks (and only those tasks)
 */
// wait-wound mutex 사용의 경우, mutex 가 ww_mutex 에 포함되어 있음
struct mutex {
	atomic_long_t		owner; 
    // lock 을 잡은 task_struct 의 주소
    // owner 이 0 이면 즉 NULL 이면 lock 을 소유한 
    // task_struct 가 없으므로 lock 이 free 상태 
    //
    // 또한 address 가 L1_CACHE_BYTES align 되어 있으므로(6 bit)
    // 0,1,2 bit 로 lock 의 상태정보를 나타냄 
    //
    // 0 BIT 설정           - lock 잠김 mutex 를 얻으려 기다리는 동안 잠들어야 될때 설정 
    // (MUTEX_FLAG_WAITERS) - lock 잡고있던 thread 는 lock release 시 이 bit 가 설정되어 
    //                        있다면 waiter 를 wakeup 해주어야 함을 알수 있음
    //                      => 불필요한 연산 방지
    //                      => slowpath 들어간 thread 가 있을때 설정됨
    //
    // 1 BIT 설정           - 이미 lock 잡으려다 fail 후, 일정시간 sleep 했던 thread 가 
    // (MUTEX_FLAG_HANDOFF)   깨어나 다시 lock 잡으려 시도할때 실패하게 되면 
    //                        다시 또 sleep 들어가기 전에 이 bit 설정 
    //                      - lock 잡고있던 thread 는 lock release 시 이 bit 가 설정되어 
    //                        있다면 다른 새로 lock 을 잡으려는 thread 또는 Optimistic 
    //                        spinning 중인 thread 가 lock 을 먼저 잡기 전지 않도록 하기 
    //                        위해 단순히 owner 를 0 으로 clear 하는 것이 아니라, sleep 
    //                        들어간 thread 를 깨우고, direct 로 wait_list 의 첫번재 
    //                        thread 에게 소유권을 넘겨줌
    //                      => unfairness 방지  
    //
    // 2 BIT 설정          - 1bit 설정 으로 lock 잡은 thread 가 lock 을 handoff 
    // (MUTEX_FLAG_PICKUP)   하게될 때, 즉 건네 주게 될 때1 BIT clear 하고, 
    //                       pickup bit 를 새 owner 와 함께 설정함 
    //                     => 같은 owner 가 같은 lock 에 또접근시 오류 막기 위해
    //
	spinlock_t		wait_lock;
    // wait_list 에 대한 lock (slow path 용)
#ifdef CONFIG_MUTEX_SPIN_ON_OWNER
	struct optimistic_spin_queue osq; /* Spinner MCS lock */ 
    // midpath 를 위한 MCS lock 알고리즘으로 동작하는 OSQ lock     
#endif
	struct list_head	wait_list;
    // lock 잡기를 대기하는 process wait queue (mid path 용도)
#ifdef CONFIG_DEBUG_MUTEXES
	void			*magic;
    // mutex 관련 debugging 정보
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
    // lock validator
#endif
};

static inline struct task_struct *__mutex_owner(struct mutex *lock)
{
	return (struct task_struct *)(atomic_long_read(&lock->owner) & ~0x07); 
    // ... 1111 1000 => 즉 lock->owner 에서 task_struct 뽑아냄
}

/*
 * This is the control structure for tasks blocked on mutex,
 * which resides on the blocked task's kernel stack:
 */
struct mutex_waiter {
	struct list_head	list;
    // 다음 대기 mutex_waiter node 와 연결
	struct task_struct	*task; 
    // 대기하는 thread 의 task_struct
	struct ww_acquire_ctx	*ww_ctx; 
    // FIXME
    // wait-wound mutex 관련    
#ifdef CONFIG_DEBUG_MUTEXES
	void			*magic; 
    // mutex 관련 debug 정보
#endif
};

#ifdef CONFIG_DEBUG_MUTEXES

#define __DEBUG_MUTEX_INITIALIZER(lockname)				\
	, .magic = &lockname

extern void mutex_destroy(struct mutex *lock);

#else

# define __DEBUG_MUTEX_INITIALIZER(lockname)

static inline void mutex_destroy(struct mutex *lock) {}

#endif

/**
 * mutex_init - initialize the mutex
 * @mutex: the mutex to be initialized
 *
 * Initialize the mutex to unlocked state.
 *
 * It is not allowed to initialize an already locked mutex.
 */
#define mutex_init(mutex)						\
do {									\
	static struct lock_class_key __key;				\
									\
	__mutex_init((mutex), #mutex, &__key);				\
} while (0)

#ifdef CONFIG_DEBUG_LOCK_ALLOC
# define __DEP_MAP_MUTEX_INITIALIZER(lockname) \
		, .dep_map = { .name = #lockname }
#else
# define __DEP_MAP_MUTEX_INITIALIZER(lockname)
#endif

#define __MUTEX_INITIALIZER(lockname) \
		{ .owner = ATOMIC_LONG_INIT(0) \
		, .wait_lock = __SPIN_LOCK_UNLOCKED(lockname.wait_lock) \
		, .wait_list = LIST_HEAD_INIT(lockname.wait_list) \
		__DEBUG_MUTEX_INITIALIZER(lockname) \
		__DEP_MAP_MUTEX_INITIALIZER(lockname) }

#define DEFINE_MUTEX(mutexname) \
	struct mutex mutexname = __MUTEX_INITIALIZER(mutexname)

extern void __mutex_init(struct mutex *lock, const char *name,
			 struct lock_class_key *key);

/**
 * mutex_is_locked - is the mutex locked
 * @lock: the mutex to be queried
 *
 * Returns 1 if the mutex is locked, 0 if unlocked.
 */
static inline int mutex_is_locked(struct mutex *lock)
{
	/*
	 * XXX think about spin_is_locked
	 */
	return __mutex_owner(lock) != NULL;
}

/*
 * See kernel/locking/mutex.c for detailed documentation of these APIs.
 * Also see Documentation/locking/mutex-design.txt.
 */
#ifdef CONFIG_DEBUG_LOCK_ALLOC
extern void mutex_lock_nested(struct mutex *lock, unsigned int subclass);
extern void _mutex_lock_nest_lock(struct mutex *lock, struct lockdep_map *nest_lock);

extern int __must_check mutex_lock_interruptible_nested(struct mutex *lock,
					unsigned int subclass);
extern int __must_check mutex_lock_killable_nested(struct mutex *lock,
					unsigned int subclass);
extern void mutex_lock_io_nested(struct mutex *lock, unsigned int subclass);

#define mutex_lock(lock) mutex_lock_nested(lock, 0)
#define mutex_lock_interruptible(lock) mutex_lock_interruptible_nested(lock, 0)
#define mutex_lock_killable(lock) mutex_lock_killable_nested(lock, 0)
#define mutex_lock_io(lock) mutex_lock_io_nested(lock, 0)

#define mutex_lock_nest_lock(lock, nest_lock)				\
do {									\
	typecheck(struct lockdep_map *, &(nest_lock)->dep_map);	\
	_mutex_lock_nest_lock(lock, &(nest_lock)->dep_map);		\
} while (0)

#else
extern void mutex_lock(struct mutex *lock);
extern int __must_check mutex_lock_interruptible(struct mutex *lock);
extern int __must_check mutex_lock_killable(struct mutex *lock);
extern void mutex_lock_io(struct mutex *lock);

# define mutex_lock_nested(lock, subclass) mutex_lock(lock)
# define mutex_lock_interruptible_nested(lock, subclass) mutex_lock_interruptible(lock)
# define mutex_lock_killable_nested(lock, subclass) mutex_lock_killable(lock)
# define mutex_lock_nest_lock(lock, nest_lock) mutex_lock(lock)
# define mutex_lock_io_nested(lock, subclass) mutex_lock(lock)
#endif

/*
 * NOTE: mutex_trylock() follows the spin_trylock() convention,
 *       not the down_trylock() convention!
 *
 * Returns 1 if the mutex has been acquired successfully, and 0 on contention.
 */
extern int mutex_trylock(struct mutex *lock);
extern void mutex_unlock(struct mutex *lock);

extern int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock);

/*
 * These values are chosen such that FAIL and SUCCESS match the
 * values of the regular mutex_trylock().
 */
enum mutex_trylock_recursive_enum {
	MUTEX_TRYLOCK_FAILED    = 0,
	MUTEX_TRYLOCK_SUCCESS   = 1,
	MUTEX_TRYLOCK_RECURSIVE,
};

/**
 * mutex_trylock_recursive - trylock variant that allows recursive locking
 * @lock: mutex to be locked
 *
 * This function should not be used, _ever_. It is purely for hysterical GEM
 * raisins, and once those are gone this will be removed.
 *
 * Returns:
 *  MUTEX_TRYLOCK_FAILED    - trylock failed,
 *  MUTEX_TRYLOCK_SUCCESS   - lock acquired,
 *  MUTEX_TRYLOCK_RECURSIVE - we already owned the lock.
 */
static inline /* __deprecated */ __must_check enum mutex_trylock_recursive_enum
mutex_trylock_recursive(struct mutex *lock)
{
	if (unlikely(__mutex_owner(lock) == current))
		return MUTEX_TRYLOCK_RECURSIVE;

	return mutex_trylock(lock);
}

#endif /* __LINUX_MUTEX_H */
