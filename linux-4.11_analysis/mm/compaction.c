/*
 * linux/mm/compaction.c
 *
 * Memory compaction for the reduction of external fragmentation. Note that
 * this heavily depends upon page migration to do all the real heavy
 * lifting
 *
 * Copyright IBM Corp. 2007-2010 Mel Gorman <mel@csn.ul.ie>
 */
#include <linux/cpu.h>
#include <linux/swap.h>
#include <linux/migrate.h>
#include <linux/compaction.h>
#include <linux/mm_inline.h>
#include <linux/sched/signal.h>
#include <linux/backing-dev.h>
#include <linux/sysctl.h>
#include <linux/sysfs.h>
#include <linux/page-isolation.h>
#include <linux/kasan.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/page_owner.h>
#include "internal.h"

#ifdef CONFIG_COMPACTION
static inline void count_compact_event(enum vm_event_item item)
{
	count_vm_event(item);
}

static inline void count_compact_events(enum vm_event_item item, long delta)
{
	count_vm_events(item, delta);
}
#else
#define count_compact_event(item) do { } while (0)
#define count_compact_events(item, delta) do { } while (0)
#endif

#if defined CONFIG_COMPACTION || defined CONFIG_CMA

#define CREATE_TRACE_POINTS
#include <trace/events/compaction.h>

#define block_start_pfn(pfn, order)	round_down(pfn, 1UL << (order))
#define block_end_pfn(pfn, order)	ALIGN((pfn) + 1, 1UL << (order))
#define pageblock_start_pfn(pfn)	block_start_pfn(pfn, pageblock_order)
// low_pfn 이 속한 page block 내의 start pfn
#define pageblock_end_pfn(pfn)		block_end_pfn(pfn, pageblock_order)
// low_pfn 이 속한 page block 내의 end pfn

static unsigned long release_freepages(struct list_head *freelist)
{
	struct page *page, *next;
	unsigned long high_pfn = 0;

	list_for_each_entry_safe(page, next, freelist, lru) {
		unsigned long pfn = page_to_pfn(page);
		list_del(&page->lru);
		__free_page(page);
		if (pfn > high_pfn)
			high_pfn = pfn;
	}

	return high_pfn;
}

// list 의 즉 freelist 에 있는 buddy 로부터 isolate 된 
// page 들에 대해 임시 list 에 옮기고, list 합쳐줌 
// 즉 order-0 단위 page 여러개의 list 로 만들어줌
static void map_pages(struct list_head *list)
{
	unsigned int i, order, nr_pages;
	struct page *page, *next;
	LIST_HEAD(tmp_list);
    // tmp_list 라는 list_head 생성 및 초기화 

    // ... ---pppp----pp----pppppppp---- ...
    //   
    //   pppp                    p-p-p-p
    //   |                             |
    //   pp                =>    -------
    //   |                       |
    //   pppppppp                p-p
    //   |                         |
    //   ...                     ---
    //                           |
    //                           p-p-p-p-p-p-p-p
    //                           ...
    //    

	list_for_each_entry_safe(page, next, list, lru) {
    // list 에 있는 buddy 로부터 isolte 된 
		list_del(&page->lru);
        // list 에서 page 삭제
		order = page_private(page);
        // 그 page 의 private 에서 order 정보 가져옴 
		nr_pages = 1 << order;

		post_alloc_hook(page, order, __GFP_MOVABLE);
        // page flag, order 지우기, _refcount 1 설정 등의 
        // page 할당되기 전의 초기화 작업 수행
		if (order)
			split_page(page, order);
            // 연속적 page 범위 내의 모든 page 들의 
            // _refcount 값을 설정해줌
		for (i = 0; i < nr_pages; i++) {
            // 임시 list 에 split 정보 update 
            // 한 base page 를 추가
			list_add(&page->lru, &tmp_list);
			page++;
		}
	}

	list_splice(&tmp_list, list);
}
// synchronous compaction 의 경우 scanning 될수 있는 page 의 조건
// migratee type 이 MOVABLE 이어야 함
static inline bool migrate_async_suitable(int migratetype)
{
	return is_migrate_cma(migratetype) || migratetype == MIGRATE_MOVABLE;
}

#ifdef CONFIG_COMPACTION
// file-backed page 이면서 isolate_page 연산이 있다면 true
int PageMovable(struct page *page)
{
	struct address_space *mapping;

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	if (!__PageMovable(page))
		return 0;
        // movable page 가 아니라면 0 반환

    // movable page 인데
    mapping = page_mapping(page);
    // anonymous page 라면 mapping 은 NULL
	if (mapping && mapping->a_ops && mapping->a_ops->isolate_page)
		return 1;
        // file-backed page 이며 관련연산 있으면 1 반환
	return 0;
    // anonymous page 라면 NULL 반환
}
EXPORT_SYMBOL(PageMovable);

void __SetPageMovable(struct page *page, struct address_space *mapping)
{
	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE((unsigned long)mapping & PAGE_MAPPING_MOVABLE, page);
	page->mapping = (void *)((unsigned long)mapping | PAGE_MAPPING_MOVABLE);
}
EXPORT_SYMBOL(__SetPageMovable);

void __ClearPageMovable(struct page *page)
{
	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(!PageMovable(page), page);
	/*
	 * Clear registered address_space val with keeping PAGE_MAPPING_MOVABLE
	 * flag so that VM can catch up released page by driver after isolation.
	 * With it, VM migration doesn't try to put it back.
	 */
	page->mapping = (void *)((unsigned long)page->mapping &
				PAGE_MAPPING_MOVABLE);
}
EXPORT_SYMBOL(__ClearPageMovable);

/* Do not skip compaction more than 64 times */
#define COMPACT_MAX_DEFER_SHIFT 6

/*
 * Compaction is deferred when compaction fails to result in a page
 * allocation success. 1 << compact_defer_limit compactions are skipped up
 * to a limit of 1 << COMPACT_MAX_DEFER_SHIFT
 */
// order 요청의 page 할당이 실패한 경우, compaction 이 향후 일정 횟수 
// skip 될 수 있도록 설정
void defer_compaction(struct zone *zone, int order)
{
	zone->compact_considered = 0;
	zone->compact_defer_shift++;
    // skip 할 compaction 의 최대값 증가
	if (order < zone->compact_order_failed)
		zone->compact_order_failed = order;
        // compact_order_failed 에 설정된 값보다 저 작은 order 요청인데 
        // 실패한 경우, 추후 order 이상의 요청에 대해 skip 해 줄 수 있도록
        // compact_order_failed 를 재설정
	if (zone->compact_defer_shift > COMPACT_MAX_DEFER_SHIFT)
		zone->compact_defer_shift = COMPACT_MAX_DEFER_SHIFT;
        // 최대 skip 가능한 값보다 넘어갔을 경우, 최대값으로 설정
	trace_mm_compaction_defer_compaction(zone, order);
}

/* Returns true if compaction should be skipped this time */
// 현재 zone 에서 compaction 을 skip 해주어야 하는지 검사
bool compaction_deferred(struct zone *zone, int order)
{
	unsigned long defer_limit = 1UL << zone->compact_defer_shift;
    // compaction 을 skip 해줄 수 있는 최대 제한값
	if (order < zone->compact_order_failed)
		return false;
        // 현재 요청 order 가 compact_order_failed 보다 작을 경우, 
        // compaction 을 진행 해 주어도 됨
	/* Avoid possible overflow */
	if (++zone->compact_considered > defer_limit)
		zone->compact_considered = defer_limit;
        // 현재 compactino skip 시도인 compact_considered 를 증가 시키고, 
        // 최대 제한 값인 defer_limit 보다 클 경우, overflow 막기 위해 
        // 최대 값으로 설정. 
	if (zone->compact_considered >= defer_limit)
		return false;
        // 최대 skip 가능 compaction 수를 넘을 경우, 
        // compaction 을 진행해 주어야 함

	trace_mm_compaction_deferred(zone, order);

	return true;
}

/*
 * Update defer tracking counters after successful compaction of given order,
 * which means an allocation either succeeded (alloc_success == true) or is
 * expected to succeed.
 */
// compaction 을 skip 하기 위한 count tracking 을 쵝화 해주고
// failed order 증가 등 수행
void compaction_defer_reset(struct zone *zone, int order,
		bool alloc_success)
{
	if (alloc_success) {
		zone->compact_considered = 0;
        // last failure 이 후 시도된  compaction 횟수를 0 으로 다시 초기화
		zone->compact_defer_shift = 0;
        // skip 할 최대 compaction 의 수 초기화         
	}
	if (order >= zone->compact_order_failed)
		zone->compact_order_failed = order + 1;
        // compact_order_failed 설정되었을 시, 
        // compact_order_failed 증가해 주어, 
        // skip 되지 않고 compaction 될 order 의 쿠기 증가해줌. 
        // (요청 order < compact_order_failed 이면 compaction 진행)
	trace_mm_compaction_defer_reset(zone, order);
}

/* Returns true if restarting compaction after many failures */
// compaction 을 재시작해야 하는지 검사
bool compaction_restarting(struct zone *zone, int order)
{
	if (order < zone->compact_order_failed)
		return false;
        // 현재 요청 order 가 zone 의 compact_order_failed 보다 작다면 
        // cache 된 page block 정보들 최기화 및 scanner 위치 초기화 없이 
        // 그냥 하던위치에서 수행
	return zone->compact_defer_shift == COMPACT_MAX_DEFER_SHIFT &&
		zone->compact_considered >= 1UL << zone->compact_defer_shift;
    // zone 을 다 검사했는데 확보하지 못해 compaction 이 defer 될 때 마다, 
    // 증가하는 defer 최대 가능 횟수가 COMPACT_MAX_DEFER_SHIFT 까지 도달했으며
    // skip 된compaction 의 수 인 compact_considered  가 최대에 도달한 경우 
    // cache 정보 및 scanner 위치 초기화
}

/* Returns true if the pageblock should be scanned for pages to isolate. */
// page 가 속한 pageblock 에 대한 bitmap entry 에 PB_migrate_skip 가 설정되어 
// 현재 page block 에 대한 scanning 을 건너뛰어도 되는지 검사
static inline bool isolation_suitable(struct compact_control *cc,
					struct page *page)
{
	if (cc->ignore_skip_hint)
		return true;
    // ignore_skip_hint 가 설정되어 있을 경우, 여기 해당 page block 은 
    // scanning 과정에서 무시하라는 hit bit 를 무시하고 검사

	return !get_pageblock_skip(page);
    // page 가 속한 page block 에 해당되는 bitmap entry 에 
    // PB_migrate_skip bit 가 설정되어 있는지 있는지 검사
}

// zone 내의 compactino 관련 cache 정보 초기화
static void reset_cached_positions(struct zone *zone)
{
	zone->compact_cached_migrate_pfn[0] = zone->zone_start_pfn; 
    // asynchronous migrate scanner cache 초기화
	zone->compact_cached_migrate_pfn[1] = zone->zone_start_pfn;
    // synchronous migrate scanner cache 초기화
	zone->compact_cached_free_pfn =
				pageblock_start_pfn(zone_end_pfn(zone) - 1);
    // free scanner 초기화 
    // zone 내의 마지막 page block 으로 설정
}

/*
 * This function is called to clear all cached information on pageblocks that
 * should be skipped for page isolation when the migrate and free page scanner
 * meet.
 */ 
// page block 에 대한 정보를 기록했던 정보들을 초기화 한다. 
//  - page block bitmap skipbit clear 
//  - scanner 위치 정보 초기화
static void __reset_isolation_suitable(struct zone *zone)
{
	unsigned long start_pfn = zone->zone_start_pfn;
	unsigned long end_pfn = zone_end_pfn(zone);
	unsigned long pfn;

	zone->compact_blockskip_flush = false;
    // page block skip 정보 초기화 할것이므로 false 로 다시 설정
	/* Walk the zone and mark every pageblock as suitable for isolation */
	for (pfn = start_pfn; pfn < end_pfn; pfn += pageblock_nr_pages) {
        // zone 의 첫 pfn 부터 끝 pfn 까지 순회하며 
		struct page *page;

		cond_resched();

		if (!pfn_valid(pfn))
			continue;

		page = pfn_to_page(pfn);
		if (zone != page_zone(page))
			continue;

		clear_pageblock_skip(page);
        // zone 의page block 관리하는 pageblock_flags 의 
        // struct page 관련 PB_migrate_skip 를 모두 clear 해줌
	}

	reset_cached_positions(zone);
    // migrate, free scanner 의 cache 된 위치 정보 초기화
}
// 
// kcompactd 가 scan 하며 설정한 page block 에 대한 hint bit 들을 모두 clear
void reset_isolation_suitable(pg_data_t *pgdat)
{
	int zoneid;

	for (zoneid = 0; zoneid < MAX_NR_ZONES; zoneid++) {
		struct zone *zone = &pgdat->node_zones[zoneid];
		if (!populated_zone(zone))
			continue;

		/* Only flush if a full compaction finished recently */
		if (zone->compact_blockskip_flush)
			__reset_isolation_suitable(zone);
	}
}

