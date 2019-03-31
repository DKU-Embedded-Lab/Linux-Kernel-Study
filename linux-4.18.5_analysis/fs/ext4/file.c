// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/ext4/file.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext4 fs regular file handling primitives
 *
 *  64-bit file support on 64-bit platforms by Jakub Jelinek
 *	(jj@sunsite.ms.mff.cuni.cz)
 */

#include <linux/time.h>
#include <linux/fs.h>
#include <linux/iomap.h>
#include <linux/mount.h>
#include <linux/path.h>
#include <linux/dax.h>
#include <linux/quotaops.h>
#include <linux/pagevec.h>
#include <linux/uio.h>
#include <linux/mman.h>
#include "ext4.h"
#include "ext4_jbd2.h"
#include "xattr.h"
#include "acl.h"

#ifdef CONFIG_FS_DAX
static ssize_t ext4_dax_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	if (!inode_trylock_shared(inode)) {
		if (iocb->ki_flags & IOCB_NOWAIT)
			return -EAGAIN;
		inode_lock_shared(inode);
	}
	/*
	 * Recheck under inode lock - at this point we are sure it cannot
	 * change anymore
	 */
	if (!IS_DAX(inode)) {
		inode_unlock_shared(inode);
		/* Fallback to buffered IO in case we cannot support DAX */
		return generic_file_read_iter(iocb, to);
	}
	ret = dax_iomap_rw(iocb, to, &ext4_iomap_ops);
	inode_unlock_shared(inode);

	file_accessed(iocb->ki_filp);
	return ret;
}
#endif

static ssize_t ext4_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	if (unlikely(ext4_forced_shutdown(EXT4_SB(file_inode(iocb->ki_filp)->i_sb))))
		return -EIO;

	if (!iov_iter_count(to))
		return 0; /* skip atime */

#ifdef CONFIG_FS_DAX
	if (IS_DAX(file_inode(iocb->ki_filp)))
		return ext4_dax_read_iter(iocb, to);
#endif
	return generic_file_read_iter(iocb, to);
}

/*
 * Called when an inode is released. Note that this is different
 * from ext4_file_open: open gets called at every open, but release
 * gets called only when /all/ the files are closed.
 */
static int ext4_release_file(struct inode *inode, struct file *filp)
{
	if (ext4_test_inode_state(inode, EXT4_STATE_DA_ALLOC_CLOSE)) {
		ext4_alloc_da_blocks(inode);
		ext4_clear_inode_state(inode, EXT4_STATE_DA_ALLOC_CLOSE);
	}
	/* if we are the last writer on the inode, drop the block reservation */
	if ((filp->f_mode & FMODE_WRITE) &&
			(atomic_read(&inode->i_writecount) == 1) &&
		        !EXT4_I(inode)->i_reserved_data_blocks)
	{
		down_write(&EXT4_I(inode)->i_data_sem);
		ext4_discard_preallocations(inode);
		up_write(&EXT4_I(inode)->i_data_sem);
	}
	if (is_dx(inode) && filp->private_data)
		ext4_htree_free_dir_info(filp->private_data);

	return 0;
}

static void ext4_unwritten_wait(struct inode *inode)
{
	wait_queue_head_t *wq = ext4_ioend_wq(inode);

	wait_event(*wq, (atomic_read(&EXT4_I(inode)->i_unwritten) == 0));
}

/*
 * This tests whether the IO in question is block-aligned or not.
 * Ext4 utilizes unwritten extents when hole-filling during direct IO, and they
 * are converted to written only after the IO is complete.  Until they are
 * mapped, these blocks appear as holes, so dio_zero_block() will assume that
 * it needs to zero out portions of the start and/or end block.  If 2 AIO
 * threads are at work on the same unwritten block, they must be synchronized
 * or one thread will zero the other's data, causing corruption.
 */
static int
ext4_unaligned_aio(struct inode *inode, struct iov_iter *from, loff_t pos)
{
	struct super_block *sb = inode->i_sb;
	int blockmask = sb->s_blocksize - 1;

	if (pos >= i_size_read(inode))
		return 0;

	if ((pos | iov_iter_alignment(from)) & blockmask)
		return 1;

	return 0;
}

