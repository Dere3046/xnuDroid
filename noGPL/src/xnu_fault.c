// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#include <linux/module.h>

#include "xnu_boot.h"
#include "xnu_fault.h"
#include "xnu_main.h"

struct xnu_deferred_state {
	unsigned long far;
	unsigned long esr;
	unsigned long pc;
	unsigned long pstate;
};

static unsigned long (*g_xnu_kfault)(unsigned long far, unsigned long esr);
static unsigned long (*g_xnu_kfault_recover)(unsigned long pc,
					     unsigned long far,
					     unsigned long esr);
static struct xnu_deferred_state g_deferred_slot[LINUX64_CPU_SLOTS];

__nocfi unsigned long xnu_fault_deferred(unsigned long *frame)
{
	static unsigned long last_far_slot[LINUX64_CPU_SLOTS];
	static unsigned long repeats_slot[LINUX64_CPU_SLOTS];
	struct xnu_deferred_state *def = &g_deferred_slot[xnu_this_slot()];
	unsigned long far = def->far;
	unsigned long esr = def->esr;
	unsigned long pc = def->pc;
	unsigned s0 = xnu_this_slot();
	unsigned long repeats;
	long kr;

	repeats = (far == last_far_slot[s0]) ? repeats_slot[s0] + 1 : 0;
	repeats_slot[s0] = repeats;
	last_far_slot[s0] = far;

	frame[31] = def->pstate;
	kr = g_xnu_kfault(far, esr);
	if (xnu_this_slot() != s0) {
		unsigned long x18;

		__asm__ volatile("mov %0, x18" : "=r"(x18));
		frame[18] = x18;
		g_xnu.kfault_moved++;
	}
	if (kr == 0) {
		if (repeats && (repeats & (repeats - 1)) == 0 &&
		    repeats >= 1024) {
			xnu_logkv("xnu: repeated kernel fault", repeats);
			xnu_logkv("xnu:   far", far);
			xnu_logkv("xnu:   esr", esr);
			xnu_logkv("xnu:   pc ", pc);
			xnu_logkv("xnu:   lr ", frame[30]);
		}
		if ((repeats & 63) == 63) {
			xnu_world_upcall(LINUX64_CALL_PREEMPTED, 0, 0);
		}
		return pc;
	}

	if (g_xnu_kfault_recover) {
		unsigned long rpc = g_xnu_kfault_recover(pc, far, esr);

		if (rpc) {
			frame[0] = 14;
			return rpc;
		}
	}

	g_xnu.in_xnu_slot[xnu_this_slot()] = 0;
	xnu_logs("xnu: XNU took a fault vm_fault cannot resolve");
	xnu_logkv("xnu:   pc ", pc);
	xnu_logkv("xnu:   lr ", frame[30]);
	xnu_logkv("xnu:   far", far);
	xnu_logkv("xnu:   esr", esr);
	return (unsigned long)xnu_world_fault_abort;
}

static int heal_s1pie_perm(unsigned long far, unsigned long *esrp)
{
	unsigned long esr = *esrp;
	unsigned long fsc = esr & 0x3fUL;
	unsigned long long *tbl;
	unsigned long long desc;
	unsigned long idx;

	if (!g_xnu.window_l1 || !(esr & (1UL << 6)) ||
	    fsc < 0x0dUL || fsc > 0x0fUL ||
	    far < g_xnu.va_lo || far >= g_xnu.va_hi) {
		return 0;
	}
	idx = (far >> 30) & 511UL;
	desc = g_xnu.window_l1[idx];
	if ((desc & 3ULL) == 1ULL) {
		tbl = &g_xnu.window_l1[idx];
	} else if ((desc & 3ULL) == 3ULL) {
		tbl = xnu_tbl_va(desc);
		if (!tbl) {
			return 0;
		}
		idx = (far >> 21) & 511UL;
		desc = tbl[idx];
		if ((desc & 3ULL) == 1ULL) {
			tbl = &tbl[idx];
		} else if ((desc & 3ULL) == 3ULL) {
			tbl = xnu_tbl_va(desc);
			if (!tbl) {
				return 0;
			}
			idx = (far >> 12) & 511UL;
			desc = tbl[idx];
			if ((desc & 3ULL) != 3ULL) {
				*esrp = (esr & ~0x3fUL) | 0x07UL;
				return 0;
			}
			tbl = &tbl[idx];
		} else {
			*esrp = (esr & ~0x3fUL) | 0x06UL;
			return 0;
		}
	} else {
		*esrp = (esr & ~0x3fUL) | 0x05UL;
		return 0;
	}
	if ((desc & (3ULL << 6)) || (desc & (1ULL << 51))) {
		return 0;
	}
	*tbl = desc | (1ULL << 51);
	__asm__ volatile("dsb ishst\n\t"
			 "tlbi vmalle1is\n\t"
			 "dsb ish\n\t"
			 "isb" ::: "memory");
	return 1;
}

