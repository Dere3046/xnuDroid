// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#include <linux/module.h>
#include <linux/printk.h>

#include "fki.h"
#include "fki_console.h"
#include "fki_cpu.h"
#include "fki_hook.h"
#include "fki_hostcall.h"
#include "fki_io.h"
#include "fki_mem.h"
#include "sym_reg.h"

int fki_version(void)
{
	return FKI_VERSION;
}
EXPORT_SYMBOL(fki_version);

void fki_log(const char *s, unsigned long len)
{
	pr_info("[fki] %.*s\n", (int)len, s);
}
EXPORT_SYMBOL(fki_log);

void fki_log_alert(const char *s, unsigned long len)
{
	pr_alert("[fki] %.*s\n", (int)len, s);
}
EXPORT_SYMBOL(fki_log_alert);

static int __init fki_init(void)
{
	int err;

	err = fki_sym_init();
	if (err) {
		return err;
	}

	err = fki_hook_init();
	if (err) {
		goto err_sym;
	}

	err = fki_cpu_init();
	if (err) {
		goto err_hook;
	}

	fki_mem_init();

	err = fki_hostcall_init();
	if (err) {
		pr_warn("[fki] no /dev/xnu-hostcall (%d)\n", err);
	}

	err = fki_console_init();
	if (err) {
		pr_warn("[fki] no /dev/xnu-console (%d)\n", err);
	}

	fki_io_init();
	pr_info("[fki] initialized v%d\n", FKI_VERSION);
	return 0;

err_hook:
	fki_hook_exit();
err_sym:
	fki_sym_exit();
	return err;
}

static void __exit fki_exit(void)
{
	fki_io_exit();
	fki_console_exit();
	fki_hostcall_exit();
	fki_mem_exit();
	fki_cpu_exit();
	fki_hook_exit();
	fki_sym_exit();
	pr_info("[fki] unloaded\n");
}

module_init(fki_init);
module_exit(fki_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("dere3046, Heyang Zhou");
MODULE_DESCRIPTION("Foreign kernel interface");
