// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#include "xrt.h"
#define ENOEXEC 8
#define ENOMEM 12
#include "macho.h"

#define FOR_EACH_LC(img, pos, end) \
	for ((pos) = (const void *)((img)->file + sizeof(struct mh64)), \
	     (end) = (const void *)((img)->file + sizeof(struct mh64) + \
				    ((const struct mh64 *)(img)->file)->sizeofcmds); \
	     (pos) < (end); \
	     (pos) = (const void *)((const char *)(pos) + (pos)->cmdsize))

int macho_load(struct xnu_image *img, const void *file, unsigned long size,
	       unsigned long extra)
{
	const struct mh64 *mh = file;
	const struct lc *lc_it;
	const struct lc *lc_end;
	int err;

	img->file = file;
	img->file_size = size;
	img->chained_off = 0;
	img->chained_size = 0;
	if (size < sizeof(*mh) || mh->magic != MH_MAGIC_64) {
		return -ENOEXEC;
	}
	img->vmlow = ~0ULL;
	img->vmhigh = 0;

	FOR_EACH_LC(img, lc_it, lc_end) {
		if (lc_it->cmd == LC_SEGMENT_64) {
			const struct seg64 *seg = (const void *)lc_it;
			const struct sect64 *sc;
			u32_t i;

			if (!seg->vmsize ||
			    !x_strncmp(seg->segname, "__PAGEZERO", 16)) {
				continue;
			}
			if (seg->vmaddr < img->vmlow) {
				img->vmlow = seg->vmaddr;
			}
			if (seg->vmaddr + seg->vmsize > img->vmhigh) {
				img->vmhigh = seg->vmaddr + seg->vmsize;
			}
			sc = (const void *)(seg + 1);
			for (i = 0; i < seg->nsects; i++) {
				if (!x_strncmp(sc[i].sectname, "__thread_starts", 16)) {
					img->thread_starts = sc[i].addr;
					img->thread_starts_size = sc[i].size;
				}
			}
		} else if (lc_it->cmd == LC_SYMTAB) {
			img->symtab = (const void *)lc_it;
		} else if (lc_it->cmd == LC_DYLD_CHAINED_FIXUPS) {
			const struct linkedit_data_lc *ld = (const void *)lc_it;

			img->chained_off = ld->dataoff;
			img->chained_size = ld->datasize;
		} else if (lc_it->cmd == LC_UNIXTHREAD) {
			const u32_t *state = (const void *)(lc_it + 1);

			img->entry = *(const u64_t *)(state + 2 + 2 * 32);
		}
	}

	if (img->vmhigh <= img->vmlow) {
		return -ENOEXEC;
	}
	img->filetype = mh->filetype;

	if (mh->filetype == MH_FILESET) {
		img->symtab = 0;
		FOR_EACH_LC(img, lc_it, lc_end) {
			const struct fileset_entry_lc *fe;
			const struct mh64 *kh;
			const struct lc *kl;
			u32_t i;

			if (lc_it->cmd != LC_FILESET_ENTRY) {
				continue;
			}
			fe = (const void *)lc_it;
			if (fe->entry_id >= fe->cmdsize ||
			    x_strcmp((const char *)fe + fe->entry_id,
				     "com.apple.kernel")) {
				continue;
			}
			if (fe->fileoff + sizeof(struct mh64) > size) {
				return -ENOEXEC;
			}
			img->kernel_mh = fe->vmaddr;
			kh = (const void *)((const char *)file + fe->fileoff);
			kl = (const void *)(kh + 1);
			for (i = 0; i < kh->ncmds; i++,
			     kl = (const void *)((const char *)kl + kl->cmdsize)) {
				if (kl->cmd == LC_SYMTAB) {
					img->symtab = (const void *)kl;
				}
			}
		}
		if (!img->kernel_mh || !img->symtab) {
			return -ENOEXEC;
		}
	}

	if (extra == ~0UL) {
		return 0;
	}
	err = fki_region_alloc(img->vmhigh - img->vmlow + extra, &img->region);
	if (err) {
		return err;
	}
	img->slide = (u64_t)img->region.va - img->vmlow;
	return macho_copy(img, img->region.va);
}

