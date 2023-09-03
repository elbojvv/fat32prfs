// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/fs/fat/file.c
 *
 *  Written 1992,1993 by Werner Almesberger
 *
 *  regular file handling primitives for fat-based filesystems
 */

#include <linux/capability.h>
#include <linux/module.h>
#include <linux/compat.h>
#include <linux/mount.h>
#include <linux/blkdev.h>
#include <linux/backing-dev.h>
#include <linux/fsnotify.h>
#include <linux/security.h>
#include <linux/falloc.h>
#include "fat_prfs.h"

// for writing files
#include <linux/kernel.h>
#include <linux/init.h>
//#include <linux/module.h>
#include <linux/syscalls.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fcntl.h>
#include <asm/uaccess.h>

#include <asm/div64.h>
//#include <linux/math.h>

static long fat_fallocate(struct file *file, int mode,
			  loff_t offset, loff_t len);

static int fat_ioctl_get_attributes(struct inode *inode, u32 __user *user_attr)
{
	u32 attr;

	inode_lock_shared(inode);
	attr = fat_make_attrs(inode);
	inode_unlock_shared(inode);

	return put_user(attr, user_attr);
}

static int fat_ioctl_set_attributes(struct file *file, u32 __user *user_attr)
{
	struct inode *inode = file_inode(file);
	struct msdos_sb_info *sbi = MSDOS_SB(inode->i_sb);
	int is_dir = S_ISDIR(inode->i_mode);
	u32 attr, oldattr;
	struct iattr ia;
	int err;

	err = get_user(attr, user_attr);
	if (err)
		goto out;

	err = mnt_want_write_file(file);
	if (err)
		goto out;
	inode_lock(inode);

	/*
	 * ATTR_VOLUME and ATTR_DIR cannot be changed; this also
	 * prevents the user from turning us into a VFAT
	 * longname entry.  Also, we obviously can't set
	 * any of the NTFS attributes in the high 24 bits.
	 */
	attr &= 0xff & ~(ATTR_VOLUME | ATTR_DIR);
	/* Merge in ATTR_VOLUME and ATTR_DIR */
	attr |= (MSDOS_I(inode)->i_attrs & ATTR_VOLUME) |
		(is_dir ? ATTR_DIR : 0);
	oldattr = fat_make_attrs(inode);

	/* Equivalent to a chmod() */
	ia.ia_valid = ATTR_MODE | ATTR_CTIME;
	ia.ia_ctime = current_time(inode);
	if (is_dir)
		ia.ia_mode = fat_make_mode(sbi, attr, S_IRWXUGO);
	else {
		ia.ia_mode = fat_make_mode(sbi, attr,
			S_IRUGO | S_IWUGO | (inode->i_mode & S_IXUGO));
	}

	/* The root directory has no attributes */
	if (inode->i_ino == MSDOS_ROOT_INO && attr != ATTR_DIR) {
		err = -EINVAL;
		goto out_unlock_inode;
	}

	if (sbi->options.sys_immutable &&
	    ((attr | oldattr) & ATTR_SYS) &&
	    !capable(CAP_LINUX_IMMUTABLE)) {
		err = -EPERM;
		goto out_unlock_inode;
	}

	/*
	 * The security check is questionable...  We single
	 * out the RO attribute for checking by the security
	 * module, just because it maps to a file mode.
	 */
	err = security_inode_setattr(file_mnt_user_ns(file),
				     file->f_path.dentry, &ia);
	if (err)
		goto out_unlock_inode;

	/* This MUST be done before doing anything irreversible... */
	err = fat_setattr_prfs(file_mnt_user_ns(file), file->f_path.dentry, &ia);
	if (err)
		goto out_unlock_inode;

	fsnotify_change(file->f_path.dentry, ia.ia_valid);
	if (sbi->options.sys_immutable) {
		if (attr & ATTR_SYS)
			inode->i_flags |= S_IMMUTABLE;
		else
			inode->i_flags &= ~S_IMMUTABLE;
	}

	fat_save_attrs(inode, attr);
	mark_inode_dirty(inode);
out_unlock_inode:
	inode_unlock(inode);
	mnt_drop_write_file(file);
out:
	return err;
}

