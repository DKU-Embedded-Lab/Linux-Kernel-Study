/*
 * bio-integrity.c - bio data integrity extensions
 *
 * Copyright (C) 2007, 2008, 2009 Oracle Corporation
 * Written by: Martin K. Petersen <martin.petersen@oracle.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139,
 * USA.
 *
 */

#include <linux/blkdev.h>
#include <linux/mempool.h>
#include <linux/export.h>
#include <linux/bio.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include "blk.h"

#define BIP_INLINE_VECS	4

static struct kmem_cache *bip_slab;
static struct workqueue_struct *kintegrityd_wq;

void blk_flush_integrity(void)
{
	flush_workqueue(kintegrityd_wq);
}

/**
 * bio_integrity_alloc - Allocate integrity payload and attach it to bio
 * @bio:	bio to attach integrity metadata to
 * @gfp_mask:	Memory allocation mask
 * @nr_vecs:	Number of integrity metadata scatter-gather elements
 *
 * Description: This function prepares a bio for attaching integrity
 * metadata.  nr_vecs specifies the maximum number of pages containing
 * integrity metadata that can be attached.
 */
// 
// integrity metadata payload 할당 및 bio 에 덧붙임
struct bio_integrity_payload *bio_integrity_alloc(struct bio *bio,
						  gfp_t gfp_mask,
						  unsigned int nr_vecs)
{
	struct bio_integrity_payload *bip;
	struct bio_set *bs = bio->bi_pool;
	unsigned inline_vecs;

	if (!bs || !bs->bio_integrity_pool) {
        // bio_set 에 integrity metadata 를 위한 mempool 할당 안되어 있을 시, kmalloc 할당
		bip = kmalloc(sizeof(struct bio_integrity_payload) +
			      sizeof(struct bio_vec) * nr_vecs, gfp_mask);
		inline_vecs = nr_vecs;
	} else {
        // 이미 할당되어 있을시, mempool 이용하여 할당 및 inline_vecs 을 4로 초기화
		bip = mempool_alloc(bs->bio_integrity_pool, gfp_mask);
		inline_vecs = BIP_INLINE_VECS;
	}

	if (unlikely(!bip))
		return ERR_PTR(-ENOMEM);

	memset(bip, 0, sizeof(*bip));

	if (nr_vecs > inline_vecs) {
        // bip를 mempool 로 할당한 경우  
        // page 의 수가 4page 보다 크다면
		unsigned long idx = 0;

		bip->bip_vec = bvec_alloc(gfp_mask, nr_vecs, &idx,
					  bs->bvec_integrity_pool);
        // mempool 에서 bio_vec 할당
		if (!bip->bip_vec)
			goto err;
		bip->bip_max_vcnt = bvec_nr_vecs(idx);
		bip->bip_slab = idx;
	} else {
        // bip 를 kmalloc 으로 할당한 경우
		bip->bip_vec = bip->bip_inline_vecs;
        // bio_vec 을 bio_integrity_payload 에서 활용
		bip->bip_max_vcnt = inline_vecs;
        // 최대 수를 4개로 저의
	}

	bip->bip_bio = bio;
	bio->bi_integrity = bip;
    // bio 와 서로 연결
	bio->bi_opf |= REQ_INTEGRITY;

	return bip;
err:
	mempool_free(bip, bs->bio_integrity_pool);
	return ERR_PTR(-ENOMEM);
}
EXPORT_SYMBOL(bio_integrity_alloc);

/**
 * bio_integrity_free - Free bio integrity payload
 * @bio:	bio containing bip to be freed
 *
 * Description: Used to free the integrity portion of a bio. Usually
 * called from bio_free().
 */
