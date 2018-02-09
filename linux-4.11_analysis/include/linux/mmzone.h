#ifndef _LINUX_MMZONE_H
#define _LINUX_MMZONE_H

#ifndef __ASSEMBLY__
#ifndef __GENERATING_BOUNDS_H

#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/bitops.h>
#include <linux/cache.h>
#include <linux/threads.h>
#include <linux/numa.h>
#include <linux/init.h>
#include <linux/seqlock.h>
#include <linux/nodemask.h>
#include <linux/pageblock-flags.h>
#include <linux/page-flags-layout.h>
#include <linux/atomic.h>
#include <asm/page.h>

/* Free memory management - zoned buddy allocator.  */
#ifndef CONFIG_FORCE_MAX_ZONEORDER
#define MAX_ORDER 11 
// FORCE_MAX_ZONEORDER 조정을 통해 MAX_ORDER 수정 가능(e.g. arch/arm/Kconfig )
#else
#define MAX_ORDER CONFIG_FORCE_MAX_ZONEORDER
#endif
#define MAX_ORDER_NR_PAGES (1 << (MAX_ORDER - 1))

/*
 * PAGE_ALLOC_COSTLY_ORDER is the order at which allocations are deemed
 * costly to service.  That is between allocation orders which should
 * coalesce naturally under reasonable reclaim pressure and those which
 * will not.
 */
#define PAGE_ALLOC_COSTLY_ORDER 3

enum {
	MIGRATE_UNMOVABLE, 
    // memory 내에서 위치가 변경될 수 없고, 회수도 불가능한 page block
    // kernel domain page 들 중 core 부분의 page 에 속함. 
	MIGRATE_MOVABLE,
    // memory 내에서 위치 변경 가능 및 회수 가능한 page
    // user application 에 의해 할당된 page table 을 통해 mapping 되는 page block
    // page table 내의 mapping 정보 변경을 통해 위치가 변경 될 수 있음
	MIGRATE_RECLAIMABLE,
    // memory 내에서 이동이 불가능하지만, kswapd 등에 의해 page 회수가 가능한 page block
	MIGRATE_PCPTYPES,	/* the number of types on the pcp lists */ 
    // per-CPU cache 에 존재하는 migrate type 까지를 나타내기 위한 flag
	MIGRATE_HIGHATOMIC = MIGRATE_PCPTYPES,
    // MIGRATE_RESERVE 가 삭제되고 추가된 것으로 high-order atomic allocation 을 위한 
    // page 들을 예약해둠(최대 전체 zone 의 1%)
#ifdef CONFIG_CMA
	/*
	 * MIGRATE_CMA migration type is designed to mimic the way
	 * ZONE_MOVABLE works.  Only movable pages can be allocated
	 * from MIGRATE_CMA pageblocks and page allocator never
	 * implicitly change migration type of MIGRATE_CMA pageblock.
	 *
	 * The way to use it is to change migratetype of a range of
	 * pageblocks to MIGRATE_CMA which can be done by
	 * __free_pageblock_cma() function.  What is important though
	 * is that a range of pageblocks must be aligned to
	 * MAX_ORDER_NR_PAGES should biggest page be bigger then
	 * a single pageblock.
	 */
	MIGRATE_CMA,
    // 연속적 물리 memory 를 할당하는 CMA allocator 에 의해 관리되는 page block
#endif
#ifdef CONFIG_MEMORY_ISOLATION
	MIGRATE_ISOLATE,	/* can't allocate from here */
    // memory 회수 등의 작업이 진행되는 동안  기존 page list 에서 일단 
    // 분리시켜 놓기 위한 virtual page type
#endif
	MIGRATE_TYPES
};

/* In mm/page_alloc.c; keep in sync also with show_migration_types() there */
extern char * const migratetype_names[MIGRATE_TYPES];

#ifdef CONFIG_CMA
#  define is_migrate_cma(migratetype) unlikely((migratetype) == MIGRATE_CMA)
#  define is_migrate_cma_page(_page) (get_pageblock_migratetype(_page) == MIGRATE_CMA)
#else
#  define is_migrate_cma(migratetype) false
#  define is_migrate_cma_page(_page) false
#endif

#define for_each_migratetype_order(order, type) \
	for (order = 0; order < MAX_ORDER; order++) \
		for (type = 0; type < MIGRATE_TYPES; type++)

extern int page_group_by_mobility_disabled;

#define NR_MIGRATETYPE_BITS (PB_migrate_end - PB_migrate + 1)
#define MIGRATETYPE_MASK ((1UL << NR_MIGRATETYPE_BITS) - 1)

#define get_pageblock_migratetype(page)					\
	get_pfnblock_flags_mask(page, page_to_pfn(page),		\
			PB_migrate_end, MIGRATETYPE_MASK)

struct free_area {
	struct list_head	free_list[MIGRATE_TYPES];
    // free page 를 page block type 별로 묶기 위해 
    // migrate type 별로 free page block 을 연결한 것 
    // migrate type 단위 page block 관리를 통해 MOVABLE, NON_MOVABLE 을
    // 분리하여 user domain 의 page 와 kernel domain 의 page 를 따로 관리하기 위함
    // 하지만 각 domain 의 page block 부족시 다른 domain 으로 fall back 가능
    // fall back order : MIGRATE_UNMOVABLE -> MIGRATE_RECLAIMABLE -> MIGRATE_MOVABLE -> MIGRATE_RESERVE
    //                   MIGRATE_MOVABLE -> MIGRATE_RECLAIMABLE -> MIGRATE_UNMOVABLE -> MIGRATE_RESERVE       
    // 
    // free_list 에 연결된 page 중 앞에 있을수록 hot page 이며 뒤에 있을 수록 cold 한 page
	unsigned long		nr_free;
    // 현재 order 의 모든 page type 들의 free page 의 수
};

struct pglist_data;

/*
 * zone->lock and the zone lru_lock are two of the hottest locks in the kernel.
 * So add a wild amount of padding here to ensure that they fall into separate
 * cachelines.  There are very few zone structures in the machine, so space
 * consumption is not a concern here.
 */
#if defined(CONFIG_SMP)
struct zone_padding {
	char x[0];
} ____cacheline_internodealigned_in_smp;
#define ZONE_PADDING(name)	struct zone_padding name;
#else
#define ZONE_PADDING(name)
#endif

enum zone_stat_item {
	/* First 128 byte cacheline (assuming 64 bit words) */
	NR_FREE_PAGES, // free page 의 수
	NR_ZONE_LRU_BASE, /* Used only for compaction and reclaim retry */
	NR_ZONE_INACTIVE_ANON = NR_ZONE_LRU_BASE,
	NR_ZONE_ACTIVE_ANON,
	NR_ZONE_INACTIVE_FILE,
	NR_ZONE_ACTIVE_FILE,
	NR_ZONE_UNEVICTABLE,
	NR_ZONE_WRITE_PENDING,	/* Count of dirty, writeback and unstable pages */
	NR_MLOCK,		/* mlock()ed pages found and moved off LRU */
	NR_SLAB_RECLAIMABLE,
	NR_SLAB_UNRECLAIMABLE,
	NR_PAGETABLE,		/* used for pagetables */
	NR_KERNEL_STACK_KB,	/* measured in KiB */
	/* Second 128 byte cacheline */
	NR_BOUNCE,
#if IS_ENABLED(CONFIG_ZSMALLOC)
	NR_ZSPAGES,		/* allocated in zsmalloc */
#endif
#ifdef CONFIG_NUMA
	NUMA_HIT,		/* allocated in intended node */
	NUMA_MISS,		/* allocated in non intended node */
	NUMA_FOREIGN,		/* was intended here, hit elsewhere */
	NUMA_INTERLEAVE_HIT,	/* interleaver preferred this zone */
	NUMA_LOCAL,		/* allocation from local node */
	NUMA_OTHER,		/* allocation from other node */
#endif
	NR_FREE_CMA_PAGES,
	NR_VM_ZONE_STAT_ITEMS };

