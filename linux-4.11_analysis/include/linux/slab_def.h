#ifndef _LINUX_SLAB_DEF_H
#define	_LINUX_SLAB_DEF_H

#include <linux/reciprocal_div.h>

/*
 * Definitions unique to the original Linux SLAB allocator.
 */
// slab cache allocator 에서 각 cache type을 관리하는 구조체로 
// 각 kmem_cache 당 하나의 object type만 관리하며, slab의 위치 및 
// slab들의 object들에대한 정보를 담고 있음. 
//
// <-----|------|------|------|------|------|------|------|------|------|------|------|>
//   ... | page | page | page | page | page | page | page | page | page | page | page |...
//       |    slab1    |             |    slab2    |                    |    slab3    |   
//       |xxxxxxxxxxxxx|             |ooooooooooooo|                    |xxooxxxooooxx| 
//           objects                     objects                            objects 
//
//       struct page 를 통해 각 object의 할당 상태 표현
//  
//    ---kmem_cache---
//    |  cpu_cache <-|-----------> array_cache
//    |  ...         |
//    |  node <------|-----
//    ----------------    |   
//                        ----> --kmem_cache_node---
//                              |  slabs_partial <-|--> slab3
//                              |  slabs_full <----|--> slab1
//                              |  slabs_free <----|--> slab2
//                              | ...              |
//                              --------------------
// object 할당 우선순위.
//  1. per-CPU slab cache 에서 최근 free object 할당
//  2. 현재 가진 slab에서의 미사용 object 할당
//  3. buddy 로부터 새로 받은 slab에서 object 할당
//                              
//  
struct kmem_cache {
	struct array_cache __percpu *cpu_cache;
    // 각 CPU마다의 최근 free 된 object들을 별도로 관리(LIFO 방식)하는 
    // object 단위의 per-CPU cache
/* 1) Cache tunables. Protected by slab_mutex */
	unsigned int batchcount;
    // per-CPU cache 가 부족할 때, slab에서 per-CPU cache로 object들을 넘겨주거나
    // slab cache가 부족할 때, 더 할당할 object의 수 
	unsigned int limit;
    // per-CPU cache가 수용할 수 있는 최대 object 수 
    // 현재 per-CPU cache의 object 수가 limit를 넘는다면 batchcount 만큼을 slab에게 
    // 다시 돌려줌    
	unsigned int shared;

	unsigned int size;
	struct reciprocal_value reciprocal_buffer_size; 
    // object의 slab내 index를 결정 할 때, 사용. 
    // Division이 Multiplication 보다 느리므로 object의 index 계산 시, 
    // Newton-Raphson 기법을 사용하며 추후, 사용하기 위해 reciprocal_value 를 
    // 미리 저장해둠
    // FIXME
    // 과정은 추후...
    //
/* 2) touched by every alloc & free from the backend */

	unsigned int flags;		/* constant flags */ 
    // cache properties 정보 저장. 
    //  e.g. CFLGS_OFF_SLAB : slab management 정보를 object 가 담긴 slab과 별도로 관리
	unsigned int num;		/* # of objs per slab */
    // 하나의 slab이 가질 수 있는 object의 최대 수
/* 3) cache_grow/shrink */
	/* order of pgs per slab (2^n) */
	unsigned int gfporder;
    // slab이 가지고 있는 page 수 
    //   e.g. gfporder : 2 -> slab 이 4 개의 page로 구성 
       
	/* force GFP flags, e.g. GFP_DMA */
	gfp_t allocflags;

	size_t colour;			/* cache colouring range */ 
    // slab coloring 은 cache performance 를 위해 사용. 각 slab들이 다른 offset 으로 
    // 시작하게 함으로써, L1 cache line 에 똑같이 mapping되는 것을 막음
    //
    // slab coloring 에서의 설정가능 최대 color 수 
    // e.g. 5 개의 color 가능, colour_off : 8 BYTE 
    //  => 각 slab들에서 0, 8, 16, 24, 32 의 offset 이후, object 사용
	unsigned int colour_off;	/* colour offset */ 
    // color offset으로 coloring value 에 곱하여 사용. 
	struct kmem_cache *freelist_cache;
	unsigned int freelist_size;

	/* constructor func */
	void (*ctor)(void *obj);
    // object 생성시 별도로 추가 호출되도록 설정가능 한 함수
/* 4) cache creation/removal */
	const char *name;
    // 생성한 cache의 이름 e.g. inode_cache
	struct list_head list;
    // 여러 kmem_cache 들을 연결
	int refcount;
    // cache 삭제시 사용할 references counter
	int object_size;
    // cache 생성시, 요청한 object 크기 + fill byte(padding)
	int align;
    // 정려할 byte수
/* 5) statistics */
#ifdef CONFIG_DEBUG_SLAB
	unsigned long num_active;
	unsigned long num_allocations;
	unsigned long high_mark;
	unsigned long grown;
	unsigned long reaped;
	unsigned long errors;
	unsigned long max_freeable;
	unsigned long node_allocs;
	unsigned long node_frees;
	unsigned long node_overflow;
	atomic_t allochit;
	atomic_t allocmiss;
	atomic_t freehit;
	atomic_t freemiss;
#ifdef CONFIG_DEBUG_SLAB_LEAK
	atomic_t store_user_clean;
#endif

	/*
	 * If debugging is enabled, then the allocator can add additional
	 * fields and/or padding to every object. size contains the total
	 * object size including these internal fields, the following two
	 * variables contain the offset to the user object and its size.
	 */
	int obj_offset;
#endif /* CONFIG_DEBUG_SLAB */

#ifdef CONFIG_MEMCG
	struct memcg_cache_params memcg_params;
#endif
#ifdef CONFIG_KASAN
	struct kasan_cache kasan_info;
#endif

#ifdef CONFIG_SLAB_FREELIST_RANDOM
	unsigned int *random_seq;
#endif

	struct kmem_cache_node *node[MAX_NUMNODES];
    // node 별로 full slabs, partially free slabs, free slabs 의 3가지 
    // linked list로 object들이 모인 slab관리
};

static inline void *nearest_obj(struct kmem_cache *cache, struct page *page,
				void *x)
{
	void *object = x - (x - page->s_mem) % cache->size;
	void *last_object = page->s_mem + (cache->num - 1) * cache->size;

	if (unlikely(object > last_object))
		return last_object;
	else
		return object;
}

#endif	/* _LINUX_SLAB_DEF_H */
