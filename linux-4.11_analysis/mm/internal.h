/* internal.h: mm/ internal definitions
 *
 * Copyright (C) 2004 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#ifndef __MM_INTERNAL_H
#define __MM_INTERNAL_H

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/tracepoint-defs.h>

/*
 * The set of flags that only affect watermark checking and reclaim
 * behaviour. This is used by the MM to obey the caller constraints
 * about IO, FS and watermark checking while ignoring placement
 * hints such as HIGHMEM usage.
 */
#define GFP_RECLAIM_MASK (__GFP_RECLAIM|__GFP_HIGH|__GFP_IO|__GFP_FS|\
			__GFP_NOWARN|__GFP_REPEAT|__GFP_NOFAIL|\
			__GFP_NORETRY|__GFP_MEMALLOC|__GFP_NOMEMALLOC|\
			__GFP_ATOMIC)

/* The GFP flags allowed during early boot */
#define GFP_BOOT_MASK (__GFP_BITS_MASK & ~(__GFP_RECLAIM|__GFP_IO|__GFP_FS))

/* Control allocation cpuset and node placement constraints */
#define GFP_CONSTRAINT_MASK (__GFP_HARDWALL|__GFP_THISNODE)

/* Do not use these with a slab allocator */
#define GFP_SLAB_BUG_MASK (__GFP_DMA32|__GFP_HIGHMEM|~__GFP_BITS_MASK)

void page_writeback_init(void);

int do_swap_page(struct vm_fault *vmf);

void free_pgtables(struct mmu_gather *tlb, struct vm_area_struct *start_vma,
		unsigned long floor, unsigned long ceiling);

static inline bool can_madv_dontneed_vma(struct vm_area_struct *vma)
{
	return !(vma->vm_flags & (VM_LOCKED|VM_HUGETLB|VM_PFNMAP));
}

void unmap_page_range(struct mmu_gather *tlb,
			     struct vm_area_struct *vma,
			     unsigned long addr, unsigned long end,
			     struct zap_details *details);

extern int __do_page_cache_readahead(struct address_space *mapping,
		struct file *filp, pgoff_t offset, unsigned long nr_to_read,
		unsigned long lookahead_size);

/*
 * Submit IO for the read-ahead request in file_ra_state.
 */
static inline unsigned long ra_submit(struct file_ra_state *ra,
		struct address_space *mapping, struct file *filp)
{
	return __do_page_cache_readahead(mapping, filp,
					ra->start, ra->size, ra->async_size);
}

/*
 * Turn a non-refcounted page (->_refcount == 0) into refcounted with
 * a count of one.
 */
static inline void set_page_refcounted(struct page *page)
{
	VM_BUG_ON_PAGE(PageTail(page), page);
	VM_BUG_ON_PAGE(page_ref_count(page), page);
	set_page_count(page, 1);
}

extern unsigned long highest_memmap_pfn;

/*
 * in mm/vmscan.c:
 */
extern int isolate_lru_page(struct page *page);
extern void putback_lru_page(struct page *page);
extern bool pgdat_reclaimable(struct pglist_data *pgdat);

/*
 * in mm/rmap.c:
 */
extern pmd_t *mm_find_pmd(struct mm_struct *mm, unsigned long address);

/*
 * in mm/page_alloc.c
 */

/*
 * Structure for holding the mostly immutable allocation parameters passed
 * between functions involved in allocations, including the alloc_pages*
 * family of functions.
 *
 * nodemask, migratetype and high_zoneidx are initialized only once in
 * __alloc_pages_nodemask() and then never change.
 *
 * zonelist, preferred_zone and classzone_idx are set first in
 * __alloc_pages_nodemask() for the fast path, and might be later changed
 * in __alloc_pages_slowpath(). All other functions pass the whole strucure
 * by a const pointer.
 */
struct alloc_context {
	struct zonelist *zonelist;
    // page 할당 실패시 다음 free_area 를 찾을 fallback list 
	nodemask_t *nodemask;
    // gfp 값과 mempolicy 에 의해 설정된 nodemask 로 
    // page 할당 가능한 node
	struct zoneref *preferred_zoneref; 
    // fast path 를 위해 사용?  
	int migratetype;
    // page 를 받아올 migrate type
	enum zone_type high_zoneidx;   
    // 할당 요청이 들어온 ZONE
	bool spread_dirty_pages;
    // write 용 file page cache 할당 요청인지 검사 
    // node 별 dirty page 를 동일하게 배분되도록 해야함
};

#define ac_classzone_idx(ac) zonelist_zone_idx(ac->preferred_zoneref)

