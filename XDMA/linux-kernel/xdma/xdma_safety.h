/* SPDX-License-Identifier: GPL-2.0 */
#ifndef XDMA_SAFETY_H
#define XDMA_SAFETY_H

/* Subtraction is safe only after checking offset against length. */
static inline int xdma_range_valid(unsigned long long length,
		unsigned long long offset, unsigned long long size)
{
	return offset <= length && size <= length - offset;
}

/* Bound allocation and CPU/MMIO work for a single XVC ioctl to 1 MiB. */
#define XDMA_XVC_MAX_BITS (8U * 1024U * 1024U)
static inline unsigned int xdma_xvc_bytes(unsigned int bits)
{
	if (!bits || bits > XDMA_XVC_MAX_BITS)
		return 0;
	return bits / 8 + !!(bits % 8);
}

#endif
