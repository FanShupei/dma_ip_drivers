/* SPDX-License-Identifier: GPL-2.0 */
/* Hardware-free fault injection for the actual cdev_sgdma request routines. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/uio.h>
#include "xdma_safety.h"

typedef uint64_t u64;
#define __user
#define GFP_KERNEL 0
#define DMA_TO_DEVICE 1
#define DMA_FROM_DEVICE 2
#define IOCB_NOWAIT 1
#define EIOCBQUEUED 529
#define KERNEL_VERSION(a, b, c) (((a) << 16) | ((b) << 8) | (c))
#define LINUX_VERSION_CODE KERNEL_VERSION(7, 2, 0)
#define READ_ONCE(x) (x)
#define min(a, b) ((a) < (b) ? (a) : (b))
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
struct work_struct { void (*fn)(struct work_struct *); };
#define INIT_WORK(w, f) ((w)->fn = (f))
struct file { void *private_data; };
struct kiocb {
	struct file *ki_filp;
	loff_t ki_pos;
	unsigned int ki_flags;
	void (*ki_complete)(struct kiocb *, ssize_t);
};
struct xdma_engine { int channel, dir; bool non_incr_addr; };
struct xdma_pci_dev { bool removing; int io_lock, io_wq; };
struct xdma_cdev {
	struct xdma_engine *engine;
	struct xdma_pci_dev *xpdev;
	void *xdev;
};
struct sg_table { size_t len; };
struct xdma_io_cb {
	void *buf;
	size_t len;
	loff_t ep_addr;
	bool write;
	struct sg_table sgt;
	void *pages;
};
struct iov_iter {
	const struct iovec *iov;
	void *ubuf;
	size_t count, iov_offset;
	unsigned long nr_segs;
	bool is_ubuf, kernel;
};
static size_t iov_iter_count(const struct iov_iter *i) { return i->count; }
static bool user_backed_iter(const struct iov_iter *i) { return !i->kernel; }
static bool iter_is_ubuf(const struct iov_iter *i) { return i->is_ubuf; }
static const struct iovec *iter_iov(const struct iov_iter *i) { return i->iov; }
static void iov_iter_advance(struct iov_iter *i, size_t n)
{
	assert(n <= i->count);
	i->count -= n;
	i->iov_offset += n;
	if (i->is_ubuf)
		return;
	while (i->nr_segs && i->iov_offset >= i->iov->iov_len) {
		i->iov_offset -= i->iov->iov_len;
		i->iov++;
		i->nr_segs--;
	}
}
static bool is_sync_kiocb(const struct kiocb *i) { return !i->ki_complete; }
static void down_read(int *lock) { assert(!*lock); *lock = 1; }
static void up_read(int *lock) { assert(*lock); *lock = 0; }
static unsigned int h2c_timeout = 10, c2h_timeout = 10;
static unsigned int allocations, alloc_calls, fail_alloc, map_calls, fail_map;
static unsigned int dma_calls, fail_dma, completions;
static ssize_t short_dma, completion_result;
static loff_t addresses[16];
static size_t lengths[16];
static struct work_struct *queued;

static void *kcalloc(size_t nr, size_t size, int flags)
{
	void *p;

	if (++alloc_calls == fail_alloc)
		return NULL;
	p = calloc(nr, size);
	assert(p);
	allocations++;
	return p;
}
static void *kzalloc(size_t size, int flags) { return kcalloc(1, size, flags); }
static void kfree(void *p) { if (p) { assert(allocations); allocations--; free(p); } }
static int queue_work(int wq, struct work_struct *work)
{
	assert(!queued);
	queued = work;
	return 1;
}
static int xcdev_check(const char *name, struct xdma_cdev *dev, int engine)
{
	return dev && dev->engine ? 0 : -EINVAL;
}
static int check_transfer_align(struct xdma_engine *e, const void *buf,
		size_t count, loff_t pos, int sync)
{
	return ((uintptr_t)buf & 3) == ((unsigned long long)pos & 3) ? 0 : -EINVAL;
}
static int char_sgdma_map_user_buf_to_sgl(struct xdma_io_cb *cb, bool write)
{
	if (++map_calls == fail_map)
		return -EFAULT;
	cb->pages = kzalloc(1, 0);
	if (!cb->pages)
		return -ENOMEM;
	cb->sgt.len = cb->len;
	return 0;
}
static void char_sgdma_unmap_user_buf(struct xdma_io_cb *cb, bool write)
{
	kfree(cb->pages);
	cb->pages = NULL;
}
static ssize_t xdma_xfer_submit(void *dev, int channel, bool write,
		loff_t addr, struct sg_table *sg, int mapped, int timeout)
{
	assert(dma_calls < 16);
	addresses[dma_calls] = addr;
	lengths[dma_calls++] = sg->len;
	if (dma_calls == fail_dma)
		return -EIO;
	return short_dma >= 0 ? short_dma : (ssize_t)sg->len;
}
#include "sgdma_under_test.inc"

static struct xdma_engine engine;
static struct xdma_pci_dev device;
static struct xdma_cdev cdev;
static struct file file;
static struct kiocb iocb;
static char data[32] __attribute__((aligned(16)));
static struct iovec vectors[3];
static struct iov_iter iter;

static void complete(struct kiocb *i, ssize_t rv)
{
	assert(!allocations && !device.io_lock);
	completion_result = rv;
	completions++;
}
static void setup(bool async)
{
	assert(!allocations && !queued);
	alloc_calls = map_calls = dma_calls = completions = 0;
	fail_alloc = fail_map = fail_dma = 0;
	short_dma = -1;
	device = (struct xdma_pci_dev){0};
	engine = (struct xdma_engine){.dir = DMA_FROM_DEVICE};
	cdev = (struct xdma_cdev){.engine = &engine, .xpdev = &device};
	file.private_data = &cdev;
	iocb = (struct kiocb){.ki_filp = &file, .ki_pos = 0x90000000,
		.ki_complete = async ? complete : NULL};
	vectors[0] = (struct iovec){data, 4};
	vectors[1] = (struct iovec){data + 4, 8};
	vectors[2] = (struct iovec){data + 12, 4};
	iter = (struct iov_iter){.iov = vectors, .nr_segs = 3, .count = 16};
}
static void run_work(void)
{
	struct work_struct *work = queued;

	assert(work);
	queued = NULL;
	work->fn(work);
}
int main(void)
{
	unsigned int i;

	setup(false);
	assert(cdev_read_iter(&iocb, &iter) == 16);
	assert(!queued && !completions && !allocations && !iter.count);
	assert(iocb.ki_pos == 0x90000010);
	assert(addresses[0] == 0x90000000 && addresses[1] == 0x90000004);
	assert(addresses[2] == 0x9000000c);
	puts("PASS synchronous multi-vector I/O and endpoint positions");

	setup(true);
	assert(cdev_read_iter(&iocb, &iter) == -EIOCBQUEUED);
	assert(!dma_calls && !completions);
	run_work();
	assert(completions == 1 && completion_result == 16 && !allocations);
	puts("PASS async completion exactly once after buffer cleanup");

	setup(true);
	vectors[1].iov_base = data + 5;
	assert(cdev_read_iter(&iocb, &iter) == -EINVAL);
	assert(!dma_calls && !queued && !allocations && iter.count == 16);
	setup(true);
	fail_map = 2;
	assert(cdev_read_iter(&iocb, &iter) == -EFAULT);
	assert(!dma_calls && !queued && !allocations);
	puts("PASS later-vector validation/pinning failures unwind before DMA");

	for (i = 1; i <= 5; i++) {
		setup(true);
		fail_alloc = i;
		assert(cdev_read_iter(&iocb, &iter) == -ENOMEM);
		assert(!queued && !allocations && !dma_calls);
	}
	puts("PASS every request/vector allocation failure");

	setup(true);
	fail_dma = 1;
	assert(cdev_read_iter(&iocb, &iter) == -EIOCBQUEUED);
	run_work();
	assert(completions == 1 && completion_result == -EIO);
	setup(true);
	fail_dma = 2;
	assert(cdev_read_iter(&iocb, &iter) == -EIOCBQUEUED);
	run_work();
	assert(completion_result == 4 && iocb.ki_pos == 0x90000004);
	puts("PASS DMA failure propagation and partial completion");

	setup(false);
	short_dma = 2;
	assert(cdev_read_iter(&iocb, &iter) == 2);
	assert(dma_calls == 1 && iter.count == 14 && iter.iov_offset == 2);
	puts("PASS short synchronous transfer advances only completed bytes");

	setup(true);
	assert(cdev_read_iter(&iocb, &iter) == -EIOCBQUEUED);
	device.removing = true;
	run_work();
	assert(completions == 1 && completion_result == -ENODEV && !dma_calls);
	puts("PASS queued AIO during removal releases buffers without DMA");

	setup(false);
	engine.dir = DMA_TO_DEVICE;
	engine.non_incr_addr = true;
	assert(cdev_write_iter(&iocb, &iter) == 16);
	assert(addresses[0] == addresses[1] && addresses[1] == addresses[2]);
	assert(iocb.ki_pos == 0x90000000);
	puts("PASS fixed-address writes preserve endpoint address");

	setup(false);
	vectors[0].iov_len = 8;
	iter.iov_offset = 4;
	assert(cdev_read_iter(&iocb, &iter) == 16);
	assert(lengths[0] == 4);
	setup(false);
	vectors[0].iov_len = 0;
	iter.count = 12;
	assert(cdev_read_iter(&iocb, &iter) == 12 && dma_calls == 2);
	setup(false);
	iter.count = 0;
	assert(cdev_read_iter(&iocb, &iter) == 0 && !alloc_calls);
	puts("PASS iterator offsets and empty vectors");

	setup(false);
	iter.is_ubuf = true;
	iter.ubuf = data;
	iter.nr_segs = 0;
	assert(cdev_read_iter(&iocb, &iter) == 16 && dma_calls == 1);
	setup(false);
	iter.kernel = true;
	assert(cdev_read_iter(&iocb, &iter) == -EOPNOTSUPP);
	setup(false);
	iocb.ki_flags = IOCB_NOWAIT;
	assert(cdev_read_iter(&iocb, &iter) == -EOPNOTSUPP);
	puts("PASS UBUF support and unsupported iterator/NOWAIT rejection");

	setup(false);
	iocb.ki_pos = LLONG_MAX - 3;
	assert(cdev_read_iter(&iocb, &iter) == -EINVAL && !allocations);
	assert(xdma_range_valid(65536, 65532, 4));
	assert(!xdma_range_valid(65536, 65536, 4));
	assert(!xdma_range_valid(65536, 131072, 4096));
	assert(!xdma_range_valid(65536, ULLONG_MAX, 4));
	assert(!xdma_range_valid(65536, 1, ULLONG_MAX));
	assert(xdma_xvc_bytes(1) == 1 && xdma_xvc_bytes(9) == 2);
	assert(xdma_xvc_bytes(XDMA_XVC_MAX_BITS) == 1024 * 1024);
	assert(!xdma_xvc_bytes(0) && !xdma_xvc_bytes(XDMA_XVC_MAX_BITS + 1));
	assert(!xdma_xvc_bytes(UINT_MAX) && !xdma_xvc_bytes(UINT_MAX - 6));
	puts("PASS endpoint overflow, BAR underflow, and XVC length boundaries");
	return 0;
}