static int fat_ioctl_get_volume_id(struct inode *inode, u32 __user *user_attr)
{
	struct msdos_sb_info *sbi = MSDOS_SB(inode->i_sb);
	return put_user(sbi->vol_id, user_attr);
}

static int fat_ioctl_fitrim(struct inode *inode, unsigned long arg)
{
	struct super_block *sb = inode->i_sb;
	struct fstrim_range __user *user_range;
	struct fstrim_range range;
	int err;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (!bdev_max_discard_sectors(sb->s_bdev))
		return -EOPNOTSUPP;

	user_range = (struct fstrim_range __user *)arg;
	if (copy_from_user(&range, user_range, sizeof(range)))
		return -EFAULT;

	range.minlen = max_t(unsigned int, range.minlen,
			     bdev_discard_granularity(sb->s_bdev));

	err = fat_trim_fs(inode, &range);
	if (err < 0)
		return err;

	if (copy_to_user(user_range, &range, sizeof(range)))
		return -EFAULT;

	return 0;
}

long fat_generic_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	u32 __user *user_attr = (u32 __user *)arg;

	//dbg printk(KERN_INFO "fat_generic_ioctl function...\n");

	switch (cmd) {
	case FAT_IOCTL_GET_ATTRIBUTES:
		return fat_ioctl_get_attributes(inode, user_attr);
	case FAT_IOCTL_SET_ATTRIBUTES:
		return fat_ioctl_set_attributes(filp, user_attr);
	case FAT_IOCTL_GET_VOLUME_ID:
		return fat_ioctl_get_volume_id(inode, user_attr);
	case FITRIM:
		return fat_ioctl_fitrim(inode, arg);
	default:
		return -ENOTTY;	/* Inappropriate ioctl for device */
	}
}

static int fat_file_release(struct inode *inode, struct file *filp)
{
	//dbg printk(KERN_INFO "fat_file_release function...\n");
	if ((filp->f_mode & FMODE_WRITE) &&
	    MSDOS_SB(inode->i_sb)->options.flush) {
		fat_flush_inodes_prfs(inode->i_sb, inode, NULL);
		set_current_state(TASK_UNINTERRUPTIBLE);
		io_schedule_timeout(HZ/10);
	}
	return 0;
}

int fat_file_fsync(struct file *filp, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = filp->f_mapping->host;
	int err;

	//dbg printk(KERN_INFO "fat_file_fsync function...\n");

	err = __generic_file_fsync(filp, start, end, datasync);
	if (err)
		return err;

	err = sync_mapping_buffers(MSDOS_SB(inode->i_sb)->fat_inode->i_mapping);
	if (err)
		return err;

	return blkdev_issue_flush(inode->i_sb->s_bdev);
}

// How to clone a file: https://stackoverflow.com/questions/60665151/clone-a-file-in-linux-kernel-module
// https://www.linuxjournal.com/article/8110

/*
static void write_file(char *filename, char *data)
{
  struct file *file;
  loff_t pos = 0;
  int fd;

  mm_segment_t old_fs = get_fs();
  set_fs(KERNEL_DS);

  fd = sys_open(filename, O_WRONLY|O_CREAT, 0644);
  if (fd >= 0) {
    sys_write(fd, data, strlen(data));
    file = fget(fd);
    if (file) {
      vfs_write(file, data, strlen(data), &pos);
      fput(file);
    }
    sys_close(fd);
  }
  set_fs(old_fs);
}
*/

// file_is_real
// returns 1 if exists and size > 0
// returns 0 otherwise
int file_is_real(char * fname)
{
	struct file *test_filp;
    int rc;
    struct kstat *stat;

	printk(KERN_INFO "file_is_real: %s\n", fname); 

	test_filp = filp_open(fname, O_RDONLY  , 0);
	if (IS_ERR(test_filp) || (test_filp == NULL)) 
	{
		printk(KERN_INFO "file_is_real: %s: not found\n", fname);
		return 0;
	}
	// get size
	stat =(struct kstat *) kmalloc(sizeof(struct kstat), GFP_KERNEL);
	rc = vfs_getattr(&test_filp->f_path, stat, STATX_SIZE, AT_STATX_SYNC_AS_STAT);
	filp_close(test_filp, NULL);
	if(rc == 0)
	{
		printk(KERN_INFO "file_is_real: %s: file size is zero\n", fname);
		return 0;
	}
	printk(KERN_INFO "file_is_real: %s: file exists. Size: %lu\n", fname, (long unsigned int)stat->size);
	return 1;
}	


