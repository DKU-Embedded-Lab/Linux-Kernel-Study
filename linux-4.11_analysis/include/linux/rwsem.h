/* rwsem.h: R/W semaphores, public interface
 *
 * Written by David Howells (dhowells@redhat.com).
 * Derived from asm-i386/semaphore.h
 */

#ifndef _LINUX_RWSEM_H
#define _LINUX_RWSEM_H

#include <linux/linkage.h>

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/err.h>
#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
#include <linux/osq_lock.h>
#endif

struct rw_semaphore;

#ifdef CONFIG_RWSEM_GENERIC_SPINLOCK
#include <linux/rwsem-spinlock.h> /* use a generic implementation */
#define __RWSEM_INIT_COUNT(name)	.count = RWSEM_UNLOCKED_VALUE
#else
/* All arch specific implementations share the same struct */
struct rw_semaphore {
	atomic_long_t count; 
    //  0x0000000000000000 - unlocked 상태이며, waiter 없음 
    //                         (RWSEM_UNLOCKED_VALUE)
    //  0x000000000000000x - x reader 가 CS 에 동작중 or lock 잡으려는 중, write waiter 없음 
    //                         x = CS 에 동작중인 reader + lock 잡는중인 reader
    //                         (x*RWSEM_ACTIVE_BIAS)
    //  0xffffffff0000000x - 3 가지 경우 가능 
    //                       * x reader 가 CS 에 동작중 or lock 잡으려는 중, waiter 있음 
    //                         x = CS 에 동작중인 reader + lock 잡는중인 reader
    //                         (x*RWSEM_ACTIVE_BIAS + RWSEM_WAITING_BIAS)
    //                       * 1 writer 가 CS 에 동작중, waiter 없음
    //                         ()
    //                       * 1 writer 가 lock 잡으려는 중, waiter 없음 
    //                         ()
    //  0xffffffff00000001 - 2 가지 경우 가능 
    //                       * 1 reader 가 CS 에 동작중 or lock 잡으려는 중, waiter 있음 
    //                         (RWSEM_ACTIVE_BIAS + RWSEM_WAITING_BIAS)
    //                       * 1 writer 가 CS 에 동작중 or lock 잡으려는 중, waiter 없음 
    //                         (RWSEM_ACTIVE_WRITE_BIAS)
    //  0xffffffff00000000 - reader or writer 가 queue 에 있지만, 아직 lock 잡으려 하지 
    //                       않거나 CS 에 동작중이지 않음
    //                         (RWSEM_WAITING_BIAS)
    //  0xfffffffe00000001 - 1 writer 가 CS 에 동작중 or lock 잡으려는 중, queue 에 waiter 있음
    //                         (RWSEM_ACTIVE_WRITE_BIAS + RWSEM_WAITING_BIAS)
	struct list_head wait_list; 
    // lock 얻기를 대기중인 process 들
	raw_spinlock_t wait_lock;
    // wait list 보호하기 위한 
#ifdef CONFIG_RWSEM_SPIN_ON_OWNER
	struct optimistic_spin_queue osq; /* spinner MCS lock */ 
    // MCS 알고리즘의 OSQ lock
	/*
	 * Write owner. Used as a speculative check to see
	 * if the owner is running on the cpu.
	 */
	struct task_struct *owner;
    // 현재 lock 소유한 task_struct  
    // writer 가 
    //    lock   하면... owner field 를 자신의 task_struct 씀 
    //    unlock 하면... owner filed 를 clear
    //    
    // reader 가 
    //    lock   하면... owner field 가 1 로 설정
    //    unlock 하면    변경안하고 그대로 둠 
    //
    // => 0
    //   lock 이 free
    // => 1
    //   lock 을 reader 가 잡았었거나, 바로 전 lock holder 가 reader 였음
    // => task_struct 주소
    //   writer 가 lock 잡은 상태
#endif
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map; 
    // lock debugging 관련
#endif
};

extern struct rw_semaphore *rwsem_down_read_failed(struct rw_semaphore *sem);
extern struct rw_semaphore *rwsem_down_write_failed(struct rw_semaphore *sem);
extern struct rw_semaphore *rwsem_down_write_failed_killable(struct rw_semaphore *sem);
extern struct rw_semaphore *rwsem_wake(struct rw_semaphore *);
extern struct rw_semaphore *rwsem_downgrade_wake(struct rw_semaphore *sem);

/* Include the arch specific part */
#include <asm/rwsem.h>

/* In all implementations count != 0 means locked */
static inline int rwsem_is_locked(struct rw_semaphore *sem)
{
	return atomic_long_read(&sem->count) != 0;
}