/* Is IO overwriting allocated and initialized blocks? */
static bool ext4_overwrite_io(struct inode *inode, loff_t pos, loff_t len)
{
	struct ext4_map_blocks map;
	unsigned int blkbits = inode->i_blkbits;
	int err, blklen;

	if (pos + len > i_size_read(inode))
		return false;

	map.m_lblk = pos >> blkbits;
	map.m_len = EXT4_MAX_BLOCKS(len, pos, blkbits);
	blklen = map.m_len;

	err = ext4_map_blocks(NULL, inode, &map, 0);
	/*
	 * 'err==len' means that all of the blocks have been preallocated,
	 * regardless of whether they have been initialized or not. To exclude
	 * unwritten extents, we need to check m_flags.
	 */
	return err == blklen && (map.m_flags & EXT4_MAP_MAPPED);
}

static ssize_t ext4_write_checks(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		return ret;
	/*
	 * If we have encountered a bitmap-format file, the size limit
	 * is smaller than s_maxbytes, which is for extent-mapped files.
	 */
	if (!(ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))) {
		struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);

		if (iocb->ki_pos >= sbi->s_bitmap_maxbytes)
			return -EFBIG;
		iov_iter_truncate(from, sbi->s_bitmap_maxbytes - iocb->ki_pos);
	}
	return iov_iter_count(from);
}

#ifdef CONFIG_FS_DAX
static ssize_t
ext4_dax_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	if (!inode_trylock(inode)) {
		if (iocb->ki_flags & IOCB_NOWAIT)
			return -EAGAIN;
		inode_lock(inode);
	}
	ret = ext4_write_checks(iocb, from);
	if (ret <= 0)
		goto out;
	ret = file_remove_privs(iocb->ki_filp);
	if (ret)
		goto out;
	ret = file_update_time(iocb->ki_filp);
	if (ret)
		goto out;

	ret = dax_iomap_rw(iocb, from, &ext4_iomap_ops);
out:
	inode_unlock(inode);
	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}
#endif

static ssize_t
ext4_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	int o_direct = iocb->ki_flags & IOCB_DIRECT;
	int unaligned_aio = 0;
	int overwrite = 0;
	ssize_t ret;

	if (unlikely(ext4_forced_shutdown(EXT4_SB(inode->i_sb))))
		return -EIO;

#ifdef CONFIG_FS_DAX
	if (IS_DAX(inode))
		return ext4_dax_write_iter(iocb, from);
#endif
	if (!o_direct && (iocb->ki_flags & IOCB_NOWAIT))
		return -EOPNOTSUPP;

	if (!inode_trylock(inode)) {
		if (iocb->ki_flags & IOCB_NOWAIT)
			return -EAGAIN;
		inode_lock(inode);
	}

	ret = ext4_write_checks(iocb, from);
	if (ret <= 0)
		goto out;

	/*
	 * Unaligned direct AIO must be serialized among each other as zeroing
	 * of partial blocks of two competing unaligned AIOs can result in data
	 * corruption.
	 */
	if (o_direct && ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS) &&
	    !is_sync_kiocb(iocb) &&
	    ext4_unaligned_aio(inode, from, iocb->ki_pos)) {
		unaligned_aio = 1;
		ext4_unwritten_wait(inode);
	}

	iocb->private = &overwrite;
	/* Check whether we do a DIO overwrite or not */
	if (o_direct && !unaligned_aio) {
		if (ext4_overwrite_io(inode, iocb->ki_pos, iov_iter_count(from))) {
			if (ext4_should_dioread_nolock(inode))
				overwrite = 1;
		} else if (iocb->ki_flags & IOCB_NOWAIT) {
			ret = -EAGAIN;
			goto out;
		}
	}

	ret = __generic_file_write_iter(iocb, from);
	inode_unlock(inode);

	if (ret > 0)
		ret = generic_write_sync(iocb, ret);

	return ret;

out:
	inode_unlock(inode);
	return ret;
}

