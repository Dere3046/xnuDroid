// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef XNU_MON_H
#define XNU_MON_H

#include "macho.h"

void xnu_acct_add(unsigned long call, u64_t ticks);
void xnu_mon_start(void);
void xnu_mon_stop(void);

#endif