enum node_stat_item {
	NR_LRU_BASE,
	NR_INACTIVE_ANON = NR_LRU_BASE, /* must match order of LRU_[IN]ACTIVE */ //inactive anonymous page 의 수   
	NR_ACTIVE_ANON,		/*  "     "     "   "       "         */ // active anonymous page 의 수
	NR_INACTIVE_FILE,	/*  "     "     "   "       "         */ // inactive file-backed page 의 수
	NR_ACTIVE_FILE,		/*  "     "     "   "       "         */ // active file-backed page 의 수
	NR_UNEVICTABLE,		/*  "     "     "   "       "         */
	NR_ISOLATED_ANON,	/* Temporary isolated pages from anon lru */
	NR_ISOLATED_FILE,	/* Temporary isolated pages from file lru */
	NR_PAGES_SCANNED,	/* pages scanned since last reclaim */
	WORKINGSET_REFAULT,
	WORKINGSET_ACTIVATE,
	WORKINGSET_NODERECLAIM,
	NR_ANON_MAPPED,	/* Mapped anonymous pages */
	NR_FILE_MAPPED,	/* pagecache pages mapped into pagetables.
			   only modified from process context */
	NR_FILE_PAGES,
	NR_FILE_DIRTY,
	NR_WRITEBACK,
	NR_WRITEBACK_TEMP,	/* Writeback using temporary buffers */
	NR_SHMEM,		/* shmem pages (included tmpfs/GEM pages) */
	NR_SHMEM_THPS,
	NR_SHMEM_PMDMAPPED,
	NR_ANON_THPS,
	NR_UNSTABLE_NFS,	/* NFS unstable pages */
	NR_VMSCAN_WRITE,
	NR_VMSCAN_IMMEDIATE,	/* Prioritise for reclaim when writeback ends */
	NR_DIRTIED,		/* page dirtyings since bootup */
	NR_WRITTEN,		/* page writings since bootup */
	NR_VM_NODE_STAT_ITEMS
};

/*
 * We do arithmetic on the LRU lists in various places in the code,
 * so it is important to keep the active lists LRU_ACTIVE higher in
 * the array than the corresponding inactive lists, and to keep
 * the *_FILE lists LRU_FILE higher than the corresponding _ANON lists.
 *
 * This has to be kept in sync with the statistics in zone_stat_item
 * above and the descriptions in vmstat_text in mm/vmstat.c
 */
#define LRU_BASE 0
#define LRU_ACTIVE 1
#define LRU_FILE 2

enum lru_list {
	LRU_INACTIVE_ANON = LRU_BASE,
	LRU_ACTIVE_ANON = LRU_BASE + LRU_ACTIVE,
	LRU_INACTIVE_FILE = LRU_BASE + LRU_FILE,
	LRU_ACTIVE_FILE = LRU_BASE + LRU_FILE + LRU_ACTIVE, 
    	LRU_UNEVICTABLE,
	NR_LRU_LISTS
}; 


// active 
//  : 처음 참조된 것은 active 가 됨 
//    주기적으로 active, inactive 의 ratio 를 비교하여 참조되지 않는 page 는
//    inactive 로 옮기고, 참조된 것은 active list 의 선두로옮김
// inactive 
//  : memory 부족하여 회수시, 
//    anonymous page  - mem 부족시, swap 으로 옮김
//    file-backed page - mem 부족시, clean 은 회수, 
//                       dirty 는 backing store 에 기록하기 위해  writeback 
//                       을 수행하고 inactive list 의 선두로 옮김(rotate)
//                       rotate 된 후에 다시 후미로 밀려날 경우,page 회수

#define for_each_lru(lru) for (lru = 0; lru < NR_LRU_LISTS; lru++)

#define for_each_evictable_lru(lru) for (lru = 0; lru <= LRU_ACTIVE_FILE; lru++)

static inline int is_file_lru(enum lru_list lru)
{
	return (lru == LRU_INACTIVE_FILE || lru == LRU_ACTIVE_FILE);
}

static inline int is_active_lru(enum lru_list lru)
{
	return (lru == LRU_ACTIVE_ANON || lru == LRU_ACTIVE_FILE);
}

struct zone_reclaim_stat {
	/*
	 * The pageout code in vmscan.c keeps track of how many of the
	 * mem/swap backed and file backed pages are referenced.
	 * The higher the rotated/scanned ratio, the more valuable
	 * that cache is.
	 *
	 * The anon LRU stats live in [0], file LRU stats in [1]
	 */
	unsigned long		recent_rotated[2];
    // 
	unsigned long		recent_scanned[2];
    // 
};

struct lruvec {
	struct list_head		lists[NR_LRU_LISTS];
	struct zone_reclaim_stat	reclaim_stat;
	/* Evictions & activations on the inactive file list */
	atomic_long_t			inactive_age;
#ifdef CONFIG_MEMCG
	struct pglist_data *pgdat;
#endif
};

/* Mask used at gathering information at once (see memcontrol.c) */
#define LRU_ALL_FILE (BIT(LRU_INACTIVE_FILE) | BIT(LRU_ACTIVE_FILE))
#define LRU_ALL_ANON (BIT(LRU_INACTIVE_ANON) | BIT(LRU_ACTIVE_ANON))
#define LRU_ALL	     ((1 << NR_LRU_LISTS) - 1)

/* Isolate unmapped file */
#define ISOLATE_UNMAPPED	((__force isolate_mode_t)0x2)
/* Isolate for asynchronous migration */
#define ISOLATE_ASYNC_MIGRATE	((__force isolate_mode_t)0x4)
/* Isolate unevictable pages */
#define ISOLATE_UNEVICTABLE	((__force isolate_mode_t)0x8)

/* LRU Isolation modes. */
typedef unsigned __bitwise isolate_mode_t;

enum zone_watermarks {
	WMARK_MIN,
    // 이 값보다 낮아지게 되면 mem 할당 요청 있을 때마다 
    // sync 하게 swap 하거나 page reclaim 수행하여 WMARK_HIGH 
    // 에 도달할때 까지 수행 
	WMARK_LOW,
    // 이 값보다 낮아지게 되면 kswapd 깨우고 async 하게 
    // swap 동작함
	WMARK_HIGH,
    // 이거 보다 높으면 충분한 free memory 있음
	NR_WMARK
};

#define min_wmark_pages(z) (z->watermark[WMARK_MIN])
#define low_wmark_pages(z) (z->watermark[WMARK_LOW])
#define high_wmark_pages(z) (z->watermark[WMARK_HIGH])
//
// +---------------------------------------------+
// | +--------+ +--------+ +--------+ +--------+ |
// | |  CPU 0 | |  CPU 1 | |  CPU 2 | |  CPU 3 | |
// | +--------+ +--------+ +--------+ +--------+ |
// | +-----------------------------------------+ |
// | |                   8 MB                  | |
// | +-----------------------------------------+ |
// +-------------------NODE 0--------------------+
//                       
//
//
// node - ZONE_DMA     - per_cpu_pages  
//                           |      
//                           |      --- CPU0 - MIGRATE_UNMOVABLE   - p0-p0-p0-...
//                           |      |          MIGRATE_MOVABLE     - p0-p0-p0-...
//                           |      |          MIGRATE_RECLAIMABLE - p0-p0-p0-...
//                           |      |
//                           |      --- CPU1 - MIGRATE_UNMOVABLE   - p0-p0-p0-...
//                           |      |          MIGRATE_MOVABLE     - p0-p0-p0-...
//                           |      |          MIGRATE_RECLAIMABLE - p0-p0-p0-...
//                           |      |
//                           |      --- CPU2 - MIGRATE_UNMOVABLE   - p0-p0-p0-...
//                           |      |          MIGRATE_MOVABLE     - p0-p0-p0-...
//                           |      |          MIGRATE_RECLAIMABLE - p0-p0-p0-...
//                           |      |
//                           |      --- CPU3 - MIGRATE_UNMOVABLE   - p0-p0-p0-...
//                           |      |          MIGRATE_MOVABLE     - p0-p0-p0-...
//                           |      |          MIGRATE_RECLAIMABLE - p0-p0-p0-...
//                           |      |
//                           +      |
//                         buddy  order 0
//                                order 1
//                                order 2
//                                ...
//
//        ZONE_NORMAL  - per_cpu_pages  same as ZONE_NORMAL
//                           |
//                           +
//                         buddy
//
//        ZONE_HIGHMEM - per_cpu_pages  same as ZONE_HIGHMEM
//                           | 
//                           +
//                         buddy 

// per page cache 로 buddy 애래서 order-0 요청의 page 에 대해 처리 
struct per_cpu_pages {
	int count;		/* number of pages in the list */ 
    // list 의 page 수
	int high;		/* high watermark, emptying needed */
    // count 가 넘으면 안되는 값 즉 count < high 이어야 함
	int batch;		/* chunk size for buddy add/remove */
    // list 가 비게 될 때, buddy 로부터 한번에 받아올 page pool 내의 
    // page 개수

