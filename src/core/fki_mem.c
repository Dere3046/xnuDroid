// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#include <linux/efi.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/hardirq.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/irqflags.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/nodemask.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <asm/barrier.h>
#include <asm/pgtable.h>
#include <asm/sysreg.h>
#include <asm/tlbflush.h>

#include "fki.h"
#include "fki_mem.h"
#include "hk_patch.h"
#include "sym_reg.h"

struct cma;

typedef int (*set_memory_fn)(unsigned long addr, int numpages);
typedef void *(*vzalloc_fn)(unsigned long size);
typedef void (*vfree_fn)(const void *addr);
typedef struct page *(*vmalloc_to_page_fn)(const void *vmalloc_addr);
typedef struct page *(*alloc_contig_fn)(unsigned long nr_pages, gfp_t gfp,
					int nid, nodemask_t *nodemask);
typedef void (*free_contig_fn)(unsigned long pfn, unsigned long nr_pages);
typedef struct page *(*cma_alloc_fn)(struct cma *cma, unsigned long count,
				     unsigned int align, bool no_warn);
typedef bool (*cma_release_fn)(struct cma *cma, const struct page *pages,
			       unsigned long count);
typedef int (*efi_mem_type_fn)(unsigned long phys_addr);
typedef int (*region_intersects_fn)(resource_size_t offset, size_t size,
				    unsigned long flags, unsigned long desc);
typedef void *(*memremap_fn)(resource_size_t offset, size_t size,
			     unsigned long flags);
typedef void (*memunmap_fn)(void *addr);
typedef void (*set_swapper_pgd_fn)(pgd_t *pgdp, pgd_t pgd);

static unsigned long carve_base;
module_param(carve_base, ulong, 0444);
static unsigned long carve_mb;
module_param(carve_mb, ulong, 0444);
static unsigned long reserve_mb;
module_param(reserve_mb, ulong, 0444);

static struct page *g_resv_pg;
static unsigned long g_resv_cnt;
static bool g_resv_used;
static bool g_carve_used;

static __nocfi noinline int call_set_memory_x(unsigned long addr, int n)
{
	set_memory_fn fn = (set_memory_fn)fki_sym_addr(FKI_SYM_SET_MEMORY_X);

	if (!fn) {
		return -ENOENT;
	}
	return fn(addr, n);
}

static __nocfi noinline int call_set_memory_nx(unsigned long addr, int n)
{
	set_memory_fn fn = (set_memory_fn)fki_sym_addr(FKI_SYM_SET_MEMORY_NX);

	if (!fn) {
		return -ENOENT;
	}
	return fn(addr, n);
}

static __nocfi noinline struct page *call_alloc_contig(unsigned long nr_pages,
						       gfp_t gfp, int nid)
{
	alloc_contig_fn fn = (alloc_contig_fn)fki_sym_addr(FKI_SYM_ALLOC_CONTIG);

	if (!fn) {
		return NULL;
	}
	return fn(nr_pages, gfp, nid, NULL);
}

static __nocfi noinline void call_free_contig(unsigned long pfn,
					      unsigned long nr_pages)
{
	free_contig_fn fn = (free_contig_fn)fki_sym_addr(FKI_SYM_FREE_CONTIG);

	if (!fn) {
		return;
	}
	fn(pfn, nr_pages);
}

static struct cma *get_default_cma(void)
{
	struct cma **area = (struct cma **)fki_sym_addr(FKI_SYM_CMA_AREA);

	if (!area) {
		return NULL;
	}
	return *area;
}

static __nocfi noinline struct page *call_cma_alloc(unsigned long cnt)
{
	struct cma *cma = get_default_cma();
	cma_alloc_fn fn = (cma_alloc_fn)fki_sym_addr(FKI_SYM_CMA_ALLOC);

	if (!cma || !fn) {
		return NULL;
	}
	return fn(cma, cnt, get_order(SZ_2M), true);
}

static __nocfi noinline void call_cma_release(const struct page *pg,
					      unsigned long cnt)
{
	struct cma *cma = get_default_cma();
	cma_release_fn fn = (cma_release_fn)fki_sym_addr(FKI_SYM_CMA_RELEASE);

	if (!cma || !fn) {
		return;
	}
	fn(cma, pg, cnt);
}

static __nocfi noinline int call_efi_mem_type(unsigned long phys)
{
	efi_mem_type_fn fn = (efi_mem_type_fn)fki_sym_addr(FKI_SYM_EFI_MEM_TYPE);

	if (!fn) {
		return -ENODEV;
	}
	return fn(phys);
}

