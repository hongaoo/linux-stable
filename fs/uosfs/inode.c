/*
 * Resizable simple ram filesystem for Linux.
 *
 * Copyright (C) 2000 Linus Torvalds.
 *               2000 Transmeta Corp.
 *
 * Usage limits added by David Gibson, Linuxcare Australia.
 * This file is released under the GPL.
 */

/*
 * NOTE! This filesystem is probably most useful
 * not as a real filesystem, but as an example of
 * how virtual filesystems can be written.
 *
 * It doesn't get much simpler than this. Consider
 * that this file implements the full semantics of
 * a POSIX-compliant read-write filesystem.
 *
 * Note in particular how the filesystem does not
 * need to implement any data structures of its own
 * to keep track of the virtual data: using the VFS
 * caches is sufficient.
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/uosfs.h>
#include <linux/sched.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/seq_file.h>
#include "internal.h"
#include "uosfs_proc.h"

struct uosfs_mount_opts {
	umode_t mode;
};

struct uosfs_fs_info {
	struct uosfs_mount_opts mount_opts;
};

#define uosfs_DEFAULT_MODE	0777

static const struct super_operations uosfs_ops;
static const struct inode_operations uosfs_dir_inode_operations;

struct inode *uosfs_get_inode(struct super_block *sb,
				const struct inode *dir, umode_t mode, dev_t dev)
{
	struct inode * inode = new_inode(sb);

	if (inode) {
		inode->i_ino = get_next_ino();
		inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
		inode->i_mapping->a_ops = &ram_aops;
		mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
		mapping_set_unevictable(inode->i_mapping);
		inode->i_atime = inode->i_mtime = inode_set_ctime_current(inode);
		switch (mode & S_IFMT) {
		default:
			init_special_inode(inode, mode, dev);
			break;
		case S_IFREG:
			inode->i_op = &uosfs_file_inode_operations;
			inode->i_fop = &uosfs_file_operations;
			break;
		case S_IFDIR:
			inode->i_op = &uosfs_dir_inode_operations;
			inode->i_fop = &uosfs_dir_operations;

			/* directory inodes start off with i_nlink == 2 (for "." entry) */
			inc_nlink(inode);
			break;
		case S_IFLNK:
			inode->i_op = &page_symlink_inode_operations;
			inode_nohighmem(inode);
			break;
		}
	}
	return inode;
}

/*
 * File creation. Allocate an inode, and we're done..
 */
/* SMP-safe */
static int
uosfs_mknod(struct mnt_idmap *idmap, struct inode *dir,
	    struct dentry *dentry, umode_t mode, dev_t dev)
{
	struct inode * inode = uosfs_get_inode(dir->i_sb, dir, mode, dev);
	int error = -ENOSPC;

	if (inode) {
		d_instantiate(dentry, inode);
		dget(dentry);	/* Extra count - pin the dentry in core */
		error = 0;
		dir->i_mtime = inode_set_ctime_current(dir);
	}
	return error;
}

static int uosfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode)
{
	int retval = uosfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFDIR, 0);
	if (!retval)
		inc_nlink(dir);
	return retval;
}

static int uosfs_create(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode, bool excl)
{
	return uosfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFREG, 0);
}

static int uosfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, const char *symname)
{
	struct inode *inode;
	int error = -ENOSPC;

	inode = uosfs_get_inode(dir->i_sb, dir, S_IFLNK|S_IRWXUGO, 0);
	if (inode) {
		int l = strlen(symname)+1;
		error = page_symlink(inode, symname, l);
		if (!error) {
			d_instantiate(dentry, inode);
			dget(dentry);
			dir->i_mtime = inode_set_ctime_current(dir);
		} else
			iput(inode);
	}
	return error;
}

static int uosfs_tmpfile(struct mnt_idmap *idmap,
			 struct inode *dir, struct file *file, umode_t mode)
{
	struct inode *inode;

	inode = uosfs_get_inode(dir->i_sb, dir, mode, 0);
	if (!inode)
		return -ENOSPC;
	d_tmpfile(file, inode);
	return finish_open_simple(file, 0);
}

static const struct inode_operations uosfs_dir_inode_operations = {
	.create		= uosfs_create,
	.lookup		= simple_lookup,
	.link		= simple_link,
	.unlink		= simple_unlink,
	.symlink	= uosfs_symlink,
	.mkdir		= uosfs_mkdir,
	.rmdir		= simple_rmdir,
	.mknod		= uosfs_mknod,
	.rename		= simple_rename,
	.tmpfile	= uosfs_tmpfile,
};