#ifdef CONFIG_FS_DAX
static vm_fault_t ext4_dax_huge_fault(struct vm_fault *vmf,
		enum page_entry_size pe_size)
{
	int error = 0;
	vm_fault_t result;
	int retries = 0;
	handle_t *handle = NULL;
	struct inode *inode = file_inode(vmf->vma->vm_file);
	struct super_block *sb = inode->i_sb;

	/*
	 * We have to distinguish real writes from writes which will result in a
	 * COW page; COW writes should *not* poke the journal (the file will not
	 * be changed). Doing so would cause unintended failures when mounted
	 * read-only.
	 *
	 * We check for VM_SHARED rather than vmf->cow_page since the latter is
	 * unset for pe_size != PE_SIZE_PTE (i.e. only in do_cow_fault); for
	 * other sizes, dax_iomap_fault will handle splitting / fallback so that
	 * we eventually come back with a COW page.
	 */
	bool write = (vmf->flags & FAULT_FLAG_WRITE) &&
		(vmf->vma->vm_flags & VM_SHARED);
	pfn_t pfn;

	if (write) {
		sb_start_pagefault(sb);
		file_update_time(vmf->vma->vm_file);
		down_read(&EXT4_I(inode)->i_mmap_sem);
retry:
		handle = ext4_journal_start_sb(sb, EXT4_HT_WRITE_PAGE,
					       EXT4_DATA_TRANS_BLOCKS(sb));
		if (IS_ERR(handle)) {
			up_read(&EXT4_I(inode)->i_mmap_sem);
			sb_end_pagefault(sb);
			return VM_FAULT_SIGBUS;
		}
	} else {
		down_read(&EXT4_I(inode)->i_mmap_sem);
	}
	result = dax_iomap_fault(vmf, pe_size, &pfn, &error, &ext4_iomap_ops);
	if (write) {
		ext4_journal_stop(handle);

		if ((result & VM_FAULT_ERROR) && error == -ENOSPC &&
		    ext4_should_retry_alloc(sb, &retries))
			goto retry;
		/* Handling synchronous page fault? */
		if (result & VM_FAULT_NEEDDSYNC)
			result = dax_finish_sync_fault(vmf, pe_size, pfn);
		up_read(&EXT4_I(inode)->i_mmap_sem);
		sb_end_pagefault(sb);
	} else {
		up_read(&EXT4_I(inode)->i_mmap_sem);
	}

	return result;
}

static vm_fault_t ext4_dax_fault(struct vm_fault *vmf)
{
	return ext4_dax_huge_fault(vmf, PE_SIZE_PTE);
}

static const struct vm_operations_struct ext4_dax_vm_ops = {
	.fault		= ext4_dax_fault,
	.huge_fault	= ext4_dax_huge_fault,
	.page_mkwrite	= ext4_dax_fault,
	.pfn_mkwrite	= ext4_dax_fault,
};
#else
#define ext4_dax_vm_ops	ext4_file_vm_ops
#endif

static const struct vm_operations_struct ext4_file_vm_ops = {
	.fault		= ext4_filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite   = ext4_page_mkwrite,
};

static int ext4_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file->f_mapping->host;

	if (unlikely(ext4_forced_shutdown(EXT4_SB(inode->i_sb))))
		return -EIO;

	/*
	 * We don't support synchronous mappings for non-DAX files. At least
	 * until someone comes with a sensible use case.
	 */
	if (!IS_DAX(file_inode(file)) && (vma->vm_flags & VM_SYNC))
		return -EOPNOTSUPP;

	file_accessed(file);
	if (IS_DAX(file_inode(file))) {
		vma->vm_ops = &ext4_dax_vm_ops;
		vma->vm_flags |= VM_MIXEDMAP | VM_HUGEPAGE;
	} else {
		vma->vm_ops = &ext4_file_vm_ops;
	}
	return 0;
}

