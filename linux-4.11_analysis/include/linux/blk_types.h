/*
 * Block data types and constants.  Directly include this file only to
 * break include dependency loop.
 */
#ifndef __LINUX_BLK_TYPES_H
#define __LINUX_BLK_TYPES_H

#include <linux/types.h>
#include <linux/bvec.h>

struct bio_set;
struct bio;
struct bio_integrity_payload;
struct page;
struct block_device;
struct io_context;
struct cgroup_subsys_state;
typedef void (bio_end_io_t) (struct bio *);

/*
 * main unit of I/O for the block layer and lower layers (ie drivers and
 * stacking drivers)
 */
// i/o 를 수행할 disk 영역의 정보와 disk 에 저장될 또는 disk 의 내용이 옮겨질 memory 
// 내의 page 위치 정보를 가지고 있음
// 하나의 bio는 disk 내의 연속된 영역을 나타냄 
// 여러개의 bio들이 모여 하나의 request 로 합쳐지기도 함
// generic_make_request 또는 submit_bio 함수를 통해 bio layer 에 queue 되며, 해당 I/O
// request 가 완료될때 까지 대기 하지 않고, queueing 만 수행 후 반환.
// (queue 에 공간이 없다면 여유공간 즉 memory 가 확보될 때 까지 대기하기도 함)
struct bio {
	struct bio		*bi_next;	/* request queue link */
    // 하나의 single linked list를 구성하고 있는 bio linked list
	struct block_device	*bi_bdev;
    // request 가 속해있는(I/O될) block device
	int			bi_error;
	unsigned int		bi_opf;		/* bottom bits req flags,
						 * top bits REQ_OP. Use
						 * accessors.
						 */
    // 현재 i/o request 의 request type 등의 정보 flag 
    // e.g. 
    //  0000000000000000
    //          ********
    //                |
    //                ----- * 이 8 가지 bit 는 request type 나타냄.
    //                      - LSB 가 1 일 경우 WRITE 임(REQ_OP_WRITE)
    //                      - LSB 가 0 일 경우 READ 임(REQ_OP_READ)
    //
    // REQ_NOWAIT 설정되어 있을 시, queue 에 들어갈 공간이 없다면 BLK_STS_AGAIN 상태로 반환
	unsigned short		bi_flags;	/* status, command, etc */
	unsigned short		bi_ioprio;

	struct bvec_iter	bi_iter;
    // 현재 bio 내의 bio_vec 둘을 iterate 할 때, 현재까지의 정보들을 관리하기 위한 함수
    // 즉 iterate 하는 도중 얼마나 많은 sector 와 buffer 가 소모되었는지 등 정보 유지 
	/* Number of segments in this BIO after
	 * physical address coalescing is performed.
	 */
	unsigned int		bi_phys_segments;
    // 현재 bio 관련 memory 영역의 segment 수 한 bio 에 여러 segment가 있을 수도 있고,
    // 단일 segment 가 있을수도 있다.
	/*
	 * To keep track of the max segment size, we account for the
	 * sizes of the first and last mergeable segments in this bio.
	 */
	unsigned int		bi_seg_front_size;
	unsigned int		bi_seg_back_size;

	atomic_t		__bi_remaining;

	bio_end_io_t		*bi_end_io;
    // hardware transfer 종료시 호출되어 해당 bio의 i/o종료를 알림
	void			*bi_private;
#ifdef CONFIG_BLK_CGROUP
	/*
	 * Optional ioc and css associated with this bio.  Put on bio
	 * release.  Read comment on top of bio_associate_current().
	 */
	struct io_context	*bi_ioc;
	struct cgroup_subsys_state *bi_css;
#endif
	union {
#if defined(CONFIG_BLK_DEV_INTEGRITY)
		struct bio_integrity_payload *bi_integrity; /* data integrity */
#endif
	};

	unsigned short		bi_vcnt;	/* how many bio_vec's */
    // bio bector array의 entry수
	/*
	 * Everything starting with bi_max_vecs will be preserved by bio_reset()
	 */

	unsigned short		bi_max_vecs;	/* max bvl_vecs we can hold */

	atomic_t		__bi_cnt;	/* pin count */

	struct bio_vec		*bi_io_vec;	/* the actual vec list */

	struct bio_set		*bi_pool;

