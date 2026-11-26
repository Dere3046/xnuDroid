// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/hrtimer.h>
#include <linux/irqflags.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/smp.h>
#include <linux/timekeeping.h>
#include <linux/version.h>
#include <asm/arch_timer.h>
#include <asm/barrier.h>
#include <asm/cpufeature.h>
#include <asm/cputype.h>
#include <asm/neon.h>
#include <asm/sysreg.h>

#include "fki.h"
#include "fki_cpu.h"
#include "sym_reg.h"

#define FKI_SCTLR_EN (SCTLR_ELx_ENIA | SCTLR_ELx_ENIB | SCTLR_ELx_ENDA | SCTLR_ELx_ENDB)
#define FKI_ALT_STACK_ORDER get_order(2 * THREAD_SIZE)

struct fki_runs {
	void (*fn)(void *);
	void *arg;
};

struct fki_idle_args {
	fki_u64 ns;
	const volatile unsigned int *wake;
};

typedef int (*sched_hrtimeout_fn)(ktime_t *expires, u64 delta_ns,
				  const enum hrtimer_mode mode);
typedef void (*schedule_fn)(void);
typedef signed long (*schedule_timeout_fn)(signed long timeout);
typedef void (*msleep_fn)(unsigned int msecs);
typedef int (*set_cpus_allowed_fn)(struct task_struct *p,
				   const struct cpumask *new_mask);
typedef struct task_struct *(*kthread_create_node_fn)(int (*threadfn)(void *),
						      void *data, int node,
						      const char *namefmt, ...);
typedef void (*kthread_bind_fn)(struct task_struct *k, unsigned int cpu);
typedef int (*kthread_stop_fn)(struct task_struct *k);
typedef bool (*kthread_should_stop_fn)(void);
typedef int (*wake_up_process_fn)(struct task_struct *p);
typedef void (*put_task_struct_fn)(struct task_struct *t);
typedef int (*smp_call_single_fn)(int cpu, smp_call_func_t func, void *info,
				  int wait);
typedef int (*smp_call_async_fn)(int cpu, call_single_data_t *csd);
typedef void (*ktime_get_real_ts64_fn)(struct timespec64 *ts);
typedef void (*get_random_bytes_fn)(void *buf, size_t len);
typedef void (*neon_void_fn)(void);
typedef void (*neon_state_fn)(struct user_fpsimd_state *state);
typedef struct page *(*alloc_pages_fn)(gfp_t gfp, unsigned int order,
				       int preferred_nid, nodemask_t *nodemask);
typedef void (*free_pages_fn)(unsigned long addr, unsigned int order);

static void *g_alt_stack[NR_CPUS];
static struct task_struct *g_idle_task[NR_CPUS];
static atomic_t g_kick_claimed[NR_CPUS];
static DEFINE_PER_CPU(call_single_data_t, g_kick_csd);
static int g_cpu_list[NR_CPUS];
static int g_cpu_list_n;
module_param_array_named(cpu_list, g_cpu_list, int, &g_cpu_list_n, 0444);

static int fki_cpu_map(int cpu)
{
	if (!g_cpu_list_n) {
		return cpu;
	}
	return (cpu >= 0 && cpu < g_cpu_list_n) ? g_cpu_list[cpu] : -1;
}

void fki_run_on_stack(void (*fn)(void *), void *arg, void *top);

asm(
"	.pushsection .text\n"
"	.type	fki_run_on_stack, %function\n"
"fki_run_on_stack:\n"
"	hint	#34\n"
"	stp	x29, x30, [sp, #-16]!\n"
"	mov	x29, sp\n"
"	mov	x9, sp\n"
"	mov	sp, x2\n"
"	stp	x9, xzr, [sp, #-16]!\n"
"	mov	x9, x0\n"
"	mov	x0, x1\n"
"	blr	x9\n"
"	ldp	x9, xzr, [sp], #16\n"
"	mov	sp, x9\n"
"	ldp	x29, x30, [sp], #16\n"
"	ret\n"
"	.size	fki_run_on_stack, . - fki_run_on_stack\n"
"	.popsection\n");

