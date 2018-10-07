/*
 * bvec iterator
 *
 * Copyright (C) 2001 Ming Lei <ming.lei@canonical.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public Licens
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-
 */
#ifndef __LINUX_BVEC_ITER_H
#define __LINUX_BVEC_ITER_H

#include <linux/kernel.h>
#include <linux/bug.h>

/*
 * was unsigned short, but we might as well be ready for > 64kB I/O pages
 */
// 하나의 bio 를 구성하는 요소들 bio 내에서 array로 존재
struct bio_vec {
	struct page	*bv_page;
	unsigned int	bv_len;
    // i/o 할 byte 단위 크기
	unsigned int	bv_offset;
    // i/o 할 page frame 내의 위치
};

struct bvec_iter {
	sector_t		bi_sector;	/* device address in 512 byte sectors */
    // io 를 진행한 sector 수
	unsigned int		bi_size;	/* residual I/O count */
    // struct bio 내의 i/o 가 수행되어야 할 전체 byte단위 크기 
    // 즉 각 bvec 들의 bv_len 들을 합한 값
	unsigned int		bi_idx;		/* current index into bvl_vec */
    // 
	unsigned int            bi_bvec_done;	/* number of bytes completed in
						   current bvec */
    // 현재 bvec 에서 i/o complete 된 byte 수
};

/*
 * various member access, note that bio_data should of course not be used
 * on highmem page vectors
 */
#define __bvec_iter_bvec(bvec, iter)	(&(bvec)[(iter).bi_idx])

#define bvec_iter_page(bvec, iter)				\
	(__bvec_iter_bvec((bvec), (iter))->bv_page)

#define bvec_iter_len(bvec, iter)				\
	min((iter).bi_size,					\
	    __bvec_iter_bvec((bvec), (iter))->bv_len - (iter).bi_bvec_done)

#define bvec_iter_offset(bvec, iter)				\
	(__bvec_iter_bvec((bvec), (iter))->bv_offset + (iter).bi_bvec_done)

#define bvec_iter_bvec(bvec, iter)				\
((struct bio_vec) {						\
	.bv_page	= bvec_iter_page((bvec), (iter)),	\
	.bv_len		= bvec_iter_len((bvec), (iter)),	\
	.bv_offset	= bvec_iter_offset((bvec), (iter)),	\
})

static inline void bvec_iter_advance(const struct bio_vec *bv,
				     struct bvec_iter *iter,
				     unsigned bytes)
{
	WARN_ONCE(bytes > iter->bi_size,
		  "Attempted to advance past end of bvec iter\n");

	while (bytes) {
		unsigned iter_len = bvec_iter_len(bv, *iter);
		unsigned len = min(bytes, iter_len);

		bytes -= len;
		iter->bi_size -= len;
		iter->bi_bvec_done += len;

		if (iter->bi_bvec_done == __bvec_iter_bvec(bv, *iter)->bv_len) {
			iter->bi_bvec_done = 0;
			iter->bi_idx++;
		}
	}
}

#define for_each_bvec(bvl, bio_vec, iter, start)			\
	for (iter = (start);						\
	     (iter).bi_size &&						\
		((bvl = bvec_iter_bvec((bio_vec), (iter))), 1);	\
	     bvec_iter_advance((bio_vec), &(iter), (bvl).bv_len))

#endif /* __LINUX_BVEC_ITER_H */
