#ifndef _LINUX_VMALLOC_H
#define _LINUX_VMALLOC_H

#include <linux/spinlock.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/llist.h>
#include <asm/page.h>		/* pgprot_t */
#include <linux/rbtree.h>

struct vm_area_struct;		/* vma defining user mapping in mm_types.h */
struct notifier_block;		/* in notifier.h */

/* bits in flags of vmalloc's vm_struct below */
#define VM_IOREMAP		0x00000001	/* ioremap() and friends */
// random 물리 page 가 할당되었으며, vmalloc/ioremap 영역에 할당됨
#define VM_ALLOC		0x00000002	/* vmalloc() */
// vmalloc 통해 할당됨
#define VM_MAP			0x00000004	/* vmap()ed pages */ 
// 물리 memory 도 연속적으로 할당됨
#define VM_USERMAP		0x00000008	/* suitable for remap_vmalloc_range */
// vmalloc page 들을 user-space 에 mapping 해줌
#define VM_UNINITIALIZED	0x00000020	/* vm_struct is not fully initialized */
// vm_struct 가 fully initialized 안됨 읭?
#define VM_NO_GUARD		0x00000040      /* don't add guard page */
// guard hole 따로 없음 
#define VM_KASAN		0x00000080      /* has allocated kasan shadow memory */
// KASan kernel address space 에 사용됨
// 
// KASan 이란?
//  - 2015 년 v4.0 부터 추가된 ASan(Address Sanitizer) 의 kernel space implementation 로써 
//    kernel memory access 에 대해 user-after-free 또는 out-of-bound memory access 등의 
//    memory 취약점 감지
//
//  - kernel address 의 8 byte 에 대한 kasan shadow region 의 1 byte 를 할당해서 
//    (e.g. x86_64 의 경우, 전체 128 TB 의 kernel address space 에 대한(user 128 TB, kernel 128 TB) 잘못된 접근 
//    감지를 위해 address space  중 16 TB 를 kasan address space 로 사용)kernel 의 모든 address space 에 대해 
//    잘못된 접근 감지 
//
//  - kasan 에서의 각 byte 의 의미.
//     * 0 이면 전체 8 byte 영역이 유효함
//     * 1-7 값이면 set 된 byte 만 유효함 
//       e.g. 2 이면 처음 두 byte 만 valid 함
//             kernel addr space                                mapped kasan addr space
//       ...  0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 ...           00000010  
//             v    v   iv   iv   iv   iv   iv   iv
//                          8 byte memory                         1 shadow byte
//
//       e.g. -1 이면 모든 byte invlid 함
//             kernel addr space                                mapped kasan addr space
//       ...  0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00 ...           11111111  
//             iv   iv   iv   iv   iv   iv   iv   iv
//                          8 byte memory                         1 shadow byte
//
//     * 음수면 모든 8 byte 가 non-accessable memory 를 의미. 
//
//  - GCC 5.0 의 kernel address space runtime checking 기능 이용. 
//     * 모든 memory alloc/free operation 이 일어날 때 마다 compiler 가  __asan_loadN() 과 __asan_storeN() 함수로
//       hook 되게 하여 target address 에 대해 검사하고 invalid 시 log 에 출력하며, target addr 에 해당하는 
//       kasan 의 byte 를 수정 
//       e.g. __asan_load8 이 load operation 전에 수행, __asan_store8 이 store operation 전에 수행
//     * memory access 될 때, kasan shadow memory state 를 검사하여 valid 인지 확인
//
//  - kernel bootin 시, zero page 가 KASan shadow addr space 에 map 되고, 
//
//  - shadow_addr = (addr>>3) + KASAN_SHADOW_OFFSET 
//       e.g. 64 bit addr space
//
//       0x0000000000000000--------------
//                           |  user    |
//                  128 TB   |  addr    |
//                           |  space   |
//       0x00007fffffffffff--------------                        ------------
//                           |  hole    |                        |  hole    |
//       0xffff800000000000--------------                        ------------
//                           |   ...    |                        |   ...    |
//       0xffff880000000000--------------                        ------------
//                           |  kernel  |                        |  kernel  |
//                   64 TB   |  direct  | page free ---          |  direct  |---shadow addr 에 해당되는 부분..
//                           |  mapping | page alloc--|----      |  mapping |    - rmqueue 시, alloc 되는 영역에 해당되는
//       0xffffc7ffffffffff--------------             |   |      ------------      Byte 를 0x0 으로 clear
//                           |   ...    |             |   |      |   ...    |    - __free_pages 시,free 되는 영역에 해당되는
//       0xffffec0000000000--------------             |   |      ------------      Byte 를 0xFF 로 poisoning
//                           |  KASan   |             |   |      |  KASan   |      (KASAN_FREE_PAGE)
//       128 TB/8 =  16 TB   |  shadow  |         (addr>>3)+     |  shadow  |  
//                           |  memory  |   +KASAN_SHADOW_OFFSET |  memory  |
//                           |          |             |   |      |          |
//                           |          |<-------------   |      |          |
//                           |          |<-----------------      |          |
//       0xfffffc0000000000--------------                        ------------
//                           |   ...    |                        |   ...    |
//       0xffffffff80000000--------------                        ------------
//                           |  kernel  |                        |  kernel  |
//                  512 MB   |  text    |                        |  text    |
//                           |  mapping |                        |  mapping |
//       0xffffffffa0000000--------------                        ------------
//                           |   ...    |                        |   ...    |
//       0xffffffffffffffff--------------                        ------------
//
/* bits [20..32] reserved for arch specific ioremap internals */

