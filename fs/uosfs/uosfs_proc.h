/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UOSFS_PROC_H__
#define __UOSFS_PROC_H__
#include <linux/proc_fs.h>

#define UOSFS_PATH_FILITER_PROC "uosfs_path_filiter_pid_add_path"
#define UOSFS_PATH_FILITER_ENABLE_PROC "uosfs_path_filiter_enable"

extern const struct proc_ops uosfs_path_filiter_fops;
extern const struct proc_ops uosfs_path_filiter_enable_fops;

#endif