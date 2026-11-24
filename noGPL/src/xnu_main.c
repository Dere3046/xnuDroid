// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#include <linux/module.h>

#include "xnu_boot.h"
#include "xnu_fault.h"
#include "xnu_main.h"
#include "xnu_mon.h"
#include "xnu_upcall.h"

struct xnu_world xnu_W;
struct xnu_world *xnu_worlds[LINUX64_CPU_SLOTS];
struct xnu_world g_worlds[XNU_MAX_CPUS];

struct xnu_ctx g_xnu = {
	.image = "xnu/kernel.macho",
	.mode = 2,
	.ram_mb = 256,
	.quiet_console = 1,
	.cpus = 1,
	.fb_width = 1280,
	.fb_height = 800,
	.fb_dump_ms = 1000,
	.model = "VirtualMac2,1",
	.vm16k = 1,
	.monitor_cpu = -2,
	.auto_slide = 1,
	.va_lo = XNU_VA_LO,
	.va_hi = XNU_VA_HI,
	.l1_first = XNU_L1_FIRST,
	.l1_last = XNU_L1_LAST,
};

module_param_named(image, g_xnu.image, charp, 0444);
module_param_named(mode, g_xnu.mode, int, 0444);
module_param_named(ram_mb, g_xnu.ram_mb, int, 0444);
module_param_named(kmax, g_xnu.kmax, int, 0444);
module_param_named(idle_max, g_xnu.idle_max, int, 0444);
module_param_named(quiet_console, g_xnu.quiet_console, int, 0444);
module_param_named(cpus, g_xnu.cpus, int, 0444);
module_param_named(stop_run, g_xnu.stop_run, int, 0644);
module_param_named(assert_fatal, g_xnu.assert_fatal, int, 0444);
module_param_named(bootargs, g_xnu.bootargs, charp, 0444);
module_param_named(ramdisk, g_xnu.ramdisk, charp, 0444);
module_param_named(trustcache, g_xnu.trustcache, charp, 0444);
module_param_named(fb_width, g_xnu.fb_width, int, 0444);
module_param_named(fb_height, g_xnu.fb_height, int, 0444);
module_param_named(fb_dump, g_xnu.fb_dump, charp, 0444);
module_param_named(fb_dump_ms, g_xnu.fb_dump_ms, int, 0444);
module_param_named(hostdisk, g_xnu.hostdisk, charp, 0444);
module_param_named(hostdisk_rw, g_xnu.hostdisk_rw, int, 0444);
module_param_named(console_in, g_xnu.console_in, charp, 0444);
module_param_named(input_in, g_xnu.input_in, charp, 0444);
module_param_named(keystore, g_xnu.keystore, charp, 0444);
module_param_named(disk, g_xnu.disk, charp, 0444);
module_param_named(boot_uuid, g_xnu.boot_uuid, charp, 0444);
module_param_named(preboot_uuid, g_xnu.preboot_uuid, charp, 0444);
module_param_named(root_uuid, g_xnu.root_uuid, charp, 0444);
module_param_named(model, g_xnu.model, charp, 0444);
module_param_named(vm16k, g_xnu.vm16k, int, 0444);
module_param_named(park_secondaries, g_xnu.park_secondaries, int, 0444);
module_param_named(perf, g_xnu.perf, int, 0644);
module_param_named(monitor_cpu, g_xnu.monitor_cpu, int, 0444);
module_param_named(dump_threads, g_xnu.dump_threads, int, 0644);
module_param_named(auto_slide, g_xnu.auto_slide, int, 0444);

void xnu_logkv(const char *key, u64_t val)
{
	char buf[96];
	char *ptr = buf;
	int i;

	while (*key) {
		*ptr++ = *key++;
	}
	*ptr++ = ' ';
	*ptr++ = '0';
	*ptr++ = 'x';
	for (i = 60; i >= 0; i -= 4) {
		*ptr++ = "0123456789abcdef"[(val >> i) & 15];
	}
	*ptr = 0;
	xnu_logs(buf);
}

static int cpu_fn(void *arg)
{
	int cpu = (int)(unsigned long)arg;
	long ret;

	fki_thread_wait(&g_xnu.cpu_start_arg[cpu]);
	if (!g_xnu.cpu_start_arg[cpu]) {
		return 0;
	}
	xnu_logkv("xnu: XNU CPU starts on Linux CPU", (u64_t)cpu);
	if (g_xnu.park_secondaries) {
		xnu_logkv("xnu: secondary CPU parked", (u64_t)cpu);
		g_xnu.cpu_running[cpu] = 0;
		while (!fki_thread_should_stop()) {
			fki_msleep(10);
		}
		return 0;
	}

	g_xnu.in_xnu_slot[xnu_this_slot()] = 1;
	ret = xnu_world_call(g_xnu.cpu_entry, g_xnu.cpu_start_arg[cpu]);
	g_xnu.in_xnu_slot[xnu_this_slot()] = 0;
	if (!ret) {
		xnu_logkv("xnu: linux64_cpu_entry returned on CPU", (u64_t)cpu);
	}
	g_xnu.stop_run = 1;
	g_xnu.cpu_running[cpu] = 0;
	while (!fki_thread_should_stop()) {
		fki_msleep(10);
	}
	return 0;
}

