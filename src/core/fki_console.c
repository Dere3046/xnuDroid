// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/irqflags.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "fki.h"
#include "fki_console.h"
#include "fki_cpu.h"
#include "sym_reg.h"

#define CON_TX_SIZE 65536
#define CON_RX_SIZE 16384

typedef int (*misc_register_fn)(struct miscdevice *misc);
typedef void (*misc_deregister_fn)(struct miscdevice *misc);

static DECLARE_WAIT_QUEUE_HEAD(g_con_read_wait);
static DECLARE_WAIT_QUEUE_HEAD(g_con_write_wait);
static DEFINE_SPINLOCK(g_con_lock);

static char g_con_tx[CON_TX_SIZE];
static unsigned int g_con_tx_head;
static unsigned int g_con_tx_tail;

static char g_con_rx[CON_RX_SIZE];
static unsigned int g_con_rx_head;
static unsigned int g_con_rx_tail;

static int g_con_open;
static bool g_con_registered;

static __nocfi noinline int call_misc_register(struct miscdevice *misc)
{
	misc_register_fn fn =
		(misc_register_fn)fki_sym_addr(FKI_SYM_MISC_REGISTER);

	if (!fn) {
		return -ENOTSUPP;
	}
	return fn(misc);
}

static __nocfi noinline void call_misc_deregister(struct miscdevice *misc)
{
	misc_deregister_fn fn =
		(misc_deregister_fn)fki_sym_addr(FKI_SYM_MISC_DEREGISTER);

	if (fn) {
		fn(misc);
	}
}

static inline unsigned int con_tx_len_locked(void)
{
	return g_con_tx_head - g_con_tx_tail;
}

static inline unsigned int con_rx_len_locked(void)
{
	return g_con_rx_head - g_con_rx_tail;
}

struct con_io_args {
	const char *wbuf;
	char *rbuf;
	unsigned long len;
	long ret;
};

static void con_write_body(void *ptr)
{
	struct con_io_args *args = ptr;
	unsigned long flags;
	unsigned long i;

	spin_lock_irqsave(&g_con_lock, flags);
	for (i = 0; i < args->len; i++) {
		if (con_tx_len_locked() >= CON_TX_SIZE) {
			g_con_tx_tail++;
		}
		g_con_tx[g_con_tx_head & (CON_TX_SIZE - 1)] = args->wbuf[i];
		g_con_tx_head++;
	}
	args->ret = (long)args->len;
	spin_unlock_irqrestore(&g_con_lock, flags);

	local_irq_enable();
	preempt_enable();
	wake_up_interruptible(&g_con_read_wait);
	preempt_disable();
	local_irq_disable();
}

static void con_read_body(void *ptr)
{
	struct con_io_args *args = ptr;
	unsigned long flags;
	unsigned long got = 0;

	spin_lock_irqsave(&g_con_lock, flags);
	while (got < args->len && con_rx_len_locked() > 0) {
		args->rbuf[got++] = g_con_rx[g_con_rx_tail & (CON_RX_SIZE - 1)];
		g_con_rx_tail++;
	}
	args->ret = (long)got;
	spin_unlock_irqrestore(&g_con_lock, flags);

	if (got > 0) {
		local_irq_enable();
		preempt_enable();
		wake_up_interruptible(&g_con_write_wait);
		preempt_disable();
		local_irq_disable();
	}
}

long fki_console_write(const char *buf, unsigned long len)
{
	struct con_io_args args = {
		.wbuf = buf,
		.len = len,
		.ret = 0,
	};

	if (!buf || !len) {
		return 0;
	}
	fki_linux_runs_ext(con_write_body, &args);
	return args.ret;
}
EXPORT_SYMBOL(fki_console_write);

long fki_console_read(char *buf, unsigned long len)
{
	struct con_io_args args = {
		.rbuf = buf,
		.len = len,
		.ret = 0,
	};

	if (!buf || !len) {
		return 0;
	}
	fki_linux_runs_ext(con_read_body, &args);
	return args.ret;
}
EXPORT_SYMBOL(fki_console_read);

int fki_console_open_count(void)
{
	return READ_ONCE(g_con_open);
}
EXPORT_SYMBOL(fki_console_open_count);

