/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2023 Renesas Electronics Corp.
 */

#ifndef __ASM_DMA_NONCOHERENT_H
#define __ASM_DMA_NONCOHERENT_H

#include <linux/dma-direct.h>

#ifdef CONFIG_RISCV_DMA_NONCOHERENT

/*
 * struct riscv_cache_ops - Structure for CMO function pointers
 *
 * @dma_cache_wback: Function pointer for clean cache
 * @dma_cache_inv: Function pointer for invalidate cache
 * @dma_cache_wback_inv: Function pointer for flushing the cache
 */
struct riscv_cache_ops {
	void (*dma_cache_wback)(void *vaddr, unsigned long size);
	void (*dma_cache_inv)(void *vaddr, unsigned long size);
	void (*dma_cache_wback_inv)(void *vaddr, unsigned long size);
};

extern struct riscv_cache_ops noncoherent_cache_ops;

void riscv_noncoherent_register_cache_ops(const struct riscv_cache_ops *ops);

static inline void riscv_dma_noncoherent_cache_wback(void *vaddr, size_t size)
{
	noncoherent_cache_ops.dma_cache_wback(vaddr, size);
}

static inline void riscv_dma_noncoherent_cache_inv(void *vaddr, size_t size)
{
	noncoherent_cache_ops.dma_cache_inv(vaddr, size);
}

static inline void riscv_dma_noncoherent_wback_inv(void *vaddr, size_t size)
{
	noncoherent_cache_ops.dma_cache_wback_inv(vaddr, size);
}

static inline void riscv_dma_noncoherent_pmem_clean(void *vaddr, size_t size)
{
	riscv_dma_noncoherent_cache_wback(vaddr, size);
}

static inline void riscv_dma_noncoherent_pmem_inval(void *vaddr, size_t size)
{
	riscv_dma_noncoherent_cache_inv(vaddr, size);
}
#else

static inline void riscv_dma_noncoherent_pmem_clean(void *vaddr, size_t size) {}
static inline void riscv_dma_noncoherent_pmem_inval(void *vaddr, size_t size) {}

#endif /* CONFIG_RISCV_DMA_NONCOHERENT */

#endif	/* __ASM_DMA_NONCOHERENT_H */
