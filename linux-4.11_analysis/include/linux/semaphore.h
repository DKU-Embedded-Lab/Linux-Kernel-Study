/*
 * Copyright (c) 2008 Intel Corporation
 * Author: Matthew Wilcox <willy@linux.intel.com>
 *
 * Distributed under the terms of the GNU GPL, version 2
 *
 * Please see kernel/semaphore.c for documentation of these functions
 */
#ifndef __LINUX_SEMAPHORE_H
#define __LINUX_SEMAPHORE_H

#include <linux/list.h>
#include <linux/spinlock.h>

/* Please don't access any members of this structure directly */
struct semaphore {
	raw_spinlock_t		lock;
    // wait list 를 보호하기 위한 spinlock 
	unsigned int		count;
    // critical section 에 들어갈 수 있는 process 의 수
	struct list_head	wait_list;
    // lock 얻기위해 대기중인 process 들
};

#define __SEMAPHORE_INITIALIZER(name, n)				\
{									\
	.lock		= __RAW_SPIN_LOCK_UNLOCKED((name).lock),	\
	.count		= n,						\
	.wait_list	= LIST_HEAD_INIT((name).wait_list),		\
}

#define DEFINE_SEMAPHORE(name)	\
	struct semaphore name = __SEMAPHORE_INITIALIZER(name, 1) 
// 정적 초기화는 binary semaphore 용도

static inline void sema_init(struct semaphore *sem, int val)
{
	static struct lock_class_key __key;
	*sem = (struct semaphore) __SEMAPHORE_INITIALIZER(*sem, val);
	lockdep_init_map(&sem->lock.dep_map, "semaphore->lock", &__key, 0); 
    // lock validator 관련 함수
}

extern void down(struct semaphore *sem);
extern int __must_check down_interruptible(struct semaphore *sem);
// semaphore acquire 함수. 성공시, count 감소하며 실패시 TASK_INTERRUPTABLE
extern int __must_check down_killable(struct semaphore *sem); 
// down_interruptible 와 같지만 실패시 TASK_KILLABLE
extern int __must_check down_trylock(struct semaphore *sem);
extern int __must_check down_timeout(struct semaphore *sem, long jiffies);
// jiffies timeout 만큼 sleep 하다가 interrupt 됨
extern void up(struct semaphore *sem);

#endif /* __LINUX_SEMAPHORE_H */