	/* Lists of pages, one per migrate type stored on the pcp-lists */
	struct list_head lists[MIGRATE_PCPTYPES];
    // MIGRATE_UNMOVABLE 
    // MIGRATE_MOVABLE 
    // MIGRATE_RECLAIMABLE 
    // 3 가지 type 의 order 0 개수의 page list 존재     
    // list 에서 뒤에 있을수록(last entry) cold page 이고, 
    // 앞에 있을 수록(first entry) hot page 임
};

// order 0 짜리 요청시 여기서 할당해줌 
// 미리 buddy 로부터 1 개짜리 page 들을 받아 놓음
struct per_cpu_pageset {
	struct per_cpu_pages pcp;
    // per-CPU cache 
#ifdef CONFIG_NUMA
	s8 expire;
#endif
#ifdef CONFIG_SMP
	s8 stat_threshold;
	s8 vm_stat_diff[NR_VM_ZONE_STAT_ITEMS];
#endif
};

struct per_cpu_nodestat {
	s8 stat_threshold;
	s8 vm_node_stat_diff[NR_VM_NODE_STAT_ITEMS];
};

#endif /* !__GENERATING_BOUNDS.H */

enum zone_type {
#ifdef CONFIG_ZONE_DMA 
	/*
	 * ZONE_DMA is used when there are devices that are not able
	 * to do DMA to all of addressable memory (ZONE_NORMAL). Then we
	 * carve out the portion of memory that is needed for these devices.
	 * The range is arch specific.
	 *
	 * Some examples
	 *
	 * Architecture		Limit
	 * ---------------------------
	 * parisc, ia64, sparc	<4G
	 * s390			<2G
	 * arm			Various
	 * alpha		Unlimited or 0-16MB.
	 *
	 * i386, x86_64 and multiple other arches
	 * 			<16M.
	 */
	ZONE_DMA, 
    // dma suitable memory
    // IA-32 는 16MB
#endif
#ifdef CONFIG_ZONE_DMA32
	/*
	 * x86_64 needs two ZONE_DMAs because it supports devices that are
	 * only able to do DMA to the lower 16M but also 32 bit devices that
	 * can only do DMA areas below 4G.
	 */
	ZONE_DMA32,
    // dma suitable memory in 32-bit addressable area. 
    // 64 bit 에서만 있음
    //  32 bit machine 여서 이 영역은 0 크기
    //  amd64 에서는 4G 임 
    //
#endif
	/*
	 * Normal addressable memory is in ZONE_NORMAL. DMA operations can be
	 * performed on pages in ZONE_NORMAL if the DMA devices support
	 * transfers to all addressable memory.
	 */
	ZONE_NORMAL, 
    // kernel 이 direct mapped 된 부분
#ifdef CONFIG_HIGHMEM
	/*
	 * A memory area that is only addressable by the kernel through
	 * mapping portions into its own address space. This is for example
	 * used by i386 to allow the kernel to address the memory beyond
	 * 900MB. The kernel will set up special mappings (page
	 * table entries on i386) for each page that the kernel needs to
	 * access.
	 */
	ZONE_HIGHMEM,
    // 64 bit 에서는 없는 부분
#endif
	ZONE_MOVABLE,
    // persudo zone  
    //
    // kernel 내부에서 fragmentation 을 방지하기 위한 compaction 을 수행하기 위해 구분지어 놓음 
    // memory hotplug 를 위한 zone
#ifdef CONFIG_ZONE_DEVICE
	ZONE_DEVICE,
#endif
	__MAX_NR_ZONES

};

#ifndef __GENERATING_BOUNDS_H

struct zone {
	/* Read-mostly fields */

	/* zone watermarks, access with *_wmark_pages(zone) macros */
	unsigned long watermark[NR_WMARK];
    // free page 의 수를 위한 아래의 세가지 watermark
    // WMARK_MIN, WMARK_LOW, WMARK_HIGH  
    // 
	unsigned long nr_reserved_highatomic;
    // ?
	/*
	 * We don't know if the memory that we're going to allocate will be
	 * freeable or/and it will be released eventually, so to avoid totally
	 * wasting several GB of ram we must reserve some of the lower zone
	 * memory (otherwise we risk to run OOM on the lower zones despite
	 * there being tons of freeable ram on the higher zones).  This array is
	 * recalculated at runtime if the sysctl_lowmem_reserve_ratio sysctl
	 * changes.
	 */
	long lowmem_reserve[MAX_NR_ZONES]; 
    // OOM 을 막기 위해 reserve 되는 page 들 
    // 되게 중요한 memory allocation 요청인데 memory 가 
    // 없어서 못주는 경우를 대비해 각 zone 별로 예비 page 를 둠

#ifdef CONFIG_NUMA
	int node;
    // zone 이 속한 node
#endif
	struct pglist_data	*zone_pgdat; 
    // zone 이 속한 pglist_data 즉 node struct 가리키는 back pointer
	struct per_cpu_pageset __percpu *pageset;
    // per-CPU  hot&cold cache 
    // buddy 에서 할당해 주기 전, order 0 짜리 page 라면 여기서 할당해줌
#ifndef CONFIG_SPARSEMEM
	/*
	 * Flags for a pageblock_nr_pages block. See pageblock-flags.h.
	 * In SPARSEMEM, this map is stored in struct mem_section
	 */
	unsigned long		*pageblock_flags; 
    // zone 내부의 page block 들이 어떤 migrate type 에 속하는지 관리 
    //
#endif /* CONFIG_SPARSEMEM */

	/* zone_start_pfn == zone_start_paddr >> PAGE_SHIFT */
	unsigned long		zone_start_pfn;
    // zone 시작 page 주소
	/*
	 * spanned_pages is the total pages spanned by the zone, including
	 * holes, which is calculated as:
	 * 	spanned_pages = zone_end_pfn - zone_start_pfn;
	 *
	 * present_pages is physical pages existing within the zone, which
	 * is calculated as:
	 *	present_pages = spanned_pages - absent_pages(pages in holes);
	 *
	 * managed_pages is present pages managed by the buddy system, which
	 * is calculated as (reserved_pages includes pages allocated by the
	 * bootmem allocator):
	 *	managed_pages = present_pages - reserved_pages;
	 *
	 * So present_pages may be used by memory hotplug or memory power
	 * management logic to figure out unmanaged pages by checking
	 * (present_pages - managed_pages). And managed_pages should be used
	 * by page allocator and vm scanner to calculate all kinds of watermarks
	 * and thresholds.
	 *
	 * Locking rules:
	 *
	 * zone_start_pfn and spanned_pages are protected by span_seqlock.
	 * It is a seqlock because it has to be read outside of zone->lock,
	 * and it is done in the main allocator path.  But, it is written
	 * quite infrequently.
	 *
	 * The span_seq lock is declared along with zone->lock because it is
	 * frequently read in proximity to zone->lock.  It's good to
	 * give them a chance of being in the same cacheline.
	 *
	 * Write access to present_pages at runtime should be protected by
	 * mem_hotplug_begin/end(). Any reader who can't tolerant drift of
	 * present_pages should get_online_mems() to get a stable value.
	 *
	 * Read access to managed_pages should be safe because it's unsigned
	 * long. Write access to zone->managed_pages and totalram_pages are
	 * protected by managed_page_count_lock at runtime. Idealy only
	 * adjust_managed_page_count() should be used instead of directly
	 * touching zone->managed_pages and totalram_pages.
	 */
	unsigned long		managed_pages;
    // buddy 에 의해 관리되는 page 들을 의미하는 것으로 
    // bootmem_data 에 의해 예약된 page 등 제외한 값임
    // present_pages - reserved_pages
    //
	unsigned long		spanned_pages;
    // hole page 즉 padding 을 포함한 total page 들을 포함한 toatl pages 
    // zone_end_pfn - zone_start_pfn
    //
	unsigned long		present_pages;
    // holge pages 들을 제외한 pages 
    // spanned_pages - absent_pages(pages in holes)
    //

	const char		*name;
    // zone 의 conventional name 을 담고있음. e.g. NORMAL, DMA, HIGHMEM

#ifdef CONFIG_MEMORY_ISOLATION
	/*
	 * Number of isolated pageblock. It is used to solve incorrect
	 * freepage counting problem due to racy retrieving migratetype
	 * of pageblock. Protected by zone->lock.
	 */
	unsigned long		nr_isolate_pageblock; 
    // isolated page 가뭐지 hugepage 만들기 전에 isolate 시키던데..?
#endif

#ifdef CONFIG_MEMORY_HOTPLUG
	/* see spanned/present_pages for more description */
	seqlock_t		span_seqlock; 
    // zone_start_pfn, spanned_pages 에 대한 lock
#endif