/*
 * If no pages were isolated then mark this pageblock to be skipped in the
 * future. The information is later cleared by __reset_isolation_suitable().
 */
static void update_pageblock_skip(struct compact_control *cc,
			struct page *page, unsigned long nr_isolated,
			bool migrate_scanner)
{
	struct zone *zone = cc->zone;
	unsigned long pfn;

	if (cc->ignore_skip_hint)
		return;
    // skip bit 있어도 검사하도록 되어 있다면 skip bit 설정할 필요가 없으므로 종료
    
	if (!page)
		return;
    // page block 을 찾기 위한 struct page 없다면 종료
	if (nr_isolated)
		return;
    // isolate 된 page 가 있다면 skip bit 설정 안함
	set_pageblock_skip(page);
    // skip bit 설정
	pfn = page_to_pfn(page);

	/* Update where async and sync compaction should restart */
    // scanner cache 를 update
    if (migrate_scanner) {
		if (pfn > zone->compact_cached_migrate_pfn[0])
			zone->compact_cached_migrate_pfn[0] = pfn;
		if (cc->mode != MIGRATE_ASYNC &&
		    pfn > zone->compact_cached_migrate_pfn[1])
			zone->compact_cached_migrate_pfn[1] = pfn;
	} else {
		if (pfn < zone->compact_cached_free_pfn)
			zone->compact_cached_free_pfn = pfn;
	}
}
#else
static inline bool isolation_suitable(struct compact_control *cc,
					struct page *page)
{
	return true;
}

static void update_pageblock_skip(struct compact_control *cc,
			struct page *page, unsigned long nr_isolated,
			bool migrate_scanner)
{
}
#endif /* CONFIG_COMPACTION */

/*
 * Compaction requires the taking of some coarse locks that are potentially
 * very heavily contended. For async compaction, back out if the lock cannot
 * be taken immediately. For sync compaction, spin on the lock if needed.
 *
 * Returns true if the lock is held
 * Returns false if the lock is not held and compaction should abort
 */ 
// 
// async mode compaction 일 경우, lock 을 얻을 수 없다면 바로 종료
static bool compact_trylock_irqsave(spinlock_t *lock, unsigned long *flags,
						struct compact_control *cc)
{
	if (cc->mode == MIGRATE_ASYNC) {
		if (!spin_trylock_irqsave(lock, *flags)) {
			cc->contended = true;
			return false;
		}
	} else {
		spin_lock_irqsave(lock, *flags);
	}

	return true;
}

/*
 * Compaction requires the taking of some coarse locks that are potentially
 * very heavily contended. The lock should be periodically unlocked to avoid
 * having disabled IRQs for a long time, even when there is nobody waiting on
 * the lock. It might also be that allowing the IRQs will result in
 * need_resched() becoming true. If scheduling is needed, async compaction
 * aborts. Sync compaction schedules.
 * Either compaction type will also abort if a fatal signal is pending.
 * In either case if the lock was locked, it is dropped and not regained.
 *
 * Returns true if compaction should abort due to fatal signal pending, or
 *		async compaction due to need_resched()
 * Returns false when compaction can continue (sync compaction might have
 *		scheduled)
 */ 
// lock 잡혔다면 풀고, compaction 중지 여부검사
static bool compact_unlock_should_abort(spinlock_t *lock,
		unsigned long flags, bool *locked, struct compact_control *cc)
{
	if (*locked) {
        // lock 잡힌 경우
		spin_unlock_irqrestore(lock, flags);
        // lock 풀어주고
		*locked = false;
	}

    // compaction 및 scanning 중지해야 되는지 검사
	if (fatal_signal_pending(current)) {
		cc->contended = true;
		return true;
	}

	if (need_resched()) {
		if (cc->mode == MIGRATE_ASYNC) {
			cc->contended = true;
			return true;
		}
		cond_resched();
	}

	return false;
}

/*
 * Aside from avoiding lock contention, compaction also periodically checks
 * need_resched() and either schedules in sync compaction or aborts async
 * compaction. This is similar to what compact_unlock_should_abort() does, but
 * is used where no lock is concerned.
 *
 * Returns false when no scheduling was needed, or sync compaction scheduled.
 * Returns true when async compaction should abort.
 */
// scanning 시에 너무 많은 memory 영역에 대해 scanning 하면 안되므로 
// 중단되기 위한 조건중 하나로 더 높은 task 또는 
static inline bool compact_should_abort(struct compact_control *cc)
{
	/* async compaction aborts if contended */
	if (need_resched()) {
        // reschedule 요청이 있다면..
		if (cc->mode == MIGRATE_ASYNC) {
            // asynchronous mode 였다면 
			cc->contended = true;
			return true;
            // scanning 중단(64MB align 영역까지 도달한 경우)
		}

		cond_resched();
        // synchronous mode 였다면 다른 task 호출
	}

	return false;
}

/*
 * Isolate free pages onto a private freelist. If @strict is true, will abort
 * returning 0 on any invalid PFNs or non-free pages inside of the pageblock
 * (even though it may still end up isolating some pages).
 */
static unsigned long isolate_freepages_block(struct compact_control *cc,
				unsigned long *start_pfn,
				unsigned long end_pfn,
				struct list_head *freelist,
				bool strict)
{
	int nr_scanned = 0, total_isolated = 0;
	struct page *cursor, *valid_page = NULL;
	unsigned long flags = 0;
	bool locked = false;
	unsigned long blockpfn = *start_pfn;
	unsigned int order;

	cursor = pfn_to_page(blockpfn);
    // 검사할 single page block 의 첫번째 base page 의 struct page
	/* Isolate free pages. */ 
	for (; blockpfn < end_pfn; blockpfn++, cursor++) {
        // single page block 내를 순회
		int isolated;
		struct page *page = cursor;

		/*
		 * Periodically drop the lock (if held) regardless of its
		 * contention, to give chance to IRQs. Abort if fatal signal
		 * pending or async compaction detects need_resched()
		 */
		if (!(blockpfn % SWAP_CLUSTER_MAX)
		    && compact_unlock_should_abort(&cc->zone->lock, flags,
								&locked, cc))
			break;
            // scan 중인 위치가 SWAP_CLUSTER_MAX align 위치의 pfn 에 
            // 도달 할 때마다 scanning 중지해야 되는지 여부 검사

		nr_scanned++;
		if (!pfn_valid_within(blockpfn))
			goto isolate_fail;
            // hole 이 아닌지 등 유효 page 검사

		if (!valid_page)
			valid_page = page;

		/*
		 * For compound pages such as THP and hugetlbfs, we can save
		 * potentially a lot of iterations if we skip them at once.
		 * The check is racy, but we can consider only valid values
		 * and the only danger is skipping too much.
		 */
		if (PageCompound(page)) {
            // compound page 인 경우
			unsigned int comp_order = compound_order(page);
            // 첫번째 tail page 로부터 compound page 의 크기를 읽어옴 
			if (likely(comp_order < MAX_ORDER)) {
				blockpfn += (1UL << comp_order) - 1;
				cursor += (1UL << comp_order) - 1;
			}
            // compound page 의 크기만큼 검사할 page 를 건너 뜀 
            // (struct page 와 pfn 모두 이동)

			goto isolate_fail;
            // THP 일 경우 page block 하나가 통채로 pass 될수도 있으므로 
            // isolate_fail 로 이동 
		}

		if (!PageBuddy(page))
			goto isolate_fail;
            // buddy 에 있는 free page 가 아닐 경우 pass
		/*
		 * If we already hold the lock, we can skip some rechecking.
		 * Note that if we hold the lock now, checked_pageblock was
		 * already set in some previous iteration (or strict is true),
		 * so it is correct to skip the suitable migration target
		 * recheck as well.
		 */
		if (!locked) {
			/*
			 * The zone lock must be held to isolate freepages.
			 * Unfortunately this is a very coarse lock and can be
			 * heavily contended if there are parallel allocations
			 * or parallel compactions. For async compaction do not
			 * spin on the lock and we acquire the lock as late as
			 * possible.
			 */
			locked = compact_trylock_irqsave(&cc->zone->lock,
								&flags, cc);
			if (!locked)
				break;
                // lock이 false 라면 async mode 에서 lock  을 
                // 얻을 수 없는 상황이므로 현재 page block scanning 종료

			/* Recheck this is a buddy page under lock */
			if (!PageBuddy(page))
				goto isolate_fail; 
            // lock 잡은 상태에서 다시한번 buddy page 인지 검사
		}

		/* Found a free page, will break it into order-0 pages */
		order = page_order(page);
        // buddy 의 free page 이므로 private 필드에서 order 정보 얻어옴
		isolated = __isolate_free_page(page, order);
        // order 크기의 page 를 buddy 로부터 빼내고, 그 free page 의 
        // free base page 크기 반환
        
		if (!isolated)
			break;
            // isolated 가 0 이면 watermark 보다 free page 가 작아 
            // isolate 못하는 경우이므로 현재 page block scanning 종료  
		set_page_private(page, order);
        // page 에 order 정보는 다시 설정
		total_isolated += isolated;
        // 전체 isolated 한 free page 정보 수정
		cc->nr_freepages += isolated;
        // 현재 scanning 에 isolate 한 free page 정보 수정
		list_add_tail(&page->lru, freelist);
        // compact_control 의 freelist 에 추가
		if (!strict && cc->nr_migratepages <= cc->nr_freepages) {
			blockpfn += isolated;
			break;
		}
		/* Advance to the end of split page */
		blockpfn += isolated - 1;
		cursor += isolated - 1;
		continue;

isolate_fail:
		if (strict)
			break;
		else
			continue;

	}

	if (locked)
		spin_unlock_irqrestore(&cc->zone->lock, flags);

	/*
	 * There is a tiny chance that we have read bogus compound_order(),
	 * so be careful to not go outside of the pageblock.
	 */
	if (unlikely(blockpfn > end_pfn))
		blockpfn = end_pfn;

	trace_mm_compaction_isolate_freepages(*start_pfn, blockpfn,
					nr_scanned, total_isolated);

	/* Record how far we have got within the block */
	*start_pfn = blockpfn;
    // single page block 내에서 어디까지 검사하였는지 update 

	/*
	 * If strict isolation is requested by CMA then check that all the
	 * pages requested were isolated. If there were any failures, 0 is
	 * returned and CMA will fail.
	 */
	if (strict && blockpfn < end_pfn)
		total_isolated = 0;

	/* Update the pageblock-skip if the whole pageblock was scanned */
	if (blockpfn == end_pfn)
		update_pageblock_skip(cc, valid_page, total_isolated, false);
        // isolated 된 free page 가 없다면 skip bit 설정
	cc->total_free_scanned += nr_scanned;
	if (total_isolated)
		count_compact_events(COMPACTISOLATED, total_isolated);
	return total_isolated;
}

/**
 * isolate_freepages_range() - isolate free pages.
 * @start_pfn: The first PFN to start isolating.
 * @end_pfn:   The one-past-last PFN.
 *
 * Non-free pages, invalid PFNs, or zone boundaries within the
 * [start_pfn, end_pfn) range are considered errors, cause function to
 * undo its actions and return zero.
 *
 * Otherwise, function returns one-past-the-last PFN of isolated page
 * (which may be greater then end_pfn if end fell in a middle of
 * a free page).
 */