static __always_inline void pac_keys_read(struct fki_pac_keys *k)
{
	k->ia[0] = read_sysreg_s(SYS_APIAKEYLO_EL1);
	k->ia[1] = read_sysreg_s(SYS_APIAKEYHI_EL1);
	k->ib[0] = read_sysreg_s(SYS_APIBKEYLO_EL1);
	k->ib[1] = read_sysreg_s(SYS_APIBKEYHI_EL1);
	k->da[0] = read_sysreg_s(SYS_APDAKEYLO_EL1);
	k->da[1] = read_sysreg_s(SYS_APDAKEYHI_EL1);
	k->db[0] = read_sysreg_s(SYS_APDBKEYLO_EL1);
	k->db[1] = read_sysreg_s(SYS_APDBKEYHI_EL1);
	k->ga[0] = read_sysreg_s(SYS_APGAKEYLO_EL1);
	k->ga[1] = read_sysreg_s(SYS_APGAKEYHI_EL1);
	k->sctlr_en = read_sysreg(sctlr_el1) & FKI_SCTLR_EN;
}

static __always_inline void pac_keys_write(const struct fki_pac_keys *k)
{
	write_sysreg_s(k->ia[0], SYS_APIAKEYLO_EL1);
	write_sysreg_s(k->ia[1], SYS_APIAKEYHI_EL1);
	write_sysreg_s(k->ib[0], SYS_APIBKEYLO_EL1);
	write_sysreg_s(k->ib[1], SYS_APIBKEYHI_EL1);
	write_sysreg_s(k->da[0], SYS_APDAKEYLO_EL1);
	write_sysreg_s(k->da[1], SYS_APDAKEYHI_EL1);
	write_sysreg_s(k->db[0], SYS_APDBKEYLO_EL1);
	write_sysreg_s(k->db[1], SYS_APDBKEYHI_EL1);
	write_sysreg_s(k->ga[0], SYS_APGAKEYLO_EL1);
	write_sysreg_s(k->ga[1], SYS_APGAKEYHI_EL1);
	sysreg_clear_set(sctlr_el1, FKI_SCTLR_EN, k->sctlr_en & FKI_SCTLR_EN);
	isb();
}

static __nocfi noinline void call_kernel_neon_begin(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
	neon_state_fn begin_fn;
	neon_state_fn end_fn;
	struct user_fpsimd_state scratch;

	begin_fn = (neon_state_fn)fki_sym_addr(FKI_SYM_KERNEL_NEON_BEGIN);
	end_fn = (neon_state_fn)fki_sym_addr(FKI_SYM_KERNEL_NEON_END);
	if (begin_fn && end_fn) {
		begin_fn(&scratch);
		end_fn(&scratch);
	}
#else
	neon_void_fn begin_fn;
	neon_void_fn end_fn;

	begin_fn = (neon_void_fn)fki_sym_addr(FKI_SYM_KERNEL_NEON_BEGIN);
	end_fn = (neon_void_fn)fki_sym_addr(FKI_SYM_KERNEL_NEON_END);
	if (begin_fn && end_fn) {
		begin_fn();
		end_fn();
	}
#endif
}

static __nocfi noinline void call_schedule(void)
{
	schedule_fn fn = (schedule_fn)fki_sym_addr(FKI_SYM_SCHEDULE);

	if (fn) {
		fn();
	}
}

static __nocfi noinline signed long call_schedule_timeout(signed long timeout)
{
	schedule_timeout_fn fn;

	fn = (schedule_timeout_fn)fki_sym_addr(FKI_SYM_SCHEDULE_TIMEOUT);
	if (!fn) {
		return timeout;
	}
	return fn(timeout);
}

static __nocfi noinline void call_msleep(unsigned int ms)
{
	msleep_fn fn = (msleep_fn)fki_sym_addr(FKI_SYM_MSLEEP);

	if (fn) {
		fn(ms);
	}
}