void bio_integrity_free(struct bio *bio)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);
	struct bio_set *bs = bio->bi_pool;

	if (bip->bip_flags & BIP_BLOCK_INTEGRITY)
		kfree(page_address(bip->bip_vec->bv_page) +
		      bip->bip_vec->bv_offset);

	if (bs && bs->bio_integrity_pool) {
		bvec_free(bs->bvec_integrity_pool, bip->bip_vec, bip->bip_slab);

		mempool_free(bip, bs->bio_integrity_pool);
	} else {
		kfree(bip);
	}

	bio->bi_integrity = NULL;
}
EXPORT_SYMBOL(bio_integrity_free);

/**
 * bio_integrity_add_page - Attach integrity metadata
 * @bio:	bio to update
 * @page:	page containing integrity metadata
 * @len:	number of bytes of integrity metadata in page
 * @offset:	start offset within page
 *
 * Description: Attach a page containing integrity metadata to bio.
 */
// 
// integrity metadata 용도로 할당한 page 의 offset 부터 len 만큼 bio 에 연결
int bio_integrity_add_page(struct bio *bio, struct page *page,
			   unsigned int len, unsigned int offset)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);
	struct bio_vec *iv;

	if (bip->bip_vcnt >= bip->bip_max_vcnt) {
		printk(KERN_ERR "%s: bip_vec full\n", __func__);
		return 0;
	}

	iv = bip->bip_vec + bip->bip_vcnt;

	if (bip->bip_vcnt &&
	    bvec_gap_to_prev(bdev_get_queue(bio->bi_bdev),
			     &bip->bip_vec[bip->bip_vcnt - 1], offset))
		return 0;

	iv->bv_page = page;
	iv->bv_len = len;
	iv->bv_offset = offset;
	bip->bip_vcnt++;

	return len;
}
EXPORT_SYMBOL(bio_integrity_add_page);

/**
 * bio_integrity_enabled - Check whether integrity can be passed
 * @bio:	bio to check
 *
 * Description: Determines whether bio_integrity_prep() can be called
 * on this bio or not.	bio data direction and target device must be
 * set prior to calling.  The functions honors the write_generate and
 * read_verify flags in sysfs.
 */
//
// blk_integrity 검사 여부 확인
bool bio_integrity_enabled(struct bio *bio)
{
	struct blk_integrity *bi = bdev_get_integrity(bio->bi_bdev);
    // requesst_queue 에서 blk_integrity 가져옴 

	if (bio_op(bio) != REQ_OP_READ && bio_op(bio) != REQ_OP_WRITE)
		return false;
    // sector read 거나 flush volatile cache (i.e write) 가 아니면 종료

	/* Already protected? */
	if (bio_integrity(bio))
		return false;
    // bio 의 operation 속성에 REQ_INTEGRITY 이 설정되어 integrity payload 가 
    // 포함되어 있는지 검사하여 bio 에서 bio_integrity_payload 가져옴

	if (bi == NULL)
		return false;

	if (bio_data_dir(bio) == READ && bi->profile->verify_fn != NULL &&
	    (bi->flags & BLK_INTEGRITY_VERIFY))
		return true;
    // 현재 bio 가 REQ_OP_WRITE 일 경우 integrity verify 함수 존재 여부 확인

	if (bio_data_dir(bio) == WRITE && bi->profile->generate_fn != NULL &&
	    (bi->flags & BLK_INTEGRITY_GENERATE))
		return true;

	return false;
}
EXPORT_SYMBOL(bio_integrity_enabled);

/**
 * bio_integrity_intervals - Return number of integrity intervals for a bio
 * @bi:		blk_integrity profile for device
 * @sectors:	Size of the bio in 512-byte sectors
 *
 * Description: The block layer calculates everything in 512 byte
 * sectors but integrity metadata is done in terms of the data integrity
 * interval size of the storage device.  Convert the block layer sectors
 * to the appropriate number of integrity intervals.
 */ 
// 
// 현재 bio의 I/O 크기를 기반으로 integrity metadata 가 저장될 횟수를 반환
static inline unsigned int bio_integrity_intervals(struct blk_integrity *bi,
						   unsigned int sectors)
{
	return sectors >> (bi->interval_exp - 9);
    // 몇개의 integrity metadata 를 추가하여 덧붙일 것인지 반환
    // (현재 bio에서 I/O할 sector의 수) >> (integrity metadata 의 byte 단위 interval 2 승수 - 9)
}

