// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/irqflags.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/preempt.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

#include "fki.h"
#include "fki_cpu.h"
#include "fki_hostcall.h"
#include "sym_reg.h"

#define HC_MAX (8UL << 20)
#define HC_TIMEOUT (120 * HZ)

struct xnu_hostcall_xlate {
	__u64 va;
	__u64 len;
	__u64 out;
	__u64 max;
};

#define XNU_HOSTCALL_XLATE _IOWR('X', 1, struct xnu_hostcall_xlate)

struct hc_call {
	const void *req;
	unsigned long len;
	void *reply;
	unsigned long max;
	long result;
};

typedef void *(*vzalloc_fn)(unsigned long size);
typedef void (*vfree_fn)(const void *addr);
typedef int (*misc_register_fn)(struct miscdevice *misc);
typedef void (*misc_deregister_fn)(struct miscdevice *misc);
typedef void (*rwsem_op_fn)(void *sem);
typedef struct vm_area_struct *(*find_vma_fn)(struct mm_struct *mm,
					      unsigned long addr);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
typedef int (*follow_pfnmap_start_fn)(struct follow_pfnmap_args *args);
typedef void (*follow_pfnmap_end_fn)(struct follow_pfnmap_args *args);
#endif
typedef int (*follow_pfn_fn)(struct vm_area_struct *vma, unsigned long address,
			     unsigned long *pfn);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
typedef long (*gup_remote_fn)(struct mm_struct *mm, unsigned long start,
			      unsigned long nr_pages, unsigned int gup_flags,
			      struct page **pages, int *locked);
#else
typedef long (*gup_remote_fn)(struct mm_struct *mm, unsigned long start,
			      unsigned long nr_pages, unsigned int gup_flags,
			      struct page **pages,
			      struct vm_area_struct **vmas, int *locked);
#endif
typedef void (*put_page_fn)(void *obj);

static DECLARE_WAIT_QUEUE_HEAD(g_hc_req_wait);
static DECLARE_WAIT_QUEUE_HEAD(g_hc_done_wait);
static DEFINE_SPINLOCK(g_hc_lock);
static DEFINE_MUTEX(g_hc_mutex);
static void *g_hc_buf;
static unsigned long g_hc_len;
static u64 g_hc_seq;
static int g_hc_state;
static long g_hc_result;
static int g_hc_open;
static bool g_hc_registered;

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

static __nocfi noinline int call_misc_register(struct miscdevice *misc)
{
	misc_register_fn fn;

	fn = (misc_register_fn)fki_sym_addr(FKI_SYM_MISC_REGISTER);
	if (!fn) {
		return -ENODEV;
	}
	return fn(misc);
}

static __nocfi noinline void call_misc_deregister(struct miscdevice *misc)
{
	misc_deregister_fn fn;

	fn = (misc_deregister_fn)fki_sym_addr(FKI_SYM_MISC_DEREGISTER);
	if (fn) {
		fn(misc);
	}
}

static __nocfi noinline void call_down_read(void *sem)
{
	rwsem_op_fn fn = (rwsem_op_fn)fki_sym_addr(FKI_SYM_DOWN_READ);

	if (fn) {
		fn(sem);
	}
}

static __nocfi noinline void call_up_read(void *sem)
{
	rwsem_op_fn fn = (rwsem_op_fn)fki_sym_addr(FKI_SYM_UP_READ);

	if (fn) {
		fn(sem);
	}
}

static __nocfi noinline struct vm_area_struct *call_find_vma(
	struct mm_struct *mm, unsigned long addr)
{
	find_vma_fn fn = (find_vma_fn)fki_sym_addr(FKI_SYM_FIND_VMA);

	if (!fn) {
		return NULL;
	}
	return fn(mm, addr);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
static __nocfi noinline int call_follow_pfnmap_start(
	struct follow_pfnmap_args *args)
{
	follow_pfnmap_start_fn fn;

