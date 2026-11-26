// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/version.h>
#include <asm/debug-monitors.h>
#include <asm/sysreg.h>

#include "fki.h"
#include "fki_hook.h"
#include "hk.h"
#include "hk_inline.h"
#include "hk_kprobe.h"
#include "hk_patch.h"
#include "sym_reg.h"

#define FKI_PSR_MODE_MASK 0x0fUL

struct fki_hook_entry {
	struct hk_kprobe probe;
	bool active;
};

struct fki_hk_kprobe_node {
	struct hk_kprobe probe;
	int (*pre)(void *regs, void *arg);
	void *arg;
};

struct fki_hk_inline_node {
	struct hk_inline hook;
	int (*decide)(struct fki_inline_regs *regs, void *arg);
	void *arg;
};

_Static_assert(sizeof(struct fki_inline_regs) == sizeof(struct hk_inline_regs),
	       "inline regs layout");

static struct fki_hook_entry g_hooks[FKI_HOOK_MAX];
static bool g_hk_ready;
static bool g_brk_active;
static unsigned long g_fault_lo;
static unsigned long g_fault_hi;
static int (*g_fault_handler)(unsigned long far, unsigned long esr, void *regs);

typedef void (*fki_brk_reg_fn)(struct break_hook *hook);

static __nocfi noinline void call_reg_brk(struct break_hook *hook)
{
	fki_brk_reg_fn fn = (fki_brk_reg_fn)fki_sym_addr(FKI_SYM_REG_BRK_HOOK);

	if (fn) {
		fn(hook);
	}
}

static __nocfi noinline void call_unreg_brk(struct break_hook *hook)
{
	fki_brk_reg_fn fn = (fki_brk_reg_fn)fki_sym_addr(FKI_SYM_UNREG_BRK_HOOK);

	if (fn) {
		fn(hook);
	}
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 0, 0)
typedef unsigned long fki_brk_esr_t;
#else
typedef unsigned int fki_brk_esr_t;
#endif

static int fki_brk_pre(struct pt_regs *regs, fki_brk_esr_t esr);

static struct break_hook g_brk_apple = {
	.fn = fki_brk_pre,
	.imm = 0xf000,
	.mask = 0x0fff,
};

static struct break_hook g_brk_trap = {
	.fn = fki_brk_pre,
	.imm = 0x0001,
	.mask = 0x0000,
};

static bool pt_regs_layout_ok(void)
{
	return fki_layout_ok(FKI_LAY_PT_REGS_REGS) &&
	       fki_layout_ok(FKI_LAY_PT_REGS_PC) &&
	       fki_layout_ok(FKI_LAY_PT_REGS_PSTATE);
}

static u64 *pt_regs_arr(void *regs)
{
	return (u64 *)fki_layout_ptr(regs, FKI_LAY_PT_REGS_REGS);
}

static u64 *pt_regs_pc_ptr(void *regs)
{
	return (u64 *)fki_layout_ptr(regs, FKI_LAY_PT_REGS_PC);
}

static u64 *pt_regs_pstate_ptr(void *regs)
{
	return (u64 *)fki_layout_ptr(regs, FKI_LAY_PT_REGS_PSTATE);
}

static bool pt_regs_is_user(void *regs)
{
	u64 *pstate = pt_regs_pstate_ptr(regs);

	if (!pstate) {
		return true;
	}
	return (*pstate & FKI_PSR_MODE_MASK) == 0;
}
NOKPROBE_SYMBOL(pt_regs_is_user);