int macho_copy(struct xnu_image *img, void *dst)
{
	const struct lc *lc_it;
	const struct lc *lc_end;

	FOR_EACH_LC(img, lc_it, lc_end) {
		const struct seg64 *seg;

		if (lc_it->cmd != LC_SEGMENT_64) {
			continue;
		}
		seg = (const void *)lc_it;
		if (!seg->vmsize || !seg->filesize ||
		    !x_strncmp(seg->segname, "__PAGEZERO", 16)) {
			continue;
		}
		if (seg->fileoff + seg->filesize > img->file_size) {
			return -ENOEXEC;
		}
		x_memcpy((char *)dst + (seg->vmaddr - img->vmlow),
			 img->file + seg->fileoff, seg->filesize);
	}
	return 0;
}

static inline u64_t pac_sign(u64_t v, unsigned key, u64_t disc)
{
	register u64_t r0 asm("x16") = v;
	register u64_t r1 asm("x17") = disc;

	switch (key) {
	case 0:
		asm volatile(".inst 0xdac10230" : "+r"(r0) : "r"(r1));
		break;
	case 1:
		asm volatile(".inst 0xdac10630" : "+r"(r0) : "r"(r1));
		break;
	case 2:
		asm volatile(".inst 0xdac10a30" : "+r"(r0) : "r"(r1));
		break;
	default:
		asm volatile(".inst 0xdac10e30" : "+r"(r0) : "r"(r1));
		break;
	}
	return r0;
}

static int macho_rebase_chained(struct xnu_image *img)
{
	const struct chained_fixups_hdr *hdr;
	const struct chained_starts_in_image *img_starts;
	u64_t cnt = 0;
	u32_t seg_idx;

	if (!img->chained_off ||
	    (unsigned long)img->chained_off + img->chained_size > img->file_size ||
	    img->chained_size < sizeof(*hdr)) {
		return -ENOEXEC;
	}
	hdr = (const void *)(img->file + img->chained_off);
	if (hdr->starts_offset >= img->chained_size) {
		return -ENOEXEC;
	}
	img_starts = (const void *)((const char *)hdr + hdr->starts_offset);

	for (seg_idx = 0; seg_idx < img_starts->seg_count; seg_idx++) {
		const struct chained_starts_in_segment *seg;
		u64_t seg_base;
		u32_t page_idx;

		if (!img_starts->seg_info_offset[seg_idx]) {
			continue;
		}
		seg = (const void *)((const char *)img_starts +
				     img_starts->seg_info_offset[seg_idx]);
		if (seg->pointer_format != 8 && seg->pointer_format != 9) {
			return -ENOEXEC;
		}
		seg_base = img->vmlow + img->slide + seg->segment_offset;
		for (page_idx = 0; page_idx < seg->page_count; page_idx++) {
			u64_t addr;
			u64_t delta;

			if (seg->page_start[page_idx] == 0xffff) {
				continue;
			}
			addr = seg_base + (u64_t)page_idx * seg->page_size +
			       seg->page_start[page_idx];
			do {
				u64_t val = *(u64_t *)addr;
				u64_t nv;

				delta = (val >> 51) & 0xfff;
				nv = img->vmlow + img->slide +
				     (val & 0x3fffffffULL);
				if (seg->pointer_format == 9 &&
				    (val & (1ULL << 63))) {
					u64_t disc = (val >> 32) & 0xffff;

					if (val & (1ULL << 48)) {
						disc = disc ?
						       ((addr & 0x0000ffffffffffffULL) |
							(disc << 48)) :
						       addr;
					}
					*(u64_t *)addr = pac_sign(nv,
								  (val >> 49) & 3,
								  disc);
				} else {
					u64_t hi = (val >> 30) & 0xff;

					*(u64_t *)addr = nv | (hi << 56);
				}
				cnt++;
				addr += delta * 4;
			} while (delta);
		}
	}
	return (int)cnt;
}