// file_readwrite
// returns 0 if read
// returns 1 if modified (RDWR or WR)
int file_readwrite(struct file * filp)
{
	printk(KERN_INFO "file_readwrite: f_flags: %o\n", (int)(filp->f_flags)); 
	if (((int)(filp->f_flags) & 3) >0) return 1;
	return 0;
}

// file_justcreated
// returns 0 alread exists
// returns 1 just created (with vfat_create)
int file_justcreated(struct file * filp)
{
	printk(KERN_INFO "file_justcreated: f_flags: %o\n", (int)(filp->f_mode)); 
	if (((int)(filp->f_mode) & (O_SYNC)) > 0 ) return 1;
	return 0;
}

// filename_backup
// checks whether first part of filename is _NNNNNNNNNNNN_ (N=number)
// returns 0 is not backup filename
// returns 1 is backup filename
int filename_backup(const char * fname)
{
	int s_idx;
	if (strlen(fname) < 15) return 0;
	if (fname[0] != '_') return 0;
	if (fname[14] != '_') return 0;
	for (s_idx=1; s_idx<14; s_idx++)
	{
		if (fname[s_idx] < '0') return 0;
		if (fname[s_idx] > '9') return 0;
	}
	return 1;
}
EXPORT_SYMBOL_GPL(filename_backup);

// create_backup_filename_trailing
// create beginning _NNNNNNNNNNNN_ (N=number) of backup filename from present time
void create_backup_filename_trailing(char * fname, int len)
{
	struct timespec64 now;
	//long long unsigned int sc, ms;
	//float tsc;
	char stmp1[20], stmp2[20];
	
	if (len <16) 
	{
		printk(KERN_INFO "create_backup_filename_trailing: char * too small. Is: %i, should be at least 16.\n", len);
		return;
	}		
	ktime_get_real_ts64(&now);
	//tsc = (float)now.tv_sec;
	//sc = (long long unsigned int)(tsc - 1e10 * (float)((long long unsigned int)(tsc / 1e10)));
	//ms = (long long unsigned int)((float)now.tv_nsec / 1e6);
	//snprintf(fname, 15, "_%010llu%03llu_", sc, ms);
	snprintf(stmp1, 16, "%015llu", now.tv_sec);
	snprintf(stmp2, 10, "%09lu", now.tv_nsec);
	snprintf(fname, 16, "_%c%c%c%c%c%c%c%c%c%c%c%c%c_", 
		stmp1[5], stmp1[6], stmp1[7], stmp1[8], stmp1[9], stmp1[10], stmp1[11], stmp1[12], stmp1[13], stmp1[14],
		stmp2[0], stmp2[1], stmp2[2] );
	printk(KERN_INFO "create_backup_filename_trailing: %llu %lu %s %s %s\n", now.tv_sec, now.tv_nsec, stmp1, stmp2, fname);
}

// prfs_make_backup
// make backup file (with _NN..NN_)
// returns 0 on success
// returns -1 on failure
int prfs_make_backup(const char * fname)
{
	struct file *original_filp, *copy_filp;
	char fn2[260], tme[20]; 
	int snpres;

	create_backup_filename_trailing(tme, sizeof tme);
	snpres = snprintf(fn2, sizeof fn2, "%s%s", tme, fname);
	printk(KERN_INFO "prfs_make_backup: %s fn2: %s, res: %i\n", fname, fn2, snpres);
	// https://stackoverflow.com/questions/60665151/clone-a-file-in-linux-kernel-module
	//original_filp = fcheck(o_fd);
	printk(KERN_INFO "prfs_make_backup: open read file: %s\n", fname);
	original_filp = filp_open(fname, O_RDONLY  , 0);
	if (IS_ERR(original_filp) || (original_filp == NULL)) 
	{
		printk(KERN_INFO "prfs_make_backup: %s: error opening fname in copy: exiting\n", fname);
		return -1;
	}
	printk(KERN_INFO "prfs_make_backup: %s: open write file: %s\n", fname, fn2);
	copy_filp = filp_open(fn2, O_CREAT | O_RDWR  , 0644);
	if (IS_ERR(copy_filp) || (copy_filp == NULL)) 
	{
		printk(KERN_INFO "prfs_make_backup: %s: error opening %s in copy: exiting\n", fname, fn2);
		return -1;
	}
	printk(KERN_INFO "prfs_make_backup: %s: start copying files\n", fname);
	vfs_copy_file_range(original_filp, 0, copy_filp, 0, i_size_read(original_filp->f_inode), 0);
	printk(KERN_INFO "prfs_make_backup: %s: closing files\n", fname);
	filp_close(copy_filp, NULL);
	filp_close(original_filp, NULL);
	printk(KERN_INFO "prfs_make_backup: %s: finished copying\n", fname);
	return 0;
}
EXPORT_SYMBOL_GPL(prfs_make_backup);

