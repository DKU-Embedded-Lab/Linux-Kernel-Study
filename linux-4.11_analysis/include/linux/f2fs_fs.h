/**
 * include/linux/f2fs_fs.h
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef _LINUX_F2FS_FS_H
#define _LINUX_F2FS_FS_H

#include <linux/pagemap.h>
#include <linux/types.h>

#define F2FS_SUPER_OFFSET		1024	/* byte-size offset */
#define F2FS_MIN_LOG_SECTOR_SIZE	9	/* 9 bits for 512 bytes */
#define F2FS_MAX_LOG_SECTOR_SIZE	12	/* 12 bits for 4096 bytes */
#define F2FS_LOG_SECTORS_PER_BLOCK	3	/* log number for sector/blk */
#define F2FS_BLKSIZE			4096	/* support only 4KB block */
#define F2FS_BLKSIZE_BITS		12	/* bits for F2FS_BLKSIZE */
#define F2FS_MAX_EXTENSION		64	/* # of extension entries */
#define F2FS_BLK_ALIGN(x)	(((x) + F2FS_BLKSIZE - 1) >> F2FS_BLKSIZE_BITS)

#define NULL_ADDR		((block_t)0)	/* used as block_t addresses */
#define NEW_ADDR		((block_t)-1)	/* used as block_t addresses */

#define F2FS_BYTES_TO_BLK(bytes)	((bytes) >> F2FS_BLKSIZE_BITS)
#define F2FS_BLK_TO_BYTES(blk)		((blk) << F2FS_BLKSIZE_BITS)

/* 0, 1(node nid), 2(meta nid) are reserved node id */
#define F2FS_RESERVED_NODE_NUM		3

#define F2FS_ROOT_INO(sbi)	(sbi->root_ino_num)
#define F2FS_NODE_INO(sbi)	(sbi->node_ino_num)
#define F2FS_META_INO(sbi)	(sbi->meta_ino_num)

#define F2FS_IO_SIZE(sbi)	(1 << (sbi)->write_io_size_bits) /* Blocks */
#define F2FS_IO_SIZE_KB(sbi)	(1 << ((sbi)->write_io_size_bits + 2)) /* KB */
#define F2FS_IO_SIZE_BYTES(sbi)	(1 << ((sbi)->write_io_size_bits + 12)) /* B */
#define F2FS_IO_SIZE_BITS(sbi)	((sbi)->write_io_size_bits) /* power of 2 */
#define F2FS_IO_SIZE_MASK(sbi)	(F2FS_IO_SIZE(sbi) - 1)

/* This flag is used by node and meta inodes, and by recovery */
#define GFP_F2FS_ZERO		(GFP_NOFS | __GFP_ZERO)
#define GFP_F2FS_HIGH_ZERO	(GFP_NOFS | __GFP_ZERO | __GFP_HIGHMEM)

/*
 * For further optimization on multi-head logs, on-disk layout supports maximum
 * 16 logs by default. The number, 16, is expected to cover all the cases
 * enoughly. The implementaion currently uses no more than 6 logs.
 * Half the logs are used for nodes, and the other half are used for data.
 */
#define MAX_ACTIVE_LOGS	16
#define MAX_ACTIVE_NODE_LOGS	8
#define MAX_ACTIVE_DATA_LOGS	8

#define VERSION_LEN	256
#define MAX_VOLUME_NAME		512
#define MAX_PATH_LEN		64
#define MAX_DEVICES		8

/*
 * For superblock
 */
struct f2fs_device {
	__u8 path[MAX_PATH_LEN];                                                // 64
	__le32 total_segments;                                                  // 4  68
} __packed;

