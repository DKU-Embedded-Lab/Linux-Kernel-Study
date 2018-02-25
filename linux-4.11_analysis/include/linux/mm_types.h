#ifndef _LINUX_MM_TYPES_H
#define _LINUX_MM_TYPES_H

#include <linux/mm_types_task.h>

#include <linux/auxvec.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/uprobes.h>
#include <linux/page-flags-layout.h>
#include <linux/workqueue.h>

#include <asm/mmu.h>

#ifndef AT_VECTOR_SIZE_ARCH
#define AT_VECTOR_SIZE_ARCH 0
#endif
#define AT_VECTOR_SIZE (2*(AT_VECTOR_SIZE_ARCH + AT_VECTOR_SIZE_BASE + 1))

struct address_space;
struct mem_cgroup;

/*
 * Each physical page in the system has a struct page associated with
 * it to keep track of whatever it is we are using the page for at the
 * moment. Note that we have no way to track which tasks are using
 * a page, though if it is a pagecache page, rmap structures can tell us
 * who is mapping it.
 *
 * The objects in struct page are organized in double word blocks in
 * order to allows us to use atomic double word operations on portions
 * of struct page. That is currently only used by slub but the arrangement
 * allows the use of atomic double word operations on the flags/mapping
 * and lru list pointers also.
 */ 
// memory 에 존재한는 page frame 들이 어떤 목적으로 어디에 어떻게 쓰이고 있다는
// 것을 나타내기 위한 것
// memory 에 존재한는 모든 page frame 들은 각각에 대해 struct page 들을 가지며, 
// 이는 배열로 pglist_data->node_mem_map 에 관리
struct page {
	/* First double word block */
	unsigned long flags;		/* Atomic flags, some possibly
					 * updated asynchronously */
    // <compound page 관련 flag 정보>
    // 현재 page 가 compound page 를 구성하고 있다면 flag 에 다음 두가지 
    // 정보둥에 하나를 가지고 있음. 
    //
    // 64 bit
    //  - compound page 내에서 head page 라면... PG_head
    //  - compound page 내에서 tail page 들 중 하나라면 ... PG_tail 
    // ==> 지금은 PG_tail 안씀 PG_head 이 flag 에 설정되어 있다면 head page 
    //     tail page 는 compound_head 라는 filed 에 head page 의 주소가 담겨 
    //     있다면 tail page 인거임
    //  - compound page 가 pte 와 pmd 에 모두 map 되어 있는상태라면 
    //    첫번째 tail page 의 flag 에 PG_double_map 이 설정되어 있음
    union {
		struct address_space *mapping;	/* If low bit clear, points to
						 * inode address_space, or NULL.
						 * If page mapped as anonymous
						 * memory, low bit is set, and
						 * it points to anon_vma object:
						 * see PAGE_MAPPING_ANON below.
						 */ 
        // mapping 은 anonymous page 라면 anon_vma 로의 값을 
        // 가지고 있고, file backed page 라면 backing store 
        // 정보와 연결될 address_space 를 가짐 
        //  - 두개를 구분하기 위해 anonymous page 일 경우 mapping 
        //    변수의 0-bit 가 1로 set 된다
        //    (이걸로 구분할 수 있는 이유가 어차피 pointer 주소는 
        //     word boundary 이기 때문에 어차피 주소값의  뒷부분 
        //     0으로 채워져있음)
        //  - MOVABLE page 일 경우, page 의 mapping 
        //    변수의 1-bit 가 1로 set 된다.(PAGE_MAPPING_MOVABLE)
        //  - THP 의 경우, 두번째 tail page 의 mapping 변수를 
        //    deferred list head 로 이용
        //  - 

		void *s_mem;			/* slab first object */
		atomic_t compound_mapcount;	/* first tail page */ 
        // compound page 의 경우에는 tail page (두번째 이후부터의 page)들 중
        // 첫번재 tail page 의 compound_mapcount 변수에 pte 에 map된 횟수가
        // 저장된다.
        // 즉 두번째 page 에 기록됨
        //  - compound page 가 할당되면 첫번째 tail page 의 compound_mapcount 가
        //    -1 로 초기화 된다.
        // compound page 와 일반적인 high order 할당의 차이점은 
        // TLB 성능을 높이기 위해 pte 단위가 아닌 pmd 단위로 할당한다는 것