int get_prfs_mode() 
{
	//struct file *prfsmode_filp;
	//char pmode[10]; 
	int prfs_mode; // 0: PRFS, 1: read only, 2: only backupfiles writable
/*
	// read PRFS mode from /mnt/ramdisk/prfsmode.txt
	prfsmode_filp = filp_open("/mnt/ramdisk/prfsmode.txt", O_RDONLY  , 0);
	if (IS_ERR(prfsmode_filp) || (prfsmode_filp == NULL)) 
	{
		// file does not exist: read only
		if (prfsmode_filp != NULL) filp_close(prfsmode_filp, NULL);
		prfs_mode=1;
		printk(KERN_INFO "get_prfs_mode: PRFS file does not exist: mode 1\n");
	} else 
	{
		pmode[0]='1';
		kernel_read(prfsmode_filp, pmode, sizeof pmode, &prfsmode_filp->f_pos);
		filp_close(prfsmode_filp, NULL);
		prfs_mode = pmode[0]-48;
		printk(KERN_INFO "get_prfs_mode: PRFS mode %i\n", prfs_mode);
	}
*/	
	prfs_mode = get_proc_prfs_mode();
	
	if (prfs_mode < 0) prfs_mode = 1;
	if (prfs_mode > 2) prfs_mode = 1;

	printk(KERN_INFO "get_prfs_mode (file.c): %i\n", prfs_mode);

	return prfs_mode;
}
EXPORT_SYMBOL_GPL(get_prfs_mode);