int fki_hook_init(void)
{
	struct hk_cfg cfg;
	int err;

	if (g_hk_ready) {
		return 0;
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.resolve = fki_sym_resolve;
	err = hk_init(&cfg);
	if (err) {
		pr_err("[fki] hk_init failed: %d\n", err);
		return err;
	}

	g_hk_ready = true;
	return 0;
}

void fki_hook_exit(void)
{
	int i;

	if (!g_hk_ready) {
		return;
	}
	for (i = 0; i < FKI_HOOK_MAX; i++) {
		fki_hook_kprobe_del(i);
	}
	hk_exit();
	g_hk_ready = false;
}

int fki_hook_kprobe_add(enum fki_hook_slot slot, const char *sym,
			kprobe_pre_handler_t pre, bool skip_pad)
{
	struct hk_kprobe_report rep;
	struct fki_hook_entry *entry;
	int err;

	if (!g_hk_ready || slot < 0 || slot >= FKI_HOOK_MAX || !sym || !pre) {
		return -EINVAL;
	}

	entry = &g_hooks[slot];
	if (entry->active) {
		return -EBUSY;
	}

	memset(&entry->probe, 0, sizeof(entry->probe));
	if (skip_pad) {
		entry->probe.adjust = hk_kprobe_adjust_pad;
	}

	if (!hk_kprobe_check(sym, &rep) && rep.state == HK_KPROBE_BLACKLISTED) {
		hk_kprobe_clear_blacklist();
	}

	err = hk_kprobe_install(&entry->probe, sym, pre);
	if (err) {
		return err;
	}

	entry->active = true;
	return 0;
}

void fki_hook_kprobe_del(enum fki_hook_slot slot)
{
	struct fki_hook_entry *entry;

	if (slot < 0 || slot >= FKI_HOOK_MAX) {
		return;
	}
	entry = &g_hooks[slot];
	if (!entry->active) {
		return;
	}
	hk_kprobe_remove(&entry->probe);
	entry->active = false;
}

static __nocfi int call_fault_handler(unsigned long far, unsigned long esr,
				      void *regs)
{
	int (*h)(unsigned long, unsigned long, void *);

	h = READ_ONCE(g_fault_handler);
	if (!h) {
		return -EINVAL;
	}
	return h(far, esr, regs);
}
NOKPROBE_SYMBOL(call_fault_handler);

static void kprobe_regs_return(void *kregs)
{
	u64 *arr = pt_regs_arr(kregs);
	u64 *pc = pt_regs_pc_ptr(kregs);

	if (!arr || !pc) {
		return;
	}
	arr[30] = hk_pac_strip(arr[30]);
	*pc = arr[30];
}
NOKPROBE_SYMBOL(kprobe_regs_return);

static int fki_fault_common(void *regs, unsigned long far, unsigned long esr)
{
	u64 *pc;

	if (!regs || pt_regs_is_user(regs)) {
		return -EINVAL;
	}
	pc = pt_regs_pc_ptr(regs);
	if (!pc) {
		return -EINVAL;
	}
	if ((far < g_fault_lo || far >= g_fault_hi) &&
	    (*pc < g_fault_lo || *pc >= g_fault_hi)) {
		return -ENOENT;
	}
	return call_fault_handler(far, esr, regs);
}
NOKPROBE_SYMBOL(fki_fault_common);

static int fki_fault_pre(struct kprobe *p, struct pt_regs *kregs)
{
	u64 *karr = pt_regs_arr(kregs);

	if (!karr || fki_fault_common((void *)karr[2], karr[0], karr[1])) {
		return 0;
	}
	kprobe_regs_return(kregs);
	return 1;
}
NOKPROBE_SYMBOL(fki_fault_pre);

static bool arm64_insn_is_store(u32 insn)
{
	if ((insn & 0xffffffe0U) == 0xd50b7420U) {
		return true;
	}
	if ((insn & 0x3f000000U) == 0x08000000U) {
		if ((insn & 0x00a00000U) == 0x00a00000U) {
			return true;
		}
		return !(insn & (1U << 22));
	}
	if ((insn & 0x3a000000U) == 0x28000000U) {
		return !(insn & (1U << 22));
	}
	if ((insn & 0x3a000000U) == 0x38000000U) {
		if (!(insn & (1U << 26)) && !(insn & (1U << 24)) &&
		    (insn & (1U << 21)) && (insn & 0x00000c00U) == 0U) {
			if ((insn & 0x3ffffc00U) == 0x38bfc000U) {
				return false;
			}
			return true;
		}
		if (insn & (1U << 26)) {
			return !(insn & (1U << 22));
		}
		return ((insn >> 22) & 3U) == 0U;
	}
	if ((insn & 0xbf000000U) == 0x0c000000U) {
		return !(insn & (1U << 22));
	}
	return false;
}
NOKPROBE_SYMBOL(arm64_insn_is_store);

static unsigned long synthesize_fixup_esr(void *regs, unsigned long far)
{
	u64 *pc_ptr = pt_regs_pc_ptr(regs);
	unsigned long pc;
	u32 insn;

	if (!pc_ptr) {
		return 0x96000007UL;
	}
	pc = *pc_ptr;
	if (far == pc) {
		return 0x86000007UL;
	}
	if (pc >= g_fault_lo && pc < g_fault_hi && !(pc & 3UL)) {
		insn = *(const u32 *)pc;
		if (arm64_insn_is_store(insn)) {
			return 0x9600004fUL;
		}
	}
	return 0x96000007UL;
}
NOKPROBE_SYMBOL(synthesize_fixup_esr);

static int fki_fixup_pre(struct kprobe *p, struct pt_regs *kregs)
{
	u64 *karr = pt_regs_arr(kregs);
	unsigned long far;
	unsigned long esr;

	if (!karr) {
		return 0;
	}
	far = read_sysreg(far_el1);
	esr = read_sysreg(esr_el1);
	/* Kprobe BRK #4 on fixup_exception overwrites ESR_EL1 */
	if ((esr >> 26) == 0x3c) {
		esr = synthesize_fixup_esr((void *)karr[0], far);
	}
	if (fki_fault_common((void *)karr[0], far, esr)) {
		return 0;
	}
	karr[0] = 1;
	kprobe_regs_return(kregs);
	return 1;
}
NOKPROBE_SYMBOL(fki_fixup_pre);

static int fki_die_pre(struct kprobe *p, struct pt_regs *kregs)
{
	u64 *karr = pt_regs_arr(kregs);
	void *regs;
	unsigned long esr;
	u64 *pc;

	if (!karr) {
		return 0;
	}
	regs = (void *)karr[1];
	esr = karr[2];
	if (!regs || pt_regs_is_user(regs)) {
		return 0;
	}
	pc = pt_regs_pc_ptr(regs);
	if (!pc || *pc < g_fault_lo || *pc >= g_fault_hi) {
		return 0;
	}
	if (call_fault_handler(0, esr, regs)) {
		return 0;
	}
	kprobe_regs_return(kregs);
	return 1;
}
NOKPROBE_SYMBOL(fki_die_pre);

static int fki_brk_pre(struct pt_regs *regs, fki_brk_esr_t esr)
{
	u64 *pc;

	if (!regs || pt_regs_is_user(regs)) {
		return DBG_HOOK_ERROR;
	}
	pc = pt_regs_pc_ptr(regs);
	if (!pc || *pc < g_fault_lo || *pc >= g_fault_hi) {
		return DBG_HOOK_ERROR;
	}
	if (call_fault_handler(0, esr, regs)) {
		return DBG_HOOK_ERROR;
	}
	return DBG_HOOK_HANDLED;
}
NOKPROBE_SYMBOL(fki_brk_pre);

unsigned long fki_regs_pc(void *regs)
{
	u64 *pc = pt_regs_pc_ptr(regs);

	return pc ? *pc : 0;
}
EXPORT_SYMBOL(fki_regs_pc);

unsigned long fki_regs_lr(void *regs)
{
	u64 *arr = pt_regs_arr(regs);

	return arr ? arr[30] : 0;
}
EXPORT_SYMBOL(fki_regs_lr);

unsigned long fki_regs_pstate(void *regs)
{
	u64 *pstate = pt_regs_pstate_ptr(regs);

	return pstate ? *pstate : 0;
}
EXPORT_SYMBOL(fki_regs_pstate);

void fki_regs_set_pc(void *regs, unsigned long pc)
{
	u64 *pc_ptr = pt_regs_pc_ptr(regs);

	if (pc_ptr) {
		*pc_ptr = pc;
	}
}
EXPORT_SYMBOL(fki_regs_set_pc);

void fki_regs_set_reg(void *regs, unsigned int idx, unsigned long val)
{
	u64 *arr = pt_regs_arr(regs);

	if (arr && idx < 31) {
		arr[idx] = val;
	}
}
EXPORT_SYMBOL(fki_regs_set_reg);

unsigned long fki_regs_reg(void *regs, unsigned int idx)
{
	u64 *arr = pt_regs_arr(regs);

	if (!arr || idx >= 31) {
		return 0;
	}
	return arr[idx];
}
EXPORT_SYMBOL(fki_regs_reg);

int fki_fault_register(unsigned long lo, unsigned long hi,
		       int (*handler)(unsigned long far, unsigned long esr,
				      void *regs))
{
	int err;

	if (!pt_regs_layout_ok()) {
		return -ENOTSUPP;
	}
	if (g_hooks[FKI_HOOK_FAULT].active) {
		return -EBUSY;
	}
	g_fault_lo = lo;
	g_fault_hi = hi;
	WRITE_ONCE(g_fault_handler, handler);

	err = fki_hook_kprobe_add(FKI_HOOK_FAULT, "__do_kernel_fault",
				  fki_fault_pre, true);
	if (err) {
		err = fki_hook_kprobe_add(FKI_HOOK_FAULT, "do_bad_area",
					  fki_fault_pre, true);
		if (err) {
			pr_warn("[fki] fault kprobe failed: %d\n", err);
			WRITE_ONCE(g_fault_handler, NULL);
			return err;
		}
		if (fki_hook_kprobe_add(FKI_HOOK_FIXUP, "fixup_exception",
					fki_fixup_pre, true)) {
			pr_warn("[fki] no fixup_exception probe installed\n");
		}
	}

	if (fki_hook_kprobe_add(FKI_HOOK_DIE, "die", fki_die_pre, true)) {
		pr_warn("[fki] no die probe installed\n");
	}
	if (fki_sym_addr(FKI_SYM_REG_BRK_HOOK) &&
	    fki_sym_addr(FKI_SYM_UNREG_BRK_HOOK)) {
		call_reg_brk(&g_brk_apple);
		call_reg_brk(&g_brk_trap);
		g_brk_active = true;
	}
	return 0;
}
EXPORT_SYMBOL(fki_fault_register);

void fki_fault_unregister(void)
{
	if (g_brk_active) {
		call_unreg_brk(&g_brk_trap);
		call_unreg_brk(&g_brk_apple);
		g_brk_active = false;
	}
	fki_hook_kprobe_del(FKI_HOOK_DIE);
	fki_hook_kprobe_del(FKI_HOOK_FIXUP);
	fki_hook_kprobe_del(FKI_HOOK_FAULT);
	WRITE_ONCE(g_fault_handler, NULL);
}
EXPORT_SYMBOL(fki_fault_unregister);

static __nocfi int hk_kprobe_dispatch(struct kprobe *p, struct pt_regs *regs)
{
	struct fki_hk_kprobe_node *node;
	int ret;

	node = container_of(p, struct fki_hk_kprobe_node, probe.kp);
	if (!node->pre) {
		return 0;
	}
	ret = node->pre(regs, node->arg);
	if (ret) {
		kprobe_regs_return(regs);
		return 1;
	}
	return 0;
}
NOKPROBE_SYMBOL(hk_kprobe_dispatch);

void *fki_hk_kprobe_add(const char *sym, int (*pre)(void *regs, void *arg),
			void *arg, int skip_pad)
{
	struct hk_kprobe_report rep;
	struct fki_hk_kprobe_node *node;
	int err;

	if (!g_hk_ready || !sym || !pre || !pt_regs_layout_ok()) {
		return NULL;
	}
	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node) {
		return NULL;
	}
	node->pre = pre;
	node->arg = arg;
	if (skip_pad) {
		node->probe.adjust = hk_kprobe_adjust_pad;
	}
	if (!hk_kprobe_check(sym, &rep) && rep.state == HK_KPROBE_BLACKLISTED) {
		hk_kprobe_clear_blacklist();
	}
	err = hk_kprobe_install(&node->probe, sym, hk_kprobe_dispatch);
	if (err) {
		kfree(node);
		return NULL;
	}
	return node;
}
EXPORT_SYMBOL(fki_hk_kprobe_add);