	fn = (follow_pfnmap_start_fn)fki_sym_addr(FKI_SYM_FOLLOW_PFNMAP_START);
	if (!fn) {
		return -ENOTSUPP;
	}
	return fn(args);
}

static __nocfi noinline void call_follow_pfnmap_end(
	struct follow_pfnmap_args *args)
{
	follow_pfnmap_end_fn fn;

	fn = (follow_pfnmap_end_fn)fki_sym_addr(FKI_SYM_FOLLOW_PFNMAP_END);
	if (fn) {
		fn(args);
	}
}
#endif

static __nocfi noinline int call_follow_pfn(struct vm_area_struct *vma,
					    unsigned long addr,
					    unsigned long *pfn)
{
	follow_pfn_fn fn = (follow_pfn_fn)fki_sym_addr(FKI_SYM_FOLLOW_PFN);

	if (!fn) {
		return -ENOTSUPP;
	}
	return fn(vma, addr, pfn);
}

static __nocfi noinline long call_gup_remote(struct mm_struct *mm,
					     unsigned long va,
					     struct page **pg)
{
	gup_remote_fn fn = (gup_remote_fn)fki_sym_addr(FKI_SYM_GUP_REMOTE);

	if (!fn) {
		return -ENOTSUPP;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
	return fn(mm, va, 1, FOLL_WRITE, pg, NULL);
#else
	return fn(mm, va, 1, FOLL_WRITE, pg, NULL, NULL);
#endif
}

static __nocfi noinline void call_put_page(struct page *pg)
{
	put_page_fn fn = (put_page_fn)fki_sym_addr(FKI_SYM_PUT_PAGE);

	if (fn) {
		fn(compound_head(pg));
	}
}

static void hc_call_body(void *ptr)
{
	struct hc_call *call = ptr;
	static u64 seq;
	long ret;

	local_irq_enable();
	preempt_enable();
	mutex_lock(&g_hc_mutex);
	if (!g_hc_buf) {
		g_hc_buf = call_vzalloc(HC_MAX);
	}
	if (!g_hc_buf || call->len > HC_MAX) {
		ret = g_hc_buf ? -EMSGSIZE : -ENOMEM;
		goto out;
	}

	preempt_disable();
	local_irq_disable();
	memcpy(g_hc_buf, call->req, call->len);
	local_irq_enable();
	preempt_enable();

	spin_lock(&g_hc_lock);
	if (!g_hc_open) {
		spin_unlock(&g_hc_lock);
		ret = -ENODEV;
		goto out;
	}
	g_hc_len = call->len;
	g_hc_seq = ++seq;
	g_hc_state = 1;
	spin_unlock(&g_hc_lock);
	wake_up_interruptible(&g_hc_req_wait);

	wait_event_timeout(g_hc_done_wait,
			   READ_ONCE(g_hc_state) == 3 || !READ_ONCE(g_hc_open),
			   HC_TIMEOUT);

	spin_lock(&g_hc_lock);
	if (g_hc_state != 3) {
		ret = g_hc_open ? -ETIMEDOUT : -ENODEV;
		g_hc_state = 0;
		g_hc_seq = 0;
		spin_unlock(&g_hc_lock);
		goto out;
	}
	spin_unlock(&g_hc_lock);

	ret = g_hc_result;
	if (ret >= 0) {
		unsigned long copy_len = min_t(unsigned long, g_hc_len,
					       call->max);

		preempt_disable();
		local_irq_disable();
		memcpy(call->reply, g_hc_buf, copy_len);
		local_irq_enable();
		preempt_enable();
		ret = g_hc_len > call->max ? -EMSGSIZE : (long)copy_len;
	}

	spin_lock(&g_hc_lock);
	g_hc_state = 0;
	g_hc_seq = 0;
	spin_unlock(&g_hc_lock);

out:
	mutex_unlock(&g_hc_mutex);
	preempt_disable();
	local_irq_disable();
	call->result = ret;
}

long fki_hostcall(const void *req, unsigned long len, void *reply,
		  unsigned long max)
{
	struct hc_call call = { req, len, reply, max, -EIO };

	fki_linux_runs_ext(hc_call_body, &call);
	return call.result;
}
EXPORT_SYMBOL(fki_hostcall);

static ssize_t hc_read(struct file *f, char __user *buf, size_t n, loff_t *off)
{
	u64 seq;
	int err;

	err = wait_event_interruptible(g_hc_req_wait,
				       READ_ONCE(g_hc_state) == 1);
	if (err) {
		return err;
	}
	spin_lock(&g_hc_lock);
	if (g_hc_state != 1) {
		spin_unlock(&g_hc_lock);
		return -EAGAIN;
	}
	g_hc_state = 2;
	seq = g_hc_seq;
	spin_unlock(&g_hc_lock);

	if (n < 8 + g_hc_len) {
		return -EMSGSIZE;
	}
	if (copy_to_user(buf, &seq, 8) ||
	    copy_to_user(buf + 8, g_hc_buf, g_hc_len)) {
		return -EFAULT;
	}
	return 8 + g_hc_len;
}

static ssize_t hc_write(struct file *f, const char __user *buf, size_t n,
			loff_t *off)
{
	u64 seq;

	if (n < 8 || n - 8 > HC_MAX) {
		return -EINVAL;
	}
	if (copy_from_user(&seq, buf, 8)) {
		return -EFAULT;
	}
	spin_lock(&g_hc_lock);
	if (g_hc_state != 2 || seq != g_hc_seq) {
		spin_unlock(&g_hc_lock);
		return -ESTALE;
	}
	spin_unlock(&g_hc_lock);

	if (copy_from_user(g_hc_buf, buf + 8, n - 8)) {
		return -EFAULT;
	}
	g_hc_len = n - 8;
	g_hc_result = 0;
	WRITE_ONCE(g_hc_state, 3);
	wake_up(&g_hc_done_wait);
	return n;
}

static struct vm_area_struct *hc_find_vma(struct mm_struct *mm,
					  unsigned long va)
{
	struct vm_area_struct *vma = call_find_vma(mm, va);
	unsigned long *vm_start;

	if (!vma) {
		return NULL;
	}
	vm_start = (unsigned long *)fki_layout_ptr(vma, FKI_LAY_VMA_START);
	if (!vm_start || va < *vm_start) {
		return NULL;
	}
	return vma;
}

static long hc_xlate_pfnmap(struct vm_area_struct *vma, unsigned long va,
			    u64 *pa)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	if (fki_sym_addr(FKI_SYM_FOLLOW_PFNMAP_START) &&
	    fki_sym_addr(FKI_SYM_FOLLOW_PFNMAP_END)) {
		struct follow_pfnmap_args args = { .vma = vma, .address = va };
		int err = call_follow_pfnmap_start(&args);

		if (err) {
			return err;
		}
		*pa = (u64)args.pfn << PAGE_SHIFT;
		call_follow_pfnmap_end(&args);
		return 0;
	}
#endif
	{
		unsigned long pfn = 0;
		int err = call_follow_pfn(vma, va, &pfn);

		if (err) {
			return err;
		}
		*pa = (u64)pfn << PAGE_SHIFT;
		return 0;
	}
}

static long hc_xlate_one(struct mm_struct *mm, unsigned long va, u64 *pa)
{
	struct vm_area_struct *vma = hc_find_vma(mm, va);
	unsigned long *vm_flags;
	struct page *pg;

	if (!vma) {
		return -EFAULT;
	}
	vm_flags = (unsigned long *)fki_layout_ptr(vma, FKI_LAY_VMA_FLAGS);
	if (!vm_flags) {
		return -ENOTSUPP;
	}
	if (*vm_flags & (VM_IO | VM_PFNMAP)) {
		return hc_xlate_pfnmap(vma, va, pa);
	}
	if (call_gup_remote(mm, va, &pg) != 1) {
		return -EFAULT;
	}
	*pa = page_to_phys(pg);
	call_put_page(pg);
	return 0;
}

static long hc_xlate(struct xnu_hostcall_xlate __user *ua)
{
	struct xnu_hostcall_xlate req;
	struct mm_struct **mm_ptr;
	struct mm_struct *mm;
	void *mmap_lock;
	unsigned long i;
	unsigned long nr_pages;
	long err = 0;

	if (!fki_layout_ok(FKI_LAY_TASK_MM) ||
	    !fki_layout_ok(FKI_LAY_MM_LOCK) ||
	    !fki_layout_ok(FKI_LAY_VMA_START) ||
	    !fki_layout_ok(FKI_LAY_VMA_FLAGS) ||
	    !fki_sym_addr(FKI_SYM_DOWN_READ) ||
	    !fki_sym_addr(FKI_SYM_UP_READ) ||
	    !fki_sym_addr(FKI_SYM_FIND_VMA)) {
		return -ENOTSUPP;
	}
	mm_ptr = (struct mm_struct **)fki_layout_ptr(current, FKI_LAY_TASK_MM);
	if (!mm_ptr || !*mm_ptr) {
		return -EINVAL;
	}
	mm = *mm_ptr;
	mmap_lock = fki_layout_ptr(mm, FKI_LAY_MM_LOCK);
	if (!mmap_lock) {
		return -ENOTSUPP;
	}

	if (copy_from_user(&req, ua, sizeof(req))) {
		return -EFAULT;
	}
	if ((req.va | req.len) & (PAGE_SIZE - 1)) {
		return -EINVAL;
	}
	nr_pages = req.len >> PAGE_SHIFT;
	if (nr_pages > req.max) {
		return -E2BIG;
	}
	call_down_read(mmap_lock);
	for (i = 0; i < nr_pages && !err; i++) {
		u64 pa = 0;

		err = hc_xlate_one(mm, req.va + (i << PAGE_SHIFT), &pa);
		if (!err && put_user(pa, (u64 __user *)req.out + i)) {
			err = -EFAULT;
		}
	}
	call_up_read(mmap_lock);
	return err;
}

static long hc_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	if (cmd == XNU_HOSTCALL_XLATE) {
		return hc_xlate((struct xnu_hostcall_xlate __user *)arg);
	}
	return -ENOTTY;
}

