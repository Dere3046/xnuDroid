// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef FKI_HOOK_H
#define FKI_HOOK_H

#include <linux/kprobes.h>
#include <linux/types.h>

enum fki_hook_slot {
	FKI_HOOK_FAULT = 0,
	FKI_HOOK_FIXUP,
	FKI_HOOK_DIE,
	FKI_HOOK_MAX
};

int fki_hook_init(void);
void fki_hook_exit(void);
int fki_hook_kprobe_add(enum fki_hook_slot slot, const char *sym,
			kprobe_pre_handler_t pre, bool skip_pad);
void fki_hook_kprobe_del(enum fki_hook_slot slot);

#endif