	int initialized;
    // zone 초기화 완료되면 설정되는 값
	/* Write-intensive fields used from the page allocator */
	ZONE_PADDING(_pad1_)

	/* free areas of different sizes */
	struct free_area	free_area[MAX_ORDER];
    // buddy 를 위한 order 별 free page 들의 list
    //
	/* zone flags, see below */
	unsigned long		flags;

	/* Primarily protects free_area */
	spinlock_t		lock; 
    // free_area 에 대한 locking

	/* Write-intensive fields used by compaction and vmstats. */
	ZONE_PADDING(_pad2_)

	/*
	 * When free pages are below this point, additional steps are taken
	 * when reading the number of free pages to avoid per-cpu counter
	 * drift allowing watermarks to be breached
	 */
	unsigned long percpu_drift_mark;
    // ??

#if defined CONFIG_COMPACTION || defined CONFIG_CMA
	/* pfn where compaction free scanner should start */
	unsigned long		compact_cached_free_pfn;
	/* pfn where async and sync compaction migration scanner should start */
	unsigned long		compact_cached_migrate_pfn[2];
#endif

#ifdef CONFIG_COMPACTION
	/*
	 * On compaction failure, 1<<compact_defer_shift compactions
	 * are skipped before trying again. The number attempted since
	 * last failure is tracked with compact_considered.
	 */
	unsigned int		compact_considered;
	unsigned int		compact_defer_shift;
	int			compact_order_failed;
#endif

#if defined CONFIG_COMPACTION || defined CONFIG_CMA
	/* Set to true when the PG_migrate_skip bits should be cleared */
	bool			compact_blockskip_flush;
#endif

	bool			contiguous;

	ZONE_PADDING(_pad3_)
	/* Zone statistics */
	atomic_long_t		vm_stat[NR_VM_ZONE_STAT_ITEMS]; 
    // zone 별 각종 통계정보를 관리 
    // e.g. free page 수 , file backed page 수..
} ____cacheline_internodealigned_in_smp;

enum pgdat_flags {
	PGDAT_CONGESTED,		/* pgdat has many dirty pages backed by
					 * a congested BDI
					 */
	PGDAT_DIRTY,			/* reclaim scanning has recently found
					 * many dirty file pages at the tail
					 * of the LRU.
					 */
	PGDAT_WRITEBACK,		/* reclaim scanning has recently found
					 * many pages under writeback
					 */
	PGDAT_RECLAIM_LOCKED,		/* prevents concurrent reclaim */
};

static inline unsigned long zone_end_pfn(const struct zone *zone)
{
	return zone->zone_start_pfn + zone->spanned_pages;
}

static inline bool zone_spans_pfn(const struct zone *zone, unsigned long pfn)
{
	return zone->zone_start_pfn <= pfn && pfn < zone_end_pfn(zone);
}

static inline bool zone_is_initialized(struct zone *zone)
{
	return zone->initialized;
}

static inline bool zone_is_empty(struct zone *zone)
{
	return zone->spanned_pages == 0;
}

/*
 * The "priority" of VM scanning is how much of the queues we will scan in one
 * go. A value of 12 for DEF_PRIORITY implies that we will scan 1/4096th of the
 * queues ("queue_length >> 12") during an aging round.
 */
#define DEF_PRIORITY 12

/* Maximum number of zones on a zonelist */
#define MAX_ZONES_PER_ZONELIST (MAX_NUMNODES * MAX_NR_ZONES)
// 모든 node 에 있는 zone 들 다 합친 것
enum {
	ZONELIST_FALLBACK,	/* zonelist with fallback */
#ifdef CONFIG_NUMA
	/*
	 * The NUMA zonelists are doubled because we need zonelists that
	 * restrict the allocations to a single node for __GFP_THISNODE.
	 */
	ZONELIST_NOFALLBACK,	/* zonelist without fallback (__GFP_THISNODE) */
#endif
	MAX_ZONELISTS
};

/*
 * This struct contains information about a zone in a zonelist. It is stored
 * here to avoid dereferences into large structures and lookups of tables
 */
struct zoneref {
	struct zone *zone;	/* Pointer to actual zone */
	int zone_idx;		/* zone_idx(zoneref->zone) */ 
    // ZONE_DMA, ZONE_NORMAL, ZONE_HIGHMEM 등에 해당하는 idx 
    // 즉 모든 ZONE 다 있다면 ...
    //           ZONE_DMA -> ZONE_DMA32 -> ZONE_NORMAL -> ZONE_HIGHMEM -> ZONE_NORMAL -> ZONE_DEVICE
    // zone_idx      0           1             2              3               4              5 
    //  이며 여기에 해당되는
};

/*
 * One allocation request operates on a zonelist. A zonelist
 * is a list of zones, the first one is the 'goal' of the
 * allocation, the other zones are fallback zones, in decreasing
 * priority.
 *
 * To speed the reading of the zonelist, the zonerefs contain the zone index
 * of the entry being read. Helper functions to access information given
 * a struct zoneref are
 *
 * zonelist_zone()	- Return the struct zone * for an entry in _zonerefs
 * zonelist_zone_idx()	- Return the index of the zone for an entry
 * zonelist_node_idx()	- Return the index of the node for an entry
 */ 
// performance 이유로 각 node 는 node 별 CPU 에 해당하는 local node 에 memory 를 
// 할당하려 하지만 local 에 할당이 실패할 경우를 대비하여 memory allocation 을 위한 
// alternative node 의 list 를 가진다.
// zonelist  배열에서 뒷 entry 일수록 할당 우선순위가 낮음 
// 
// UMA 의 경우.. fallback list 의 초기화 결과
//  NODE : A, B, C (A->B->C)
//  ZONE : ZONE_HIGHMEM-2, ZONE_NORMAL-1, ZONE_DMA-0
//   위와 같다고할 때...
// 
//  node A 의 fallback list 할당 상태는...  
// 
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[0] = A2
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[1] = A1
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[2] = A0
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[3] = B2
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[4] = B1
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[5] = B0
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[6] = C2
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[7] = C1
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[8] = C0
//
// NUMA 의 경우 ... 
//  node A 의 할당 순서가 C,B,D 라고 할때...
//      1
//   A ---- C
// 2 |      | 2
//   |      |
//   B ---- D
//      1
//
//                        |         32bit        |         64bit        |
//                        -----------------------------------------------
// current_zonelist_order | ZONELIST_ORDER_ZONE  |  ZONELIST_ORDER_NODE |
//
// < 32 bit 는... default 로 ZONELIST_ORDER_ZONE 
// 
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[0]  = A HIGHMEM
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[1]  = C HIGHMEM
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[2]  = B HIGHMEM
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[3]  = D HIGHMEM 
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[4]  = A NORMAL
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[5]  = C NORMAL
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[6]  = B NORMAL
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[7]  = D NORMAL
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[8]  = A DMA
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[9]  = C DMA
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[10] = B DMA
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[11] = D DMA
//   
//      node_zonelist[ZONELIST_NOFALLBACK]->_zonerefs[0] = A HIGHMEM
//      node_zonelist[ZONELIST_NOFALLBACK]->_zonerefs[1] = A NORMAL
//      node_zonelist[ZONELIST_NOFALLBACK]->_zonerefs[2] = A DMA
//
// < 64 bit 는... default 로 ZONELIST_ORDER_NODE >
//
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[0]  = A NORMAL
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[1]  = A DMA
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[2]  = C NORMAL
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[3]  = C DMA
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[4]  = B NORMAL
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[5]  = B DMA
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[6]  = D NORMAL
//      node_zonelist[ZONELIST_FALLBACK]->_zonerefs[7]  = D DMA 
//
//      node_zonelist[ZONELIST_NOFALLBACK]->_zonerefs[0] = A NORMAL
//      node_zonelist[ZONELIST_NOFALLBACK]->_zonerefs[1] = A DMA
//
struct zonelist {
	struct zoneref _zonerefs[MAX_ZONES_PER_ZONELIST + 1]; 
    // +1 은 list 의 end 를 위한 null pointer  
};

#ifndef CONFIG_DISCONTIGMEM
/* The array of struct pages - for discontigmem use pgdat->lmem_map */
extern struct page *mem_map;
#endif

