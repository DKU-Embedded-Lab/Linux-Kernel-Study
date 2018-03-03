#ifndef _LINUX_COMPACTION_H
#define _LINUX_COMPACTION_H

/*
 * Determines how hard direct compaction should try to succeed.
 * Lower value means higher priority, analogically to reclaim priority.
 */ 
// direction compaction 을 얼마나 hard 하게 할지에 대한 level 정도
enum compact_priority {                                     //<priority level>
	COMPACT_PRIO_SYNC_FULL,                                 // high     0  
	MIN_COMPACT_PRIORITY = COMPACT_PRIO_SYNC_FULL,          //          0
	COMPACT_PRIO_SYNC_LIGHT,                                //          1
	MIN_COMPACT_COSTLY_PRIORITY = COMPACT_PRIO_SYNC_LIGHT,  //          1
	DEF_COMPACT_PRIORITY = COMPACT_PRIO_SYNC_LIGHT,         // default  1
	COMPACT_PRIO_ASYNC,                                     //          2
	INIT_COMPACT_PRIORITY = COMPACT_PRIO_ASYNC              // low      2
};

/* Return values for compact_zone() and try_to_compact_pages() */
/* When adding new states, please adjust include/trace/events/compaction.h */
enum compact_result {
	/* For more detailed tracepoint output - internal to compaction */
	COMPACT_NOT_SUITABLE_ZONE,    
    // - tracepoint 의 debugging message 용도로 그냥 COMPACT_SKIPPED 와 같은 의미

	/*
	 * compaction didn't start as it was not possible or direct reclaim
	 * was more suitable
	 */
	COMPACT_SKIPPED, 
    // - compaction 해주면 안되는 gfp flag 들이 있어 compaction 수행안함 
    // - compaction_suitable 에서 compaction 가능 여부 검사 시, free page 가 
    //   적으므로 현재 zone 의  compaction 을 skip 한다는 의미로 사용

	/* compaction didn't start as it was deferred due to past failures */
	COMPACT_DEFERRED,    
    // - compaction 요청이 왔지만 그전 compaction 이 실패한지 얼마 되지 않아
    //   compaction 이 미루어진 상태임
    //   (그전 lound 에서 compaction 이 실패하여 compaction term 이 증가됨)

	/* compaction not active last round */
	COMPACT_INACTIVE = COMPACT_DEFERRED,
    // - compaction 이 동작중이지 않은 상태임
    //   (그전 lound 이후, 아직 다음 lound 수행될 시간이 아님)  

	/* For more detailed tracepoint output - internal to compaction */
	COMPACT_NO_SUITABLE_PAGE,    
    // - tracepoint 의 debugging message 용도로 그냥 COMPACT_CONTINUE 와 같은 의미

	/* compaction should continue to another pageblock */
	COMPACT_CONTINUE,
    // - 다른 page block 에 대해서도 계속 compaction 을 해주어야 함 
    // - compaction_suitable 에서 compaction 가능 여부 검사 시, free page 의 수가
    //   그래도 좀 있어 compactino 결과 page 확보가 가능 할 것으로 보이므로
    //   compaction 해도 된다는 의미로 사용
   
	/*
	 * The full zone was compacted scanned but wasn't successfull to compact
	 * suitable pages.
	 */
	COMPACT_COMPLETE,
    // - zone 전체를 scan 하였지만 order 에 맞는 page 만큼 compaction 하짐 못함 
    //   즉 두 scanner 가 만났지만 free page 부족함

	/*
	 * direct compaction has scanned part of the zone but wasn't successfull
	 * to compact suitable pages.
	 */
	COMPACT_PARTIAL_SKIPPED,
    // - zone 일부를 scan 하였지만 order 에 맞는 page 만큼 compaction 하짐 못함 
    //   즉 두 scanner 가 만났지만 free page 부족함

	/* compaction terminated prematurely due to lock contentions */
	COMPACT_CONTENDED,
    // - compaction 과정이 lock 때문에 대기되어야 하거나 task 에
    //   pending signal 이 있는 경우    
	/*
	 * direct compaction terminated after concluding that the allocation
	 * should now succeed
	 */    
	COMPACT_SUCCESS,
    // - direct compaction 성공하여 요청 order 의 page 할당 가능한 상태 
    //   compaction_suitable 에서 compaction 가능 여부 검사시, 
    //   compaction 없이도 할당 가능한 상태임을 나타냄
};

struct alloc_context; /* in mm/internal.h */

/*
 * Number of free order-0 pages that should be available above given watermark
 * to make sure compaction has reasonable chance of not running out of free
 * pages that it needs to isolate as migration target during its work.
 */
