// SPDX-License-Identifier: APSL-2.0
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef XNU_MAIN_H
#define XNU_MAIN_H

#include "fki.h"
#include "linux64_abi.h"
#include "macho.h"
#include "world.h"
#include "xrt.h"

#define XNU_MAX_CPUS LINUX64_MAX_CPUS
#define XNU_L1_FIRST 384
#define XNU_L1_LAST 495
#define XNU_L1_SCAN_LO 258
#define XNU_L1_SCAN_HI 505
#define XNU_VA_BASE 0xffffff8000000000ULL
#define XNU_VA_LO 0xffffffe000000000ULL
#define XNU_VA_HI 0xfffffffc00000000ULL
#define XNU_ACCT_N 64

#define XNU_TC_SEG_SIZE (g_xnu.tc_size ? g_xnu.tc_size + 8 : 0)
#define XNU_FB_SIZE \
	(g_xnu.fb_width > 0 && g_xnu.fb_height > 0 ? \
	 (u64_t)g_xnu.fb_width * g_xnu.fb_height * 4 : 0)
#define XNU_BOOT_EXTRA \
	(((g_xnu.rd_size + 0x3fff) & ~0x3fffULL) + \
	 ((XNU_TC_SEG_SIZE + 0x3fff) & ~0x3fffULL) + \
	 ((XNU_FB_SIZE + 0x3fff) & ~0x3fffULL))

struct boot_args_v2 {
	unsigned short revision;
	unsigned short version;
	u64_t virt_base;
	u64_t phys_base;
	u64_t mem_size;
	u64_t top_of_kernel_data;
	struct {
		u64_t v_base_addr;
		u64_t v_display;
		u64_t v_row_bytes;
		u64_t v_width;
		u64_t v_height;
		u64_t v_depth;
	} video;
	unsigned int machine_type;
	void *device_tree_p;
	unsigned int device_tree_length;
	char command_line[1024];
	u64_t boot_flags;
	u64_t mem_size_actual;
};

struct xnu_ctx {
	char *image;
	int mode;
	int ram_mb;
	int kmax;
	int idle_max;
	int quiet_console;
	int cpus;
	int stop_run;
	int assert_fatal;
	char *bootargs;
	char *ramdisk;
	char *trustcache;
	int fb_width;
	int fb_height;
	char *fb_dump;
	int fb_dump_ms;
	char *hostdisk;
	int hostdisk_rw;
	char *console_in;
	char *input_in;
	char *keystore;
	char *disk;
	char *boot_uuid;
	char *preboot_uuid;
	char *root_uuid;
	char *model;
	char *soc_name;
	char *gpu_model;
	unsigned long ecpus;
	int vm16k;
	int park_secondaries;
	int perf;
	int monitor_cpu;
	int dump_threads;
	int auto_slide;

	u64_t va_lo;
	u64_t va_hi;
	unsigned l1_first;
	unsigned l1_last;

	struct xnu_image img;
	struct fki_region l2page;
	struct fki_chunk *m4_chunks;
	struct fki_region *m4_l3pages;
	struct linux64_h2g_slot *m4_h2g;
	unsigned int *m4_g2h;
	unsigned int m4_n_chunks;
	unsigned int m4_n_l3;
	unsigned int m4_hash_mask;
	u64_t m4_gpa_base;
	u64_t m4_map_reserve;
	unsigned long long *window_slot;
	unsigned long long *window_l1;
	unsigned char window_owned[512];
	u64_t diag_va[6];
	void *blob;
	int kcount;

	const void *rd_data;
	unsigned long rd_size;
	void *rd_blob;
	const void *tc_data;
	unsigned long tc_size;
	void *tc_blob;
	u64_t fb_off;

	void *hd_file;
	fki_u64 hd_size;
	void *ci_file;
	fki_u64 ci_off;
	void *in_file;
	fki_u64 in_off;
	void *ks_file;
	void *dk_file;
	fki_u64 dk_size;

	signed char linux_cpu_of_slot[LINUX64_CPU_SLOTS];
	unsigned long long cpu_mpidr[XNU_MAX_CPUS];
	void *cpu_thread[XNU_MAX_CPUS];
	volatile int cpu_running[XNU_MAX_CPUS];
	unsigned long cpu_start_arg[XNU_MAX_CPUS];
	u64_t cpu_entry;

	volatile int in_xnu_slot[LINUX64_CPU_SLOTS];
	unsigned long kfaults;
	unsigned long kfault_moved;

	unsigned long acct_ticks[LINUX64_CPU_SLOTS][XNU_ACCT_N];
	unsigned long acct_calls[LINUX64_CPU_SLOTS][XNU_ACCT_N];
	unsigned long acct_disk[LINUX64_CPU_SLOTS];
	volatile const unsigned long *xnu_el0_stat;
	unsigned long upcalls_slot[LINUX64_CPU_SLOTS];
	unsigned long last_call_slot[LINUX64_CPU_SLOTS];
	volatile int in_call_slot[LINUX64_CPU_SLOTS];
	volatile const unsigned long *xnu_dbg;
};

extern struct xnu_ctx g_xnu;
extern struct xnu_world g_worlds[XNU_MAX_CPUS];

static inline unsigned xnu_this_slot(void)
{
	unsigned long m;

	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(m));
	return LINUX64_CPU_SLOT(m);
}

static inline void xnu_logs(const char *s)
{
	fki_log(s, x_strlen(s));
}

static inline void xnu_logs_alert(const char *s)
{
	fki_log_alert(s, x_strlen(s));
}

void xnu_logkv(const char *k, u64_t v);
int xnu_enter_arm_init(struct boot_args_v2 *ba);
void xnu_stop_commit(void);
int xnu_stop_park(void);

#endif