static int ext4_sample_last_mounted(struct super_block *sb,
				    struct vfsmount *mnt)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);		//ext4 super block에서 파일시스템 정보를 가져옴
	struct path path;				//vfsmount의 포인터 위치에서 가져온다. 파일시스템 정보 포인터 경로
	char buf[64], *cp;				//버퍼 및 복사 포인터
	handle_t *handle;				//jbd2_journal_handle 구조체, 저널링의 위한 구조체
	int err;					//에러 변수
	
	// mount option 확인
	// sbi는 ext4의 슈퍼블록으로 마운트 정보를 가지고 있다.
	// EXT4_MF_MNTDIR_SAMPLED는 현재 마운트가 되어있는가를 확인하는 플래그이다.
	// 이 플래그가 확인되면 굳이 한번 더 마운트할 필요가 없기때문에 있는 정보를 활용한다.
	// 따라서, 있다면 아무 문제가 없는 것이므로 0 바로 반환
	// * 왜 샘플이라고 표현했는가는 궁금함.
	if (likely(sbi->s_mount_flags & EXT4_MF_MNTDIR_SAMPLED))
		return 0;
	
	// sb_rdonly(sb) : 슈퍼블록이 읽기전용인지 확인한다.
	// sb_start_intwrite_trylock(sb) : 슈퍼 블록에 대한 쓰기 락 획득
	// 위 두 함수를 이용하여 읽기전용이나 쓰기 lock을 잡았거나 둘 중 하나 일때, 그래도 0을 반환
	// 즉, 쓰기권한을 가지고 저널을 기록하거나, 기록할 필요없는 읽기전용인지를 확인한다.
	// ************** lock부분에 대한 추가 설명 필요 
	if (sb_rdonly(sb) || !sb_start_intwrite_trylock(sb))
		return 0;
	
	//마운트처리를 위해 플래그 설정을 지정해준다.
	sbi->s_mount_flags |= EXT4_MF_MNTDIR_SAMPLED;
	/*
	 * Sample where the filesystem has been mounted and
	 * store it in the superblock for sysadmin convenience
	 * when trying to sort through large numbers of block
	 * devices or filesystem images.
	 */
	
	
	memset(buf, 0, sizeof(buf));				//버퍼 사이즈 설정
	path.mnt = mnt;						//파일시스템 정보 포인터를 연결
	path.dentry = mnt->mnt_root;				//파일 시스템의 루트 디렉토리 연결
	cp = d_path(&path, buf, sizeof(buf));			//경로 임시 복사
	err = 0;						//0이면 정상
	
	//경로 에러 검사
	if (IS_ERR(cp))
		goto out;
	
	//open journaling 시작
	//ext4_journal_start_sb() : micro 함수, __ext4_journal_start_sb() 호출
	//journaling 시작
	//**********journaling의 자세한 분석은 추후 예정
	handle = ext4_journal_start_sb(sb, EXT4_HT_MISC, 1);
	err = PTR_ERR(handle);					//journaling 에러 확인
	if (IS_ERR(handle))
		goto out;
	BUFFER_TRACE(sbi->s_sbh, "get_write_access");		//journaling 트레이싱
	
	//ext4_journal_get_write_access() : journal 기록을 시작한다.
	err = ext4_journal_get_write_access(handle, sbi->s_sbh); 
	if (err)						//에러처리
		goto out_journal;
	strlcpy(sbi->s_es->s_last_mounted, cp,			//기록할 내용
		sizeof(sbi->s_es->s_last_mounted));
	ext4_handle_dirty_super(handle, sb);			//journal 기록
out_journal:
	ext4_journal_stop(handle);				//journal 기록 중지
out:
	sb_end_intwrite(sb);					//슈퍼블록에 대한 쓰기 락 해제
	return err;						//결과 반환
}


