#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/coredump.h>
#include <linux/mmu_notifier.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/mm_inline.h>
#include <linux/kthread.h>
#include <linux/khugepaged.h>
#include <linux/freezer.h>
#include <linux/mman.h>
#include <linux/hashtable.h>
#include <linux/userfaultfd_k.h>
#include <linux/page_idle.h>
#include <linux/swapops.h>
#include <linux/shmem_fs.h>

#include <asm/tlb.h>
#include <asm/pgalloc.h>
#include "internal.h"

enum scan_result {
	SCAN_FAIL,
	SCAN_SUCCEED,
	SCAN_PMD_NULL,
	SCAN_EXCEED_NONE_PTE,
	SCAN_PTE_NON_PRESENT,
	SCAN_PAGE_RO,
	SCAN_LACK_REFERENCED_PAGE,
	SCAN_PAGE_NULL,
	SCAN_SCAN_ABORT,
	SCAN_PAGE_COUNT,
	SCAN_PAGE_LRU,
	SCAN_PAGE_LOCK,
	SCAN_PAGE_ANON,
	SCAN_PAGE_COMPOUND,
	SCAN_ANY_PROCESS,
	SCAN_VMA_NULL,
	SCAN_VMA_CHECK,
	SCAN_ADDRESS_RANGE,
	SCAN_SWAP_CACHE_PAGE,
	SCAN_DEL_PAGE_LRU,
	SCAN_ALLOC_HUGE_PAGE_FAIL,
	SCAN_CGROUP_CHARGE_FAIL,
	SCAN_EXCEED_SWAP_PTE,
	SCAN_TRUNCATED,
};

#define CREATE_TRACE_POINTS
#include <trace/events/huge_memory.h>

/* default scan 8*512 pte (or vmas) every 30 second */
static unsigned int khugepaged_pages_to_scan __read_mostly;
static unsigned int khugepaged_pages_collapsed;
static unsigned int khugepaged_full_scans;
static unsigned int khugepaged_scan_sleep_millisecs __read_mostly = 10000;
/* during fragmentation poll the hugepage allocator once every minute */
static unsigned int khugepaged_alloc_sleep_millisecs __read_mostly = 60000;
static unsigned long khugepaged_sleep_expire;
static DEFINE_SPINLOCK(khugepaged_mm_lock);
static DECLARE_WAIT_QUEUE_HEAD(khugepaged_wait);
/*
 * default collapse hugepages if there is at least one pte mapped like
 * it would have happened if the vma was large enough during page
 * fault.
 */
static unsigned int khugepaged_max_ptes_none __read_mostly;
static unsigned int khugepaged_max_ptes_swap __read_mostly;

#define MM_SLOTS_HASH_BITS 10
static __read_mostly DEFINE_HASHTABLE(mm_slots_hash, MM_SLOTS_HASH_BITS);

static struct kmem_cache *mm_slot_cache __read_mostly;

/**
 * struct mm_slot - hash lookup from mm to mm_slot
 * @hash: hash collision list
 * @mm_node: khugepaged scan list headed in khugepaged_scan.mm_head
 * @mm: the mm that this information is valid for
 */
struct mm_slot {
	struct hlist_node hash;
	struct list_head mm_node;
	struct mm_struct *mm;
};

/**
 * struct khugepaged_scan - cursor for scanning
 * @mm_head: the head of the mm list to scan
 * @mm_slot: the current mm_slot we are scanning
 * @address: the next address inside that to be scanned
 *
 * There is only the one khugepaged_scan instance of this cursor structure.
 */
struct khugepaged_scan {
	struct list_head mm_head;
	struct mm_slot *mm_slot;
	unsigned long address;
};

static struct khugepaged_scan khugepaged_scan = {
	.mm_head = LIST_HEAD_INIT(khugepaged_scan.mm_head),
};

#ifdef CONFIG_SYSFS
static ssize_t scan_sleep_millisecs_show(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	return sprintf(buf, "%u\n", khugepaged_scan_sleep_millisecs);
}

static ssize_t scan_sleep_millisecs_store(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	unsigned long msecs;
	int err;

	err = kstrtoul(buf, 10, &msecs);
	if (err || msecs > UINT_MAX)
		return -EINVAL;

	khugepaged_scan_sleep_millisecs = msecs;
	khugepaged_sleep_expire = 0;
	wake_up_interruptible(&khugepaged_wait);

	return count;
}
static struct kobj_attribute scan_sleep_millisecs_attr =
	__ATTR(scan_sleep_millisecs, 0644, scan_sleep_millisecs_show,
	       scan_sleep_millisecs_store);

static ssize_t alloc_sleep_millisecs_show(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  char *buf)
{
	return sprintf(buf, "%u\n", khugepaged_alloc_sleep_millisecs);
}

static ssize_t alloc_sleep_millisecs_store(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	unsigned long msecs;
	int err;

	err = kstrtoul(buf, 10, &msecs);
	if (err || msecs > UINT_MAX)
		return -EINVAL;

	khugepaged_alloc_sleep_millisecs = msecs;
	khugepaged_sleep_expire = 0;
	wake_up_interruptible(&khugepaged_wait);

	return count;
}
static struct kobj_attribute alloc_sleep_millisecs_attr =
	__ATTR(alloc_sleep_millisecs, 0644, alloc_sleep_millisecs_show,
	       alloc_sleep_millisecs_store);

static ssize_t pages_to_scan_show(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  char *buf)
{
	return sprintf(buf, "%u\n", khugepaged_pages_to_scan);
}
static ssize_t pages_to_scan_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int err;
	unsigned long pages;

	err = kstrtoul(buf, 10, &pages);
	if (err || !pages || pages > UINT_MAX)
		return -EINVAL;

	khugepaged_pages_to_scan = pages;

	return count;
}
static struct kobj_attribute pages_to_scan_attr =
	__ATTR(pages_to_scan, 0644, pages_to_scan_show,
	       pages_to_scan_store);

static ssize_t pages_collapsed_show(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    char *buf)
{
	return sprintf(buf, "%u\n", khugepaged_pages_collapsed);
}
static struct kobj_attribute pages_collapsed_attr =
	__ATTR_RO(pages_collapsed);

static ssize_t full_scans_show(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       char *buf)
{
	return sprintf(buf, "%u\n", khugepaged_full_scans);
}
static struct kobj_attribute full_scans_attr =
	__ATTR_RO(full_scans);

static ssize_t khugepaged_defrag_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return single_hugepage_flag_show(kobj, attr, buf,
				TRANSPARENT_HUGEPAGE_DEFRAG_KHUGEPAGED_FLAG);
}
static ssize_t khugepaged_defrag_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	return single_hugepage_flag_store(kobj, attr, buf, count,
				 TRANSPARENT_HUGEPAGE_DEFRAG_KHUGEPAGED_FLAG);
}
static struct kobj_attribute khugepaged_defrag_attr =
	__ATTR(defrag, 0644, khugepaged_defrag_show,
	       khugepaged_defrag_store);

/*
 * max_ptes_none controls if khugepaged should collapse hugepages over
 * any unmapped ptes in turn potentially increasing the memory
 * footprint of the vmas. When max_ptes_none is 0 khugepaged will not
 * reduce the available free memory in the system as it
 * runs. Increasing max_ptes_none will instead potentially reduce the
 * free memory in the system during the khugepaged scan.
 */
static ssize_t khugepaged_max_ptes_none_show(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     char *buf)
{
	return sprintf(buf, "%u\n", khugepaged_max_ptes_none);
}
static ssize_t khugepaged_max_ptes_none_store(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      const char *buf, size_t count)
{
	int err;
	unsigned long max_ptes_none;

	err = kstrtoul(buf, 10, &max_ptes_none);
	if (err || max_ptes_none > HPAGE_PMD_NR-1)
		return -EINVAL;

	khugepaged_max_ptes_none = max_ptes_none;

	return count;
}
static struct kobj_attribute khugepaged_max_ptes_none_attr =
	__ATTR(max_ptes_none, 0644, khugepaged_max_ptes_none_show,
	       khugepaged_max_ptes_none_store);

static ssize_t khugepaged_max_ptes_swap_show(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     char *buf)
{
	return sprintf(buf, "%u\n", khugepaged_max_ptes_swap);
}

static ssize_t khugepaged_max_ptes_swap_store(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      const char *buf, size_t count)
{
	int err;
	unsigned long max_ptes_swap;

	err  = kstrtoul(buf, 10, &max_ptes_swap);
	if (err || max_ptes_swap > HPAGE_PMD_NR-1)
		return -EINVAL;

	khugepaged_max_ptes_swap = max_ptes_swap;

	return count;
}

static struct kobj_attribute khugepaged_max_ptes_swap_attr =
	__ATTR(max_ptes_swap, 0644, khugepaged_max_ptes_swap_show,
	       khugepaged_max_ptes_swap_store);

static struct attribute *khugepaged_attr[] = {
	&khugepaged_defrag_attr.attr,
	&khugepaged_max_ptes_none_attr.attr,
	&pages_to_scan_attr.attr,
	&pages_collapsed_attr.attr,
	&full_scans_attr.attr,
	&scan_sleep_millisecs_attr.attr,
	&alloc_sleep_millisecs_attr.attr,
	&khugepaged_max_ptes_swap_attr.attr,
	NULL,
};

struct attribute_group khugepaged_attr_group = {
	.attrs = khugepaged_attr,
	.name = "khugepaged",
};
#endif /* CONFIG_SYSFS */

#define VM_NO_KHUGEPAGED (VM_SPECIAL | VM_HUGETLB)

int hugepage_madvise(struct vm_area_struct *vma,
		     unsigned long *vm_flags, int advice)
{
	switch (advice) {
	case MADV_HUGEPAGE:
#ifdef CONFIG_S390
		/*
		 * qemu blindly sets MADV_HUGEPAGE on all allocations, but s390
		 * can't handle this properly after s390_enable_sie, so we simply
		 * ignore the madvise to prevent qemu from causing a SIGSEGV.
		 */
		if (mm_has_pgste(vma->vm_mm))
			return 0;
#endif
		*vm_flags &= ~VM_NOHUGEPAGE;
		*vm_flags |= VM_HUGEPAGE;
		/*
		 * If the vma become good for khugepaged to scan,
		 * register it here without waiting a page fault that
		 * may not happen any time soon.
		 */
		if (!(*vm_flags & VM_NO_KHUGEPAGED) &&
				khugepaged_enter_vma_merge(vma, *vm_flags))
			return -ENOMEM;
		break;
	case MADV_NOHUGEPAGE:
		*vm_flags &= ~VM_HUGEPAGE;
		*vm_flags |= VM_NOHUGEPAGE;
		/*
		 * Setting VM_NOHUGEPAGE will prevent khugepaged from scanning
		 * this vma even if we leave the mm registered in khugepaged if
		 * it got registered before VM_NOHUGEPAGE was set.
		 */
		break;
	}

	return 0;
}

int __init khugepaged_init(void)
{
	mm_slot_cache = kmem_cache_create("khugepaged_mm_slot",
					  sizeof(struct mm_slot),
					  __alignof__(struct mm_slot), 0, NULL);
	if (!mm_slot_cache)
		return -ENOMEM;

	khugepaged_pages_to_scan = HPAGE_PMD_NR * 8;
	khugepaged_max_ptes_none = HPAGE_PMD_NR - 1;
	khugepaged_max_ptes_swap = HPAGE_PMD_NR / 8;

	return 0;
}