int xnu_enter_arm_init(struct boot_args_v2 *ba)
{
	u64_t a_init;
	u64_t a_hook;
	u64_t a_slide;
	u64_t a_pmap;
	unsigned long long tcr;
	unsigned long long ttbr0;
	int cpu;
	int slot_idx;
	long ret;

	if (g_xnu.img.filetype == MH_FILESET) {
		xnu_logs("xnu: kernel collection mode");
	} else {
		struct fki_pac_keys saved;
		unsigned long cookie = fki_enter(&xnu_W.xnu_keys, &saved);
		int cnt = macho_rebase(&g_xnu.img);
		u64_t mh_unslid = 0;
		int segs;

		fki_leave(cookie, &saved);
		xnu_logkv("xnu: fixups applied", (u64_t)cnt);
		if (cnt < 0) {
			return cnt;
		}
		segs = macho_slide_header(&g_xnu.img, &mh_unslid);
		xnu_logkv("xnu: load commands slid", (u64_t)segs);
	}

	a_init = macho_sym(&g_xnu.img, "_arm_init");
	a_hook = macho_sym(&g_xnu.img, "_linux64_hook");
	a_slide = macho_sym(&g_xnu.img, "_linux64_slide");
	xnu_logkv("xnu: arm_init at", a_init);
	if (!a_init || !a_hook || !a_slide) {
		xnu_logs("xnu: arm_init or linux64_hook missing");
		return -2;
	}
	*(u64_t *)a_hook = (u64_t)xnu_world_upcall;
	*(u64_t *)a_slide = g_xnu.img.slide;
	if (g_xnu.mode == 4) {
		struct linux64_phys_map *pm;

		a_pmap = macho_sym(&g_xnu.img, "_linux64_phys_map");
		if (!a_pmap) {
			xnu_logs("xnu: linux64_phys_map missing (rebuild XNU for mode 4)");
			return -2;
		}
		pm = (struct linux64_phys_map *)a_pmap;
		pm->gpa_base = g_xnu.m4_gpa_base;
		pm->gpa_end = g_xnu.m4_gpa_base +
			      ((u64_t)g_xnu.m4_n_chunks << LINUX64_CHUNK_SHIFT);
		pm->g2h = (const __UINT32_TYPE__ *)g_xnu.m4_g2h;
		pm->h2g = g_xnu.m4_h2g;
		pm->n_chunks = g_xnu.m4_n_chunks;
		pm->hash_mask = g_xnu.m4_hash_mask;
	}

	g_xnu.diag_va[0] = a_init;
	g_xnu.diag_va[1] = g_xnu.img.vmlow;
	g_xnu.diag_va[2] = (u64_t)ba;
	g_xnu.diag_va[3] = (u64_t)ba->device_tree_p;
	g_xnu.diag_va[4] = macho_sym(&g_xnu.img, "_port_crumb");
	g_xnu.diag_va[5] = macho_sym(&g_xnu.img, "_strlen");
	g_xnu.xnu_dbg = (const unsigned long *)macho_sym(&g_xnu.img,
							 "_linux64_dbg");
	g_xnu.xnu_el0_stat = (const unsigned long *)macho_sym(&g_xnu.img,
							      "_linux64_el0_stat");

	xnu_mon_start();
	xnu_fault_setup();

	__asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
	__asm__ volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
	/* User pmaps walk 47 bits from L0 with 4 KiB pages (T0SZ_HW=17) */
	xnu_W.xnu_tcr = (tcr & ~(1ULL << 22) & ~0x3fULL & ~(3ULL << 14)) | 17ULL;
	xnu_W.xnu_ttbr0 = ttbr0;

	g_xnu.cpu_entry = macho_sym(&g_xnu.img, "_linux64_cpu_entry");
	for (cpu = 1; cpu < g_xnu.cpus; cpu++) {
		g_worlds[cpu].xnu_keys = xnu_W.xnu_keys;
		g_worlds[cpu].xnu_tcr = xnu_W.xnu_tcr;
		g_worlds[cpu].xnu_ttbr0 = xnu_W.xnu_ttbr0;
	}
	if (g_xnu.cpus > 1 && !g_xnu.cpu_entry) {
		xnu_logs("xnu: missing linux64_cpu_entry, forcing 1 CPU");
		g_xnu.cpus = 1;
	}
	for (cpu = 1; cpu < g_xnu.cpus; cpu++) {
		g_xnu.cpu_start_arg[cpu] = 0;
		g_xnu.cpu_thread[cpu] = fki_thread_start(cpu_fn,
							 (void *)(unsigned long)cpu,
							 cpu, "xnu-cpu");
		if (!g_xnu.cpu_thread[cpu]) {
			xnu_logkv("xnu: failed to create thread for CPU",
				  (u64_t)cpu);
			g_xnu.cpus = cpu;
			break;
		}
	}

	xnu_logs("xnu: entering arm_init");
	g_xnu.in_xnu_slot[xnu_this_slot()] = 1;
	ret = xnu_world_call(a_init, (unsigned long)ba);
	g_xnu.in_xnu_slot[xnu_this_slot()] = 0;
	g_xnu.stop_run = 1;

	for (cpu = 1; cpu < g_xnu.cpus; cpu++) {
		for (slot_idx = 0; g_xnu.cpu_running[cpu] && slot_idx < 500;
		     slot_idx++) {
			fki_kick_cpu(cpu);
			fki_msleep(10);
		}
		if (g_xnu.cpu_running[cpu]) {
			xnu_logkv("xnu: CPU did not leave XNU in 5 s",
				  (u64_t)cpu);
		}
		fki_thread_stop(g_xnu.cpu_thread[cpu]);
		g_xnu.cpu_thread[cpu] = 0;
	}

	xnu_fault_teardown();
	xnu_logkv("xnu: kernel faults forwarded", g_xnu.kfaults);
	xnu_mon_stop();
	xnu_logkv(ret ? "xnu: run ended by hook" : "xnu: arm_init returned",
		  (u64_t)ret);
	return 0;
}

