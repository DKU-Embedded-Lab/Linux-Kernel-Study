/*
 * Queued read/write locks
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * (C) Copyright 2013-2014 Hewlett-Packard Development Company, L.P.
 *
 * Authors: Waiman Long <waiman.long@hp.com>
 */
#include <linux/smp.h>
#include <linux/bug.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <asm/qrwlock.h>

/*
 * This internal data structure is used for optimizing access to some of
 * the subfields within the atomic_t cnts.
 */
struct __qrwlock {
	union {
		atomic_t cnts;
		struct {
#ifdef __LITTLE_ENDIAN
			u8 wmode;	/* Writer mode   */
			u8 rcnts[3];	/* Reader counts */
#else
			u8 rcnts[3];	/* Reader counts */
			u8 wmode;	/* Writer mode   */
#endif
		};
	};
	arch_spinlock_t	lock;
};

/**
 * rspin_until_writer_unlock - inc reader count & spin until writer is gone
 * @lock  : Pointer to queue rwlock structure
 * @writer: Current queue rwlock writer status byte
 *
 * In interrupt context or at the head of the queue, the reader will just
 * increment the reader count & wait until the writer releases the lock.
 */
static __always_inline void
rspin_until_writer_unlock(struct qrwlock *lock, u32 cnts)
{
	while ((cnts & _QW_WMASK) == _QW_LOCKED) {
		cpu_relax();
		cnts = atomic_read_acquire(&lock->cnts);
	}
}

/**
 * queued_read_lock_slowpath - acquire read lock of a queue rwlock
 * @lock: Pointer to queue rwlock structure
 * @cnts: Current qrwlock lock value
 */
void queued_read_lock_slowpath(struct qrwlock *lock, u32 cnts)
{
	/*
	 * Readers come here when they cannot get the lock without waiting
	 */
	if (unlikely(in_interrupt())) {
		/*
		 * Readers in interrupt context will get the lock immediately
		 * if the writer is just waiting (not holding the lock yet).
		 * The rspin_until_writer_unlock() function returns immediately
		 * in this case. Otherwise, they will spin (with ACQUIRE
		 * semantics) until the lock is available without waiting in
		 * the queue.
		 */
		rspin_until_writer_unlock(lock, cnts); 
        // internal context 의 경우, waiter 에 상관 없이, 
        // writer lock 여부만 확인 후, writer lock 끝나면 바로 진입 
        // (wait_lock 잡지 않고 바로 writer 풀리길 대기)
		return;
	}
	atomic_sub(_QR_BIAS, &lock->cnts);
    // 일단 증가시켜 놓은것 뺌
	/*
	 * Put the reader into the wait queue
	 */
	arch_spin_lock(&lock->wait_lock);
    // interrupt context 가 아니므로 lock 잡고 순서 대기
	/*
	 * The ACQUIRE semantics of the following spinning code ensure
	 * that accesses can't leak upwards out of our subsequent critical
	 * section in the case that the lock is currently held for write.
	 */
	cnts = atomic_fetch_add_acquire(_QR_BIAS, &lock->cnts);
	rspin_until_writer_unlock(lock, cnts);
    // writer lock 이 풀리길 대기

	/*
	 * Signal the next one in queue to become queue head
	 */
	arch_spin_unlock(&lock->wait_lock);
}
EXPORT_SYMBOL(queued_read_lock_slowpath);

/**
 * queued_write_lock_slowpath - acquire write lock of a queue rwlock
 * @lock : Pointer to queue rwlock structure
 */
void queued_write_lock_slowpath(struct qrwlock *lock)
{
	u32 cnts;

	/* Put the writer into the wait queue */
	arch_spin_lock(&lock->wait_lock);

	/* Try to acquire the lock directly if no reader is present */
	if (!atomic_read(&lock->cnts) &&
	    (atomic_cmpxchg_acquire(&lock->cnts, 0, _QW_LOCKED) == 0))
		goto unlock;
        // writer 가 lock 얻을 수 있는지 한번 더 검사
	/*
	 * Set the waiting flag to notify readers that a writer is pending,
	 * or wait for a previous writer to go away.
	 */
	for (;;) {
		struct __qrwlock *l = (struct __qrwlock *)lock;

		if (!READ_ONCE(l->wmode) &&
		   (cmpxchg_relaxed(&l->wmode, 0, _QW_WAITING) == 0))
			break;
        // * writer 가 있을 경우, 
        //   -> writer 수행이 끝날 때까지 spinning
        // * writer 가 없을 경우,
        //   -> writer 가 대기중임으로 나타내는 _QW_WAITING 설정 후, break

		cpu_relax();
	}

	/* When no more readers, set the locked flag */
	for (;;) {
		cnts = atomic_read(&lock->cnts);
        // reader 개수, writer flag 읽어옴 
		if ((cnts == _QW_WAITING) &&
		    (atomic_cmpxchg_acquire(&lock->cnts, _QW_WAITING,
					    _QW_LOCKED) == _QW_WAITING))
			break;
            // reader 가 다 나갈 때까지 기다리다가 lock 획득
            // (cnts 에 reader counter 가 0 이 되어 _QW_WAITING 만 남음)

		cpu_relax();
	}
unlock:
	arch_spin_unlock(&lock->wait_lock);
}
EXPORT_SYMBOL(queued_write_lock_slowpath);