void __init khugepaged_destroy(void)
{
	kmem_cache_destroy(mm_slot_cache);
}

static inline struct mm_slot *alloc_mm_slot(void)
{
	if (!mm_slot_cache)	/* initialization failed */
		return NULL;
	return kmem_cache_zalloc(mm_slot_cache, GFP_KERNEL);
}

static inline void free_mm_slot(struct mm_slot *mm_slot)
{
	kmem_cache_free(mm_slot_cache, mm_slot);
}

static struct mm_slot *get_mm_slot(struct mm_struct *mm)
{
	struct mm_slot *mm_slot;

	hash_for_each_possible(mm_slots_hash, mm_slot, hash, (unsigned long)mm)
		if (mm == mm_slot->mm)
			return mm_slot;

	return NULL;
}

static void insert_to_mm_slots_hash(struct mm_struct *mm,
				    struct mm_slot *mm_slot)
{
	mm_slot->mm = mm;
	hash_add(mm_slots_hash, &mm_slot->hash, (long)mm);
}

static inline int khugepaged_test_exit(struct mm_struct *mm)
{
	return atomic_read(&mm->mm_users) == 0;
}

int __khugepaged_enter(struct mm_struct *mm)
{
	struct mm_slot *mm_slot;
	int wakeup;

	mm_slot = alloc_mm_slot();
	if (!mm_slot)
		return -ENOMEM;

	/* __khugepaged_exit() must not run from under us */
	VM_BUG_ON_MM(khugepaged_test_exit(mm), mm);
	if (unlikely(test_and_set_bit(MMF_VM_HUGEPAGE, &mm->flags))) {
		free_mm_slot(mm_slot);
		return 0;
	}

	spin_lock(&khugepaged_mm_lock);
	insert_to_mm_slots_hash(mm, mm_slot);
	/*
	 * Insert just behind the scanning cursor, to let the area settle
	 * down a little.
	 */
	wakeup = list_empty(&khugepaged_scan.mm_head);
	list_add_tail(&mm_slot->mm_node, &khugepaged_scan.mm_head);
	spin_unlock(&khugepaged_mm_lock);

	mmgrab(mm);
	if (wakeup)
		wake_up_interruptible(&khugepaged_wait); 
    // scan 할 mm 이 없어 khugepaged 가 동작중이지 않았을 경우, 
    // 현재 scan 할 놈 넣어주었으니 깨워줌
    // khugepaged 를 깨워줌

	return 0;
}

int khugepaged_enter_vma_merge(struct vm_area_struct *vma,
			       unsigned long vm_flags)
{
	unsigned long hstart, hend;
	if (!vma->anon_vma)
		/*
		 * Not yet faulted in so we will register later in the
		 * page fault if needed.
		 */
		return 0;
	if (vma->vm_ops || (vm_flags & VM_NO_KHUGEPAGED))
		/* khugepaged not yet working on file or special mappings */
		return 0;
	hstart = (vma->vm_start + ~HPAGE_PMD_MASK) & HPAGE_PMD_MASK;
	hend = vma->vm_end & HPAGE_PMD_MASK;
	if (hstart < hend)
		return khugepaged_enter(vma, vm_flags);
	return 0;
}

void __khugepaged_exit(struct mm_struct *mm)
{
	struct mm_slot *mm_slot;
	int free = 0;

	spin_lock(&khugepaged_mm_lock);
	mm_slot = get_mm_slot(mm);
	if (mm_slot && khugepaged_scan.mm_slot != mm_slot) {
		hash_del(&mm_slot->hash);
		list_del(&mm_slot->mm_node);
		free = 1;
	}
	spin_unlock(&khugepaged_mm_lock);

	if (free) {
		clear_bit(MMF_VM_HUGEPAGE, &mm->flags);
		free_mm_slot(mm_slot);
		mmdrop(mm);
	} else if (mm_slot) {
		/*
		 * This is required to serialize against
		 * khugepaged_test_exit() (which is guaranteed to run
		 * under mmap sem read mode). Stop here (after we
		 * return all pagetables will be destroyed) until
		 * khugepaged has finished working on the pagetables
		 * under the mmap_sem.
		 */
		down_write(&mm->mmap_sem);
		up_write(&mm->mmap_sem);
	}
}

static void release_pte_page(struct page *page)
{
	/* 0 stands for page_is_file_cache(page) == false */
	dec_node_page_state(page, NR_ISOLATED_ANON + 0);
	unlock_page(page);
	putback_lru_page(page);
}

static void release_pte_pages(pte_t *pte, pte_t *_pte)
{
	while (--_pte >= pte) {
		pte_t pteval = *_pte;
		if (!pte_none(pteval) && !is_zero_pfn(pte_pfn(pteval)))
			release_pte_page(pte_page(pteval));
	}
}

static int __collapse_huge_page_isolate(struct vm_area_struct *vma,
					unsigned long address,
					pte_t *pte)
{
	struct page *page = NULL;
	pte_t *_pte;
	int none_or_zero = 0, result = 0, referenced = 0;
	bool writable = false;

	for (_pte = pte; _pte < pte+HPAGE_PMD_NR;
	     _pte++, address += PAGE_SIZE) {
		pte_t pteval = *_pte;
		if (pte_none(pteval) || (pte_present(pteval) &&
				is_zero_pfn(pte_pfn(pteval)))) {
			if (!userfaultfd_armed(vma) &&
			    ++none_or_zero <= khugepaged_max_ptes_none) {
				continue;
			} else {
				result = SCAN_EXCEED_NONE_PTE;
				goto out;
			}
		}
		if (!pte_present(pteval)) {
			result = SCAN_PTE_NON_PRESENT;
			goto out;
		}
		page = vm_normal_page(vma, address, pteval);
		if (unlikely(!page)) {
			result = SCAN_PAGE_NULL;
			goto out;
		}

		VM_BUG_ON_PAGE(PageCompound(page), page);
		VM_BUG_ON_PAGE(!PageAnon(page), page);
		VM_BUG_ON_PAGE(!PageSwapBacked(page), page);

		/*
		 * We can do it before isolate_lru_page because the
		 * page can't be freed from under us. NOTE: PG_lock
		 * is needed to serialize against split_huge_page
		 * when invoked from the VM.
		 */
		if (!trylock_page(page)) {
			result = SCAN_PAGE_LOCK;
			goto out;
		}

		/*
		 * cannot use mapcount: can't collapse if there's a gup pin.
		 * The page must only be referenced by the scanned process
		 * and page swap cache.
		 */
		if (page_count(page) != 1 + !!PageSwapCache(page)) {
			unlock_page(page);
			result = SCAN_PAGE_COUNT;
			goto out;
		}
		if (pte_write(pteval)) {
			writable = true;
		} else {
			if (PageSwapCache(page) &&
			    !reuse_swap_page(page, NULL)) {
				unlock_page(page);
				result = SCAN_SWAP_CACHE_PAGE;
				goto out;
			}
			/*
			 * Page is not in the swap cache. It can be collapsed
			 * into a THP.
			 */
		}

		/*
		 * Isolate the page to avoid collapsing an hugepage
		 * currently in use by the VM.
		 */
		if (isolate_lru_page(page)) {
			unlock_page(page);
			result = SCAN_DEL_PAGE_LRU;
			goto out;
		}
		/* 0 stands for page_is_file_cache(page) == false */
		inc_node_page_state(page, NR_ISOLATED_ANON + 0);
		VM_BUG_ON_PAGE(!PageLocked(page), page);
		VM_BUG_ON_PAGE(PageLRU(page), page);

		/* There should be enough young pte to collapse the page */
		if (pte_young(pteval) ||
		    page_is_young(page) || PageReferenced(page) ||
		    mmu_notifier_test_young(vma->vm_mm, address))
			referenced++;
	}
	if (likely(writable)) {
		if (likely(referenced)) {
			result = SCAN_SUCCEED;
			trace_mm_collapse_huge_page_isolate(page, none_or_zero,
							    referenced, writable, result);
			return 1;
		}
	} else {
		result = SCAN_PAGE_RO;
	}

out:
	release_pte_pages(pte, _pte);
	trace_mm_collapse_huge_page_isolate(page, none_or_zero,
					    referenced, writable, result);
	return 0;
}


// pte : 합쳐질 pte들 
// page : 새롭게 할당한 page 
// vma : 합쳐질 pte 들의 2MB 연속된 vma 
// address : 위의 vma 영역의 vm_start , vm_end 사이의 2MB align 된 영역의 시작 주소
// ptl : page table lock 
//
//
// pte 들 총 512 개 entry 를 새로운 page 로 복사할거임
static void __collapse_huge_page_copy(pte_t *pte, struct page *page,
				      struct vm_area_struct *vma,
				      unsigned long address,
				      spinlock_t *ptl)
{
	pte_t *_pte; 
    // pte 한개씩 총  512 확인할거임
	for (_pte = pte; _pte < pte+HPAGE_PMD_NR; _pte++) {
		pte_t pteval = *_pte; //pte 안에 들어있는 값
		struct page *src_page;

        // pte 에 0이 들어 있다면 즉 해당 virtual address 에 대한 physical address 가 없으면 
        //
        // old page 의 ptte
        //                ------------------                    
        //         va 0   | pte[0] { 0x0  } | ----|           -> 0
        //         va 1   | pte[1] { 0xff } | ----|--|        -> 0 
        //         va 2   | pte[2] { 0x4f } | ----|--|--|     -> 0
        //         ...    | ...             |     |  |  |
        //         va 511 | pte[0] { 0x8f } | ----|--|--|--|  -> 0 
        //                -------------------     |  |  |  |
        // 할당한 huge page 의pte                 |  |  |  |
        //                 ------------------     |  |  |  |          
        //         va 0   | pte[0] { 0x11  } | <---  |  |  |
        //         va 1   | pte[1] { 0x12  } | <------  |  |
        //         va 2   | pte[2] { 0x13  } | <---------  |
        //         ...    | ...              |             |
        //         va 511 | pte[0] { 0x20b } | <------------
        //                -------------------


		if (pte_none(pteval) || is_zero_pfn(pte_pfn(pteval))) { 

            // page 의 memory 영역을 0 으로 clear
            // clear_user_highpage 에서 내부적으로 clear_page(page)  호출 
            //
            // Calloc doesn't do page allocations; it does heap allocation at a user level. 
            // Page allocation is a kernel operation. If, when a request is made for more 
            // memory in user space, there is none on the free list then a page allocation will occur. 
            // Whether you use calloc or malloc won't affect this. Pages are always cleared as a 
            // security precaution before being made available to user space. 
            //
			clear_user_highpage(page, address); 
            //  
            // struct mm_rss_stat {
            //  atomic_long_t count[NR_MM_COUNTERS];
            //  };
            // 여기 counter 들중 MM_ANONPAGES 에 해당하는 counter 를 1 만큼 증가.
            // count 는 MM_FILEPAGES, MM_ANONPAGES, MM_SWAPENTS, MM_SHEMEMPAGES 가 있음
            //
			add_mm_counter(vma->vm_mm, MM_ANONPAGES, 1);
			if (is_zero_pfn(pte_pfn(pteval))) {
				/*
				 * ptl mostly unnecessary.
				 */
				spin_lock(ptl);
				/*
				 * paravirt calls inside pte_clear here are
				 * superfluous.
				 */ 
                // _pte 의 내용 0 으로 clear
				pte_clear(vma->vm_mm, address, _pte);
				spin_unlock(ptl);
			}
		} else {
			// 복사할 page 를 찾아서
            src_page = pte_page(pteval);
            // src_page 와 page 의 content 를 896MB~1G 영역에 임시 mapping 하여 src_page 의 내용을 
            // page 로 copy 
			copy_user_highpage(page, src_page, address, vma);
			VM_BUG_ON_PAGE(page_mapcount(src_page) != 1, src_page);
			release_pte_page(src_page);
			/*
			 * ptl mostly unnecessary, but preempt has to
			 * be disabled to update the per-cpu stats
			 * inside page_remove_rmap().
			 */
			spin_lock(ptl);
			/*
			 * paravirt calls inside pte_clear here are
			 * superfluous.
			 */
            // pte 내용 0 으로 clear
			pte_clear(vma->vm_mm, address, _pte); 

            // 
            // reverse mapping ? 
            // 
            //             // 
            //          process 1                          process 2
            //          page table 1                       page table 2
            //         ----------------------                ---------------------   
            //         | pte[0] { 0x4ef3 }  |                | pte[0] { 0xe6f3 } |
            //         | pte[1] { 0x204a }  |-------- -------| pte[1] { 0x204a } |
            //         | pte[2] { 0x8fe3 }  |        |       | pte[2] { 0xeff2 } |
            //         | ...                |        |       | ...               |
            //         | pte[0] { 0xf8f1 }  |        |       | pte[0] { 0x6ef1 } | 
            //         ----------------------        |       ---------------------
            //                                       |
            //                                       |
            // physical memory                       |
            //                                       |
            //                                       *
            // ---------------------------------------------------------------------------------------
            // |    aaaaaaaaa|aaaaaaaaaaa|aaaaaaaaaaa|aaaaaaaaaaa|aaaaaaa     |       ... |           |
            // 0            0x4A        0x104a      0x204a      0x304a       0x404a   ...
		    //     
            //  priority search tree ?
            //      시스템 내의 메모리가 부족할 때 프로세스가 사용하는 메모리 페이지를 해지하기 위해 필요한데
            //      공유 매핑의 경우 하나의 페이지를 여러 프로세스에서 (혹은 한 프로세스 내에서도 여러 위치에서)
            //      참조하고 있을 수 있기 때문에 해당 페이지를 해지하려면 그 페이지를 참조하는 모든 곳의
            //      페이지 테이블을 찾아서 더 이상 페이지가 존재하지 않는다고 표시해야 하기 때문에 매핑된 모든 
            //      페이지에 대해 별도의 자료 구조를 두는 대신 해당 페이지를 매핑하는 메모리 구역(vm_area_struct) 
            //      별로 관리하는 방법을 사용하는데이 때 사용되는 것이 바로 PST이다.
            //
            //
            // filebacked page 에서는 reverses mapping 이 address_space 에서 i_mmap 에 표현되고
            // anonymous page 에서는 address_space 대신 쓰이는 anon_vma 의 root 를 통해 표현
            
            page_remove_rmap(src_page, false);
			spin_unlock(ptl);
            // page free 수행
			free_page_and_swap_cache(src_page);
		}

		address += PAGE_SIZE;
		page++;
	}
}

