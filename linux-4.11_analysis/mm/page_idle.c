#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/fs.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/mmu_notifier.h>
#include <linux/page_ext.h>
#include <linux/page_idle.h>

#define BITMAP_CHUNK_SIZE	sizeof(u64)
#define BITMAP_CHUNK_BITS	(BITMAP_CHUNK_SIZE * BITS_PER_BYTE)

/*
 * Idle page tracking only considers user memory pages, for other types of
 * pages the idle flag is always unset and an attempt to set it is silently
 * ignored.
 *
 * We treat a page as a user memory page if it is on an LRU list, because it is
 * always safe to pass such a page to rmap_walk(), which is essential for idle
 * page tracking. With such an indicator of user pages we can skip isolated
 * pages, but since there are not usually many of them, it will hardly affect
 * the overall result.
 *
 * This function tries to get a user memory page by pfn as described above.
 */
static struct page *page_idle_get_page(unsigned long pfn)
{
	struct page *page;
	struct zone *zone;

	if (!pfn_valid(pfn))
		return NULL;

	page = pfn_to_page(pfn);
	if (!page || !PageLRU(page) ||
	    !get_page_unless_zero(page))
		return NULL;

	zone = page_zone(page);
	spin_lock_irq(zone_lru_lock(zone));
	if (unlikely(!PageLRU(page))) {
		put_page(page);
		page = NULL;
	}
	spin_unlock_irq(zone_lru_lock(zone));
	return page;
}

static int page_idle_clear_pte_refs_one(struct page *page,
					struct vm_area_struct *vma,
					unsigned long addr, void *arg)
{
	struct page_vma_mapped_walk pvmw = {
		.page = page,
		.vma = vma,
		.address = addr,
	};
	bool referenced = false;

	while (page_vma_mapped_walk(&pvmw)) {
		addr = pvmw.address;
		if (pvmw.pte) {
			referenced = ptep_clear_young_notify(vma, addr,
					pvmw.pte);
            // pte 의 ACCESSED BIT clear 수행
		} else if (IS_ENABLED(CONFIG_TRANSPARENT_HUGEPAGE)) {
			referenced = pmdp_clear_young_notify(vma, addr,
					pvmw.pmd);
            // THP 설정되어 있다면 pmd 의 ACCESSED BIT clear 수행
		} else {
			/* unexpected pmd-mapped page? */
			WARN_ON_ONCE(1);
		}
	}
    // ACCESSED BIT 가 설정되어 있었어서 ACCESSED BIT 를 clear 하고 
    // 그전 값을 가져왔다면 referenced 는 false 가 아님
	if (referenced) {
		clear_page_idle(page);
        // page 가 reference 되었으니 PG_idle clear 수행 
		/*
		 * We cleared the referenced bit in a mapping to this page. To
		 * avoid interference with page reclaim, mark it young so that
		 * page_referenced() will return > 0.
		 */
		set_page_young(page);
        // idle page tracking 을 위해 page teble 의 accessed bit 를 
        // clear 해주었으므로 page reclaim 에 영향을 주지 않게 
        // PG_young 을 설정하여 추후, page reclaim 에서 page_referenced 
        // 검사 시, true 반환되도록 해줌
	}
	return SWAP_AGAIN;
}
// 주어진 struct page 에 대해 idle page tracking 수행   
// page 에 해당되는 pte 에 accessed bit 설정된 경우.. 
//  - ACCESSED bit clear 
//  - idle flag clear 
//  - youg flag set 
// 을 수행해줌
static void page_idle_clear_pte_refs(struct page *page)
{
	/*
	 * Since rwc.arg is unused, rwc is effectively immutable, so we
	 * can make it static const to save some cycles and stack.
	 */
	static const struct rmap_walk_control rwc = {
		.rmap_one = page_idle_clear_pte_refs_one,
		.anon_lock = page_lock_anon_vma_read,
	};
	bool need_lock;

	if (!page_mapped(page) ||
	    !page_rmapping(page))
		return;
    // pte map 된 page 인지 검사하여 map 되지 않은 page 라면 out 하고 
    // map 된 page 라면 mapping flag 제거

	need_lock = !PageAnon(page) || PageKsm(page);
	if (need_lock && !trylock_page(page))
		return;
    // anonymous page 가 아니고, KSM page 인 경우, 
    // idle page tracking 하는데 PG_locked 설정해 주어야 함. 
    // 그전에 먼저 lock 기다려야 된다면 idle tracking pass 함
	rmap_walk(page, (struct rmap_walk_control *)&rwc);

	if (need_lock)
		unlock_page(page);
}