/*
 * The pg_data_t structure is used in machines with CONFIG_DISCONTIGMEM
 * (mostly NUMA machines?) to denote a higher-level memory zone than the
 * zone denotes.
 *
 * On NUMA machines, each NUMA node would have a pg_data_t to describe
 * it's memory layout.
 *
 * Memory statistics and page replacement data structures are maintained on a
 * per-zone basis.
 */
struct bootmem_data;
// node 를 나타내는 구조체 struct pglist_data == pg_data_t 
//
// SPARSEMEM, DISCONTIGUOUSMEM, FLATMEM 에 대해... 
//
// FLATMEM
//  - 가장 단순한 방식으로, 연속된 물리주소를 통해 시스템의 메모리에 
//  동등하게 접근할 수 있는 환경으로 kernel에서는 이를 하나의 노드 
//  (contig_page_data)로 관리 가능.
//
// DISCONTIGUOUSMEM
//  - 물리 주소 상에 hole이 있어서 시스템 메모리가 나뉘어져 있는 환경 
//    으로 kernel은 노드 내의 모든 페이지는 연속된 주소를 가진다고 
//    가정하므로, 각각의 나뉘어진 메모리 영역을 별도의 노드로 관리해야함.
//   => 걍 NUMA ?
// 
// SPARSEMEM
//  - contiguous 여부와 상관없이 시스템의 메모리를 섹션이라는 일정한 
//    크기로 나누고 각 섹션을 개별적으로 online/offline 시킬 수 있음.
//    만약 discontiguous memory인 경우라면 처음부터 hole에 해당하는 
//    영역을 offline으로 잡아두면 됨. memory hotpluging을 위해 사용
// 
// UMA - FLATMEM 대부분, DISCONTIGUOUSMEM/SPARSEMEM 도 구성할라면 가능
// NUMA - DISCONTIGUOUSMEM,SPARSEMEM 이 대부분
//
//
typedef struct pglist_data {
	struct zone node_zones[MAX_NR_ZONES];
    // 현재 node 에서 각 zone 에 해당하는 zone struct 배열 
    // DMA,DMA32,NORMAL,HIGHMEM,(MOVABLE)
	struct zonelist node_zonelists[MAX_ZONELISTS];
    // 두가지 zonelist 를 가지고 있음
    // 1. fallback 상황을 위해 다른 node 에 할당하기 위한 zonelist 
    //    현재 zone 에 memory 가 할당 불가능 할 때, 다른 node 에 memory 를 할당하기 위한
    //    우선순위 node list 이며 그 각 node 에 해당하는 zone 정보도 가지고 있음(fallback order) 
    // 2. fallback 이 없는 zonelist  
    //    현재 node 내에서의 zone type 별 fallback list
    //
    //
	int nr_zones;
    // 현재 node 가 가진 zone 의 수
#ifdef CONFIG_FLAT_NODE_MEM_MAP	/* means !SPARSEMEM */
	struct page *node_mem_map;
    // 해당 node의 모든 physical page frame 에 대한 
    // struct page 배열의 첫번째 page 
    // 즉 모든 SYSTEM 의 physical page frame 은 struct page 를 가지고 있음
    // CONFIG_SPARSEMEM 이 설정되어 있으면 이 변수가 없음? 
    // 그러니까. UMA 가 아닌 NUMA 같은 경우에는 이런 page 배열 구조가 없는 건가?
#ifdef CONFIG_PAGE_EXTENSION
	struct page_ext *node_page_ext; 
    // 32bit 에서는 page flag 에 더이상 추가될 공간이 없기 때문에 이 struct 
    // 를 통해 사용
    // flat memory model 이 아닐 경우사용 불가능...
    // idle page tracking, page owner tracking 등의 debugging 용도로 사용
    // flat memory model 이 아닌 경우 CONFIG_PAGE_EXTENSION 옵션을 주어 
    // 설정 가능 
    // 
    // <idle page tracking>
    // page idle flag
    //  - 이 patch 전에 idle page 들을 tracking 하는 방법은...
    //      1. /proc/PID/clear_refs 를 통해 특정 process 가 사용하는 page 
    //         들의 accessed bit 를 clear 
    //         echo 1 > /proc/PID/clear_refs
    //      2. 일정 시간 지난 후, smaps 의 Referenced 를 확인  
    //            # /proc/PID/smaps 
    //              process 의 mapping 정보 확인 가능
    //    => unmapped file pages 는 확인 불가, reclaimer logic 에 영향
    //    
    //  - userspace 에서 /sys/kernel/mm/page_idle/bitmap 에 page 에 해당하는 
    //    offset 위치에 bit 를 set 하여 설정 가능
    //  - page_referenced,read system call(mark_page_accessed) 등을 통해 page 
    //    가 page table 을 통해 접근이 될 때 clear 됨
    //  - 사용 예시
    //      1. /proc/PID/pagemap 을 통해 확인 할 수 있는 특정 workload 에서 
    //         사용되는 page 들을 확인하여 /sys/kernel/mm/page_idle/bitmap 
    //         에 이 page 들에 대해 idle flag 설정 
    //
    //            # /proc/pid/pagemap.  This file lets a userspace process find out which
    //              physical frame each virtual page is mapped to.  It contains one 64-bit
    //              value for each virtual page, containing the following data (from
    //              fs/proc/task_mmu.c, above pagemap_read):
    //
    //              * Bits 0-54  page frame number (PFN) if present
    //              * Bits 0-4   swap type if swapped
    //              * Bits 5-54  swap offset if swapped
    //              * Bit  55    pte is soft-dirty (see Documentation/vm/soft-dirty.txt)
    //              * Bit  56    page exclusively mapped (since 4.2)
    //              * Bits 57-60 zero
    //              * Bit  61    page is file-page or shared-anon (since 3.5)
    //              * Bit  62    page swapped
    //              * Bit  63    page present
    //
    //      2. 일정 시간 지난 후, /sys/kernel/mm/page_idle/bitmap 을 읽어
    //         workload 들에 의해 사용되지 않는 page 들 확인 가능 
    // page young flag
    //  - bitmap file 에 write 를 수행하여 page table entry 의 accessed bit 가 
    //    clear 될 때 page 의 young flag 가 set  됨
    //
    //<page owner tracking>
    //  - 이 patch 전에 page owner 확인하기 위한 방법은  tracepoint 가 있음.
    //    하지만 tracebuffer 를 늘려주어야 하고, pgm 돌면서 계속 buffer를 다 
    //    써버리기 때문에 계속 관리 필요. 따라서 debugging 힘듬
    //  - 사용 예시
    //      * fragmentation statistics 에 확용 가능
    //      * CMA failure debugging 에 활용 가능
    //
    // 2014 년 LG 전자의 김준수 박사님이 추가 개쩜 심지어 CMA 랑 perf 관리자이심 
#endif
#endif
#ifndef CONFIG_NO_BOOTMEM
	struct bootmem_data *bdata;
    // booting 될 때 필요한 boot memory allocator
#endif
#ifdef CONFIG_MEMORY_HOTPLUG
	/*
	 * Must be held any time you expect node_start_pfn, node_present_pages
	 * or node_spanned_pages stay constant.  Holding this will also
	 * guarantee that any pfn_valid() stays that way.
	 *
	 * pgdat_resize_lock() and pgdat_resize_unlock() are provided to
	 * manipulate node_size_lock without checking for CONFIG_MEMORY_HOTPLUG.
	 *
	 * Nests above zone->lock and zone->span_seqlock
	 */
	spinlock_t node_size_lock;
#endif
	unsigned long node_start_pfn;
    // NUMA 에서 각 node 들에 존재하는 page frame  0 부터 시작하는 
    // logical number 를 갖음 이 node 에서의 logical start number 를 의미
    // UMA 의 경우
	unsigned long node_present_pages; /* total number of physical pages */
    // page frame 의 총 개수 (hole 포함)
    // 즉 node 에 normal or high memory 가 있다는 것을 뜻함  
	unsigned long node_spanned_pages; /* total size of physical page
					     range, including holes */
    // page frame 기반으로 계산한 전체 크기
    // hole 미포함
	int node_id;
    // node id
	wait_queue_head_t kswapd_wait; 
    // kswapd  가 zone 에서 page frame 을 backing store 로 내리기 위한 wait queue
	wait_queue_head_t pfmemalloc_wait;
    // page allocation 을 대기중인 task 들의 wait queue
	struct task_struct *kswapd;	/* Protected by
					   mem_hotplug_begin/end() */ 
    // 현재 zone 의 swap 을 담당하고 있는 kswapd 의 task_struct 를 가리킴
	int kswapd_order;
	enum zone_type kswapd_classzone_idx;

#ifdef CONFIG_COMPACTION
	int kcompactd_max_order;
	enum zone_type kcompactd_classzone_idx;
	wait_queue_head_t kcompactd_wait;
    // cpmpaction 을 대기중인 task 들의 wait queue
	struct task_struct *kcompactd; 
    // 현재 zone 의compaction 을 담당하고 있는 kcompactd 의 task_struct 를 가리킴
#endif
#ifdef CONFIG_NUMA_BALANCING
	/* Lock serializing the migrate rate limiting window */
	spinlock_t numabalancing_migrate_lock;

	/* Rate limiting time interval */
	unsigned long numabalancing_migrate_next_window;

	/* Number of pages migrated during the rate limiting time interval */
	unsigned long numabalancing_migrate_nr_pages;
#endif
	/*
	 * This is a per-node reserve of pages that are not available
	 * to userspace allocations.
	 */
	unsigned long		totalreserve_pages;
    // node 내의 OOM 막기 위해 있어햐 하는  최소한의 reserve page 수 
    // node 당 totalreserve_pages 만큼의 free page 는 가지고 있어야 함
#ifdef CONFIG_NUMA
	/*
	 * zone reclaim becomes active if more unmapped pages exist.
	 */
	unsigned long		min_unmapped_pages;
	unsigned long		min_slab_pages;
#endif /* CONFIG_NUMA */

	/* Write-intensive fields used by page reclaim */
	ZONE_PADDING(_pad1_)
	spinlock_t		lru_lock;

#ifdef CONFIG_DEFERRED_STRUCT_PAGE_INIT
	/*
	 * If memory initialisation on large machines is deferred then this
	 * is the first PFN that needs to be initialised.
	 */
	unsigned long first_deferred_pfn;
#endif /* CONFIG_DEFERRED_STRUCT_PAGE_INIT */

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
	spinlock_t split_queue_lock;
    // split queue 에 대한 접근 보호
	struct list_head split_queue;
    // base page 로 split 될 THP 들의 list
	unsigned long split_queue_len;
#endif

	/* Fields commonly accessed by the page reclaim scanner */
	struct lruvec		lruvec;
    // 기존에 struct zone 에서 관리하던  active_list, inactive_list 변수를
    // zone 에서 pglist_data 로 옮김 
    // 기존에 active_list, inactive_list 로 관리하던 것을 lruvec 내의 
    // 5 개로 확대하여 관리 
    //  LRU_ACTIVE_ANON, LRU_INACTIVE_ANON, LRU_ACTIVE_FILE, LRU_INACTIVE_FILE 
    // memory 부족 등으로 page 회수 목적으로 관리 

	/*
	 * The target ratio of ACTIVE_ANON to INACTIVE_ANON pages on
	 * this node's LRU.  Maintained by the pageout code.
	 */
	unsigned int inactive_ratio;

	unsigned long		flags;

	ZONE_PADDING(_pad2_)

	/* Per-node vmstats */
	struct per_cpu_nodestat __percpu *per_cpu_nodestats;
	atomic_long_t		vm_stat[NR_VM_NODE_STAT_ITEMS]; 
    // node 별 각종 통계정보를 관리 
    // e.g. free page 수 , file backed page 수..
} pg_data_t;