static __nocfi noinline void *call_vzalloc(unsigned long size)
{
	vzalloc_fn fn = (vzalloc_fn)fki_sym_addr(FKI_SYM_VZALLOC);

	if (!fn) {
		return NULL;
	}
	return fn(size);
}

static __nocfi noinline void call_vfree(const void *addr)
{
	vfree_fn fn = (vfree_fn)fki_sym_addr(FKI_SYM_VFREE);

	if (fn) {
		fn(addr);
	}
}

static __nocfi noinline struct page *call_vmalloc_to_page(const void *addr)
{
	vmalloc_to_page_fn fn;

	fn = (vmalloc_to_page_fn)fki_sym_addr(FKI_SYM_VMALLOC_TO_PAGE);
	if (!fn) {
		return NULL;
	}
	return fn(addr);
}

static __nocfi noinline int call_region_intersects(resource_size_t offset,
						   size_t size,
						   unsigned long flags,
						   unsigned long desc)
{
	region_intersects_fn fn;

	fn = (region_intersects_fn)fki_sym_addr(FKI_SYM_REGION_INTERSECTS);
	if (!fn) {
		return REGION_MIXED;
	}
	return fn(offset, size, flags, desc);
}

static __nocfi noinline void *call_memremap(resource_size_t offset,
					    size_t size, unsigned long flags)
{
	memremap_fn fn = (memremap_fn)fki_sym_addr(FKI_SYM_MEMREMAP);

	if (!fn) {
		return NULL;
	}
	return fn(offset, size, flags);
}

static __nocfi noinline void call_memunmap(void *addr)
{
	memunmap_fn fn = (memunmap_fn)fki_sym_addr(FKI_SYM_MEMUNMAP);

	if (fn) {
		fn(addr);
	}
}

int fki_region_alloc(unsigned long size, struct fki_region *region)
{
	unsigned long nr_pages = PAGE_ALIGN(size) >> PAGE_SHIFT;
	struct page *pg;
	void *va;
	int err;

	if (!region || !nr_pages) {
		return -EINVAL;
	}
	va = call_vzalloc(nr_pages << PAGE_SHIFT);
	if (!va) {
		return -ENOMEM;
	}
	pg = call_vmalloc_to_page(va);
	if (!pg) {
		call_vfree(va);
		return -EFAULT;
	}
	err = call_set_memory_x((unsigned long)va, (int)nr_pages);
	if (err) {
		call_vfree(va);
		return err;
	}
	region->va = va;
	region->pa = page_to_phys(pg);
	region->size = nr_pages << PAGE_SHIFT;
	region->handle = NULL;
	return 0;
}
EXPORT_SYMBOL(fki_region_alloc);

void fki_region_free(struct fki_region *region)
{
	if (!region || !region->va) {
		return;
	}
	call_set_memory_nx((unsigned long)region->va,
			   (int)(region->size >> PAGE_SHIFT));
	call_vfree(region->va);
	region->va = NULL;
}
EXPORT_SYMBOL(fki_region_free);

static int fki_carve_check(unsigned long base, unsigned long len)
{
	unsigned long end = base + len;
	unsigned long stext = fki_sym_addr(FKI_SYM_STEXT);
	unsigned long kend = fki_sym_addr(FKI_SYM_END);
	u64 *kimg_voff = (u64 *)fki_sym_addr(FKI_SYM_KIMAGE_VOFFSET);
	unsigned long addr;

	if (end < base) {
		pr_err("[fki] carve [%#lx, %#lx) overflows physical space\n",
		       base, end);
		return -EINVAL;
	}

	if (call_efi_mem_type(base) >= 0) {
		for (addr = base; addr < end; addr += SZ_2M) {
			int mem_type = call_efi_mem_type(addr);

			if (mem_type != EFI_LOADER_CODE &&
			    mem_type != EFI_LOADER_DATA &&
			    mem_type != EFI_BOOT_SERVICES_CODE &&
			    mem_type != EFI_BOOT_SERVICES_DATA &&
			    mem_type != EFI_CONVENTIONAL_MEMORY) {
				pr_err("[fki] carve [%#lx, %#lx) not RAM (EFI %d at %#lx)\n",
				       base, end, mem_type, addr);
				return -EINVAL;
			}
		}
	}

	if (stext && kend && kimg_voff) {
		unsigned long ks = stext - *kimg_voff;
		unsigned long ke = kend - *kimg_voff;

		if (base < ke && ks < end) {
			pr_err("[fki] carve [%#lx, %#lx) overlaps kernel [%#lx, %#lx)\n",
			       base, end, ks, ke);
			return -EBUSY;
		}
	}

	if (call_region_intersects(base, len, IORESOURCE_SYSTEM_RAM,
				   IORES_DESC_NONE) != REGION_DISJOINT) {
		pr_err("[fki] carve [%#lx, %#lx) overlaps IORESOURCE_SYSTEM_RAM\n",
		       base, end);
		return -EBUSY;
	}
	if (call_region_intersects(base, len, IORESOURCE_MEM,
				   IORES_DESC_NONE) != REGION_DISJOINT) {
		pr_err("[fki] carve [%#lx, %#lx) overlaps IORESOURCE_MEM\n",
		       base, end);
		return -EBUSY;
	}
	return 0;
}

