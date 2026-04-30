// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sample module that registers a conflict kprobe intended to overlay the
 * primary probe and block optimization. Removing this module should allow the
 * optimizer to re-optimize the primary probe.
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>

static char conflict_symbol[KSYM_NAME_LEN] = "kernel_clone";
module_param_string(conflict_symbol, conflict_symbol, KSYM_NAME_LEN, 0444);
MODULE_PARM_DESC(conflict_symbol, "Symbol name on which to plant the conflict probe");

static struct kprobe kp_conflict = {
	.symbol_name = conflict_symbol,
};

static int __kprobes conflict_pre(struct kprobe *p, struct pt_regs *regs)
{
#ifdef CONFIG_X86
	pr_info("%s pre: ip=0x%lx, flags=0x%lx", p->symbol_name, regs->ip, regs->flags);
#elif defined(CONFIG_ARM64)
	pr_info("%s pre: pc=0x%lx, pstate=0x%lx", p->symbol_name,
		(long)regs->pc, (long)regs->pstate);
#else
	pr_info("%s pre (addr=%px)", p->symbol_name, p->addr);
#endif
	return 0;
}


static int __init kprobe_conflict_init(void)
{
	int ret;

	kp_conflict.pre_handler = conflict_pre;

	ret = register_kprobe(&kp_conflict);
	if (ret < 0) {
		pr_err("failed to register conflict probe: %d\n", ret);
		return ret;
	}

	pr_info("conflict probe planted at %p\n", kp_conflict.addr);
	return 0;
}

static void __exit kprobe_conflict_exit(void)
{
	unregister_kprobe(&kp_conflict);
	pr_info("conflict probe removed\n");
}

module_init(kprobe_conflict_init);
module_exit(kprobe_conflict_exit);
MODULE_DESCRIPTION("conflict kprobe demo for re-optimization");
MODULE_LICENSE("GPL");
