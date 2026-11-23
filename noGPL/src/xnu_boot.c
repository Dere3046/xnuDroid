// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#include <linux/module.h>

#include "adt.h"
#include "xnu_boot.h"
#include "xnu_main.h"

#define XNU_FAKE_PHYS 0x800000000ULL
#define XNU_WINDOW_WANT_SLOTS 16U

typedef unsigned long (*strlen_fn)(const char *);
typedef unsigned int (*crc32_fn)(unsigned int, const void *, unsigned long);

static char g_root_matching[256];
static const char g_cmdline[] = "debug=0x8 -v -noprogress serial=0 startup_debug=1";
static const char g_cmdline_4k[] = " vm_compression_limit=1572864";
static const char g_cmdline_16k[] = " vm_compression_limit=393216 linux64_vm16k=1";

unsigned long long *xnu_tbl_va(u64_t desc)
{
	u64_t pa = desc & 0x0000fffffffff000ULL;

	if (pa < g_xnu.img.region.pa ||
	    pa >= g_xnu.img.region.pa + g_xnu.img.region.size) {
		return 0;
	}
	return (unsigned long long *)((char *)g_xnu.img.region.va +
				      (pa - g_xnu.img.region.pa));
}

void xnu_walk_log(const char *what, u64_t va, const unsigned long long *l1)
{
	u64_t d1 = l1[(va >> 30) & 511];
	u64_t d2 = 0;
	u64_t d3 = 0;

	xnu_logs(what);
	xnu_logkv("xnu:   va", va);
	xnu_logkv("xnu:   L1", d1);
	if ((d1 & 3) == 3) {
		unsigned long long *t2 = xnu_tbl_va(d1);

		if (!t2) {
			xnu_logs("xnu:   L2 table outside XNU DRAM");
			return;
		}
		d2 = t2[(va >> 21) & 511];
		xnu_logkv("xnu:   L2", d2);
		if ((d2 & 3) == 3) {
			unsigned long long *t3 = xnu_tbl_va(d2);

			if (!t3) {
				xnu_logs("xnu:   L3 table outside XNU DRAM");
				return;
			}
			d3 = t3[(va >> 12) & 511];
			xnu_logkv("xnu:   L3", d3);
		}
	}
}

__nocfi int xnu_run_m1(void)
{
	struct fki_pac_keys saved;
	unsigned long cookie;
	int cnt;
	u64_t ver;
	u64_t sl;
	u64_t cr;
	unsigned long len;
	unsigned int crc;

	cookie = fki_enter(&xnu_W.xnu_keys, &saved);
	cnt = macho_rebase(&g_xnu.img);
	fki_leave(cookie, &saved);
	xnu_logkv("xnu: threaded fixups applied", (u64_t)cnt);
	if (cnt < 0) {
		return cnt;
	}

	ver = macho_sym(&g_xnu.img, "_version");
	sl = macho_sym(&g_xnu.img, "_strlen");
	cr = macho_sym(&g_xnu.img, "_crc32");
	if (!ver || !sl || !cr) {
		xnu_logs("xnu: symbols missing");
		return 0;
	}

	cookie = fki_enter(&xnu_W.xnu_keys, &saved);
	len = ((strlen_fn)sl)((const char *)ver);
	crc = ((crc32_fn)cr)(0, (const void *)ver, len);
	fki_leave(cookie, &saved);

	xnu_logs("xnu: XNU code ran inside Linux");
	fki_log((const char *)ver, len);
	xnu_logkv("xnu: strlen(version)", len);
	xnu_logkv("xnu: crc32(version)", crc);
	return 0;
}