// rwsem 의 count 를
#define __RWSEM_INIT_COUNT(name)	.count = ATOMIC_LONG_INIT(RWSEM_UNLOCKED_VALUE)

#endif

/* Common initializer macros and functions */

#ifdef CONFIG_DEBUG_LOCK_ALLOC
# define __RWSEM_DEP_MAP_INIT(lockname) , .dep_map = { .name = #lockname }
#else
# define __RWSEM_DEP_MAP_INIT(lockname)
#endif

#ifdef CONFIG_RWSEM_SPIN_ON_OWNER 
// rwsem 내의 OSQ lock 관련 필드 초기화
#define __RWSEM_OPT_INIT(lockname) , .osq = OSQ_LOCK_UNLOCKED, .owner = NULL
#else
#define __RWSEM_OPT_INIT(lockname)
#endif

#define __RWSEM_INITIALIZER(name)				\
	{ __RWSEM_INIT_COUNT(name),				\
	  .wait_list = LIST_HEAD_INIT((name).wait_list),	\
	  .wait_lock = __RAW_SPIN_LOCK_UNLOCKED(name.wait_lock)	\
	  __RWSEM_OPT_INIT(name)				\
	  __RWSEM_DEP_MAP_INIT(name) }

#define DECLARE_RWSEM(name) \
	struct rw_semaphore name = __RWSEM_INITIALIZER(name)

extern void __init_rwsem(struct rw_semaphore *sem, const char *name,
			 struct lock_class_key *key);

#define init_rwsem(sem)						\
do {								\
	static struct lock_class_key __key;			\
								\
	__init_rwsem((sem), #sem, &__key);			\
} while (0)

/*
 * This is the same regardless of which rwsem implementation that is being used.
 * It is just a heuristic meant to be called by somebody alreadying holding the
 * rwsem to see if somebody from an incompatible type is wanting access to the
 * lock.
 */
static inline int rwsem_is_contended(struct rw_semaphore *sem)
{
	return !list_empty(&sem->wait_list);
}

/*
 * lock for reading
 */
extern void down_read(struct rw_semaphore *sem);

/*
 * trylock for reading -- returns 1 if successful, 0 if contention
 */
extern int down_read_trylock(struct rw_semaphore *sem);

/*
 * lock for writing
 */
extern void down_write(struct rw_semaphore *sem);
extern int __must_check down_write_killable(struct rw_semaphore *sem);

/*
 * trylock for writing -- returns 1 if successful, 0 if contention
 */
extern int down_write_trylock(struct rw_semaphore *sem);

/*
 * release a read lock
 */
extern void up_read(struct rw_semaphore *sem);

/*
 * release a write lock
 */
extern void up_write(struct rw_semaphore *sem);

/*
 * downgrade write lock to read lock
 */
extern void downgrade_write(struct rw_semaphore *sem);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
/*
 * nested locking. NOTE: rwsems are not allowed to recurse
 * (which occurs if the same task tries to acquire the same
 * lock instance multiple times), but multiple locks of the
 * same lock class might be taken, if the order of the locks
 * is always the same. This ordering rule can be expressed
 * to lockdep via the _nested() APIs, but enumerating the
 * subclasses that are used. (If the nesting relationship is
 * static then another method for expressing nested locking is
 * the explicit definition of lock class keys and the use of
 * lockdep_set_class() at lock initialization time.
 * See Documentation/locking/lockdep-design.txt for more details.)
 */
extern void down_read_nested(struct rw_semaphore *sem, int subclass);
extern void down_write_nested(struct rw_semaphore *sem, int subclass);
extern int down_write_killable_nested(struct rw_semaphore *sem, int subclass);
extern void _down_write_nest_lock(struct rw_semaphore *sem, struct lockdep_map *nest_lock);

# define down_write_nest_lock(sem, nest_lock)			\
do {								\
	typecheck(struct lockdep_map *, &(nest_lock)->dep_map);	\
	_down_write_nest_lock(sem, &(nest_lock)->dep_map);	\
} while (0);

/*
 * Take/release a lock when not the owner will release it.
 *
 * [ This API should be avoided as much as possible - the
 *   proper abstraction for this case is completions. ]
 */
extern void down_read_non_owner(struct rw_semaphore *sem);
extern void up_read_non_owner(struct rw_semaphore *sem);
#else
# define down_read_nested(sem, subclass)		down_read(sem)
# define down_write_nest_lock(sem, nest_lock)	down_write(sem)
# define down_write_nested(sem, subclass)	down_write(sem)
# define down_write_killable_nested(sem, subclass)	down_write_killable(sem)
# define down_read_non_owner(sem)		down_read(sem)
# define up_read_non_owner(sem)			up_read(sem)
#endif

#endif /* _LINUX_RWSEM_H */