//ext4 File Open function
static int ext4_file_open(struct inode * inode, struct file * filp)
{
	//반환값 저장변수 선언
	int ret;
	
	/*
		EXT4의 슈퍼블록이 오류가 있는 없는지 검사
		unlikely/likely 함수 : 거짓, 참 중에서 상대적으로 많이 발생하는 것을 지정하여
		컴파일러단에서 분기예측을 할 수 있게 도와줌.
		여기서는 일반적으로 오류가 나지 않으므로, 0을 주로 반환
		
		EXT4_SB() 함수 : return sb->s_fs_info로 Filesystem private info를 반환한다.
		
		ext4_forced_shutdown() 함수 : ext4 sbi의 flags를 비교하여 오류가 있는지 점검한다.
		에러면 -EIO 바로 반환

		결국은 에러 검사
		
		** 추가 필요 사항 **
		- test_bit함수를 사용하는 아직 어떻게 동작하는지 파악안됨.
	*/
	if (unlikely(ext4_forced_shutdown(EXT4_SB(inode->i_sb))))
		return -EIO;
	
	/*
		기존의 마운트를 했는지를 검사하는 함수이다.
		만약, 읽기전용이라면 저널을 할 필요가 없기때문에 그대로 반환
		아니라면, 마운트한 저널 내용을 기록하고 슈퍼블록 정보를 갱신한다.
		문제가 있다면 true가 반환되어 open이 중지된다.
	*/	
	ret = ext4_sample_last_mounted(inode->i_sb, filp->f_path.mnt);
	if (ret)
		return ret;
	
	/*
		암호화된 파일시스템일 경우 key를 먼저 획득해야한다.
		확인을 한 뒤 열지말지 결정한다.
		0이 반환되면 성공	
	*/
	ret = fscrypt_file_open(inode, filp);
	if (ret)
		return ret;

	/*
	 * Set up the jbd2_inode if we are opening the inode for
	 * writing and the journal is present
	 */
	if (filp->f_mode & FMODE_WRITE) {
		ret = ext4_inode_attach_jinode(inode);
		if (ret < 0)
			return ret;
	}
	
	//i/o가 차단될 경우 -EAGIN이 반환될 수 있다. --> FMODE_NOWAIT
	filp->f_mode |= FMODE_NOWAIT;
	//dquot_file_open() : generic_file_open()을 호출한다.
	return dquot_file_open(inode, filp);
}

/*
 * ext4_llseek() handles both block-mapped and extent-mapped maxbytes values
 * by calling generic_file_llseek_size() with the appropriate maxbytes
 * value for each.
 */
loff_t ext4_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;
	loff_t maxbytes;

	if (!(ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS)))
		maxbytes = EXT4_SB(inode->i_sb)->s_bitmap_maxbytes;
	else
		maxbytes = inode->i_sb->s_maxbytes;

	switch (whence) {
	default:
		return generic_file_llseek_size(file, offset, whence,
						maxbytes, i_size_read(inode));
	case SEEK_HOLE:
		inode_lock_shared(inode);
		offset = iomap_seek_hole(inode, offset, &ext4_iomap_ops);
		inode_unlock_shared(inode);
		break;
	case SEEK_DATA:
		inode_lock_shared(inode);
		offset = iomap_seek_data(inode, offset, &ext4_iomap_ops);
		inode_unlock_shared(inode);
		break;
	}

	if (offset < 0)
		return offset;
	return vfs_setpos(file, offset, maxbytes);
}

const struct file_operations ext4_file_operations = {
	.llseek		= ext4_llseek,
	.read_iter	= ext4_file_read_iter,
	.write_iter	= ext4_file_write_iter,
	.unlocked_ioctl = ext4_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= ext4_compat_ioctl,
#endif
	.mmap		= ext4_file_mmap,
	.mmap_supported_flags = MAP_SYNC,
	.open		= ext4_file_open,
	.release	= ext4_release_file,
	.fsync		= ext4_sync_file,
	.get_unmapped_area = thp_get_unmapped_area,
	.splice_read	= generic_file_splice_read,
	.splice_write	= iter_file_splice_write,
	.fallocate	= ext4_fallocate,
};

const struct inode_operations ext4_file_inode_operations = {
	.setattr	= ext4_setattr,
	.getattr	= ext4_file_getattr,
	.listxattr	= ext4_listxattr,
	.get_acl	= ext4_get_acl,
	.set_acl	= ext4_set_acl,
	.fiemap		= ext4_fiemap,
};