/*
 * Locate the struct page for both the matching buddy in our
 * pair (buddy1) and the combined O(n+1) page they form (page).
 *
 * 1) Any buddy B1 will have an order O twin B2 which satisfies
 * the following equation:
 *     B2 = B1 ^ (1 << O)
 * For example, if the starting buddy (buddy2) is #8 its order
 * 1 buddy is #10:
 *     B2 = 8 ^ (1 << 1) = 8 ^ 2 = 10
 *
 * 2) Any buddy B will have an order O+1 parent P which
 * satisfies the following equation:
 *     P = B & ~(1 << O)
 *
 * Assumption: *_mem_map is contiguous at least up to MAX_ORDER
 */
// page_pfn 에 대한 order 단위 buddy page frame 을 구하는 함수. 
// XOR 연산을 활용하여 buddy 구함
//  - 나머지 bit 그대로 두고 order 크기의 bit 가 설정되어 있다면 
//    꺼주고 설정되어 있지 않다면 켜줌으로써 buddy 를 구함 
//    e.g. 
//         order 1 0000 0001 에 대해 on/off 
//         pfn 0  ... 0000 0000 -> buddy_pfn 1  ... 0000 0001
//         pfn 1  ... 0000 0001 -> buddy_pfn 0  ... 0000 0000
//
//         order 3 0000 1000 에 대해 on/off
//         pfn 16 ... 0001 0000 -> buddy_pfn 24 ... 0001 1000
//         pfn 24 ... 0001 1000 -> buddy_pfn 16 ... 0001 0000
//
static inline unsigned long
__find_buddy_pfn(unsigned long page_pfn, unsigned int order)
{
	return page_pfn ^ (1 << order);
    // order 위치의 bit 를 set/unset 하여 buddy pfn 구함
}

extern struct page *__pageblock_pfn_to_page(unsigned long start_pfn,
				unsigned long end_pfn, struct zone *zone);
// 
// pageblock 내의 page 범위인 start_pfn ~ end_pfn 이 속한 zone 이 
// zone 이 맞다면 첫 base page 의 struct page 반환
static inline struct page *pageblock_pfn_to_page(unsigned long start_pfn,
				unsigned long end_pfn, struct zone *zone)
{
	if (zone->contiguous)
		return pfn_to_page(start_pfn);

	return __pageblock_pfn_to_page(start_pfn, end_pfn, zone);
}

extern int __isolate_free_page(struct page *page, unsigned int order);
extern void __free_pages_bootmem(struct page *page, unsigned long pfn,
					unsigned int order);
extern void prep_compound_page(struct page *page, unsigned int order);
extern void post_alloc_hook(struct page *page, unsigned int order,
					gfp_t gfp_flags);
extern int user_min_free_kbytes;

#if defined CONFIG_COMPACTION || defined CONFIG_CMA

/*
 * in mm/compaction.c
 */
/*
 * compact_control is used to track pages being migrated and the free pages
 * they are being migrated to during memory compaction. The free_pfn starts
 * at the end of a zone and migrate_pfn begins at the start. Movable pages
 * are moved to the end of a zone during a compaction run and the run
 * completes when free_pfn <= migrate_pfn
 */
