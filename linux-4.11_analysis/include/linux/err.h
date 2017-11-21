#ifndef _LINUX_ERR_H
#define _LINUX_ERR_H

#include <linux/compiler.h>
#include <linux/types.h>

#include <asm/errno.h>

/*
 * Kernel pointers have redundant information, so we can use a
 * scheme where we can return either an error code or a normal
 * pointer with the same return value.
 *
 * This should be a per-architecture thing, to allow different
 * error and pointer decisions.
 */
#define MAX_ERRNO	4095

#ifndef __ASSEMBLY__

#define IS_ERR_VALUE(x) unlikely((unsigned long)(void *)(x) >= (unsigned long)-MAX_ERRNO)
//  -MAX_ERRNO 즉 -4095 를 unsigned long 으로 cast 한 값과 x 를 비교함. 
//  4095 는0x00..00fff 이를 1의 보수 취하면 0xff..ff000 이고 2의 보수는 0xff..ff001 이 됨.
//  즉 0xff...ff001 <= x <= 0xff..fffff (invalid range of pointer 인지 확인하는 것임)

static inline void * __must_check ERR_PTR(long error)
{
	return (void *) error;
}

static inline long __must_check PTR_ERR(__force const void *ptr)
{
	return (long) ptr;
}

static inline bool __must_check IS_ERR(__force const void *ptr)
{ 
    // __must_check 은 아래와 같이 정의됨. 
    // #define __must_check __attribute__((warn_unused_result))
    // 즉IS_ERR 함수의 return value 가 무언가의 check 용도 등으로 사용되지 안흔다면
    // warning 을 띄우라고 compiler 에게 알리는 것.
    //
    // __force 는 그냥 const void* 로 conversion 을 꼭 하라 하는것. 
	return IS_ERR_VALUE((unsigned long)ptr);
}

static inline bool __must_check IS_ERR_OR_NULL(__force const void *ptr)
{
	return unlikely(!ptr) || IS_ERR_VALUE((unsigned long)ptr);
}

/**
 * ERR_CAST - Explicitly cast an error-valued pointer to another pointer type
 * @ptr: The pointer to cast.
 *
 * Explicitly cast an error-valued pointer to another pointer type in such a
 * way as to make it clear that's what's going on.
 */
static inline void * __must_check ERR_CAST(__force const void *ptr)
{
	/* cast away the const */
	return (void *) ptr;
}

static inline int __must_check PTR_ERR_OR_ZERO(__force const void *ptr)
{
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);
	else
		return 0;
}

/* Deprecated */
#define PTR_RET(p) PTR_ERR_OR_ZERO(p)

#endif

#endif /* _LINUX_ERR_H */