/*
 * Maximum alignment for ioremap() regions.
 * Can be overriden by arch-specific value.
 */
#ifndef IOREMAP_MAX_ORDER
#define IOREMAP_MAX_ORDER	(7 + PAGE_SHIFT)	/* 128 pages */
#endif
// 전체 virtual address space 중 kernel 영역에 해당하는  
// virtual address space 를 관리하기 위한 structure 
// (user space 의 virtual address space 는 vm_area_struct 가 관리)
//  - VM_USERMAP 등으로 vmalloc page 를 user space 에 mapping 해주기도 함
struct vm_struct {
	struct vm_struct	*next;
    // vmalloc 영역을 연결하기 위해 사용
	void			*addr;
    // 연속적인 kernel virtual address space 의 시작 주소 
	unsigned long		size;
    // 연속적인 kernel virtual address space 의 크기
	unsigned long		flags;
    // memory section 관련 flag 들.
    //  - 현재 vm_struct 의 virtual addr space 에 대한 할당 형태.
    //    e.g. VM_MAP : 물리적으로도 연속임
    //         VM_ALLOC : vmalloc 통해 생성됨
    //         VM_NO_GUARD : guard page 없음
    //         ...
	struct page		**pages;
    // kernel virtual addr space 영역에서 사용하는 
    // pointer 배열을 가리킴(이중 pointer) 
    // 즉 이 vm_struct 이 page 10 개 짜리고 물리 mem 의 
    // page 가 3 개로 쪼개져 있으면
    //
    // vaddr ----vpvpvpvpvpvpvpvpvpvp----...    
    //
    //    vm_struct->pages--
    //                     |
    //                  *page----
    //                  *page --|--------------
    //                  *page --|-------------|--------------
    //                          |             |             |
    // paddr -----------------p1p2p3------p4p5p6p7-------p8p9p10
    //
	unsigned int		nr_pages;
    // kernel virtual addr space 에서 사용하느 page 수
	phys_addr_t		phys_addr;
    // 그 page 들 중 첫 page 의 시작 물리 주소 
	const void		*caller;
};
//                  
//                         D ---------------------------
//                       vmap_area                     |
//                        *     *                      |
//      ----------- B    *       *  F -----------------|------------------
//      |         vmap_area      vmap_area             |                 |
//      |          *   *              *  *             |                 |
//      |    A    *  C  *        E   *    *   G        |                 |
//      |  vmap_area vmap_area  vmap_area  vmap_area   |                 |
//      |  ... |         |          |          |       |                 |
//      |      |         |          |          --------|-----------------|--------
//      |      |         |          -------------------|---------        |       |
//      |      |         -----------------------       |        |        |       |
//      -------|-----------------------        |       |        |        |       |
//             ---------------        |        |       |        |        |       |
//                           |        |        |       |        |        |       |
//                         vm_area--vm_area--vm_area--vm_area--vm_area--vm_area-vm_area ..
//                           A        B        C        D        E        F       G 
//                           |       |         |        |        |        |       |
//                          ---     ---     -----   -------     -----   ----     --
//                          | |     | |     | | |   | | | |     | | |   | | |   | |
//                          p p     p p     p p p   p p p p     p p p   p p p   p p
//                          p p     p p     p p p   p p p p     p p p   p p p   p p
//                            p     p       p         p p p     p p p   p   p     p
//                            p             p           p p     p p         p
//                                                      p       p           p
//                                                      p
//
//    --pp--pppp---ppp---pp---pppp----pp--pp----pp--ppp--pppppp---pppp--ppppp--pppp----ppp--ppp-pp---ppppp--pp--ppp-- ...
// 
// kernel virtual address space 에서 vm_area 에 대한 할당 상태를 
// 관리하기 위한 구조체로 red black tree 를 통해 할당 상태 관리 및 
// 할당해줄 영역을 찾는 등의 일 수행
struct vmap_area {
	unsigned long va_start;
    // 해당 vmalloc 영역 vm_struct 의 시작 주소
	unsigned long va_end;
    // 해당 vmalloc 영역 vm_struct 의 끝 주소
	unsigned long flags;
	struct rb_node rb_node;         /* address sorted rbtree */
    // vm_area_struct 가 augmented rb-tree 로 관리되는 것 처럼 
    // vm_struct 도 vmap_area 통해 list 로 관리됨 
	struct list_head list;          /* address sorted list */
    // vmap_area 가 list 로도 관리됨
	struct llist_node purge_list;    /* "lazy purge" list */
    // 현재 vmap_area 가 속한 purge_list(삭제시 한번에 삭제하는 단위)
	struct vm_struct *vm;
    // vmap_area 가 관리하는 vm_area 
	struct rcu_head rcu_head;
};