static __nocfi noinline int call_set_cpus_allowed(struct task_struct *task,
						  const struct cpumask *mask)
{
	set_cpus_allowed_fn fn;

	fn = (set_cpus_allowed_fn)fki_sym_addr(FKI_SYM_SET_CPUS_ALLOWED);
	if (!fn) {
		return -ENODEV;
	}
	return fn(task, mask);
}

static __nocfi noinline struct task_struct *call_kthread_create(
	int (*fn_entry)(void *), void *arg, const char *name)
{
	kthread_create_node_fn fn;

	fn = (kthread_create_node_fn)fki_sym_addr(FKI_SYM_KTHREAD_CREATE_NODE);
	if (!fn) {
		return ERR_PTR(-ENODEV);
	}
	return fn(fn_entry, arg, NUMA_NO_NODE, "%s", name);
}

static __nocfi noinline void call_kthread_bind(struct task_struct *task,
					       unsigned int cpu)
{
	kthread_bind_fn fn = (kthread_bind_fn)fki_sym_addr(FKI_SYM_KTHREAD_BIND);

	if (fn) {
		fn(task, cpu);
	}
}

static __nocfi noinline int call_kthread_stop(struct task_struct *task)
{
	kthread_stop_fn fn = (kthread_stop_fn)fki_sym_addr(FKI_SYM_KTHREAD_STOP);

	if (!fn) {
		return -ENODEV;
	}
	return fn(task);
}

static __nocfi noinline bool call_kthread_should_stop(void)
{
	kthread_should_stop_fn fn;

	fn = (kthread_should_stop_fn)fki_sym_addr(FKI_SYM_KTHREAD_SHOULD_STOP);
	if (!fn) {
		return false;
	}
	return fn();
}

static __nocfi noinline int call_wake_up_process(struct task_struct *task)
{
	wake_up_process_fn fn;

	fn = (wake_up_process_fn)fki_sym_addr(FKI_SYM_WAKE_UP_PROCESS);
	if (!fn) {
		return 0;
	}
	return fn(task);
}

static __nocfi noinline void call_put_task_struct(struct task_struct *task)
{
	put_task_struct_fn fn;

	fn = (put_task_struct_fn)fki_sym_addr(FKI_SYM_PUT_TASK_STRUCT);
	if (fn) {
		fn(task);
	}
}

static __nocfi noinline int call_smp_single(int cpu, smp_call_func_t func,
					    void *info, int wait)
{
	smp_call_single_fn fn;

	fn = (smp_call_single_fn)fki_sym_addr(FKI_SYM_SMP_CALL_SINGLE);
	if (!fn) {
		return -ENODEV;
	}
	return fn(cpu, func, info, wait);
}

static __nocfi noinline int call_smp_async(int cpu, call_single_data_t *csd)
{
	smp_call_async_fn fn = (smp_call_async_fn)fki_sym_addr(FKI_SYM_SMP_CALL_ASYNC);

	if (!fn) {
		return -ENODEV;
	}
	return fn(cpu, csd);
}

static __nocfi noinline void call_ktime_get_real_ts64(struct timespec64 *ts)
{
	ktime_get_real_ts64_fn fn;

	fn = (ktime_get_real_ts64_fn)fki_sym_addr(FKI_SYM_KTIME_GET_REAL_TS64);
	if (fn) {
		fn(ts);
	} else {
		ts->tv_sec = 0;
		ts->tv_nsec = 0;
	}
}

static __nocfi noinline void call_get_random_bytes(void *buf, size_t len)
{
	get_random_bytes_fn fn;

	fn = (get_random_bytes_fn)fki_sym_addr(FKI_SYM_GET_RANDOM_BYTES);
	if (fn) {
		fn(buf, len);
	}
}

static __nocfi noinline struct page *call_alloc_pages(gfp_t gfp,
						      unsigned int order)
{
	alloc_pages_fn fn = (alloc_pages_fn)fki_sym_addr(FKI_SYM_ALLOC_PAGES);

	if (!fn) {
		return NULL;
	}
	return fn(gfp, order, numa_node_id(), NULL);
}