int fki_contig_alloc(unsigned long size, struct fki_region *region)
{
	unsigned long nr_pages = 1;
	struct page *pg;
	int err;

	if (!region || !size) {
		return -EINVAL;
	}

	if (carve_mb && !g_carve_used &&
	    ALIGN(size, SZ_2M) <= (carve_mb << 20) &&
	    !(carve_base & (SZ_2M - 1))) {
		unsigned long clen = ALIGN(size, SZ_2M);
		void *va;

		err = fki_carve_check(carve_base, clen);
		if (err) {
			return err;
		}
		va = call_memremap(carve_base, clen, MEMREMAP_WB);
		if (va) {
			g_carve_used = true;
			region->va = va;
			region->size = clen;
			memset(region->va, 0, region->size);
			region->pa = carve_base;
			region->handle = (void *)4UL;
			return 0;
		}
		pr_warn("[fki] carve at %#lx not mappable\n", carve_base);
	}

	if (g_resv_pg && !g_resv_used &&
	    (ALIGN(size, SZ_2M) >> PAGE_SHIFT) <= g_resv_cnt) {
		g_resv_used = true;
		region->va = page_address(g_resv_pg);
		region->size = ALIGN(size, SZ_2M);
		memset(region->va, 0, region->size);
		region->pa = page_to_phys(g_resv_pg);
		region->handle = (void *)((unsigned long)g_resv_pg | 2UL);
		return 0;
	}

	if (size >= (64UL << 20)) {
		unsigned long cnt = ALIGN(size, SZ_2M) >> PAGE_SHIFT;

		pg = call_cma_alloc(cnt);
		if (pg) {
			region->va = page_address(pg);
			region->size = cnt << PAGE_SHIFT;
			memset(region->va, 0, region->size);
			region->pa = page_to_phys(pg);
			region->handle = (void *)((unsigned long)pg | 1UL);
			return 0;
		}
	}

	if (!fki_sym_addr(FKI_SYM_ALLOC_CONTIG)) {
		return -ENOENT;
	}
	while ((nr_pages << PAGE_SHIFT) < size) {
		nr_pages <<= 1;
	}
	pg = call_alloc_contig(nr_pages, GFP_KERNEL | __GFP_NOWARN,
			       numa_node_id());
	if (!pg) {
		return -ENOMEM;
	}
	region->va = page_address(pg);
	region->size = nr_pages << PAGE_SHIFT;
	memset(region->va, 0, region->size);
	region->pa = page_to_phys(pg);
	region->handle = pg;
	return 0;
}
EXPORT_SYMBOL(fki_contig_alloc);

void fki_contig_free(struct fki_region *region)
{
	if (!region || !region->handle) {
		return;
	}
	if ((unsigned long)region->handle == 4UL) {
		call_memunmap(region->va);
		g_carve_used = false;
		return;
	}
	if ((unsigned long)region->handle & 2UL) {
		g_resv_used = false;
		return;
	}
	if ((unsigned long)region->handle & 1UL) {
		struct page *pg = (struct page *)((unsigned long)region->handle & ~1UL);

		call_cma_release(pg, region->size >> PAGE_SHIFT);
		return;
	}
	call_free_contig(page_to_pfn((struct page *)region->handle),
			 region->size >> PAGE_SHIFT);
}
EXPORT_SYMBOL(fki_contig_free);