static struct boot_args_v2 *build_boot(unsigned char *base_va, u64_t phys,
				       u64_t size, u64_t image_off,
				       u64_t image_size)
{
	static unsigned char seed[256];
	u64_t off_args = (image_off + image_size + 0x3fff) & ~0x3fffULL;
	struct boot_args_v2 *ba = (void *)(base_va + off_args);
	u64_t off_dt = off_args + 0x4000;
	struct adt adt;
	int berr;
	int i;
	int cpu;
	u64_t mpidr;
	u64_t off_rd;
	u64_t off_tc;
	u64_t off_fb;
	u64_t dt_len;

	fki_random(seed, sizeof(seed));
	berr = fki_bind_cpu(0);
	if (berr) {
		xnu_logkv("xnu: cannot bind to cpu0, err", (u64_t)berr);
		return 0;
	}
	if (g_xnu.cpus < 1) {
		g_xnu.cpus = 1;
	}
	if (g_xnu.cpus > XNU_MAX_CPUS) {
		g_xnu.cpus = XNU_MAX_CPUS;
	}
	if (g_xnu.cpus > fki_num_cpus()) {
		g_xnu.cpus = fki_num_cpus();
	}
	for (i = 0; i < LINUX64_CPU_SLOTS; i++) {
		g_xnu.linux_cpu_of_slot[i] = -1;
	}
	for (cpu = 0; cpu < g_xnu.cpus; cpu++) {
		fki_u64 cpu_mpidr = 0;

		if (fki_cpu_mpidr(cpu, &cpu_mpidr) ||
		    g_xnu.linux_cpu_of_slot[LINUX64_CPU_SLOT(cpu_mpidr)] >= 0) {
			xnu_logkv("xnu: no distinct MPIDR slot for CPU",
				  (u64_t)cpu);
			g_xnu.cpus = cpu;
			break;
		}
		g_xnu.cpu_mpidr[cpu] = cpu_mpidr & 0xffff;
		g_xnu.linux_cpu_of_slot[LINUX64_CPU_SLOT(cpu_mpidr)] = (signed char)cpu;
		xnu_worlds[LINUX64_CPU_SLOT(cpu_mpidr)] = cpu ? &g_worlds[cpu] : &xnu_W;
	}

	mpidr = g_xnu.cpu_mpidr[0];
	xnu_logkv("xnu: running on MPIDR Aff1:Aff0", mpidr);
	xnu_logkv("xnu: XNU CPUs", (u64_t)g_xnu.cpus);

	adt_init(&adt, base_va + off_dt, 0x40000);
	adt_node(&adt, 3, 6);
	adt_prop_str(&adt, "name", "device-tree");
	{
		static char compat[96];
		static const char own[] = "xnulinux,linux64";
		unsigned own_len = sizeof(own);
		unsigned model_len = (unsigned)x_strlen(g_xnu.model);

		x_memcpy(compat, own, own_len);
		if (model_len && own_len + model_len + 1 <= sizeof(compat) &&
		    x_strcmp(g_xnu.model, "xnulinux")) {
			x_memcpy(compat + own_len, g_xnu.model, model_len + 1);
			own_len += model_len + 1;
		}
		adt_prop(&adt, "compatible", compat, own_len);
	}
	adt_prop_str(&adt, "model", g_xnu.model);

	if (g_xnu.root_uuid && x_strlen(g_xnu.root_uuid) == 36) {
		static const char pre[] =
			"<dict><key>IOProviderClass</key><string>IOMedia</string>"
			"<key>IOPropertyMatch</key><dict><key>UUID</key><string>";
		static const char post[] = "</string></dict></dict>";

		x_memcpy(g_root_matching, pre, sizeof(pre) - 1);
		x_memcpy(g_root_matching + sizeof(pre) - 1, g_xnu.root_uuid, 36);
		x_memcpy(g_root_matching + sizeof(pre) - 1 + 36, post,
			 sizeof(post));
	}

	adt_node(&adt, 22 + (g_xnu.boot_uuid ? 1 : 0) +
		 (g_xnu.preboot_uuid ? 1 : 0) +
		 (g_root_matching[0] ? 1 : 0), 3);
	if (g_root_matching[0]) {
		adt_prop_str(&adt, "root-matching", g_root_matching);
	}
	if (g_xnu.boot_uuid) {
		adt_prop_str(&adt, "boot-uuid", g_xnu.boot_uuid);
	}
	if (g_xnu.preboot_uuid) {
		adt_prop_str(&adt, "apfs-preboot-uuid", g_xnu.preboot_uuid);
	}
	adt_prop_str(&adt, "name", "chosen");
	adt_prop_u64(&adt, "dram-base", phys);
	adt_prop_u64(&adt, "dram-size", size);
	adt_prop(&adt, "random-seed", seed, sizeof(seed));
	adt_prop_str(&adt, "firmware-version", "xnulinux-m2");
	adt_prop_u32(&adt, "debug-enabled", 1);
	{
		static const unsigned char zero48[48];

		adt_prop(&adt, "boot-manifest-hash", zero48, sizeof(zero48));
	}
	{
		static unsigned char nvram[0x10000];

		adt_prop_u32(&adt, "nvram-bank-size", sizeof(nvram));
		adt_prop_u32(&adt, "nvram-bank-count", 1);
		adt_prop_u32(&adt, "nvram-current-bank", 0);
		adt_prop(&adt, "nvram-proxy-data", nvram, sizeof(nvram));
	}
	adt_prop_str(&adt, "crypto-hash-method", "sha2-384");
	adt_prop_u32(&adt, "chip-id", 0xfe00);
	adt_prop_u32(&adt, "board-id", 0x20);
	adt_prop_u64(&adt, "unique-chip-id", 0x0000584e554c4e58ULL);
	adt_prop_u32(&adt, "chip-epoch", 0);
	adt_prop_u32(&adt, "security-domain", 1);
	adt_prop_u32(&adt, "certificate-production-status", 0);
	adt_prop_u32(&adt, "certificate-security-mode", 0);
	adt_prop_u32(&adt, "effective-production-status-ap", 0);
	adt_prop_u32(&adt, "effective-security-mode-ap", 0);
	adt_prop_u32(&adt, "mix-n-match-prevention-status", 0);

	off_rd = (off_dt + 0x40000 + 0x3fff) & ~0x3fffULL;
	off_tc = (off_rd + g_xnu.rd_size + 0x3fff) & ~0x3fffULL;
	off_fb = (off_tc + XNU_TC_SEG_SIZE + 0x3fff) & ~0x3fffULL;

	adt_node(&adt, 1 + (g_xnu.rd_size ? 1 : 0) + (g_xnu.tc_size ? 1 : 0),
		 0);
	adt_prop_str(&adt, "name", "memory-map");
	if (g_xnu.rd_size) {
		u64_t rd[2] = { phys + off_rd, g_xnu.rd_size };

		adt_prop(&adt, "RAMDisk", rd, sizeof(rd));
	}
	if (g_xnu.tc_size) {
		u64_t tc[2] = { phys + off_tc, XNU_TC_SEG_SIZE };

		adt_prop(&adt, "TrustCache", tc, sizeof(tc));
	}
	adt_node(&adt, 1, 0);
	adt_prop_str(&adt, "name", "manifest-properties");
	adt_node(&adt, 3, 0);
	adt_prop_str(&adt, "name", "asmb");
	adt_prop_u64(&adt, "lp-sip0", 0x2);
	adt_prop_u32(&adt, "lp-sip1", 1);

	adt_node(&adt, 1, g_xnu.cpus);
	adt_prop_str(&adt, "name", "cpus");
	for (cpu = 0; cpu < g_xnu.cpus; cpu++) {
		static char cname[XNU_MAX_CPUS][8];

		cname[cpu][0] = 'c';
		cname[cpu][1] = 'p';
		cname[cpu][2] = 'u';
		if (cpu < 10) {
			cname[cpu][3] = '0' + cpu;
			cname[cpu][4] = 0;
		} else {
			cname[cpu][3] = '1';
			cname[cpu][4] = '0' + cpu - 10;
			cname[cpu][5] = 0;
		}
		adt_node(&adt, 6, 0);
		adt_prop_str(&adt, "name", cname[cpu]);
		adt_prop_str(&adt, "compatible", "xnulinux,cpu");
		adt_prop_str(&adt, "state", cpu ? "waiting" : "running");
		adt_prop_u32(&adt, "reg", (unsigned)g_xnu.cpu_mpidr[cpu]);
		adt_prop_u64(&adt, "timebase-frequency",
			     fki_counter_frequency());
		adt_prop_u32(&adt, "cluster-type", 'P');
	}

	adt_node(&adt, 1, 0);
	adt_prop_str(&adt, "name", "defaults");
	adt_node(&adt, 4, 0);
	adt_prop_str(&adt, "name", "product");
	adt_prop_str(&adt, "sub-product-type", g_xnu.model);
	adt_prop_str(&adt, "unique-model", g_xnu.model);
	adt_prop_str(&adt, "product-name", "xnulinux");
	adt_node(&adt, 1, 0);
	adt_prop_str(&adt, "name", "options");

	adt_node(&adt, 3, XNU_FB_SIZE ? 1 : 0);
	adt_prop_str(&adt, "name", "arm-io");
	adt_prop_str(&adt, "device_type", "linux64-io");
	{
		u64_t ranges[3] = { 0, 0x7000000000ULL, 0x10000000ULL };

		adt_prop(&adt, "ranges", ranges, sizeof(ranges));
	}
	if (XNU_FB_SIZE) {
		adt_node(&adt, 2, 0);
		adt_prop_str(&adt, "name", "gpu");
		adt_prop_str(&adt, "compatible", "paravirtualizedgraphics,gpu");
	}
	if (adt.err) {
		xnu_logs("xnu: ADT overflow");
		return 0;
	}
	dt_len = adt_size(&adt);

	ba->revision = 2;
	ba->version = 2;
	ba->virt_base = (u64_t)base_va;
	ba->phys_base = phys;
	ba->mem_size = size;
	ba->mem_size_actual = size;
	ba->top_of_kernel_data = phys + ((off_dt + dt_len + 0x3fff) &
					 ~0x3fffULL);

	if (g_xnu.rd_size) {
		x_memcpy(base_va + off_rd, g_xnu.rd_data, g_xnu.rd_size);
		ba->top_of_kernel_data = phys + ((off_rd + g_xnu.rd_size +
						  0x3fff) & ~0x3fffULL);
		xnu_logkv("xnu: root RAM disk bytes", g_xnu.rd_size);
	}
	if (g_xnu.tc_size) {
		unsigned int hdr[2] = { 1, 8 };

		x_memcpy(base_va + off_tc, hdr, sizeof(hdr));
		x_memcpy(base_va + off_tc + 8, g_xnu.tc_data, g_xnu.tc_size);
		ba->top_of_kernel_data = phys + ((off_tc + XNU_TC_SEG_SIZE +
						  0x3fff) & ~0x3fffULL);
		xnu_logkv("xnu: static trust cache bytes", g_xnu.tc_size);
	}
	if (XNU_FB_SIZE) {
		unsigned int *px = (unsigned int *)(base_va + off_fb);
		u64_t fi;

		for (fi = 0; fi < XNU_FB_SIZE / 4; fi++) {
			px[fi] = 0xff000000;
		}
		ba->video.v_base_addr = phys + off_fb;
		ba->video.v_display = 0;
		ba->video.v_row_bytes = (u64_t)g_xnu.fb_width * 4;
		ba->video.v_width = (u64_t)g_xnu.fb_width;
		ba->video.v_height = (u64_t)g_xnu.fb_height;
		ba->video.v_depth = 32;
		ba->top_of_kernel_data = phys + ((off_fb + XNU_FB_SIZE +
						  0x3fff) & ~0x3fffULL);
		g_xnu.fb_off = off_fb;
		xnu_logkv("xnu: framebuffer bytes", XNU_FB_SIZE);
	}

	ba->device_tree_p = base_va + off_dt;
	ba->device_tree_length = (unsigned)dt_len;
	x_memcpy(ba->command_line, g_cmdline, x_strlen(g_cmdline) + 1);
	{
		const char *pg = g_xnu.vm16k ? g_cmdline_16k : g_cmdline_4k;

		x_memcpy(ba->command_line + x_strlen(ba->command_line), pg,
			 x_strlen(pg) + 1);
	}
	if (g_xnu.rd_size || g_xnu.hd_file) {
		const char *rd = " rd=md0";

		x_memcpy(ba->command_line + x_strlen(ba->command_line), rd,
			 x_strlen(rd) + 1);
	}
	if (g_xnu.bootargs &&
	    x_strlen(ba->command_line) + 1 + x_strlen(g_xnu.bootargs) <
	    sizeof(ba->command_line)) {
		char *tail = ba->command_line + x_strlen(ba->command_line);

		*tail++ = ' ';
		x_memcpy(tail, g_xnu.bootargs, x_strlen(g_xnu.bootargs) + 1);
	}
	xnu_logkv("xnu: boot args at", (u64_t)ba);
	xnu_logkv("xnu: ADT bytes", dt_len);
	xnu_logkv("xnu: topOfKernelData (phys)", ba->top_of_kernel_data);
	return ba;
}

