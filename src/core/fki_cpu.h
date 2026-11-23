// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef FKI_CPU_H
#define FKI_CPU_H

int fki_cpu_init(void);
void fki_cpu_exit(void);
void fki_linux_runs_ext(void (*fn)(void *), void *arg);
void fki_wake_idle_cpu(int cpu);

#endif