struct f2fs_super_block {                                                   // 0x400
	__le32 magic;			/* Magic Number */                              // 4    4               
	__le16 major_ver;		/* Major Version */                             // 2    6
	__le16 minor_ver;		/* Minor Version */                             // 2    8
	__le32 log_sectorsize;		/* log2 sector size in bytes */             // 4    12              sector : 2^9            =>  512 byte 
	__le32 log_sectors_per_block;	/* log2 # of sectors per block */       // 4    16   - 1        blocks : 2^3 sects      =>    8 sectors
	__le32 log_blocksize;		/* log2 block size in bytes */              // 4    20              blksize : 2^12 byte     => 4096 bytes
	__le32 log_blocks_per_seg;	/* log2 # of blocks per segment */          // 4    24              segment : 2^9 blocks    =>  512 blocks
	__le32 segs_per_sec;		/* # of segments per section */             // 4    28              section : 2^1 segments  =>   2 segments
	__le32 secs_per_zone;		/* # of sections per zone */                // 4    32   - 2        zone : 2^1 sections     =>   2 sections
	__le32 checksum_offset;		/* checksum offset inside super block */    // 4    36              
	__le64 block_count;		/* total # of user blocks */                    // 8    44              total blocks :       0x100000 blocks => 4 GB
	__le32 section_count;		/* total # of sections */                   // 4    48   - 3        total sections :        0x7ed   
	__le32 segment_count;		/* total # of segments */                   // 4    52              total segments :        0x7ff 
	__le32 segment_count_ckpt;	/* # of segments for checkpoint */          // 4    56              CP    segments :          0x2 -|           =>    1024 blocks   =>   0x400000 bytes 
	__le32 segment_count_sit;	/* # of segments for SIT */                 // 4    60              SIT   segments :          0x2  |           =>    1024 blocks   =>   0x400000 bytes
	__le32 segment_count_nat;	/* # of segments for NAT */                 // 4    64   - 4        NAT   segments :          0xa  |-> 0x7ff   =>    5120 blocks   =>  0x1400000 bytes
	__le32 segment_count_ssa;	/* # of segments for SSA */                 // 4    68              SSA   segments :          0x4  |           =>    2048 blocks   =>   0x800000 bytes
	__le32 segment_count_main;	/* # of segments for main area */           // 4    72              MAIN  segments :        0x7ed -|           => 1038848 blocks   => 0xFDA00000 bytes
	__le32 segment0_blkaddr;	/* start block address of segment 0 */      // 4    76              segment-0 blk addr :   0x0100 
	__le32 cp_blkaddr;		/* start block address of checkpoint */         // 4    80   - 5        CP    blk addr :        0x100 blocks       =>  0x100000 bytes 
	__le32 sit_blkaddr;		/* start block address of SIT */                // 4    84              SIT   blk addr :        0x500              =>  0x500000 bytes // ?
	__le32 nat_blkaddr;		/* start block address of NAT */                // 4    88              NAT   blk addr :        0x900              =>  0x900000 bytes
	__le32 ssa_blkaddr;		/* start block address of SSA */                // 4    92              SSA   blk addr :       0x1d00              => 0x1d00000 bytes // ?
	__le32 main_blkaddr;		/* start block address of main area */      // 4    96   - 6        MAIN  blk addr :       0x2500              => 0x2500000 bytes // ?
	__le32 root_ino;		/* root inode number */                         // 4    100             root inode :             0x03 
	__le32 node_ino;		/* node inode number */                         // 4    104             node inode :             0x01 
	__le32 meta_ino;		/* meta inode number */                         // 4    108             meta inode :             0x02
	__u8 uuid[16];			/* 128-bit uuid for volume */                   // 16   124  - 7(128)
	__le16 volume_name[MAX_VOLUME_NAME];	/* volume name */               // 1024 1148
	__le32 extension_count;		/* # of extensions below */                 // 4    1152               -> 0x87c
	__u8 extension_list[F2FS_MAX_EXTENSION][8];	/* extension array */       // 512  1664               -> 0x880
	__le32 cp_payload;                                                      // 4    1668
	__u8 version[VERSION_LEN];	/* the kernel version */                    // 256  1924               -> 0xa84
	__u8 init_version[VERSION_LEN];	/* the initial kernel version */        // 256  2180               -> 0xb84
	__le32 feature;			/* defined features */                          // 4    2184
	__u8 encryption_level;		/* versioning level for encryption */       // 1    2185
	__u8 encrypt_pw_salt[16];	/* Salt used for string2key algorithm */    // 16   2201
	struct f2fs_device devs[MAX_DEVICES];	/* device list */               // 544  2745 
	__u8 reserved[327];		/* valid reserved region */                     // 327  3072
} __packed;                                                                 
                                                                            
/*                                                                          
 * For checkpoint                                                           
 */                                                                         