static __nocfi noinline void call_free_pages(unsigned long addr,
					     unsigned int order)
{
	free_pages_fn fn = (free_pages_fn)fki_sym_addr(FKI_SYM_FREE_PAGES);

	if (fn) {
		fn(addr, order);
	}
}

static void fki_task_ref_get(struct task_struct *task)
{
	atomic_t *usage = (atomic_t *)fki_layout_ptr(task, FKI_LAY_TASK_USAGE);

	if (usage) {
		atomic_inc(usage);
	}
}

static void fki_task_ref_put(struct task_struct *task)
{
	atomic_t *usage = (atomic_t *)fki_layout_ptr(task, FKI_LAY_TASK_USAGE);

	if (usage && atomic_dec_and_test(usage)) {
		call_put_task_struct(task);
	}
}

static void fki_fpsimd_forget(void)
{
	call_kernel_neon_begin();
}

__attribute__((target("branch-protection=none")))
unsigned long fki_enter(const struct fki_pac_keys *foreign,
			struct fki_pac_keys *saved)
{
	unsigned long flags;

	fki_fpsimd_forget();
	preempt_disable();
	local_irq_save(flags);
	pac_keys_read(saved);
	pac_keys_write(foreign);
	return flags;
}
EXPORT_SYMBOL(fki_enter);

__attribute__((target("branch-protection=none")))
void fki_leave(unsigned long cookie, const struct fki_pac_keys *saved)
{
	pac_keys_write(saved);
	local_irq_restore(cookie);
	preempt_enable();
}
EXPORT_SYMBOL(fki_leave);

__attribute__((target("branch-protection=none")))
void fki_pac_swap(const struct fki_pac_keys *load, struct fki_pac_keys *save)
{
	pac_keys_read(save);
	pac_keys_write(load);
}
EXPORT_SYMBOL(fki_pac_swap);

static __nocfi void fki_runs_then_forget(void *ptr)
{
	struct fki_runs *run = ptr;

	run->fn(run->arg);
	local_irq_enable();
	fki_fpsimd_forget();
	local_irq_disable();
}

static bool sp_on_current_task_stack(unsigned long sp)
{
	void **stack_ptr = (void **)fki_layout_ptr(current, FKI_LAY_TASK_STACK);
	unsigned long low;

	if (!stack_ptr) {
		return false;
	}
	low = (unsigned long)*stack_ptr;
	return sp >= low && sp < low + THREAD_SIZE;
}

static __nocfi void fki_linux_runs_body(void (*fn)(void *), void *arg)
{
	unsigned long sp = current_stack_pointer;
	int cpu = smp_processor_id();

	if (sp_on_current_task_stack(sp)) {
		fn(arg);
		return;
	}
	fki_run_on_stack(fn, arg, (char *)g_alt_stack[cpu] + THREAD_SIZE);
}

static void fki_linux_runs(void (*fn)(void *), void *arg)
{
	unsigned long daif = read_sysreg(daif);
	struct fki_runs run = { fn, arg };

	fki_linux_runs_body(fki_runs_then_forget, &run);
	write_sysreg(daif, daif);
}

void fki_linux_runs_ext(void (*fn)(void *), void *arg)
{
	fki_linux_runs(fn, arg);
}

void fki_wake_idle_cpu(int cpu)
{
	struct task_struct *task;

	if (cpu < 0 || cpu >= nr_cpu_ids) {
		return;
	}
	task = READ_ONCE(g_idle_task[cpu]);
	if (task) {
		call_wake_up_process(task);
	}
}

static __nocfi noinline int call_sched_hrtimeout(ktime_t *expires, u64 delta_ns,
						 const enum hrtimer_mode mode)
{
	sched_hrtimeout_fn fn;

	fn = (sched_hrtimeout_fn)fki_sym_addr(FKI_SYM_SCHED_HRTIMEOUT);
	if (!fn) {
		return call_schedule_timeout(msecs_to_jiffies(1));
	}
	return fn(expires, delta_ns, mode);
}

