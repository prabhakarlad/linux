/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2023 Renesas Electronics Corp.
 */

#ifndef __ASM_DMA_NONCOHERENT_H
#define __ASM_DMA_NONCOHERENT_H

#include <linux/dma-direct.h>

/*
 * struct riscv_cache_ops - Structure for CMO function pointers
 *
 * @clean: Function pointer for clean cache
 * @inval: Function pointer for invalidate cache
 * @flush: Function pointer for flushing the cache
 */
struct riscv_cache_ops {
	void (*clean)(phys_addr_t paddr, unsigned long size);
	void (*inval)(phys_addr_t paddr, unsigned long size);
	void (*flush)(phys_addr_t paddr, unsigned long size);
};

extern struct riscv_cache_ops noncoherent_cache_ops;

void riscv_noncoherent_register_cache_ops(const struct riscv_cache_ops *ops);

#endif	/* __ASM_DMA_NONCOHERENT_H */