static inline unsigned int bio_integrity_bytes(struct blk_integrity *bi,
					       unsigned int sectors)
{
	return bio_integrity_intervals(bi, sectors) * bi->tuple_size;
}

/**
 * bio_integrity_process - Process integrity metadata for a bio
 * @bio:	bio to generate/verify integrity metadata for
 * @proc_fn:	Pointer to the relevant processing function
 */
static int bio_integrity_process(struct bio *bio,
				 integrity_processing_fn *proc_fn)
{
	struct blk_integrity *bi = bdev_get_integrity(bio->bi_bdev);
	struct blk_integrity_iter iter;
	struct bvec_iter bviter;
	struct bio_vec bv;
	struct bio_integrity_payload *bip = bio_integrity(bio);
	unsigned int ret = 0;
	void *prot_buf = page_address(bip->bip_vec->bv_page) +
		bip->bip_vec->bv_offset;

	iter.disk_name = bio->bi_bdev->bd_disk->disk_name;
	iter.interval = 1 << bi->interval_exp;
	iter.seed = bip_get_seed(bip);
	iter.prot_buf = prot_buf;

	bio_for_each_segment(bv, bio, bviter) {
		void *kaddr = kmap_atomic(bv.bv_page);

		iter.data_buf = kaddr + bv.bv_offset;
		iter.data_size = bv.bv_len;

		ret = proc_fn(&iter);
		if (ret) {
			kunmap_atomic(kaddr);
			return ret;
		}

		kunmap_atomic(kaddr);
	}
	return ret;
}

/**
 * bio_integrity_prep - Prepare bio for integrity I/O
 * @bio:	bio to prepare
 *
 * Description: Allocates a buffer for integrity metadata, maps the
 * pages and attaches them to a bio.  The bio must have data
 * direction, target device and start sector set priot to calling.  In
 * the WRITE case, integrity metadata will be generated using the
 * block device's integrity function.  In the READ case, the buffer
 * will be prepared for DMA and a suitable end_io handler set up.
 */