static void khugepaged_alloc_sleep(void)
{
	DEFINE_WAIT(wait);

	add_wait_queue(&khugepaged_wait, &wait);
	freezable_schedule_timeout_interruptible(
		msecs_to_jiffies(khugepaged_alloc_sleep_millisecs));
	remove_wait_queue(&khugepaged_wait, &wait);
}

static int khugepaged_node_load[MAX_NUMNODES];

static bool khugepaged_scan_abort(int nid)
{
	int i;

	/*
	 * If node_reclaim_mode is disabled, then no extra effort is made to
	 * allocate memory locally.
	 */
    // NUMA 가 아니라면 바로 return 
    if (!node_reclaim_mode)
		return false;

	/* If there is a count for this node already, it must be acceptable */
    // NUMA 일 경우, 현재 node id 에 해당하는 load 가 이미 있다면 그 node 는 
    // huge page collapse 를 위한 page reclaim 이 가능한 node 이므로 함수 return    
    if (khugepaged_node_load[nid])
		return false;

    // huge page collapse 시, 어느 node 에서 결국에 어느 node 로 callapse 를 수행하게 될 지 모름 
    // 따라서 load 가 기록된 부분은 즉 hstart ~ hend 사이의 page 가 존재하는 물리 메모리의 node 가 
    // 현재 node 와의 거리가 RECLAIM_DISTANCE 보다 작은지 검사 크다면 collapse 후 보는 이득보다 합치는데 
    // 비용이 많이 들 수 있으므로 true return
	for (i = 0; i < MAX_NUMNODES; i++) {
		if (!khugepaged_node_load[i])
			continue;
		if (node_distance(nid, i) > RECLAIM_DISTANCE)
			return true;
	}
	return false;
}

/* Defrag for khugepaged will enter direct reclaim/compaction if necessary */
static inline gfp_t alloc_hugepage_khugepaged_gfpmask(void)
{
	return khugepaged_defrag() ? GFP_TRANSHUGE : GFP_TRANSHUGE_LIGHT;
}

#ifdef CONFIG_NUMA
static int khugepaged_find_target_node(void)
{
	static int last_khugepaged_target_node = NUMA_NO_NODE;
	int nid, target_node = 0, max_value = 0;

	/* find first node with max normal pages hit */ 
    // khugepaged_node_load 를 통해 지금까지 기록한 hstart 부터 hend 까지의 영역 중 
    // 2MB virtual address 1개에서의 가장 많은 페이지가 속한 node 를 구함.
	for (nid = 0; nid < MAX_NUMNODES; nid++) // MAX_NUMNODES 는 1 << 6 로 32 임.
		if (khugepaged_node_load[nid] > max_value) {
			max_value = khugepaged_node_load[nid]; // node id 에 해당하는 
			target_node = nid;
		}

	/* do some balance if several nodes have the same hit record */
    // 현재 hugepage 할당하기로 결정한 node number 가 가장 마지막에 할당한 node 번호보다 작다면  
    //
    //            ------------------                    khugepaged_node_load
    //            | pte[0] { 0xf3 } | -> node 0         | node 0 |  160 <- target_node
    //            | pte[1] { 0x4a } | -> node 0         | node 1 |  32 
    //            | pte[2] { 0x4a } | -> node 2         | node 2 |  160 <- last_khugepaged_target_node 라면 
    //            | ...             |                   | node 3 |  160 
    //            | pte[0] { 0x8f } | -> node 1
    //            ------------------- 
    //
    //              max_value : 160 
    //              target_node : 0
    //

    // 현재 target_node 가 그전 collapse 하여 hugepage 를 할당한 node 번호보다 작거나 같다면
	if (target_node <= last_khugepaged_target_node)
		for (nid = last_khugepaged_target_node + 1; nid < MAX_NUMNODES;
				nid++)
			if (max_value == khugepaged_node_load[nid]) { 
                // target_node 를 0 에서 3 으로 변경.
                // 현재의 target 과 값이 같은 그 전의 할당한 node 번호 다음번호들을 찾아 
                // target_node 를 변경한다?...
				target_node = nid;
				break;
			}

	last_khugepaged_target_node = target_node;
	return target_node;
}

static bool khugepaged_prealloc_page(struct page **hpage, bool *wait)
{
    // hpage 가 invalid pointer address 라면 첫번째 는 봐주는듯. 
    // 그냥 wait 를 false 로 만들고, 
    // hpage 주소 null 로 초기화 및 휴면상태 들어감. 
    //
    // 생성한 hpage 가 invalid pointer address 가 아니고 현재 *hpage 가 null 이 아니면,
    // prealloc 이 되어 있는 상태임. put_page 를 통해 *hpage 를 release 해줌. 
    //  - lru list 에서 제거. 
    //  - memory allocator 에게 page 반환.
    // 
    // INFO 
    //  - THP 에서 collapse candidate 를 찾기 전, candidate 를 복사할 THP 를 미리 
    //    preallocate 하는 과정이 있었음 하지만, 이건 !NUMA 에서는 어느 node 에 할당하는게 
    //    좋을지 결정되지 않은 상태에서 미리 pre alloc 하는 것은 안좋으므로 patch 됨.    
    //
    //  - 이건 CONFIG_NUMA 일때의 함수임. !NUMA 일때는, 816 번째줄 이후쯤에 같은 이름의 함수 있음.
    //    !NUMA 일때는 여기서 2M 크기의 hugepage 를 미리 allocate 함. 
    //
	if (IS_ERR(*hpage)) {
		if (!*wait)
			return false;

		*wait = false;
		*hpage = NULL;
		khugepaged_alloc_sleep();
	} else if (*hpage) {
		put_page(*hpage);
		*hpage = NULL;
	}

	return true;
}

static struct page *
khugepaged_alloc_page(struct page **hpage, gfp_t gfp, int node)
{
	VM_BUG_ON_PAGE(*hpage, *hpage);

    // TODO
	*hpage = __alloc_pages_node(node, gfp, HPAGE_PMD_ORDER);
	if (unlikely(!*hpage)) {
		count_vm_event(THP_COLLAPSE_ALLOC_FAILED);
		*hpage = ERR_PTR(-ENOMEM);
		return NULL;
	}

    // hpage 는 일반 page 와 다른 구조를 가짐. 
    // 이를 위해 아래와 같은 설정 해줌.
    // compound page :
    //      page[0]   (head page) 
    //      page[1]   (tail page) - descructor function 설정
    //      page[2]   (tail page) - deffered list 를 mapping 위치에 설정
    //      ...
    //      page[511] (tail page) 
    //
	prep_transhuge_page(*hpage); 
    // sys 에 log 출력을 위한 것
	count_vm_event(THP_COLLAPSE_ALLOC);
	return *hpage;
}
#else
static int khugepaged_find_target_node(void)
{
	return 0;
}

static inline struct page *alloc_khugepaged_hugepage(void)
{
	struct page *page;

	page = alloc_pages(alloc_hugepage_khugepaged_gfpmask(),
			   HPAGE_PMD_ORDER);
	if (page)
		prep_transhuge_page(page);
	return page;
}

static struct page *khugepaged_alloc_hugepage(bool *wait)
{
	struct page *hpage;

	do {
		hpage = alloc_khugepaged_hugepage();
		if (!hpage) {
			count_vm_event(THP_COLLAPSE_ALLOC_FAILED);
			if (!*wait)
				return NULL;

			*wait = false;
			khugepaged_alloc_sleep();
		} else
			count_vm_event(THP_COLLAPSE_ALLOC);
	} while (unlikely(!hpage) && likely(khugepaged_enabled()));

	return hpage;
}

static bool khugepaged_prealloc_page(struct page **hpage, bool *wait)
{
	if (!*hpage)
		*hpage = khugepaged_alloc_hugepage(wait);

	if (unlikely(!*hpage))
		return false;

	return true;
}

static struct page *
khugepaged_alloc_page(struct page **hpage, gfp_t gfp, int node)
{
	VM_BUG_ON(!*hpage);

	return  *hpage;
}
#endif

