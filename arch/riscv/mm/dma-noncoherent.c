// SPDX-License-Identifier: GPL-2.0-only
/*
 * RISC-V specific functions to support DMA for non-coherent devices
 *
 * Copyright (c) 2021 Western Digital Corporation or its affiliates.
 */

#include <linux/dma-direct.h>
#include <linux/dma-map-ops.h>
#include <linux/mm.h>
#include <asm/cacheflush.h>
#include <asm/dma-noncoherent.h>

static bool noncoherent_supported;

#define RISCV_CBOM_ZICBOM_CMO_OP(_op, _start, _size, _cachesize)		\
	asm volatile("mv a0, %1\n\t"						\
		     "j 2f\n\t"							\
		     "3:\n\t"							\
		     CBO_##_op(a0)						\
		     "add a0, a0, %0\n\t"					\
		     "2:\n\t"							\
		     "bltu a0, %2, 3b\n\t"					\
		     : : "r"(_cachesize),					\
			 "r"((unsigned long)(_start) & ~((_cachesize) - 1UL)),	\
			 "r"((unsigned long)(_start) + (_size))			\
		     : "a0")

static void riscv_cbom_zicbom_cmo_wback(void *vaddr, unsigned long size)
{
	unsigned long addr = (unsigned long)vaddr;

	RISCV_CBOM_ZICBOM_CMO_OP(clean, addr, size, riscv_cbom_block_size);
}

static void riscv_cbom_zicbom_cmo_inv(void *vaddr, unsigned long size)
{
	unsigned long addr = (unsigned long)vaddr;

	RISCV_CBOM_ZICBOM_CMO_OP(inval, addr, size, riscv_cbom_block_size);
}

static void riscv_cbom_zicbom_cmo_wback_inv(void *vaddr, unsigned long size)
{
	unsigned long addr = (unsigned long)vaddr;

	RISCV_CBOM_ZICBOM_CMO_OP(flush, addr, size, riscv_cbom_block_size);
}

/* Default the CMO ops to ZICBOM */
struct riscv_cache_ops noncoherent_cache_ops = {
	.dma_cache_wback = &riscv_cbom_zicbom_cmo_wback,
	.dma_cache_inv = &riscv_cbom_zicbom_cmo_inv,
	.dma_cache_wback_inv = &riscv_cbom_zicbom_cmo_wback_inv,
};
EXPORT_SYMBOL_GPL(noncoherent_cache_ops);

static inline void arch_dma_cache_wback(phys_addr_t paddr, size_t size)
{
	void *vaddr = phys_to_virt(paddr);

	riscv_dma_noncoherent_cache_wback(vaddr, size);
}

static inline void arch_dma_cache_inv(phys_addr_t paddr, size_t size)
{
	void *vaddr = phys_to_virt(paddr);

	riscv_dma_noncoherent_cache_inv(vaddr, size);
}

static inline void arch_dma_cache_wback_inv(phys_addr_t paddr, size_t size)
{
	void *vaddr = phys_to_virt(paddr);

	riscv_dma_noncoherent_wback_inv(vaddr, size);
}

static inline bool arch_sync_dma_clean_before_fromdevice(void)
{
	return true;
}

static inline bool arch_sync_dma_cpu_needs_post_dma_flush(void)
{
	return true;
}

#include <linux/dma-sync.h>


void arch_dma_prep_coherent(struct page *page, size_t size)
{
	void *flush_addr = page_address(page);

	riscv_dma_noncoherent_wback_inv(flush_addr, size);
}

void arch_setup_dma_ops(struct device *dev, u64 dma_base, u64 size,
		const struct iommu_ops *iommu, bool coherent)
{
	WARN_TAINT(!coherent && riscv_cbom_block_size > ARCH_DMA_MINALIGN,
		   TAINT_CPU_OUT_OF_SPEC,
		   "%s %s: ARCH_DMA_MINALIGN smaller than riscv,cbom-block-size (%d < %d)",
		   dev_driver_string(dev), dev_name(dev),
		   ARCH_DMA_MINALIGN, riscv_cbom_block_size);

	WARN_TAINT(!coherent && !noncoherent_supported, TAINT_CPU_OUT_OF_SPEC,
		   "%s %s: device non-coherent but no non-coherent operations supported",
		   dev_driver_string(dev), dev_name(dev));

	dev->dma_coherent = coherent;
}

void riscv_noncoherent_supported(void)
{
	WARN(!riscv_cbom_block_size,
	     "Non-coherent DMA support enabled without a block size\n");
	noncoherent_supported = true;
}

void riscv_noncoherent_register_cache_ops(const struct riscv_cache_ops *ops)
{
	noncoherent_cache_ops = *ops;
}
EXPORT_SYMBOL_GPL(riscv_noncoherent_register_cache_ops);
