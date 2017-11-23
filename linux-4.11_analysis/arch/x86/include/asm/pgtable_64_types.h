#ifndef _ASM_X86_PGTABLE_64_DEFS_H
#define _ASM_X86_PGTABLE_64_DEFS_H

#include <asm/sparsemem.h>

#ifndef __ASSEMBLY__
#include <linux/types.h>
#include <asm/kaslr.h>

/*
 * These are used to make use of C type-checking..
 */
typedef unsigned long	pteval_t;
typedef unsigned long	pmdval_t;
typedef unsigned long	pudval_t;
typedef unsigned long	pgdval_t;
typedef unsigned long	pgprotval_t;

typedef struct { pteval_t pte; } pte_t; 
// unsigned long 하나 뿐인데 struct 안에 들엉 있는 이유는
// Helper function 을 통해서만 접근 및 조작이 가능하도록 하기 위해

#endif	/* !__ASSEMBLY__ */

#define SHARED_KERNEL_PMD	0

/*
 * PGDIR_SHIFT determines what a top-level page table entry can map
 */
#define PGDIR_SHIFT	39
#define PTRS_PER_PGD	512

/*
 * 3rd level page
 */
#define PUD_SHIFT	30
#define PTRS_PER_PUD	512

/*
 * PMD_SHIFT determines the size of the area a middle-level
 * page table can map
 */
#define PMD_SHIFT	21
#define PTRS_PER_PMD	512

/*
 * entries per page directory level
 */
#define PTRS_PER_PTE	512

//                                          
// --------------------------------------------------------------
// |    PGD     |   PUD     |   PMD     |   PTE     |   offset  |
// --------------------------------------------------------------
//                                                       12
//                                            9     <----------->
//                                                  PAGE_SHIFT(12) => 4096 개entry
//                                9     <----------------------->
//                                             PMD_SHIFT(21)       => 512 개 entry
//                    9     <----------------------------------->
//                                       PUD_SHIFT(30)             => 512 개 entry
//       9      <----------------------------------------------->
//                              PGDIR_SHIFT(39)                    => 512 개 entry     
// 
// virtual address 의 나머지 16bit 를 Canonial address space 를 표현하기 위해 사용
// 48~64 까지는 User 영역인지, Kernel 영역인지를 구분하기 위해 사용함.
// USER 영역주소가 0 bit 부터 쭈욱 addressing 되어 46 번 bit 까지 사용 
// KERNEL 영역 주소는 63 bit ~ 47 bit 까지 1로 설정된 상태에서 다시 
// 0bit 부터 addressing
//
//
//      0x0000 0000 0000 0000 
//    ~ 0x0000 7FFF FFFF FFFF  : USER 영역
//
//      0xFFFF 8000 0000 0000 
//    ~ 0XFFFF FFFF FFFF FFFF  : KERNEL 영역
//
#define PMD_SIZE	(_AC(1, UL) << PMD_SHIFT)
#define PMD_MASK	(~(PMD_SIZE - 1))
#define PUD_SIZE	(_AC(1, UL) << PUD_SHIFT)
#define PUD_MASK	(~(PUD_SIZE - 1))
#define PGDIR_SIZE	(_AC(1, UL) << PGDIR_SHIFT)
#define PGDIR_MASK	(~(PGDIR_SIZE - 1))

/* See Documentation/x86/x86_64/mm.txt for a description of the memory map. */
#define MAXMEM		_AC(__AC(1, UL) << MAX_PHYSMEM_BITS, UL)
#define VMALLOC_SIZE_TB	_AC(32, UL)
#define __VMALLOC_BASE	_AC(0xffffc90000000000, UL)
#define __VMEMMAP_BASE	_AC(0xffffea0000000000, UL)
#ifdef CONFIG_RANDOMIZE_MEMORY
#define VMALLOC_START	vmalloc_base
#define VMEMMAP_START	vmemmap_base
#else
#define VMALLOC_START	__VMALLOC_BASE
#define VMEMMAP_START	__VMEMMAP_BASE
#endif /* CONFIG_RANDOMIZE_MEMORY */
#define VMALLOC_END	(VMALLOC_START + _AC((VMALLOC_SIZE_TB << 40) - 1, UL))
#define MODULES_VADDR    (__START_KERNEL_map + KERNEL_IMAGE_SIZE)
#define MODULES_END      _AC(0xffffffffff000000, UL)
#define MODULES_LEN   (MODULES_END - MODULES_VADDR)
#define ESPFIX_PGD_ENTRY _AC(-2, UL)
#define ESPFIX_BASE_ADDR (ESPFIX_PGD_ENTRY << PGDIR_SHIFT)
#define EFI_VA_START	 ( -4 * (_AC(1, UL) << 30))
#define EFI_VA_END	 (-68 * (_AC(1, UL) << 30))

#define EARLY_DYNAMIC_PAGE_TABLES	64

#endif /* _ASM_X86_PGTABLE_64_DEFS_H */