unsigned long
isolate_freepages_range(struct compact_control *cc,
			unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long isolated, pfn, block_start_pfn, block_end_pfn;
	LIST_HEAD(freelist);

	pfn = start_pfn;
	block_start_pfn = pageblock_start_pfn(pfn);
	if (block_start_pfn < cc->zone->zone_start_pfn)
		block_start_pfn = cc->zone->zone_start_pfn;
	block_end_pfn = pageblock_end_pfn(pfn);

	for (; pfn < end_pfn; pfn += isolated,
				block_start_pfn = block_end_pfn,
				block_end_pfn += pageblock_nr_pages) {
		/* Protect pfn from changing by isolate_freepages_block */
		unsigned long isolate_start_pfn = pfn;

		block_end_pfn = min(block_end_pfn, end_pfn);

		/*
		 * pfn could pass the block_end_pfn if isolated freepage
		 * is more than pageblock order. In this case, we adjust
		 * scanning range to right one.
		 */
		if (pfn >= block_end_pfn) {
			block_start_pfn = pageblock_start_pfn(pfn);
			block_end_pfn = pageblock_end_pfn(pfn);
			block_end_pfn = min(block_end_pfn, end_pfn);
		}

		if (!pageblock_pfn_to_page(block_start_pfn,
					block_end_pfn, cc->zone))
			break;

		isolated = isolate_freepages_block(cc, &isolate_start_pfn,
						block_end_pfn, &freelist, true);

		/*
		 * In strict mode, isolate_freepages_block() returns 0 if
		 * there are any holes in the block (ie. invalid PFNs or
		 * non-free pages).
		 */
		if (!isolated)
			break;

		/*
		 * If we managed to isolate pages, it is always (1 << n) *
		 * pageblock_nr_pages for some non-negative n.  (Max order
		 * page may span two pageblocks).
		 */
	}

	/* __isolate_free_page() does not map the pages */
	map_pages(&freelist);

	if (pfn < end_pfn) {
		/* Loop terminated early, cleanup. */
		release_freepages(&freelist);
		return 0;
	}

	/* We don't use freelists for anything. */
	return pfn;
}

/* Similar to reclaim, but different enough that they don't share logic */
// isolated page 가 전체 active page, inavtive page 를 합한것의 반이상이면
// 너무 많이 isolate 한 것
static bool too_many_isolated(struct zone *zone)
{
	unsigned long active, inactive, isolated;

	inactive = node_page_state(zone->zone_pgdat, NR_INACTIVE_FILE) +
			node_page_state(zone->zone_pgdat, NR_INACTIVE_ANON);
	active = node_page_state(zone->zone_pgdat, NR_ACTIVE_FILE) +
			node_page_state(zone->zone_pgdat, NR_ACTIVE_ANON);
	isolated = node_page_state(zone->zone_pgdat, NR_ISOLATED_FILE) +
			node_page_state(zone->zone_pgdat, NR_ISOLATED_ANON);

	return isolated > (inactive + active) / 2;
}

/**
 * isolate_migratepages_block() - isolate all migrate-able pages within
 *				  a single pageblock
 * @cc:		Compaction control structure.
 * @low_pfn:	The first PFN to isolate
 * @end_pfn:	The one-past-the-last PFN to isolate, within same pageblock
 * @isolate_mode: Isolation mode to be used.
 *
 * Isolate all pages that can be migrated from the range specified by
 * [low_pfn, end_pfn). The range is expected to be within same pageblock.
 * Returns zero if there is a fatal signal pending, otherwise PFN of the
 * first page that was not scanned (which may be both less, equal to or more
 * than end_pfn).
 *
 * The pages are isolated on cc->migratepages list (not required to be empty),
 * and cc->nr_migratepages is updated accordingly. The cc->migrate_pfn field
 * is neither read nor updated.
 */ 
// 현재 low_pfn 부터 end_pfn 까지의 single page block 에 존재하는 512 개의 
// base page 들 중 isolate  가능한 base page 들에 대해 isolate 수행
static unsigned long
isolate_migratepages_block(struct compact_control *cc, unsigned long low_pfn,
			unsigned long end_pfn, isolate_mode_t isolate_mode)
{
	struct zone *zone = cc->zone;
	unsigned long nr_scanned = 0, nr_isolated = 0;
	struct lruvec *lruvec;
	unsigned long flags = 0;
	bool locked = false;
    // node 의 LRU lock 이 잡혀 있는지 여부
	struct page *page = NULL, *valid_page = NULL;
	unsigned long start_pfn = low_pfn;
	bool skip_on_failure = false;
	unsigned long next_skip_pfn = 0;

	/*
	 * Ensure that there are not too many pages isolated from the LRU
	 * list by either parallel reclaimers or compaction. If there are,
	 * delay for some time until fewer pages are isolated
	 */
    // 현재 low_pfn ~ end_pfn 의 single page block 에 대해 isolate 해주기 전 
    // 지금까지 너무 많이 isolate 되었는지 확인
	while (unlikely(too_many_isolated(zone))) {
    // 너무 많이 isolate 하였다면 ...
    // (isolated page >= (active page + inactive page)/2 )
		/* async migration should just abort */
		if (cc->mode == MIGRATE_ASYNC)
			return 0;
            // asynchronous compaction 인데 너무 많이 isolate 된 것이라면 

		congestion_wait(BLK_RW_ASYNC, HZ/10);
        // 일정 시간 sleep  
		if (fatal_signal_pending(current))
			return 0;
            // async 든 sync 든 pending signal 이 있는 경우는 종료
	}

    
	if (compact_should_abort(cc))
		return 0;
        // isolate 작업을 시작하기 앞서 isolate 중단해야하는지 검사 
        //  - async 이며 다른 task 수행되어야 하면 종료 
        //  - sync 이며 다른 task 수행되어야 하면 reschedule

	if (cc->direct_compaction && (cc->mode == MIGRATE_ASYNC)) {
		skip_on_failure = true;
		next_skip_pfn = block_end_pfn(low_pfn, cc->order);
	}
    // FIXME direct_compaction 이면서 MIGRATE_ASYNC ?

	/* Time to isolate some pages for migration */
	for (; low_pfn < end_pfn; low_pfn++) {
        // low_pfn 부터 end_pfn 까지의 page frame 순회 

		if (skip_on_failure && low_pfn >= next_skip_pfn) {
			/*
			 * We have isolated all migration candidates in the
			 * previous order-aligned block, and did not skip it due
			 * to failure. We should migrate the pages now and
			 * hopefully succeed compaction.
			 */
			if (nr_isolated)
				break;

			/*
			 * We failed to isolate in the previous order-aligned
			 * block. Set the new boundary to the end of the
			 * current block. Note we can't simply increase
			 * next_skip_pfn by 1 << order, as low_pfn might have
			 * been incremented by a higher number due to skipping
			 * a compound or a high-order buddy page in the
			 * previous loop iteration.
			 */
			next_skip_pfn = block_end_pfn(low_pfn, cc->order);
		}
        // FIXME - skip_on_failure, next_skip_pfn 이란?

		/*
		 * Periodically drop the lock (if held) regardless of its
		 * contention, to give chance to IRQs. Abort async compaction
		 * if contended.
		 */ 
        // SWAP_CLUSTER_MAX 만큼의 align pfn 에 도달 할 때 마다 
        // lock 잡혔다면 풀고, compaction & scanning 중단 검사
		if (!(low_pfn % SWAP_CLUSTER_MAX)
		    && compact_unlock_should_abort(zone_lru_lock(zone), flags,
								&locked, cc))
			break;
            // scannning 중단 조건 
            //  - SWAP_CLUSTER_MAX align pfn 도달 했으며 pending signal 있음 
            //  - SWAP_CLUSTER_MAX align pfn 도달 했으며 async compaction 에서 
            //    reschedule 되어야 함

        // zone 내에 hole 이 있는 경우 pfn 이 hole 의 pfn 인지 검사
		if (!pfn_valid_within(low_pfn))
			goto isolate_fail;
            // invalid pfn 일 경우
            // 다음 page frame 을 검사 또는 종료되기 위해 이동

        // scan 중단될 조건들 검사 통화했으므로 이제 isolate 시작
		nr_scanned++;
        // scan count 증가 
		page = pfn_to_page(low_pfn);
        // pfn 으로 struct page 구함
		if (!valid_page)
			valid_page = page;
        // 추후 필요시, page block 에 skip bit 를 설정 할 수 있도록 
        // page block 의 첫번재 page 의 struct page 가지고 있음 
		/*
		 * Skip if free. We read page order here without zone lock
		 * which is generally unsafe, but the race window is small and
		 * the worst thing that can happen is that we skip some
		 * potential isolation targets.
		 */
		if (PageBuddy(page)) {
            // buddy page 인 경우 
            // in-use page 가 아니므로 compaction 대상 아님 건너뛰어야함
            //
            //  - struct page 의 _mapcount 가 PAGE_BUDDY_MAPCOUNT_VALUE(-128)
            //    이라면 buddy 소속의 free page 임
			unsigned long freepage_order = page_order_unsafe(page);
            // struct page 의 private field 를 통해 page 로부터 
            // 연속된 free page 의 order 를 알아옴

			/*
			 * Without lock, we cannot be sure that what we got is
			 * a valid page order. Consider only values in the
			 * valid order range to prevent low_pfn overflow.
			 */
			if (freepage_order > 0 && freepage_order < MAX_ORDER)
				low_pfn += (1UL << freepage_order) - 1;
            // buddy page 로부터 buddy page 의 order 만큼 skip 하여
            // buddy page 만큼 건너 뛰고 page 계속 검사
			continue;
		}

		/*
		 * Regardless of being on LRU, compound pages such as THP and
		 * hugetlbfs are not to be compacted. We can potentially save
		 * a lot of iterations if we skip them at once. The check is
		 * racy, but we can consider only valid values and the only
		 * danger is skipping too much.
		 */
		if (PageCompound(page)) {
            // head page 이거나 tail page 라면 즉 compound page 인 경우
            // compound page 이므로 compaction 대상 아님 건너뛰어야 함 
            // 즉 THP, hugetlbfs 는 compaction 대상이 아님
            //
            // - flag 에 PG_head 설정되있거나 compound_head 의 0 bit 가
            //   설정되어 있다면 compound page 임
			unsigned int comp_order = compound_order(page);
            // 첫번째  tail page 에서 compound page 의 크기를 읽어옴            
			if (likely(comp_order < MAX_ORDER))
				low_pfn += (1UL << comp_order) - 1;
            // compound page 로부터 compound page 의 order 만큼 skip 하여
            // compound page 만큼 건너 뛰고 THP, hugetlbfs 는 page 가 page block 
            // 크기이므로 이미 끝났을 수 있으므로 isolate_fail 로 이동
			goto isolate_fail;
		}

		/*
		 * Check may be lockless but that's ok as we recheck later.
		 * It's possible to migrate LRU and non-lru movable pages.
		 * Skip any other type of page
		 */
		if (!PageLRU(page)) {
            // driver 에서의 zsmalloc, virtio-balloon page 등의 non-lru page 에 
            // 속하는 page 라면...
			/*
			 * __PageMovable can return false positive so we need
			 * to verify it under page_lock.
			 */
			if (unlikely(__PageMovable(page)) &&
					!PageIsolated(page)) {
                // MOVABLE page 이면서.. (mapping 의 1-bit 설정)
                // Isolate 된것도 아니라면.. (PG_isolated 가 없음)
				if (locked) {
					spin_unlock_irqrestore(zone_lru_lock(zone),
									flags);
					locked = false;
				}
                // MOVABLE 이고, 이미 LRU 에도 없는데(PG_lru 없음)                
                // Isolated 되어 있지도 않음 
                // -> Isolate 해주어야 함 
				if (!isolate_movable_page(page, isolate_mode))
					goto isolate_success; 
                    // page 에 PG_isolated 설정 성공하면 isolate_success 로 이동 
			}

			goto isolate_fail;
		}

        // 여기부턴 이제 일반적인 lru page 들에 대한 isolation 수행 과정
		/*
		 * Migration will fail if an anonymous page is pinned in memory,
		 * so avoid taking lru_lock and isolating it unnecessarily in an
		 * admittedly racy check.
		 */
		if (!page_mapping(page) &&
		    page_count(page) > page_mapcount(page))
			goto isolate_fail;
            // anonymous page 이면서 struct page 의 
            // reference count(_refcount) 가 mapped count(_mapcount) 보다 큰 경우 
            // 
            // FIXME - reference count 가 mapcount 보다 큰 경우가 왜 isolate faile 인가?

		/*
		 * Only allow to migrate anonymous pages in GFP_NOFS context
		 * because those do not depend on fs locks.
		 */
		if (!(cc->gfp_mask & __GFP_FS) && page_mapping(page))
			goto isolate_fail;
            // __GFP_FS 설정 안되어 있고, file-backed page 면 
            // 즉 VFS 연산 허용 file-backed page 라면 isolate_fail

		/* If we already hold the lock, we can skip some rechecking */
		if (!locked) {
			locked = compact_trylock_irqsave(zone_lru_lock(zone),
								&flags, cc);
            // lru lock 바로 못잡으면 async 에서는 바로 종료
			if (!locked)
				break;

			/* Recheck PageLRU and PageCompound under lock */
			if (!PageLRU(page))
				goto isolate_fail;
                // non lru page 처리 했으므로 non-lru page 라면 종료

			/*
			 * Page become compound since the non-locked check,
			 * and it's on LRU. It can only be a THP so the order
			 * is safe to read and it's 0 for tail pages.
			 */
            // compound page 다시 한번 확인하여 order 크기만큼 skip
			if (unlikely(PageCompound(page))) {
				low_pfn += (1UL << compound_order(page)) - 1;
				goto isolate_fail;
			}
		}

		lruvec = mem_cgroup_page_lruvec(page, zone->zone_pgdat);

		/* Try isolate the page */ 

        // lru page 에 대하여 page isolation 수행
		if (__isolate_lru_page(page, isolate_mode) != 0)
			goto isolate_fail;

		VM_BUG_ON_PAGE(PageCompound(page), page);

		/* Successfully isolated */
		del_page_from_lru_list(page, lruvec, page_lru(page));
        // page 가 속해있던 lruvec 에서 제거해 준다.
		inc_node_page_state(page,
				NR_ISOLATED_ANON + page_is_file_cache(page));

isolate_success:
		list_add(&page->lru, &cc->migratepages);
        // PG_isolated 설정한 page 를 lru 에서 
        // migratepages 라는 list 에 추가
		cc->nr_migratepages++;
        // isolate 한 
		nr_isolated++;

#ifdef CONFIG_SON  
        page->son_compact_target=1;
#endif

		/*
		 * Record where we could have freed pages by migration and not
		 * yet flushed them to buddy allocator.
		 * - this is the lowest page that was isolated and likely be
		 * then freed by migration.
		 */
		if (!cc->last_migrated_pfn)
			cc->last_migrated_pfn = low_pfn;
            // 최근 migrate 된 pfn cache 정보 update
		/* Avoid isolating too much */
		if (cc->nr_migratepages == COMPACT_CLUSTER_MAX) {
            // page block 내에 isolate 된 page 의 수가 32 개라면 
            // 너무 많이 isolate 하고 한번에 옮기지 않기 위해 
            // migratino 수행
			++low_pfn;
			break;
		}

		continue;
isolate_fail:
		if (!skip_on_failure)
			continue;

		/*
		 * We have isolated some pages, but then failed. Release them
		 * instead of migrating, as we cannot form the cc->order buddy
		 * page anyway.
		 */
        // isolate 실패한 경우
		if (nr_isolated) {
            // 이미 몇개 isolate 한 상태라면..
			if (locked) {
				spin_unlock_irqrestore(zone_lru_lock(zone), flags);
				locked = false;
			}
			putback_movable_pages(&cc->migratepages);
            // isolate 하였던 page 들을 원래 lru 로 돌려놓음 
			cc->nr_migratepages = 0;
			cc->last_migrated_pfn = 0;
			nr_isolated = 0;
		}

		if (low_pfn < next_skip_pfn) {
			low_pfn = next_skip_pfn - 1;
			/*
			 * The check near the loop beginning would have updated
			 * next_skip_pfn too, but this is a bit simpler.
			 */
			next_skip_pfn += 1UL << cc->order;
            // FIXME - next_skip_pfn 이란?            
		}
	}

	/*
	 * The PageBuddy() check could have potentially brought us outside
	 * the range to be scanned.
	 */
	if (unlikely(low_pfn > end_pfn))
		low_pfn = end_pfn;

	if (locked)
		spin_unlock_irqrestore(zone_lru_lock(zone), flags);

	/*
	 * Update the pageblock-skip information and cached scanner pfn,
	 * if the whole pageblock was scanned without isolating any page.
	 */
    // 전체 page block 을 scan 하였을 경우 base page 하나도 
    // isolate 하지 못하였다면 skip bit 설정
	if (low_pfn == end_pfn)
		update_pageblock_skip(cc, valid_page, nr_isolated, true);
         
	trace_mm_compaction_isolate_migratepages(start_pfn, low_pfn,
						nr_scanned, nr_isolated);

	cc->total_migrate_scanned += nr_scanned;
	if (nr_isolated)
		count_compact_events(COMPACTISOLATED, nr_isolated);

	return low_pfn;
}

