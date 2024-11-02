/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_UOSFS_H
#define _LINUX_UOSFS_H

#include <linux/fs_parser.h> // bleh...

struct inode *uosfs_get_inode(struct super_block *sb, const struct inode *dir,
	 umode_t mode, dev_t dev);
extern int uosfs_init_fs_context(struct fs_context *fc);
extern void uosfs_kill_sb(struct super_block *sb);

#ifdef CONFIG_MMU
static inline int
uosfs_nommu_expand_for_mapping(struct inode *inode, size_t newsize)
{
	return 0;
}
#else
extern int uosfs_nommu_expand_for_mapping(struct inode *inode, size_t newsize);
#endif

extern const struct fs_parameter_spec uosfs_fs_parameters[];
extern const struct file_operations uosfs_file_operations;
extern const struct vm_operations_struct generic_file_vm_ops;

#endif