		/* page_deferred_list().next	 -- second tail page */
	};

	/* Second double word */
	union {
		pgoff_t index;		/* Our offset within mapping. */
        // mmap 에서의 즉 struct page 배열에서의 page frame offset 
        //
        // buddy allocator 로부터 page 가 할당되면 
        // 현재 page 가 속한 migrate type 번호가 들어감
		void *freelist;		/* sl[aou]b first free object */
		/* page_deferred_list().prev	-- second tail page */
	};

	union {
#if defined(CONFIG_HAVE_CMPXCHG_DOUBLE) && \
	defined(CONFIG_HAVE_ALIGNED_STRUCT_PAGE)
		/* Used for cmpxchg_double in slub */
		unsigned long counters;
#else
		/*
		 * Keep _refcount separate from slub cmpxchg_double data.
		 * As the rest of the double word is protected by slab_lock
		 * but _refcount is not.
		 */
		unsigned counters;
#endif
		struct {

			union {
				/*
				 * Count of ptes mapped in mms, to show when
				 * page is mapped & limit reverse map searches.
				 *
				 * Extra information about page type may be
				 * stored here for pages that are never mapped,
				 * in which case the value MUST BE <= -2.
				 * See page-flags.h for more details.
				 */
				atomic_t _mapcount;
                // 해당 page 을 가리키고 있는 pte 의 수를 의미
                // page 를 가리키는 pte 가 없을 시, -1 값을 가짐 
                // 즉 초기에는 -1 값이며 reverse mapping 에 추가 
                // 될 때마다 1씩 증가
                // e.g. 두개의 process 에서 해당 page를 사용 중일 시,
                //      이 값은 1임
                // page를 사용중인 process 의 수를 알 수 있음  
                //
                // buddy allocator 에서 ....
                //  - buddy allocator 의 free list 에 추가되게 될 때,
                //    __SetPageBuddy 함수를 통해 -128 값인 
                //    PAGE_BUDDY_MAPCOUNT_VALUE 로 설정
                //  
                //  - buddy allocator 로부터 할당되어 빠져나갈 때,
                //    __ClearPageBuddy 함수를 통해 -1 로 설정됨                
                //
				unsigned int active;		/* SLAB */
				struct {			/* SLUB */
					unsigned inuse:16;
					unsigned objects:15;
					unsigned frozen:1;
				};
				int units;			/* SLOB */
			};
			/*
			 * Usage count, *USE WRAPPER FUNCTION* when manual
			 * accounting. See page_ref.h
			 */
			atomic_t _refcount; 
            // page reference count
            // 할당될 때, 1 로 초기화 됨.
		};
	};

	/*
	 * Third double word block
	 *
	 * WARNING: bit 0 of the first word encode PageTail(). That means
	 * the rest users of the storage space MUST NOT use the bit to
	 * avoid collision and false-positive PageTail().
	 */
	union {
		struct list_head lru;	
                // per-CPU page list 에서 page 끼리 연결되기 위한 list_head 
                //
                //
                //
                    /* Pageout list, eg. active_list
					 * protected by zone_lru_lock !
					 * Can be used as a generic list
					 * by the page owner.
					 */
		struct dev_pagemap *pgmap; /* ZONE_DEVICE pages are never on an
					    * lru or handled by a slab
					    * allocator, this points to the
					    * hosting device page map.
					    */
		struct {		/* slub per cpu partial pages */
			struct page *next;	/* Next partial slab */
#ifdef CONFIG_64BIT
			int pages;	/* Nr of partial slabs left */
			int pobjects;	/* Approximate # of objects */
#else
			short int pages;
			short int pobjects;
#endif
		};