#define CP_NAT_BITS_FLAG	0x00000080
#define CP_CRC_RECOVERY_FLAG	0x00000040
#define CP_FASTBOOT_FLAG	0x00000020
#define CP_FSCK_FLAG		0x00000010
#define CP_ERROR_FLAG		0x00000008
#define CP_COMPACT_SUM_FLAG	0x00000004
#define CP_ORPHAN_PRESENT_FLAG	0x00000002
#define CP_UMOUNT_FLAG		0x00000001

#define F2FS_CP_PACKS		2	/* # of checkpoint packs */

struct f2fs_checkpoint {                                                    // -> 0x100000
	__le64 checkpoint_ver;		/* checkpoint block version number */       // 8    8           checkpoint version          : 0x1 byte
	__le64 user_block_count;	/* # of user blocks */                      // 8    16  - 1     user blocks                 : 0xed200 blocks
	__le64 valid_block_count;	/* # of valid blocks in main area */        // 8    24          valid blocks in MAIN        : 0x2 blocks
	__le32 rsvd_segment_count;	/* # of reserved segments for gc */         // 4    28          reserved segment            : 0x47 segments
	__le32 overprov_segment_count;	/* # of overprovision segments */       // 4    32  - 2     overprovision segments      : 0x84 segments
	__le32 free_segment_count;	/* # of free segments in main area */       // 4    36          free segment count in MAIN  : 0x7e7 segments

	/* information of current node segments */
	__le32 cur_node_segno[MAX_ACTIVE_NODE_LOGS];                            // 32   68          current node segments       : 0x7ec, 0x7eb, 0x7ea, 0xff..ff, ..., 0xff..ff
	__le16 cur_node_blkoff[MAX_ACTIVE_NODE_LOGS];                           // 16   84          current node blk offset     : 0x01,  0x00,  0x00,  0x00
	/* information of current data segments */
	__le32 cur_data_segno[MAX_ACTIVE_DATA_LOGS];                            // 32   116         current data segments       : 0x7e9, 0x01,  0x00,  0xff..ff, ..., 0xff..ff 
	__le16 cur_data_blkoff[MAX_ACTIVE_DATA_LOGS];                           // 16   132         current data blk offset     : 0x01,  0x00,  0x00,  0x00
	__le32 ckpt_flags;		/* Flags : umount and journal_present */        // 4    136         check point flags           : 0x05
	__le32 cp_pack_total_block_count;	/* total # of one cp pack */        // 4    140         cp pack total block         : 0x06
	__le32 cp_pack_start_sum;	/* start block number of data summary */    // 4    144         data summary block addr     : 0x01
	__le32 valid_node_count;	/* Total number of valid nodes */           // 4    148         total valid node            : 0x01
	__le32 valid_inode_count;	/* Total number of valid inodes */          // 4    152         total valid inode           : 0x01
	__le32 next_free_nid;		/* Next free node number */                 // 4    156         next free node id           : 0x04
	__le32 sit_ver_bitmap_bytesize;	/* Default value 64 */                  // 4    160         sit version bitmap          : 0x40
	__le32 nat_ver_bitmap_bytesize; /* Default value 256 */                 // 4    164         nat version bitmap          : 0x140
	__le32 checksum_offset;		/* checksum offset inside cp block */       // 4    168         checksum offset             : 0x0ffc 
	__le64 elapsed_time;		/* mounted time */                          // 8    176         mounted time                : 0x00
	/* allocation type of current segment */
	unsigned char alloc_type[MAX_ACTIVE_LOGS];                              // 16   192         allocation type             

	/* SIT and NAT version bitmap */
	unsigned char sit_nat_version_bitmap[1];                                // 1    193         sit, nat version bitmap     
} __packed;

/*
 * For orphan inode management
 */
#define F2FS_ORPHANS_PER_BLOCK	1020

#define GET_ORPHAN_BLOCKS(n)	((n + F2FS_ORPHANS_PER_BLOCK - 1) / \
					F2FS_ORPHANS_PER_BLOCK)

struct f2fs_orphan_block {
	__le32 ino[F2FS_ORPHANS_PER_BLOCK];	/* inode numbers */
	__le32 reserved;	/* reserved */
	__le16 blk_addr;	/* block index in current CP */
	__le16 blk_count;	/* Number of orphan inode blocks in CP */
	__le32 entry_count;	/* Total number of orphan nodes in current CP */
	__le32 check_sum;	/* CRC32 for orphan inode block */
} __packed;

/*
 * For NODE structure
 */
