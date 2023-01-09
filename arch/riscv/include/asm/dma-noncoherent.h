/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2022 Renesas Electronics Corp.
 */

#ifndef __ASM_DMA_NONCOHERENT_H
#define __ASM_DMA_NONCOHERENT_H

#include <linux/dma-direct.h>

#ifdef CONFIG_RISCV_DMA_NONCOHERENT

enum dma_noncoherent_ops {
	NON_COHERENT_SYNC_DMA_FOR_DEVICE = 0,
	NON_COHERENT_SYNC_DMA_FOR_CPU,
	NON_COHERENT_DMA_PREP,
	NON_COHERENT_DMA_PMEM,
};

/*
 * struct riscv_cache_ops - Structure for CMO function pointers
 * @clean_range: Function pointer for clean cache
 * @inv_range: Function pointer for invalidate cache
 * @flush_range: Function pointer for flushing the cache
 * @cmo_universal: Function pointer for platforms who want
 *  to handle CMO themselves. If this function pointer is set rest of the
 *  function pointers will be NULL.
 */
struct riscv_cache_ops {
	void (*clean_range)(unsigned long addr, unsigned long size);
	void (*inv_range)(unsigned long addr, unsigned long size);
	void (*flush_range)(unsigned long addr, unsigned long size);
	void (*cmo_universal)(void *vaddr, size_t size,
			      enum dma_data_direction dir,
		              enum dma_noncoherent_ops ops);
};

extern struct riscv_cache_ops noncoherent_cache_ops;

void riscv_noncoherent_register_cache_ops(const struct riscv_cache_ops *ops);

static inline void riscv_dma_noncoherent_clean(void *vaddr, size_t size)
{
	if (noncoherent_cache_ops.clean_range) {
		unsigned long addr = (unsigned long)vaddr;

		noncoherent_cache_ops.clean_range(addr, size);
	}
}

static inline void riscv_dma_noncoherent_flush(void *vaddr, size_t size)
{
	if (noncoherent_cache_ops.flush_range) {
		unsigned long addr = (unsigned long)vaddr;

		noncoherent_cache_ops.flush_range(addr, size);
	}
}

static inline void riscv_dma_noncoherent_inval(void *vaddr, size_t size)
{
	if (noncoherent_cache_ops.inv_range) {
		unsigned long addr = (unsigned long)vaddr;

		noncoherent_cache_ops.inv_range(addr, size);
	}
}

static inline void riscv_dma_noncoherent_pmem_clean(void *vaddr, size_t size)
{
	if (noncoherent_cache_ops.cmo_universal) {
		noncoherent_cache_ops.cmo_universal(vaddr, size, -1,
						    NON_COHERENT_DMA_PMEM);
		return;
	}

	riscv_dma_noncoherent_clean(vaddr, size);
}

static inline void riscv_dma_noncoherent_pmem_inval(void *vaddr, size_t size)
{
	if (noncoherent_cache_ops.cmo_universal) {
		noncoherent_cache_ops.cmo_universal(vaddr, size, -1,
						    NON_COHERENT_DMA_PMEM);
		return;
	}

	riscv_dma_noncoherent_inval(vaddr, size);
}
#else

static inline void riscv_dma_noncoherent_pmem_clean(void *vaddr, size_t size) {}
static inline void riscv_dma_noncoherent_pmem_inval(void *vaddr, size_t size) {}

#endif /* CONFIG_RISCV_DMA_NONCOHERENT */

#endif	/* __ASM_DMA_NONCOHERENT_H */