static __nocfi noinline void call_set_swapper_pgd(pgd_t *pgdp, u64 desc)
{
	set_swapper_pgd_fn fn = (set_swapper_pgd_fn)fki_sym_addr(FKI_SYM_SET_SWAPPER_PGD);

	if (!fn) {
		return;
	}
	fn(pgdp, __pgd(desc));
}

static bool swapper_slot_ptr(unsigned long long *slot, pgd_t **out)
{
	u64 *kimg_voff = (u64 *)fki_sym_addr(FKI_SYM_KIMAGE_VOFFSET);
	unsigned long swapper = fki_sym_addr(FKI_SYM_SWAPPER_PG_DIR);
	unsigned long swapper_pa;
	unsigned long slot_pa;

	if (!swapper || !kimg_voff) {
		return false;
	}
	swapper_pa = swapper - *kimg_voff;
	slot_pa = __pa(slot);
	if (slot_pa < swapper_pa || slot_pa >= swapper_pa + PAGE_SIZE) {
		return false;
	}
	*out = (pgd_t *)(swapper + (slot_pa - swapper_pa));
	return true;
}

int fki_window_l1(unsigned long va, unsigned long long **l1_va, fki_u64 *l1_pa)
{
	u64 tcr = read_sysreg(tcr_el1);
	u64 ttbr1 = read_sysreg(ttbr1_el1) & GENMASK_ULL(47, 12);
	u64 va_bits = 64 - ((tcr >> 16) & 0x3f);
	u64 *l0;
	u64 entry;

	if (!l1_va || !l1_pa) {
		return -EINVAL;
	}
	/* TG1 == 2 selects the 4 KiB granule */
	if (((tcr >> 30) & 3) != 2 || va_bits < 31 || va_bits > 48) {
		return -ENOTSUPP;
	}
	if ((u64)va < (-(1ULL << va_bits))) {
		return -EINVAL;
	}
	if (va_bits <= 39) {
		*l1_pa = ttbr1;
		*l1_va = phys_to_virt(ttbr1);
		return 0;
	}

	l0 = phys_to_virt(ttbr1);
	entry = READ_ONCE(l0[(va >> 39) & 511]);
	if ((entry & 3) != 3) {
		return -ENOENT;
	}
	*l1_pa = entry & GENMASK_ULL(47, 12);
	*l1_va = phys_to_virt(*l1_pa);
	return 0;
}
EXPORT_SYMBOL(fki_window_l1);

void fki_window_set(unsigned long long *slot, fki_u64 desc)
{
	pgd_t *pgdp;
	u64 val = desc;

	if (!slot) {
		return;
	}
	if (hk_va_writable((unsigned long)slot)) {
		WRITE_ONCE(*slot, val);
	} else if (fki_sym_addr(FKI_SYM_SET_SWAPPER_PGD) &&
		   swapper_slot_ptr(slot, &pgdp)) {
		call_set_swapper_pgd(pgdp, val);
	} else if (!irqs_disabled() && !in_atomic()) {
		hk_write_fixmap(slot, &val, sizeof(val));
	} else {
		pr_warn("[fki] window slot %px read-only in atomic context\n",
			slot);
		return;
	}
	dsb(ishst);
	__tlbi(vmalle1is);
	dsb(ish);
	isb();
}
EXPORT_SYMBOL(fki_window_set);

fki_u64 fki_phys_peek(fki_u64 pa)
{
	if (!pfn_valid(PHYS_PFN(pa))) {
		return ~0ULL;
	}
	return READ_ONCE(*(u64 *)phys_to_virt(pa & ~7ULL));
}
EXPORT_SYMBOL(fki_phys_peek);

int fki_mem_init(void)
{
	unsigned long cnt;

	if (carve_mb) {
		pr_info("[fki] carve %lu MiB at %#lx\n", carve_mb, carve_base);
	}
	if (!reserve_mb) {
		return 0;
	}
	cnt = ALIGN(reserve_mb << 20, SZ_2M) >> PAGE_SHIFT;
	g_resv_pg = call_cma_alloc(cnt);
	if (g_resv_pg) {
		g_resv_cnt = cnt;
	}
	pr_info("[fki] reserve %lu MiB %s\n", reserve_mb,
		g_resv_pg ? "ok" : "failed");
	return 0;
}

void fki_mem_exit(void)
{
	if (g_resv_pg && !g_resv_used) {
		call_cma_release(g_resv_pg, g_resv_cnt);
		g_resv_pg = NULL;
		g_resv_cnt = 0;
	}
}
