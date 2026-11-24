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
#include <linux/spinlock.h>
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
typedef int (*filp_close_fn)(struct file *filp, fl_owner_t id);
typedef ssize_t (*kernel_read_fn)(struct file *file, void *buf, size_t count,
				  loff_t *pos);
typedef ssize_t (*kernel_write_fn)(struct file *file, const void *buf,
				   size_t count, loff_t *pos);
typedef int (*request_firmware_fn)(const struct firmware **fw,
				   const char *name, struct device *device);
typedef void (*release_firmware_fn)(const struct firmware *fw);
typedef void *(*vzalloc_fn)(unsigned long size);
typedef void (*vfree_fn)(const void *addr);
typedef struct net_device *(*alloc_etherdev_fn)(int sizeof_priv,
						unsigned int txqs,
						unsigned int rxqs);
typedef void (*free_netdev_fn)(struct net_device *dev);
typedef int (*register_netdev_fn)(struct net_device *dev);
typedef void (*unregister_netdev_fn)(struct net_device *dev);
typedef __be16 (*eth_type_trans_fn)(struct sk_buff *skb,
				    struct net_device *dev);
typedef int (*netif_rx_fn)(struct sk_buff *skb);
typedef void (*dev_addr_mod_fn)(struct net_device *dev, unsigned int offset,
				const void *addr, size_t len);
typedef struct sk_buff *(*netdev_alloc_skb_fn)(struct net_device *dev,
					       unsigned int len, gfp_t gfp);
typedef void *(*skb_put_fn)(struct sk_buff *skb, unsigned int len);
typedef int (*skb_copy_bits_fn)(const struct sk_buff *skb, int offset, void *to,
				int len);
typedef struct sk_buff *(*skb_dequeue_fn)(struct sk_buff_head *list);
typedef void (*skb_queue_tail_fn)(struct sk_buff_head *list,
				  struct sk_buff *newsk);
typedef void (*kfree_skb_reason_fn)(struct sk_buff *skb, unsigned int reason);
typedef void (*consume_skb_fn)(struct sk_buff *skb);

static struct net_device *g_netdev;
static struct sk_buff_head g_net_rxq;
static struct net_device_ops fki_net_ops;

static __nocfi noinline struct file *call_filp_open(const char *path, int flags,
						    umode_t mode)
{
	filp_open_fn fn = (filp_open_fn)fki_sym_addr(FKI_SYM_FILP_OPEN);

	if (!fn) {
		return ERR_PTR(-ENOENT);
	}
	return fn(path, flags, mode);
}