		struct rcu_head rcu_head;	/* Used by SLAB
						 * when destroying via RCU
						 */
		/* Tail pages of compound page */
		struct {
			unsigned long compound_head; /* If bit zero is set */ 
            // tail page 들 ㅣhead page 의 주소를 가리킴 
            // compound page 내의 tail page 일 경우, 0 bit 가 설정되어
            // 현재 page 가 tail 인지 알 수 있음 
			/* First tail page only */
#ifdef CONFIG_64BIT
			/*
			 * On 64 bit system we have enough space in struct page
			 * to encode compound_dtor and compound_order with
			 * unsigned int. It can help compiler generate better or
			 * smaller code on some archtectures.
			 */
			unsigned int compound_dtor;
            // first tail page 일 경우 여기에 
            // 현재 compound page destructor 함수들 목록내의 
            // offset 이 설정되어 어떤 함수를 통해 현재 compound page 를 
            // free 할 것인지 알 수 있음             
            // 이를 통해 compound_page_dtors 내의 destructor 함수 접근 
            // (comound page , thp, hugetlbfs 에 따라 다름)
			unsigned int compound_order; 
            // first tail page 일 경우 여기에 
            // 현재 compound page 의 크기 즉 order 값 설정됨  
            //
            // thp 라면 여차피 order 는9 로 정해져 있으므로 order 정보는 
            // 따로 저장 안하고 compound_dtor 만 TRANSHUGE_PAGE_DTOR 로 설정
#else
			unsigned short int compound_dtor;
			unsigned short int compound_order;
#endif
		};

#if defined(CONFIG_TRANSPARENT_HUGEPAGE) && USE_SPLIT_PMD_PTLOCKS
		struct {
			unsigned long __pad;	/* do not overlay pmd_huge_pte
						 * with compound_head to avoid
						 * possible bit 0 collision.
						 */
			pgtable_t pmd_huge_pte; /* protected by page->ptl */
		};
#endif
	};

	/* Remainder is not double word aligned */
	union {
		unsigned long private;		
        //  - page 가 free list 에 있을 때는 
        //    buddy 에서의 order slot 번호를 가짐 
        //    (해당 order 의 연속된 pageg 의 첫번째 page 가 가짐)
        //
        //  - page 가 할당되게 되면 0 으로 초기화되어 시작 
        //    swap-entry 를 가지기도 함
        //
                        /* Mapping-private opaque data:
					 	 * usually used for buffer_heads
						 * if PagePrivate set; used for
						 * swp_entry_t if PageSwapCache;
						 * indicates order in the buddy
						 * system if PG_buddy is set.
						 */
#if USE_SPLIT_PTE_PTLOCKS
#if ALLOC_SPLIT_PTLOCKS
		spinlock_t *ptl;
#else
		spinlock_t ptl;
#endif
#endif
		struct kmem_cache *slab_cache;	/* SL[AU]B: Pointer to slab */
	};

#ifdef CONFIG_MEMCG
	struct mem_cgroup *mem_cgroup;
#endif

	/*
	 * On machines where all RAM is mapped into kernel address space,
	 * we can simply calculate the virtual address. On machines with
	 * highmem some memory is mapped into kernel virtual memory
	 * dynamically, so we need a place to store that address.
	 * Note that this field could be 16 bits on x86 ... ;)
	 *
	 * Architectures with slow multiplication can define
	 * WANT_PAGE_VIRTUAL in asm/page.h
	 */
#if defined(WANT_PAGE_VIRTUAL)
	void *virtual;			/* Kernel virtual address (NULL if
					   not kmapped, ie. highmem) */

#endif /* WANT_PAGE_VIRTUAL */

#ifdef CONFIG_KMEMCHECK
	/*
	 * kmemcheck wants to track the status of each byte in a page; this
	 * is a pointer to such a status block. NULL if not tracked.
	 */
	void *shadow;
#endif

#ifdef LAST_CPUPID_NOT_IN_PAGE_FLAGS
	int _last_cpupid;
#endif
}
/*
 * The struct page can be forced to be double word aligned so that atomic ops
 * on double words work. The SLUB allocator can make use of such a feature.
 */
#ifdef CONFIG_HAVE_ALIGNED_STRUCT_PAGE
	__aligned(2 * sizeof(unsigned long))
#endif
;

#define PAGE_FRAG_CACHE_MAX_SIZE	__ALIGN_MASK(32768, ~PAGE_MASK)
#define PAGE_FRAG_CACHE_MAX_ORDER	get_order(PAGE_FRAG_CACHE_MAX_SIZE)

struct page_frag_cache {
	void * va;
#if (PAGE_SIZE < PAGE_FRAG_CACHE_MAX_SIZE)
	__u16 offset;
	__u16 size;
#else
	__u32 offset;
#endif
	/* we maintain a pagecount bias, so that we dont dirty cache line
	 * containing page->_refcount every time we allocate a fragment.
	 */
	unsigned int		pagecnt_bias;
	bool pfmemalloc;
};

typedef unsigned long vm_flags_t;