int xnu_run_m2a(void)
{
	u64_t image_size = g_xnu.img.vmhigh - g_xnu.img.vmlow;
	struct boot_args_v2 *ba;

	ba = build_boot(g_xnu.img.region.va, XNU_FAKE_PHYS,
			g_xnu.img.region.size, 0, image_size);
	if (!ba) {
		return -22;
	}
	return xnu_enter_arm_init(ba);
}

static int scan_free_l1(const unsigned long long *l1, unsigned lo,
			unsigned hi, unsigned min_slots,
			unsigned want_slots, unsigned *out_lo,
			unsigned *out_hi)
{
	unsigned best_lo = 0;
	unsigned best_hi = 0;
	unsigned best_len = 0;
	unsigned i = hi;

	for (;;) {
		unsigned run_hi;
		unsigned run_lo;
		unsigned len;

		if (l1[i]) {
			if (i == lo) {
				break;
			}
			i--;
			continue;
		}
		run_hi = i;
		while (i > lo && !l1[i - 1]) {
			i--;
		}
		run_lo = i;
		len = run_hi - run_lo + 1;
		if (len >= want_slots) {
			*out_lo = run_lo;
			*out_hi = run_hi;
			return 0;
		}
		if (len >= min_slots && len > best_len) {
			best_lo = run_lo;
			best_hi = run_hi;
			best_len = len;
		}
		if (i == lo) {
			break;
		}
		i--;
	}
	if (!best_len) {
		return -16;
	}
	*out_lo = best_lo;
	*out_hi = best_hi;
	return 0;
}