int bio_integrity_prep(struct bio *bio)
{
	struct bio_integrity_payload *bip;
	struct blk_integrity *bi;
	struct request_queue *q;
	void *buf;
	unsigned long start, end;
	unsigned int len, nr_pages;
	unsigned int bytes, offset, i;
	unsigned int intervals;

	bi = bdev_get_integrity(bio->bi_bdev);
    // requesst_queue 에서 bio_integrity 를 가져옴
	q = bdev_get_queue(bio->bi_bdev);
    // request_queue 를 가져옴
	BUG_ON(bi == NULL);
	BUG_ON(bio_integrity(bio));

	intervals = bio_integrity_intervals(bi, bio_sectors(bio));
    // integrity metadata 를 덧붙일 횟수를 반환

	/* Allocate kernel buffer for protection data */
	len = intervals * bi->tuple_size;
    // 전체 integrity metadata 의 크기 와 횟수를 곱하여 전체 
    // integrity metadata 의 크기 계산
	buf = kmalloc(len, GFP_NOIO | q->bounce_gfp);
    // len 크기의 integrity metadata 용 buffer 할당
	if (unlikely(buf == NULL)) {
		printk(KERN_ERR "could not allocate integrity buffer\n");
		return -ENOMEM;
	}

	end = (((unsigned long) buf) + len + PAGE_SIZE - 1) >> PAGE_SHIFT;
	start = ((unsigned long) buf) >> PAGE_SHIFT;
	nr_pages = end - start;
    // 할당한 page 의 시작 pfn, 끝 pfn, integrity metadata 용 할당한 buffer의 page 수 구함

	/* Allocate bio integrity payload and integrity vectors */
	bip = bio_integrity_alloc(bio, GFP_NOIO, nr_pages);
    // bio_integrity_payload 할당 및 stuct bio 와 연결 수행
	if (IS_ERR(bip)) {
		printk(KERN_ERR "could not allocate data integrity bioset\n");
		kfree(buf);
		return PTR_ERR(bip);
	}

	bip->bip_flags |= BIP_BLOCK_INTEGRITY;
	bip->bip_iter.bi_size = len;
	bip_set_seed(bip, bio->bi_iter.bi_sector);
    // bio 의 bi_sector 를 seed 로 사용
    
	if (bi->flags & BLK_INTEGRITY_IP_CHECKSUM)
		bip->bip_flags |= BIP_IP_CHECKSUM;

	/* Map it */
	offset = offset_in_page(buf);
	for (i = 0 ; i < nr_pages ; i++) {
		int ret;
		bytes = PAGE_SIZE - offset;

		if (len <= 0)
			break;

		if (bytes > len)
			bytes = len;

		ret = bio_integrity_add_page(bio, virt_to_page(buf),
					     bytes, offset);
        // integrity metadata 용으로 할당한 page 인 buf 부터 len 까지를 
        // bip 내의 bio_vec 와 연결해줌
		if (ret == 0)
			return 0;

		if (ret < bytes)
			break;

		buf += bytes;
		len -= bytes;
		offset = 0;
	}

	/* Install custom I/O completion handler if read verify is enabled */
	if (bio_data_dir(bio) == READ) {
		bip->bip_end_io = bio->bi_end_io;
		bio->bi_end_io = bio_integrity_endio;
	}

	/* Auto-generate integrity metadata if this is a write */
	if (bio_data_dir(bio) == WRITE)
		bio_integrity_process(bio, bi->profile->generate_fn);

	return 0;
}
EXPORT_SYMBOL(bio_integrity_prep);

/**
 * bio_integrity_verify_fn - Integrity I/O completion worker
 * @work:	Work struct stored in bio to be verified
 *
 * Description: This workqueue function is called to complete a READ
 * request.  The function verifies the transferred integrity metadata
 * and then calls the original bio end_io function.
 */
static void bio_integrity_verify_fn(struct work_struct *work)
{
	struct bio_integrity_payload *bip =
		container_of(work, struct bio_integrity_payload, bip_work);
	struct bio *bio = bip->bip_bio;
	struct blk_integrity *bi = bdev_get_integrity(bio->bi_bdev);

	bio->bi_error = bio_integrity_process(bio, bi->profile->verify_fn);

	/* Restore original bio completion handler */
	bio->bi_end_io = bip->bip_end_io;
	bio_endio(bio);
}

/**
 * bio_integrity_endio - Integrity I/O completion function
 * @bio:	Protected bio
 * @error:	Pointer to errno
 *
 * Description: Completion for integrity I/O
 *
 * Normally I/O completion is done in interrupt context.  However,
 * verifying I/O integrity is a time-consuming task which must be run
 * in process context.	This function postpones completion
 * accordingly.
 */
void bio_integrity_endio(struct bio *bio)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);

	BUG_ON(bip->bip_bio != bio);

	/* In case of an I/O error there is no point in verifying the
	 * integrity metadata.  Restore original bio end_io handler
	 * and run it.
	 */
	if (bio->bi_error) {
		bio->bi_end_io = bip->bip_end_io;
		bio_endio(bio);

		return;
	}

	INIT_WORK(&bip->bip_work, bio_integrity_verify_fn);
	queue_work(kintegrityd_wq, &bip->bip_work);
}
EXPORT_SYMBOL(bio_integrity_endio);

/**
 * bio_integrity_advance - Advance integrity vector
 * @bio:	bio whose integrity vector to update
 * @bytes_done:	number of data bytes that have been completed
 *
 * Description: This function calculates how many integrity bytes the
 * number of completed data bytes correspond to and advances the
 * integrity vector accordingly.
 */