/*
 * A region containing a mapping of a non-memory backed file under NOMMU
 * conditions.  These are held in a global tree and are pinned by the VMAs that
 * map parts of them.
 */
struct vm_region {
	struct rb_node	vm_rb;		/* link in global region tree */
	vm_flags_t	vm_flags;	/* VMA vm_flags */
	unsigned long	vm_start;	/* start address of region */
	unsigned long	vm_end;		/* region initialised to here */
	unsigned long	vm_top;		/* region allocated to here */
	unsigned long	vm_pgoff;	/* the offset in vm_file corresponding to vm_start */
	struct file	*vm_file;	/* the backing file or NULL */

	int		vm_usage;	/* region usage count (access under nommu_region_sem) */
	bool		vm_icache_flushed : 1; /* true if the icache has been flushed for
						* this region */
};

#ifdef CONFIG_USERFAULTFD
#define NULL_VM_UFFD_CTX ((struct vm_userfaultfd_ctx) { NULL, })
struct vm_userfaultfd_ctx {
	struct userfaultfd_ctx *ctx;
};
#else /* CONFIG_USERFAULTFD */
#define NULL_VM_UFFD_CTX ((struct vm_userfaultfd_ctx) {})
struct vm_userfaultfd_ctx {};
#endif /* CONFIG_USERFAULTFD */

/*
 * This struct defines a memory VMM memory area. There is one of these
 * per VM-area/task.  A VM area is any part of the process virtual memory
 * space that has a special rule for the page-fault handlers (ie a shared
 * library, the executable area etc).
 */
// 전체 virtual address space 중 user space  process 의 virtual address spcae 를 
// 관리하기 위한 structure 
// (kernel address space 는 struct vm_struct 로 관리)
struct vm_area_struct {
	/* The first cache line has the info for VMA tree walking. */

	unsigned long vm_start;		/* Our start address within vm_mm. */
    // virtual address 시작 주소
	unsigned long vm_end;		/* The first byte after our end address within vm_mm. */
    // virtual address 끝 주소

	/* linked list of VM areas per task, sorted by address */
	struct vm_area_struct *vm_next, *vm_prev; 
    // vm_area_struct 의 linear liking pointer 

	struct rb_node vm_rb;
    // red black tree 에서의 현재 vm_area_struct 가 속한 node 로 
    // rb tree 에서 찾아서 container of 로 vm_area_struct 찾음

	/*
	 * Largest free memory gap in bytes to the left of this VMA.
	 * Either between this VMA and vma->vm_prev, or between one of the
	 * VMAs below us in the VMA rbtree and its ->vm_prev. This helps
	 * get_unmapped_area find a free area of the right size.
	 */
	unsigned long rb_subtree_gap; 
    // 지금 vma 와 바로 전 vma 사이의 gap 또는 그전 vma 들 사이에서의 gap 들 
    // 중 가장 큰 gap 으로 get_unmapped_area 에서 size 에 맞는 빈 영역을 
    // 찾을 때 확용
    // augmented rb tree 에서 관리된느 정보 

	/* Second cache line starts here. */

	struct mm_struct *vm_mm;	/* The address space we belong to. */ 
    // vm_area_struct 가 속한 mm 을 가르키기 위한 back-pointer 
	pgprot_t vm_page_prot;		/* Access permissions of this VMA. */ 
    // 해당 address 에 대한 접근 권한
	unsigned long vm_flags;		/* Flags, see mm.h. */
    // vm region 에 대한 properties 
    //  e.g. VM_READ, VM_WRITE, VM_EXEC, VM_SHARED : page 내용에 대한 read,write,exec,shared 가능 여부 
    //       VM_MAYREAD, VM_MAYWRITE, VM_MAYEXEC, VM_MAYSHARE : VM_ 위 플래그들이 설정될수 있다?(mprotect) 
    //       VM_GROWSDOWN, VM_GROWSUP : stack 은 VM_GROWSDOWN, heapd 은 VM_GROWSUP 
    //       VM_DONTCOPY : fork 시 해당 vm  영역을 copy 하지 말것 
    //       VM_DONTEXPAND : vm 영역 rmremap 등으로 확장 불가능