static bool hugepage_vma_check(struct vm_area_struct *vma)
{
	if ((!(vma->vm_flags & VM_HUGEPAGE) && !khugepaged_always()) ||
	    (vma->vm_flags & VM_NOHUGEPAGE))
		return false;
	if (shmem_file(vma->vm_file)) {
		if (!IS_ENABLED(CONFIG_TRANSPARENT_HUGE_PAGECACHE))
			return false;
		return IS_ALIGNED((vma->vm_start >> PAGE_SHIFT) - vma->vm_pgoff,
				HPAGE_PMD_NR);
	}
	if (!vma->anon_vma || vma->vm_ops)
		return false;
	if (is_vma_temporary_stack(vma))
		return false;
	return !(vma->vm_flags & VM_NO_KHUGEPAGED);
}

/*
 * If mmap_sem temporarily dropped, revalidate vma
 * before taking mmap_sem.
 * Return 0 if succeeds, otherwise return none-zero
 * value (scan code).
 */

//      vma_area_struct                                                               vm_area_struct                                vm_area_struct
// ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// |    aaaaaaaaa|aaaaaaaaaaa|aaaaaaaaaaa|aaaaaaaaaaa|aaaaaaa     |           |       aaaa|aaaaaa     |           |           |     aaaaaa|aaaaaaaaaaa|aaa        |           |
// 0            2M          4M          6M          8M          10M          12M         14M         16M         18M         20M         22M         24M         26M         28M
//      ^                                                   ^                         ^         ^                                   ^                    ^
//      vm_start                                            vm_end                    vm_start  vm_end                              vm_start             vm_end
//               ^                                   ^                                    ^                                               ^           ^
//               hstart                              hend                                 hstart/hend                                     hstart      hend 
//               ^           ^           ^           
//               address     address     address   -> 이 함수에서 설정되어 있을 address 위치 예시들


static int hugepage_vma_revalidate(struct mm_struct *mm, unsigned long address,
		struct vm_area_struct **vmap)
{
	struct vm_area_struct *vma;
	unsigned long hstart, hend;

	if (unlikely(khugepaged_test_exit(mm)))
		return SCAN_ANY_PROCESS;

    // 여기서는 address 가 huge page 의 hend까지 왔을듯 하지만 어차피 2MB vma 찾을 때 hstart 는 올림, hend 는 내림
    // 으로 찾았으므로 address < vma->vm_end 인 경우일 경우가 대부분임 따라서 vma 찾을 수 있음
	*vmap = vma = find_vma(mm, address);
	if (!vma)
		return SCAN_VMA_NULL;

	hstart = (vma->vm_start + ~HPAGE_PMD_MASK) & HPAGE_PMD_MASK;
	hend = vma->vm_end & HPAGE_PMD_MASK; 
    // 이경우엔 range error
	if (address < hstart || address + HPAGE_PMD_SIZE > hend)
		return SCAN_ADDRESS_RANGE;
	if (!hugepage_vma_check(vma))
		return SCAN_VMA_CHECK;
	return 0;
}

/*
 * Bring missing pages in from swap, to complete THP collapse.
 * Only done if khugepaged_scan_pmd believes it is worthwhile.
 *
 * Called and returns without pte mapped or spinlocks held,
 * but with mmap_sem held to protect against vma changes.
 */

static bool __collapse_huge_page_swapin(struct mm_struct *mm,
					struct vm_area_struct *vma,
					unsigned long address, pmd_t *pmd,
					int referenced)
{
	int swapped_in = 0, ret = 0; 
    /*
     * vm_fault is filled by the the pagefault handler and passed to the vma's
     * ->fault function. The vma's ->fault is responsible for returning a bitmask
     * of VM_FAULT_xxx flags that give details about how the fault was handled.
    */

	struct vm_fault vmf = {
		.vma = vma, // target vma
		.address = address, // faulting virtual address
		.flags = FAULT_FLAG_ALLOW_RETRY, 
		.pmd = pmd, // pointer to pmd entry matching the 'address'
		.pgoff = linear_page_index(vma, address), //logical page offse based on vma 
        // .orig_pte : value of PTE at the time of fault  
        //	pte_t *pte : Pointer to pte entry matching the 'address'. NULL if the page table hasn't been allocated.
	};

	/* we only decide to swapin, if there is enough young ptes */
    // 512 / 2 보다 많은 pte young 이 있을시 swapin 을 수행한다. 
    if (referenced < HPAGE_PMD_NR/2) {
		trace_mm_collapse_huge_page_swapin(mm, swapped_in, referenced, 0);
		return false;
	}
	vmf.pte = pte_offset_map(pmd, address);
	for (; vmf.address < address + HPAGE_PMD_NR*PAGE_SIZE; // 512 * 4K -> 2 MB
			vmf.pte++, vmf.address += PAGE_SIZE) {
		vmf.orig_pte = *vmf.pte;
		if (!is_swap_pte(vmf.orig_pte))
			continue;
		swapped_in++;
		ret = do_swap_page(&vmf);

		/* do_swap_page returns VM_FAULT_RETRY with released mmap_sem */
		if (ret & VM_FAULT_RETRY) {
			down_read(&mm->mmap_sem);
			if (hugepage_vma_revalidate(mm, address, &vmf.vma)) {
				/* vma is no longer available, don't continue to swapin */
				trace_mm_collapse_huge_page_swapin(mm, swapped_in, referenced, 0);
				return false;
			}
			/* check if the pmd is still valid */
			if (mm_find_pmd(mm, address) != pmd)
				return false;
		}
		if (ret & VM_FAULT_ERROR) {
			trace_mm_collapse_huge_page_swapin(mm, swapped_in, referenced, 0);
			return false;
		}
		/* pte is unmapped now, we need to map it */
		vmf.pte = pte_offset_map(pmd, vmf.address);
	}
	vmf.pte--;
	pte_unmap(vmf.pte);
	trace_mm_collapse_huge_page_swapin(mm, swapped_in, referenced, 1);
	return true;
}

static void collapse_huge_page(struct mm_struct *mm,
				   unsigned long address,
				   struct page **hpage,
				   int node, int referenced)
{
	pmd_t *pmd, _pmd;
	pte_t *pte;
	pgtable_t pgtable;
	struct page *new_page;
	spinlock_t *pmd_ptl, *pte_ptl;
	int isolated = 0, result = 0;
	struct mem_cgroup *memcg;
	struct vm_area_struct *vma;
	unsigned long mmun_start;	/* For mmu_notifiers */
	unsigned long mmun_end;		/* For mmu_notifiers */
	gfp_t gfp;

	VM_BUG_ON(address & ~HPAGE_PMD_MASK);

	/* Only allocate from the target node */
    // hugepage 할당을 위한 gfp flag 설정.
    //
    //  __GFP_THISNODE 
    //  : 지정된 노드에서만 할당 허용. 
    //
    // defrag 정책에 따라 아래와 같은 두가지 flag 중 하나 선택
    //
    // GFP_TRANSHUGE - (GFP_TRANSHUGE_LIGHT | __GFP_DIRECT_RECLAIM)
    //  : compound allocation, memory 부족하면 fail 하며   
    //    kswapd/kcompactd 안깨움, khugepaged 에서 사용
    // GFP_TRANSHUGE_LIGHT 
    //  : compound allocation, memory 부족하면 fail 하며 
    //    kswapd/kcompactd 안깨움, reclaim/compaction 수행 안함, page fault path 에서 사용되고
    //

	gfp = alloc_hugepage_khugepaged_gfpmask() | __GFP_THISNODE;

	/*
	 * Before allocating the hugepage, release the mmap_sem read lock.
	 * The allocation can take potentially a long time if it involves
	 * sync compaction, and we do not need to hold the mmap_sem during
	 * that. We will recheck the vma after taking it again in write mode.
	 */
	up_read(&mm->mmap_sem);

    // TODO
    //      __alloc_pages_node(node, gfp, HPAGE_PMD_ORDER); 분석
    // 
    // hugepage 할당하고, 이 할당된 compound huge page 를 위한 설정을 해줌.
    new_page = khugepaged_alloc_page(hpage, gfp, node);
	if (!new_page) {
		result = SCAN_ALLOC_HUGE_PAGE_FAIL;
		goto out_nolock;
	}
    // 할당한 huge page 에 mem cgroup 설정
	if (unlikely(mem_cgroup_try_charge(new_page, mm, gfp, &memcg, true))) {
		result = SCAN_CGROUP_CHARGE_FAIL;
		goto out_nolock;
	}

	down_read(&mm->mmap_sem);

    // FIXME
    // vm_start, vm_end 에서 hstart, hend 계산하여 address 에대해 range 요류 체크 수행 
	result = hugepage_vma_revalidate(mm, address, &vma);
	if (result) {
		mem_cgroup_cancel_charge(new_page, memcg, true);
		up_read(&mm->mmap_sem);
		goto out_nolock;
	}

	pmd = mm_find_pmd(mm, address);
	if (!pmd) {
		result = SCAN_PMD_NULL;
		mem_cgroup_cancel_charge(new_page, memcg, true);
		up_read(&mm->mmap_sem);
		goto out_nolock;
	}

	/*
	 * __collapse_huge_page_swapin always returns with mmap_sem locked.
	 * If it fails, we release mmap_sem and jump out_nolock.
	 * Continuing to collapse causes inconsistency.
	 */
    // 현재 virtual 2MB 영역에서 pte flag 가 !pte_none(pte) && !pte_present(pte); 맞으면 
    //  pte_none : entry 비었으면 1, 뭐 있으면 0
    //  pte_present : pte 에 present bit 가 설정되어 있으면 1, 없으면 0
    //

    if (!__collapse_huge_page_swapin(mm, vma, address, pmd, referenced)) {
		mem_cgroup_cancel_charge(new_page, memcg, true);
		up_read(&mm->mmap_sem);
		goto out_nolock;
	}

	up_read(&mm->mmap_sem);
	/*
	 * Prevent all access to pagetables with the exception of
	 * gup_fast later handled by the ptep_clear_flush and the VM
	 * handled by the anon_vma lock + PG_lock.
	 */
	down_write(&mm->mmap_sem);
	result = hugepage_vma_revalidate(mm, address, &vma);
	if (result)
		goto out;
	/* check if the pmd is still valid */
	if (mm_find_pmd(mm, address) != pmd)
		goto out;

	anon_vma_lock_write(vma->anon_vma);

	pte = pte_offset_map(pmd, address);
	pte_ptl = pte_lockptr(mm, pmd);

    //      vma_area_struct                                                               vm_area_struct                                vm_area_struct
    // ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    // |    aaaaaaaaa|aaaaaaaaaaa|aaaaaaaaaaa|aaaaaaaaaaa|aaaaaaa     |           |       aaaa|aaaaaa     |           |           |     aaaaaa|aaaaaaaaaaa|aaa        |           |
    // 0            2M          4M          6M          8M          10M          12M         14M         16M         18M         20M         22M         24M         26M         28M
    //      ^                                                   ^                         ^         ^                                   ^                    ^
    //      vm_start                                            vm_end                    vm_start  vm_end                              vm_start             vm_end
    //               ^                                   ^                                    ^                                               ^           ^
    //               hstart                              hend                                 hstart/hend                                     hstart      hend 
    //               ^           ^           ^           
    //               address     address     address   -> 이 함수에서 설정되어 있을 address 위치 예시들 
    //               mmun_start--mmun_end   
    //                           mmun_start--mmun_end
    //                                       mmun_start--mmun_end


    // mmu_notifier ? 
    // The main purpose of an MMU notifier callback is to tell the interested subsystem that 
    // something has changed with one or more pages; that subsystem should respond by simply 
    // invalidating its own mapping for those pages. The next time a fault occurs on one of 
    // the affected pages, the mapping will be re-established, reflecting the new state of affairs.
	mmun_start = address;
	mmun_end   = address + HPAGE_PMD_SIZE; 

    /* ==================== MMU NOTIFIER START===========================*/
    // tlb flush 를 위해 mmun_start 부터 mmun_end 까지가 map된 상태에서 호출
	mmu_notifier_invalidate_range_start(mm, mmun_start, mmun_end);
	pmd_ptl = pmd_lock(mm, pmd); /* probably unnecessary */
	/*
	 * After this gup_fast can't run anymore. This also removes
	 * any huge TLB entry from the CPU so we won't allow
	 * huge and small TLB entries for the same virtual address
	 * to avoid the risk of CPU bugs in that area.
	 */ 
    // hugepage 를 위해 기존 2MB 영역에 대한 pmd 를 clear 하고 그 주소 가져옴
	_pmd = pmdp_collapse_flush(vma, address, pmd);
	spin_unlock(pmd_ptl);
	mmu_notifier_invalidate_range_end(mm, mmun_start, mmun_end);
    /* ==================== MMU NOTIFIER END===========================*/

	spin_lock(pte_ptl); 
    // copy 할 때 까지 기존 page 들이 없어지지 않도록 2MB 영역이 속하 page 들의  reference 를 증가해 놓음
	isolated = __collapse_huge_page_isolate(vma, address, pte);
	spin_unlock(pte_ptl);

	if (unlikely(!isolated)) {
		pte_unmap(pte);
		spin_lock(pmd_ptl);
		BUG_ON(!pmd_none(*pmd));
		/*
		 * We can only use set_pmd_at when establishing
		 * hugepmds and never for establishing regular pmds that
		 * points to regular pagetables. Use pmd_populate for that
		 */
		pmd_populate(mm, pmd, pmd_pgtable(_pmd));
		spin_unlock(pmd_ptl);
		anon_vma_unlock_write(vma->anon_vma);
		result = SCAN_FAIL;
		goto out;
	}

	/*
	 * All pages are isolated and locked so anon_vma rmap
	 * can't run anymore.
	 */
	anon_vma_unlock_write(vma->anon_vma);

    // 여기서 할당한 hugepage 를 clear 하며 복사 
    // pte 부터 pte[511] 까지의 physical memeory address 에 존재하는 내용들을 new_page 로 복사
	__collapse_huge_page_copy(pte, new_page, vma, address, pte_ptl);
    // kmap 으로 pte가 임시 map 되어 있을수도 있으므로 unmap 해줌.
	pte_unmap(pte);
	__SetPageUptodate(new_page);
	pgtable = pmd_pgtable(_pmd);

    // pmd 생성하는데 _PAGE_PSE 설정하여 hugepage pmd 로 설정함 
	_pmd = mk_huge_pmd(new_page, vma->vm_page_prot);
	_pmd = maybe_pmd_mkwrite(pmd_mkdirty(_pmd), vma);

	/*
	 * spin_lock() below is not the equivalent of smp_wmb(), so
	 * this is needed to avoid the copy_huge_page writes to become
	 * visible after the set_pmd_at() write.
	 */
	smp_wmb();

	spin_lock(pmd_ptl);
	BUG_ON(!pmd_none(*pmd)); 
    // anon reverse map 설정
	page_add_new_anon_rmap(new_page, vma, address, true);
    // cgroup 관련..
	mem_cgroup_commit_charge(new_page, memcg, false, true);
	lru_cache_add_active_or_unevictable(new_page, vma);
	pgtable_trans_huge_deposit(mm, pmd, pgtable);
	set_pmd_at(mm, address, pmd, _pmd);
	update_mmu_cache_pmd(vma, address, pmd);
	spin_unlock(pmd_ptl);

	*hpage = NULL;

	khugepaged_pages_collapsed++;
	result = SCAN_SUCCEED;
out_up_write:
	up_write(&mm->mmap_sem);
out_nolock:
	trace_mm_collapse_huge_page(mm, isolated, result);
	return;
out:
	mem_cgroup_cancel_charge(new_page, memcg, true);
	goto out_up_write;
}