// page compactino 과정에서 migrate scanner, free scanner 에 의해 
// migrate 되는 page 들을 관리하기 위한 structure
struct compact_control {
	struct list_head freepages;	/* List of free pages to migrate to */
    // scan 결과 LRU list 에서 isolated 된 free page 들
	struct list_head migratepages;	/* List of pages being migrated */
    // scan 결과 LRU list 에서 isolated 된 in-use page 들 
	unsigned long nr_freepages;	/* Number of isolated free pages */
    // page 의 내용들이 옮겨지기 위해 LRU 에서 isolate 된 free page 
    // 각 scanning round 당 횟수
	unsigned long nr_migratepages;	/* Number of pages to migrate */
    // page 의 내용들이 옮겨지기 위해 LRU 에서 isolate 된 migrate page 
    // 즉 PG_isolated 설정된 page 의 수 
    // 각 scanning round 당 횟수  
	unsigned long total_migrate_scanned;
    // 총 migrate scanner 가 scan 한 page 들
	unsigned long total_free_scanned;
    // 총 free scanner 가 scan 함 page 들
	unsigned long free_pfn;		/* isolate_freepages search base */
    // free scanner 가 scan 시작 할 위치
	unsigned long migrate_pfn;	/* isolate_migratepages search base */
    // migrate scanner 가 scan 시작 할 위치
	unsigned long last_migrated_pfn;/* Not yet flushed page being freed */ 
    // 최근에 PG_isolated 설정된 page frame 
	enum migrate_mode mode;		/* Async or sync migration mode */
    // kcompactd 에 의한 asynchronous comapction 인지 
    // page alloc 과정에서의 synchronous compaction 인지 구분
	bool ignore_skip_hint;		/* Scan blocks even if marked skip */
    // page block 내의 compaction scanning 을 도와주기 위한 
    // 해당 page block 에 대해 skip 하라는 PB_migrate_skip bit 가 설정되어 
    // 있어도 migrate 가능 검사를 수행한다. 
	bool ignore_block_suitable;	/* Scan blocks considered unsuitable */
	bool direct_compaction;		/* False from kcompactd or /proc/... */
    // page fault 과정에서의 compaction 이면 1 임 
    // kcompactd 또는 /proc/sys/vm/compact_memory 를 통한 compactino 이면0 임 
	bool whole_zone;		/* Whole zone should/has been scanned */
    // 전체 zone 에 대해 scan 필요한지 검사.
    // (e.g. compaction priority 가 높으면 즉 free page 확보 최대한 해야 하면 
    // 전체 zone 에 대해 compaction 수행함)
	int order;			/* order a direct compactor needs */
    // direct compactino 시, compaction 을통해 할당해야 하는 page 수
	const gfp_t gfp_mask;		/* gfp mask of a direct compactor */
    // direct compaction 시의 gfp flag
	const unsigned int alloc_flags;	/* alloc flags of a direct compactor */
	const int classzone_idx;	/* zone index of a direct compactor */
	struct zone *zone;
	bool contended;			/* Signal lock or sched contention */ 
    // asynchronous mode (kcompactd) 에 의한 scanning 시...
    //  - 더 우선순위 높은 task 있거나, CPU time 시간 다됫거나 등 
    //    reschedule 요청이 있어 scanning 종료 
    //  - pending signal 이 있어 종료
};

unsigned long
isolate_freepages_range(struct compact_control *cc,
			unsigned long start_pfn, unsigned long end_pfn);
unsigned long
isolate_migratepages_range(struct compact_control *cc,
			   unsigned long low_pfn, unsigned long end_pfn);
int find_suitable_fallback(struct free_area *area, unsigned int order,
			int migratetype, bool only_stealable, bool *can_steal);

#endif

/*
 * This function returns the order of a free page in the buddy system. In
 * general, page_zone(page)->lock must be held by the caller to prevent the
 * page from being allocated in parallel and returning garbage as the order.
 * If a caller does not hold page_zone(page)->lock, it must guarantee that the
 * page cannot be allocated or merged in parallel. Alternatively, it must
 * handle invalid values gracefully, and use page_order_unsafe() below.
 */
static inline unsigned int page_order(struct page *page)
{
	/* PageBuddy() must be checked by the caller */
	return page_private(page);
}

/*
 * Like page_order(), but for callers who cannot afford to hold the zone lock.
 * PageBuddy() should be checked first by the caller to minimize race window,
 * and invalid values must be handled gracefully.
 *
 * READ_ONCE is used so that if the caller assigns the result into a local
 * variable and e.g. tests it for valid range before using, the compiler cannot
 * decide to remove the variable and inline the page_private(page) multiple
 * times, potentially observing different values in the tests and the actual
 * use of the result.
 */
#define page_order_unsafe(page)		READ_ONCE(page_private(page))

static inline bool is_cow_mapping(vm_flags_t flags)
{
	return (flags & (VM_SHARED | VM_MAYWRITE)) == VM_MAYWRITE;
}

/*
 * These three helpers classifies VMAs for virtual memory accounting.
 */

/*
 * Executable code area - executable, not writable, not stack
 */
static inline bool is_exec_mapping(vm_flags_t flags)
{
	return (flags & (VM_EXEC | VM_WRITE | VM_STACK)) == VM_EXEC;
}

/*
 * Stack area - atomatically grows in one direction
 *
 * VM_GROWSUP / VM_GROWSDOWN VMAs are always private anonymous:
 * do_mmap() forbids all other combinations.
 */
static inline bool is_stack_mapping(vm_flags_t flags)
{
	return (flags & VM_STACK) == VM_STACK;
}

/*
 * Data area - private, writable, not stack
 */
static inline bool is_data_mapping(vm_flags_t flags)
{
	return (flags & (VM_WRITE | VM_SHARED | VM_STACK)) == VM_WRITE;
}

