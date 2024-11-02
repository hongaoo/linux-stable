# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the linux ramfs routines.
#

obj-y += uosfs.o

file-mmu-y := file-nommu.o
file-mmu-$(CONFIG_MMU) := file-mmu.o
uosfs-objs += inode.o $(file-mmu-y)