/*
 * Display the mount options in /proc/mounts.
 */
static int uosfs_show_options(struct seq_file *m, struct dentry *root)
{
	struct uosfs_fs_info *fsi = root->d_sb->s_fs_info;

	if (fsi->mount_opts.mode != uosfs_DEFAULT_MODE)
		seq_printf(m, ",mode=%o", fsi->mount_opts.mode);
	return 0;
}

static const struct super_operations uosfs_ops = {
	.statfs		= simple_statfs,
	.drop_inode	= generic_delete_inode,
	.show_options	= uosfs_show_options,
};

enum uosfs_param {
	Opt_mode,
};

const struct fs_parameter_spec uosfs_fs_parameters[] = {
	fsparam_u32oct("mode",	Opt_mode),
	{}
};

static int uosfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct fs_parse_result result;
	struct uosfs_fs_info *fsi = fc->s_fs_info;
	int opt;

	opt = fs_parse(fc, uosfs_fs_parameters, param, &result);
	if (opt == -ENOPARAM) {
		opt = vfs_parse_fs_param_source(fc, param);
		if (opt != -ENOPARAM)
			return opt;
		/*
		 * We might like to report bad mount options here;
		 * but traditionally ramfs has ignored all mount options,
		 * and as it is used as a !CONFIG_SHMEM simple substitute
		 * for tmpfs, better continue to ignore other mount options.
		 */
		return 0;
	}
	if (opt < 0)
		return opt;

	switch (opt) {
	case Opt_mode:
		fsi->mount_opts.mode = result.uint_32 & S_IALLUGO;
		break;
	}

	return 0;
}

static int uosfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct uosfs_fs_info *fsi = sb->s_fs_info;
	struct inode *inode;

	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_blocksize		= PAGE_SIZE;
	sb->s_blocksize_bits	= PAGE_SHIFT;
	sb->s_magic		= UOSFS_MAGIC;
	sb->s_op		= &uosfs_ops;
	sb->s_time_gran		= 1;

	inode = uosfs_get_inode(sb, NULL, S_IFDIR | fsi->mount_opts.mode, 0);
	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		return -ENOMEM;

	return 0;
}

static int uosfs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, uosfs_fill_super);
}

static void uosfs_free_fc(struct fs_context *fc)
{
	kfree(fc->s_fs_info);
}

static const struct fs_context_operations uosfs_context_ops = {
	.free		= uosfs_free_fc,
	.parse_param	= uosfs_parse_param,
	.get_tree	= uosfs_get_tree,
};

char *uos_path_filiter_path_name;
int uosfs_init_fs_context(struct fs_context *fc)
{
	struct uosfs_fs_info *fsi;
	struct proc_dir_entry *proc_entry;

	fsi = kzalloc(sizeof(*fsi), GFP_KERNEL);
	if (!fsi)
		return -ENOMEM;

	fsi->mount_opts.mode = uosfs_DEFAULT_MODE;
	fc->s_fs_info = fsi;
	fc->ops = &uosfs_context_ops;

    uos_path_filiter_path_name = kmalloc(1024,GFP_KERNEL);

	proc_entry = proc_create(UOSFS_PATH_FILITER_PROC, 0666, NULL, &uosfs_path_filiter_fops);
    if (!proc_entry) {
        printk(KERN_ERR "Failed to create %s\n",UOSFS_PATH_FILITER_PROC);
        kfree(fsi);
		return -ENOENT;;
    }

	proc_entry = proc_create(UOSFS_PATH_FILITER_ENABLE_PROC, 0666, NULL, &uosfs_path_filiter_enable_fops);
    if (!proc_entry) {
        printk(KERN_ERR "Failed to create %s\n",UOSFS_PATH_FILITER_ENABLE_PROC);
        kfree(fsi);
		return -ENOENT;;
    }
	return 0;
}

void uosfs_kill_sb(struct super_block *sb)
{
	kfree(sb->s_fs_info);
	kill_litter_super(sb);
}

static struct file_system_type uosfs_fs_type = {
	.name		= "uosfs",
	.init_fs_context = uosfs_init_fs_context,
	.parameters	= uosfs_fs_parameters,
	.kill_sb	= uosfs_kill_sb,
	.fs_flags	= FS_USERNS_MOUNT,
};

static int __init init_uosfs_fs(void)
{
	return register_filesystem(&uosfs_fs_type);
}
fs_initcall(init_uosfs_fs);
