// SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#ifndef FKI_H
#define FKI_H

#define FKI_VERSION 0

#ifndef __KERNEL__
#include <stdint.h>
typedef uint64_t fki_u64;
#else
#include <linux/types.h>
typedef u64 fki_u64;
#endif

struct fki_region {
	void *va;
	fki_u64 pa;
	fki_u64 size;
	void *handle;
};

struct fki_pac_keys {
	fki_u64 ia[2];
	fki_u64 ib[2];
	fki_u64 da[2];
	fki_u64 db[2];
	fki_u64 ga[2];
	fki_u64 sctlr_en;
};

struct fki_inline_regs {
	unsigned long x[8];
	unsigned long x8;
	unsigned long lr;
	unsigned long sp;
};

#ifdef __cplusplus
extern "C" {
#endif

int fki_version(void);
void fki_log(const char *s, unsigned long len);
void fki_log_alert(const char *s, unsigned long len);

int fki_region_alloc(unsigned long size, struct fki_region *r);
void fki_region_free(struct fki_region *r);
int fki_contig_alloc(unsigned long size, struct fki_region *r);
void fki_contig_free(struct fki_region *r);

int fki_blob_get(const char *name, const void **data, unsigned long *size,
		 void **handle);
void fki_blob_put(void *handle);

unsigned long fki_enter(const struct fki_pac_keys *foreign,
			struct fki_pac_keys *saved);
void fki_leave(unsigned long cookie, const struct fki_pac_keys *saved);
void fki_pac_swap(const struct fki_pac_keys *load, struct fki_pac_keys *save);

fki_u64 fki_counter_frequency(void);
fki_u64 fki_counter(void);
void fki_random(void *buf, unsigned long len);
void fki_realtime(fki_u64 *sec, fki_u64 *nsec);

int fki_window_l1(unsigned long va, unsigned long long **l1_va,
		  fki_u64 *l1_pa);
void fki_window_set(unsigned long long *slot, fki_u64 desc);
fki_u64 fki_phys_peek(fki_u64 pa);

void fki_idle_until(fki_u64 counter_deadline, fki_u64 max_ns,
		    const volatile unsigned int *wake);
void fki_yield(void);

long fki_net_send(const void *frame, unsigned long len);
long fki_net_recv(void *buf, unsigned long len);
long fki_net_pending(void);

int fki_bind_cpu(int cpu);
int fki_num_cpus(void);
int fki_cpu_mpidr(int cpu, fki_u64 *mpidr);
void fki_kick_cpu(int cpu);

void *fki_thread_start(int (*fn)(void *), void *arg, int cpu, const char *name);
void fki_thread_stop(void *thread);
int fki_thread_should_stop(void);
void fki_thread_wait(const volatile unsigned long *flag);
void fki_thread_wake(void *thread);
void fki_msleep(unsigned int ms);

unsigned long fki_regs_pc(void *regs);
unsigned long fki_regs_lr(void *regs);
unsigned long fki_regs_pstate(void *regs);
void fki_regs_set_pc(void *regs, unsigned long pc);
void fki_regs_set_reg(void *regs, unsigned int n, unsigned long v);
unsigned long fki_regs_reg(void *regs, unsigned int n);
int fki_fault_register(unsigned long lo, unsigned long hi,
		       int (*handler)(unsigned long far, unsigned long esr,
				      void *regs));
void fki_fault_unregister(void);

void *fki_file_open(const char *path, int writable, fki_u64 *size);
long fki_file_rw(void *file, int write, void *buf, unsigned long len,
		 fki_u64 off);
void fki_file_close(void *file);
long fki_file_pwrite(void *file, const void *buf, unsigned long len,
		     fki_u64 off);

long fki_hostcall(const void *req, unsigned long len, void *reply,
		  unsigned long max);

unsigned long fki_kr_resolve(const char *name);
int fki_kr_addr_name(unsigned long addr, char *buf, int max);
unsigned long fki_kr_base(void);
unsigned int fki_kr_num_syms(void);

int fki_ti_btf_available(void);
int fki_ti_type_by_name(const char *name, unsigned int kind,
			unsigned int *type_id);
unsigned int fki_ti_type_size(unsigned int type_id);
int fki_ti_member_off(unsigned int type_id, const char *member,
		      unsigned int *bit_off, unsigned int *bit_sz);
int fki_ti_struct_off(const char *type, const char *member,
		      unsigned int *byte_off);

void *fki_hk_kprobe_add(const char *sym, int (*pre)(void *regs, void *arg),
			void *arg, int skip_pad);
void *fki_hk_kprobe_add_at(unsigned long addr,
			   int (*pre)(void *regs, void *arg),
			   void *arg, int skip_pad);
void fki_hk_kprobe_del(void *handle);
int fki_hk_inline_hook(const char *sym, const char *wrapper_sym,
		       void **handle, unsigned long *orig_out);
int fki_hk_inline_hook_decide(const char *sym,
			      int (*decide)(struct fki_inline_regs *regs,
					    void *arg),
			      void *arg, void **handle,
			      unsigned long *orig_out);
void fki_hk_inline_unhook(void *handle);
int fki_hk_write_text(void *dst, const void *src, unsigned long len);
unsigned long fki_hk_resolve(const char *name);

#ifdef __cplusplus
}
#endif

#endif