int prfs_file_open(struct inode * inode, struct file * filp)
{
	int rtv; // return value
	char fn1[260];
	int fcres;
	int prfs_mode; // 0: PRFS, 1: read only, 2: only backupfiles writable

	printk(KERN_INFO "prfs_file_open: *** %s, f_flags: %04o\n", filp->f_path.dentry->d_iname, (int)filp->f_flags);
	printk(KERN_INFO "prfs_file_open: %s, f_mode:  %04o\n", filp->f_path.dentry->d_iname, (int)filp->f_mode);
	printk(KERN_INFO "prfs_file_open: %s, i_state: %lu\n", filp->f_path.dentry->d_iname, inode->i_state );
	//printk(KERN_INFO "prfs_file_open: %s, d_lock: %i\n", filp->f_path.dentry->d_iname, (int)inode->i_flock->fl_flags ); // 
	//printk(KERN_INFO "prfs_file_open: %s, f_mode: %04o, i_sate: %i, d_lock: %lu\n", filp->f_path.dentry->d_iname, (int)filp->f_mode, inode->i_state, (int)inode->i_lock.rlock ); // .rlock.raw_lock
	
	prfs_mode = get_prfs_mode();
	
	strncpy ( fn1, filp->f_path.dentry->d_iname, sizeof(fn1) );
	//printk(KERN_INFO "prfs_file_open: fn1: %s\n", fn1);

		
	switch (prfs_mode) 
	{
		case 0: // PRFS MODE
			//printk(KERN_INFO "prfs_file_open: ff: %i, &3: %i\n", ff, ff & 3);
			if (file_readwrite(filp) == 1) // write 
			{  // writing
				printk(KERN_INFO "prfs_file_open: %s: write, need copy?\n", fn1);
				if (filename_backup(fn1) == 1) 
				{
					// no copy
					printk(KERN_INFO "prfs_file_open: %s: this is a backup filename: does not need copy.\n", fn1);
					// Check whether backup file exist
					if (file_justcreated(filp) == 0) {
						printk(KERN_INFO "prfs_file_open: %s: this backup file does exist; is WORM: exit writing\n", fn1);
						return -1;
					}
				} else {
					// check whether is new file (then no copy needed)
					if (file_justcreated(filp) == 1) 
					{
						printk(KERN_INFO "prfs_file_open: %s: this file does not (really) exist; no backup needed.\n", fn1);
					} else {
						// Make backup. If backup fails, block writing to the file
						printk(KERN_INFO "prfs_file_open: %s: no backup filename, does need copy\n", fn1);
						
						fcres = prfs_make_backup(fn1);
						if (fcres==-1) 
						{
							printk(KERN_INFO "prfs_file_open: %s: error making backup; access denied.\n", fn1);
							return -1;
						}
		/*				
						create_backup_filename_trailing(tme, sizeof tme);
						snpres = snprintf(fn2, sizeof fn2, "%s%s", tme, fn1);
						printk(KERN_INFO "prfs_file_open: %s fn2: %s, res: %i\n", fn1, fn2, snpres);
						// https://stackoverflow.com/questions/60665151/clone-a-file-in-linux-kernel-module
						//original_filp = fcheck(o_fd);
						printk(KERN_INFO "prfs_file_open: open read file: %s\n", fn1);
						original_filp = filp_open(fn1, O_RDONLY  , 0);
						if (IS_ERR(original_filp) || (original_filp == NULL)) 
						{
							printk(KERN_INFO "prfs_file_open: %s: error opening fn1 in copy: exiting\n", fn1);
							return -1;
						}
						printk(KERN_INFO "prfs_file_open: %s: open write file: %s\n", fn1, fn2);
						copy_filp = filp_open(fn2, O_CREAT | O_RDWR  , 0644);
						if (IS_ERR(copy_filp) || (copy_filp == NULL)) 
						{
							printk(KERN_INFO "prfs_file_open: %s: error opening %s in copy: exiting\n", fn1, fn2);
							return -1;
						}
						printk(KERN_INFO "prfs_file_open: %s: start copying files\n", fn1);
						vfs_copy_file_range(original_filp, 0, copy_filp, 0, i_size_read(original_filp->f_inode), 0);
						printk(KERN_INFO "prfs_file_open: %s: closing files\n", fn1);
						filp_close(copy_filp, NULL);
						filp_close(original_filp, NULL);
						printk(KERN_INFO "prfs_file_open: %s: finished copying\n", fn1);
		*/
					}
				}
			} else {
				printk(KERN_INFO "prfs_file_open: %s: reading; allowed\n", fn1);
			}
			break;
		
		case 1: // READ-ONLY
			if (file_readwrite(filp) == 1) // write 
			{  // writing
				return -1;
			}	
			break;
		
		
		case 2: // Only backup editable
			if (file_readwrite(filp) == 1) // write 
			{ 
				if (filename_backup(fn1) == 0)  // This is not a backup file; so no writing allowed
				{
					return -1;
				}					
			}
			break;
		
		default:
			printk(KERN_INFO "prfs_file_open: %s: INVALIDE PRFS mode: %i\n", fn1, prfs_mode);
			return -1;
	}
	
	rtv = generic_file_open(inode, filp);
	return rtv;
}

const struct file_operations fat_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.write_iter	= generic_file_write_iter,
	.mmap		= generic_file_mmap,
	.release	= fat_file_release,
	.unlocked_ioctl	= fat_generic_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.fsync		= fat_file_fsync,
	.splice_read	= generic_file_splice_read,
	.splice_write	= iter_file_splice_write,
	.fallocate	= fat_fallocate,
	.open		= prfs_file_open
};