static __nocfi noinline void call_filp_close(struct file *file)
{
	filp_close_fn fn = (filp_close_fn)fki_sym_addr(FKI_SYM_FILP_CLOSE);

	if (fn) {
		fn(file, NULL);
	}
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

static __nocfi noinline int call_request_firmware(const struct firmware **fw,
						  const char *name)
{
	request_firmware_fn fn;

	fn = (request_firmware_fn)fki_sym_addr(FKI_SYM_REQUEST_FIRMWARE);
	if (!fn) {
		return -ENOENT;
	}
	return fn(fw, name, NULL);
}

static __nocfi noinline void call_release_firmware(const struct firmware *fw)
{
	release_firmware_fn fn;

	fn = (release_firmware_fn)fki_sym_addr(FKI_SYM_RELEASE_FIRMWARE);
	if (fn) {
		fn(fw);
	}
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

static __nocfi noinline struct net_device *call_alloc_etherdev(void)
{
	alloc_etherdev_fn fn;

	fn = (alloc_etherdev_fn)fki_sym_addr(FKI_SYM_ALLOC_ETHERDEV_MQS);
	if (!fn) {
		return NULL;
	}
	return fn(0, 1, 1);
}

static __nocfi noinline void call_free_netdev(struct net_device *dev)
{
	free_netdev_fn fn = (free_netdev_fn)fki_sym_addr(FKI_SYM_FREE_NETDEV);

	if (fn) {
		fn(dev);
	}
}

static __nocfi noinline int call_register_netdev(struct net_device *dev)
{
	register_netdev_fn fn;

	fn = (register_netdev_fn)fki_sym_addr(FKI_SYM_REGISTER_NETDEV);
	if (!fn) {
		return -ENODEV;
	}
	return fn(dev);
}

static __nocfi noinline void call_unregister_netdev(struct net_device *dev)
{
	unregister_netdev_fn fn;

	fn = (unregister_netdev_fn)fki_sym_addr(FKI_SYM_UNREGISTER_NETDEV);
	if (fn) {
		fn(dev);
	}
}

static __nocfi noinline __be16 call_eth_type_trans(struct sk_buff *skb,
						   struct net_device *dev)
{
	eth_type_trans_fn fn;

	fn = (eth_type_trans_fn)fki_sym_addr(FKI_SYM_ETH_TYPE_TRANS);
	if (!fn) {
		return 0;
	}
	return fn(skb, dev);
}

static __nocfi noinline int call_netif_rx(struct sk_buff *skb)
{
	netif_rx_fn fn = (netif_rx_fn)fki_sym_addr(FKI_SYM_NETIF_RX);

	if (!fn) {
		return NET_RX_DROP;
	}
	return fn(skb);
}

static __nocfi noinline void call_dev_addr_mod(struct net_device *dev,
					       const void *addr, size_t len)
{
	dev_addr_mod_fn fn = (dev_addr_mod_fn)fki_sym_addr(FKI_SYM_DEV_ADDR_MOD);

	if (fn) {
		fn(dev, 0, addr, len);
	}
}

static __nocfi noinline struct sk_buff *call_netdev_alloc_skb(
	struct net_device *dev, unsigned int len, gfp_t gfp)
{
	netdev_alloc_skb_fn fn;

	fn = (netdev_alloc_skb_fn)fki_sym_addr(FKI_SYM_NETDEV_ALLOC_SKB);
	if (!fn) {
		return NULL;
	}
	return fn(dev, len, gfp);
}

static __nocfi noinline void *call_skb_put(struct sk_buff *skb,
					   unsigned int len)
{
	skb_put_fn fn = (skb_put_fn)fki_sym_addr(FKI_SYM_SKB_PUT);

	if (!fn) {
		return NULL;
	}
	return fn(skb, len);
}

static __nocfi noinline int call_skb_copy_bits(const struct sk_buff *skb,
					       int off, void *to, int len)
{
	skb_copy_bits_fn fn;

	fn = (skb_copy_bits_fn)fki_sym_addr(FKI_SYM_SKB_COPY_BITS);
	if (!fn) {
		return -ENODEV;
	}
	return fn(skb, off, to, len);
}

static __nocfi noinline struct sk_buff *call_skb_dequeue(
	struct sk_buff_head *list)
{
	skb_dequeue_fn fn = (skb_dequeue_fn)fki_sym_addr(FKI_SYM_SKB_DEQUEUE);

	if (!fn) {
		return NULL;
	}
	return fn(list);
}

static __nocfi noinline void call_skb_queue_tail(struct sk_buff_head *list,
						 struct sk_buff *skb)
{
	skb_queue_tail_fn fn;

	fn = (skb_queue_tail_fn)fki_sym_addr(FKI_SYM_SKB_QUEUE_TAIL);
	if (fn) {
		fn(list, skb);
	}
}

static __nocfi noinline void call_kfree_skb_any(struct sk_buff *skb,
						bool dropped)
{
	kfree_skb_reason_fn reason_fn;
	consume_skb_fn consume_fn;

	reason_fn = (kfree_skb_reason_fn)fki_sym_addr(
		FKI_SYM_DEV_KFREE_SKB_REASON);
	if (reason_fn) {
		reason_fn(skb, dropped ? 2U : 1U);
		return;
	}
	reason_fn = (kfree_skb_reason_fn)fki_sym_addr(
		FKI_SYM_DEV_KFREE_SKB_ANY);
	if (reason_fn) {
		reason_fn(skb, dropped ? 1U : 0U);
		return;
	}
	consume_fn = (consume_skb_fn)fki_sym_addr(FKI_SYM_CONSUME_SKB);
	if (consume_fn) {
		consume_fn(skb);
	}
}

static bool netdev_layout_ok(void)
{
	return fki_layout_ok(FKI_LAY_NETDEV_NAME) &&
	       fki_layout_ok(FKI_LAY_NETDEV_OPS) &&
	       fki_layout_ok(FKI_LAY_NETDEV_DEV_ADDR) &&
	       fki_layout_ok(FKI_LAY_NETDEV_ADDR_ASSIGN) &&
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

static bool netdev_syms_ok(void)
{
	return fki_sym_addr(FKI_SYM_ALLOC_ETHERDEV_MQS) &&
	       fki_sym_addr(FKI_SYM_FREE_NETDEV) &&
	       fki_sym_addr(FKI_SYM_REGISTER_NETDEV) &&
	       fki_sym_addr(FKI_SYM_UNREGISTER_NETDEV) &&
	       fki_sym_addr(FKI_SYM_ETH_MAC_ADDR) &&
	       fki_sym_addr(FKI_SYM_ETH_VALIDATE_ADDR) &&
	       fki_sym_addr(FKI_SYM_ETH_TYPE_TRANS) &&
	       fki_sym_addr(FKI_SYM_NETIF_RX) &&
	       fki_sym_addr(FKI_SYM_NETDEV_ALLOC_SKB) &&
	       fki_sym_addr(FKI_SYM_SKB_PUT) &&
	       fki_sym_addr(FKI_SYM_SKB_COPY_BITS) &&
	       fki_sym_addr(FKI_SYM_SKB_DEQUEUE) &&
	       fki_sym_addr(FKI_SYM_SKB_QUEUE_TAIL) &&
	       (fki_sym_addr(FKI_SYM_DEV_KFREE_SKB_REASON) ||
		fki_sym_addr(FKI_SYM_DEV_KFREE_SKB_ANY) ||
		fki_sym_addr(FKI_SYM_CONSUME_SKB));
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

#define FKI_FILE_BLOB_MAX (512UL << 20)
#define FKI_FILE_BLOB_SLOTS 8

static DEFINE_SPINLOCK(g_blob_lock);
static void *g_file_blobs[FKI_FILE_BLOB_SLOTS];

static int blob_slot_add(void *buf)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&g_blob_lock, flags);
	for (i = 0; i < FKI_FILE_BLOB_SLOTS; i++) {
		if (!g_file_blobs[i]) {
			g_file_blobs[i] = buf;
			spin_unlock_irqrestore(&g_blob_lock, flags);
			return 0;
		}
	}
	spin_unlock_irqrestore(&g_blob_lock, flags);
	return -ENOSPC;
}

static int blob_slot_del(void *buf)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&g_blob_lock, flags);
	for (i = 0; i < FKI_FILE_BLOB_SLOTS; i++) {
		if (g_file_blobs[i] == buf) {
			g_file_blobs[i] = NULL;
			spin_unlock_irqrestore(&g_blob_lock, flags);
			return 1;
		}
	}
	spin_unlock_irqrestore(&g_blob_lock, flags);
	return 0;
}

