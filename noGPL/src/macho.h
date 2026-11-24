// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#ifndef XNU_MACHO_H
#define XNU_MACHO_H

#include "fki.h"

typedef unsigned int u32_t;
typedef unsigned long long u64_t;

#define MH_MAGIC_64 0xfeedfacf
#define LC_SEGMENT_64 0x19
#define LC_SYMTAB 0x2
#define LC_UNIXTHREAD 0x5
#define LC_DYLD_CHAINED_FIXUPS 0x80000034
#define LC_FILESET_ENTRY 0x80000035
#define MH_FILESET 12

struct mh64 {
	u32_t magic;
	u32_t cputype;
	u32_t cpusubtype;
	u32_t filetype;
	u32_t ncmds;
	u32_t sizeofcmds;
	u32_t flags;
	u32_t reserved;
};

struct lc {
	u32_t cmd;
	u32_t cmdsize;
};

struct seg64 {
	u32_t cmd;
	u32_t cmdsize;
	char segname[16];
	u64_t vmaddr;
	u64_t vmsize;
	u64_t fileoff;
	u64_t filesize;
	u32_t maxprot;
	u32_t initprot;
	u32_t nsects;
	u32_t flags;
};

struct sect64 {
	char sectname[16];
	char segname[16];
	u64_t addr;
	u64_t size;
	u32_t offset;
	u32_t align;
	u32_t reloff;
	u32_t nreloc;
	u32_t flags;
	u32_t r1;
	u32_t r2;
	u32_t r3;
};

struct symtab_lc {
	u32_t cmd;
	u32_t cmdsize;
	u32_t symoff;
	u32_t nsyms;
	u32_t stroff;
	u32_t strsize;
};

struct nlist64 {
	u32_t n_strx;
	unsigned char n_type;
	unsigned char n_sect;
	unsigned short n_desc;
	u64_t n_value;
};

struct xnu_image {
	const unsigned char *file;
	unsigned long file_size;
	u64_t vmlow;
	u64_t vmhigh;
	struct fki_region region;
	u64_t slide;
	const struct symtab_lc *symtab;
	u64_t thread_starts;
	u64_t thread_starts_size;
	u32_t chained_off;
	u32_t chained_size;
	u64_t entry;
	u32_t filetype;
	u64_t kernel_mh;
};

struct fileset_entry_lc {
	u32_t cmd;
	u32_t cmdsize;
	u64_t vmaddr;
	u64_t fileoff;
	u32_t entry_id;
	u32_t reserved;
};

struct linkedit_data_lc {
	u32_t cmd;
	u32_t cmdsize;
	u32_t dataoff;
	u32_t datasize;
};

struct chained_fixups_hdr {
	u32_t fixups_version;
	u32_t starts_offset;
	u32_t imports_offset;
	u32_t symbols_offset;
	u32_t imports_count;
	u32_t imports_format;
	u32_t symbols_format;
};

struct chained_starts_in_image {
	u32_t seg_count;
	u32_t seg_info_offset[];
};

struct chained_starts_in_segment {
	u32_t size;
	unsigned short page_size;
	unsigned short pointer_format;
	u64_t segment_offset;
	u32_t max_valid_pointer;
	unsigned short page_count;
	unsigned short page_start[];
};

int macho_load(struct xnu_image *img, const void *file, unsigned long size,
	       unsigned long extra);
int macho_copy(struct xnu_image *img, void *dst);
int macho_rebase(struct xnu_image *img);
int macho_slide_header(struct xnu_image *img, u64_t *mh_unslid);
u64_t macho_sym(struct xnu_image *img, const char *name);

#endif