/**
 * isolate_migratepages_range() - isolate migrate-able pages in a PFN range
 * @cc:        Compaction control structure.
 * @start_pfn: The first PFN to start isolating.
 * @end_pfn:   The one-past-last PFN.
 *
 * Returns zero if isolation fails fatally due to e.g. pending signal.
 * Otherwise, function returns one-past-the-last PFN of isolated page
 * (which may be greater than end_pfn if end fell in a middle of a THP page).
 */
unsigned long
isolate_migratepages_range(struct compact_control *cc, unsigned long start_pfn,
							unsigned long end_pfn)
{
	unsigned long pfn, block_start_pfn, block_end_pfn;

	/* Scan block by block. First and last block may be incomplete */
	pfn = start_pfn;
	block_start_pfn = pageblock_start_pfn(pfn);
	if (block_start_pfn < cc->zone->zone_start_pfn)
		block_start_pfn = cc->zone->zone_start_pfn;
	block_end_pfn = pageblock_end_pfn(pfn);

	for (; pfn < end_pfn; pfn = block_end_pfn,
				block_start_pfn = block_end_pfn,
				block_end_pfn += pageblock_nr_pages) {

		block_end_pfn = min(block_end_pfn, end_pfn);

		if (!pageblock_pfn_to_page(block_start_pfn,
					block_end_pfn, cc->zone))
			continue;

		pfn = isolate_migratepages_block(cc, pfn, block_end_pfn,
							ISOLATE_UNEVICTABLE);

		if (!pfn)
			break;

		if (cc->nr_migratepages == COMPACT_CLUSTER_MAX)
			break;
	}

	return pfn;
}

#endif /* CONFIG_COMPACTION || CONFIG_CMA */
#ifdef CONFIG_COMPACTION

/* Returns true if the page is within a block suitable for migration to */
static bool suitable_migration_target(struct compact_control *cc,
							struct page *page)
{
	if (cc->ignore_block_suitable)
		return true;

	/* If the page is a large free page, then disallow migration */
	if (PageBuddy(page)) {
        // PG_buddy 설정되어 있는 buddy 가 관리하는 
        // free page  인 경우,
		/*
		 * We are checking page_order without zone->lock taken. But
		 * the only small danger is that we skip a potentially suitable
		 * pageblock, so it's not worth to check order for valid range.
		 */
        // buddy 의 private 에 있는 order 정보를 읽어와 
        // 연속적인 free page 의 크기가 512 개 이상인지 검사       
		if (page_order_unsafe(page) >= pageblock_order)
			return false;
            // 512 개 이상이라면 여기로 migrate 해봣자 
            // 연속적 free page block 소모 이므로 선택하지 않음 
	}

	/* If the block is MIGRATE_MOVABLE or MIGRATE_CMA, allow migration */ 
    // free page 이 속한 pageblock 의 migrate type 검사
	if (migrate_async_suitable(get_pageblock_migratetype(page)))
		return true;
        // page block 이 MOVABLE 에 속한 다면 migrate page 가 옮겨질 대상 가능 

	/* Otherwise skip the block */
	return false;
}

/*
 * Test whether the free scanner has reached the same or lower pageblock than
 * the migration scanner, and compaction should thus terminate.
 */ 
// migrate scanner 와 free scanner 가 검사하던 page block index 가 만났는지 검사
// migrate scanner pfn 이 free scanner pfn 보다 커진다면 만난 것
static inline bool compact_scanners_met(struct compact_control *cc)
{
	return (cc->free_pfn >> pageblock_order)
		<= (cc->migrate_pfn >> pageblock_order);
}

/*
 * Based on information in the current compact_control, find blocks
 * suitable for isolating free pages from and then isolate them.
 */ 

// zone 에서 migrate 해줄 page 가 옮겨질 free page 들들에 대해 
// LRU 와isolate 수행 및 freepagelist 에 추가
// page block 들에 대해 하나의 page block 마다 isolate_freepages_block 수행
static void isolate_freepages(struct compact_control *cc)
{
	struct zone *zone = cc->zone;
	struct page *page;
	unsigned long block_start_pfn;	/* start of current pageblock */
	unsigned long isolate_start_pfn; /* exact pfn we start at */
	unsigned long block_end_pfn;	/* end of current pageblock */
	unsigned long low_pfn;	     /* lowest pfn scanner is able to scan */
	struct list_head *freelist = &cc->freepages;

	/*
	 * Initialise the free scanner. The starting point is where we last
	 * successfully isolated from, zone-cached value, or the end of the
	 * zone when isolating for the first time. For looping we also need
	 * this pfn aligned down to the pageblock boundary, because we do
	 * block_start_pfn -= pageblock_nr_pages in the for loop.
	 * For ending point, take care when isolating in last pageblock of a
	 * a zone which ends in the middle of a pageblock.
	 * The low boundary is the end of the pageblock the migration scanner
	 * is using.
	 */
	isolate_start_pfn = cc->free_pfn;
    // compact_control 에 cache 된 pfn 위치부터 free page scanning 시작
	block_start_pfn = pageblock_start_pfn(cc->free_pfn);
    // scanning 시작 page 가 속한 page block 의 시작 page frame
	block_end_pfn = min(block_start_pfn + pageblock_nr_pages,
						zone_end_pfn(zone));
    // 아래와 같이 page 가 속한 page block 의 끝 page frame 이 
    // zone 의 마지막 page frame 보다 클 수 있으므로 최소값으로 설정 
    //
    //                   block_start_pfn   block_end_pfn
    //                              |         |
    //  ... ----|---------+---------+----*    +
    //                                   | 
    //                             zone_end_pfn(zone)
    //
	low_pfn = pageblock_end_pfn(cc->migrate_pfn);
    // low_pfn : migrate scanner 가 동작하던 위치로 free scanner 가 
    // 넘어가면 안됨
	/*
	 * Isolate free pages until enough are available to migrate the
	 * pages on cc->migratepages. We stop searching if the migrate
	 * and free page scanners meet or enough free pages are isolated.
	 */
	for (; block_start_pfn >= low_pfn;
				block_end_pfn = block_start_pfn,
				block_start_pfn -= pageblock_nr_pages,
				isolate_start_pfn = block_start_pfn) {
		/*
		 * This can iterate a massively long zone without finding any
		 * suitable migration targets, so periodically check if we need
		 * to schedule, or even abort async compaction.
		 */

        // migrate scanner 와 마찬가지로 SWAP_CLUSTER_MAX align 영역에 
        // 도달하게 될 때마다 scanning 중단해야 되는지 검사 
		if (!(block_start_pfn % (SWAP_CLUSTER_MAX * pageblock_nr_pages))
						&& compact_should_abort(cc))
			break;
            // scanning 중단 조건 
            //  - SWAP_CLUSTER_MAX align pfn 만큼 scanner 가 도착했고 
            //    async scana 일 때, reschedule 되어야 하면 중단 

		page = pageblock_pfn_to_page(block_start_pfn, block_end_pfn,
									zone); 
        // block_start_pfn ~ block_end_pfn 의 첫 base page 에 해당하는 
        // struct page 를 가져옴 
        
		if (!page)
			continue;

		/* Check the block is suitable for migration */
		if (!suitable_migration_target(cc, page))
			continue;
            // page 가 속한 page block 이 통채로 비어있다면 다음 page block 검사
            // page block 이 MOVABLE 이 아니라면 다음 page block 검사

		/* If isolation recently failed, do not retry */ 
		if (!isolation_suitable(cc, page))
			continue;
            // skip bit 가 설정되어 있다면 다음 page block 검사

		/* Found a block suitable for isolating free pages from. */ 
        // low_pfn 부터 block_end_pfn 까지의 sigle page block 내의 
        // 512 개 page frame 에 대해 isolate_mode 로 isolate 수행
		isolate_freepages_block(cc, &isolate_start_pfn, block_end_pfn,
					freelist, false);

		/*
		 * If we isolated enough freepages, or aborted due to lock
		 * contention, terminate.
		 */
		if ((cc->nr_freepages >= cc->nr_migratepages)
							|| cc->contended) {
        // migrate page 들이 옮겨질 free page 를 충분히 확보하였거나 
        // lock contention 이 있던 경우, 더이상 page block 을 검사하지 않고 종료
			if (isolate_start_pfn >= block_end_pfn) {
            // 현재 page block 을 다 검사 하였다면 
				/*
				 * Restart at previous pageblock if more
				 * freepages can be isolated next time.
				 */
				isolate_start_pfn =
					block_start_pfn - pageblock_nr_pages;
                // 다음 page block 부터 수행 될 수 있도록 page block 위치 조정
                // 
                //                                                         isolate_start_pfn
                //                                                                |
                //                                                                *
                // ... -------+-------------------------+-------------------------+---- ...
                //                                      *  검사중이던 page block  *
                //                                      |                         |
                //                            block_start_pfn                  block_end_pfn 
                //
                // 위와 같이 검사중이던 page block 을 다 검사하여 isolate_start_pfn 이 
                // block_end_pfn 의 위치 이상일 경우, 다음에 검사될 위치 설정을 위해 update
                //
                //     isolate_start_pfn
                //            |
                //            *
                // ... -------+-------------------------+-------------------------+---- ...
                //              다음에 검사할 page blck *  검사중이던 page block  *
                //                                      |                         |
                //                            block_start_pfn                  block_end_pfn 
			}
			break;
		} else if (isolate_start_pfn < block_end_pfn) {             
			/*
			 * If isolation failed early, do not continue
			 * needlessly.
			 */
			break;
		}
	}

	/* __isolate_free_page() does not map the pages */     
	map_pages(freelist);
    // page block 에서 isolate 한 page 에 대해 split 수행 

	/*
	 * Record where the free scanner will restart next time. Either we
	 * broke from the loop and set isolate_start_pfn based on the last
	 * call to isolate_freepages_block(), or we met the migration scanner
	 * and the loop terminated due to isolate_start_pfn < low_pfn
	 */
	cc->free_pfn = isolate_start_pfn;
    // compact_control 의 어디까지 스캔한건지 정보 update
}

