// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 * Copyright (C) 2026 Heyang Zhou
 */

#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/irqflags.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "fki.h"
#include "fki_cpu.h"
#include "fki_io.h"
#include "sym_reg.h"

#define FKI_FILE_CHUNK (256UL << 10)
#define FKI_NET_QLEN 1024

struct fki_file_args {
	void *file;
	int write;
	void *buf;
	unsigned long len;
	fki_u64 off;
	long ret;
};

struct fki_net_io {
	const void *in;
	void *out;
	unsigned long len;
	long result;
};

typedef struct file *(*filp_open_fn)(const char *filename, int flags,
				     umode_t mode);
typedef ssize_t (*kernel_read_fn)(struct file *file, void *buf, size_t count,
				  loff_t *pos);
typedef ssize_t (*kernel_write_fn)(struct file *file, const void *buf,
				   size_t count, loff_t *pos);

static struct net_device *g_netdev;
static struct sk_buff_head g_net_rxq;

static __nocfi noinline struct file *call_filp_open(const char *path, int flags,
						    umode_t mode)
{
	filp_open_fn fn = (filp_open_fn)fki_sym_addr(FKI_SYM_FILP_OPEN);

	if (!fn) {
		return ERR_PTR(-ENOENT);
	}
	return fn(path, flags, mode);
}

static __nocfi noinline ssize_t call_kernel_read(struct file *f, void *buf,
						 size_t count, loff_t *pos)
{
	kernel_read_fn fn = (kernel_read_fn)fki_sym_addr(FKI_SYM_KERNEL_READ);

	if (!fn) {
		return -ENOENT;
	}
	return fn(f, buf, count, pos);
}

static __nocfi noinline ssize_t call_kernel_write(struct file *f,
						  const void *buf,
						  size_t count, loff_t *pos)
{
	kernel_write_fn fn = (kernel_write_fn)fki_sym_addr(FKI_SYM_KERNEL_WRITE);

	if (!fn) {
		return -ENOENT;
	}
	return fn(f, buf, count, pos);
}

static bool netdev_layout_ok(void)
{
	return fki_layout_ok(FKI_LAY_NETDEV_NAME) &&
	       fki_layout_ok(FKI_LAY_NETDEV_OPS) &&
	       fki_layout_ok(FKI_LAY_NETDEV_MIN_MTU) &&
	       fki_layout_ok(FKI_LAY_NETDEV_MAX_MTU) &&
	       fki_layout_ok(FKI_LAY_NETDEV_STATS) &&
	       fki_layout_ok(FKI_LAY_NETSTATS_RX_PKTS) &&
	       fki_layout_ok(FKI_LAY_NETSTATS_RX_BYTES) &&
	       fki_layout_ok(FKI_LAY_NETSTATS_RX_DROP) &&
	       fki_layout_ok(FKI_LAY_NETSTATS_TX_PKTS) &&
	       fki_layout_ok(FKI_LAY_NETSTATS_TX_BYTES) &&
	       fki_layout_ok(FKI_LAY_NETSTATS_TX_DROP);
}

static unsigned long *netdev_stat_ptr(struct net_device *dev,
				      enum fki_layout_id field)
{
	void *stats = fki_layout_ptr(dev, FKI_LAY_NETDEV_STATS);

	if (!stats) {
		return NULL;
	}
	return (unsigned long *)fki_layout_ptr(stats, field);
}

static void netdev_stat_inc(struct net_device *dev, enum fki_layout_id field,
			    unsigned long delta)
{
	unsigned long *stat = netdev_stat_ptr(dev, field);

	if (stat) {
		*stat += delta;
	}
}

int fki_blob_get(const char *name, const void **data, unsigned long *size,
		 void **handle)
{
	const struct firmware *fw;
	const u8 *const *fw_data;
	const size_t *fw_size;
	int err;

	if (!name || !data || !size || !handle) {
		return -EINVAL;
	}
	if (!fki_layout_ok(FKI_LAY_FW_DATA) || !fki_layout_ok(FKI_LAY_FW_SIZE)) {
		return -ENOTSUPP;
	}
	err = request_firmware(&fw, name, NULL);
	if (err) {
		return err;
	}
	fw_data = (const u8 *const *)fki_layout_ptr(fw, FKI_LAY_FW_DATA);
	fw_size = (const size_t *)fki_layout_ptr(fw, FKI_LAY_FW_SIZE);
	if (!fw_data || !fw_size) {
		release_firmware(fw);
		return -ENOTSUPP;
	}
	*data = *fw_data;
	*size = *fw_size;
	*handle = (void *)fw;
	return 0;
}
EXPORT_SYMBOL(fki_blob_get);