struct f2fs_extent {
	__le32 fofs;		/* start file offset of the extent */               // 4    4
	__le32 blk;		/* start block address of the extent */                 // 4    8
	__le32 len;		/* lengh of the extent */                               // 4    12
} __packed;

#define F2FS_NAME_LEN		255
#define F2FS_INLINE_XATTR_ADDRS	50	/* 200 bytes for inline xattrs */
#define DEF_ADDRS_PER_INODE	923	/* Address Pointers in an Inode */
#define DEF_NIDS_PER_INODE	5	/* Node IDs in an Inode */
#define ADDRS_PER_INODE(inode)	addrs_per_inode(inode)
#define ADDRS_PER_BLOCK		1018	/* Address Pointers in a Direct Block */
#define NIDS_PER_BLOCK		1018	/* Node IDs in an Indirect Block */

#define ADDRS_PER_PAGE(page, inode)	\
	(IS_INODE(page) ? ADDRS_PER_INODE(inode) : ADDRS_PER_BLOCK)

#define	NODE_DIR1_BLOCK		(DEF_ADDRS_PER_INODE + 1)
#define	NODE_DIR2_BLOCK		(DEF_ADDRS_PER_INODE + 2)
#define	NODE_IND1_BLOCK		(DEF_ADDRS_PER_INODE + 3)
#define	NODE_IND2_BLOCK		(DEF_ADDRS_PER_INODE + 4)
#define	NODE_DIND_BLOCK		(DEF_ADDRS_PER_INODE + 5)

#define F2FS_INLINE_XATTR	0x01	/* file inline xattr flag */
#define F2FS_INLINE_DATA	0x02	/* file inline data flag */
#define F2FS_INLINE_DENTRY	0x04	/* file inline dentry flag */
#define F2FS_DATA_EXIST		0x08	/* file inline data exist flag */
#define F2FS_INLINE_DOTS	0x10	/* file having implicit dot dentries */

#define MAX_INLINE_DATA		(sizeof(__le32) * (DEF_ADDRS_PER_INODE - \
						F2FS_INLINE_XATTR_ADDRS - 1))

struct f2fs_inode { // -> root inode : 0xffd00000 
	__le16 i_mode;			/* file mode */                                 // 2    2           inode mode          : 0x41ed  
	__u8 i_advise;			/* file hints */                                // 1    3           file hints          : 0x00
	__u8 i_inline;			/* file inline flags */                         // 1    4           file flag           : 0x00
	__le32 i_uid;			/* user ID */                                   // 4    8           user id             : 0x00000000
	__le32 i_gid;			/* group ID */                                  // 4    12          group id            : 0x00000000
	__le32 i_links;			/* links count */                               // 4    16      -1  inode links         : 0x00000002
	__le64 i_size;			/* file size in bytes */                        // 8    24          file size(byte)     : 0x00000000 00001000 bytes
	__le64 i_blocks;		/* file size in blocks */                       // 8    32      -2  file size(blocks)   : 0x00000000 00000002 blocks
	__le64 i_atime;			/* access time */                               // 8    40          access time         : 0x00000000 5d36b494
	__le64 i_ctime;			/* change time */                               // 8    48      -3  create time         : 0x00000000 5d36b494
	__le64 i_mtime;			/* modification time */                         // 8    56          modification time   : 0x00000000 5d36b494
	__le32 i_atime_nsec;		/* access time in nano scale */             // 4    60          atime nano          : 0x00000000
	__le32 i_ctime_nsec;		/* change time in nano scale */             // 4    64      -4  ctime nano          : 0x00000000
	__le32 i_mtime_nsec;		/* modification time in nano scale */       // 4    68          mtime nano          : 0x00000000
	__le32 i_generation;		/* file version (for NFS) */                // 4    72          i generation 
	__le32 i_current_depth;		/* only for directory depth */              // 4    76          dir depth           : 0x00000001
	__u8 i_name[F2FS_NAME_LEN];	/* file name for SPOR */                    // 255  347
	__u8 i_dir_level;		/* dentry_level for large dir */                // 1    348

	struct f2fs_extent i_ext;	/* caching a largest extent */              // 12   360

	__le32 i_addr[DEF_ADDRS_PER_INODE];	/* Pointers to data blocks */       // 3692 4052        data block addr     : 0x000ff700 blocks => 0xff700000 bytes