static __nocfi int xnu_fault(unsigned long far, unsigned long esr, void *regs)
{
	unsigned slot = xnu_this_slot();
	unsigned long fpc;
	unsigned long pc;
	unsigned long *fp;
	int i;

	if (!g_xnu.in_xnu_slot[slot]) {
		return 1;
	}
	fpc = fki_regs_pc(regs);
	if (fpc >= g_xnu.va_lo && fpc < g_xnu.va_hi &&
	    heal_s1pie_perm(far, &esr)) {
		return 0;
	}
	if (far && ((far >= g_xnu.va_lo && far < g_xnu.va_hi) ||
		    far < 0x0001000000000000ULL) &&
	    g_xnu_kfault && fpc >= g_xnu.va_lo && fpc < g_xnu.va_hi) {
		g_xnu.kfaults++;
		g_deferred_slot[slot].far = far;
		g_deferred_slot[slot].esr = esr;
		g_deferred_slot[slot].pc = fpc;
		g_deferred_slot[slot].pstate = fki_regs_pstate(regs);
		fki_regs_set_pc(regs, (unsigned long)xnu_fault_tramp);
		return 0;
	}

	pc = fki_regs_pc(regs);
	if (pc < g_xnu.va_lo || pc >= g_xnu.va_hi) {
		return 1;
	}
	if ((esr >> 26) == 0x3c && (esr & 0xffff) == 0xff00) {
		static unsigned long ubsan_traps;

		if (ubsan_traps++ < 8) {
			xnu_logkv("xnu: UBSan soft trap, pc", pc);
		}
		fki_regs_set_pc(regs, pc + 4);
		return 0;
	}
	if (g_xnu_kfault_recover) {
		unsigned long rpc = g_xnu_kfault_recover(pc, far, esr);

		if (rpc) {
			fki_regs_set_reg(regs, 0, 14);
			fki_regs_set_pc(regs, rpc);
			return 0;
		}
	}

	g_xnu.in_xnu_slot[slot] = 0;
	xnu_logs("xnu: XNU took an unhandled fault");
	xnu_logkv("xnu:   pc ", pc);
	xnu_logkv("xnu:   lr ", fki_regs_lr(regs));
	xnu_logkv("xnu:   far", far);
	xnu_logkv("xnu:   esr", esr);
	xnu_logkv("xnu:   CPU slot", slot);
	xnu_logkv("xnu:   x0 ", fki_regs_reg(regs, 0));
	xnu_logkv("xnu:   x1 ", fki_regs_reg(regs, 1));

	fp = (unsigned long *)fki_regs_reg(regs, 29);
	for (i = 0; i < 12 && fp && !((unsigned long)fp & 15) &&
	     (unsigned long)fp >= g_xnu.va_lo &&
	     (unsigned long)fp < g_xnu.va_hi; i++) {
		xnu_logkv("xnu:   frame", fp[1] | 0xffffff8000000000ul);
		if ((unsigned long *)fp[0] <= fp) {
			break;
		}
		fp = (unsigned long *)fp[0];
	}
	fki_regs_set_pc(regs, (unsigned long)xnu_world_fault_abort);
	return 0;
}

void xnu_fault_setup(void)
{
	int err;

	g_xnu_kfault = (void *)macho_sym(&g_xnu.img, "_linux64_kernel_fault");
	g_xnu_kfault_recover = (void *)macho_sym(&g_xnu.img,
						 "_linux64_kernel_fault_recover");
	err = fki_fault_register(g_xnu.va_lo, g_xnu.va_hi, xnu_fault);
	if (err == 0 && g_xnu_kfault) {
		xnu_logs("xnu: kernel faults forwarded to XNU");
	} else {
		xnu_logkv("xnu: fault register failed", (u64_t)(long)err);
	}
}

void xnu_fault_teardown(void)
{
	fki_fault_unregister();
}