void fki_blob_put(void *handle)
{
	if (handle) {
		release_firmware(handle);
	}
}
EXPORT_SYMBOL(fki_blob_put);

void *fki_file_open(const char *path, int writable, fki_u64 *size)
{
	struct file *file;
	void **mapping;
	void **host;
	loff_t *isize;

	if (!path) {
		return NULL;
	}
	if (size && (!fki_layout_ok(FKI_LAY_FILE_MAPPING) ||
		     !fki_layout_ok(FKI_LAY_ADDR_HOST) ||
		     !fki_layout_ok(FKI_LAY_INODE_SIZE))) {
		return NULL;
	}
	file = call_filp_open(path,
			      (writable ? O_RDWR : O_RDONLY) | O_LARGEFILE, 0);
	if (IS_ERR(file)) {
		return NULL;
	}
	if (size) {
		mapping = (void **)fki_layout_ptr(file, FKI_LAY_FILE_MAPPING);
		host = mapping && *mapping ?
		       (void **)fki_layout_ptr(*mapping, FKI_LAY_ADDR_HOST) :
		       NULL;
		isize = host && *host ?
			(loff_t *)fki_layout_ptr(*host, FKI_LAY_INODE_SIZE) :
			NULL;
		if (!isize) {
			filp_close(file, NULL);
			return NULL;
		}
		*size = READ_ONCE(*isize);
	}
	return file;
}
EXPORT_SYMBOL(fki_file_open);

static long fki_file_rw_body(void *file, int write, void *buf,
			     unsigned long len, fki_u64 off)
{
	struct file *filp = file;
	unsigned long done = 0;
	long ret = 0;
	void *bounce;

	local_irq_enable();
	preempt_enable();
	bounce = kvmalloc(min(len, FKI_FILE_CHUNK), GFP_KERNEL);
	if (!bounce) {
		ret = -ENOMEM;
	}
	while (bounce && done < len) {
		unsigned long chunk = min(len - done, FKI_FILE_CHUNK);
		loff_t pos = off + done;
		ssize_t io_ret;

		if (write) {
			preempt_disable();
			local_irq_disable();
			memcpy(bounce, (char *)buf + done, chunk);
			local_irq_enable();
			preempt_enable();
			io_ret = call_kernel_write(filp, bounce, chunk, &pos);
		} else {
			io_ret = call_kernel_read(filp, bounce, chunk, &pos);
			if (io_ret > 0) {
				preempt_disable();
				local_irq_disable();
				memcpy((char *)buf + done, bounce, io_ret);
				local_irq_enable();
				preempt_enable();
			}
		}
		if (io_ret < 0) {
			ret = io_ret;
			break;
		}
		done += io_ret;
		if ((unsigned long)io_ret < chunk) {
			break;
		}
	}
	kvfree(bounce);
	preempt_disable();
	local_irq_disable();
	return ret < 0 && !done ? ret : (long)done;
}

static void fki_file_rw_thunk(void *ptr)
{
	struct fki_file_args *args = ptr;

	args->ret = fki_file_rw_body(args->file, args->write, args->buf,
				     args->len, args->off);
}

long fki_file_rw(void *file, int write, void *buf, unsigned long len,
		 fki_u64 off)
{
	struct fki_file_args args = { file, write, buf, len, off, -EIO };

	if (!file) {
		return -EBADF;
	}
	fki_linux_runs_ext(fki_file_rw_thunk, &args);
	return args.ret;
}
EXPORT_SYMBOL(fki_file_rw);

long fki_file_pwrite(void *file, const void *buf, unsigned long len,
		     fki_u64 off)
{
	loff_t pos = off;

	if (!file) {
		return -EBADF;
	}
	return call_kernel_write(file, buf, len, &pos);
}
EXPORT_SYMBOL(fki_file_pwrite);

void fki_file_close(void *file)
{
	if (file) {
		filp_close(file, NULL);
	}
}
EXPORT_SYMBOL(fki_file_close);

