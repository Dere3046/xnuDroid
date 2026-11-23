// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef XNU_BOOT_H
#define XNU_BOOT_H

#include "macho.h"

unsigned long long *xnu_tbl_va(u64_t desc);
void xnu_walk_log(const char *what, u64_t va, const unsigned long long *l1);
int xnu_run_m1(void);
int xnu_run_m2a(void);
int xnu_run_m2b(void);

#endif
