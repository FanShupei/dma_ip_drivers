/* SPDX-License-Identifier: GPL-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

typedef uint32_t u32;
typedef uint64_t u64;
#define KERNEL_VERSION(a, b, c) (((a) << 16) | ((b) << 8) | (c))
#define LINUX_VERSION_CODE KERNEL_VERSION(7, 2, 0)
#define HAS_SWAKE_UP 0
#define XDMA_DESC_STOPPED 1
#define XDMA_DESC_EOP 2
#define XDMA_DESC_COMPLETED 4
#define XFER_FLAG_NEED_UNMAP 1
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define spin_lock_irqsave(lock, flags) do { *(lock) = 1; (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { *(lock) = 0; (void)(flags); } while (0)
#define init_waitqueue_head(q) (*(q) = 1)
#define dbg_sg(...)
#define dbg_desc(...)
struct xdma_desc { u32 next_lo, control, value; };
struct xdma_result { u32 value; };
struct sg_table { void *sgl; unsigned int nents, orig_nents; };
struct xdma_request_cb { unsigned int sw_desc_cnt, sw_desc_idx; };
struct xdma_transfer {
	int wq, dir;
	struct xdma_desc *desc_virt;
	struct xdma_result *res_virt;
	u64 desc_bus, res_bus;
	int desc_index, desc_adjacent, desc_num, desc_cmpl_th;
	bool last_in_request;
	unsigned int flags;
	struct sg_table *sgt;
};
struct xdma_engine {
	u32 desc_max;
	int desc_idx, desc_used;
	int lock, dir, eop_flush;
	struct xdma_desc *desc;
	struct xdma_result *cyclic_result;
	u64 desc_bus, cyclic_result_bus;
};
struct pci_dev { int dev; };
struct xdma_dev { struct pci_dev *pdev; };
static void transfer_desc_init(struct xdma_transfer *t, unsigned int n)
{
	memset(t->desc_virt, 0, sizeof(*t->desc_virt) * n);
}
static void transfer_build(struct xdma_engine *e, struct xdma_request_cb *r,
		struct xdma_transfer *t, unsigned int n)
{
	unsigned int i;

	assert(n <= r->sw_desc_cnt - r->sw_desc_idx);
	for (i = 0; i < n; i++)
		t->desc_virt[i].value = ++r->sw_desc_idx;
}
static void xdma_desc_control_set(struct xdma_desc *d, u32 c) { d->control = c; }
static u32 xdma_get_next_adj(unsigned int n, u32 low) { return n; }
static void xdma_desc_adjacent(struct xdma_desc *d, u32 adj) { }
static void xdma_desc_done(struct xdma_desc *d, unsigned int n) { }
static unsigned int unmaps, unmapped_nents;
static void dma_unmap_sg(int *dev, void *sgl, unsigned int n, int dir)
{
	unmaps++;
	unmapped_nents = n;
}
#include "descriptors_under_test.inc"

int main(void)
{
	struct xdma_desc descriptors[8], saved[8];
	struct xdma_result results[8];
	struct xdma_engine e = {.desc_max = 8, .desc = descriptors,
		.cyclic_result = results};
	struct xdma_request_cb req = {.sw_desc_cnt = 25};
	struct xdma_transfer transfer;
	unsigned int rounds = 0;
	struct pci_dev pci = {0};
	struct xdma_dev dev = {.pdev = &pci};
	struct sg_table sg = {.nents = 2, .orig_nents = 9};

	memset(descriptors, 0x55, sizeof(descriptors));
	memcpy(saved, descriptors, sizeof(saved));
	e.desc_used = 8;
	assert(transfer_init(&e, &req, &transfer) == -EBUSY);
	assert(!memcmp(saved, descriptors, sizeof(saved)) && !e.lock);
	assert(!req.sw_desc_idx && e.desc_used == 8);
	puts("PASS full descriptor ring rejects reuse without modifying descriptors");

	e.desc_used = 6;
	assert(!transfer_init(&e, &req, &transfer));
	assert(transfer.desc_num == 2 && e.desc_used == 8);
	assert(!memcmp(saved + 2, descriptors + 2, sizeof(saved) - 2 * sizeof(*saved)));
	puts("PASS allocation is bounded by available descriptor slots");

	e.desc_used = 0;
	e.desc_idx = 7;
	req.sw_desc_idx = 0;
	while (req.sw_desc_idx < req.sw_desc_cnt) {
		assert(!transfer_init(&e, &req, &transfer));
		assert(transfer.desc_index + transfer.desc_num <= 8);
		assert(e.desc_used <= 8);
		e.desc_used -= transfer.desc_num;
		rounds++;
	}
	assert(rounds == 4 && req.sw_desc_idx == 25 && !e.desc_used);
	puts("PASS a request spanning more than two chunks reuses one transfer safely");

	transfer.sgt = &sg;
	transfer.flags = XFER_FLAG_NEED_UNMAP;
	transfer.last_in_request = true;
	transfer_destroy(&dev, &transfer);
	assert(unmaps == 1 && unmapped_nents == 9 && !sg.nents);
	transfer_destroy(&dev, &transfer);
	assert(unmaps == 1);
	sg.nents = 2;
	transfer.flags = 0;
	transfer_destroy(&dev, &transfer);
	assert(unmaps == 1 && sg.nents == 2);
	puts("PASS merged scatterlists unmap once using original entry count");
	return 0;
}