/* mm/util.c */
void __vma_link_list(struct mm_struct *mm, struct vm_area_struct *vma,
		struct vm_area_struct *prev, struct rb_node *rb_parent);

#ifdef CONFIG_MMU
extern long populate_vma_page_range(struct vm_area_struct *vma,
		unsigned long start, unsigned long end, int *nonblocking);
extern void munlock_vma_pages_range(struct vm_area_struct *vma,
			unsigned long start, unsigned long end);
static inline void munlock_vma_pages_all(struct vm_area_struct *vma)
{
	munlock_vma_pages_range(vma, vma->vm_start, vma->vm_end);
}

/*
 * must be called with vma's mmap_sem held for read or write, and page locked.
 */
extern void mlock_vma_page(struct page *page);
extern unsigned int munlock_vma_page(struct page *page);

/*
 * Clear the page's PageMlocked().  This can be useful in a situation where
 * we want to unconditionally remove a page from the pagecache -- e.g.,
 * on truncation or freeing.
 *
 * It is legal to call this function for any page, mlocked or not.
 * If called for a page that is still mapped by mlocked vmas, all we do
 * is revert to lazy LRU behaviour -- semantics are not broken.
 */
extern void clear_page_mlock(struct page *page);

/*
 * mlock_migrate_page - called only from migrate_misplaced_transhuge_page()
 * (because that does not go through the full procedure of migration ptes):
 * to migrate the Mlocked page flag; update statistics.
 */
static inline void mlock_migrate_page(struct page *newpage, struct page *page)
{
	if (TestClearPageMlocked(page)) {
		int nr_pages = hpage_nr_pages(page);

		/* Holding pmd lock, no change in irq context: __mod is safe */
		__mod_zone_page_state(page_zone(page), NR_MLOCK, -nr_pages);
		SetPageMlocked(newpage);
		__mod_zone_page_state(page_zone(newpage), NR_MLOCK, nr_pages);
	}
}

extern pmd_t maybe_pmd_mkwrite(pmd_t pmd, struct vm_area_struct *vma);

/*
 * At what user virtual address is page expected in @vma?
 */
static inline unsigned long
__vma_address(struct page *page, struct vm_area_struct *vma)
{
	pgoff_t pgoff = page_to_pgoff(page);
    // vm 에서의 vm_pgoff 와 같은 index 값을 가져옴
	return vma->vm_start + ((pgoff - vma->vm_pgoff) << PAGE_SHIFT);
}

static inline unsigned long
vma_address(struct page *page, struct vm_area_struct *vma)
{
	unsigned long start, end;

	start = __vma_address(page, vma);
	end = start + PAGE_SIZE * (hpage_nr_pages(page) - 1);

	/* page should be within @vma mapping range */
	VM_BUG_ON_VMA(end < vma->vm_start || start >= vma->vm_end, vma);

	return max(start, vma->vm_start);
}

#else /* !CONFIG_MMU */
static inline void clear_page_mlock(struct page *page) { }
static inline void mlock_vma_page(struct page *page) { }
static inline void mlock_migrate_page(struct page *new, struct page *old) { }

#endif /* !CONFIG_MMU */

/*
 * Return the mem_map entry representing the 'offset' subpage within
 * the maximally aligned gigantic page 'base'.  Handle any discontiguity
 * in the mem_map at MAX_ORDER_NR_PAGES boundaries.
 */
static inline struct page *mem_map_offset(struct page *base, int offset)
{
	if (unlikely(offset >= MAX_ORDER_NR_PAGES))
		return nth_page(base, offset);
	return base + offset;
}

/*
 * Iterator over all subpages within the maximally aligned gigantic
 * page 'base'.  Handle any discontiguity in the mem_map.
 */
static inline struct page *mem_map_next(struct page *iter,
						struct page *base, int offset)
{
	if (unlikely((offset & (MAX_ORDER_NR_PAGES - 1)) == 0)) {
		unsigned long pfn = page_to_pfn(base) + offset;
		if (!pfn_valid(pfn))
			return NULL;
		return pfn_to_page(pfn);
	}
	return iter + 1;
}

/*
 * FLATMEM and DISCONTIGMEM configurations use alloc_bootmem_node,
 * so all functions starting at paging_init should be marked __init
 * in those cases. SPARSEMEM, however, allows for memory hotplug,
 * and alloc_bootmem_node is not used.
 */
#ifdef CONFIG_SPARSEMEM
#define __paginginit __meminit
#else
#define __paginginit __init
#endif

/* Memory initialisation debug and verification */
enum mminit_level {
	MMINIT_WARNING,
	MMINIT_VERIFY,
	MMINIT_TRACE
};