static int fki_blob_get_file(const char *path, const void **data,
			     unsigned long *size, void **handle)
{
	fki_u64 fsz = 0;
	unsigned long done = 0;
	void *file;
	void *buf;

	file = fki_file_open(path, 0, &fsz);
	if (!file) {
		return -ENOENT;
	}
	if (!fsz || fsz > FKI_FILE_BLOB_MAX) {
		call_filp_close(file);
		return -EINVAL;
	}
	buf = call_vzalloc((unsigned long)fsz);
	if (!buf) {
		call_filp_close(file);
		return -ENOMEM;
	}
	while (done < (unsigned long)fsz) {
		unsigned long chunk = min((unsigned long)fsz - done,
					  FKI_FILE_CHUNK);
		loff_t pos = done;
		ssize_t n = call_kernel_read(file, (char *)buf + done, chunk,
					     &pos);

		if (n < 0) {
			call_filp_close(file);
			call_vfree(buf);
			return (int)n;
		}
		if (n == 0) {
			break;
		}
		done += (unsigned long)n;
	}
	call_filp_close(file);
	if (done != (unsigned long)fsz) {
		call_vfree(buf);
		return -EIO;
	}
	if (blob_slot_add(buf) < 0) {
		call_vfree(buf);
		return -ENOSPC;
	}
	*data = buf;
	*size = done;
	*handle = buf;
	return 0;
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
	if (name[0] == '/') {
		return fki_blob_get_file(name, data, size, handle);
	}
	if (!fki_layout_ok(FKI_LAY_FW_DATA) || !fki_layout_ok(FKI_LAY_FW_SIZE)) {
		return -ENOTSUPP;
	}
	err = call_request_firmware(&fw, name);
	if (err) {
		return err;
	}
	fw_data = (const u8 *const *)fki_layout_ptr(fw, FKI_LAY_FW_DATA);
	fw_size = (const size_t *)fki_layout_ptr(fw, FKI_LAY_FW_SIZE);
	if (!fw_data || !fw_size) {
		call_release_firmware(fw);
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
	if (!handle) {
		return;
	}
	if (blob_slot_del(handle)) {
		call_vfree(handle);
		return;
	}
	call_release_firmware(handle);
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
			call_filp_close(file);
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
	bounce = call_vzalloc(min(len, FKI_FILE_CHUNK));
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
	call_vfree(bounce);
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
		call_filp_close(file);
	}
}
EXPORT_SYMBOL(fki_file_close);

