#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/page_ext.h>
#include <linux/poison.h>
#include <linux/ratelimit.h>

static bool __page_poisoning_enabled __read_mostly;
static bool want_page_poisoning __read_mostly;

static int early_page_poison_param(char *buf)
{
	if (!buf)
		return -EINVAL;
	return strtobool(buf, &want_page_poisoning);
}
early_param("page_poison", early_page_poison_param);

bool page_poisoning_enabled(void)
{
	return __page_poisoning_enabled;
}

static bool need_page_poisoning(void)
{
	return want_page_poisoning;
}

static void init_page_poisoning(void)
{
	/*
	 * page poisoning is debug page alloc for some arches. If either
	 * of those options are enabled, enable poisoning
	 */
	if (!IS_ENABLED(CONFIG_ARCH_SUPPORTS_DEBUG_PAGEALLOC)) {
		if (!want_page_poisoning && !debug_pagealloc_enabled())
			return;
	} else {
		if (!want_page_poisoning)
			return;
	}

	__page_poisoning_enabled = true;
}

struct page_ext_operations page_poisoning_ops = {
	.need = need_page_poisoning,
	.init = init_page_poisoning,
};

static inline void set_page_poison(struct page *page)
{
	struct page_ext *page_ext;

	page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return;

	__set_bit(PAGE_EXT_DEBUG_POISON, &page_ext->flags);
}
// struct page 내의 
static inline void clear_page_poison(struct page *page)
{
	struct page_ext *page_ext;

	page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return;

	__clear_bit(PAGE_EXT_DEBUG_POISON, &page_ext->flags);
}
//
// CONFIG_PAGE_POISONING 이 설정되어 있을 경우에 해당
bool page_is_poisoned(struct page *page)
{
	struct page_ext *page_ext;

	page_ext = lookup_page_ext(page);
    // CONFIG_PAGE_EXTENSION 이 설정되어 있는 경우..
    // page_ext 배열에서 struct page 에 해당하는  
    // struct page_ext entry 를 가져옴
	if (unlikely(!page_ext))
		return false;

	return test_bit(PAGE_EXT_DEBUG_POISON, &page_ext->flags);
    // page_ext 에 PAGE_EXT_DEBUG_POISON 이 설정되어 있는지 검사. 
    // 설정되어 있다면 page allocation 시, poisoned pattern 을 
    // 검사 해 주어야 한다. 
}

static void poison_page(struct page *page)
{
	void *addr = kmap_atomic(page);

	set_page_poison(page);
	memset(addr, PAGE_POISON, PAGE_SIZE);
	kunmap_atomic(addr);
}

static void poison_pages(struct page *page, int n)
{
	int i;

	for (i = 0; i < n; i++)
		poison_page(page + i);
}

static bool single_bit_flip(unsigned char a, unsigned char b)
{
	unsigned char error = a ^ b;

	return error && !(error & (error - 1));
}

static void check_poison_mem(unsigned char *mem, size_t bytes)
{
	static DEFINE_RATELIMIT_STATE(ratelimit, 5 * HZ, 10);
	unsigned char *start;
	unsigned char *end;

	if (IS_ENABLED(CONFIG_PAGE_POISONING_NO_SANITY))
		return;

	start = memchr_inv(mem, PAGE_POISON, bytes);
	if (!start)
		return;

	for (end = mem + bytes - 1; end > start; end--) {
		if (*end != PAGE_POISON)
			break;
	}

	if (!__ratelimit(&ratelimit))
		return;
	else if (start == end && single_bit_flip(*start, PAGE_POISON))
		pr_err("pagealloc: single bit error\n");
	else
		pr_err("pagealloc: memory corruption\n");

	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_ADDRESS, 16, 1, start,
			end - start + 1, 1);
	dump_stack();
}
// page allocation 수행 시, poisoned page 에 대해 검사 수행 및 
// page 사용을 위해 page poisoned flag clear 수행
static void unpoison_page(struct page *page)
{
	void *addr;

	if (!page_is_poisoned(page))
		return;

	addr = kmap_atomic(page);
	check_poison_mem(addr, PAGE_SIZE);
    // poisoned 된 page 에 대해 error & corruption 검사 수행
	clear_page_poison(page);
    // poisoned 되었다는 flag clear
	kunmap_atomic(addr);
}

static void unpoison_pages(struct page *page, int n)
{
	int i;

	for (i = 0; i < n; i++)
		unpoison_page(page + i);
}
// page poisoining 기능 설정 시, page allocation ,free 과정에서 
// poisoned pattern 설정 및 해제 등 수행
void kernel_poison_pages(struct page *page, int numpages, int enable)
{
	if (!page_poisoning_enabled())
		return;
    // page poisoning 기능 on 안되있으면 나감
	if (enable)
		unpoison_pages(page, numpages);
    // page allocation 하는 경우 poisoned 된 
    // page 에 대해 검사하며 unpoison 수행
	else
		poison_pages(page, numpages);
}

#ifndef CONFIG_ARCH_SUPPORTS_DEBUG_PAGEALLOC
void __kernel_map_pages(struct page *page, int numpages, int enable)
{
	/* This function does nothing, all work is done via poison pages */
}
#endif