static ssize_t page_idle_bitmap_read(struct file *file, struct kobject *kobj,
				     struct bin_attribute *attr, char *buf,
				     loff_t pos, size_t count)
{
	u64 *out = (u64 *)buf;
	struct page *page;
	unsigned long pfn, end_pfn;
	int bit;

	if (pos % BITMAP_CHUNK_SIZE || count % BITMAP_CHUNK_SIZE)
		return -EINVAL;

	pfn = pos * BITS_PER_BYTE;
	if (pfn >= max_pfn)
		return 0;

	end_pfn = pfn + count * BITS_PER_BYTE;
	if (end_pfn > max_pfn)
		end_pfn = ALIGN(max_pfn, BITMAP_CHUNK_BITS);

	for (; pfn < end_pfn; pfn++) {
		bit = pfn % BITMAP_CHUNK_BITS;
		if (!bit)
			*out = 0ULL;
		page = page_idle_get_page(pfn);
		if (page) {
			if (page_is_idle(page)) {
				/*
				 * The page might have been referenced via a
				 * pte, in which case it is not idle. Clear
				 * refs and recheck.
				 */
				page_idle_clear_pte_refs(page);
				if (page_is_idle(page))
					*out |= 1ULL << bit;
			}
			put_page(page);
		}
		if (bit == BITMAP_CHUNK_BITS - 1)
			out++;
		cond_resched();
	}
	return (char *)out - buf;
}

static ssize_t page_idle_bitmap_write(struct file *file, struct kobject *kobj,
				      struct bin_attribute *attr, char *buf,
				      loff_t pos, size_t count)
{
	const u64 *in = (u64 *)buf;
	struct page *page;
	unsigned long pfn, end_pfn;
	int bit;

	if (pos % BITMAP_CHUNK_SIZE || count % BITMAP_CHUNK_SIZE)
		return -EINVAL;

	pfn = pos * BITS_PER_BYTE;
	if (pfn >= max_pfn)
		return -ENXIO;

	end_pfn = pfn + count * BITS_PER_BYTE;
	if (end_pfn > max_pfn)
		end_pfn = ALIGN(max_pfn, BITMAP_CHUNK_BITS);

	for (; pfn < end_pfn; pfn++) {
		bit = pfn % BITMAP_CHUNK_BITS;
		if ((*in >> bit) & 1) {
			page = page_idle_get_page(pfn);
			if (page) {
				page_idle_clear_pte_refs(page);
				set_page_idle(page);
				put_page(page);
			}
		}
		if (bit == BITMAP_CHUNK_BITS - 1)
			in++;
		cond_resched();
	}
	return (char *)in - buf;
}

static struct bin_attribute page_idle_bitmap_attr =
		__BIN_ATTR(bitmap, S_IRUSR | S_IWUSR,
			   page_idle_bitmap_read, page_idle_bitmap_write, 0);

static struct bin_attribute *page_idle_bin_attrs[] = {
	&page_idle_bitmap_attr,
	NULL,
};

static struct attribute_group page_idle_attr_group = {
	.bin_attrs = page_idle_bin_attrs,
	.name = "page_idle",
};

#ifndef CONFIG_64BIT
static bool need_page_idle(void)
{
	return true;
}
struct page_ext_operations page_idle_ops = {
	.need = need_page_idle,
};
#endif

static int __init page_idle_init(void)
{
	int err;

	err = sysfs_create_group(mm_kobj, &page_idle_attr_group);
	if (err) {
		pr_err("page_idle: register sysfs failed\n");
		return err;
	}
	return 0;
}
subsys_initcall(page_idle_init);