// 현재 process 의 mm 이 가진 여러 vm_area_struct 들 중 vma 에서 address 부터 시작하여 2^21 만큼(2MB) 만큼
// 연속적인 virtual address 영역이 사용되고 있으니 여기를 합쳐서 hpage 로 반환하자.
// 근대 여기 이영역을 hugepage 로 합쳐도 되는지 검사 먼저 하자.
static int khugepaged_scan_pmd(struct mm_struct *mm,
			       struct vm_area_struct *vma,
			       unsigned long address,
			       struct page **hpage)
{
	pmd_t *pmd;
	pte_t *pte, *_pte;
	int ret = 0, none_or_zero = 0, result = 0, referenced = 0;
	struct page *page = NULL;
	unsigned long _address;
	spinlock_t *ptl;
	int node = NUMA_NO_NODE, unmapped = 0;
	bool writable = false;

	VM_BUG_ON(address & ~HPAGE_PMD_MASK); 

    // address 에 해당하는 pmd 가져옴 
    //      typedef struct { pmdval_t pmd;  } pmd_t;
    //
	pmd = mm_find_pmd(mm, address);
	if (!pmd) {
		result = SCAN_PMD_NULL;
		goto out;
	}
    // khugepaged 의 node 별 부하 체크 배열을 0으로 초기화
	memset(khugepaged_node_load, 0, sizeof(khugepaged_node_load)); 

    // pmd table 이 들어있는 page 의 page table lock을 잡고 address 에 해당하는 pte 가져옴 
    //      typedef struct { pteval_t pte;  } pte_t;

	pte = pte_offset_map_lock(mm, pmd, address, &ptl);
    // pte 를 가져옴.
    // ex) 3 level table 일 경우 page 찾는 방법
    //      pgd = pgd_offset(address);  
    //      pmd = pmd_offset(pgd, address);  
    //      pte = *pte_offset_map(pmd, address);  
    //      page = pte_page(pte);
    //
    // ex) 4 level table 일 경우 page 찾는 방법
    //      pgd = pgd_offset(mm, address);
    //      pud = pud_alloc(mm, pgd, address);
    //      pmd = pmd_alloc(mm, pud, address);
    //      pte = pte_offset_map(pmd, address); 
    // 
    // pmd 의주소로부터 address 의 pte index 를 활용해 pte 의 주소를 가져옴.
    // mm 의 page table lock 잡음
    //

    // VIRTUAL MEMORY
    //      vm_area_struct
    // ---------------------------------------------------------------------------------------
    // |    aaaaaaaaa|aaaaaaaaaaa|aaaaaaaaaaa|aaaaaaaaaaa|aaaaaaa     |    ...    |           |
    // 0            2M          4M          6M          8M          10M   ...
    //      ^                                                   ^
    //      vm_start                                            vm_end
    //               ^                                      ^
    //               hstart                                 hend 
    //               ^
    //               address
    //                 ^
    //                 address+PAGE_SIZE

    // pmd_t { pmdval_t pmd } pmd_t;
    //                  2
    //   
    // --------------------------------------------------------------------------------------
    // pte_t[0]   pte_t[1]   pte_t[2]   ...   pte_t[255] |  pte_t[0]  ...
    //
    //
    //
    // PHYSICAL MEMORY
    // ---------------------------------------------------------------------------------------
    // |             |           |           |           |           |    ...    |           |
    // 0            2M          4M          6M          8M          10M   ...
    //      ^                                                   ^
    //      vm_start                                            vm_end
    //               ^                                      ^
    //               hstart                                 hend 
    //               ^
    //               address