static void xnu_cleanup(void)
{
	if (g_xnu.window_l1) {
		int i;

		for (i = 0; i < 512; i++) {
			if (g_xnu.window_owned[i]) {
				fki_window_set(&g_xnu.window_l1[i], 0);
				g_xnu.window_owned[i] = 0;
			}
		}
		g_xnu.window_l1 = 0;
	}
	if (g_xnu.mode == 4) {
		if (g_xnu.m4_l3pages) {
			unsigned int ti;

			for (ti = 0; ti < g_xnu.m4_n_l3; ti++) {
				if (g_xnu.m4_l3pages[ti].va) {
					fki_contig_free(&g_xnu.m4_l3pages[ti]);
					g_xnu.m4_l3pages[ti].va = 0;
				}
			}
			fki_vfree(g_xnu.m4_l3pages);
			g_xnu.m4_l3pages = 0;
			g_xnu.m4_n_l3 = 0;
		}
		if (g_xnu.m4_chunks) {
			fki_chunk_free_16k(g_xnu.m4_n_chunks, g_xnu.m4_chunks);
			fki_vfree(g_xnu.m4_chunks);
			g_xnu.m4_chunks = 0;
			g_xnu.m4_n_chunks = 0;
		}
		if (g_xnu.l2page.va) {
			fki_contig_free(&g_xnu.l2page);
			g_xnu.l2page.va = 0;
		}
	} else if (g_xnu.mode == 3) {
		if (g_xnu.img.region.va) {
			fki_contig_free(&g_xnu.img.region);
			g_xnu.img.region.va = 0;
		}
		if (g_xnu.l2page.va) {
			fki_contig_free(&g_xnu.l2page);
			g_xnu.l2page.va = 0;
		}
	} else if (g_xnu.img.region.va) {
		fki_region_free(&g_xnu.img.region);
		g_xnu.img.region.va = 0;
	}
	if (g_xnu.blob) {
		fki_blob_put(g_xnu.blob);
		g_xnu.blob = 0;
	}
	if (g_xnu.rd_blob) {
		fki_blob_put(g_xnu.rd_blob);
		g_xnu.rd_blob = 0;
	}
	if (g_xnu.tc_blob) {
		fki_blob_put(g_xnu.tc_blob);
		g_xnu.tc_blob = 0;
	}
	if (g_xnu.hd_file) {
		fki_file_close(g_xnu.hd_file);
		g_xnu.hd_file = 0;
	}
	if (g_xnu.ks_file) {
		fki_file_close(g_xnu.ks_file);
		g_xnu.ks_file = 0;
	}
	if (g_xnu.dk_file) {
		fki_file_close(g_xnu.dk_file);
		g_xnu.dk_file = 0;
	}
	if (g_xnu.ci_file) {
		fki_file_close(g_xnu.ci_file);
		g_xnu.ci_file = 0;
	}
	if (g_xnu.in_file) {
		fki_file_close(g_xnu.in_file);
		g_xnu.in_file = 0;
	}
}