	/*
	 * We can inline a number of vecs at the end of the bio, to avoid
	 * double allocations for a small number of bio_vecs. This member
	 * MUST obviously be kept at the very end of the bio.
	 */
	struct bio_vec		bi_inline_vecs[0];
};

#define BIO_RESET_BYTES		offsetof(struct bio, bi_max_vecs)

/*
 * bio flags
 */
#define BIO_SEG_VALID	1	/* bi_phys_segments valid */
#define BIO_CLONED	2	/* doesn't own data */
#define BIO_BOUNCED	3	/* bio is a bounce bio */
#define BIO_USER_MAPPED 4	/* contains user pages */
#define BIO_NULL_MAPPED 5	/* contains invalid user pages */
#define BIO_QUIET	6	/* Make BIO Quiet */
#define BIO_CHAIN	7	/* chained bio, ->bi_remaining in effect */
#define BIO_REFFED	8	/* bio has elevated ->bi_cnt */
#define BIO_THROTTLED	9	/* This bio has already been subjected to
				 * throttling rules. Don't do it again. */

/*
 * Flags starting here get preserved by bio_reset() - this includes
 * BVEC_POOL_IDX()
 */
#define BIO_RESET_BITS	10

/*
 * We support 6 different bvec pools, the last one is magic in that it
 * is backed by a mempool.
 */
#define BVEC_POOL_NR		6
#define BVEC_POOL_MAX		(BVEC_POOL_NR - 1)

/*
 * Top 4 bits of bio flags indicate the pool the bvecs came from.  We add
 * 1 to the actual index so that 0 indicates that there are no bvecs to be
 * freed.
 */
#define BVEC_POOL_BITS		(4)
#define BVEC_POOL_OFFSET	(16 - BVEC_POOL_BITS)
#define BVEC_POOL_IDX(bio)	((bio)->bi_flags >> BVEC_POOL_OFFSET)

/*
 * Operations and flags common to the bio and request structures.
 * We use 8 bits for encoding the operation, and the remaining 24 for flags.
 *
 * The least significant bit of the operation number indicates the data
 * transfer direction:
 *
 *   - if the least significant bit is set transfers are TO the device
 *   - if the least significant bit is not set transfers are FROM the device
 *
 * If a operation does not transfer data the least significant bit has no
 * meaning.
 */
#define REQ_OP_BITS	8
#define REQ_OP_MASK	((1 << REQ_OP_BITS) - 1) // .... 1111 1111
#define REQ_FLAG_BITS	24

enum req_opf {
	/* read sectors from the device */
	REQ_OP_READ		= 0,
	/* write sectors to the device */
	REQ_OP_WRITE		= 1,
	/* flush the volatile write cache */
	REQ_OP_FLUSH		= 2,
	/* discard sectors */
	REQ_OP_DISCARD		= 3,
	/* get zone information */
	REQ_OP_ZONE_REPORT	= 4,
	/* securely erase sectors */
	REQ_OP_SECURE_ERASE	= 5,
	/* seset a zone write pointer */
	REQ_OP_ZONE_RESET	= 6,
	/* write the same sector many times */
	REQ_OP_WRITE_SAME	= 7,
	/* write the zero filled sector many times */
	REQ_OP_WRITE_ZEROES	= 8,

	/* SCSI passthrough using struct scsi_request */
	REQ_OP_SCSI_IN		= 32,
	REQ_OP_SCSI_OUT		= 33,
	/* Driver private requests */
	REQ_OP_DRV_IN		= 34,
	REQ_OP_DRV_OUT		= 35,

	REQ_OP_LAST,
};

enum req_flag_bits {
	__REQ_FAILFAST_DEV =	/* no driver retries of device errors */
		REQ_OP_BITS,
	__REQ_FAILFAST_TRANSPORT, /* no driver retries of transport errors */
	__REQ_FAILFAST_DRIVER,	/* no driver retries of driver errors */
	__REQ_SYNC,		/* request is sync (sync write or read) */
	__REQ_META,		/* metadata io request */
	__REQ_PRIO,		/* boost priority in cfq */
	__REQ_NOMERGE,		/* don't touch this for merging */
	__REQ_IDLE,		/* anticipate more IO after this one */
	__REQ_INTEGRITY,	/* I/O includes block integrity payload */
	__REQ_FUA,		/* forced unit access */
	__REQ_PREFLUSH,		/* request for cache flush */
	__REQ_RAHEAD,		/* read ahead, can fail anytime */
	__REQ_BACKGROUND,	/* background IO */
	__REQ_NR_BITS,		/* stops here */
};

