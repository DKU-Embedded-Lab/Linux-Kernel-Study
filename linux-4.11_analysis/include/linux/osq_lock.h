#ifndef __LINUX_OSQ_LOCK_H
#define __LINUX_OSQ_LOCK_H

/*
 * An MCS like lock especially tailored for optimistic spinning for sleeping
 * lock implementations (mutex, rwsem, etc).
 */
struct optimistic_spin_node {
	struct optimistic_spin_node *next, *prev; 
    // 다음 lock 대기자, 전대기자
	int locked; /* 1 if lock acquired */
    // 0 - lock acquire 기다리는중..
    // 1 - lock acquire 성공
	int cpu; /* encoded CPU # + 1 value */ 
    // 현재 per-CPU optimistic_spin_node 가 
    // 소속된 CPU 번호
};

struct optimistic_spin_queue {
	/*
	 * Stores an encoded value of the CPU # of the tail node in the queue.
	 * If the queue is empty, then it's set to OSQ_UNLOCKED_VAL.
	 */
	atomic_t tail; 
    // lock 대기중인 per-CPU optimistic_spin_node list 에서 
    // 마지막 node 가 속한 CPU 번호로 1 부터 시작하도록 설정됨
    // 0 은 OSQ_UNLOCKED_VAL 을 나타내는 값으로 osq queue 에 아무것도 
    // 없을 경우 즉 처음으로 osq 에 진입 한 경우임
};

#define OSQ_UNLOCKED_VAL (0)

/* Init macro and function. */
#define OSQ_LOCK_UNLOCKED { ATOMIC_INIT(OSQ_UNLOCKED_VAL) }

static inline void osq_lock_init(struct optimistic_spin_queue *lock)
{
	atomic_set(&lock->tail, OSQ_UNLOCKED_VAL);
    // optimistic queue 의tail 을 unlocked 로 설정
}

extern bool osq_lock(struct optimistic_spin_queue *lock);
extern void osq_unlock(struct optimistic_spin_queue *lock);

static inline bool osq_is_locked(struct optimistic_spin_queue *lock)
{
	return atomic_read(&lock->tail) != OSQ_UNLOCKED_VAL;
}

#endif