static ssize_t con_dev_read(struct file *f, char __user *ubuf, size_t n,
			    loff_t *off)
{
	char tmp[1024];
	unsigned long flags;
	size_t total = 0;

	(void)off;
	if (n == 0) {
		return 0;
	}

	spin_lock_irqsave(&g_con_lock, flags);
	while (con_tx_len_locked() == 0) {
		spin_unlock_irqrestore(&g_con_lock, flags);
		if (f->f_flags & O_NONBLOCK) {
			return -EAGAIN;
		}
		if (wait_event_interruptible(g_con_read_wait,
					     con_tx_len_locked() > 0)) {
			return -ERESTARTSYS;
		}
		spin_lock_irqsave(&g_con_lock, flags);
	}

	while (total < n && con_tx_len_locked() > 0) {
		size_t chunk = 0;

		while (chunk < sizeof(tmp) && total + chunk < n &&
		       con_tx_len_locked() > 0) {
			tmp[chunk++] =
				g_con_tx[g_con_tx_tail & (CON_TX_SIZE - 1)];
			g_con_tx_tail++;
		}
		spin_unlock_irqrestore(&g_con_lock, flags);

		if (copy_to_user(ubuf + total, tmp, chunk)) {
			return total > 0 ? (ssize_t)total : -EFAULT;
		}
		total += chunk;
		if (total >= n) {
			return (ssize_t)total;
		}
		spin_lock_irqsave(&g_con_lock, flags);
	}
	spin_unlock_irqrestore(&g_con_lock, flags);
	return (ssize_t)total;
}

static ssize_t con_dev_write(struct file *f, const char __user *ubuf, size_t n,
			     loff_t *off)
{
	char tmp[1024];
	unsigned long flags;
	size_t written = 0;

	(void)off;
	if (n == 0) {
		return 0;
	}

	while (written < n) {
		size_t chunk = n - written;
		size_t pushed = 0;

		if (chunk > sizeof(tmp)) {
			chunk = sizeof(tmp);
		}
		if (copy_from_user(tmp, ubuf + written, chunk)) {
			return written > 0 ? (ssize_t)written : -EFAULT;
		}

		spin_lock_irqsave(&g_con_lock, flags);
		while (con_rx_len_locked() >= CON_RX_SIZE) {
			spin_unlock_irqrestore(&g_con_lock, flags);
			if (written > 0) {
				fki_wake_idle_cpu(0);
				return (ssize_t)written;
			}
			if (f->f_flags & O_NONBLOCK) {
				return -EAGAIN;
			}
			if (wait_event_interruptible(g_con_write_wait,
						     con_rx_len_locked() <
						     CON_RX_SIZE)) {
				return -ERESTARTSYS;
			}
			spin_lock_irqsave(&g_con_lock, flags);
		}

		while (pushed < chunk && con_rx_len_locked() < CON_RX_SIZE) {
			g_con_rx[g_con_rx_head & (CON_RX_SIZE - 1)] =
				tmp[pushed++];
			g_con_rx_head++;
		}
		spin_unlock_irqrestore(&g_con_lock, flags);

		written += pushed;
		if (pushed > 0) {
			fki_wake_idle_cpu(0);
		}
	}
	return (ssize_t)written;
}

static __poll_t con_dev_poll(struct file *f, struct poll_table_struct *wait)
{
	unsigned long flags;
	__poll_t mask = 0;

	poll_wait(f, &g_con_read_wait, wait);
	poll_wait(f, &g_con_write_wait, wait);

	spin_lock_irqsave(&g_con_lock, flags);
	if (con_tx_len_locked() > 0) {
		mask |= EPOLLIN | EPOLLRDNORM;
	}
	if (con_rx_len_locked() < CON_RX_SIZE) {
		mask |= EPOLLOUT | EPOLLWRNORM;
	}
	spin_unlock_irqrestore(&g_con_lock, flags);

	return mask;
}

static int con_dev_open(struct inode *i, struct file *f)
{
	unsigned long flags;

	(void)i;
	(void)f;
	spin_lock_irqsave(&g_con_lock, flags);
	g_con_open++;
	spin_unlock_irqrestore(&g_con_lock, flags);
	return 0;
}

static int con_dev_release(struct inode *i, struct file *f)
{
	unsigned long flags;

	(void)i;
	(void)f;
	spin_lock_irqsave(&g_con_lock, flags);
	if (g_con_open > 0) {
		g_con_open--;
	}
	spin_unlock_irqrestore(&g_con_lock, flags);
	return 0;
}

static struct file_operations g_con_fops = {
	.owner = THIS_MODULE,
	.open = con_dev_open,
	.release = con_dev_release,
	.read = con_dev_read,
	.write = con_dev_write,
	.poll = con_dev_poll,
};

static struct miscdevice g_con_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "xnu-console",
	.fops = &g_con_fops,
	.mode = 0600,
};

int fki_console_init(void)
{
	int err;

	g_con_fops.llseek = (void *)fki_sym_addr(FKI_SYM_NOOP_LLSEEK);
	err = call_misc_register(&g_con_dev);
	if (!err) {
		g_con_registered = true;
	}
	return err;
}

void fki_console_exit(void)
{
	if (g_con_registered) {
		call_misc_deregister(&g_con_dev);
		g_con_registered = false;
	}
}