    for (_address = address, _pte = pte; _pte < pte+HPAGE_PMD_NR; 
	     _pte++, _address += PAGE_SIZE) { 

        // HPAGE_PMD_NR 까지 즉 page table 의512 개 entry 를 모두 돌때 까지 page table entry 는
        // 1개씩  증가시키고, virtual address 인 _address 는 PAGE_SIZE 만큼 증가하며 순회.
		pte_t pteval = *_pte; 
        // page table entry 가 가지고 있는 
        // struct {pteval_t pte; }pte_t 
        // 
		if (is_swap_pte(pteval)) {
            // 해당 page table entry 가NONE 설정이 아니면서 PRESENT BIT 가 없는 경우 즉 
            // pte entry 는 있지만, 현재 memory 에 올라와 있지 않은 경우. 
            // khugepaged_max_ptes_swap 의 정해져 있는 값인 512/8 값보다 작다면 계속 검사 수행.
			if (++unmapped <= khugepaged_max_ptes_swap) {
				continue;
			} else { 
                // khugepaged_max_ptes_swap 보다 크면 즉 2M VM 영역의 1/8 이상이 swap 되어 있다면,
                // hugepage 를 생성하지 않고 끝냄.
				result = SCAN_EXCEED_SWAP_PTE;
				goto out_unmap;
			}
		}
        // 현재 memory 에 올라와 있지 않은 경우 즉 virtial address 가 physical memory 에 올라와 
        // 있지 않아 disk 로부터 가져와야 할 경우.
		if (pte_none(pteval) || is_zero_pfn(pte_pfn(pteval))) {
            // pte_none 검사를 통해 pteval 이 현재 0이거나 
            //
            // 마찬가지로 khugepaged_max_ptes_none 의 값인 511 보다 작을 경우,계속 hugepage 만들어도
            // 되는지 검사 수행. 
			if (!userfaultfd_armed(vma) &&
			    ++none_or_zero <= khugepaged_max_ptes_none) {
				continue;
			} else {
				result = SCAN_EXCEED_NONE_PTE;
				goto out_unmap;
			}
		}
        // INFO 
        //  뭐임?
		if (!pte_present(pteval)) {
			result = SCAN_PTE_NON_PRESENT;
			goto out_unmap;
		}
        //현재 접근주인 page table entry 가 writable ?
		if (pte_write(pteval))
			writable = true;

        // pteval 에 해당되는 page struct 를 가져옴. 
		page = vm_normal_page(vma, _address, pteval);
		if (unlikely(!page)) {
			result = SCAN_PAGE_NULL;
			goto out_unmap;
		}

		/* TODO: teach khugepaged to collapse THP mapped with pte */

        //
        // Compound Page 인지 검사
        //  head page 인가? -> page flag 에 PG_head 가 있거나 
        //  tail page 인가? -> compound page 가 1 로 설정되어 있거나 
        //
        //  Compound page 라면 현재 vm 에 대해 collapse 할 필요 없음.
        //  INFO 
        //      Compound page 라면 할필요 없음
        if (PageCompound(page)) {
			result = SCAN_PAGE_COMPOUND;
			goto out_unmap;
		}

		/*
		 * Record which node the original page is from and save this
		 * information to khugepaged_node_load[].
		 * Khupaged will allocate hugepage from the node has the max
		 * hit record.
		 */ 
        // 합치려는 virtual address 의 2MB 영역에 해당하느 page 가 어느 node 에 현재 할당되어 있는 page 인지 검사
        // khugepage 내에서 node 부하를 검사하여 max hit record 에 있는 node에 hugepage 할당. 
        
        // ---------------------------------------------------------------------------------------
        // |    aaaaaaaaa|aaaaaaaaaaa|aaaaaaaaaaa|aaaaaaaaaaa|aaaaaaa     |    ...    |           |
        // 0            2M          4M          6M          8M          10M   ...
        //      ^                                                   ^
        //      vm_start                                            vm_end
        //               ^                                      ^
        //               hstart                                 hend 
        //               ^
        //               address
        //                 ^
        //                 address+PAGE_SIZE
        //               |     2M     |
        //               +             + 
        //              +               +  
        //             +                 +
        //            +                   +
        //                0         411    511
        //               |4K|4K|...|4K|...|4K|
        //                          ^
        //                          page 
        //         node   1  3      0
        //
        //              - 총 512 개의 page  
        //              - 현재page 가 속한 node 가 0 이라면 
        //                node 0 - node 1 사이의 거리가 < RECLAIM_DISTANCE ? 
        //                node 0 - node 3 사이의 거리가 < RECLAIM_DISTANCE ?  
        //
        //
        //                node 0 - node 2 여기에 속한 page 없으므로 검사 필요 없음 
        //
        //            ------------------                    khugepaged_node_load
        //            | pte[0] { 0xf3 } | -> node 0         | node 0 | 310
        //            | pte[1] { 0x4a } | -> node 0         | node 1 |  54
        //            | pte[2] { 0x4a } | -> node 2         | node 2 | 
        //            | ...             |                   | node 3 |  47
        //            | pte[0] { 0x8f } | -> node 1
        //            -------------------

		node = page_to_nid(page); 
        // 현재 page 가 소속된 node 가 hstart ~ hend 내의 현재까지 scan 한 address 까지에서 기록된 page 의 node 사이의 
        // 거리가 너무 먼지 검사 
		if (khugepaged_scan_abort(node)) {
			result = SCAN_SCAN_ABORT;
			goto out_unmap;
		}
		khugepaged_node_load[node]++;
        // page 에 PG_lru(LRU flag) 설정되어 있는지 검사 설정되어 있으면 true 반환
        // PG_lru 가 설정되어 있다는 것은 active_list 또는 inactive_list 둘중 하나에 있다는 것임없으면 swap out 된것
        // 다른 이유일 수도..
		if (!PageLRU(page)) {
			result = SCAN_PAGE_LRU;
			goto out_unmap;
		}
        // page 에PG_locked 설정되어 있는지 검사 설정되어 있으면 true 반환
        // PG_locked 가 설정되어 있다는 것은 현재 page가disk io 를 위해 lock 된 것임. disk I/O 시작할 시 해당 page 에 
        // PG_lock 을 설정 하고 disk I/O 끝나면 PG_locked 해제함.
        //
        // 다른 이유일 수도...
		if (PageLocked(page)) {
			result = SCAN_PAGE_LOCK;
			goto out_unmap;
		}
        // page 의 mapping 이 address_space 가 아니라 anon_vma 인지 검사를 위해 low bit 가 PAGE_MAPPPING_ANON 인지 확인 
        // 즉 page 가 anonymous page 인지 검사. 
        // 맞으면 true 아니면 false
		if (!PageAnon(page)) {
			result = SCAN_PAGE_ANON;
			goto out_unmap;
		}

		/*
		 * cannot use mapcount: can't collapse if there's a gup pin.
		 * The page must only be referenced by the scanned process
		 * and page swap cache.
		 */ 
        // 이건 뭐임?
		if (page_count(page) != 1 + !!PageSwapCache(page)) {
			result = SCAN_PAGE_COUNT;
			goto out_unmap;
		}
        // pte 에 accessed flag 설정됨?
        // 
		if (pte_young(pteval) ||
		    page_is_young(page) || PageReferenced(page) ||
		    mmu_notifier_test_young(vma->vm_mm, address))
			referenced++;
	}
    // 512 개 검사 후,
    // 여기까지 와야지만 collapse 가능함. 
	if (writable) {
		if (referenced) {
			result = SCAN_SUCCEED;
			ret = 1;
		} else {
			result = SCAN_LACK_REFERENCED_PAGE;
		}
	} else {
		result = SCAN_PAGE_RO;
	}
out_unmap: 
	pte_unmap_unlock(pte, ptl);
	if (ret) {
        // 현재config 가 UMA 라면 바로 0 return 하고(현재 NODE 에서 할당받고) NUMA 라면 할당받을 node 를 정함.  
		node = khugepaged_find_target_node();
		/* collapse_huge_page will return with the mmap_sem released */
		collapse_huge_page(mm, address, hpage, node, referenced);
	}
out:
	trace_mm_khugepaged_scan_pmd(mm, page, writable, referenced,
				     none_or_zero, result, unmapped);
	return ret;
}

static void collect_mm_slot(struct mm_slot *mm_slot)
{
	struct mm_struct *mm = mm_slot->mm;

	VM_BUG_ON(NR_CPUS != 1 && !spin_is_locked(&khugepaged_mm_lock));

	if (khugepaged_test_exit(mm)) {
		/* free mm_slot */
		hash_del(&mm_slot->hash);
		list_del(&mm_slot->mm_node);

		/*
		 * Not strictly needed because the mm exited already.
		 *
		 * clear_bit(MMF_VM_HUGEPAGE, &mm->flags);
		 */

		/* khugepaged_mm_lock actually not necessary for the below */
		free_mm_slot(mm_slot);
		mmdrop(mm);
	}
}

#if defined(CONFIG_SHMEM) && defined(CONFIG_TRANSPARENT_HUGE_PAGECACHE)
static void retract_page_tables(struct address_space *mapping, pgoff_t pgoff)
{
	struct vm_area_struct *vma;
	unsigned long addr;
	pmd_t *pmd, _pmd;

	i_mmap_lock_write(mapping);
	vma_interval_tree_foreach(vma, &mapping->i_mmap, pgoff, pgoff) {
		/* probably overkill */
		if (vma->anon_vma)
			continue;
		addr = vma->vm_start + ((pgoff - vma->vm_pgoff) << PAGE_SHIFT);
		if (addr & ~HPAGE_PMD_MASK)
			continue;
		if (vma->vm_end < addr + HPAGE_PMD_SIZE)
			continue;
		pmd = mm_find_pmd(vma->vm_mm, addr);
		if (!pmd)
			continue;
		/*
		 * We need exclusive mmap_sem to retract page table.
		 * If trylock fails we would end up with pte-mapped THP after
		 * re-fault. Not ideal, but it's more important to not disturb
		 * the system too much.
		 */
		if (down_write_trylock(&vma->vm_mm->mmap_sem)) {
			spinlock_t *ptl = pmd_lock(vma->vm_mm, pmd);
			/* assume page table is clear */
			_pmd = pmdp_collapse_flush(vma, addr, pmd);
			spin_unlock(ptl);
			up_write(&vma->vm_mm->mmap_sem);
			atomic_long_dec(&vma->vm_mm->nr_ptes);
			pte_free(vma->vm_mm, pmd_pgtable(_pmd));
		}
	}
	i_mmap_unlock_write(mapping);
}

/**
 * collapse_shmem - collapse small tmpfs/shmem pages into huge one.
 *
 * Basic scheme is simple, details are more complex:
 *  - allocate and freeze a new huge page;
 *  - scan over radix tree replacing old pages the new one
 *    + swap in pages if necessary;
 *    + fill in gaps;
 *    + keep old pages around in case if rollback is required;
 *  - if replacing succeed:
 *    + copy data over;
 *    + free old pages;
 *    + unfreeze huge page;
 *  - if replacing failed;
 *    + put all pages back and unfreeze them;
 *    + restore gaps in the radix-tree;
 *    + free huge page;
 */
static void collapse_shmem(struct mm_struct *mm,
		struct address_space *mapping, pgoff_t start,
		struct page **hpage, int node)
{
	gfp_t gfp;
	struct page *page, *new_page, *tmp;
	struct mem_cgroup *memcg;
	pgoff_t index, end = start + HPAGE_PMD_NR;
	LIST_HEAD(pagelist);
	struct radix_tree_iter iter;
	void **slot;
	int nr_none = 0, result = SCAN_SUCCEED;

	VM_BUG_ON(start & (HPAGE_PMD_NR - 1));

	/* Only allocate from the target node */
	gfp = alloc_hugepage_khugepaged_gfpmask() | __GFP_THISNODE;

	new_page = khugepaged_alloc_page(hpage, gfp, node);
	if (!new_page) {
		result = SCAN_ALLOC_HUGE_PAGE_FAIL;
		goto out;
	}

	if (unlikely(mem_cgroup_try_charge(new_page, mm, gfp, &memcg, true))) {
		result = SCAN_CGROUP_CHARGE_FAIL;
		goto out;
	}

	new_page->index = start;
	new_page->mapping = mapping;
	__SetPageSwapBacked(new_page);
	__SetPageLocked(new_page);
	BUG_ON(!page_ref_freeze(new_page, 1));


	/*
	 * At this point the new_page is 'frozen' (page_count() is zero), locked
	 * and not up-to-date. It's safe to insert it into radix tree, because
	 * nobody would be able to map it or use it in other way until we
	 * unfreeze it.
	 */

	index = start;
	spin_lock_irq(&mapping->tree_lock);
	radix_tree_for_each_slot(slot, &mapping->page_tree, &iter, start) {
		int n = min(iter.index, end) - index;

		/*
		 * Handle holes in the radix tree: charge it from shmem and
		 * insert relevant subpage of new_page into the radix-tree.
		 */
		if (n && !shmem_charge(mapping->host, n)) {
			result = SCAN_FAIL;
			break;
		}
		nr_none += n;
		for (; index < min(iter.index, end); index++) {
			radix_tree_insert(&mapping->page_tree, index,
					new_page + (index % HPAGE_PMD_NR));
		}

		/* We are done. */
		if (index >= end)
			break;

		page = radix_tree_deref_slot_protected(slot,
				&mapping->tree_lock);
		if (radix_tree_exceptional_entry(page) || !PageUptodate(page)) {
			spin_unlock_irq(&mapping->tree_lock);
			/* swap in or instantiate fallocated page */
			if (shmem_getpage(mapping->host, index, &page,
						SGP_NOHUGE)) {
				result = SCAN_FAIL;
				goto tree_unlocked;
			}
			spin_lock_irq(&mapping->tree_lock);
		} else if (trylock_page(page)) {
			get_page(page);
		} else {
			result = SCAN_PAGE_LOCK;
			break;
		}

		/*
		 * The page must be locked, so we can drop the tree_lock
		 * without racing with truncate.
		 */
		VM_BUG_ON_PAGE(!PageLocked(page), page);
		VM_BUG_ON_PAGE(!PageUptodate(page), page);
		VM_BUG_ON_PAGE(PageTransCompound(page), page);

		if (page_mapping(page) != mapping) {
			result = SCAN_TRUNCATED;
			goto out_unlock;
		}
		spin_unlock_irq(&mapping->tree_lock);

		if (isolate_lru_page(page)) {
			result = SCAN_DEL_PAGE_LRU;
			goto out_isolate_failed;
		}

		if (page_mapped(page))
			unmap_mapping_range(mapping, index << PAGE_SHIFT,
					PAGE_SIZE, 0);

		spin_lock_irq(&mapping->tree_lock);

		slot = radix_tree_lookup_slot(&mapping->page_tree, index);
		VM_BUG_ON_PAGE(page != radix_tree_deref_slot_protected(slot,
					&mapping->tree_lock), page);
		VM_BUG_ON_PAGE(page_mapped(page), page);

		/*
		 * The page is expected to have page_count() == 3:
		 *  - we hold a pin on it;
		 *  - one reference from radix tree;
		 *  - one from isolate_lru_page;
		 */
		if (!page_ref_freeze(page, 3)) {
			result = SCAN_PAGE_COUNT;
			goto out_lru;
		}

		/*
		 * Add the page to the list to be able to undo the collapse if
		 * something go wrong.
		 */
		list_add_tail(&page->lru, &pagelist);

		/* Finally, replace with the new page. */
		radix_tree_replace_slot(&mapping->page_tree, slot,
				new_page + (index % HPAGE_PMD_NR));

		slot = radix_tree_iter_resume(slot, &iter);
		index++;
		continue;
out_lru:
		spin_unlock_irq(&mapping->tree_lock);
		putback_lru_page(page);
out_isolate_failed:
		unlock_page(page);
		put_page(page);
		goto tree_unlocked;
out_unlock:
		unlock_page(page);
		put_page(page);
		break;
	}