/*
 *	Highlevel APIs for driver use
 */
extern void vm_unmap_ram(const void *mem, unsigned int count);
extern void *vm_map_ram(struct page **pages, unsigned int count,
				int node, pgprot_t prot);
extern void vm_unmap_aliases(void);

#ifdef CONFIG_MMU
extern void __init vmalloc_init(void);
#else
static inline void vmalloc_init(void)
{
}
#endif

extern void *vmalloc(unsigned long size);
extern void *vzalloc(unsigned long size);
extern void *vmalloc_user(unsigned long size);
extern void *vmalloc_node(unsigned long size, int node);
extern void *vzalloc_node(unsigned long size, int node);
extern void *vmalloc_exec(unsigned long size);
extern void *vmalloc_32(unsigned long size);
extern void *vmalloc_32_user(unsigned long size);
extern void *__vmalloc(unsigned long size, gfp_t gfp_mask, pgprot_t prot);
extern void *__vmalloc_node_range(unsigned long size, unsigned long align,
			unsigned long start, unsigned long end, gfp_t gfp_mask,
			pgprot_t prot, unsigned long vm_flags, int node,
			const void *caller);

extern void vfree(const void *addr);
extern void vfree_atomic(const void *addr);

extern void *vmap(struct page **pages, unsigned int count,
			unsigned long flags, pgprot_t prot);
extern void vunmap(const void *addr);

extern int remap_vmalloc_range_partial(struct vm_area_struct *vma,
				       unsigned long uaddr, void *kaddr,
				       unsigned long size);

extern int remap_vmalloc_range(struct vm_area_struct *vma, void *addr,
							unsigned long pgoff);
void vmalloc_sync_all(void);
 
/*
 *	Lowlevel-APIs (not for driver use!)
 */

static inline size_t get_vm_area_size(const struct vm_struct *area)
{
	if (!(area->flags & VM_NO_GUARD))
		/* return actual size without guard page */
		return area->size - PAGE_SIZE;
	else
		return area->size;

}

extern struct vm_struct *get_vm_area(unsigned long size, unsigned long flags);
extern struct vm_struct *get_vm_area_caller(unsigned long size,
					unsigned long flags, const void *caller);
extern struct vm_struct *__get_vm_area(unsigned long size, unsigned long flags,
					unsigned long start, unsigned long end);
extern struct vm_struct *__get_vm_area_caller(unsigned long size,
					unsigned long flags,
					unsigned long start, unsigned long end,
					const void *caller);
extern struct vm_struct *remove_vm_area(const void *addr);
extern struct vm_struct *find_vm_area(const void *addr);

extern int map_vm_area(struct vm_struct *area, pgprot_t prot,
			struct page **pages);
#ifdef CONFIG_MMU
extern int map_kernel_range_noflush(unsigned long start, unsigned long size,
				    pgprot_t prot, struct page **pages);
extern void unmap_kernel_range_noflush(unsigned long addr, unsigned long size);
extern void unmap_kernel_range(unsigned long addr, unsigned long size);
#else
static inline int
map_kernel_range_noflush(unsigned long start, unsigned long size,
			pgprot_t prot, struct page **pages)
{
	return size >> PAGE_SHIFT;
}
static inline void
unmap_kernel_range_noflush(unsigned long addr, unsigned long size)
{
}
static inline void
unmap_kernel_range(unsigned long addr, unsigned long size)
{
}
#endif

/* Allocate/destroy a 'vmalloc' VM area. */
extern struct vm_struct *alloc_vm_area(size_t size, pte_t **ptes);
extern void free_vm_area(struct vm_struct *area);

/* for /dev/kmem */
extern long vread(char *buf, char *addr, unsigned long count);
extern long vwrite(char *buf, char *addr, unsigned long count);

/*
 *	Internals.  Dont't use..
 */
extern struct list_head vmap_area_list;
extern __init void vm_area_add_early(struct vm_struct *vm);
extern __init void vm_area_register_early(struct vm_struct *vm, size_t align);

#ifdef CONFIG_SMP
# ifdef CONFIG_MMU
struct vm_struct **pcpu_get_vm_areas(const unsigned long *offsets,
				     const size_t *sizes, int nr_vms,
				     size_t align);

void pcpu_free_vm_areas(struct vm_struct **vms, int nr_vms);
# else
static inline struct vm_struct **
pcpu_get_vm_areas(const unsigned long *offsets,
		const size_t *sizes, int nr_vms,
		size_t align)
{
	return NULL;
}

static inline void
pcpu_free_vm_areas(struct vm_struct **vms, int nr_vms)
{
}
# endif
#endif

#ifdef CONFIG_MMU
#define VMALLOC_TOTAL (VMALLOC_END - VMALLOC_START)
#else
#define VMALLOC_TOTAL 0UL
#endif

int register_vmap_purge_notifier(struct notifier_block *nb);
int unregister_vmap_purge_notifier(struct notifier_block *nb);

#endif /* _LINUX_VMALLOC_H */