static int pick_boot_window(const unsigned long long *l1, u64_t nat_base,
			    unsigned need_slots, unsigned want_slots,
			    u64_t *win_base, unsigned *idx_out,
			    unsigned *first_out, unsigned *last_out)
{
	unsigned long long tcr;
	unsigned va_bits;
	unsigned scan_lo;
	unsigned scan_hi;
	unsigned nat_idx;
	unsigned lo;
	unsigned hi;

	__asm__ volatile("mrs %0, tcr_el1" : "=r"(tcr));
	va_bits = 64U - (unsigned)((tcr >> 16) & 0x3fULL);
	scan_lo = va_bits <= 39U ? XNU_L1_SCAN_LO : XNU_L1_FIRST;
	scan_hi = va_bits <= 39U ? XNU_L1_SCAN_HI : XNU_L1_LAST;

	nat_idx = (unsigned)((nat_base >> 30) & 511U);
	if (nat_base >= XNU_VA_LO && nat_base < XNU_VA_HI &&
	    nat_idx + need_slots - 1 <= XNU_L1_LAST) {
		unsigned k;

		for (k = 0; k < need_slots; k++) {
			if (l1[nat_idx + k]) {
				break;
			}
		}
		if (k == need_slots) {
			lo = nat_idx;
			while (lo > XNU_L1_FIRST && !l1[lo - 1]) {
				lo--;
			}
			hi = nat_idx + need_slots - 1;
			while (hi < XNU_L1_LAST && !l1[hi + 1]) {
				hi++;
			}
			*win_base = nat_base;
			*idx_out = nat_idx;
			*first_out = lo;
			*last_out = hi;
			return 0;
		}
	}

	if (!g_xnu.auto_slide) {
		xnu_logkv("xnu: L1 slot already used by Linux", l1[nat_idx]);
		return -16;
	}
	if (scan_free_l1(l1, scan_lo, scan_hi, need_slots, want_slots,
			 &lo, &hi)) {
		xnu_logs("xnu: no free L1 window");
		return -16;
	}
	*idx_out = lo;
	*first_out = lo;
	*last_out = hi;
	*win_base = XNU_VA_BASE + ((u64_t)lo << 30);
	xnu_logkv("xnu: relocated L1 window slot", lo);
	xnu_logkv("xnu: relocated L1 window count", hi - lo + 1);
	return 0;
}

