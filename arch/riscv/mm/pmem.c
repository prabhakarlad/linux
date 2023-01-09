// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Ventana Micro Systems Inc.
 */

#include <linux/export.h>
#include <linux/libnvdimm.h>

#include <asm/dma-noncoherent.h>

void arch_wb_cache_pmem(void *addr, size_t size)
{
	riscv_dma_noncoherent_pmem_clean(addr, size);
}
EXPORT_SYMBOL_GPL(arch_wb_cache_pmem);

void arch_invalidate_pmem(void *addr, size_t size)
{
	riscv_dma_noncoherent_pmem_inval(addr, size);
}
EXPORT_SYMBOL_GPL(arch_invalidate_pmem);