/*
 * This is a migrate-callback that "allocates" freepages by taking pages
 * from the isolated freelists in the block we are migrating to.
 */
//
// compaction control 의 freepages 로부터 page 를 하나 받아 반환하며 
// 기존 freepages lsit 에서 삭제해줌 
static struct page *compaction_alloc(struct page *migratepage,
					unsigned long data,
					int **result)
{
	struct compact_control *cc = (struct compact_control *)data;
	struct page *freepage;

	/*
	 * Isolate free pages if necessary, and if we are not aborting due to
	 * contention.
	 */
	if (list_empty(&cc->freepages)) {
        // freepages list 가 비어있다면
		if (!cc->contended)
			isolate_freepages(cc);
            // free scanner 가동작
        // free scanner 가동후에도 freepages list 가 
        // 비어있다면 page 가져오기 실패 및 종료
		if (list_empty(&cc->freepages))
			return NULL;
	}

	freepage = list_entry(cc->freepages.next, struct page, lru);
    // freepages list 에서 선두의 page 하나를 가져옴
	list_del(&freepage->lru);
	cc->nr_freepages--;

	return freepage;
}

/*
 * This is a migrate-callback that "frees" freepages back to the isolated
 * freelist.  All pages on the freelist are from the same zone, so there is no
 * special handling needed for NUMA.
 */ 
// struct page 를 compaction control 의 freepages list 에 추가
static void compaction_free(struct page *page, unsigned long data)
{
	struct compact_control *cc = (struct compact_control *)data;

	list_add(&page->lru, &cc->freepages);
	cc->nr_freepages++;
}

/* possible outcome of isolate_migratepages */
typedef enum {
	ISOLATE_ABORT,		/* Abort compaction now */
	ISOLATE_NONE,		/* No pages isolated, continue scanning */
	ISOLATE_SUCCESS,	/* Pages isolated, migrate */
} isolate_migrate_t;

/*
 * Allow userspace to control policy on scanning the unevictable LRU for
 * compactable pages.
 */
int sysctl_compact_unevictable_allowed __read_mostly = 1;
// /proc/sys/vm/compact_unevictable_allowed 에 1 이 write 된 경우, 
// unevictable page 들(mlocked page)에 대하여도 compaction 수행 
// default 값은 1 로 설정되어 있음



/*
 * Isolate all pages that can be migrated from the first suitable block,
 * starting at the block pointed to by the migrate scanner pfn within
 * compact_control.
 */
// 
// zone 에서 migrate 해줄 page 들에 대해 LRU 와isolate 수행 및 migratelist 에 추가
// page block 들에 대해 하나의 page block 마다 isolate_migratepages_block 수행
static isolate_migrate_t isolate_migratepages(struct zone *zone,
					struct compact_control *cc)
{
	unsigned long block_start_pfn;
	unsigned long block_end_pfn;
	unsigned long low_pfn;
	struct page *page;
	const isolate_mode_t isolate_mode =
		(sysctl_compact_unevictable_allowed ? ISOLATE_UNEVICTABLE : 0) |
		(cc->mode != MIGRATE_SYNC ? ISOLATE_ASYNC_MIGRATE : 0); 
    // migration page level 및 migratino type 설정
    // e.g. unevictable page 까지 migrate 를 허용하며 async mode 로 migrate 할 일 시... 
    //      isolate_mode :  
    //                        ----ISOLATE_UNEVICTABLE
    //                       * --------ISOLATE_ASYNC_MIGRATE
    //                      * *       
    //                     **
    //          .... 0000 1100 
	/*
	 * Start at where we last stopped, or beginning of the zone as
	 * initialized by compact_zone()
	 */
	low_pfn = cc->migrate_pfn;
    // low_pfn 을 migrate scanner 가 scan 시작할 위치로 설정
	block_start_pfn = pageblock_start_pfn(low_pfn);
    // low_pfn 이 속한 page block 내의 start pfn 

	if (block_start_pfn < zone->zone_start_pfn)
		block_start_pfn = zone->zone_start_pfn; 
        // page block 시작 위치가 zone 범위 넘어가면 재설정

	/* Only scan within a pageblock boundary */
	block_end_pfn = pageblock_end_pfn(low_pfn);
    // low_pfn 이 속한 page block 내의 end pfn

	/*
	 * Iterate over whole pageblocks until we find the first suitable.
	 * Do not cross the free scanner.
	 */
	for (; block_end_pfn <= cc->free_pfn;
			low_pfn = block_end_pfn,
			block_start_pfn = block_end_pfn,
			block_end_pfn += pageblock_nr_pages) {
        // free_pfn 까지 있는 모든 page block 들에 대해 
        // page block 단위로 scan 하며 page block 내의 base page isolate
		/*
		 * This can potentially iterate a massively long zone with
		 * many pageblocks unsuitable, so periodically check if we
		 * need to schedule, or even abort async compaction.
		 */ 

        // pfn 이 SWAP_CLUSTER_MAX 에 도달 할 때 마다,  
        // scanning 을 중지해야 되는 조건검사 수행 
		if (!(low_pfn % (SWAP_CLUSTER_MAX * pageblock_nr_pages))
						&& compact_should_abort(cc))
			break;
            // scanning 중지 조건 
            // 
            // * asynchronous mode(kcompactd) 인 경우...
            //    - page block 32 개(base page 16384 개) 즉 64MB 단위 align 영역까지 scan 될 때마다 
            //      중단조건 검사 
            //    - re-schedule 되여야 한다면 scanning 종료 
            //
            //      low_pfn
            //       |   ------- 여기까지 scan 하였는데 re-schedule 요청이 있다면
            //       |   |       scanning 중단 
            //       |   *
            //     ------------------------------------------------------
            //     |     |     |     |     |   ...    |     |     |     |
            //     ------------------------------------------------------
            //     0     64   128   192   256
            //

		page = pageblock_pfn_to_page(block_start_pfn, block_end_pfn,
									zone);
        // scan 할 page block(block_start_pfn ~ block_end_pfn 범위) 내의 
        // 첫번째 page 의 struct page 가져옴
		if (!page)
			continue;
            // 다음 page block 으로 이동

		/* If isolation recently failed, do not retry */
		if (!isolation_suitable(cc, page))
			continue;
        // 현재 page block 의 migrate skip bit 검사 후 
        // 설정되어 있다면 다음 page block 검사 수행
        // (PB_migrate_skip 는 scan  할 page block 을 줄여주기 위한 hint 로 동작 해당 
        // page block 에 대해 migration 이 취소된 적 있는 경우, 이 hint 가 page block bitmap 에 써짐)

		/*
		 * For async compaction, also only scan in MOVABLE blocks.
		 * Async compaction is optimistic to see if the minimum amount
		 * of work satisfies the allocation.
		 */
		if (cc->mode == MIGRATE_ASYNC &&
		    !migrate_async_suitable(get_pageblock_migratetype(page)))
			continue;         
        // asynchronous compaction 의 경우, scanning 하려는 page block 의 migrate type 이 
        // MIGRATE_MOVABLE 또는 MIGRATE_CMA 가 아닐 경우, 다음 page block scan 수행
        // 즉. asynchronous compaction 에서는 MOVABLE page 만 compaction 해줌

		/* Perform the isolation */ 

        // low_pfn 부터 block_end_pfn 까지의 sigle page block 내의 
        // 512 개 page frame 에 대해 isolate_mode 로 isolate 수행
		low_pfn = isolate_migratepages_block(cc, low_pfn,
						block_end_pfn, isolate_mode);
        // 현재 page block 내의 base page 들에 대해 PG_lru 설정되어 있는 
        // page 들에 PG_lru 없애고 PG_isolated 추가하고.. 
        // 
        // low_pfn 이 0 이어서 compaction 중지되어야 하는 경우
        //  - async compaction 에서 너무 많이 isolate 한 경우
        //  - async compaction 에서 pending signal 있는 경우 
        //  - sync compaction 에서 pending signal 있는 경우
        //
		if (!low_pfn || cc->contended)
			return ISOLATE_ABORT;
            // isolate 중지된 경우 또는 lock contention 으로 중지된 경우 
            // compaction 중지
		/*
		 * Either we isolated something and proceed with migration. Or
		 * we failed and compact_zone should decide if we should
		 * continue or not.
		 */
		break;
	}

	/* Record where migration scanner will be restarted. */
	cc->migrate_pfn = low_pfn;

	return cc->nr_migratepages ? ISOLATE_SUCCESS : ISOLATE_NONE;
}

/*
 * order == -1 is expected when compacting via
 * /proc/sys/vm/compact_memory
 */ 
// 
// page fault 과정이 아니라
// /proc/sys/vm/compact_memory 에 1 을 write 하여 compactino 수행하라고 시킨
// 경우,  order 가  -1 로 요청됨
static inline bool is_via_compact_memory(int order)
{
	return order == -1;
}
// compaction 의 상태를 알아온다.  
// loop 결과 아래 상태면 compaction 성공
//  - 두 migrate scanner 가 만났거나 
//  - free page 가 wmark 보다 많고 연속적 order 에 맞는 
//    연속된 free page 가 있는지 검사
static enum compact_result __compact_finished(struct zone *zone, struct compact_control *cc,
			    const int migratetype)
{
	unsigned int order;
	unsigned long watermark;

	if (cc->contended || fatal_signal_pending(current))
		return COMPACT_CONTENDED;
    // compaction 과정에서 lock 을 기다려야 되는 상황이거나 
    // 현재 task 가 처리해주어야 할 signal 을 받은 경우
    
	/* Compaction run completes if the migrate and free scanner meet */
	if (compact_scanners_met(cc)) {
        // migrate scanner 와 free scanner 가 만났다면...
        //
		/* Let the next compaction start anew. */
		reset_cached_positions(zone);
        // cache 되어 있던 migrate scanner, free scanner 의 page frame index 초기화
		/*
		 * Mark that the PG_migrate_skip information should be cleared
		 * by kswapd when it goes to sleep. kcompactd does not set the
		 * flag itself as the decision to be clear should be directly
		 * based on an allocation request.
		 */
		if (cc->direct_compaction)
			zone->compact_blockskip_flush = true;
        // kswapd 가 sleep 되기 전 PG_migrate_skip clear 및 scanner index clear 를 
        // 해 줄 수 있도록 compact_blockskip_flush 를 true 로 설정

		if (cc->whole_zone)
			return COMPACT_COMPLETE;
            // 전체 zone 영역에 대한 compaction 에서 두 scanner 가 만났지만
            // free page 확보를 하지 못함
		else
			return COMPACT_PARTIAL_SKIPPED;
            // 부분 zone 영역에 대한 compaction 에서 두 scanner 가 만났지만
            // free page 확보를 하지 못함
	}

    // 두 scanner 가 안만났다면 ...
    if (is_via_compact_memory(cc->order))
		return COMPACT_CONTINUE;
        // 두 scanner 가 안만났고, compact_memory 에 write 요청해서 
        // compaction 수행된 경우, order 만족 여부와 관계 없이 만날때까지 
        // 전체 zone 에 대해 수행되어야 하므로 계속 수행

	/* Compaction run is not finished if the watermark is not met */ 

    // proc 의 compact_memory 조작을 통하 compaction 이 아니며 
    // 아직 두 scanner 가 만나지 않은 경우, 지금까지 상황이 충분히 
    // compaction 된 것인지 확인 필요함 
    watermark = zone->watermark[cc->alloc_flags & ALLOC_WMARK_MASK];
    // free page 기준 확인하기 위한 watermark 값을 가져옴
	if (!zone_watermark_ok(zone, cc->order, watermark, cc->classzone_idx,
							cc->alloc_flags))
		return COMPACT_CONTINUE;
        // free page 가 watermark 보다 부족하다면 계속 compaction 해주어야됨

    // free page 가 wmark 보다 많은 경우 이제 그만큼 연속적 page 가 있는 것인지 검사

	/* Direct compactor: Is a suitable page free? */
	for (order = cc->order; order < MAX_ORDER; order++) {
		struct free_area *area = &zone->free_area[order];
		bool can_steal;

		/* Job done if page is free of the right migratetype */
		if (!list_empty(&area->free_list[migratetype]))
			return COMPACT_SUCCESS;
            // order 에대한 free page list 에 page 가 있다면  
            // page 할당해 줄 수 있게되었으므로 
            // scanner 안만났어도 compaction 더이상 안해도됨 완료

#ifdef CONFIG_CMA
		/* MIGRATE_MOVABLE can fallback on MIGRATE_CMA */
		if (migratetype == MIGRATE_MOVABLE &&
			!list_empty(&area->free_list[MIGRATE_CMA]))
			return COMPACT_SUCCESS;
            // MIGRATE_MOVABLE 의 경우, MIGRATE_MOVABLE 의 free page list 에 
            // page 가 없다면 MIGRATE_CMA 에서 할당받을 수 있는지 확인해 본다. 
#endif
		/*
		 * Job done if allocation would steal freepages from
		 * other migratetype buddy lists.
		 */
        // 다른 migrate type 에서도 order 의 free page 여부 확인
		if (find_suitable_fallback(area, order, migratetype,
						true, &can_steal) != -1)
			return COMPACT_SUCCESS; 
            // 다른 migrate type 에서 page 받을 수 있다면 종료

	}

	return COMPACT_NO_SUITABLE_PAGE;
    // 두 scanner 가 만나지 않았으며 order 에 대한 free page 가 
    // 모든 migrate type 에서 없는경우 compactino 계속해주어야 함
}