	__le32 i_nid[DEF_NIDS_PER_INODE];	/* direct(2), indirect(2),          // 20   4072
						double_indirect(1) node id */
} __packed;

struct direct_node {
	__le32 addr[ADDRS_PER_BLOCK];	/* array of data block address */       // 4072(4*1018)
} __packed;

struct indirect_node {
	__le32 nid[NIDS_PER_BLOCK];	/* array of data block address */           // 4072(4*1018)
} __packed;

enum {
	COLD_BIT_SHIFT = 0,
	FSYNC_BIT_SHIFT,
	DENT_BIT_SHIFT,
	OFFSET_BIT_SHIFT
};

#define OFFSET_BIT_MASK		(0x07)	/* (0x01 << OFFSET_BIT_SHIFT) - 1 */

struct node_footer {
	__le32 nid;		/* node id */                                           // 4    4
	__le32 ino;		/* inode nunmber */                                     // 4    8
	__le32 flag;		/* include cold/fsync/dentry marks and offset */    // 4    12
	__le64 cp_ver;		/* checkpoint version */                            // 8    20
	__le32 next_blkaddr;	/* next node page block address */              // 4    24
} __packed;     

/*
 * ffd00fe0: 00000000 00000000 03000000 03000000  ................
 * ffd00ff0: 00000000 01000000 00000000 01fd0f00  ................
 */

struct f2fs_node {
	/* can be one of three types: inode, direct, and indirect types */
	union {
		struct f2fs_inode i;
		struct direct_node dn;
		struct indirect_node in;
	};                                                                      // 4072 4072
	struct node_footer footer;                                              // 24   4096
} __packed;// 4KB

/*
 * For NAT entries
 */
#define NAT_ENTRY_PER_BLOCK (PAGE_SIZE / sizeof(struct f2fs_nat_entry))
#define NAT_ENTRY_BITMAP_SIZE	((NAT_ENTRY_PER_BLOCK + 7) / 8)

struct f2fs_nat_entry { /// -> 0x900000
	__u8 version;		/* latest version of cached nat entry */            // 1    1
	__le32 ino;		/* inode number */                                      // 4    5   
	__le32 block_addr;	/* block address */                                 // 4    9
} __packed;

struct f2fs_nat_block { // -> 0x900000
	struct f2fs_nat_entry entries[NAT_ENTRY_PER_BLOCK];
} __packed;
/*
 *  ver ino      blkaddr
 *  00  00000000 00000000 
 *  00  01000000 01000000 -> node inode
 *  00  02000000 01000000 -> meta inode
 *  00  03000000 00fd0f00 -> root inode : 0xffd00000 
 *  00  00000000 00000000
 *  ... 
 *  CP 의 next_free_nid 는 0x04
 */

/*
 * For SIT entries
 *
 * Each segment is 2MB in size by default so that a bitmap for validity of
 * there-in blocks should occupy 64 bytes, 512 bits.
 * Not allow to change this.
 */
#define SIT_VBLOCK_MAP_SIZE 64
#define SIT_ENTRY_PER_BLOCK (PAGE_SIZE / sizeof(struct f2fs_sit_entry))

/*
 * Note that f2fs_sit_entry->vblocks has the following bit-field information.
 * [15:10] : allocation type such as CURSEG_XXXX_TYPE
 * [9:0] : valid block count
 */
#define SIT_VBLOCKS_SHIFT	10
#define SIT_VBLOCKS_MASK	((1 << SIT_VBLOCKS_SHIFT) - 1)
#define GET_SIT_VBLOCKS(raw_sit)				\
	(le16_to_cpu((raw_sit)->vblocks) & SIT_VBLOCKS_MASK)
#define GET_SIT_TYPE(raw_sit)					\
	((le16_to_cpu((raw_sit)->vblocks) & ~SIT_VBLOCKS_MASK)	\
	 >> SIT_VBLOCKS_SHIFT)

struct f2fs_sit_entry { // -> 0x500000
	__le16 vblocks;				/* reference above */                       // 2    2
	__u8 valid_map[SIT_VBLOCK_MAP_SIZE];	/* bitmap for valid blocks */   // 64   66
	__le64 mtime;				/* segment age for cleaning */              // 8    74
} __packed;

struct f2fs_sit_block {     // -> 0x500000
	struct f2fs_sit_entry entries[SIT_ENTRY_PER_BLOCK];                     // 4K/sizeof(f2fs_sit_entry)
} __packed;