#define REQ_FAILFAST_DEV	(1ULL << __REQ_FAILFAST_DEV)
#define REQ_FAILFAST_TRANSPORT	(1ULL << __REQ_FAILFAST_TRANSPORT)
#define REQ_FAILFAST_DRIVER	(1ULL << __REQ_FAILFAST_DRIVER)
#define REQ_SYNC		(1ULL << __REQ_SYNC)
#define REQ_META		(1ULL << __REQ_META)
#define REQ_PRIO		(1ULL << __REQ_PRIO)
#define REQ_NOMERGE		(1ULL << __REQ_NOMERGE)
#define REQ_IDLE		(1ULL << __REQ_IDLE)
#define REQ_INTEGRITY		(1ULL << __REQ_INTEGRITY)
#define REQ_FUA			(1ULL << __REQ_FUA)
#define REQ_PREFLUSH		(1ULL << __REQ_PREFLUSH)
#define REQ_RAHEAD		(1ULL << __REQ_RAHEAD)
#define REQ_BACKGROUND		(1ULL << __REQ_BACKGROUND)

#define REQ_FAILFAST_MASK \
	(REQ_FAILFAST_DEV | REQ_FAILFAST_TRANSPORT | REQ_FAILFAST_DRIVER)

#define REQ_NOMERGE_FLAGS \
	(REQ_NOMERGE | REQ_PREFLUSH | REQ_FUA)

#define bio_op(bio) \
	((bio)->bi_opf & REQ_OP_MASK)
#define req_op(req) \
	((req)->cmd_flags & REQ_OP_MASK)

/* obsolete, don't use in new code */
static inline void bio_set_op_attrs(struct bio *bio, unsigned op,
		unsigned op_flags)
{
	bio->bi_opf = op | op_flags;
}

static inline bool op_is_write(unsigned int op)
{
	return (op & 1);
}

/*
 * Check if the bio or request is one that needs special treatment in the
 * flush state machine.
 */
static inline bool op_is_flush(unsigned int op)
{
	return op & (REQ_FUA | REQ_PREFLUSH);
}

/*
 * Reads are always treated as synchronous, as are requests with the FUA or
 * PREFLUSH flag.  Other operations may be marked as synchronous using the
 * REQ_SYNC flag.
 */
static inline bool op_is_sync(unsigned int op)
{
	return (op & REQ_OP_MASK) == REQ_OP_READ ||
		(op & (REQ_SYNC | REQ_FUA | REQ_PREFLUSH));
}

typedef unsigned int blk_qc_t;
#define BLK_QC_T_NONE		-1U
#define BLK_QC_T_SHIFT		16
#define BLK_QC_T_INTERNAL	(1U << 31)

static inline bool blk_qc_t_valid(blk_qc_t cookie)
{
	return cookie != BLK_QC_T_NONE;
}

static inline blk_qc_t blk_tag_to_qc_t(unsigned int tag, unsigned int queue_num,
				       bool internal)
{
	blk_qc_t ret = tag | (queue_num << BLK_QC_T_SHIFT);

	if (internal)
		ret |= BLK_QC_T_INTERNAL;

	return ret;
}

static inline unsigned int blk_qc_t_to_queue_num(blk_qc_t cookie)
{
	return (cookie & ~BLK_QC_T_INTERNAL) >> BLK_QC_T_SHIFT;
}

static inline unsigned int blk_qc_t_to_tag(blk_qc_t cookie)
{
	return cookie & ((1u << BLK_QC_T_SHIFT) - 1);
}

static inline bool blk_qc_t_is_internal(blk_qc_t cookie)
{
	return (cookie & BLK_QC_T_INTERNAL) != 0;
}

struct blk_issue_stat {
	u64 time;
};

#define BLK_RQ_STAT_BATCH	64

struct blk_rq_stat {
	s64 mean;
	u64 min;
	u64 max;
	s32 nr_samples;
	s32 nr_batch;
	u64 batch;
	s64 time;
};

#endif /* __LINUX_BLK_TYPES_H */