// compaction 의 상태를 알아온다. 
// loop 결과 아래 상태면 compaction 성공
//  - 두 migrate scanner 가 만났거나 
//  - free page 가 wmark 보다 많고 연속적 order 에 맞는 
//    연속된 free page 가 있는지 검사
static enum compact_result compact_finished(struct zone *zone,
			struct compact_control *cc,
			const int migratetype)
{
	int ret;

	ret = __compact_finished(zone, cc, migratetype);
	trace_mm_compaction_finished(zone, cc->order, ret);
	if (ret == COMPACT_NO_SUITABLE_PAGE)
		ret = COMPACT_CONTINUE;
        // COMPACT_NO_SUITABLE_PAGE 의 경우, 그냥 tracepoint 용도로
        // 계속 compaction 해주라는 말과 같은 것이므로 COMPACT_CONTINUE 로변경
	return ret;
}

/*
 * compaction_suitable: Is this suitable to run compaction on this zone now?
 * Returns
 *   COMPACT_SKIPPED  - If there are too few free pages for compaction
 *   COMPACT_SUCCESS  - If the allocation would succeed without compaction
 *   COMPACT_CONTINUE - If compaction should run now
 */
//  zone 에서 alloc_flags 형태의 order 요청을 완료하기 위한 compactino 가능 
//  여부 확인을 위해 wmark_target(현재 zone 의 free page 수) 값을 기반으로 계산
static enum compact_result __compaction_suitable(struct zone *zone, int order,
					unsigned int alloc_flags,
					int classzone_idx,
					unsigned long wmark_target)
{
	unsigned long watermark;

	if (is_via_compact_memory(order))
    // /proc/sys/vm/compact_memory 에 1 이 wirte 되어 모든 zone 영역에 대해 
    // compaction 이 하라고 요청한 경우
    // (compact_memory 에 1 write 해서 compactino 요청하면 order 가 -1 로요청)
		return COMPACT_CONTINUE;
        // 그냥 zone 상태 관련 없이 compaction 수행
	watermark = zone->watermark[alloc_flags & ALLOC_WMARK_MASK];
    // kcompactd 에 의한 호출의 경우, alloc_flags가 0 이므로 
    // watermark 는 WMARK_MIN 이 된다.
	/*
	 * If watermarks for high-order allocation are already met, there
	 * should be no need for compaction at all.
	 */
	if (zone_watermark_ok(zone, order, watermark, classzone_idx,
								alloc_flags))
		return COMPACT_SUCCESS;
        // order 의 요청에도 현재 free page 의 수가 watermark 값을 
        // 넘는다면 compaction 필요 없음
	/*
	 * Watermarks for order-0 must be met for compaction to be able to
	 * isolate free pages for migration targets. This means that the
	 * watermark and alloc_flags have to match, or be more pessimistic than
	 * the check in __isolate_free_page(). We don't use the direct
	 * compactor's alloc_flags, as they are not relevant for freepage
	 * isolation. We however do use the direct compactor's classzone_idx to
	 * skip over zones where lowmem reserves would prevent allocation even
	 * if compaction succeeds.
	 * For costly orders, we require low watermark instead of min for
	 * compaction to proceed to increase its chances.
	 * ALLOC_CMA is used, as pages in CMA pageblocks are considered
	 * suitable migration targets
	 */
	watermark = (order > PAGE_ALLOC_COSTLY_ORDER) ?
				low_wmark_pages(zone) : min_wmark_pages(zone);
    // order-3 이상의 costly page 요청인 경우 watermark 를 low 값으로
    // 설정하여 필요한 free page 가 더 많아지도록 하여 추후 free page 검사 
	watermark += compact_gap(order);
    // 위에 설정된 watermark 값에 요청 page order 의 page 수 * 2 를 더함 
    // (여유분의 page 수를 더한 것)
	if (!__zone_watermark_ok(zone, 0, watermark, classzone_idx,
						ALLOC_CMA, wmark_target))
        // wmark_target (free page) 가 watermark 값보다 넘는지 검사 
        // 
        // WMARK_MIN 이 20 개 WMARK_LOW 가 40 이고, free page 수가 50 개라면
        // 8 개 page 요청일 경우...
        //  - 40 + 8*2 > 50 --> COMPACT_SKIPPED
        // 2 개 page 요청일 경우...
        //  - 20 + 8*2 < 50 --> COMPACT_CONTINUE
        //
		return COMPACT_SKIPPED;
        // wmark_target 이 watermark 보다 작으므로 free page 가 얼마 
        // 없는 것. compaction 해봣자 좋지 않을듯 싶으므로 skip 함
	return COMPACT_CONTINUE;
    // wmark_target 이 watermark 보다 크므로 compaction 하면 free page 
    // 확보 가능 할 것으로 보임 compaction  해줌
}
// compaction 수행 전 compaction 수행할만한 상태인지 검사 
enum compact_result compaction_suitable(struct zone *zone, int order,
					unsigned int alloc_flags,
					int classzone_idx)
{
	enum compact_result ret;
	int fragindex;

	ret = __compaction_suitable(zone, order, alloc_flags, classzone_idx,
				    zone_page_state(zone, NR_FREE_PAGES)); 
    // watermark 값과 비교를 통한 전체 free page 수 계산법으로 
    // compaction 할만 한지 검사
    //
    // - sysfs 를 통해 compact_memory 로 compaction 요청된 경우는
    //   compaction 검사 없이 바로 compaction 수행
    //
	/*
	 * fragmentation index determines if allocation failures are due to
	 * low memory or external fragmentation
	 *
	 * index of -1000 would imply allocations might succeed depending on
	 * watermarks, but we already failed the high-order watermark check
	 * index towards 0 implies failure is due to lack of memory
	 * index towards 1000 implies failure is due to fragmentation
	 *
	 * Only compact if a failure would be due to fragmentation. Also
	 * ignore fragindex for non-costly orders where the alternative to
	 * a successful reclaim/compaction is OOM. Fragindex and the
	 * vm.extfrag_threshold sysctl is meant as a heuristic to prevent
	 * excessive compaction for costly orders, but it should not be at the
	 * expense of system stability.
	 */
	if (ret == COMPACT_CONTINUE && (order > PAGE_ALLOC_COSTLY_ORDER)) {
        // free page 수가 좀 있어 compaction 해도 된다고 판단되어도 
        // 요청 order 가 4 order 이상이라면 단순 small order free page 가 
        // 많아서 compaction overhead 가 큰지 fragmentation index 검사
        //
        // fragindex
        //  - 0 :        page 할당 불가능 상태.(compaction 해봤자, free page 확보 불가능)
        //  - 0 ~ 1000 : 0 에 가까울 수록    
        //                 - compaction 결과 free page 확보 성공률이 낮음 
        //                 - 그냥 free page가 부족한게 원인임. 
        //               1000 에 가까울 수록 
        //                 - compaction 결과 free 확보 성공률 높음
        //                 - fragmentation이 원인임.
        //  - -1000 :    page 할당 가능 상태. (compaction 필요 없음)

		fragindex = fragmentation_index(zone, order);
		if (fragindex >= 0 && fragindex <= sysctl_extfrag_threshold)
			ret = COMPACT_NOT_SUITABLE_ZONE;      
            // fragindex 가 s0 이상이고 ysfs 에 정한 값보다 작다면
            // (/proc/sys/vm/extfrag_threshold)  
            // compactino skip 함
	}

	trace_mm_compaction_suitable(zone, order, ret);
	if (ret == COMPACT_NOT_SUITABLE_ZONE)
		ret = COMPACT_SKIPPED;

	return ret;
}

bool compaction_zonelist_suitable(struct alloc_context *ac, int order,
		int alloc_flags)
{
	struct zone *zone;
	struct zoneref *z;

	/*
	 * Make sure at least one zone would pass __compaction_suitable if we continue
	 * retrying the reclaim.
	 */
	for_each_zone_zonelist_nodemask(zone, z, ac->zonelist, ac->high_zoneidx,
					ac->nodemask) {
		unsigned long available;
		enum compact_result compact_result;

		/*
		 * Do not consider all the reclaimable memory because we do not
		 * want to trash just for a single high order allocation which
		 * is even not guaranteed to appear even if __compaction_suitable
		 * is happy about the watermark check.
		 */
		available = zone_reclaimable_pages(zone) / order;
		available += zone_page_state_snapshot(zone, NR_FREE_PAGES);
		compact_result = __compaction_suitable(zone, order, alloc_flags,
				ac_classzone_idx(ac), available);
		if (compact_result != COMPACT_SKIPPED)
			return true;
	}

	return false;
}
// free scanner, migrate scanner 가 page scan 후, page migratino 수행 
// 두 scanner 가 만나거나 order 에 대한 free_area 의 free page 가 있다면
// compactino 성공
static enum compact_result compact_zone(struct zone *zone, struct compact_control *cc)
{
	enum compact_result ret;
	unsigned long start_pfn = zone->zone_start_pfn;
	unsigned long end_pfn = zone_end_pfn(zone);
	const int migratetype = gfpflags_to_migratetype(cc->gfp_mask);
	const bool sync = cc->mode != MIGRATE_ASYNC;

	ret = compaction_suitable(zone, cc->order, cc->alloc_flags,
							cc->classzone_idx);
    // compaction 수행 전 compaction 가능 여부 검사 
    //  - free page 가 얼마없거나    
    //  - small free page 가 많아 fragment index 값이 0~500(default) 일 경우
    //  => COMPACT_SKIPPED
    //
	/* Compaction is likely to fail */
	if (ret == COMPACT_SUCCESS || ret == COMPACT_SKIPPED)
		return ret;
    // 검사 결과 COMPACT_SUCCESS 로 판단되어 compaction 안해도 되는 경우이거나 
    // COMPACT_SKIPPED 로 판단되어 compaction 하면 안되는 경우라면 종료

	/* huh, compaction_suitable is returning something unexpected */
	VM_BUG_ON(ret != COMPACT_CONTINUE);