	/*
	 * Handle hole in radix tree at the end of the range.
	 * This code only triggers if there's nothing in radix tree
	 * beyond 'end'.
	 */
	if (result == SCAN_SUCCEED && index < end) {
		int n = end - index;

		if (!shmem_charge(mapping->host, n)) {
			result = SCAN_FAIL;
			goto tree_locked;
		}

		for (; index < end; index++) {
			radix_tree_insert(&mapping->page_tree, index,
					new_page + (index % HPAGE_PMD_NR));
		}
		nr_none += n;
	}

tree_locked:
	spin_unlock_irq(&mapping->tree_lock);
tree_unlocked:

	if (result == SCAN_SUCCEED) {
		unsigned long flags;
		struct zone *zone = page_zone(new_page);

		/*
		 * Replacing old pages with new one has succeed, now we need to
		 * copy the content and free old pages.
		 */
		list_for_each_entry_safe(page, tmp, &pagelist, lru) {
			copy_highpage(new_page + (page->index % HPAGE_PMD_NR),
					page);
			list_del(&page->lru);
			unlock_page(page);
			page_ref_unfreeze(page, 1);
			page->mapping = NULL;
			ClearPageActive(page);
			ClearPageUnevictable(page);
			put_page(page);
		}

		local_irq_save(flags);
		__inc_node_page_state(new_page, NR_SHMEM_THPS);
		if (nr_none) {
			__mod_node_page_state(zone->zone_pgdat, NR_FILE_PAGES, nr_none);
			__mod_node_page_state(zone->zone_pgdat, NR_SHMEM, nr_none);
		}
		local_irq_restore(flags);

		/*
		 * Remove pte page tables, so we can re-faulti
		 * the page as huge.
		 */
		retract_page_tables(mapping, start);

		/* Everything is ready, let's unfreeze the new_page */
		set_page_dirty(new_page);
		SetPageUptodate(new_page);
		page_ref_unfreeze(new_page, HPAGE_PMD_NR);
		mem_cgroup_commit_charge(new_page, memcg, false, true);
		lru_cache_add_anon(new_page);
		unlock_page(new_page);

		*hpage = NULL;
	} else {
		/* Something went wrong: rollback changes to the radix-tree */
		shmem_uncharge(mapping->host, nr_none);
		spin_lock_irq(&mapping->tree_lock);
		radix_tree_for_each_slot(slot, &mapping->page_tree, &iter,
				start) {
			if (iter.index >= end)
				break;
			page = list_first_entry_or_null(&pagelist,
					struct page, lru);
			if (!page || iter.index < page->index) {
				if (!nr_none)
					break;
				nr_none--;
				/* Put holes back where they were */
				radix_tree_delete(&mapping->page_tree,
						  iter.index);
				continue;
			}

			VM_BUG_ON_PAGE(page->index != iter.index, page);

			/* Unfreeze the page. */
			list_del(&page->lru);
			page_ref_unfreeze(page, 2);
			radix_tree_replace_slot(&mapping->page_tree,
						slot, page);
			slot = radix_tree_iter_resume(slot, &iter);
			spin_unlock_irq(&mapping->tree_lock);
			putback_lru_page(page);
			unlock_page(page);
			spin_lock_irq(&mapping->tree_lock);
		}
		VM_BUG_ON(nr_none);
		spin_unlock_irq(&mapping->tree_lock);

		/* Unfreeze new_page, caller would take care about freeing it */
		page_ref_unfreeze(new_page, 1);
		mem_cgroup_cancel_charge(new_page, memcg, true);
		unlock_page(new_page);
		new_page->mapping = NULL;
	}
out:
	VM_BUG_ON(!list_empty(&pagelist));
	/* TODO: tracepoints */
}

static void khugepaged_scan_shmem(struct mm_struct *mm,
		struct address_space *mapping,
		pgoff_t start, struct page **hpage)
{
	struct page *page = NULL;
	struct radix_tree_iter iter;
	void **slot;
	int present, swap;
	int node = NUMA_NO_NODE;
	int result = SCAN_SUCCEED;

	present = 0;
	swap = 0;
	memset(khugepaged_node_load, 0, sizeof(khugepaged_node_load));
	rcu_read_lock();
	radix_tree_for_each_slot(slot, &mapping->page_tree, &iter, start) {
		if (iter.index >= start + HPAGE_PMD_NR)
			break;

		page = radix_tree_deref_slot(slot);
		if (radix_tree_deref_retry(page)) {
			slot = radix_tree_iter_retry(&iter);
			continue;
		}

		if (radix_tree_exception(page)) {
			if (++swap > khugepaged_max_ptes_swap) {
				result = SCAN_EXCEED_SWAP_PTE;
				break;
			}
			continue;
		}

		if (PageTransCompound(page)) {
			result = SCAN_PAGE_COMPOUND;
			break;
		}

		node = page_to_nid(page);
		if (khugepaged_scan_abort(node)) {
			result = SCAN_SCAN_ABORT;
			break;
		}
		khugepaged_node_load[node]++;

		if (!PageLRU(page)) {
			result = SCAN_PAGE_LRU;
			break;
		}

		if (page_count(page) != 1 + page_mapcount(page)) {
			result = SCAN_PAGE_COUNT;
			break;
		}

		/*
		 * We probably should check if the page is referenced here, but
		 * nobody would transfer pte_young() to PageReferenced() for us.
		 * And rmap walk here is just too costly...
		 */

		present++;

		if (need_resched()) {
			slot = radix_tree_iter_resume(slot, &iter);
			cond_resched_rcu();
		}
	}
	rcu_read_unlock();

	if (result == SCAN_SUCCEED) {
		if (present < HPAGE_PMD_NR - khugepaged_max_ptes_none) {
			result = SCAN_EXCEED_NONE_PTE;
		} else {
			node = khugepaged_find_target_node();
			collapse_shmem(mm, mapping, start, hpage, node);
		}
	}

	/* TODO: tracepoints */
}
#else
static void khugepaged_scan_shmem(struct mm_struct *mm,
		struct address_space *mapping,
		pgoff_t start, struct page **hpage)
{
	BUILD_BUG();
}
#endif
// parameter 에서 pages 는 전체 scan 할 page 수인 8*512 개중에 scan 을 아직 해야 되는 남은 수임. 
static unsigned int khugepaged_scan_mm_slot(unsigned int pages,
					    struct page **hpage)
	__releases(&khugepaged_mm_lock)
	__acquires(&khugepaged_mm_lock)
{
	struct mm_slot *mm_slot;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	int progress = 0;

	VM_BUG_ON(!pages);
	VM_BUG_ON(NR_CPUS != 1 && !spin_is_locked(&khugepaged_mm_lock));

    // 현재 scan 작업을 수행할 mm 이 없다면
	if (khugepaged_scan.mm_slot) 
		mm_slot = khugepaged_scan.mm_slot; // 있다면 걍 씀.
	else {
		mm_slot = list_entry(khugepaged_scan.mm_head.next,
				     struct mm_slot, mm_node); //mm_head 에 있는 mm 목록에서 하나 가져옴
		khugepaged_scan.address = 0; // vma 주소 0 부터 scan 할것이므로 설정
		khugepaged_scan.mm_slot = mm_slot; // 현재 scan 중인 것 설정.
	}
	spin_unlock(&khugepaged_mm_lock);

	mm = mm_slot->mm;
	down_read(&mm->mmap_sem);
    // mm 에 대해 scan 작업을 시작하기 전에 mm 을가지고 있는 user 가 없어졌는지 즉 mm->mm_users 가
    // 0 인지 먼저 확인 후, 0 이라면 얘는 끝난것이므로 vm_area_struct 0 으로 설정. 
    // 있다면, 현재까지 scan 한 vm address 주소보다 vm_end 가 큰 vm_area_struct 를 가져옴.
    // 즉 다음 vma 를 가져옴
	if (unlikely(khugepaged_test_exit(mm)))
		vma = NULL;
	else
		vma = find_vma(mm, khugepaged_scan.address);

    // mm 이 가진 vma 에 대하여 scan 시작. 
	progress++;
	for (; vma; vma = vma->vm_next) {
		unsigned long hstart, hend;

		cond_resched();
        // loop 내에서 vma scan 할 때 마다, mm_user 확인하여 scan 을 종료해야 하는지 확인
		if (unlikely(khugepaged_test_exit(mm))) { 
			progress++;
			break;
		}
		if (!hugepage_vma_check(vma)) {
skip:
			progress++;
			continue;
		}
        // INFO
        //  HPAGE_PMD_MASK : 0xffff ffff ffe0 0000 
        // -> 1111 1111 1111 1111 1111 1111 1111 1111 1111 1111 1110 0000 0000 0000 0000 0000 
        //  ~HPAGE_PMD_MASK : 0x0000 0000 001f ffff
        // -> 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0001 1111 1111 1111 1111 1111
        // 
        // ex1)
        // vm_start 가0x0..50 1ed2  0000 ... 0101 0000 0001 1110 1101 0010 라면
        // vm_end   가0x0..90 162e  0000 ....1001 0000 0001 0110 0010 1110 라면 
        // 
        // h_start  는 0x60 0000 
        // h_end    는 0x80 0000 
        //
        // ---------------------------------------------------------------------------------------
        // |            |           |       aaaa|aaaaaaaaaaa|aaaaaaa     |    ...    |           |
        // 0            2M          4M          6M          8M          10M   ...
        //                                  ^                   ^
        //                                  vm_start            vm_end
        //                                      ^           ^
        //                                      hstart      hend
        // ex1)
        // vm_start 가0x0..50 1ed2  0000 ... 0101 0000 0001 1110 1101 0010 라면
        // vm_end   가0x0..73 b7b1  0000 ....0111 0011 0111 1011 0010 0001 라면  
        // 
        // h_start  는 0x60 0000
        // h_end    는 0x60 0000
        //
        // ---------------------------------------------------------------------------------------
        // |            |           |       aaaa|aaaaaaa    |            |    ...    |           |
        // 0            2M          4M          6M          8M          10M   ...
        //                                  ^          ^
        //                                  vm_start   vm_end         
        //                                      ^           
        //                                      hstart/hend
        // ex3)
        // vm_start 가0xb 8e77      0000 ... 0000 1011 1000 1110 0111 0111 라면
        // vm_end   가0x0..90 162e  0000 ....1001 0000 0001 0110 0010 1110 라면 
        // 
        // h_start  는 0x20 0000 
        // h_end    는 0x80 0000 
        //
        // ---------------------------------------------------------------------------------------
        // |    aaaaaaaaa|aaaaaaaaaaa|aaaaaaaaaaa|aaaaaaaaaaa|aaaaaaa     |    ...    |           |
        // 0            2M          4M          6M          8M          10M   ...
        //      ^                                                   ^
        //      vm_start                                            vm_end
        //               ^                                      ^
        //               hstart                                 hend 
        //
        // 정리)
        //      vma_area_struct                                                               vm_area_struct                                vm_area_struct
        // ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
        // |    aaaaaaaaa|aaaaaaaaaaa|aaaaaaaaaaa|aaaaaaaaaaa|aaaaaaa     |           |       aaaa|aaaaaa     |           |           |     aaaaaa|aaaaaaaaaaa|aaa        |           |
        // 0            2M          4M          6M          8M          10M          12M         14M         16M         18M         20M         22M         24M         26M         28M
        //      ^                                                   ^                         ^         ^                                   ^                    ^
        //      vm_start                                            vm_end                    vm_start  vm_end                              vm_start             vm_end
        //               ^                                   ^                                    ^                                               ^           ^
        //               hstart                              hend                                 hstart/hend                                     hstart      hend
        //
        //      for (; vma; vma = vma->vm_next){
        //      while (khugepaged_scan.address < hend)                                        while (khugepaged_scan.address < hend)        while (khugepaged_scan.address < hend) 
        //                {                                     }                                 {}                                              {            }
        //      }

        // 위와 같은 mask 연산을 통해 vma 영역이 virtual 영역의 2M 로 나눈 영역을 포함하고 있는 영역인지 검사하고
        // 포함하고 있을경우 vm_start 는 올림해주고, vm_end 는 내림해버림

		hstart = (vma->vm_start + ~HPAGE_PMD_MASK) & HPAGE_PMD_MASK; 
		hend = vma->vm_end & HPAGE_PMD_MASK;        
		if (hstart >= hend) // vma 영역이 2M 를 넘지 않으므로 skip 하고 다음 vma 찾음. 
			goto skip;
		if (khugepaged_scan.address > hend) 
			goto skip;
		if (khugepaged_scan.address < hstart) // scan 시작 주소 설정
			khugepaged_scan.address = hstart;
		VM_BUG_ON(khugepaged_scan.address & ~HPAGE_PMD_MASK);

		while (khugepaged_scan.address < hend) {
			int ret;
			cond_resched();
                
            // 계속 현재 mm_users 가 0 인지 즉 현재 mm_struct 를 공유하는 thread 수가 0 인지 확인
			if (unlikely(khugepaged_test_exit(mm)))  
				goto breakouterloop;

			VM_BUG_ON(khugepaged_scan.address < hstart ||
				  khugepaged_scan.address + HPAGE_PMD_SIZE >
				  hend); 
            // vma 의 file struct 인 vm_file 이 현재 null 이 아닌지, null 이 아니라면, 
            // 그 file struct 의 aops 가 shemem_ops 인지 검사 
            //
            // 즉 Shared Memory Virtual Filesystem 인지 검사함
            // file backed memory 가MAP_SHARED flag 로 mmap 된 경우.
			if (shmem_file(vma->vm_file)) {
				struct file *file;
				pgoff_t pgoff = linear_page_index(vma,
						khugepaged_scan.address);
				if (!shmem_huge_enabled(vma))
					goto skip;
				file = get_file(vma->vm_file);
				up_read(&mm->mmap_sem);
				ret = 1;
				khugepaged_scan_shmem(mm, file->f_mapping,
						pgoff, hpage);
				fput(file);
			} else {
                // vma 가 shared 가 안된 그냥 Anon 일 경우
				ret = khugepaged_scan_pmd(mm, vma,
						khugepaged_scan.address,
						hpage);
			}
			/* move to next address */
			khugepaged_scan.address += HPAGE_PMD_SIZE; 
            // 다음 scan 할 vma 에 2^21 를 더 함. 
            // 위와같이 더한 후 vma 영역이 하나의 hpage 크기면  보통 정상적으로 while 바로 빠져 나옴
            // 더 연속적으로 큰 영역이라면 계속 수행
			progress += HPAGE_PMD_NR;  
            // 512 개의 page 를 scang 했으므로 progress 512 만큼 증가.
			if (ret)
				/* we released mmap_sem so break loop */
				goto breakouterloop_mmap_sem;
			if (progress >= pages)
				goto breakouterloop; 
		}
        // 현재 vma 하나 scan 끝마쳤으므로 다음 vma 를 scan 
	}
breakouterloop:
	up_read(&mm->mmap_sem); /* exit_mmap will destroy ptes after this */
breakouterloop_mmap_sem:

	spin_lock(&khugepaged_mm_lock);
	VM_BUG_ON(khugepaged_scan.mm_slot != mm_slot);
	/*
	 * Release the current mm_slot if this mm is about to die, or
	 * if we scanned all vmas of this mm.
	 */
	if (khugepaged_test_exit(mm) || !vma) {
		/*
		 * Make sure that if mm_users is reaching zero while
		 * khugepaged runs here, khugepaged_exit will find
		 * mm_slot not pointing to the exiting mm.
		 */
		if (mm_slot->mm_node.next != &khugepaged_scan.mm_head) {
			khugepaged_scan.mm_slot = list_entry(
				mm_slot->mm_node.next,
				struct mm_slot, mm_node);
			khugepaged_scan.address = 0;
		} else {
			khugepaged_scan.mm_slot = NULL;
			khugepaged_full_scans++;
		}

		collect_mm_slot(mm_slot);
	}

	return progress;
}