static int __init xnu_init(void)
{
	const void *data;
	unsigned long size;
	int err;

	if (fki_version() != FKI_VERSION) {
		return -22;
	}
	if (g_xnu.ramdisk) {
		err = fki_blob_get(g_xnu.ramdisk, &g_xnu.rd_data,
				   &g_xnu.rd_size, &g_xnu.rd_blob);
		if (err) {
			xnu_logkv("xnu: cannot read RAM disk", (u64_t)err);
			xnu_cleanup();
			return err;
		}
	}
	if (g_xnu.trustcache) {
		err = fki_blob_get(g_xnu.trustcache, &g_xnu.tc_data,
				   &g_xnu.tc_size, &g_xnu.tc_blob);
		if (err) {
			xnu_logkv("xnu: cannot read trust cache", (u64_t)err);
			xnu_cleanup();
			return err;
		}
	}
	if (g_xnu.console_in) {
		fki_u64 sz = 0;

		g_xnu.ci_file = fki_file_open(g_xnu.console_in, 0, &sz);
		g_xnu.ci_off = sz;
		if (!g_xnu.ci_file) {
			xnu_logs("xnu: cannot open console_in");
		}
	}
	if (g_xnu.keystore) {
		fki_u64 sz = 0;

		g_xnu.ks_file = fki_file_open(g_xnu.keystore, 1, &sz);
		if (!g_xnu.ks_file) {
			xnu_logs("xnu: cannot open keystore");
		}
	}
	if (g_xnu.input_in) {
		fki_u64 sz = 0;

		g_xnu.in_file = fki_file_open(g_xnu.input_in, 0, &sz);
		g_xnu.in_off = sz & ~7ULL;
		if (!g_xnu.in_file) {
			xnu_logs("xnu: cannot open input_in");
		}
	}
	if (g_xnu.disk) {
		g_xnu.dk_file = fki_file_open(g_xnu.disk, 1, &g_xnu.dk_size);
		if (!g_xnu.dk_file) {
			xnu_logs("xnu: cannot open block disk");
			xnu_cleanup();
			return -2;
		}
		xnu_logkv("xnu: block disk bytes", g_xnu.dk_size);
	}
	if (g_xnu.hostdisk) {
		g_xnu.hd_file = fki_file_open(g_xnu.hostdisk, g_xnu.hostdisk_rw,
					      &g_xnu.hd_size);
		if (!g_xnu.hd_file) {
			xnu_logs("xnu: cannot open host disk");
			xnu_cleanup();
			return -2;
		}
		xnu_logkv("xnu: host disk bytes", g_xnu.hd_size);
	}

	err = fki_blob_get(g_xnu.image, &data, &size, &g_xnu.blob);
	if (err) {
		xnu_logkv("xnu: cannot read image", (u64_t)err);
		xnu_cleanup();
		return err;
	}
	err = macho_load(&g_xnu.img, data, size,
			 (g_xnu.mode == 3 || g_xnu.mode == 4) ? ~0UL :
			 g_xnu.mode == 2 ? (unsigned long)g_xnu.ram_mb << 20 : 0);
	if (err) {
		xnu_logkv("xnu: load failed", (u64_t)err);
		xnu_cleanup();
		return err;
	}
	xnu_logkv("xnu: image vmlow", g_xnu.img.vmlow);
	xnu_logkv("xnu: image size", g_xnu.img.vmhigh - g_xnu.img.vmlow);
	if (g_xnu.mode != 3 && g_xnu.mode != 4) {
		xnu_logkv("xnu: region at", (u64_t)g_xnu.img.region.va);
		xnu_logkv("xnu: region size", g_xnu.img.region.size);
	}

	fki_random(&xnu_W.xnu_keys, sizeof(xnu_W.xnu_keys));
	xnu_W.xnu_keys.sctlr_en = ~0ULL;

	err = g_xnu.mode == 1 ? xnu_run_m1() :
	      (g_xnu.mode == 3 || g_xnu.mode == 4) ? xnu_run_m2b() :
	      xnu_run_m2a();
	if (err) {
		xnu_cleanup();
	}
	return err;
}

static void __exit xnu_exit(void)
{
	xnu_cleanup();
}

module_init(xnu_init);
module_exit(xnu_exit);

MODULE_LICENSE("Proprietary");
MODULE_AUTHOR("dere3046, Heyang Zhou");
MODULE_DESCRIPTION("XNU personality");