static netdev_tx_t fki_net_xmit(struct sk_buff *skb, struct net_device *dev)
{
	if (skb_queue_len(&g_net_rxq) >= FKI_NET_QLEN) {
		netdev_stat_inc(dev, FKI_LAY_NETSTATS_TX_DROP, 1);
		call_kfree_skb_any(skb, true);
		return NETDEV_TX_OK;
	}
	netdev_stat_inc(dev, FKI_LAY_NETSTATS_TX_PKTS, 1);
	netdev_stat_inc(dev, FKI_LAY_NETSTATS_TX_BYTES, skb->len);
	call_skb_queue_tail(&g_net_rxq, skb);
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

static void fki_net_send_body(void *ptr)
{
	struct fki_net_io *io = ptr;
	struct sk_buff *skb;
	void *tail;

	io->result = -ENODEV;
	if (!g_netdev || io->len < ETH_HLEN || io->len > ETH_FRAME_LEN + 4) {
		return;
	}
	skb = call_netdev_alloc_skb(g_netdev, io->len + NET_IP_ALIGN,
				    GFP_ATOMIC);
	if (!skb) {
		netdev_stat_inc(g_netdev, FKI_LAY_NETSTATS_RX_DROP, 1);
		io->result = -ENOMEM;
		return;
	}
	skb_reserve(skb, NET_IP_ALIGN);
	tail = call_skb_put(skb, io->len);
	if (!tail) {
		call_kfree_skb_any(skb, true);
		io->result = -ENOMEM;
		return;
	}
	memcpy(tail, io->in, io->len);
	skb->protocol = call_eth_type_trans(skb, g_netdev);
	netdev_stat_inc(g_netdev, FKI_LAY_NETSTATS_RX_PKTS, 1);
	netdev_stat_inc(g_netdev, FKI_LAY_NETSTATS_RX_BYTES, io->len);
	local_irq_enable();
	call_netif_rx(skb);
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

static void fki_net_recv_body(void *ptr)
{
	struct fki_net_io *io = ptr;
	struct sk_buff *skb = call_skb_dequeue(&g_net_rxq);

	io->result = 0;
	if (!skb) {
		return;
	}
	if (skb->len > io->len) {
		io->result = -EMSGSIZE;
	} else if (call_skb_copy_bits(skb, 0, io->out, skb->len) == 0) {
		io->result = skb->len;
	} else {
		io->result = -EIO;
	}
	call_kfree_skb_any(skb, false);
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

static void netdev_set_random_addr(struct net_device *dev)
{
	u8 addr[ETH_ALEN];
	u8 **dev_addr;
	u8 *assign_type;

	fki_random(addr, sizeof(addr));
	addr[0] &= 0xfe;
	addr[0] |= 0x02;
	if (fki_sym_addr(FKI_SYM_DEV_ADDR_MOD)) {
		call_dev_addr_mod(dev, addr, ETH_ALEN);
	} else {
		dev_addr = (u8 **)fki_layout_ptr(dev, FKI_LAY_NETDEV_DEV_ADDR);
		if (dev_addr && *dev_addr) {
			memcpy(*dev_addr, addr, ETH_ALEN);
		}
	}
	assign_type = (u8 *)fki_layout_ptr(dev, FKI_LAY_NETDEV_ADDR_ASSIGN);
	if (assign_type) {
		*assign_type = NET_ADDR_RANDOM;
	}
}

int fki_io_init(void)
{
	struct net_device *dev;
	char *name;
	const struct net_device_ops **ops;
	unsigned int *min_mtu;
	unsigned int *max_mtu;
	int err;

	skb_queue_head_init(&g_net_rxq);
	if (!netdev_layout_ok() || !netdev_syms_ok()) {
		pr_warn("[fki] xnu0 disabled: net_device support unavailable\n");
		return 0;
	}
	fki_net_ops.ndo_open = fki_net_up;
	fki_net_ops.ndo_stop = fki_net_down;
	fki_net_ops.ndo_start_xmit = fki_net_xmit;
	fki_net_ops.ndo_set_mac_address =
		(void *)fki_sym_addr(FKI_SYM_ETH_MAC_ADDR);
	fki_net_ops.ndo_validate_addr =
		(void *)fki_sym_addr(FKI_SYM_ETH_VALIDATE_ADDR);

	dev = call_alloc_etherdev();
	if (!dev) {
		return 0;
	}
	name = (char *)fki_layout_ptr(dev, FKI_LAY_NETDEV_NAME);
	ops = (const struct net_device_ops **)fki_layout_ptr(dev,
							     FKI_LAY_NETDEV_OPS);
	min_mtu = (unsigned int *)fki_layout_ptr(dev, FKI_LAY_NETDEV_MIN_MTU);
	max_mtu = (unsigned int *)fki_layout_ptr(dev, FKI_LAY_NETDEV_MAX_MTU);
	memcpy(name, "xnu0", 5);
	*ops = &fki_net_ops;
	netdev_set_random_addr(dev);
	*min_mtu = 68;
	*max_mtu = ETH_DATA_LEN;
	err = call_register_netdev(dev);
	if (err) {
		call_free_netdev(dev);
		pr_warn("[fki] no xnu0 link (%d)\n", err);
		return 0;
	}
	g_netdev = dev;
	return 0;
}

void fki_io_exit(void)
{
	struct sk_buff *skb;

	if (g_netdev) {
		call_unregister_netdev(g_netdev);
		call_free_netdev(g_netdev);
		g_netdev = NULL;
	}
	while ((skb = call_skb_dequeue(&g_net_rxq)) != NULL) {
		call_kfree_skb_any(skb, false);
	}
}