static inline unsigned long compact_gap(unsigned int order)
{
	/*
	 * Although all the isolations for migration are temporary, compaction
	 * free scanner may have up to 1 << order pages on its list and then
	 * try to split an (order - 1) free page. At that point, a gap of
	 * 1 << order might not be enough, so it's safer to require twice that
	 * amount. Note that the number of pages on the list is also
	 * effectively limited by COMPACT_CLUSTER_MAX, as that's the maximum
	 * that the migrate scanner can have isolated on migrate list, and free
	 * scanner is only invoked when the number of isolated free pages is
	 * lower than that. But it's not worth to complicate the formula here
	 * as a bigger gap for higher orders than strictly necessary can also
	 * improve chances of compaction success.
	 */
	return 2UL << order;
}

#ifdef CONFIG_COMPACTION
extern int sysctl_compact_memory;
extern int sysctl_compaction_handler(struct ctl_table *table, int write,
			void __user *buffer, size_t *length, loff_t *ppos);
extern int sysctl_extfrag_threshold;
extern int sysctl_extfrag_handler(struct ctl_table *table, int write,
			void __user *buffer, size_t *length, loff_t *ppos);
extern int sysctl_compact_unevictable_allowed;

extern int fragmentation_index(struct zone *zone, unsigned int order);
extern enum compact_result try_to_compact_pages(gfp_t gfp_mask,
		unsigned int order, unsigned int alloc_flags,
		const struct alloc_context *ac, enum compact_priority prio);
extern void reset_isolation_suitable(pg_data_t *pgdat);
extern enum compact_result compaction_suitable(struct zone *zone, int order,
		unsigned int alloc_flags, int classzone_idx);

extern void defer_compaction(struct zone *zone, int order);
extern bool compaction_deferred(struct zone *zone, int order);
extern void compaction_defer_reset(struct zone *zone, int order,
				bool alloc_success);
extern bool compaction_restarting(struct zone *zone, int order);

/* Compaction has made some progress and retrying makes sense */
static inline bool compaction_made_progress(enum compact_result result)
{
	/*
	 * Even though this might sound confusing this in fact tells us
	 * that the compaction successfully isolated and migrated some
	 * pageblocks.
	 */
	if (result == COMPACT_SUCCESS)
		return true;

	return false;
}

/* Compaction has failed and it doesn't make much sense to keep retrying. */
static inline bool compaction_failed(enum compact_result result)
{
	/* All zones were scanned completely and still not result. */
	if (result == COMPACT_COMPLETE)
		return true;

	return false;
}

/*
 * Compaction  has backed off for some reason. It might be throttling or
 * lock contention. Retrying is still worthwhile.
 */
static inline bool compaction_withdrawn(enum compact_result result)
{
	/*
	 * Compaction backed off due to watermark checks for order-0
	 * so the regular reclaim has to try harder and reclaim something.
	 */
	if (result == COMPACT_SKIPPED)
		return true;

	/*
	 * If compaction is deferred for high-order allocations, it is
	 * because sync compaction recently failed. If this is the case
	 * and the caller requested a THP allocation, we do not want
	 * to heavily disrupt the system, so we fail the allocation
	 * instead of entering direct reclaim.
	 */
	if (result == COMPACT_DEFERRED)
		return true;

	/*
	 * If compaction in async mode encounters contention or blocks higher
	 * priority task we back off early rather than cause stalls.
	 */
	if (result == COMPACT_CONTENDED)
		return true;

	/*
	 * Page scanners have met but we haven't scanned full zones so this
	 * is a back off in fact.
	 */
	if (result == COMPACT_PARTIAL_SKIPPED)
		return true;

	return false;
}


bool compaction_zonelist_suitable(struct alloc_context *ac, int order,
					int alloc_flags);

extern int kcompactd_run(int nid);
extern void kcompactd_stop(int nid);
extern void wakeup_kcompactd(pg_data_t *pgdat, int order, int classzone_idx);

#else
static inline void reset_isolation_suitable(pg_data_t *pgdat)
{
}

static inline enum compact_result compaction_suitable(struct zone *zone, int order,
					int alloc_flags, int classzone_idx)
{
	return COMPACT_SKIPPED;
}

static inline void defer_compaction(struct zone *zone, int order)
{
}

static inline bool compaction_deferred(struct zone *zone, int order)
{
	return true;
}

static inline bool compaction_made_progress(enum compact_result result)
{
	return false;
}

static inline bool compaction_failed(enum compact_result result)
{
	return false;
}

static inline bool compaction_withdrawn(enum compact_result result)
{
	return true;
}

static inline int kcompactd_run(int nid)
{
	return 0;
}
static inline void kcompactd_stop(int nid)
{
}

static inline void wakeup_kcompactd(pg_data_t *pgdat, int order, int classzone_idx)
{
}

#endif /* CONFIG_COMPACTION */

#if defined(CONFIG_COMPACTION) && defined(CONFIG_SYSFS) && defined(CONFIG_NUMA)
struct node;
extern int compaction_register_node(struct node *node);
extern void compaction_unregister_node(struct node *node);

#else

static inline int compaction_register_node(struct node *node)
{
	return 0;
}

static inline void compaction_unregister_node(struct node *node)
{
}
#endif /* CONFIG_COMPACTION && CONFIG_SYSFS && CONFIG_NUMA */

#endif /* _LINUX_COMPACTION_H */