#define node_present_pages(nid)	(NODE_DATA(nid)->node_present_pages)
#define node_spanned_pages(nid)	(NODE_DATA(nid)->node_spanned_pages)
#ifdef CONFIG_FLAT_NODE_MEM_MAP
#define pgdat_page_nr(pgdat, pagenr)	((pgdat)->node_mem_map + (pagenr))
#else
#define pgdat_page_nr(pgdat, pagenr)	pfn_to_page((pgdat)->node_start_pfn + (pagenr))
#endif
#define nid_page_nr(nid, pagenr) 	pgdat_page_nr(NODE_DATA(nid),(pagenr))

#define node_start_pfn(nid)	(NODE_DATA(nid)->node_start_pfn)
#define node_end_pfn(nid) pgdat_end_pfn(NODE_DATA(nid))
static inline spinlock_t *zone_lru_lock(struct zone *zone)
{
	return &zone->zone_pgdat->lru_lock;
}

static inline struct lruvec *node_lruvec(struct pglist_data *pgdat)
{
	return &pgdat->lruvec;
}

static inline unsigned long pgdat_end_pfn(pg_data_t *pgdat)
{
	return pgdat->node_start_pfn + pgdat->node_spanned_pages;
}

static inline bool pgdat_is_empty(pg_data_t *pgdat)
{
	return !pgdat->node_start_pfn && !pgdat->node_spanned_pages;
}

static inline int zone_id(const struct zone *zone)
{
	struct pglist_data *pgdat = zone->zone_pgdat;

	return zone - pgdat->node_zones;
}

#ifdef CONFIG_ZONE_DEVICE
static inline bool is_dev_zone(const struct zone *zone)
{
	return zone_id(zone) == ZONE_DEVICE;
}
#else
static inline bool is_dev_zone(const struct zone *zone)
{
	return false;
}
#endif

#include <linux/memory_hotplug.h>

extern struct mutex zonelists_mutex;
void build_all_zonelists(pg_data_t *pgdat, struct zone *zone);
void wakeup_kswapd(struct zone *zone, int order, enum zone_type classzone_idx);
bool __zone_watermark_ok(struct zone *z, unsigned int order, unsigned long mark,
			 int classzone_idx, unsigned int alloc_flags,
			 long free_pages);
bool zone_watermark_ok(struct zone *z, unsigned int order,
		unsigned long mark, int classzone_idx,
		unsigned int alloc_flags);
bool zone_watermark_ok_safe(struct zone *z, unsigned int order,
		unsigned long mark, int classzone_idx);
enum memmap_context {
	MEMMAP_EARLY,
	MEMMAP_HOTPLUG,
};
extern int init_currently_empty_zone(struct zone *zone, unsigned long start_pfn,
				     unsigned long size);

extern void lruvec_init(struct lruvec *lruvec);

static inline struct pglist_data *lruvec_pgdat(struct lruvec *lruvec)
{
#ifdef CONFIG_MEMCG
	return lruvec->pgdat;
#else
	return container_of(lruvec, struct pglist_data, lruvec);
#endif
}

extern unsigned long lruvec_lru_size(struct lruvec *lruvec, enum lru_list lru, int zone_idx);

#ifdef CONFIG_HAVE_MEMORY_PRESENT
void memory_present(int nid, unsigned long start, unsigned long end);
#else
static inline void memory_present(int nid, unsigned long start, unsigned long end) {}
#endif

#ifdef CONFIG_HAVE_MEMORYLESS_NODES
int local_memory_node(int node_id);
#else
static inline int local_memory_node(int node_id) { return node_id; };
#endif

#ifdef CONFIG_NEED_NODE_MEMMAP_SIZE
unsigned long __init node_memmap_size_bytes(int, unsigned long, unsigned long);
#endif

/*
 * zone_idx() returns 0 for the ZONE_DMA zone, 1 for the ZONE_NORMAL zone, etc.
 */
#define zone_idx(zone)		((zone) - (zone)->zone_pgdat->node_zones)

/*
 * Returns true if a zone has pages managed by the buddy allocator.
 * All the reclaim decisions have to use this function rather than
 * populated_zone(). If the whole zone is reserved then we can easily
 * end up with populated_zone() && !managed_zone().
 */
static inline bool managed_zone(struct zone *zone)
{
	return zone->managed_pages;
}

/* Returns true if a zone has memory */
static inline bool populated_zone(struct zone *zone)
{
	return zone->present_pages;
}

extern int movable_zone;

#ifdef CONFIG_HIGHMEM
static inline int zone_movable_is_highmem(void)
{
#ifdef CONFIG_HAVE_MEMBLOCK_NODE_MAP
	return movable_zone == ZONE_HIGHMEM;
#else
	return (ZONE_MOVABLE - 1) == ZONE_HIGHMEM;
#endif
}
#endif

static inline int is_highmem_idx(enum zone_type idx)
{
#ifdef CONFIG_HIGHMEM
	return (idx == ZONE_HIGHMEM ||
		(idx == ZONE_MOVABLE && zone_movable_is_highmem()));
#else
	return 0;
#endif
}

