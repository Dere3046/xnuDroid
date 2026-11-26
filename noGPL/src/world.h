// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#ifndef XNU_WORLD_H
#define XNU_WORLD_H

#include "fki.h"

struct xnu_world {
	unsigned long long linux_regs[14];
	unsigned long long xnu_regs[13];
	unsigned long long linux_tpidr0;
	unsigned long long linux_tpidrro0;
	unsigned long long xnu_stack_top;
	struct fki_pac_keys linux_keys;
	struct fki_pac_keys xnu_keys;
	unsigned long long linux_ttbr0;
	unsigned long long linux_tcr;
	unsigned long long xnu_ttbr0;
	unsigned long long xnu_tcr;
	unsigned long long xnu_tpidr;
	unsigned long long xnu_tpidrro;
	unsigned long long linux_scs;
};

_Static_assert(__builtin_offsetof(struct xnu_world, linux_scs) == 464,
	       "xnu_world layout");
_Static_assert(__builtin_offsetof(struct xnu_world, xnu_tpidrro) == 456,
	       "xnu_world layout");
_Static_assert(__builtin_offsetof(struct xnu_world, linux_ttbr0) == 416,
	       "xnu_world layout");
_Static_assert(sizeof(struct fki_pac_keys) == 88, "fki_pac_keys layout");
_Static_assert(__builtin_offsetof(struct xnu_world, xnu_keys) == 328,
	       "xnu_world layout");

extern struct xnu_world g_world;
extern struct xnu_world *xnu_worlds[];

long xnu_world_call(unsigned long fn, unsigned long arg);
long xnu_world_upcall(unsigned long stage, unsigned long a1, unsigned long a2);
void xnu_world_fault_abort(void);
void xnu_fault_tramp(void);
long xnu_hook(unsigned long stage, unsigned long a1, unsigned long a2);

#endif