	/*
	 * For areas with an address space and backing store,
	 * linkage into the address_space->i_mmap interval tree.
	 */
	struct {
		struct rb_node rb;
        // left subtree 들중에서의 vm_end max 값을 가지는 node
		unsigned long rb_subtree_last;
        // left subtree 들중에서의 vm_end max 값
	} shared;
    // anonymous page 의 경우엔 anon_vma_struct 를 통해 
    // vma 가 관리되어 reverse mapping 이 되지만
    // file-backed page 의 경우엔 vm_area_struct 를 직접 관리
    // 옛날엔 prio_tree 로 관리 되었지만, interval tree 로 관리되도록 patch 됨
	/*
	 * A file's MAP_PRIVATE vma can be in both i_mmap tree and anon_vma
	 * list, after a COW of one of the file pages.	A MAP_SHARED vma
	 * can only be in the i_mmap tree.  An anonymous MAP_PRIVATE, stack
	 * or brk vma (with NULL file) can only be in an anon_vma list.
	 */
	struct list_head anon_vma_chain; /* Serialized by mmap_sem & * page_table_lock */ 
    // e.g. fork 되어 
    
	struct anon_vma *anon_vma;	/* Serialized by page_table_lock */
    // anonymous page COW handling, shared page 관리 
    // reverse mapping 관련 

	/* Function pointers to deal with this struct. */
	const struct vm_operations_struct *vm_ops;
    // demand paging 을 위한 open, close, mmap 함수등 

	/* Information about our backing store: */
	unsigned long vm_pgoff;		/* Offset (within vm_file) in PAGE_SIZE units */
    // file mapping 시에 전체 file 이 mapping 된 것이라면 0 으로, 부분만 mapping 
    // 된 것이라면 그offset 을 의미(page 수 기준)
	struct file * vm_file;		/* File we map to (can be NULL). */ 
    // virtual address space 에 mapping 된 struct file
	void * vm_private_data;		/* was vm_pte (shared mem) */
    
#ifndef CONFIG_MMU
	struct vm_region *vm_region;	/* NOMMU mapping region */
#endif
#ifdef CONFIG_NUMA
	struct mempolicy *vm_policy;	/* NUMA policy for the VMA */
#endif
	struct vm_userfaultfd_ctx vm_userfaultfd_ctx;
};

struct core_thread {
	struct task_struct *task;
	struct core_thread *next;
};

struct core_state {
	atomic_t nr_threads;
	struct core_thread dumper;
	struct completion startup;
};

struct kioctx_table;
struct mm_struct {
	struct vm_area_struct *mmap;		/* list of VMAs */ 
    // mm 이 가진 vm area 의 정보를 나타내는 single linked list 
	struct rb_root mm_rb;
    // vm_area_struct 와 관련된 rb- tree 로 rb tree 의 root 를 가리킴
	u32 vmacache_seqnum;                   
    /* per-thread vmacache */ 
    // task_struct 별로 가지고 있는 
    // VMCACHE_SIZE 크기(vm_area_struct 4개)의  I
    // vmacache 에 해당하는 sequence number 
#ifdef CONFIG_MMU
	unsigned long (*get_unmapped_area) (struct file *filp,
				unsigned long addr, unsigned long len,
				unsigned long pgoff, unsigned long flags); 
    // 빈 주소 주간을 찾는 함수  
    // (len 에 맞는 larget linear free address space 찾음)
#endif
	unsigned long mmap_base;		/* base of mmap area */
    // memory mapping 영역 start address
	unsigned long mmap_legacy_base;         /* base of mmap area in bottom-up allocations */ 
    // legacy vm layout 일 경우, mmap 영역의 시작 start address 
	unsigned long task_size;		/* size of task vm space */
    // task size 를 의미하며 보통 TASK_SIZE 
    // 
    //  32bit 의 경우
    //   --------------------
    //  | kernel memory (1g)|
    //  | ------------------|
    //  | user memory (3g)  | 이 크기가 3g = 0xc0000000 = TASK_SIZE = PAGE_OFFSET 
    //  ---------------------
    //
	unsigned long highest_vm_end;		/* highest vma end address */ 
    // vma 중 맨 끝 
	pgd_t * pgd;
    // pgd_t 들을 가지고 있는 page frame 에 대한 주소
    // kernel address space 들에 대한 mapping 은 모든 process 가 동일 
    // kernel vaddr 에 대한 mapping 은 kernel global page table(swapper_pg_dir) 를 통해 참조하여 가져옴

