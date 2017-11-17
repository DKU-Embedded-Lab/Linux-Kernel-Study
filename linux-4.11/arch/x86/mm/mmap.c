/*
 * Flexible mmap layout support
 *
 * Based on code by Ingo Molnar and Andi Kleen, copyrighted
 * as follows:
 *
 * Copyright 2003-2009 Red Hat Inc.
 * All Rights Reserved.
 * Copyright 2005 Andi Kleen, SUSE Labs.
 * Copyright 2007 Jiri Kosina, SUSE Labs.
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/personality.h>
#include <linux/mm.h>
#include <linux/random.h>
#include <linux/limits.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <asm/elf.h>

struct va_alignment __read_mostly va_align = {
	.flags = -1,
};

static unsigned long stack_maxrandom_size(void)
{
	unsigned long max = 0;
	if ((current->flags & PF_RANDOMIZE) &&
		!(current->personality & ADDR_NO_RANDOMIZE)) {
		max = ((-1UL) & STACK_RND_MASK) << PAGE_SHIFT;
	}

	return max;
}

/*
 * Top of mmap area (just below the process stack).
 *
 * Leave an at least ~128 MB hole with possible stack randomization.
 */
#define MIN_GAP (128*1024*1024UL + stack_maxrandom_size())
#define MAX_GAP (TASK_SIZE/6*5)

static int mmap_is_legacy(void)
{    
	if (current->personality & ADDR_COMPAT_LAYOUT)
		return 1;
    // personality system call 을 통해 legacy layout 
    // 으로 하라고 설정되어 있는지 검사
	if (rlimit(RLIMIT_STACK) == RLIM_INFINITY)
		return 1;
    // tsk->signal->rlim[limit].rlim_cur 에 설정된 rlimit 
    // 배열요소 중 3 번째 entry 인  stack size 관련 soft limit 
    // 값이 고정되어 있는지 검사.
    // legacy layout 은 stack size 가 지정되어 있지 않고 
    // default layout 은 아래로 자라는 mmap 영역을 침범하지 않기 위해
    // stack size 가 고정이기 때문에 RLIM_INFINITY 로 설정되어 있다면 
    // legacy layout 임 
	return sysctl_legacy_va_layout; 
    // /proc/sys/vm/legacy_va_layout 에 설정된 값을 반환
}

unsigned long arch_mmap_rnd(void)
{
	unsigned long rnd;

	if (mmap_is_ia32())
#ifdef CONFIG_COMPAT
		rnd = get_random_long() & ((1UL << mmap_rnd_compat_bits) - 1);
#else
		rnd = get_random_long() & ((1UL << mmap_rnd_bits) - 1);
#endif
	else
		rnd = get_random_long() & ((1UL << mmap_rnd_bits) - 1);

	return rnd << PAGE_SHIFT;
}

static unsigned long mmap_base(unsigned long rnd)
{
	unsigned long gap = rlimit(RLIMIT_STACK);
    // default layout 일 경우, stack size 가 고정임
	if (gap < MIN_GAP)
		gap = MIN_GAP; 
    // MIN_GAP : 128*1024*1024UL + stack_maxrandom_size() 
    //           stack 크기가 최소 128 MB 는 되도록 설정
	else if (gap > MAX_GAP)
		gap = MAX_GAP;
    // MAX_GAP : (TASK_SIZE/6*5) 
    //           stack 의 최대 크기 제한
	return PAGE_ALIGN(TASK_SIZE - gap - rnd);
    // stack 의 크기를 통해 mmap 의 시작 위치 구함 
    //
    //  | kernel|
    //  --------- -> TASK_SIZE
    //  | rnd v | -> rnd        +
    //  | stack | +             |
    //  |       | |  stack size | gap
    //  |       | |             |
    //  |       | +             |
    //  |-------|               +
    //  | rnd v | -> rnd
    //  ---------
    //  | mmap  |
    //   ...
    //
}

/*
 * This function, called very early during the creation of a new
 * process VM image, sets up which VM layout function to use:
 */ 
// new process 의 vma layout 생성시마다 호출되어 layout type 결정
void arch_pick_mmap_layout(struct mm_struct *mm)
{
	unsigned long random_factor = 0UL;
    // PF_RANDOMIZE 설정되어 있다면 즉 vma 에 randomize 적용할 꺼면
    // 각 architecture 마다 제공하는 random generator 함수 사용하여 
    // random long sized number 생성
	if (current->flags & PF_RANDOMIZE)
		random_factor = arch_mmap_rnd();
    // 상위 주소로 자라는 mmap layout 의 시작 주소에 random 값 추가하여
    // legacy mmap start area 초기화
	mm->mmap_legacy_base = TASK_UNMAPPED_BASE + random_factor;
    // 이제 어떤 vm layout 을 할지 mmap_is_legacy 함수를 통해 결정
	if (mmap_is_legacy()) { 
		mm->mmap_base = mm->mmap_legacy_base; 
        // legacy 일 경우, mmap 시작 주소를 TASK_UNMAPPED_BASE 로 고정위치
		mm->get_unmapped_area = arch_get_unmapped_area;
        // free area 찾는 함수 설정
	} else {
		mm->mmap_base = mmap_base(random_factor);
        // mmap_base 함수를 통해 고정된 TASK_SIZE - stack 크기 -  random 값으로 
        // mmap 주소 설정
		mm->get_unmapped_area = arch_get_unmapped_area_topdown;
	}
}

const char *arch_vma_name(struct vm_area_struct *vma)
{
	if (vma->vm_flags & VM_MPX)
		return "[mpx]";
	return NULL;
}