/*
 * For segment summary
 *
 * One summary block contains exactly 512 summary entries, which represents
 * exactly 2MB segment by default. Not allow to change the basic units.
 *
 * NOTE: For initializing fields, you must use set_summary
 *
 * - If data page, nid represents dnode's nid
 * - If node page, nid represents the node page's nid.
 *
 * The ofs_in_node is used by only data page. It represents offset
 * from node's page's beginning to get a data block address.
 * ex) data_blkaddr = (block_t)(nodepage_start_address + ofs_in_node)
 */
#define ENTRIES_IN_SUM		512
#define	SUMMARY_SIZE		(7)	/* sizeof(struct summary) */
#define	SUM_FOOTER_SIZE		(5)	/* sizeof(struct summary_footer) */
#define SUM_ENTRY_SIZE		(SUMMARY_SIZE * ENTRIES_IN_SUM)

/* a summary entry for a 4KB-sized block in a segment */
struct f2fs_summary {
	__le32 nid;		/* parent node id */
	union {
		__u8 reserved[3];
		struct {
			__u8 version;		/* node version number */
			__le16 ofs_in_node;	/* block index in parent node */
		} __packed;
	};
} __packed;

/* summary block type, node or data, is stored to the summary_footer */
#define SUM_TYPE_NODE		(1)
#define SUM_TYPE_DATA		(0)

struct summary_footer {
	unsigned char entry_type;	/* SUM_TYPE_XXX */
	__le32 check_sum;		/* summary checksum */
} __packed;

#define SUM_JOURNAL_SIZE	(F2FS_BLKSIZE - SUM_FOOTER_SIZE -\
				SUM_ENTRY_SIZE)
#define NAT_JOURNAL_ENTRIES	((SUM_JOURNAL_SIZE - 2) /\
				sizeof(struct nat_journal_entry))
#define NAT_JOURNAL_RESERVED	((SUM_JOURNAL_SIZE - 2) %\
				sizeof(struct nat_journal_entry))
#define SIT_JOURNAL_ENTRIES	((SUM_JOURNAL_SIZE - 2) /\
				sizeof(struct sit_journal_entry))
#define SIT_JOURNAL_RESERVED	((SUM_JOURNAL_SIZE - 2) %\
				sizeof(struct sit_journal_entry))

/* Reserved area should make size of f2fs_extra_info equals to
 * that of nat_journal and sit_journal.
 */
#define EXTRA_INFO_RESERVED	(SUM_JOURNAL_SIZE - 2 - 8)

/*
 * frequently updated NAT/SIT entries can be stored in the spare area in
 * summary blocks
 */
enum {
	NAT_JOURNAL = 0,
	SIT_JOURNAL
};

struct nat_journal_entry {
	__le32 nid;
	struct f2fs_nat_entry ne;
} __packed;

struct nat_journal {
	struct nat_journal_entry entries[NAT_JOURNAL_ENTRIES];
	__u8 reserved[NAT_JOURNAL_RESERVED];
} __packed;

struct sit_journal_entry {
	__le32 segno;
	struct f2fs_sit_entry se;
} __packed;

struct sit_journal {
	struct sit_journal_entry entries[SIT_JOURNAL_ENTRIES];
	__u8 reserved[SIT_JOURNAL_RESERVED];
} __packed;

struct f2fs_extra_info {
	__le64 kbytes_written;
	__u8 reserved[EXTRA_INFO_RESERVED];
} __packed;

struct f2fs_journal {
	union {
		__le16 n_nats;
		__le16 n_sits;
	};
	/* spare area is used by NAT or SIT journals or extra info */
	union {
		struct nat_journal nat_j;
		struct sit_journal sit_j;
		struct f2fs_extra_info info;
	};
} __packed;

/* 4KB-sized summary block structure */
struct f2fs_summary_block {
	struct f2fs_summary entries[ENTRIES_IN_SUM];
	struct f2fs_journal journal;
	struct summary_footer footer;
} __packed;

/*
 * For directory operations
 */
#define F2FS_DOT_HASH		0
#define F2FS_DDOT_HASH		F2FS_DOT_HASH
#define F2FS_MAX_HASH		(~((0x3ULL) << 62))
#define F2FS_HASH_COL_BIT	((0x1ULL) << 63)

