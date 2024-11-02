/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UOSFS_PROC_H__
#define __UOSFS_PROC_H__
#include <linux/proc_fs.h>

#define UOSFS_PATH_FILITER_PROC "uosfs_path_filiter"

extern const struct proc_ops uosfs_path_filiter_fops;

#endif