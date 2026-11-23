// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/printk.h>

#include "core.h"
#include "fki.h"
#include "sym_reg.h"
#include "type_info.h"

#define FKI_BTF_KIND_STRUCT 4

struct fki_sym_entry {
	const char *name;
	const char *fallback;
	bool required;
	unsigned long addr;
};

struct fki_layout_entry {
	const char *type;
	const char *member;
	u32 off;
	bool valid;
};

static struct fki_sym_entry g_syms[FKI_SYM_COUNT] = {
	[FKI_SYM_SET_MEMORY_X] = { "set_memory_x", NULL, true, 0 },
	[FKI_SYM_SET_MEMORY_NX] = { "set_memory_nx", NULL, true, 0 },
	[FKI_SYM_ALLOC_CONTIG] = { "alloc_contig_pages_noprof", "alloc_contig_pages", false, 0 },
	[FKI_SYM_FREE_CONTIG] = { "free_contig_range", NULL, false, 0 },
	[FKI_SYM_CMA_AREA] = { "dma_contiguous_default_area", NULL, false, 0 },
	[FKI_SYM_CMA_ALLOC] = { "cma_alloc", NULL, false, 0 },
	[FKI_SYM_CMA_RELEASE] = { "cma_release", NULL, false, 0 },
	[FKI_SYM_STEXT] = { "_stext", NULL, false, 0 },
	[FKI_SYM_END] = { "_end", NULL, false, 0 },
	[FKI_SYM_KIMAGE_VOFFSET] = { "kimage_voffset", NULL, false, 0 },
	[FKI_SYM_EFI_MEM_TYPE] = { "efi_mem_type", NULL, false, 0 },
	[FKI_SYM_SCHED_HRTIMEOUT] = { "schedule_hrtimeout_range", NULL, false, 0 },
	[FKI_SYM_FILP_OPEN] = { "filp_open", NULL, false, 0 },
	[FKI_SYM_KERNEL_READ] = { "kernel_read", NULL, false, 0 },
	[FKI_SYM_KERNEL_WRITE] = { "kernel_write", NULL, false, 0 },
	[FKI_SYM_SWAPPER_PG_DIR] = { "swapper_pg_dir", NULL, false, 0 },
	[FKI_SYM_SET_SWAPPER_PGD] = { "set_swapper_pgd", NULL, false, 0 },
	[FKI_SYM_REG_BRK_HOOK] = { "register_kernel_break_hook", NULL, false, 0 },
	[FKI_SYM_UNREG_BRK_HOOK] = { "unregister_kernel_break_hook", NULL, false, 0 },
};

static struct fki_layout_entry g_layouts[FKI_LAY_COUNT] = {
	[FKI_LAY_PT_REGS_REGS] = { "pt_regs", "regs", 0, false },
	[FKI_LAY_PT_REGS_PC] = { "pt_regs", "pc", 0, false },
	[FKI_LAY_PT_REGS_PSTATE] = { "pt_regs", "pstate", 0, false },
	[FKI_LAY_TASK_STACK] = { "task_struct", "stack", 0, false },
	[FKI_LAY_TASK_MM] = { "task_struct", "mm", 0, false },
	[FKI_LAY_VMA_START] = { "vm_area_struct", "vm_start", 0, false },
	[FKI_LAY_VMA_FLAGS] = { "vm_area_struct", "vm_flags", 0, false },
	[FKI_LAY_NETDEV_NAME] = { "net_device", "name", 0, false },
	[FKI_LAY_NETDEV_OPS] = { "net_device", "netdev_ops", 0, false },
	[FKI_LAY_NETDEV_MIN_MTU] = { "net_device", "min_mtu", 0, false },
	[FKI_LAY_NETDEV_MAX_MTU] = { "net_device", "max_mtu", 0, false },
	[FKI_LAY_NETDEV_STATS] = { "net_device", "stats", 0, false },
	[FKI_LAY_NETSTATS_RX_PKTS] = { "net_device_stats", "rx_packets", 0, false },
	[FKI_LAY_NETSTATS_RX_BYTES] = { "net_device_stats", "rx_bytes", 0, false },
	[FKI_LAY_NETSTATS_RX_DROP] = { "net_device_stats", "rx_dropped", 0, false },
	[FKI_LAY_NETSTATS_TX_PKTS] = { "net_device_stats", "tx_packets", 0, false },
	[FKI_LAY_NETSTATS_TX_BYTES] = { "net_device_stats", "tx_bytes", 0, false },
	[FKI_LAY_NETSTATS_TX_DROP] = { "net_device_stats", "tx_dropped", 0, false },
	[FKI_LAY_FILE_MAPPING] = { "file", "f_mapping", 0, false },
	[FKI_LAY_ADDR_HOST] = { "address_space", "host", 0, false },
	[FKI_LAY_INODE_SIZE] = { "inode", "i_size", 0, false },
	[FKI_LAY_FW_DATA] = { "firmware", "data", 0, false },
	[FKI_LAY_FW_SIZE] = { "firmware", "size", 0, false },
};

static bool g_sym_ready;

__nocfi noinline unsigned long fki_sym_resolve(const char *name)
{
	unsigned long addr = 0;

	if (!name) {
		return 0;
	}
	if (kallrecon_klp) {
		addr = kallrecon_klp(name);
	}
	if (!addr) {
		addr = kallsyms_name_to_addr(name);
	}
	return addr;
}

