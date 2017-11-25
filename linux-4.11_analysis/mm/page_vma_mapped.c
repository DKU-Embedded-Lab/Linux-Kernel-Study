#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/hugetlb.h>
#include <linux/swap.h>
#include <linux/swapops.h>

#include "internal.h"

static inline bool check_pmd(struct page_vma_mapped_walk *pvmw)
{
	pmd_t pmde;
	/*
	 * Make sure we don't re-load pmd between present and !trans_huge check.
	 * We need a consistent view.
	 */
	pmde = READ_ONCE(*pvmw->pmd);
	return pmd_present(pmde) && !pmd_trans_huge(pmde);
}

static inline bool not_found(struct page_vma_mapped_walk *pvmw)
{
	page_vma_mapped_walk_done(pvmw);
	return false;
}

static bool map_pte(struct page_vma_mapped_walk *pvmw)
{
	pvmw->pte = pte_offset_map(pvmw->pmd, pvmw->address);
	if (!(pvmw->flags & PVMW_SYNC)) {
		if (pvmw->flags & PVMW_MIGRATION) {
			if (!is_swap_pte(*pvmw->pte))
				return false;
		} else {
			if (!pte_present(*pvmw->pte))
				return false;
		}
	}
	pvmw->ptl = pte_lockptr(pvmw->vma->vm_mm, pvmw->pmd);
    // pmd 에서의pte  offet 구해 pte 가져옴
	spin_lock(pvmw->ptl);
	return true;
}

static bool check_pte(struct page_vma_mapped_walk *pvmw)
{
	if (pvmw->flags & PVMW_MIGRATION) {
#ifdef CONFIG_MIGRATION
		swp_entry_t entry;
		if (!is_swap_pte(*pvmw->pte))
			return false;
		entry = pte_to_swp_entry(*pvmw->pte);
		if (!is_migration_entry(entry))
			return false;
		if (migration_entry_to_page(entry) - pvmw->page >=
				hpage_nr_pages(pvmw->page)) {
			return false;
		}
		if (migration_entry_to_page(entry) < pvmw->page)
			return false;
#else
		WARN_ON_ONCE(1);
#endif
	} else {
		if (!pte_present(*pvmw->pte))
			return false;

		/* THP can be referenced by any subpage */
		if (pte_page(*pvmw->pte) - pvmw->page >=
				hpage_nr_pages(pvmw->page)) {
			return false;
		}
		if (pte_page(*pvmw->pte) < pvmw->page)
			return false;
	}

	return true;
}

/**
 * page_vma_mapped_walk - check if @pvmw->page is mapped in @pvmw->vma at
 * @pvmw->address
 * @pvmw: pointer to struct page_vma_mapped_walk. page, vma, address and flags
 * must be set. pmd, pte and ptl must be NULL.
 *
 * Returns true if the page is mapped in the vma. @pvmw->pmd and @pvmw->pte point
 * to relevant page table entries. @pvmw->ptl is locked. @pvmw->address is
 * adjusted if needed (for PTE-mapped THPs).
 *
 * If @pvmw->pmd is set but @pvmw->pte is not, you have found PMD-mapped page
 * (usually THP). For PTE-mapped THP, you should run page_vma_mapped_walk() in
 * a loop to find all PTEs that map the THP.
 *
 * For HugeTLB pages, @pvmw->pte is set to the relevant page table entry
 * regardless of which page table level the page is mapped at. @pvmw->pmd is
 * NULL.
 *
 * Retruns false if there are no more page table entries for the page in
 * the vma. @pvmw->ptl is unlocked and @pvmw->pte is unmapped.
 *
 * If you need to stop the walk before page_vma_mapped_walk() returned false,
 * use page_vma_mapped_walk_done(). It will do the housekeeping.
 */ 