static netdev_tx_t fki_net_xmit(struct sk_buff *skb, struct net_device *dev)
{
	if (skb_queue_len(&g_net_rxq) >= FKI_NET_QLEN) {
		netdev_stat_inc(dev, FKI_LAY_NETSTATS_TX_DROP, 1);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	netdev_stat_inc(dev, FKI_LAY_NETSTATS_TX_PKTS, 1);
	netdev_stat_inc(dev, FKI_LAY_NETSTATS_TX_BYTES, skb->len);
	skb_queue_tail(&g_net_rxq, skb);
	fki_wake_idle_cpu(0);
	return NETDEV_TX_OK;
}

static int fki_net_up(struct net_device *dev)
{
	netif_start_queue(dev);
	return 0;
}

static int fki_net_down(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

static const struct net_device_ops fki_net_ops = {
	.ndo_open = fki_net_up,
	.ndo_stop = fki_net_down,
	.ndo_start_xmit = fki_net_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

static void fki_net_send_body(void *p)
{
	struct fki_net_io *io = p;
	struct sk_buff *skb;

	io->result = -ENODEV;
	if (!g_netdev || io->len < ETH_HLEN || io->len > ETH_FRAME_LEN + 4) {
		return;
	}
	skb = netdev_alloc_skb(g_netdev, io->len + NET_IP_ALIGN);
	if (!skb) {
		netdev_stat_inc(g_netdev, FKI_LAY_NETSTATS_RX_DROP, 1);
		io->result = -ENOMEM;
		return;
	}
	skb_reserve(skb, NET_IP_ALIGN);
	skb_put_data(skb, io->in, io->len);
	skb->protocol = eth_type_trans(skb, g_netdev);
	netdev_stat_inc(g_netdev, FKI_LAY_NETSTATS_RX_PKTS, 1);
	netdev_stat_inc(g_netdev, FKI_LAY_NETSTATS_RX_BYTES, io->len);
	local_irq_enable();
	netif_rx(skb);
	local_irq_disable();
	io->result = io->len;
}

long fki_net_send(const void *frame, unsigned long len)
{
	struct fki_net_io io = { .in = frame, .len = len };

	fki_linux_runs_ext(fki_net_send_body, &io);
	return io.result;
}
EXPORT_SYMBOL(fki_net_send);

static void fki_net_recv_body(void *p)
{
	struct fki_net_io *io = p;
	struct sk_buff *skb = skb_dequeue(&g_net_rxq);

	io->result = 0;
	if (!skb) {
		return;
	}
	if (skb->len > io->len) {
		io->result = -EMSGSIZE;
	} else if (skb_copy_bits(skb, 0, io->out, skb->len) == 0) {
		io->result = skb->len;
	} else {
		io->result = -EIO;
	}
	dev_consume_skb_any(skb);
}

long fki_net_recv(void *buf, unsigned long len)
{
	struct fki_net_io io = { .out = buf, .len = len };

	fki_linux_runs_ext(fki_net_recv_body, &io);
	return io.result;
}
EXPORT_SYMBOL(fki_net_recv);

long fki_net_pending(void)
{
	return skb_queue_len(&g_net_rxq);
}
EXPORT_SYMBOL(fki_net_pending);

int fki_io_init(void)
{
	struct net_device *dev;
	char *name;
	const struct net_device_ops **ops;
	unsigned int *min_mtu;
	unsigned int *max_mtu;
	int err;

	skb_queue_head_init(&g_net_rxq);
	if (!netdev_layout_ok()) {
		pr_warn("[fki] xnu0 disabled: net_device layout unavailable\n");
		return 0;
	}
	dev = alloc_etherdev(0);
	if (!dev) {
		return 0;
	}
	name = (char *)fki_layout_ptr(dev, FKI_LAY_NETDEV_NAME);
	ops = (const struct net_device_ops **)fki_layout_ptr(dev,
							     FKI_LAY_NETDEV_OPS);
	min_mtu = (unsigned int *)fki_layout_ptr(dev, FKI_LAY_NETDEV_MIN_MTU);
	max_mtu = (unsigned int *)fki_layout_ptr(dev, FKI_LAY_NETDEV_MAX_MTU);
	strscpy(name, "xnu0", IFNAMSIZ);
	*ops = &fki_net_ops;
	eth_hw_addr_random(dev);
	*min_mtu = 68;
	*max_mtu = ETH_DATA_LEN;
	err = register_netdev(dev);
	if (err) {
		free_netdev(dev);
		pr_warn("[fki] no xnu0 link (%d)\n", err);
		return 0;
	}
	g_netdev = dev;
	return 0;
}

void fki_io_exit(void)
{
	if (g_netdev) {
		unregister_netdev(g_netdev);
		free_netdev(g_netdev);
		g_netdev = NULL;
	}
	skb_queue_purge(&g_net_rxq);
}