static int hc_dev_open(struct inode *i, struct file *f)
{
	spin_lock(&g_hc_lock);
	g_hc_open++;
	spin_unlock(&g_hc_lock);
	return 0;
}

static int hc_dev_release(struct inode *i, struct file *f)
{
	spin_lock(&g_hc_lock);
	g_hc_open--;
	spin_unlock(&g_hc_lock);
	wake_up(&g_hc_done_wait);
	return 0;
}

static struct file_operations g_hc_fops = {
	.owner = THIS_MODULE,
	.open = hc_dev_open,
	.release = hc_dev_release,
	.read = hc_read,
	.write = hc_write,
	.unlocked_ioctl = hc_ioctl,
};

static struct miscdevice g_hc_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "xnu-hostcall",
	.fops = &g_hc_fops,
	.mode = 0600,
};

int fki_hostcall_init(void)
{
	int err;

	g_hc_fops.llseek = (void *)fki_sym_addr(FKI_SYM_NOOP_LLSEEK);
	err = call_misc_register(&g_hc_dev);
	if (!err) {
		g_hc_registered = true;
	}
	return err;
}

void fki_hostcall_exit(void)
{
	if (g_hc_registered) {
		call_misc_deregister(&g_hc_dev);
		g_hc_registered = false;
	}
	call_vfree(g_hc_buf);
	g_hc_buf = NULL;
}
