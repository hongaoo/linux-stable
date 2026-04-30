// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sample module that registers a single primary kprobe (similar to kprobe_example)
 * so that another module can overlay a conflicting probe and later hand it back
 * to the optimizer for re-optimization.
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>

static char primary_symbol[KSYM_NAME_LEN] = "kernel_clone";
module_param_string(primary_symbol, primary_symbol, KSYM_NAME_LEN, 0444);
MODULE_PARM_DESC(primary_symbol, "Symbol name on which to plant the primary probe");

static struct kprobe kp_primary = {
	.symbol_name = primary_symbol,
	.offset = 0x8,
};

static int __kprobes primary_pre(struct kprobe *p, struct pt_regs *regs)
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

static int __init kprobe_primary_init(void)
{
	int ret;

	kp_primary.pre_handler = primary_pre;

	ret = register_kprobe(&kp_primary);
	if (ret < 0) {
		pr_err("failed to register primary probe: %d\n", ret);
		return ret;
	}

	pr_info("primary probe planted at %p\n", kp_primary.addr);
	return 0;
}

static void __exit kprobe_primary_exit(void)
{
	unregister_kprobe(&kp_primary);
	pr_info("primary probe removed\n");
}

module_init(kprobe_primary_init);
module_exit(kprobe_primary_exit);
MODULE_DESCRIPTION("primary kprobe sample for conflict demo");
MODULE_LICENSE("GPL");