	/*
	 * Clear pageblock skip if there were failures recently and compaction
	 * is about to be retried after being deferred.
	 */
	if (compaction_restarting(zone, cc->order))
    // compaction 을 하던데 부터가 아니라 처음부터 다시 시작해야 하는지 검사
		__reset_isolation_suitable(zone);
        // 현재 zone 에 대하여 compaction 을 처음부터 다시 수행해야 하는 경우 
        //  - zone 의 pageblock_flags 에 설정된 skip bit 를 다 clear 
        //  - scanner pfn 위치 초기화
	/*
	 * Setup to move all movable pages to the end of the zone. Used cached
	 * information on where the scanners should start (unless we explicitly
	 * want to compact the whole zone), but check that it is initialised
	 * by ensuring the values are within zone boundaries.
	 */
    // compaction 을 시작하기 전에 scanner 의 page frame index 설정
	if (cc->whole_zone) {
        // 전체 zone 에 대해 compaction 을 해주어야 한다면 zone 의 양 끝으로  
        // scanner 의 index 를 설정
        //
		cc->migrate_pfn = start_pfn;
        // migrate scanner 이 scan 시작할 위치 설정
		cc->free_pfn = pageblock_start_pfn(end_pfn - 1);
        // free scanner 이 scan 시작할 위치 설정
        // (zone 의 마지막 pfn 이 속한 page block 시작 위치)
	} else {
        // 전체 zone 에 대한 compaction 이 아닐 경우 기존에 cache 되어있던 
        // page frame index 로 scanner index 를 설정         
		cc->migrate_pfn = zone->compact_cached_migrate_pfn[sync];
        // migrate scanner 이 scan 시작할 위치 cache에서 설정
        // - 0: async cache(kcompactd), 1 : sync cache(page fault)
		cc->free_pfn = zone->compact_cached_free_pfn;
        // free scanner 이 scan 시작할 위치 cache 에서 설정 
        
        // free scanner 의 cache 정보 잘못되면 재설정
		if (cc->free_pfn < start_pfn || cc->free_pfn >= end_pfn) {
            // cache 된 free scanner index 가 zone 의 범위를 벗어나는 경우 
			cc->free_pfn = pageblock_start_pfn(end_pfn - 1);
            // free scanner index 를 zone 의 마지막 page frame 이 속한 
            // page block 의 시작 주소로 초기화 
			zone->compact_cached_free_pfn = cc->free_pfn;
            // free scanner cache 정보 재설정
		}

        // migrate scanner cache 정보 잘못되면 재설정
		if (cc->migrate_pfn < start_pfn || cc->migrate_pfn >= end_pfn) {
            // cache 된 migrate scanner index 가 zone 의 범위를 벗어나는 경우 
			cc->migrate_pfn = start_pfn;
            // migrate scanner 의 범위를 zone 의 시작 page frame 으로 재설정            
			zone->compact_cached_migrate_pfn[0] = cc->migrate_pfn;
            // migrate scanner 의 asynchronous cache 정보 재설정
			zone->compact_cached_migrate_pfn[1] = cc->migrate_pfn;
            // migrate scanner 의 synchronous cache 정보 재설정
		}

		if (cc->migrate_pfn == start_pfn)
			cc->whole_zone = true; 
        // 재설정 결과 migrate scanner 시작 page frame 이 zone 시작 
        // page frame 이면 그냥 전체 zone 하는 거임
	}

	cc->last_migrated_pfn = 0;

	trace_mm_compaction_begin(start_pfn, cc->migrate_pfn,
				cc->free_pfn, end_pfn, sync);

	migrate_prep_local();
    // per-CPU pagevec 을 active or inactive lru list 로 가져옴
    
	while ((ret = compact_finished(zone, cc, migratetype)) ==
						COMPACT_CONTINUE) {
        // compaction 를 계속 수행해야 하는  COMPACT_CONTINUE 상태인지 검사        
        //  - scanner 가 안만났고, free page 가 wmark 보다 많아 연속적 order 에 
        //    맞는 연속된 free page 가 있음
        //  - scanner 가 안만났고, free page 가 wmark 보다 적어 compaction 계속 수행 
        //  - scanner 가 만났음.
        //
		int err;

		switch (isolate_migratepages(zone, cc)) { 
        // 먼저 migrate scanner 를 동작시켜 migrate 대상 page 를 
        // lru 에서 isolate 하여 migratepages list 로 옮김

            // zone 내의 page block 에 대해 free_pfn 만나거나 
            // resched 되어야 하거나 등등의 조건이 될 때까지 
            // page block 들 isolate 해줌
		case ISOLATE_ABORT:
            // compaction 시도 하였으나, async 에서 reschedule 요청이 오거나 
            // sync, async 에서 pending signal 이 있거나 
            // lock contention 으로 종료된 경우
			ret = COMPACT_CONTENDED;
			putback_movable_pages(&cc->migratepages);
            // isolate 한 page 들을 lru 에게 돌려줌
			cc->nr_migratepages = 0;
			goto out;
		case ISOLATE_NONE:
            // page block 들을 scan 하였으나 isolate 할 수 있는 
            // page 들이 없었던 경우
			/*
			 * We haven't isolated and migrated anything, but
			 * there might still be unflushed migrations from
			 * previous cc->order aligned block.
			 */
			goto check_drain;
		case ISOLATE_SUCCESS:
            // migrate scanner 에서 page isolate 성공적으로 한 경우
			;
		}

        // migrate scnner 가 LRU 에서 isolate 한 page 들이 들어갈 free page 를 
        // buddy 에서 isolate 한 후, migrate 수행. 
        // err : migration 실패한 횟수
		err = migrate_pages(&cc->migratepages, compaction_alloc,
				compaction_free, (unsigned long)cc, cc->mode,
				MR_COMPACTION);

		trace_mm_compaction_migratepages(cc->nr_migratepages, err,
							&cc->migratepages);

		/* All pages were either migrated or will be released */
		cc->nr_migratepages = 0;
		if (err) {
            // migration 실패한 경우, migration 해주다가 남은 
            // migratepages list 의 page 를 원래대로 되돌림 
			putback_movable_pages(&cc->migratepages);
			/*
			 * migrate_pages() may return -ENOMEM when scanners meet
			 * and we want compact_finished() to detect it
			 */
			if (err == -ENOMEM && !compact_scanners_met(cc)) {
                // migration 이 memory 부족으로 인한 실패이며, scanner 만나지 않은 경우
				ret = COMPACT_CONTENDED;
				goto out;
			}
			/*
			 * We failed to migrate at least one page in the current
			 * order-aligned block, so skip the rest of it.
			 */
			if (cc->direct_compaction &&
						(cc->mode == MIGRATE_ASYNC)) {
				cc->migrate_pfn = block_end_pfn(
						cc->migrate_pfn - 1, cc->order);
				/* Draining pcplists is useless in this case */
				cc->last_migrated_pfn = 0;

			}
		}

check_drain:
		/*
		 * Has the migration scanner moved away from the previous
		 * cc->order aligned block where we migrated from? If yes,
		 * flush the pages that were freed, so that they can merge and
		 * compact_finished() can detect immediately if allocation
		 * would succeed.
		 */
		if (cc->order > 0 && cc->last_migrated_pfn) {
			int cpu;
			unsigned long current_block_start =
				block_start_pfn(cc->migrate_pfn, cc->order);

			if (cc->last_migrated_pfn < current_block_start) {
				cpu = get_cpu();
				lru_add_drain_cpu(cpu);
				drain_local_pages(zone);
				put_cpu();
				/* No more flushing until we migrate again */
				cc->last_migrated_pfn = 0;
			}
		}

	}

out:
	/*
	 * Release free pages and update where the free scanner should restart,
	 * so we don't leave any returned pages behind in the next attempt.
	 */
	if (cc->nr_freepages > 0) {
		unsigned long free_pfn = release_freepages(&cc->freepages);

		cc->nr_freepages = 0;
		VM_BUG_ON(free_pfn == 0);
		/* The cached pfn is always the first in a pageblock */
		free_pfn = pageblock_start_pfn(free_pfn);
		/*
		 * Only go back, not forward. The cached pfn might have been
		 * already reset to zone end in compact_finished()
		 */
		if (free_pfn > zone->compact_cached_free_pfn)
			zone->compact_cached_free_pfn = free_pfn;
	}

	count_compact_events(COMPACTMIGRATE_SCANNED, cc->total_migrate_scanned);
	count_compact_events(COMPACTFREE_SCANNED, cc->total_free_scanned);

	trace_mm_compaction_end(start_pfn, cc->migrate_pfn,
				cc->free_pfn, end_pfn, sync, ret);

	return ret;
}

// classzone_idx type 인 zone 에 대해 gfp_mask 와 alloc_flags 로의 order 크기 page 
// 할당을 충족시키기 위하여 prio 로 compaction 수행 
// compaction 수행 전, compactino context 들의 초기화 해줌 
// direct compaction 이 호출 될 때 수행되는 함수
static enum compact_result compact_zone_order(struct zone *zone, int order,
		gfp_t gfp_mask, enum compact_priority prio,
		unsigned int alloc_flags, int classzone_idx)
{
	enum compact_result ret;
    // compaction 결과를 return 할 변수
	struct compact_control cc = {
		.nr_freepages = 0,
        // lru 에서 isolate 된 free page 의 수를 0 으로
		.nr_migratepages = 0,
        // lru 에서 isolate된 migrate page 의 수를 0 으로
		.total_migrate_scanned = 0,
        // migrate scanner 가 scan 한 page 의 수를 0 으로
		.total_free_scanned = 0,
        // free scanner 가 scan 한 page 의 수를 0 으로
		.order = order,
        // page 요청 order
		.gfp_mask = gfp_mask,
        // page 할당에 대한 gfp mask
		.zone = zone,
        // page 를 할당받을 zone 의 수
		.mode = (prio == COMPACT_PRIO_ASYNC) ?
					MIGRATE_ASYNC :	MIGRATE_SYNC_LIGHT,
        // kcompactd 에 의한 compaction -> MIGRATE_ASYNC
        // page fault 에서의 direct ompaction -> MIGRATE_SYNC_LIGHT 
        // direct compaction 이 호출될 때 수행되는 함수이므로, high order 가 아니라면
        // MIGRATE_SYNC_LIGHT
		.alloc_flags = alloc_flags,
		.classzone_idx = classzone_idx,
        // zone index type 을 저장
		.direct_compaction = true,
        // direct compaction 에서 호출되는 함수이므로 true
		.whole_zone = (prio == MIN_COMPACT_PRIORITY),
        // 전체 zone 에 대해 scan 할지
		.ignore_skip_hint = (prio == MIN_COMPACT_PRIORITY),
        // page block 에 skip 하라는 bit 설정되도 검사할지
		.ignore_block_suitable = (prio == MIN_COMPACT_PRIORITY) 
        // free page 선택 시, page block 선택 기준의 제한 정도 
	};
    // compaction 과정에서의 free, migrate page 를 
	INIT_LIST_HEAD(&cc.freepages);
    // free sacnner 가 scan 한 결과 free page 의 목록 초기화     
	INIT_LIST_HEAD(&cc.migratepages);
    // migrate scanner 가 scan 한 결과 in-use page 의 목록 초기화
	ret = compact_zone(zone, &cc);
    // comapctino 수행
	VM_BUG_ON(!list_empty(&cc.freepages));
	VM_BUG_ON(!list_empty(&cc.migratepages));

	return ret;
}

int sysctl_extfrag_threshold = 500;

/**
 * try_to_compact_pages - Direct compact to satisfy a high-order allocation
 * @gfp_mask: The GFP mask of the current allocation
 * @order: The order of the current allocation
 * @alloc_flags: The allocation flags of the current allocation
 * @ac: The context of current allocation
 * @mode: The migration mode for async, sync light, or sync migration
 *
 * This is the main entry point for direct page compaction.
 */
// page allocation 과정에서 order 요청에 대해 prio 라는 compaction 우선순위로 compaction 수행
enum compact_result try_to_compact_pages(gfp_t gfp_mask, unsigned int order,
		unsigned int alloc_flags, const struct alloc_context *ac,
		enum compact_priority prio)
{
	int may_perform_io = gfp_mask & __GFP_IO;
	struct zoneref *z;
	struct zone *zone;
	enum compact_result rc = COMPACT_SKIPPED;

	/*
	 * Check if the GFP flags allow compaction - GFP_NOIO is really
	 * tricky context because the migration might require IO
	 */
	if (!may_perform_io)
		return COMPACT_SKIPPED;
    // compaction 과정에서 IO 발생 가능하므로 __GFP_IO 가 설정되어 있지 않다면 안된다. 
	trace_mm_compaction_try_to_compact_pages(order, gfp_mask, prio);