int macho_rebase(struct xnu_image *img)
{
	u64_t mh_vm = 0;
	const struct lc *lc_it;
	const struct lc *lc_end;
	u32_t *ts;
	u32_t *te;
	u32_t *off_ptr;
	u64_t step;
	u64_t cnt = 0;

	if (img->chained_off) {
		return macho_rebase_chained(img);
	}

	FOR_EACH_LC(img, lc_it, lc_end) {
		const struct seg64 *seg = (const void *)lc_it;

		if (lc_it->cmd == LC_SEGMENT_64 &&
		    !x_strncmp(seg->segname, "__TEXT", 16) &&
		    seg->fileoff == 0) {
			mh_vm = seg->vmaddr;
		}
	}
	if (!img->thread_starts || !mh_vm) {
		return -ENOEXEC;
	}
	ts = (u32_t *)(img->thread_starts + img->slide);
	te = (u32_t *)((char *)ts + img->thread_starts_size);
	step = (ts[0] & 1) ? 8 : 4;

	for (off_ptr = ts + 1; off_ptr < te && *off_ptr != 0xffffffff; off_ptr++) {
		u64_t addr = mh_vm + img->slide + *off_ptr;
		u64_t delta;

		do {
			u64_t val = *(u64_t *)addr;

			if (!(val & (1ULL << 62))) {
				if (val & (1ULL << 63)) {
					u64_t nv = (val & 0xffffffffULL) + mh_vm +
						   img->slide;
					u64_t disc = (val >> 32) & 0xffff;

					if (val & (1ULL << 48)) {
						disc = disc ?
						       ((addr & 0x0000ffffffffffffULL) |
							(disc << 48)) :
						       addr;
					}
					*(u64_t *)addr = pac_sign(nv,
								  (val >> 49) & 3,
								  disc);
				} else {
					u64_t top8 = val & 0x0007F80000000000ULL;
					u64_t b43 = val & 0x000007FFFFFFFFFFULL;
					u64_t target = (top8 << 13) |
						       (((long long)(b43 << 21) >> 21) &
							0x00FFFFFFFFFFFFFFULL);

					*(u64_t *)addr = target + img->slide;
				}
				cnt++;
			}
			delta = ((val & ~(1ULL << 62)) & 0x3FF8000000000000ULL) >> 51;
			addr += delta * step;
		} while (delta);
	}
	return (int)cnt;
}

u64_t macho_sym(struct xnu_image *img, const char *name)
{
	const struct symtab_lc *st = img->symtab;
	const struct nlist64 *nl;
	const char *strs;
	u32_t i;

	if (!st) {
		return 0;
	}
	nl = (const void *)(img->file + st->symoff);
	strs = (const char *)img->file + st->stroff;
	for (i = 0; i < st->nsyms; i++) {
		if ((nl[i].n_type & 0x0e) != 0x0e ||
		    nl[i].n_strx >= st->strsize) {
			continue;
		}
		if (!x_strcmp(strs + nl[i].n_strx, name)) {
			return nl[i].n_value + img->slide;
		}
	}
	return 0;
}

int macho_slide_header(struct xnu_image *img, u64_t *mh_unslid)
{
	u64_t mh_vm = 0;
	const struct lc *lc_it;
	const struct lc *lc_end_file;
	unsigned char *mh;
	const struct mh64 *hdr;
	struct lc *lc_cur;
	struct lc *lc_end;
	int cnt = 0;

	FOR_EACH_LC(img, lc_it, lc_end_file) {
		const struct seg64 *seg = (const void *)lc_it;

		if (lc_it->cmd == LC_SEGMENT_64 &&
		    !x_strncmp(seg->segname, "__TEXT", 16) &&
		    seg->fileoff == 0) {
			mh_vm = seg->vmaddr;
		}
	}
	if (!mh_vm) {
		return -ENOEXEC;
	}
	*mh_unslid = mh_vm;
	mh = (unsigned char *)(mh_vm + img->slide);
	hdr = (const void *)mh;
	lc_cur = (void *)(mh + sizeof(struct mh64));
	lc_end = (void *)(mh + sizeof(struct mh64) + hdr->sizeofcmds);

	for (; lc_cur < lc_end;
	     lc_cur = (void *)((char *)lc_cur + lc_cur->cmdsize)) {
		struct fileset_entry_lc *fe;
		struct seg64 *sg;
		struct sect64 *sc;
		u32_t i;

		if (lc_cur->cmd == LC_FILESET_ENTRY) {
			fe = (void *)lc_cur;
			fe->vmaddr += img->slide;
			continue;
		}
		if (lc_cur->cmd != LC_SEGMENT_64) {
			continue;
		}
		sg = (void *)lc_cur;
		if (!sg->vmsize) {
			continue;
		}
		sg->vmaddr += img->slide;
		sc = (void *)(sg + 1);
		for (i = 0; i < sg->nsects; i++) {
			sc[i].addr += img->slide;
		}
		cnt++;
	}
	return cnt;
}