static void fki_idle_body(void *ptr)
{
	struct fki_idle_args *args = ptr;
	ktime_t expires = ns_to_ktime(args->ns);

	local_irq_enable();
	preempt_enable();
	set_current_state(TASK_IDLE);
	WRITE_ONCE(g_idle_task[smp_processor_id()], current);
	smp_mb();
	if (!args->wake || !READ_ONCE(*args->wake)) {
		call_sched_hrtimeout(&expires, 50000, HRTIMER_MODE_REL);
	}
	__set_current_state(TASK_RUNNING);
	WRITE_ONCE(g_idle_task[smp_processor_id()], NULL);
	preempt_disable();
	local_irq_disable();
}

static void fki_yield_body(void *ptr)
{
	local_irq_enable();
	preempt_enable();
	cond_resched();
	preempt_disable();
	local_irq_disable();
}

void fki_idle_until(fki_u64 counter_deadline, fki_u64 max_ns,
		    const volatile unsigned int *wake)
{
	u64 now = __arch_counter_get_cntvct();
	u64 freq = arch_timer_get_cntfrq();
	u64 ns;
	struct fki_idle_args args;

	if (counter_deadline <= now) {
		fki_linux_runs(fki_yield_body, NULL);
		return;
	}
	ns = div64_u64((counter_deadline - now) * 1000000ULL, freq) * 1000ULL;
	if (ns > max_ns) {
		ns = max_ns;
	}
	args.ns = ns;
	args.wake = wake;
	fki_linux_runs(fki_idle_body, &args);
}
EXPORT_SYMBOL(fki_idle_until);

void fki_yield(void)
{
	fki_linux_runs(fki_yield_body, NULL);
}
EXPORT_SYMBOL(fki_yield);

static void fki_kick_fn(void *unused)
{
	atomic_set_release(&g_kick_claimed[smp_processor_id()], 0);
}

void fki_kick_cpu(int cpu)
{
	struct task_struct *task;

	cpu = fki_cpu_map(cpu);
	if (cpu < 0 || cpu >= nr_cpu_ids) {
		return;
	}
	smp_mb();
	task = READ_ONCE(g_idle_task[cpu]);
	if (task) {
		call_wake_up_process(task);
		return;
	}
	if (cpu != smp_processor_id() &&
	    atomic_cmpxchg_acquire(&g_kick_claimed[cpu], 0, 1) == 0) {
		if (call_smp_async(cpu, per_cpu_ptr(&g_kick_csd, cpu))) {
			atomic_set_release(&g_kick_claimed[cpu], 0);
		}
	}
}
EXPORT_SYMBOL(fki_kick_cpu);

static void fki_read_mpidr(void *ptr)
{
	*(u64 *)ptr = read_cpuid_mpidr();
}

int fki_cpu_mpidr(int cpu, fki_u64 *mpidr)
{
	u64 val = 0;
	int err;

	cpu = fki_cpu_map(cpu);
	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu) || !mpidr) {
		return -EINVAL;
	}
	err = call_smp_single(cpu, fki_read_mpidr, &val, 1);
	*mpidr = val;
	return err;
}
EXPORT_SYMBOL(fki_cpu_mpidr);

int fki_num_cpus(void)
{
	return g_cpu_list_n ? g_cpu_list_n : num_online_cpus();
}
EXPORT_SYMBOL(fki_num_cpus);

int fki_bind_cpu(int cpu)
{
	cpu = fki_cpu_map(cpu);
	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu)) {
		return -EINVAL;
	}
	return call_set_cpus_allowed(current, cpumask_of(cpu));
}
EXPORT_SYMBOL(fki_bind_cpu);