	/* Compact each zone in the list */
	for_each_zone_zonelist_nodemask(zone, z, ac->zonelist, ac->high_zoneidx,
								ac->nodemask) {
        // fallback list 에 해당되는 zone 을 순회
		enum compact_result status;

		if (prio > MIN_COMPACT_PRIORITY // synchronous 에서는 DEF_COMPACT_PRIORITY 임
					&& compaction_deferred(zone, order)) {
            // compaction 요청 order 가 compact_order_failed 보다 크거나
            // comapction skip 최대 threshold 값이 넘어가지 않았다면 compaction skip 가능
			rc = max_t(enum compact_result, COMPACT_DEFERRED, rc);
            // 다른 zone 을 계속 검사
			continue;
		}
        // compaction skip 을 해주지 않아도 되는 경우라면...
        //
        // zone fallback list 에서 선택한 zone 에 대하여 gfp_mask, alloc_mask 로 order 크기 
        // page 할당을 위한 compaction 수행
		status = compact_zone_order(zone, order, gfp_mask, prio,
					alloc_flags, ac_classzone_idx(ac));
		rc = max(status, rc);

		/* The allocation should succeed, stop compacting */
		if (status == COMPACT_SUCCESS) {
            // page 할당 가능 한 경우             
			/*
			 * We think the allocation will succeed in this zone,
			 * but it is not certain, hence the false. The caller
			 * will repeat this with true if allocation indeed
			 * succeeds in this zone.
			 */
			compaction_defer_reset(zone, order, false);            
            // compaction defer counter 초기화 해주고
			break;
            // compaction 종료를 위해 loop 나감
		}
        
        // compaction 하였지만 COMPACT_SUCCESS 가 아니므로...
		if (prio != COMPACT_PRIO_ASYNC && (status == COMPACT_COMPLETE ||
					status == COMPACT_PARTIAL_SKIPPED))
            // direct compaction 에서 실패한 것이라면...
            //  - scan 을 아직 다 하지 못하였고, order 에 맞는 free page 를 구성 못했거나
            //  - scan 다 했는데도 order 에 맞는 free page 를 구성 못했거나
			/*
			 * We think that allocation won't succeed in this zone
			 * so we defer compaction there. If it ends up
			 * succeeding after all, it will be reset.
			 */
			defer_compaction(zone, order);
            // 추후 compaction 요청이 skip 될 수 있도록 counter 값 설정 및 
            // skip order 재설정 등 수행

		/*
		 * We might have stopped compacting due to need_resched() in
		 * async compaction, or due to a fatal signal detected. In that
		 * case do not try further zones
		 */
        // kcompactd 에서 실패한 것이라면...        
		if ((prio == COMPACT_PRIO_ASYNC && need_resched())
					|| fatal_signal_pending(current))
			break;
            // 재 schedule 요청이 있어서 실패해야 한다면..
            // 더이상 compaction 중지
	}

	return rc;
}


/* Compact all zones within a node */
static void compact_node(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	int zoneid;
	struct zone *zone;
	struct compact_control cc = {
		.order = -1,
		.total_migrate_scanned = 0,
		.total_free_scanned = 0,
		.mode = MIGRATE_SYNC,
		.ignore_skip_hint = true,
		.whole_zone = true,
		.gfp_mask = GFP_KERNEL,
	};


	for (zoneid = 0; zoneid < MAX_NR_ZONES; zoneid++) {

		zone = &pgdat->node_zones[zoneid];
		if (!populated_zone(zone))
			continue;

		cc.nr_freepages = 0;
		cc.nr_migratepages = 0;
		cc.zone = zone;
		INIT_LIST_HEAD(&cc.freepages);
		INIT_LIST_HEAD(&cc.migratepages);

		compact_zone(zone, &cc);

		VM_BUG_ON(!list_empty(&cc.freepages));
		VM_BUG_ON(!list_empty(&cc.migratepages));
	}
}

/* Compact all nodes in the system */
static void compact_nodes(void)
{
	int nid;

	/* Flush pending updates to the LRU lists */
	lru_add_drain_all();

	for_each_online_node(nid)
		compact_node(nid);
}

/* The written value is actually unused, all memory is compacted */
int sysctl_compact_memory;

/*
 * This is the entry point for compacting all nodes via
 * /proc/sys/vm/compact_memory
 */
int sysctl_compaction_handler(struct ctl_table *table, int write,
			void __user *buffer, size_t *length, loff_t *ppos)
{
	if (write)
		compact_nodes();

	return 0;
}

int sysctl_extfrag_handler(struct ctl_table *table, int write,
			void __user *buffer, size_t *length, loff_t *ppos)
{
	proc_dointvec_minmax(table, write, buffer, length, ppos);

	return 0;
}

#if defined(CONFIG_SYSFS) && defined(CONFIG_NUMA)

// sysfs 의 /proc/sys/vm/compact_memory 에 wirte 되어
// 전체 node 에 대해 compaction 을 해주어야 하는 경우
static ssize_t sysfs_compact_node(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	int nid = dev->id;

	if (nid >= 0 && nid < nr_node_ids && node_online(nid)) {
		/* Flush pending updates to the LRU lists */
		lru_add_drain_all();

		compact_node(nid);
	}

	return count;
}
static DEVICE_ATTR(compact, S_IWUSR, NULL, sysfs_compact_node);

int compaction_register_node(struct node *node)
{
	return device_create_file(&node->dev, &dev_attr_compact);
}

void compaction_unregister_node(struct node *node)
{
	return device_remove_file(&node->dev, &dev_attr_compact);
}
#endif /* CONFIG_SYSFS && CONFIG_NUMA */

static inline bool kcompactd_work_requested(pg_data_t *pgdat)
{
	return pgdat->kcompactd_max_order > 0 || kthread_should_stop();
}

static bool kcompactd_node_suitable(pg_data_t *pgdat)
{
	int zoneid;
	struct zone *zone;
	enum zone_type classzone_idx = pgdat->kcompactd_classzone_idx;

	for (zoneid = 0; zoneid <= classzone_idx; zoneid++) {
		zone = &pgdat->node_zones[zoneid];

		if (!populated_zone(zone))
			continue;

		if (compaction_suitable(zone, pgdat->kcompactd_max_order, 0,
					classzone_idx) == COMPACT_CONTINUE){
            trace_printk("son,compaction_suitable -> true")
			return true;    
	    }
    }

	return false;
}

static void kcompactd_do_work(pg_data_t *pgdat)
{
	/*
	 * With no special task, compact all zones so that a page of requested
	 * order is allocatable.
	 */
	int zoneid;
	struct zone *zone;
	struct compact_control cc = {
		.order = pgdat->kcompactd_max_order,
		.total_migrate_scanned = 0,
		.total_free_scanned = 0,
		.classzone_idx = pgdat->kcompactd_classzone_idx,
		.mode = MIGRATE_SYNC_LIGHT,
		.ignore_skip_hint = true,
		.gfp_mask = GFP_KERNEL,

	};
	trace_mm_compaction_kcompactd_wake(pgdat->node_id, cc.order,
							cc.classzone_idx);
	count_compact_event(KCOMPACTD_WAKE);

	for (zoneid = 0; zoneid <= cc.classzone_idx; zoneid++) {
		int status;

		zone = &pgdat->node_zones[zoneid];
		if (!populated_zone(zone))
			continue;

		if (compaction_deferred(zone, cc.order))
			continue;

		if (compaction_suitable(zone, cc.order, 0, zoneid) !=
							COMPACT_CONTINUE)
			continue;

		cc.nr_freepages = 0;
		cc.nr_migratepages = 0;
		cc.total_migrate_scanned = 0;
		cc.total_free_scanned = 0;
		cc.zone = zone;
		INIT_LIST_HEAD(&cc.freepages);
		INIT_LIST_HEAD(&cc.migratepages);

		if (kthread_should_stop())
			return;
		status = compact_zone(zone, &cc);

		if (status == COMPACT_SUCCESS) {
			compaction_defer_reset(zone, cc.order, false);
		} else if (status == COMPACT_PARTIAL_SKIPPED || status == COMPACT_COMPLETE) {
			/*
			 * We use sync migration mode here, so we defer like
			 * sync direct compaction does.
			 */
			defer_compaction(zone, cc.order);
		}

		count_compact_events(KCOMPACTD_MIGRATE_SCANNED,
				     cc.total_migrate_scanned);
		count_compact_events(KCOMPACTD_FREE_SCANNED,
				     cc.total_free_scanned);

		VM_BUG_ON(!list_empty(&cc.freepages));
		VM_BUG_ON(!list_empty(&cc.migratepages));
	}

	/*
	 * Regardless of success, we are done until woken up next. But remember
	 * the requested order/classzone_idx in case it was higher/tighter than
	 * our current ones
	 */
	if (pgdat->kcompactd_max_order <= cc.order)
		pgdat->kcompactd_max_order = 0;
	if (pgdat->kcompactd_classzone_idx >= cc.classzone_idx)
		pgdat->kcompactd_classzone_idx = pgdat->nr_zones - 1;
}
// 
// order : kswapd 로요청된 할당 order 
void wakeup_kcompactd(pg_data_t *pgdat, int order, int classzone_idx)
{
	if (!order)
		return;

	if (pgdat->kcompactd_max_order < order)
		pgdat->kcompactd_max_order = order;

	/*
	 * Pairs with implicit barrier in wait_event_freezable()
	 * such that wakeups are not missed in the lockless
	 * waitqueue_active() call.
	 */
	smp_acquire__after_ctrl_dep();

	if (pgdat->kcompactd_classzone_idx > classzone_idx)
		pgdat->kcompactd_classzone_idx = classzone_idx;

	if (!waitqueue_active(&pgdat->kcompactd_wait))
		return;

	if (!kcompactd_node_suitable(pgdat))
		return;

	trace_mm_compaction_wakeup_kcompactd(pgdat->node_id, order,
							classzone_idx);
	wake_up_interruptible(&pgdat->kcompactd_wait);
}

/*
 * The background compaction daemon, started as a kernel thread
 * from the init process.
 */
static int kcompactd(void *p)
{
	pg_data_t *pgdat = (pg_data_t*)p;
	struct task_struct *tsk = current;

	const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);

	if (!cpumask_empty(cpumask))
		set_cpus_allowed_ptr(tsk, cpumask);

	set_freezable();

	pgdat->kcompactd_max_order = 0;
	pgdat->kcompactd_classzone_idx = pgdat->nr_zones - 1;

	while (!kthread_should_stop()) {
		trace_mm_compaction_kcompactd_sleep(pgdat->node_id);
		wait_event_freezable(pgdat->kcompactd_wait,
				kcompactd_work_requested(pgdat));

		kcompactd_do_work(pgdat);
	}

	return 0;
}

/*
 * This kcompactd start function will be called by init and node-hot-add.
 * On node-hot-add, kcompactd will moved to proper cpus if cpus are hot-added.
 */
int kcompactd_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	int ret = 0;

	if (pgdat->kcompactd)
		return 0;

	pgdat->kcompactd = kthread_run(kcompactd, pgdat, "kcompactd%d", nid);
	if (IS_ERR(pgdat->kcompactd)) {
		pr_err("Failed to start kcompactd on node %d\n", nid);
		ret = PTR_ERR(pgdat->kcompactd);
		pgdat->kcompactd = NULL;
	}
	return ret;
}

/*
 * Called by memory hotplug when all memory in a node is offlined. Caller must
 * hold mem_hotplug_begin/end().
 */
void kcompactd_stop(int nid)
{
	struct task_struct *kcompactd = NODE_DATA(nid)->kcompactd;

	if (kcompactd) {
		kthread_stop(kcompactd);
		NODE_DATA(nid)->kcompactd = NULL;
	}
}

/*
 * It's optimal to keep kcompactd on the same CPUs as their memory, but
 * not required for correctness. So if the last cpu in a node goes
 * away, we get changed to run anywhere: as the first one comes back,
 * restore their cpu bindings.
 */
static int kcompactd_cpu_online(unsigned int cpu)
{
	int nid;

	for_each_node_state(nid, N_MEMORY) {
		pg_data_t *pgdat = NODE_DATA(nid);
		const struct cpumask *mask;

		mask = cpumask_of_node(pgdat->node_id);

		if (cpumask_any_and(cpu_online_mask, mask) < nr_cpu_ids)
			/* One of our CPUs online: restore mask */
			set_cpus_allowed_ptr(pgdat->kcompactd, mask);
	}
	return 0;
}

static int __init kcompactd_init(void)
{
	int nid;
	int ret;

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
					"mm/compaction:online",
					kcompactd_cpu_online, NULL);
	if (ret < 0) {
		pr_err("kcompactd: failed to register hotplug callbacks.\n");
		return ret;
	}

	for_each_node_state(nid, N_MEMORY)
		kcompactd_run(nid);
	return 0;
}
subsys_initcall(kcompactd_init)

#endif /* CONFIG_COMPACTION */