int xnu_run_m2b(void)
{
	u64_t image_size = g_xnu.img.vmhigh - g_xnu.img.vmlow;
	u64_t nat_base = g_xnu.img.vmlow & ~0x1fffffULL;
	u64_t image_off = g_xnu.img.vmlow - nat_base;
	u64_t want;
	u64_t boot_map;
	u64_t win_base;
	u64_t off;
	unsigned long long *l2;
	unsigned long long *l1;
	fki_u64 l1pa;
	unsigned want_slots;
	unsigned first;
	unsigned idx;
	unsigned l1_first;
	unsigned l1_last;
	unsigned char *base;
	struct boot_args_v2 *ba;
	int err;

	if (g_xnu.img.vmlow < XNU_VA_LO) {
		xnu_logs("xnu: unexpected link address");
		return -22;
	}
	want = image_off + image_size + 0x60000 + XNU_BOOT_EXTRA +
	       ((u64_t)g_xnu.ram_mb << 20);
	err = fki_window_l1(XNU_VA_LO, &l1, &l1pa);
	if (err) {
		xnu_logs("xnu: no L1 table for top window");
		return err;
	}
	xnu_logkv("xnu: top window L1 phys", l1pa);
	want_slots = (unsigned)((want + (1ULL << 30) - 1) >> 30);
	if (want_slots < XNU_WINDOW_WANT_SLOTS) {
		want_slots = XNU_WINDOW_WANT_SLOTS;
	}
	err = pick_boot_window(l1, nat_base, 1U, want_slots, &win_base, &idx,
			       &l1_first, &l1_last);
	if (err) {
		return err;
	}
	g_xnu.l1_first = l1_first;
	g_xnu.l1_last = l1_last;
	g_xnu.va_lo = XNU_VA_BASE + ((u64_t)l1_first << 30);
	g_xnu.va_hi = XNU_VA_BASE + ((u64_t)(l1_last + 1) << 30);

	err = fki_contig_alloc(want, &g_xnu.img.region);
	if (err) {
		xnu_logkv("xnu: contiguous allocation failed", (u64_t)err);
		return err;
	}
	xnu_logkv("xnu: contiguous DRAM phys", g_xnu.img.region.pa);
	xnu_logkv("xnu: contiguous DRAM size", g_xnu.img.region.size);
	if (g_xnu.img.region.pa & 0x1fffff) {
		xnu_logs("xnu: region not 2 MiB aligned");
		return -22;
	}

	err = fki_contig_alloc(4096, &g_xnu.l2page);
	if (err) {
		return err;
	}
	l2 = g_xnu.l2page.va;
	first = (unsigned)((win_base >> 21) & 511U);
	boot_map = g_xnu.img.region.size;
	if (boot_map > ((u64_t)(512 - first) << 21)) {
		boot_map = (u64_t)(512 - first) << 21;
	}
	if (boot_map < image_off + image_size + 0x60000 + XNU_BOOT_EXTRA +
	    (64ULL << 20)) {
		xnu_logs("xnu: image does not fit in bootstrap window");
		return -22;
	}
	xnu_logkv("xnu: bootstrap window base", win_base);
	xnu_logkv("xnu: bootstrap window bytes", boot_map);
	xnu_logkv("xnu: L1 slot", idx);

	for (off = 0; off < boot_map; off += 0x200000) {
		l2[first + (off >> 21)] = (g_xnu.img.region.pa + off) |
					  (1ULL << 54) | (1ULL << 51) |
					  (1ULL << 10) | (3ULL << 8) | 1;
	}

	g_xnu.window_slot = &l1[idx];
	g_xnu.window_l1 = l1;
	g_xnu.window_owned[idx] = 1;
	fki_window_set(g_xnu.window_slot, g_xnu.l2page.pa | 3);
	xnu_logs("xnu: bootstrap window mapped");

	base = (unsigned char *)win_base;
	macho_copy(&g_xnu.img, base + image_off);
	g_xnu.img.slide = (u64_t)(base + image_off) - g_xnu.img.vmlow;
	ba = build_boot(base, g_xnu.img.region.pa, g_xnu.img.region.size,
			image_off, image_size);
	if (!ba) {
		return -22;
	}
	return xnu_enter_arm_init(ba);
}
