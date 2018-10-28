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
    // io 를 진행할 device 내의 sector 주소
	unsigned int		bi_size;	/* residual I/O count */
    // struct bio 내의 i/o 가 수행되어야 할 전체 byte단위 크기 
    // 즉 각 bvec 들의 bv_len 들을 합한 값
	unsigned int		bi_idx;		/* current index into bvl_vec */
    // bvec_iter 에서 다음 처리할 bio_vec의 index
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
// 남은 I/O size 와 현재 bvec 내에서 남은 I/O 크기중 최소값 가져옴 

#define bvec_iter_offset(bvec, iter)				\
	(__bvec_iter_bvec((bvec), (iter))->bv_offset + (iter).bi_bvec_done)

// bvec_iter 의 bi_bvec_done 을 bvec 의 기존 값에 더해가며 
// page내의 offset인 bv_offset 을 update(기존 + 완료),
// bvec 에서 I/O 할 남은 크기(byte 단위)를 update(기존 - 완료) 
#define bvec_iter_bvec(bvec, iter)				\
((struct bio_vec) {						\
	.bv_page	= bvec_iter_page((bvec), (iter)),	\
	.bv_len		= bvec_iter_len((bvec), (iter)),	\
	.bv_offset	= bvec_iter_offset((bvec), (iter)),	\
})

// I/O 완료된 크기인 bytes 를 기반으로 bvec 을 관리하는 bvec_iter 을 update 해줌
static inline void bvec_iter_advance(const struct bio_vec *bv,
				     struct bvec_iter *iter,
				     unsigned bytes)
{
	WARN_ONCE(bytes > iter->bi_size,
		  "Attempted to advance past end of bvec iter\n");

	while (bytes) {
        // I/O 를 수행하여 byte가 0이 아니라면
		unsigned iter_len = bvec_iter_len(bv, *iter);
        // 현재 bvec 내의 I/O 가 아직 다 안끝났을 수 있으므로 
        // 현재 bvec 내에서 남은 I/O 크기 가져옴
		unsigned len = min(bytes, iter_len);

		bytes -= len;
		iter->bi_size -= len;
        // I/O 수행끝난 byte 수를 기존의 bi_size 즉은 남은 I/O 크기에서 빼줌
		iter->bi_bvec_done += len;
        // 현재 bvec loop 내에서 I/O 완료된 byte 를 증가
		if (iter->bi_bvec_done == __bvec_iter_bvec(bv, *iter)->bv_len) {
            // 현재 bvec 내에서 수행한 I/O 크기가 bvec 내에서 수행할 크기와 같다면 현재 
            // bvec 끝난 것.
			iter->bi_bvec_done = 0;
            // 다음 loop 로 넘어갈 것이므로 bi_bvec_done 다시 0으로 초기화
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