/**
 * is_highmem - helper function to quickly check if a struct zone is a 
 *              highmem zone or not.  This is an attempt to keep references
 *              to ZONE_{DMA/NORMAL/HIGHMEM/etc} in general code to a minimum.
 * @zone - pointer to struct zone variable
 */
static inline int is_highmem(struct zone *zone)
{
#ifdef CONFIG_HIGHMEM
	return is_highmem_idx(zone_idx(zone));
#else
	return 0;
#endif
}

/* These two functions are used to setup the per zone pages min values */
struct ctl_table;
int min_free_kbytes_sysctl_handler(struct ctl_table *, int,
					void __user *, size_t *, loff_t *);
int watermark_scale_factor_sysctl_handler(struct ctl_table *, int,
					void __user *, size_t *, loff_t *);
extern int sysctl_lowmem_reserve_ratio[MAX_NR_ZONES-1];
int lowmem_reserve_ratio_sysctl_handler(struct ctl_table *, int,
					void __user *, size_t *, loff_t *);
int percpu_pagelist_fraction_sysctl_handler(struct ctl_table *, int,
					void __user *, size_t *, loff_t *);
int sysctl_min_unmapped_ratio_sysctl_handler(struct ctl_table *, int,
			void __user *, size_t *, loff_t *);
int sysctl_min_slab_ratio_sysctl_handler(struct ctl_table *, int,
			void __user *, size_t *, loff_t *);

extern int numa_zonelist_order_handler(struct ctl_table *, int,
			void __user *, size_t *, loff_t *);
extern char numa_zonelist_order[];
#define NUMA_ZONELIST_ORDER_LEN 16	/* string buffer size */

#ifndef CONFIG_NEED_MULTIPLE_NODES

extern struct pglist_data contig_page_data;
#define NODE_DATA(nid)		(&contig_page_data)
#define NODE_MEM_MAP(nid)	mem_map

#else /* CONFIG_NEED_MULTIPLE_NODES */

#include <asm/mmzone.h>

#endif /* !CONFIG_NEED_MULTIPLE_NODES */

extern struct pglist_data *first_online_pgdat(void);
extern struct pglist_data *next_online_pgdat(struct pglist_data *pgdat);
extern struct zone *next_zone(struct zone *zone);

/**
 * for_each_online_pgdat - helper macro to iterate over all online nodes
 * @pgdat - pointer to a pg_data_t variable
 */
#define for_each_online_pgdat(pgdat)			\
	for (pgdat = first_online_pgdat();		\
	     pgdat;					\
	     pgdat = next_online_pgdat(pgdat))
/**
 * for_each_zone - helper macro to iterate over all memory zones
 * @zone - pointer to struct zone variable
 *
 * The user only needs to declare the zone variable, for_each_zone
 * fills it in.
 */
#define for_each_zone(zone)			        \
	for (zone = (first_online_pgdat())->node_zones; \
	     zone;					\
	     zone = next_zone(zone))

#define for_each_populated_zone(zone)		        \
	for (zone = (first_online_pgdat())->node_zones; \
	     zone;					\
	     zone = next_zone(zone))			\
		if (!populated_zone(zone))		\
			; /* do nothing */		\
		else

static inline struct zone *zonelist_zone(struct zoneref *zoneref)
{
	return zoneref->zone;
}

static inline int zonelist_zone_idx(struct zoneref *zoneref)
{
	return zoneref->zone_idx;
}

static inline int zonelist_node_idx(struct zoneref *zoneref)
{
#ifdef CONFIG_NUMA
	/* zone_to_nid not available in this context */
	return zoneref->zone->node;
#else
	return 0;
#endif /* CONFIG_NUMA */
}

struct zoneref *__next_zones_zonelist(struct zoneref *z,
					enum zone_type highest_zoneidx,
					nodemask_t *nodes);

/**
 * next_zones_zonelist - Returns the next zone at or below highest_zoneidx within the allowed nodemask using a cursor within a zonelist as a starting point
 * @z - The cursor used as a starting point for the search
 * @highest_zoneidx - The zone index of the highest zone to return
 * @nodes - An optional nodemask to filter the zonelist with
 *
 * This function returns the next zone at or below a given zone index that is
 * within the allowed nodemask using a cursor as the starting point for the
 * search. The zoneref returned is a cursor that represents the current zone
 * being examined. It should be advanced by one before calling
 * next_zones_zonelist again.
 */
static __always_inline struct zoneref *next_zones_zonelist(struct zoneref *z,
					enum zone_type highest_zoneidx,
					nodemask_t *nodes)
{
	if (likely(!nodes && zonelist_zone_idx(z) <= highest_zoneidx))
		return z;
	return __next_zones_zonelist(z, highest_zoneidx, nodes);
}

/**
 * first_zones_zonelist - Returns the first zone at or below highest_zoneidx within the allowed nodemask in a zonelist
 * @zonelist - The zonelist to search for a suitable zone
 * @highest_zoneidx - The zone index of the highest zone to return
 * @nodes - An optional nodemask to filter the zonelist with
 * @return - Zoneref pointer for the first suitable zone found (see below)
 *
 * This function returns the first zone at or below a given zone index that is
 * within the allowed nodemask. The zoneref returned is a cursor that can be
 * used to iterate the zonelist with next_zones_zonelist by advancing it by
 * one before calling.
 *
 * When no eligible zone is found, zoneref->zone is NULL (zoneref itself is
 * never NULL). This may happen either genuinely, or due to concurrent nodemask
 * update due to cpuset modification.
 */
static inline struct zoneref *first_zones_zonelist(struct zonelist *zonelist,
					enum zone_type highest_zoneidx,
					nodemask_t *nodes)
{
	return next_zones_zonelist(zonelist->_zonerefs,
							highest_zoneidx, nodes);
}

/**
 * for_each_zone_zonelist_nodemask - helper macro to iterate over valid zones in a zonelist at or below a given zone index and within a nodemask
 * @zone - The current zone in the iterator
 * @z - The current pointer within zonelist->zones being iterated
 * @zlist - The zonelist being iterated
 * @highidx - The zone index of the highest zone to return
 * @nodemask - Nodemask allowed by the allocator
 *
 * This iterator iterates though all zones at or below a given zone index and
 * within a given nodemask
 */
#define for_each_zone_zonelist_nodemask(zone, z, zlist, highidx, nodemask) \
	for (z = first_zones_zonelist(zlist, highidx, nodemask), zone = zonelist_zone(z);	\
		zone;							\
		z = next_zones_zonelist(++z, highidx, nodemask),	\
			zone = zonelist_zone(z))

#define for_next_zone_zonelist_nodemask(zone, z, zlist, highidx, nodemask) \
	for (zone = z->zone;	\
		zone;							\
		z = next_zones_zonelist(++z, highidx, nodemask),	\
			zone = zonelist_zone(z))
// #define for_next_zone_zonelist_nodemask(zone, z, zlist, highidx, nodemask) 
//	for (struct zone = (struct zoneref*)z->zone ;  
//	zone;
//	z = next_zones_zonelist(++z, highidx, nodemask),zone = zonelist_zone(z)) 
//	++z 인 이유는 
//	zone 에서 zonelist 내부가 
//	struct zoneref _zonerefs[MAX_ZONES_PER_ZONELIST + 1];
//	이렇게 되어있어서?


/**
 * for_each_zone_zonelist - helper macro to iterate over valid zones in a zonelist at or below a given zone index
 * @zone - The current zone in the iterator
 * @z - The current pointer within zonelist->zones being iterated
 * @zlist - The zonelist being iterated
 * @highidx - The zone index of the highest zone to return
 *
 * This iterator iterates though all zones at or below a given zone index.
 */
#define for_each_zone_zonelist(zone, z, zlist, highidx) \
	for_each_zone_zonelist_nodemask(zone, z, zlist, highidx, NULL)

#ifdef CONFIG_SPARSEMEM
#include <asm/sparsemem.h>
#endif

#if !defined(CONFIG_HAVE_ARCH_EARLY_PFN_TO_NID) && \
	!defined(CONFIG_HAVE_MEMBLOCK_NODE_MAP)
static inline unsigned long early_pfn_to_nid(unsigned long pfn)
{
	return 0;
}
#endif

#ifdef CONFIG_FLATMEM
#define pfn_to_nid(pfn)		(0)
#endif

#ifdef CONFIG_SPARSEMEM

/*
 * SECTION_SHIFT    		#bits space required to store a section #
 *
 * PA_SECTION_SHIFT		physical address to/from section number
 * PFN_SECTION_SHIFT		pfn to/from section number
 */
