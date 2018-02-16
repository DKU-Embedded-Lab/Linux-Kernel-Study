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

static void map_pages(struct list_head *list)
{
	unsigned int i, order, nr_pages;
	struct page *page, *next;
	LIST_HEAD(tmp_list);

	list_for_each_entry_safe(page, next, list, lru) {
		list_del(&page->lru);

		order = page_private(page);
		nr_pages = 1 << order;

		post_alloc_hook(page, order, __GFP_MOVABLE);
		if (order)
			split_page(page, order);

		for (i = 0; i < nr_pages; i++) {
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
    // movable page 가 아니라면
	mapping = page_mapping(page);
	if (mapping && mapping->a_ops && mapping->a_ops->isolate_page)
		return 1;

	return 0;
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
void defer_compaction(struct zone *zone, int order)
{
	zone->compact_considered = 0;
	zone->compact_defer_shift++;

	if (order < zone->compact_order_failed)
		zone->compact_order_failed = order;

	if (zone->compact_defer_shift > COMPACT_MAX_DEFER_SHIFT)
		zone->compact_defer_shift = COMPACT_MAX_DEFER_SHIFT;

	trace_mm_compaction_defer_compaction(zone, order);
}

/* Returns true if compaction should be skipped this time */
// 현재 zone 에서 compaction 을 건너뛰어야 하는지 검사
bool compaction_deferred(struct zone *zone, int order)
{
	unsigned long defer_limit = 1UL << zone->compact_defer_shift;

	if (order < zone->compact_order_failed)
		return false;

	/* Avoid possible overflow */
	if (++zone->compact_considered > defer_limit)
		zone->compact_considered = defer_limit;

	if (zone->compact_considered >= defer_limit)
		return false;

	trace_mm_compaction_deferred(zone, order);

	return true;
}

/*
 * Update defer tracking counters after successful compaction of given order,
 * which means an allocation either succeeded (alloc_success == true) or is
 * expected to succeed.
 */
void compaction_defer_reset(struct zone *zone, int order,
		bool alloc_success)
{
	if (alloc_success) {
		zone->compact_considered = 0;
		zone->compact_defer_shift = 0;
	}
	if (order >= zone->compact_order_failed)
		zone->compact_order_failed = order + 1;

	trace_mm_compaction_defer_reset(zone, order);
}

/* Returns true if restarting compaction after many failures */
bool compaction_restarting(struct zone *zone, int order)
{
	if (order < zone->compact_order_failed)
		return false;

	return zone->compact_defer_shift == COMPACT_MAX_DEFER_SHIFT &&
		zone->compact_considered >= 1UL << zone->compact_defer_shift;
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
// page block 에 대한 정보를 기록했던 bitmap 을 초기화 한다. 
static void __reset_isolation_suitable(struct zone *zone)
{
	unsigned long start_pfn = zone->zone_start_pfn;
	unsigned long end_pfn = zone_end_pfn(zone);
	unsigned long pfn;

	zone->compact_blockskip_flush = false;

	/* Walk the zone and mark every pageblock as suitable for isolation */
	for (pfn = start_pfn; pfn < end_pfn; pfn += pageblock_nr_pages) {
		struct page *page;

		cond_resched();

		if (!pfn_valid(pfn))
			continue;

		page = pfn_to_page(pfn);
		if (zone != page_zone(page))
			continue;

		clear_pageblock_skip(page);
	}

	reset_cached_positions(zone);
}

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

	if (!page)
		return;

	if (nr_isolated)
		return;

	set_pageblock_skip(page);

	pfn = page_to_pfn(page);

	/* Update where async and sync compaction should restart */
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
static bool compact_unlock_should_abort(spinlock_t *lock,
		unsigned long flags, bool *locked, struct compact_control *cc)
{
	if (*locked) {
		spin_unlock_irqrestore(lock, flags);
		*locked = false;
	}

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

	/* Isolate free pages. */
	for (; blockpfn < end_pfn; blockpfn++, cursor++) {
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

		nr_scanned++;
		if (!pfn_valid_within(blockpfn))
			goto isolate_fail;

		if (!valid_page)
			valid_page = page;

		/*
		 * For compound pages such as THP and hugetlbfs, we can save
		 * potentially a lot of iterations if we skip them at once.
		 * The check is racy, but we can consider only valid values
		 * and the only danger is skipping too much.
		 */
		if (PageCompound(page)) {
			unsigned int comp_order = compound_order(page);

			if (likely(comp_order < MAX_ORDER)) {
				blockpfn += (1UL << comp_order) - 1;
				cursor += (1UL << comp_order) - 1;
			}

			goto isolate_fail;
		}

		if (!PageBuddy(page))
			goto isolate_fail;

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

			/* Recheck this is a buddy page under lock */
			if (!PageBuddy(page))
				goto isolate_fail;
		}

		/* Found a free page, will break it into order-0 pages */
		order = page_order(page);
		isolated = __isolate_free_page(page, order);
		if (!isolated)
			break;
		set_page_private(page, order);

		total_isolated += isolated;
		cc->nr_freepages += isolated;
		list_add_tail(&page->lru, freelist);

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
	while (unlikely(too_many_isolated(zone))) {
    // 너무 많이 isolate 하였다면 ...
    // (active page 와 inactive page 를 합한 것의 반 이상 )
		/* async migration should just abort */
		if (cc->mode == MIGRATE_ASYNC)
			return 0;

		congestion_wait(BLK_RW_ASYNC, HZ/10);

		if (fatal_signal_pending(current))
			return 0;
	}

	if (compact_should_abort(cc))
		return 0;
        // isolate 작업을 시작하기 앞서 isolate 중단해야 
        // 하는 상황 or 다른 task 수행되어야 하는지 검사 
        // async 이며 다른 task 수행되어야 하면-> 종료 
        // sync 이며 다른 task 수행되어야 하면 -> reschedule

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
        // FIXME

		/*
		 * Periodically drop the lock (if held) regardless of its
		 * contention, to give chance to IRQs. Abort async compaction
		 * if contended.
		 */
		if (!(low_pfn % SWAP_CLUSTER_MAX)
		    && compact_unlock_should_abort(zone_lru_lock(zone), flags,
								&locked, cc))
			break;
        // 주기적으로 LRU lock 을 놓고 중단되어야 하는지 검사

		if (!pfn_valid_within(low_pfn))
			goto isolate_fail;
        // zone 내에 hole 이 있는 경우 pfn 이 hole 의 pfn 인지 검사
        
        // scan 중단될 조건들 검사 통화했으므로 이제 isolate 시작
		nr_scanned++;
        // scan count 증가 
		page = pfn_to_page(low_pfn);
        // pfn 으로 struct page 구함
		if (!valid_page)
			valid_page = page;
        
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
            // compound page 이므로 compactino 대상 아님 건너뛰어야 함 
            //
            // - flag 에 PG_head 설정되있거나 compound_head 의 0 bit 가
            //   설정되어 있다면 compound page 임
			unsigned int comp_order = compound_order(page);
            // 첫번째  tail page 에서 compound page 의 크기를 읽어옴            
			if (likely(comp_order < MAX_ORDER))
				low_pfn += (1UL << comp_order) - 1;
            // compound page 로부터 compound page 의 order 만큼 skip 하여
            // compound page 만큼 건너 뛰고 isolate_fail 로 이동
			goto isolate_fail;
		}

		/*
		 * Check may be lockless but that's ok as we recheck later.
		 * It's possible to migrate LRU and non-lru movable pages.
		 * Skip any other type of page
		 */
		if (!PageLRU(page)) {
            // PG_lru flag 가 없다면...
			/*
			 * __PageMovable can return false positive so we need
			 * to verify it under page_lock.
			 */
			if (unlikely(__PageMovable(page)) &&
					!PageIsolated(page)) {
                // MOVABLE page 이면서.. mapping 의 1-bit 설정
                // Isolate 된것도 아니라면.. PG_isolated 가 없음
				if (locked) {
					spin_unlock_irqrestore(zone_lru_lock(zone),
									flags);
					locked = false;
				}
                // MOVABLE 이고, 이미 LRU 에도 없는데(PG_lru 없음)
                // Isolated 되어 있지도 않음 Isolate 해주어야 함 
				if (!isolate_movable_page(page, isolate_mode))
					goto isolate_success; 
                    // page 에 PG_isolated 설정하고 out
			}

			goto isolate_fail;
		}

		/*
		 * Migration will fail if an anonymous page is pinned in memory,
		 * so avoid taking lru_lock and isolating it unnecessarily in an
		 * admittedly racy check.
		 */
		if (!page_mapping(page) &&
		    page_count(page) > page_mapcount(page))
			goto isolate_fail;
            // anonymous page 이면서 reference count 가 _mapcount 보다 큰 경우

		/*
		 * Only allow to migrate anonymous pages in GFP_NOFS context
		 * because those do not depend on fs locks.
		 */
		if (!(cc->gfp_mask & __GFP_FS) && page_mapping(page))
			goto isolate_fail;

		/* If we already hold the lock, we can skip some rechecking */
		if (!locked) {
			locked = compact_trylock_irqsave(zone_lru_lock(zone),
								&flags, cc);
			if (!locked)
				break;

			/* Recheck PageLRU and PageCompound under lock */
			if (!PageLRU(page))
				goto isolate_fail;

			/*
			 * Page become compound since the non-locked check,
			 * and it's on LRU. It can only be a THP so the order
			 * is safe to read and it's 0 for tail pages.
			 */
			if (unlikely(PageCompound(page))) {
				low_pfn += (1UL << compound_order(page)) - 1;
				goto isolate_fail;
			}
		}

		lruvec = mem_cgroup_page_lruvec(page, zone->zone_pgdat);

		/* Try isolate the page */
		if (__isolate_lru_page(page, isolate_mode) != 0)
			goto isolate_fail;

		VM_BUG_ON_PAGE(PageCompound(page), page);

		/* Successfully isolated */
		del_page_from_lru_list(page, lruvec, page_lru(page));
		inc_node_page_state(page,
				NR_ISOLATED_ANON + page_is_file_cache(page));

isolate_success:
		list_add(&page->lru, &cc->migratepages);
        // PG_isolated 설정한 page 를 lru 에서 
        // migratepages 라는 list 에 추가
		cc->nr_migratepages++;
		nr_isolated++;

		/*
		 * Record where we could have freed pages by migration and not
		 * yet flushed them to buddy allocator.
		 * - this is the lowest page that was isolated and likely be
		 * then freed by migration.
		 */
		if (!cc->last_migrated_pfn)
			cc->last_migrated_pfn = low_pfn;

		/* Avoid isolating too much */
		if (cc->nr_migratepages == COMPACT_CLUSTER_MAX) {
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
		if (nr_isolated) {
			if (locked) {
				spin_unlock_irqrestore(zone_lru_lock(zone), flags);
				locked = false;
			}
			putback_movable_pages(&cc->migratepages);
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
		/*
		 * We are checking page_order without zone->lock taken. But
		 * the only small danger is that we skip a potentially suitable
		 * pageblock, so it's not worth to check order for valid range.
		 */
		if (page_order_unsafe(page) >= pageblock_order)
			return false;
	}

	/* If the block is MIGRATE_MOVABLE or MIGRATE_CMA, allow migration */
	if (migrate_async_suitable(get_pageblock_migratetype(page)))
		return true;

	/* Otherwise skip the block */
	return false;
}

/*
 * Test whether the free scanner has reached the same or lower pageblock than
 * the migration scanner, and compaction should thus terminate.
 */ 
// migrate scanner 와 free scanner 가 검사하던 page block index 가 만났는지 
// 검사
static inline bool compact_scanners_met(struct compact_control *cc)
{
	return (cc->free_pfn >> pageblock_order)
		<= (cc->migrate_pfn >> pageblock_order);
}

/*
 * Based on information in the current compact_control, find blocks
 * suitable for isolating free pages from and then isolate them.
 */
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
	block_start_pfn = pageblock_start_pfn(cc->free_pfn);
	block_end_pfn = min(block_start_pfn + pageblock_nr_pages,
						zone_end_pfn(zone));
	low_pfn = pageblock_end_pfn(cc->migrate_pfn);

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
		if (!(block_start_pfn % (SWAP_CLUSTER_MAX * pageblock_nr_pages))
						&& compact_should_abort(cc))
			break;

		page = pageblock_pfn_to_page(block_start_pfn, block_end_pfn,
									zone);
		if (!page)
			continue;

		/* Check the block is suitable for migration */
		if (!suitable_migration_target(cc, page))
			continue;

		/* If isolation recently failed, do not retry */
		if (!isolation_suitable(cc, page))
			continue;

		/* Found a block suitable for isolating free pages from. */
		isolate_freepages_block(cc, &isolate_start_pfn, block_end_pfn,
					freelist, false);

		/*
		 * If we isolated enough freepages, or aborted due to lock
		 * contention, terminate.
		 */
		if ((cc->nr_freepages >= cc->nr_migratepages)
							|| cc->contended) {
			if (isolate_start_pfn >= block_end_pfn) {
				/*
				 * Restart at previous pageblock if more
				 * freepages can be isolated next time.
				 */
				isolate_start_pfn =
					block_start_pfn - pageblock_nr_pages;
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

	/*
	 * Record where the free scanner will restart next time. Either we
	 * broke from the loop and set isolate_start_pfn based on the last
	 * call to isolate_freepages_block(), or we met the migration scanner
	 * and the loop terminated due to isolate_start_pfn < low_pfn
	 */
	cc->free_pfn = isolate_start_pfn;
}

/*
 * This is a migrate-callback that "allocates" freepages by taking pages
 * from the isolated freelists in the block we are migrating to.
 */
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
		if (!cc->contended)
			isolate_freepages(cc);

		if (list_empty(&cc->freepages))
			return NULL;
	}

	freepage = list_entry(cc->freepages.next, struct page, lru);
	list_del(&freepage->lru);
	cc->nr_freepages--;

	return freepage;
}

/*
 * This is a migrate-callback that "frees" freepages back to the isolated
 * freelist.  All pages on the freelist are from the same zone, so there is no
 * special handling needed for NUMA.
 */
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
// zone 에서 migrate 해줄 page 들에 대해 LRU 와isolate 수행
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
    // e.g. unevictable page 까지 허용하며 async mode 일 시... 
    //   isolate_mode :  ... 1100
	/*
	 * Start at where we last stopped, or beginning of the zone as
	 * initialized by compact_zone()
	 */
	low_pfn = cc->migrate_pfn;
	block_start_pfn = pageblock_start_pfn(low_pfn);
    // low_pfn 이 속한 page block 내의 start pfn
	if (block_start_pfn < zone->zone_start_pfn)
		block_start_pfn = zone->zone_start_pfn;

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
        // scanning 을 무조건 모든 memory 에 하는 것이 아니라 중간에 
        // 그만할만 하면 그만 둠
		if (!(low_pfn % (SWAP_CLUSTER_MAX * pageblock_nr_pages))
						&& compact_should_abort(cc))
			break;
        // <asynchronous mode(kcompactd) 인 경우.. > 
        // page block 32 개 즉 16384 개의 page(64MB) 단위 align 영역까지 
        // scan 하였고, re-schedule 되여야 한다면 scanning 종료 
        //
        // memory 영역 
        //
        //  low_pfn
        //   |   ------- 여기까지 scan 하였는데 re-schedule 요청이 있다면
        //   |   |       scanning 중단
        //   |   *
        // ------------------------------------------------------
        // |     |     |     |     |   ...    |     |     |     |
        // ------------------------------------------------------
        // 0     64   128   192   256

		page = pageblock_pfn_to_page(block_start_pfn, block_end_pfn,
									zone);
        // scan 할 page block 내의 첫번째 page 의 struct page 가져옴
		if (!page)
			continue;
            // 다음 page block 으로 이동

		/* If isolation recently failed, do not retry */
		if (!isolation_suitable(cc, page))
			continue;
        // 현재 page block 의 migrate skip bit 검사 후 
        // 설정되어 있다면 다음 page block 검사 수행
        // PB_migrate_skip 는 scan  할 page block 을 
        // 줄여주기 위한 hint 로 동작
        // (해당 page block 에 대해 migration 이 취소된 적 있는 경우
        // 이 hint 가 page block bitmap 에 써짐)

		/*
		 * For async compaction, also only scan in MOVABLE blocks.
		 * Async compaction is optimistic to see if the minimum amount
		 * of work satisfies the allocation.
		 */
		if (cc->mode == MIGRATE_ASYNC &&
		    !migrate_async_suitable(get_pageblock_migratetype(page)))
			continue;
        // asynchronous compaction 의 경우, scanning 중인 page block 내의 
        // 이 MIGRATE_MOVABLE 에 속하지 않는다면 다음 page block 에 대해
        // scan 수행

		/* Perform the isolation */
        // low_pfn 부터 block_end_pfn 까지의 512 개 page frame 에 대해
        // isolate_mode 로 isolate 수행
		low_pfn = isolate_migratepages_block(cc, low_pfn,
						block_end_pfn, isolate_mode);
        // 현재 page block 내의 base page 들에 대해 PG_lru 설정되어 있는 
        // page 들에 PG_lru 없애고 PG_isolated 추가하고.. 
        // 
        // low_pfn 이 0 인 경우...
        //  - 너무 많이 isolate 한 경우
		if (!low_pfn || cc->contended)
			return ISOLATE_ABORT;

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
        // migrate scanner 와 free scanner 가 만났다면 
        // compaction 끝나야 함. 
		/* Let the next compaction start anew. */
		reset_cached_positions(zone);
        // 다음 round 의 compaction 을 위해 cache 정보 초기화
		/*
		 * Mark that the PG_migrate_skip information should be cleared
		 * by kswapd when it goes to sleep. kcompactd does not set the
		 * flag itself as the decision to be clear should be directly
		 * based on an allocation request.
		 */
		if (cc->direct_compaction)
			zone->compact_blockskip_flush = true;
        // FIXME compact_blockskip_flush 란?

		if (cc->whole_zone)
			return COMPACT_COMPLETE;
            // 전체 zone 영역에 대한 compaction 에서 두 scanner 가 만났지만
            // free page 확보를 하지 못함
		else
			return COMPACT_PARTIAL_SKIPPED;
            // 부분 zone 영역에 대한 compaction 에서 두 scanner 가 만났지만
            // free page 확보를 하지 못함
	}
    // 두 scanner 가 안만났다면 계속 진행
    
    if (is_via_compact_memory(cc->order))
		return COMPACT_CONTINUE;
        // 두 scanner 가 안만났고, compact_memory 에 write 요청해서 
        // compaction 수행된 경우, 만날때까지 전체 zone 에 대해 
        // 수행되어야 하므로 계속 수행

	/* Compaction run is not finished if the watermark is not met */
    // proc 의 compact_memory 조작을 통하 compactino 이 아니며 
    // 아직 두 scanner 가 만나지 않은 경우, 지금까지 상황이 충분히 
    // compaction 된 것인지 확인 필요함 
    watermark = zone->watermark[cc->alloc_flags & ALLOC_WMARK_MASK];
    // free page 기준 확인하기 위한 watermark 값을 가져옴
	if (!zone_watermark_ok(zone, cc->order, watermark, cc->classzone_idx,
							cc->alloc_flags))
		return COMPACT_CONTINUE;
        // free page 가 watermark 보다 부족하다면 계속 compaction 해주어야됨

    // free page 가 wmark 보다 많은 경우 이제 그만큼 연속적 page 가 
    // 있는 것인지 검사
	/* Direct compactor: Is a suitable page free? */
	for (order = cc->order; order < MAX_ORDER; order++) {
		struct free_area *area = &zone->free_area[order];
		bool can_steal;

		/* Job done if page is free of the right migratetype */
		if (!list_empty(&area->free_list[migratetype]))
			return COMPACT_SUCCESS;
        // order 에대한 free page list 가 비어있지 않다면 
        // scanner 안만났어도 compaction 더이상 안해도됨 완료
#ifdef CONFIG_CMA
		/* MIGRATE_MOVABLE can fallback on MIGRATE_CMA */
		if (migratetype == MIGRATE_MOVABLE &&
			!list_empty(&area->free_list[MIGRATE_CMA]))
			return COMPACT_SUCCESS;
        // CONFIG_CMA 켜있고MIGRATE_MOVABLE 에 대한 
        // 요청일 경우, UNMOVABLE 등의 다른 
        // migrate type 에서 확인 전, CMA 확인 및 
        // CMA 에 order 에 대한 free 있다면 종료
#endif
		/*
		 * Job done if allocation would steal freepages from
		 * other migratetype buddy lists.
		 */
		if (find_suitable_fallback(area, order, migratetype,
						true, &can_steal) != -1)
			return COMPACT_SUCCESS; 
        // 다른 migrate type 에서도 order 의 free page 여부 확인
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
        // 계속 compaction 해주라는 말과 같은 것이므로 
        // COMPACT_CONTINUE 로변경

	return ret;
}

/*
 * compaction_suitable: Is this suitable to run compaction on this zone now?
 * Returns
 *   COMPACT_SKIPPED  - If there are too few free pages for compaction
 *   COMPACT_SUCCESS  - If the allocation would succeed without compaction
 *   COMPACT_CONTINUE - If compaction should run now
 */
// FIXME compactino 가능 계산법 이해 안됨
static enum compact_result __compaction_suitable(struct zone *zone, int order,
					unsigned int alloc_flags,
					int classzone_idx,
					unsigned long wmark_target)
{
	unsigned long watermark;

	if (is_via_compact_memory(order))
		return COMPACT_CONTINUE;
    // /proc/sys/vm/compact_memory 에 1 이 wirte 되어 모든 zone 영역에 대해 
    // compaction 이 하라고 요청한 경우
    // (compact_memory 에 1 write 해서 compactino 요청하면 order 가 -1 로요청)
    //
	watermark = zone->watermark[alloc_flags & ALLOC_WMARK_MASK];
	/*
	 * If watermarks for high-order allocation are already met, there
	 * should be no need for compaction at all.
	 */
	if (zone_watermark_ok(zone, order, watermark, classzone_idx,
								alloc_flags))
		return COMPACT_SUCCESS;
    // order 의 요청에도 현재 free page 의 수가 watermark 값을 넘는다면
    // compaction 필요 없음
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
    // 설정하며 아닐경우 min 으로 설정 
	watermark += compact_gap(order);
	if (!__zone_watermark_ok(zone, 0, watermark, classzone_idx,
						ALLOC_CMA, wmark_target))
		return COMPACT_SKIPPED;

	return COMPACT_CONTINUE;
}
// compaction 수행 전 compaction 수행할만한 상태인지 검사 
// FIXME
enum compact_result compaction_suitable(struct zone *zone, int order,
					unsigned int alloc_flags,
					int classzone_idx)
{
	enum compact_result ret;
	int fragindex;

	ret = __compaction_suitable(zone, order, alloc_flags, classzone_idx,
				    zone_page_state(zone, NR_FREE_PAGES));
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
		fragindex = fragmentation_index(zone, order);
		if (fragindex >= 0 && fragindex <= sysctl_extfrag_threshold)
			ret = COMPACT_NOT_SUITABLE_ZONE;
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
    // FIXME compaction 가능 여부 계산 과정?
	/* Compaction is likely to fail */
	if (ret == COMPACT_SUCCESS || ret == COMPACT_SKIPPED)
		return ret;

	/* huh, compaction_suitable is returning something unexpected */
	VM_BUG_ON(ret != COMPACT_CONTINUE);

	/*
	 * Clear pageblock skip if there were failures recently and compaction
	 * is about to be retried after being deferred.
	 */
	if (compaction_restarting(zone, cc->order))
		__reset_isolation_suitable(zone);
        // 현재 zone 에 대하여 compactino 을 처음부터 다시 수행해야 하는 경우 
        // zone 의 pageblock_flags 에 설정된 skip bit 를 다 clear해줌
	/*
	 * Setup to move all movable pages to the end of the zone. Used cached
	 * information on where the scanners should start (unless we explicitly
	 * want to compact the whole zone), but check that it is initialised
	 * by ensuring the values are within zone boundaries.
	 */
    // compaction 을 시작하기 위한 초기화 수행
	if (cc->whole_zone) {
        // 전체 zone 에 대해 compaction 해야 될 때
		cc->migrate_pfn = start_pfn;
        // migrate scanner 이 scan 시작할 위치 설정
		cc->free_pfn = pageblock_start_pfn(end_pfn - 1);
        // free scanner 이 scan 시작할 위치 설정
        // (zone 의 마지막 pfn 이 속한 page block 시작 위치)
	} else {
        // 전체 zone 에 대한 compactino 이 아닐 경우
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
			zone->compact_cached_migrate_pfn[1] = cc->migrate_pfn;
            // migrate scanner cache 정보 재설정
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
    // CONFIG_MIGRATION 이 설정되어 있다면 수행 
    // FIXME 무슨의미?
    
    // compaction 상태가 COMPACT_CONTINUE 가 아닌 상황이 올때까지 
    // 계속 compact 상태 검사 수행
	while ((ret = compact_finished(zone, cc, migratetype)) ==
						COMPACT_CONTINUE) {
        // loop 결과 아래 상태면 compaction 성공
        //  - 두 migrate scanner 가 만났거나 
        //  - free page 가 wmark 보다 많고 연속적 order 에 맞는 
        //    연속된 free page 가 있는지 검사
		int err;

        // 먼저 migrate scanner 를 동작시켜 migrate 대상 page 를 
        // lru 에서 isolate 하여 migratepages 로 옮김
		switch (isolate_migratepages(zone, cc)) {
            // zone 내의 page block 에 대해 free_pfn 만나거나 
            // resched 되어야 하거나 등등의 조건이 될 때까지 
            // page block 들 isolate 해줌
		case ISOLATE_ABORT:
			ret = COMPACT_CONTENDED;
			putback_movable_pages(&cc->migratepages);
            // isolate 한 것들 되돌림
			cc->nr_migratepages = 0;
			goto out;
		case ISOLATE_NONE:
			/*
			 * We haven't isolated and migrated anything, but
			 * there might still be unflushed migrations from
			 * previous cc->order aligned block.
			 */
			goto check_drain;
		case ISOLATE_SUCCESS:
			;
		}

		err = migrate_pages(&cc->migratepages, compaction_alloc,
				compaction_free, (unsigned long)cc, cc->mode,
				MR_COMPACTION);

		trace_mm_compaction_migratepages(cc->nr_migratepages, err,
							&cc->migratepages);

		/* All pages were either migrated or will be released */
		cc->nr_migratepages = 0;
		if (err) {
			putback_movable_pages(&cc->migratepages);
			/*
			 * migrate_pages() may return -ENOMEM when scanners meet
			 * and we want compact_finished() to detect it
			 */
			if (err == -ENOMEM && !compact_scanners_met(cc)) {
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
static enum compact_result compact_zone_order(struct zone *zone, int order,
		gfp_t gfp_mask, enum compact_priority prio,
		unsigned int alloc_flags, int classzone_idx)
{
	enum compact_result ret;
    // compaction 결과
	struct compact_control cc = {
		.nr_freepages = 0,
		.nr_migratepages = 0,
		.total_migrate_scanned = 0,
		.total_free_scanned = 0,
		.order = order,
		.gfp_mask = gfp_mask,
		.zone = zone,
		.mode = (prio == COMPACT_PRIO_ASYNC) ?
					MIGRATE_ASYNC :	MIGRATE_SYNC_LIGHT,
        // kcompactd 에 의한 compaction 이 아니므로  
        // synchronous compaction mode 설정 
		.alloc_flags = alloc_flags,
		.classzone_idx = classzone_idx,
		.direct_compaction = true,
		.whole_zone = (prio == MIN_COMPACT_PRIORITY),
		.ignore_skip_hint = (prio == MIN_COMPACT_PRIORITY),
		.ignore_block_suitable = (prio == MIN_COMPACT_PRIORITY)
        // 위 세가지는 compactino priority 가 높아 전체 zone 에 대해, 
        // 전체 zone 을 다 scan 하고 모든 page 들에 대해 다 검사할지 여부            
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
            // compaction 이 수행되어야 하는우선순위가 낮거나  
            // 현재 zone 에서 compaction 이 delay 된 경우 
			rc = max_t(enum compact_result, COMPACT_DEFERRED, rc);
            // 
			continue;
		}
        // zone fallback list 에서 선택한 zone 에 대하여 gfp_mask, alloc_mask 로 order 크기 
        // page 할당을 위한 compaction 수행
		status = compact_zone_order(zone, order, gfp_mask, prio,
					alloc_flags, ac_classzone_idx(ac));
		rc = max(status, rc);

		/* The allocation should succeed, stop compacting */
		if (status == COMPACT_SUCCESS) {
			/*
			 * We think the allocation will succeed in this zone,
			 * but it is not certain, hence the false. The caller
			 * will repeat this with true if allocation indeed
			 * succeeds in this zone.
			 */
			compaction_defer_reset(zone, order, false);

			break;
		}

		if (prio != COMPACT_PRIO_ASYNC && (status == COMPACT_COMPLETE ||
					status == COMPACT_PARTIAL_SKIPPED))
			/*
			 * We think that allocation won't succeed in this zone
			 * so we defer compaction there. If it ends up
			 * succeeding after all, it will be reset.
			 */
			defer_compaction(zone, order);

		/*
		 * We might have stopped compacting due to need_resched() in
		 * async compaction, or due to a fatal signal detected. In that
		 * case do not try further zones
		 */
		if ((prio == COMPACT_PRIO_ASYNC && need_resched())
					|| fatal_signal_pending(current))
			break;
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
					classzone_idx) == COMPACT_CONTINUE)
			return true;
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