void *fki_hk_kprobe_add_at(unsigned long addr,
			   int (*pre)(void *regs, void *arg),
			   void *arg, int skip_pad)
{
	struct hk_kprobe_report rep;
	struct fki_hk_kprobe_node *node;
	int err;

	if (!g_hk_ready || !addr || !pre || !pt_regs_layout_ok()) {
		return NULL;
	}
	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node) {
		return NULL;
	}
	node->pre = pre;
	node->arg = arg;
	if (skip_pad) {
		node->probe.adjust = hk_kprobe_adjust_pad;
	}
	if (!hk_kprobe_check_at(addr, &rep) &&
	    rep.state == HK_KPROBE_BLACKLISTED) {
		hk_kprobe_clear_blacklist();
	}
	err = hk_kprobe_install_at(&node->probe, addr, hk_kprobe_dispatch);
	if (err) {
		kfree(node);
		return NULL;
	}
	return node;
}
EXPORT_SYMBOL(fki_hk_kprobe_add_at);

void fki_hk_kprobe_del(void *handle)
{
	struct fki_hk_kprobe_node *node = handle;

	if (!node) {
		return;
	}
	hk_kprobe_remove(&node->probe);
	kfree(node);
}
EXPORT_SYMBOL(fki_hk_kprobe_del);