static int fat_cont_expand(struct inode *inode, loff_t size)
{
	struct address_space *mapping = inode->i_mapping;
	loff_t start = inode->i_size, count = size - inode->i_size;
	int err;

	err = generic_cont_expand_simple(inode, size);
	if (err)
		goto out;

	fat_truncate_time_prfs(inode, NULL, S_CTIME|S_MTIME);
	mark_inode_dirty(inode);
	if (IS_SYNC(inode)) {
		int err2;

		/*
		 * Opencode syncing since we don't have a file open to use
		 * standard fsync path.
		 */
		err = filemap_fdatawrite_range(mapping, start,
					       start + count - 1);
		err2 = sync_mapping_buffers(mapping);
		if (!err)
			err = err2;
		err2 = write_inode_now(inode, 1);
		if (!err)
			err = err2;
		if (!err) {
			err =  filemap_fdatawait_range(mapping, start,
						       start + count - 1);
		}
	}
out:
	return err;
}

/*
 * Preallocate space for a file. This implements fat's fallocate file
 * operation, which gets called from sys_fallocate system call. User
 * space requests len bytes at offset. If FALLOC_FL_KEEP_SIZE is set
 * we just allocate clusters without zeroing them out. Otherwise we
 * allocate and zero out clusters via an expanding truncate.
 */
static long fat_fallocate(struct file *file, int mode,
			  loff_t offset, loff_t len)
{
	int nr_cluster; /* Number of clusters to be allocated */
	loff_t mm_bytes; /* Number of bytes to be allocated for file */
	loff_t ondisksize; /* block aligned on-disk size in bytes*/
	struct inode *inode = file->f_mapping->host;
	struct super_block *sb = inode->i_sb;
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	int err = 0;
	
	//dbg printk(KERN_INFO "fat_fallocate function...\n");

	/* No support for hole punch or other fallocate flags. */
	if (mode & ~FALLOC_FL_KEEP_SIZE)
		return -EOPNOTSUPP;

	/* No support for dir */
	if (!S_ISREG(inode->i_mode))
		return -EOPNOTSUPP;

	inode_lock(inode);
	if (mode & FALLOC_FL_KEEP_SIZE) {
		ondisksize = inode->i_blocks << 9;
		if ((offset + len) <= ondisksize)
			goto error;

		/* First compute the number of clusters to be allocated */
		mm_bytes = offset + len - ondisksize;
		nr_cluster = (mm_bytes + (sbi->cluster_size - 1)) >>
			sbi->cluster_bits;

		/* Start the allocation.We are not zeroing out the clusters */
		while (nr_cluster-- > 0) {
			err = fat_add_cluster(inode);
			if (err)
				goto error;
		}
	} else {
		if ((offset + len) <= i_size_read(inode))
			goto error;

		/* This is just an expanding truncate */
		err = fat_cont_expand(inode, (offset + len));
	}

error:
	inode_unlock(inode);
	return err;
}

/* Free all clusters after the skip'th cluster. */
static int fat_free(struct inode *inode, int skip)
{
	struct super_block *sb = inode->i_sb;
	int err, wait, free_start, i_start, i_logstart;

	if (MSDOS_I(inode)->i_start == 0)
		return 0;

	fat_cache_inval_inode(inode);

	wait = IS_DIRSYNC(inode);
	i_start = free_start = MSDOS_I(inode)->i_start;
	i_logstart = MSDOS_I(inode)->i_logstart;

	/* First, we write the new file size. */
	if (!skip) {
		MSDOS_I(inode)->i_start = 0;
		MSDOS_I(inode)->i_logstart = 0;
	}
	MSDOS_I(inode)->i_attrs |= ATTR_ARCH;
	fat_truncate_time_prfs(inode, NULL, S_CTIME|S_MTIME);
	if (wait) {
		err = fat_sync_inode_prfs(inode);
		if (err) {
			MSDOS_I(inode)->i_start = i_start;
			MSDOS_I(inode)->i_logstart = i_logstart;
			return err;
		}
	} else
		mark_inode_dirty(inode);

	/* Write a new EOF, and get the remaining cluster chain for freeing. */
	if (skip) {
		struct fat_entry fatent;
		int ret, fclus, dclus;

		ret = fat_get_cluster(inode, skip - 1, &fclus, &dclus);
		if (ret < 0)
			return ret;
		else if (ret == FAT_ENT_EOF)
			return 0;

		fatent_init(&fatent);
		ret = fat_ent_read(inode, &fatent, dclus);
		if (ret == FAT_ENT_EOF) {
			fatent_brelse(&fatent);
			return 0;
		} else if (ret == FAT_ENT_FREE) {
			fat_fs_error(sb,
				     "%s: invalid cluster chain (i_pos %lld)",
				     __func__, MSDOS_I(inode)->i_pos);
			ret = -EIO;
		} else if (ret > 0) {
			err = fat_ent_write(inode, &fatent, FAT_ENT_EOF, wait);
			if (err)
				ret = err;
		}
		fatent_brelse(&fatent);
		if (ret < 0)
			return ret;

		free_start = ret;
	}
	inode->i_blocks = skip << (MSDOS_SB(sb)->cluster_bits - 9);

	/* Freeing the remained cluster chain */
	return fat_free_clusters_prfs(inode, free_start);
}

