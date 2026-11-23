// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef XNU_FAULT_H
#define XNU_FAULT_H

void xnu_fault_setup(void);
void xnu_fault_teardown(void);
unsigned long xnu_fault_deferred(unsigned long *frame)
	__attribute__((no_sanitize("shadow-call-stack")));

#endif