typedef __le32	f2fs_hash_t;

/* One directory entry slot covers 8bytes-long file name */
#define F2FS_SLOT_LEN		8
#define F2FS_SLOT_LEN_BITS	3

#define GET_DENTRY_SLOTS(x)	((x + F2FS_SLOT_LEN - 1) >> F2FS_SLOT_LEN_BITS)

/* MAX level for dir lookup */
#define MAX_DIR_HASH_DEPTH	63

/* MAX buckets in one level of dir */
#define MAX_DIR_BUCKETS		(1 << ((MAX_DIR_HASH_DEPTH / 2) - 1))

/*
 * space utilization of regular dentry and inline dentry
 *		regular dentry			inline dentry
 * bitmap	1 * 27 = 27			1 * 23 = 23
 * reserved	1 * 3 = 3			1 * 7 = 7
 * dentry	11 * 214 = 2354			11 * 182 = 2002
 * filename	8 * 214 = 1712			8 * 182 = 1456
 * total	4096				3488
 *
 * Note: there are more reserved space in inline dentry than in regular
 * dentry, when converting inline dentry we should handle this carefully.
 */
#define NR_DENTRY_IN_BLOCK	214	/* the number of dentry in a block */
#define SIZE_OF_DIR_ENTRY	11	/* by byte */
#define SIZE_OF_DENTRY_BITMAP	((NR_DENTRY_IN_BLOCK + BITS_PER_BYTE - 1) / \
					BITS_PER_BYTE)
#define SIZE_OF_RESERVED	(PAGE_SIZE - ((SIZE_OF_DIR_ENTRY + \
				F2FS_SLOT_LEN) * \
				NR_DENTRY_IN_BLOCK + SIZE_OF_DENTRY_BITMAP))

/* One directory entry slot representing F2FS_SLOT_LEN-sized file name */
struct f2fs_dir_entry {
	__le32 hash_code;	/* hash code of file name */                    // 4    4
	__le32 ino;		/* inode number */                                  // 4    8
	__le16 name_len;	/* lengh of file name */                        // 2    10
	__u8 file_type;		/* file type */                                 // 1    11
} __packed;

/* 4KB-sized directory entry block */
struct f2fs_dentry_block {
	/* validity bitmap for directory entries in each block */
	__u8 dentry_bitmap[SIZE_OF_DENTRY_BITMAP];                          // 27       27
	__u8 reserved[SIZE_OF_RESERVED];                                    // 3        30
	struct f2fs_dir_entry dentry[NR_DENTRY_IN_BLOCK];                   // 2354     2384
	__u8 filename[NR_DENTRY_IN_BLOCK][F2FS_SLOT_LEN];                   // 1712     4096
} __packed;                                             

/* for inline dir */
#define NR_INLINE_DENTRY	(MAX_INLINE_DATA * BITS_PER_BYTE / \
				((SIZE_OF_DIR_ENTRY + F2FS_SLOT_LEN) * \
				BITS_PER_BYTE + 1))
#define INLINE_DENTRY_BITMAP_SIZE	((NR_INLINE_DENTRY + \
					BITS_PER_BYTE - 1) / BITS_PER_BYTE)
#define INLINE_RESERVED_SIZE	(MAX_INLINE_DATA - \
				((SIZE_OF_DIR_ENTRY + F2FS_SLOT_LEN) * \
				NR_INLINE_DENTRY + INLINE_DENTRY_BITMAP_SIZE))

/* inline directory entry structure */
struct f2fs_inline_dentry {
	__u8 dentry_bitmap[INLINE_DENTRY_BITMAP_SIZE];
	__u8 reserved[INLINE_RESERVED_SIZE];
	struct f2fs_dir_entry dentry[NR_INLINE_DENTRY];
	__u8 filename[NR_INLINE_DENTRY][F2FS_SLOT_LEN];
} __packed;

/* file types used in inode_info->flags */
enum {
	F2FS_FT_UNKNOWN,
	F2FS_FT_REG_FILE,
	F2FS_FT_DIR,
	F2FS_FT_CHRDEV,
	F2FS_FT_BLKDEV,
	F2FS_FT_FIFO,
	F2FS_FT_SOCK,
	F2FS_FT_SYMLINK,
	F2FS_FT_MAX
};

#define S_SHIFT 12

#endif  /* _LINUX_F2FS_FS_H */