// memory 에 존재 할 시,
// pvmw 에 pmd, pte 등을 설정해줌
bool page_vma_mapped_walk(struct page_vma_mapped_walk *pvmw)
{
	struct mm_struct *mm = pvmw->vma->vm_mm;
	struct page *page = pvmw->page;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;

	/* The only possible pmd mapping has been handled on last iteration */
	if (pvmw->pmd && !pvmw->pte)
		return not_found(pvmw);
    // pmd 설정 안되고 pte 설정 안된 경우는 처음 search 
    // FIXME
    // pmd 설정 되고   pte 설정 안된 경우는 THP 
	if (pvmw->pte)
		goto next_pte; 
    // FIXME
    // pte 설정 안된 경우는 처음 search 
    // pvmw.pte 가 있다면 pte 가져온적 있으므로 다음 pte 가져오는 부분으로 
	if (unlikely(PageHuge(pvmw->page))) { 
        // hugetlbfs 일 경우 pmd 를 통해 map 되므로 pmd 를 
        // pte 로 cast 하여 반환
		/* when pud is not present, pte will be NULL */
		pvmw->pte = huge_pte_offset(mm, pvmw->address);
		if (!pvmw->pte)
			return false;

		pvmw->ptl = huge_pte_lockptr(page_hstate(page), mm, pvmw->pte);
		spin_lock(pvmw->ptl);
		if (!check_pte(pvmw))
			return not_found(pvmw);
		return true;
	}
restart:
	pgd = pgd_offset(mm, pvmw->address);
	if (!pgd_present(*pgd))
		return false;
	p4d = p4d_offset(pgd, pvmw->address);
	if (!p4d_present(*p4d))
		return false;
	pud = pud_offset(p4d, pvmw->address);
	if (!pud_present(*pud))
		return false;
	pvmw->pmd = pmd_offset(pud, pvmw->address); 
    // 넘겨진 address 에 해당하는 pmd 찾아냄
	if (pmd_trans_huge(*pvmw->pmd)) { 
        // THP 인 경우
		pvmw->ptl = pmd_lock(mm, pvmw->pmd); 
        // page table lock 잡음
		if (!pmd_present(*pvmw->pmd))
			return not_found(pvmw); 
        // pmd 의 entry 내에 _PAGE_PRESENT bit 검사. 
        // 없다면 현재 memory 에 올라와 있지 않은 것이므로 
        // lock 풀고,unmap 하고 끝
		if (likely(pmd_trans_huge(*pvmw->pmd))) {
			if (pvmw->flags & PVMW_MIGRATION)
				return not_found(pvmw);
            // FIXME
			if (pmd_page(*pvmw->pmd) != page)
				return not_found(pvmw); 
            // pmd table 에 해당하는 page 를 가져옴
			return true;
		} else {
			/* THP pmd was split under us: handle on pte level */
			spin_unlock(pvmw->ptl);
			pvmw->ptl = NULL;
		}
	} else {
        // THP 가 아닌 경우
		if (!check_pmd(pvmw))
			return false;
	}
	if (!map_pte(pvmw))
		goto next_pte;
    // pte 가져와 pvmw->pte 에 설정
	while (1) {
		if (check_pte(pvmw))
			return true;
next_pte:
		/* Seek to next pte only makes sense for THP */
		if (!PageTransHuge(pvmw->page) || PageHuge(pvmw->page))
			return not_found(pvmw);
		do {
			pvmw->address += PAGE_SIZE;
			if (pvmw->address >= pvmw->vma->vm_end ||
			    pvmw->address >=
					__vma_address(pvmw->page, pvmw->vma) +
					hpage_nr_pages(pvmw->page) * PAGE_SIZE)
				return not_found(pvmw);
			/* Did we cross page table boundary? */
			if (pvmw->address % PMD_SIZE == 0) {
				pte_unmap(pvmw->pte);
				if (pvmw->ptl) {
					spin_unlock(pvmw->ptl);
					pvmw->ptl = NULL;
				}
				goto restart;
			} else {
				pvmw->pte++;
			}
		} while (pte_none(*pvmw->pte));

		if (!pvmw->ptl) {
			pvmw->ptl = pte_lockptr(mm, pvmw->pmd);
			spin_lock(pvmw->ptl);
		}
	}
}

/**
 * page_mapped_in_vma - check whether a page is really mapped in a VMA
 * @page: the page to test
 * @vma: the VMA to test
 *
 * Returns 1 if the page is mapped into the page tables of the VMA, 0
 * if the page is not mapped into the page tables of this VMA.  Only
 * valid for normal file or anonymous VMAs.
 */
int page_mapped_in_vma(struct page *page, struct vm_area_struct *vma)
{
	struct page_vma_mapped_walk pvmw = {
		.page = page,
		.vma = vma,
		.flags = PVMW_SYNC,
	};
	unsigned long start, end;

	start = __vma_address(page, vma);
	end = start + PAGE_SIZE * (hpage_nr_pages(page) - 1);

	if (unlikely(end < vma->vm_start || start >= vma->vm_end))
		return 0;
	pvmw.address = max(start, vma->vm_start);
	if (!page_vma_mapped_walk(&pvmw))
		return 0;
	page_vma_mapped_walk_done(&pvmw);
	return 1;
}