void *fki_thread_start(int (*fn)(void *), void *arg, int cpu, const char *name)
{
	struct task_struct *task;

	cpu = fki_cpu_map(cpu);
	if (!fn || cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu) ||
	    !fki_layout_ok(FKI_LAY_TASK_USAGE)) {
		return NULL;
	}
	task = call_kthread_create(fn, arg, name ? name : "fki");
	if (IS_ERR(task)) {
		return NULL;
	}
	call_kthread_bind(task, cpu);
	fki_task_ref_get(task);
	call_wake_up_process(task);
	return task;
}
EXPORT_SYMBOL(fki_thread_start);

void fki_thread_stop(void *thread)
{
	if (thread) {
		call_kthread_stop(thread);
		fki_task_ref_put(thread);
	}
}
EXPORT_SYMBOL(fki_thread_stop);

int fki_thread_should_stop(void)
{
	return call_kthread_should_stop();
}
EXPORT_SYMBOL(fki_thread_should_stop);

void fki_thread_wait(const volatile unsigned long *flag)
{
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (READ_ONCE(*flag) || call_kthread_should_stop()) {
			break;
		}
		call_schedule();
	}
	__set_current_state(TASK_RUNNING);
}
EXPORT_SYMBOL(fki_thread_wait);

void fki_thread_wake(void *thread)
{
	if (thread) {
		call_wake_up_process(thread);
	}
}
EXPORT_SYMBOL(fki_thread_wake);

void fki_msleep(unsigned int ms)
{
	unsigned long timeout = msecs_to_jiffies(ms) + 1;

	while (timeout) {
		__set_current_state(TASK_IDLE);
		timeout = call_schedule_timeout(timeout);
	}
}
EXPORT_SYMBOL(fki_msleep);

fki_u64 fki_counter_frequency(void)
{
	return arch_timer_get_cntfrq();
}
EXPORT_SYMBOL(fki_counter_frequency);

fki_u64 fki_counter(void)
{
	return __arch_counter_get_cntvct();
}
EXPORT_SYMBOL(fki_counter);

void fki_realtime(fki_u64 *sec, fki_u64 *nsec)
{
	struct timespec64 ts;

	call_ktime_get_real_ts64(&ts);
	if (sec) {
		*sec = ts.tv_sec;
	}
	if (nsec) {
		*nsec = ts.tv_nsec;
	}
}
EXPORT_SYMBOL(fki_realtime);

void fki_random(void *buf, unsigned long len)
{
	call_get_random_bytes(buf, len);
}
EXPORT_SYMBOL(fki_random);

static void fki_free_alt_stacks(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (g_alt_stack[cpu]) {
			call_free_pages((unsigned long)g_alt_stack[cpu],
					FKI_ALT_STACK_ORDER);
			g_alt_stack[cpu] = NULL;
		}
	}
}

int fki_cpu_init(void)
{
	int cpu;

	if (!fki_layout_ok(FKI_LAY_TASK_STACK) ||
	    !fki_layout_ok(FKI_LAY_TASK_USAGE)) {
		pr_err("[fki] task_struct layout unavailable\n");
		return -ENOTSUPP;
	}

	for_each_possible_cpu(cpu) {
		struct page *pg = call_alloc_pages(GFP_KERNEL,
						   FKI_ALT_STACK_ORDER);
		call_single_data_t *csd;

		if (!pg) {
			fki_free_alt_stacks();
			return -ENOMEM;
		}
		g_alt_stack[cpu] = page_address(pg);
		csd = per_cpu_ptr(&g_kick_csd, cpu);
#ifdef INIT_CSD
		INIT_CSD(csd, fki_kick_fn, NULL);
#else
		memset(csd, 0, sizeof(*csd));
		csd->func = fki_kick_fn;
		csd->info = NULL;
#endif
	}
	return 0;
}

void fki_cpu_exit(void)
{
	fki_free_alt_stacks();
}

int fki_has_s1pie(void)
{
#ifdef ARM64_HAS_S1PIE
	return cpus_have_cap(ARM64_HAS_S1PIE) ? 1 : 0;
#else
	return 0;
#endif
}
EXPORT_SYMBOL(fki_has_s1pie);