static void fki_probe_layouts(void)
{
	struct ti_resolver res;
	struct ti_ctx *base;
	u32 resolved = 0;
	u32 missing = 0;
	int err;
	int i;

	res.name_to_addr = fki_sym_resolve;
	err = ti_init(&res);
	if (err || !ti_btf_available()) {
		pr_warn("[fki] layout: BTF unavailable (%d)\n", err);
		return;
	}

	base = ti_base();
	for (i = 0; i < FKI_LAY_COUNT; i++) {
		struct fki_layout_entry *entry = &g_layouts[i];
		u32 bit_off = 0;
		u32 bit_sz = 0;
		u32 tid = 0;

		if (ti_type_by_name(base, entry->type, BIT(FKI_BTF_KIND_STRUCT),
				    &tid) ||
		    ti_member_off(base, tid, entry->member, &bit_off,
				  &bit_sz)) {
			missing++;
			pr_warn("[fki] layout: missing %s.%s\n",
				entry->type, entry->member);
			continue;
		}
		entry->off = bit_off / 8;
		entry->valid = true;
		resolved++;
	}

	pr_info("[fki] layout: BTF %u/%d fields resolved, %u missing\n",
		resolved, FKI_LAY_COUNT, missing);
}

int fki_sym_init(void)
{
	int found = 0;
	int i;

	if (g_sym_ready) {
		return 0;
	}

	find_kallsyms_base();
	if (!klnum_val || !kallrecon_klp) {
		pr_err("[fki] kallrecon init failed\n");
		return -ENODEV;
	}

	for (i = 0; i < FKI_SYM_COUNT; i++) {
		struct fki_sym_entry *sym = &g_syms[i];

		sym->addr = fki_sym_resolve(sym->name);
		if (!sym->addr && sym->fallback) {
			sym->addr = fki_sym_resolve(sym->fallback);
		}
		if (sym->addr) {
			found++;
		} else if (sym->required) {
			pr_err("[fki] missing required symbol: %s\n",
			       sym->name);
			return -ENOENT;
		}
	}

	pr_info("[fki] symbols: %d/%d resolved\n", found, FKI_SYM_COUNT);
	fki_probe_layouts();
	g_sym_ready = true;
	return 0;
}

void fki_sym_exit(void)
{
	ti_exit();
	g_sym_ready = false;
}

unsigned long fki_sym_addr(enum fki_sym_id id)
{
	if (id < 0 || id >= FKI_SYM_COUNT) {
		return 0;
	}
	return g_syms[id].addr;
}

bool fki_layout_ok(enum fki_layout_id id)
{
	if (id < 0 || id >= FKI_LAY_COUNT) {
		return false;
	}
	return g_layouts[id].valid;
}

u32 fki_layout_off(enum fki_layout_id id)
{
	if (!fki_layout_ok(id)) {
		return 0;
	}
	return g_layouts[id].off;
}

void *fki_layout_ptr(const void *base, enum fki_layout_id id)
{
	if (!base || !fki_layout_ok(id)) {
		return NULL;
	}
	return (char *)base + g_layouts[id].off;
}

unsigned long fki_kr_resolve(const char *name)
{
	if (!g_sym_ready) {
		return 0;
	}
	return fki_sym_resolve(name);
}
EXPORT_SYMBOL(fki_kr_resolve);

int fki_kr_addr_name(unsigned long addr, char *buf, int max)
{
	if (!g_sym_ready || !buf || max <= 0) {
		return -EINVAL;
	}
	return sym_name_at(addr, buf, max);
}
EXPORT_SYMBOL(fki_kr_addr_name);

unsigned long fki_kr_base(void)
{
	return klbase_val;
}
EXPORT_SYMBOL(fki_kr_base);

unsigned int fki_kr_num_syms(void)
{
	return klnum_val;
}
EXPORT_SYMBOL(fki_kr_num_syms);

int fki_ti_btf_available(void)
{
	return ti_btf_available() ? 1 : 0;
}
EXPORT_SYMBOL(fki_ti_btf_available);

int fki_ti_type_by_name(const char *name, unsigned int kind,
			unsigned int *type_id)
{
	u32 mask;

	if (!name || !type_id || !ti_btf_available()) {
		return -EINVAL;
	}
	mask = kind ? (1U << kind) : 0;
	return ti_type_by_name(ti_base(), name, mask, type_id);
}
EXPORT_SYMBOL(fki_ti_type_by_name);

unsigned int fki_ti_type_size(unsigned int type_id)
{
	if (!ti_btf_available()) {
		return 0;
	}
	return ti_type_size(ti_base(), type_id);
}
EXPORT_SYMBOL(fki_ti_type_size);

int fki_ti_member_off(unsigned int type_id, const char *member,
		      unsigned int *bit_off, unsigned int *bit_sz)
{
	u32 off = 0;
	u32 sz = 0;
	int err;

	if (!member || !ti_btf_available()) {
		return -EINVAL;
	}
	err = ti_member_off(ti_base(), type_id, member, &off, &sz);
	if (err) {
		return err;
	}
	if (bit_off) {
		*bit_off = off;
	}
	if (bit_sz) {
		*bit_sz = sz;
	}
	return 0;
}
EXPORT_SYMBOL(fki_ti_member_off);

int fki_ti_struct_off(const char *type, const char *member,
		      unsigned int *byte_off)
{
	u32 tid = 0;
	u32 bit_off = 0;
	u32 bit_sz = 0;
	int err;

	if (!type || !member || !byte_off || !ti_btf_available()) {
		return -EINVAL;
	}
	err = ti_type_by_name(ti_base(), type, BIT(FKI_BTF_KIND_STRUCT), &tid);
	if (err) {
		return err;
	}
	err = ti_member_off(ti_base(), tid, member, &bit_off, &bit_sz);
	if (err) {
		return err;
	}
	*byte_off = bit_off / 8;
	return 0;
}
EXPORT_SYMBOL(fki_ti_struct_off);