#define PA_SECTION_SHIFT	(SECTION_SIZE_BITS)
#define PFN_SECTION_SHIFT	(SECTION_SIZE_BITS - PAGE_SHIFT)

#define NR_MEM_SECTIONS		(1UL << SECTIONS_SHIFT)

#define PAGES_PER_SECTION       (1UL << PFN_SECTION_SHIFT)
#define PAGE_SECTION_MASK	(~(PAGES_PER_SECTION-1))

#define SECTION_BLOCKFLAGS_BITS \
	((1UL << (PFN_SECTION_SHIFT - pageblock_order)) * NR_PAGEBLOCK_BITS)

#if (MAX_ORDER - 1 + PAGE_SHIFT) > SECTION_SIZE_BITS
#error Allocator MAX_ORDER exceeds SECTION_SIZE
#endif

#define pfn_to_section_nr(pfn) ((pfn) >> PFN_SECTION_SHIFT)
#define section_nr_to_pfn(sec) ((sec) << PFN_SECTION_SHIFT)

#define SECTION_ALIGN_UP(pfn)	(((pfn) + PAGES_PER_SECTION - 1) & PAGE_SECTION_MASK)
#define SECTION_ALIGN_DOWN(pfn)	((pfn) & PAGE_SECTION_MASK)

struct page;
struct page_ext;
struct mem_section {
	/*
	 * This is, logically, a pointer to an array of struct
	 * pages.  However, it is stored with some other magic.
	 * (see sparse.c::sparse_init_one_section())
	 *
	 * Additionally during early boot we encode node id of
	 * the location of the section here to guide allocation.
	 * (see sparse.c::memory_present())
	 *
	 * Making it a UL at least makes someone do a cast
	 * before using it wrong.
	 */
	unsigned long section_mem_map;

	/* See declaration of similar field in struct zone */
	unsigned long *pageblock_flags;
#ifdef CONFIG_PAGE_EXTENSION
	/*
	 * If SPARSEMEM, pgdat doesn't have page_ext pointer. We use
	 * section. (see page_ext.h about this.)
	 */
	struct page_ext *page_ext;
	unsigned long pad;
#endif
	/*
	 * WARNING: mem_section must be a power-of-2 in size for the
	 * calculation and use of SECTION_ROOT_MASK to make sense.
	 */
};

#ifdef CONFIG_SPARSEMEM_EXTREME
#define SECTIONS_PER_ROOT       (PAGE_SIZE / sizeof (struct mem_section))
#else
#define SECTIONS_PER_ROOT	1
#endif

#define SECTION_NR_TO_ROOT(sec)	((sec) / SECTIONS_PER_ROOT)
#define NR_SECTION_ROOTS	DIV_ROUND_UP(NR_MEM_SECTIONS, SECTIONS_PER_ROOT)
#define SECTION_ROOT_MASK	(SECTIONS_PER_ROOT - 1)

#ifdef CONFIG_SPARSEMEM_EXTREME
extern struct mem_section *mem_section[NR_SECTION_ROOTS];
#else
extern struct mem_section mem_section[NR_SECTION_ROOTS][SECTIONS_PER_ROOT];
#endif

static inline struct mem_section *__nr_to_section(unsigned long nr)
{
	if (!mem_section[SECTION_NR_TO_ROOT(nr)])
		return NULL;
	return &mem_section[SECTION_NR_TO_ROOT(nr)][nr & SECTION_ROOT_MASK];
}
extern int __section_nr(struct mem_section* ms);
extern unsigned long usemap_size(void);

/*
 * We use the lower bits of the mem_map pointer to store
 * a little bit of information.  There should be at least
 * 3 bits here due to 32-bit alignment.
 */
#define	SECTION_MARKED_PRESENT	(1UL<<0)
#define SECTION_HAS_MEM_MAP	(1UL<<1)
#define SECTION_MAP_LAST_BIT	(1UL<<2)
#define SECTION_MAP_MASK	(~(SECTION_MAP_LAST_BIT-1))
#define SECTION_NID_SHIFT	2

static inline struct page *__section_mem_map_addr(struct mem_section *section)
{
	unsigned long map = section->section_mem_map;
	map &= SECTION_MAP_MASK;
	return (struct page *)map;
}

static inline int present_section(struct mem_section *section)
{
	return (section && (section->section_mem_map & SECTION_MARKED_PRESENT));
}

static inline int present_section_nr(unsigned long nr)
{
	return present_section(__nr_to_section(nr));
}

static inline int valid_section(struct mem_section *section)
{
	return (section && (section->section_mem_map & SECTION_HAS_MEM_MAP));
}

static inline int valid_section_nr(unsigned long nr)
{
	return valid_section(__nr_to_section(nr));
}

static inline struct mem_section *__pfn_to_section(unsigned long pfn)
{
	return __nr_to_section(pfn_to_section_nr(pfn));
}

#ifndef CONFIG_HAVE_ARCH_PFN_VALID
static inline int pfn_valid(unsigned long pfn)
{
	if (pfn_to_section_nr(pfn) >= NR_MEM_SECTIONS)
		return 0;
	return valid_section(__nr_to_section(pfn_to_section_nr(pfn)));
}
#endif

static inline int pfn_present(unsigned long pfn)
{
	if (pfn_to_section_nr(pfn) >= NR_MEM_SECTIONS)
		return 0;
	return present_section(__nr_to_section(pfn_to_section_nr(pfn)));
}

/*
 * These are _only_ used during initialisation, therefore they
 * can use __initdata ...  They could have names to indicate
 * this restriction.
 */
#ifdef CONFIG_NUMA
#define pfn_to_nid(pfn)							\
({									\
	unsigned long __pfn_to_nid_pfn = (pfn);				\
	page_to_nid(pfn_to_page(__pfn_to_nid_pfn));			\
})
#else
#define pfn_to_nid(pfn)		(0)
#endif

#define early_pfn_valid(pfn)	pfn_valid(pfn)
void sparse_init(void);
#else
#define sparse_init()	do {} while (0)
#define sparse_index_init(_sec, _nid)  do {} while (0)
#endif /* CONFIG_SPARSEMEM */

/*
 * During memory init memblocks map pfns to nids. The search is expensive and
 * this caches recent lookups. The implementation of __early_pfn_to_nid
 * may treat start/end as pfns or sections.
 */
struct mminit_pfnnid_cache {
	unsigned long last_start;
	unsigned long last_end;
	int last_nid;
};

#ifndef early_pfn_valid
#define early_pfn_valid(pfn)	(1)
#endif

void memory_present(int nid, unsigned long start, unsigned long end);
unsigned long __init node_memmap_size_bytes(int, unsigned long, unsigned long);

/*
 * If it is possible to have holes within a MAX_ORDER_NR_PAGES, then we
 * need to check pfn validility within that MAX_ORDER_NR_PAGES block.
 * pfn_valid_within() should be used in this case; we optimise this away
 * when we have no holes within a MAX_ORDER_NR_PAGES block.
 */
#ifdef CONFIG_HOLES_IN_ZONE
#define pfn_valid_within(pfn) pfn_valid(pfn)
#else
#define pfn_valid_within(pfn) (1)
#endif

#ifdef CONFIG_ARCH_HAS_HOLES_MEMORYMODEL
/*
 * pfn_valid() is meant to be able to tell if a given PFN has valid memmap
 * associated with it or not. In FLATMEM, it is expected that holes always
 * have valid memmap as long as there is valid PFNs either side of the hole.
 * In SPARSEMEM, it is assumed that a valid section has a memmap for the
 * entire section.
 *
 * However, an ARM, and maybe other embedded architectures in the future
 * free memmap backing holes to save memory on the assumption the memmap is
 * never used. The page_zone linkages are then broken even though pfn_valid()
 * returns true. A walker of the full memmap must then do this additional
 * check to ensure the memmap they are looking at is sane by making sure
 * the zone and PFN linkages are still valid. This is expensive, but walkers
 * of the full memmap are extremely rare.
 */
bool memmap_valid_within(unsigned long pfn,
					struct page *page, struct zone *zone);
#else
static inline bool memmap_valid_within(unsigned long pfn,
					struct page *page, struct zone *zone)
{
	return true;
}
#endif /* CONFIG_ARCH_HAS_HOLES_MEMORYMODEL */

#endif /* !__GENERATING_BOUNDS.H */
#endif /* !__ASSEMBLY__ */
#endif /* _LINUX_MMZONE_H */