static int khugepaged_has_work(void)
{
    // scan 할 mm_head 즉 mm 이 있는지 확인, list_head 에서 비어있지 않을 경우, 0 return 하므로
    // 비어있지 않고, THP 가 enable 되어 있다면, true 아니면 false 를 return 함. 

	return !list_empty(&khugepaged_scan.mm_head) &&
		khugepaged_enabled();
}

static int khugepaged_wait_event(void)
{
	return !list_empty(&khugepaged_scan.mm_head) ||
		kthread_should_stop();
}

static void khugepaged_do_scan(void)
{
	struct page *hpage = NULL;
	unsigned int progress = 0, pass_through_head = 0;
	unsigned int pages = khugepaged_pages_to_scan; // 512 * 8
	bool wait = true;

	barrier(); /* write khugepaged_pages_to_scan to local stack */
    // pages 가 제대로 기록됨을 보장하기 위해 barrier 후 다음 코드 수행. 
	while (progress < pages) { // 총 512 * 8 개의 page 를 scan 할 것임. 
		if (!khugepaged_prealloc_page(&hpage, &wait))
			break;
        // 새로운 프로세스를 schedule 함. 
        // 왜?..
		cond_resched();

		if (unlikely(kthread_should_stop() || try_to_freeze()))
			break;

		spin_lock(&khugepaged_mm_lock);
		if (!khugepaged_scan.mm_slot)
			pass_through_head++; // 현재 scanning 중인 mm_slot 이 없으면, 2 가 될때까지 기회를 줌. 

        // khugepaged_has_work 에서 khugepaged_scan.mm_head 에 scan 할 mm  이 있는지 검사함. 
        // mm_head 에 mm 이 들어가는 순간은 mm 을 새로 만드는 khugepaged_enter 함수에서 넣어주며,
        // mm 을 새로 생성하는 dup_mm 함수에서 mm 생성시 scan 할 목록에 넣어줌. 
        // INFO
        //  다른데서도 넣어주는데 너무 많아서 아직 다 못봄
		
        if (khugepaged_has_work() &&
		    pass_through_head < 2)
			progress += khugepaged_scan_mm_slot(pages - progress,
							    &hpage);
		else
			progress = pages; 
        // 현재 khugepaged 가 scan 할 mm 이 없다면, progress 를 8 * 512 page 로 즉 limit 로 설정하여
        // khugepaged scan process 를 끝냄.
		spin_unlock(&khugepaged_mm_lock);
	}

	if (!IS_ERR_OR_NULL(hpage))
		put_page(hpage);
}

static bool khugepaged_should_wakeup(void)
{
	return kthread_should_stop() ||
	       time_after_eq(jiffies, khugepaged_sleep_expire);
}

static void khugepaged_wait_work(void)
{
	if (khugepaged_has_work()) {
		const unsigned long scan_sleep_jiffies =
			msecs_to_jiffies(khugepaged_scan_sleep_millisecs);

		if (!scan_sleep_jiffies)
			return;

		khugepaged_sleep_expire = jiffies + scan_sleep_jiffies;
		wait_event_freezable_timeout(khugepaged_wait,
					     khugepaged_should_wakeup(),
					     scan_sleep_jiffies);
		return;
	}

	if (khugepaged_enabled())
		wait_event_freezable(khugepaged_wait, khugepaged_wait_event());
}

static int khugepaged(void *none)
{
	struct mm_slot *mm_slot;

	set_freezable();
	set_user_nice(current, MAX_NICE);

	while (!kthread_should_stop()) {
		khugepaged_do_scan();
		khugepaged_wait_work();
	}

	spin_lock(&khugepaged_mm_lock);
	mm_slot = khugepaged_scan.mm_slot;
	khugepaged_scan.mm_slot = NULL;
	if (mm_slot)
		collect_mm_slot(mm_slot);
	spin_unlock(&khugepaged_mm_lock);
	return 0;
}

static void set_recommended_min_free_kbytes(void)
{
	struct zone *zone;
	int nr_zones = 0;
	unsigned long recommended_min;

	for_each_populated_zone(zone)
		nr_zones++;

	/* Ensure 2 pageblocks are free to assist fragmentation avoidance */
	recommended_min = pageblock_nr_pages * nr_zones * 2;

	/*
	 * Make sure that on average at least two pageblocks are almost free
	 * of another type, one for a migratetype to fall back to and a
	 * second to avoid subsequent fallbacks of other types There are 3
	 * MIGRATE_TYPES we care about.
	 */
	recommended_min += pageblock_nr_pages * nr_zones *
			   MIGRATE_PCPTYPES * MIGRATE_PCPTYPES;

	/* don't ever allow to reserve more than 5% of the lowmem */
	recommended_min = min(recommended_min,
			      (unsigned long) nr_free_buffer_pages() / 20);
	recommended_min <<= (PAGE_SHIFT-10);

	if (recommended_min > min_free_kbytes) {
		if (user_min_free_kbytes >= 0)
			pr_info("raising min_free_kbytes from %d to %lu to help transparent hugepage allocations\n",
				min_free_kbytes, recommended_min);

		min_free_kbytes = recommended_min;
	}
	setup_per_zone_wmarks();
}

int start_stop_khugepaged(void)
{
	static struct task_struct *khugepaged_thread __read_mostly;
	static DEFINE_MUTEX(khugepaged_mutex);
	int err = 0;

	mutex_lock(&khugepaged_mutex);
	if (khugepaged_enabled()) {
		if (!khugepaged_thread)
			khugepaged_thread = kthread_run(khugepaged, NULL,
							"khugepaged");
		if (IS_ERR(khugepaged_thread)) {
			pr_err("khugepaged: kthread_run(khugepaged) failed\n");
			err = PTR_ERR(khugepaged_thread);
			khugepaged_thread = NULL;
			goto fail;
		}

		if (!list_empty(&khugepaged_scan.mm_head))
			wake_up_interruptible(&khugepaged_wait);

		set_recommended_min_free_kbytes();
	} else if (khugepaged_thread) {
		kthread_stop(khugepaged_thread);
		khugepaged_thread = NULL;
	}
fail:
	mutex_unlock(&khugepaged_mutex);
	return err;
}
