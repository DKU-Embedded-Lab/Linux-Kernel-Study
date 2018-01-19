#ifndef __ASM_GENERIC_QRWLOCK_TYPES_H
#define __ASM_GENERIC_QRWLOCK_TYPES_H

#include <linux/types.h>
#include <asm/spinlock_types.h>

/*
 * The queue read/write lock data structure
 */

typedef struct qrwlock {
	atomic_t		cnts;
    // reader counter, writer 여부 
    // 0000 0000 0000 0000 0000 0000 0000 0000 
    // <------->
    // writer 여부
    //           <--------------------------->
    //                   reder 개수
	arch_spinlock_t		wait_lock; 
    // cnts 조작을 보호하기 위한 lock
} arch_rwlock_t;

#define	__ARCH_RW_LOCK_UNLOCKED {		\
	.cnts = ATOMIC_INIT(0),			\
	.wait_lock = __ARCH_SPIN_LOCK_UNLOCKED,	\
}

#endif /* __ASM_GENERIC_QRWLOCK_TYPES_H */