	/**
	 * @mm_users: The number of users including userspace.
	 *
	 * Use mmget()/mmget_not_zero()/mmput() to modify. When this drops
	 * to 0 (i.e. when the task exits and there are no other temporary
	 * reference holders), we also release a reference on @mm_count
	 * (which may then free the &struct mm_struct if @mm_count also
	 * drops to 0).
	 */
	atomic_t mm_users;
    // mm_struct 를 사용하는 process 의 수(e.g. fork 시 증가)
	/**
	 * @mm_count: The number of references to &struct mm_struct
	 * (@mm_users count as 1).
	 *
	 * Use mmgrab()/mmdrop() to modify. When this drops to 0, the
	 * &struct mm_struct is freed.
	 */
	atomic_t mm_count;
    // mm_struct 로의 reference 가 있는지에 대한 reference counters
    // 즉 mm_users 가 하나 이상이면 mm_count 가 1이다.
    // mm_users 가 0이 되면 mm_count 또한 1 감소하며 mm_count 가 0 이 되면 
    // mm_struct 를 free 해 준다. kernel thread lazy exit 에서도 사용
	atomic_long_t nr_ptes;			/* PTE page table pages */
#if CONFIG_PGTABLE_LEVELS > 2
	atomic_long_t nr_pmds;			/* PMD page table pages */
#endif
	int map_count;				/* number of VMAs */ 
    // mm 에 포함되어 있는 vma 의 개수

	spinlock_t page_table_lock;		/* Protects page tables and some counters */
	struct rw_semaphore mmap_sem;

	struct list_head mmlist;		/* List of maybe swapped mm's.	These are globally strung
						 * together off init_mm.mmlist, and are protected
						 * by mmlist_lock
						 */


	unsigned long hiwater_rss;	/* High-watermark of RSS usage */
	unsigned long hiwater_vm;	/* High-water virtual memory usage */

	unsigned long total_vm;		/* Total pages mapped */
	unsigned long locked_vm;	/* Pages that have PG_mlocked set */
	unsigned long pinned_vm;	/* Refcount permanently increased */
	unsigned long data_vm;		/* VM_WRITE & ~VM_SHARED & ~VM_STACK */
	unsigned long exec_vm;		/* VM_EXEC & ~VM_WRITE & ~VM_STACK */
	unsigned long stack_vm;		/* VM_STACK */
	unsigned long def_flags; 
	
    unsigned long start_code, end_code, start_data, end_data; 
    // start_code, end_code : code 영역 start ~ end address 
    // start_data, end_data : data 영역 start ~ end address
	unsigned long start_brk, brk, start_stack;
    // start_brk : heap start address
    // brk : heap data 의 현재 end address
    // start_stack : stack start address
	unsigned long arg_start, arg_end, env_start, env_end; 
    // arg_start, arg_end : argument list 영역 start ~ end address
    // env_start, env_end : envvironment 영역 start ~ end address 

	unsigned long saved_auxv[AT_VECTOR_SIZE]; /* for /proc/PID/auxv */

	/*
	 * Special counters, in some configurations protected by the
	 * page_table_lock, in other configurations by being atomic.
	 */
	struct mm_rss_stat rss_stat;

	struct linux_binfmt *binfmt;

	cpumask_var_t cpu_vm_mask_var;

	/* Architecture-specific MM context */
	mm_context_t context;

	unsigned long flags; /* Must use atomic bitops to access the bits */

	struct core_state *core_state; /* coredumping support */
#ifdef CONFIG_AIO
	spinlock_t			ioctx_lock;
	struct kioctx_table __rcu	*ioctx_table;
#endif
#ifdef CONFIG_MEMCG
	/*
	 * "owner" points to a task that is regarded as the canonical
	 * user/owner of this mm. All of the following must be true in
	 * order for it to be changed:
	 *
	 * current == mm->owner
	 * current->mm != mm
	 * new_owner->mm == mm
	 * new_owner->alloc_lock is held
	 */
	struct task_struct __rcu *owner;
#endif
	struct user_namespace *user_ns;