#ifdef CONFIG_DEBUG_MEMORY_INIT

extern int mminit_loglevel;

#define mminit_dprintk(level, prefix, fmt, arg...) \
do { \
	if (level < mminit_loglevel) { \
		if (level <= MMINIT_WARNING) \
			pr_warn("mminit::" prefix " " fmt, ##arg);	\
		else \
			printk(KERN_DEBUG "mminit::" prefix " " fmt, ##arg); \
	} \
} while (0)

extern void mminit_verify_pageflags_layout(void);
extern void mminit_verify_zonelist(void);
#else

static inline void mminit_dprintk(enum mminit_level level,
				const char *prefix, const char *fmt, ...)
{
}

static inline void mminit_verify_pageflags_layout(void)
{
}

static inline void mminit_verify_zonelist(void)
{
}
#endif /* CONFIG_DEBUG_MEMORY_INIT */

/* mminit_validate_memmodel_limits is independent of CONFIG_DEBUG_MEMORY_INIT */
#if defined(CONFIG_SPARSEMEM)
extern void mminit_validate_memmodel_limits(unsigned long *start_pfn,
				unsigned long *end_pfn);
#else
static inline void mminit_validate_memmodel_limits(unsigned long *start_pfn,
				unsigned long *end_pfn)
{
}
#endif /* CONFIG_SPARSEMEM */

#define NODE_RECLAIM_NOSCAN	-2
#define NODE_RECLAIM_FULL	-1
#define NODE_RECLAIM_SOME	0
#define NODE_RECLAIM_SUCCESS	1

extern int hwpoison_filter(struct page *p);

extern u32 hwpoison_filter_dev_major;
extern u32 hwpoison_filter_dev_minor;
extern u64 hwpoison_filter_flags_mask;
extern u64 hwpoison_filter_flags_value;
extern u64 hwpoison_filter_memcg;
extern u32 hwpoison_filter_enable;

extern unsigned long  __must_check vm_mmap_pgoff(struct file *, unsigned long,
        unsigned long, unsigned long,
        unsigned long, unsigned long);

extern void set_pageblock_order(void);
unsigned long reclaim_clean_pages_from_list(struct zone *zone,
					    struct list_head *page_list);
/* The ALLOC_WMARK bits are used as an index to zone->watermark */
#define ALLOC_WMARK_MIN		WMARK_MIN 
// zone->watermark[0] 사용
#define ALLOC_WMARK_LOW		WMARK_LOW
// zone->watermark[1] 사용
#define ALLOC_WMARK_HIGH	WMARK_HIGH
// zone->watermark[2] 사용
#define ALLOC_NO_WATERMARKS	0x04 /* don't check watermarks at all */

/* Mask to get the watermark bits */
#define ALLOC_WMARK_MASK	(ALLOC_NO_WATERMARKS-1)

#define ALLOC_HARDER		0x10 /* try to alloc harder */ 
// 할당 watermark 의 기준값에 대한 rule 
// 이값 설정시, WMARK_MIN 값을 기존 min 의 1/4 배 감소
#define ALLOC_HIGH		0x20 /* __GFP_HIGH set */
// 할당 watermark 의 기준값에 대한 rule 
// 이값 설정시, WMARK_MIN 값을 기존 min 의 1/2 배 감소
#define ALLOC_CPUSET		0x40 /* check for correct cpuset */ 
// memory 를 cpuset 설정된 CPU 에 달린 영역에서 가져와야됨
// 즉 cgroup 의 cpu subsystem 에서 설정한 정책 사용
#define ALLOC_CMA		0x80 /* allow allocations from CMA areas */
// memory 를 CMA 영역에서 가져와야 한다. 
enum ttu_flags;
struct tlbflush_unmap_batch;


/*
 * only for MM internal work items which do not depend on
 * any allocations or locks which might depend on allocations
 */
extern struct workqueue_struct *mm_percpu_wq;

#ifdef CONFIG_ARCH_WANT_BATCHED_UNMAP_TLB_FLUSH
void try_to_unmap_flush(void);
void try_to_unmap_flush_dirty(void);
#else
static inline void try_to_unmap_flush(void)
{
}
static inline void try_to_unmap_flush_dirty(void)
{
}

#endif /* CONFIG_ARCH_WANT_BATCHED_UNMAP_TLB_FLUSH */

extern const struct trace_print_flags pageflag_names[];
extern const struct trace_print_flags vmaflag_names[];
extern const struct trace_print_flags gfpflag_names[];

#endif	/* __MM_INTERNAL_H */