int fki_hk_inline_hook(const char *sym, const char *wrapper_sym,
		       void **handle, unsigned long *orig_out)
{
	struct fki_hk_inline_node *node;
	int err;

	if (!g_hk_ready || !sym || !wrapper_sym || !handle) {
		return -EINVAL;
	}
	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node) {
		return -ENOMEM;
	}
	err = hk_inline_hook(&node->hook, sym, wrapper_sym);
	if (err) {
		kfree(node);
		return err;
	}
	*handle = node;
	if (orig_out) {
		*orig_out = node->hook.orig;
	}
	return 0;
}
EXPORT_SYMBOL(fki_hk_inline_hook);

static __nocfi int hk_inline_decide_bridge(struct hk_inline_regs *regs,
					   void *arg)
{
	struct fki_hk_inline_node *node = arg;

	if (!node || !node->decide) {
		return 0;
	}
	return node->decide((struct fki_inline_regs *)regs, node->arg);
}

int fki_hk_inline_hook_decide(const char *sym,
			      int (*decide)(struct fki_inline_regs *regs,
					    void *arg),
			      void *arg, void **handle,
			      unsigned long *orig_out)
{
	struct fki_hk_inline_node *node;
	struct hk_inline_ops ops;
	int err;

	if (!g_hk_ready || !sym || !decide || !handle) {
		return -EINVAL;
	}
	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node) {
		return -ENOMEM;
	}
	node->decide = decide;
	node->arg = arg;

	memset(&ops, 0, sizeof(ops));
	ops.sym = sym;
	ops.decide = hk_inline_decide_bridge;
	ops.arg = node;

	err = hk_inline_hook_ops(&node->hook, &ops);
	if (err) {
		kfree(node);
		return err;
	}
	*handle = node;
	if (orig_out) {
		*orig_out = node->hook.orig;
	}
	return 0;
}
EXPORT_SYMBOL(fki_hk_inline_hook_decide);

void fki_hk_inline_unhook(void *handle)
{
	struct fki_hk_inline_node *node = handle;

	if (!node) {
		return;
	}
	hk_inline_unhook(&node->hook);
	kfree(node);
}
EXPORT_SYMBOL(fki_hk_inline_unhook);

int fki_hk_write_text(void *dst, const void *src, unsigned long len)
{
	if (!g_hk_ready || !dst || !src || !len) {
		return -EINVAL;
	}
	return hk_write_text(dst, src, len);
}
EXPORT_SYMBOL(fki_hk_write_text);

unsigned long fki_hk_resolve(const char *name)
{
	if (!g_hk_ready || !name) {
		return 0;
	}
	return hk_resolve(name);
}
EXPORT_SYMBOL(fki_hk_resolve);