	/* store ref to file /proc/<pid>/exe symlink points to */
	struct file __rcu *exe_file;
#ifdef CONFIG_MMU_NOTIFIER
	struct mmu_notifier_mm *mmu_notifier_mm;
#endif
#if defined(CONFIG_TRANSPARENT_HUGEPAGE) && !USE_SPLIT_PMD_PTLOCKS
	pgtable_t pmd_huge_pte; /* protected by page_table_lock */
#endif
#ifdef CONFIG_CPUMASK_OFFSTACK
	struct cpumask cpumask_allocation;
#endif
#ifdef CONFIG_NUMA_BALANCING
	/*
	 * numa_next_scan is the next time that the PTEs will be marked
	 * pte_numa. NUMA hinting faults will gather statistics and migrate
	 * pages to new nodes if necessary.
	 */
	unsigned long numa_next_scan;

	/* Restart point for scanning and setting pte_numa */
	unsigned long numa_scan_offset;

	/* numa_scan_seq prevents two threads setting pte_numa */
	int numa_scan_seq;
#endif
#if defined(CONFIG_NUMA_BALANCING) || defined(CONFIG_COMPACTION)
	/*
	 * An operation with batched TLB flushing is going on. Anything that
	 * can move process memory needs to flush the TLB when moving a
	 * PROT_NONE or PROT_NUMA mapped page.
	 */
	bool tlb_flush_pending;
#endif
	struct uprobes_state uprobes_state;
#ifdef CONFIG_HUGETLB_PAGE
	atomic_long_t hugetlb_usage;
#endif
	struct work_struct async_put_work;
};

extern struct mm_struct init_mm;

static inline void mm_init_cpumask(struct mm_struct *mm)
{
#ifdef CONFIG_CPUMASK_OFFSTACK
	mm->cpu_vm_mask_var = &mm->cpumask_allocation;
#endif
	cpumask_clear(mm->cpu_vm_mask_var);
}

/* Future-safe accessor for struct mm_struct's cpu_vm_mask. */
static inline cpumask_t *mm_cpumask(struct mm_struct *mm)
{
	return mm->cpu_vm_mask_var;
}

#if defined(CONFIG_NUMA_BALANCING) || defined(CONFIG_COMPACTION)
/*
 * Memory barriers to keep this state in sync are graciously provided by
 * the page table locks, outside of which no page table modifications happen.
 * The barriers below prevent the compiler from re-ordering the instructions
 * around the memory barriers that are already present in the code.
 */
static inline bool mm_tlb_flush_pending(struct mm_struct *mm)
{
	barrier();
	return mm->tlb_flush_pending;
}
static inline void set_tlb_flush_pending(struct mm_struct *mm)
{
	mm->tlb_flush_pending = true;

	/*
	 * Guarantee that the tlb_flush_pending store does not leak into the
	 * critical section updating the page tables
	 */
	smp_mb__before_spinlock();
}
/* Clearing is done after a TLB flush, which also provides a barrier. */
static inline void clear_tlb_flush_pending(struct mm_struct *mm)
{
	barrier();
	mm->tlb_flush_pending = false;
}
#else
static inline bool mm_tlb_flush_pending(struct mm_struct *mm)
{
	return false;
}
static inline void set_tlb_flush_pending(struct mm_struct *mm)
{
}
static inline void clear_tlb_flush_pending(struct mm_struct *mm)
{
}
#endif

struct vm_fault;

struct vm_special_mapping {
	const char *name;	/* The name, e.g. "[vdso]". */

	/*
	 * If .fault is not provided, this points to a
	 * NULL-terminated array of pages that back the special mapping.
	 *
	 * This must not be NULL unless .fault is provided.
	 */
	struct page **pages;

	/*
	 * If non-NULL, then this is called to resolve page faults
	 * on the special mapping.  If used, .pages is not checked.
	 */
	int (*fault)(const struct vm_special_mapping *sm,
		     struct vm_area_struct *vma,
		     struct vm_fault *vmf);

	int (*mremap)(const struct vm_special_mapping *sm,
		     struct vm_area_struct *new_vma);
};

enum tlb_flush_reason {
	TLB_FLUSH_ON_TASK_SWITCH,
	TLB_REMOTE_SHOOTDOWN,
	TLB_LOCAL_SHOOTDOWN,
	TLB_LOCAL_MM_SHOOTDOWN,
	TLB_REMOTE_SEND_IPI,
	NR_TLB_FLUSH_REASONS,
};

 /*
  * A swap entry has to fit into a "unsigned long", as the entry is hidden
  * in the "index" field of the swapper address space.
  */
typedef struct {
	unsigned long val;
} swp_entry_t;

#endif /* _LINUX_MM_TYPES_H */