void fat_truncate_blocks(struct inode *inode, loff_t offset)
{
	struct msdos_sb_info *sbi = MSDOS_SB(inode->i_sb);
	const unsigned int cluster_size = sbi->cluster_size;
	int nr_clusters;

	/*
	 * This protects against truncating a file bigger than it was then
	 * trying to write into the hole.
	 */
	if (MSDOS_I(inode)->mmu_private > offset)
		MSDOS_I(inode)->mmu_private = offset;

	nr_clusters = (offset + (cluster_size - 1)) >> sbi->cluster_bits;

	fat_free(inode, nr_clusters);
	fat_flush_inodes_prfs(inode->i_sb, inode, NULL);
}

int fat_getattr_prfs(struct user_namespace *mnt_userns, const struct path *path,
		struct kstat *stat, u32 request_mask, unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct msdos_sb_info *sbi = MSDOS_SB(inode->i_sb);

	//dbg printk(KERN_INFO "fat_getattr_prfs function...\n");

	generic_fillattr(mnt_userns, inode, stat);
	stat->blksize = sbi->cluster_size;

	if (sbi->options.nfs == FAT_NFS_NOSTALE_RO) {
		/* Use i_pos for ino. This is used as fileid of nfs. */
		stat->ino = fat_i_pos_read(sbi, inode);
	}

	if (sbi->options.isvfat && request_mask & STATX_BTIME) {
		stat->result_mask |= STATX_BTIME;
		stat->btime = MSDOS_I(inode)->i_crtime;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(fat_getattr_prfs);

static int fat_sanitize_mode(const struct msdos_sb_info *sbi,
			     struct inode *inode, umode_t *mode_ptr)
{
	umode_t mask, perm;

	/*
	 * Note, the basic check is already done by a caller of
	 * (attr->ia_mode & ~FAT_VALID_MODE)
	 */

	if (S_ISREG(inode->i_mode))
		mask = sbi->options.fs_fmask;
	else
		mask = sbi->options.fs_dmask;

	perm = *mode_ptr & ~(S_IFMT | mask);

	/*
	 * Of the r and x bits, all (subject to umask) must be present. Of the
	 * w bits, either all (subject to umask) or none must be present.
	 *
	 * If fat_mode_can_hold_ro(inode) is false, can't change w bits.
	 */
	if ((perm & (S_IRUGO | S_IXUGO)) != (inode->i_mode & (S_IRUGO|S_IXUGO)))
		return -EPERM;
	if (fat_mode_can_hold_ro(inode)) {
		if ((perm & S_IWUGO) && ((perm & S_IWUGO) != (S_IWUGO & ~mask)))
			return -EPERM;
	} else {
		if ((perm & S_IWUGO) != (S_IWUGO & ~mask))
			return -EPERM;
	}

	*mode_ptr &= S_IFMT | perm;

	return 0;
}

static int fat_allow_set_time(struct user_namespace *mnt_userns,
			      struct msdos_sb_info *sbi, struct inode *inode)
{
	umode_t allow_utime = sbi->options.allow_utime;

	if (!vfsuid_eq_kuid(i_uid_into_vfsuid(mnt_userns, inode),
			    current_fsuid())) {
		if (vfsgid_in_group_p(i_gid_into_vfsgid(mnt_userns, inode)))
			allow_utime >>= 3;
		if (allow_utime & MAY_WRITE)
			return 1;
	}

	/* use a default check */
	return 0;
}

#define TIMES_SET_FLAGS	(ATTR_MTIME_SET | ATTR_ATIME_SET | ATTR_TIMES_SET)
/* valid file mode bits */
#define FAT_VALID_MODE	(S_IFREG | S_IFDIR | S_IRWXUGO)

int fat_setattr_prfs(struct user_namespace *mnt_userns, struct dentry *dentry,
		struct iattr *attr)
{
	struct msdos_sb_info *sbi = MSDOS_SB(dentry->d_sb);
	struct inode *inode = d_inode(dentry);
	unsigned int ia_valid;
	int error;

	//dbg printk(KERN_INFO "fat_setattr_prfs function...\n");

	/* Check for setting the inode time. */
	ia_valid = attr->ia_valid;
	if (ia_valid & TIMES_SET_FLAGS) {
		if (fat_allow_set_time(mnt_userns, sbi, inode))
			attr->ia_valid &= ~TIMES_SET_FLAGS;
	}

	error = setattr_prepare(mnt_userns, dentry, attr);
	attr->ia_valid = ia_valid;
	if (error) {
		if (sbi->options.quiet)
			error = 0;
		goto out;
	}

	/*
	 * Expand the file. Since inode_setattr() updates ->i_size
	 * before calling the ->truncate(), but FAT needs to fill the
	 * hole before it. XXX: this is no longer true with new truncate
	 * sequence.
	 */
	if (attr->ia_valid & ATTR_SIZE) {
		inode_dio_wait(inode);

		if (attr->ia_size > inode->i_size) {
			error = fat_cont_expand(inode, attr->ia_size);
			if (error || attr->ia_valid == ATTR_SIZE)
				goto out;
			attr->ia_valid &= ~ATTR_SIZE;
		}
	}

	if (((attr->ia_valid & ATTR_UID) &&
	     (!uid_eq(from_vfsuid(mnt_userns, i_user_ns(inode), attr->ia_vfsuid),
		      sbi->options.fs_uid))) ||
	    ((attr->ia_valid & ATTR_GID) &&
	     (!gid_eq(from_vfsgid(mnt_userns, i_user_ns(inode), attr->ia_vfsgid),
		      sbi->options.fs_gid))) ||
	    ((attr->ia_valid & ATTR_MODE) &&
	     (attr->ia_mode & ~FAT_VALID_MODE)))
		error = -EPERM;

	if (error) {
		if (sbi->options.quiet)
			error = 0;
		goto out;
	}

	/*
	 * We don't return -EPERM here. Yes, strange, but this is too
	 * old behavior.
	 */
	if (attr->ia_valid & ATTR_MODE) {
		if (fat_sanitize_mode(sbi, inode, &attr->ia_mode) < 0)
			attr->ia_valid &= ~ATTR_MODE;
	}

	if (attr->ia_valid & ATTR_SIZE) {
		error = fat_block_truncate_page(inode, attr->ia_size);
		if (error)
			goto out;
		down_write(&MSDOS_I(inode)->truncate_lock);
		truncate_setsize(inode, attr->ia_size);
		fat_truncate_blocks(inode, attr->ia_size);
		up_write(&MSDOS_I(inode)->truncate_lock);
	}

	/*
	 * setattr_copy can't truncate these appropriately, so we'll
	 * copy them ourselves
	 */
	if (attr->ia_valid & ATTR_ATIME)
		fat_truncate_time_prfs(inode, &attr->ia_atime, S_ATIME);
	if (attr->ia_valid & ATTR_CTIME)
		fat_truncate_time_prfs(inode, &attr->ia_ctime, S_CTIME);
	if (attr->ia_valid & ATTR_MTIME)
		fat_truncate_time_prfs(inode, &attr->ia_mtime, S_MTIME);
	attr->ia_valid &= ~(ATTR_ATIME|ATTR_CTIME|ATTR_MTIME);

	setattr_copy(mnt_userns, inode, attr);
	mark_inode_dirty(inode);
out:
	return error;
}
EXPORT_SYMBOL_GPL(fat_setattr_prfs);

const struct inode_operations fat_file_inode_operations = {
	.setattr	= fat_setattr_prfs,
	.getattr	= fat_getattr_prfs,
	.update_time	= fat_update_time_prfs,
};