void bio_integrity_advance(struct bio *bio, unsigned int bytes_done)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);
	struct blk_integrity *bi = bdev_get_integrity(bio->bi_bdev);
	unsigned bytes = bio_integrity_bytes(bi, bytes_done >> 9);

	bvec_iter_advance(bip->bip_vec, &bip->bip_iter, bytes);
}
EXPORT_SYMBOL(bio_integrity_advance);

/**
 * bio_integrity_trim - Trim integrity vector
 * @bio:	bio whose integrity vector to update
 * @offset:	offset to first data sector
 * @sectors:	number of data sectors
 *
 * Description: Used to trim the integrity vector in a cloned bio.
 * The ivec will be advanced corresponding to 'offset' data sectors
 * and the length will be truncated corresponding to 'len' data
 * sectors.
 */
void bio_integrity_trim(struct bio *bio, unsigned int offset,
			unsigned int sectors)
{
	struct bio_integrity_payload *bip = bio_integrity(bio);
	struct blk_integrity *bi = bdev_get_integrity(bio->bi_bdev);

	bio_integrity_advance(bio, offset << 9);
	bip->bip_iter.bi_size = bio_integrity_bytes(bi, sectors);
}
EXPORT_SYMBOL(bio_integrity_trim);

/**
 * bio_integrity_clone - Callback for cloning bios with integrity metadata
 * @bio:	New bio
 * @bio_src:	Original bio
 * @gfp_mask:	Memory allocation mask
 *
 * Description:	Called to allocate a bip when cloning a bio
 */
int bio_integrity_clone(struct bio *bio, struct bio *bio_src,
			gfp_t gfp_mask)
{
	struct bio_integrity_payload *bip_src = bio_integrity(bio_src);
	struct bio_integrity_payload *bip;

	BUG_ON(bip_src == NULL);

	bip = bio_integrity_alloc(bio, gfp_mask, bip_src->bip_vcnt);
	if (IS_ERR(bip))
		return PTR_ERR(bip);

	memcpy(bip->bip_vec, bip_src->bip_vec,
	       bip_src->bip_vcnt * sizeof(struct bio_vec));

	bip->bip_vcnt = bip_src->bip_vcnt;
	bip->bip_iter = bip_src->bip_iter;

	return 0;
}
EXPORT_SYMBOL(bio_integrity_clone);

int bioset_integrity_create(struct bio_set *bs, int pool_size)
{
	if (bs->bio_integrity_pool)
		return 0;

	bs->bio_integrity_pool = mempool_create_slab_pool(pool_size, bip_slab);
	if (!bs->bio_integrity_pool)
		return -1;

	bs->bvec_integrity_pool = biovec_create_pool(pool_size);
	if (!bs->bvec_integrity_pool) {
		mempool_destroy(bs->bio_integrity_pool);
		return -1;
	}

	return 0;
}
EXPORT_SYMBOL(bioset_integrity_create);

void bioset_integrity_free(struct bio_set *bs)
{
	if (bs->bio_integrity_pool)
		mempool_destroy(bs->bio_integrity_pool);

	if (bs->bvec_integrity_pool)
		mempool_destroy(bs->bvec_integrity_pool);
}
EXPORT_SYMBOL(bioset_integrity_free);

void __init bio_integrity_init(void)
{
	/*
	 * kintegrityd won't block much but may burn a lot of CPU cycles.
	 * Make it highpri CPU intensive wq with max concurrency of 1.
	 */
	kintegrityd_wq = alloc_workqueue("kintegrityd", WQ_MEM_RECLAIM |
					 WQ_HIGHPRI | WQ_CPU_INTENSIVE, 1);
	if (!kintegrityd_wq)
		panic("Failed to create kintegrityd\n");

	bip_slab = kmem_cache_create("bio_integrity_payload",
				     sizeof(struct bio_integrity_payload) +
				     sizeof(struct bio_vec) * BIP_INLINE_VECS,
				     0, SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL);
}
