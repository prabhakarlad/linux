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

struct riscv_cache_ops noncoherent_cache_ops = {
	.clean_range = NULL,
	.inv_range = NULL,
	.flush_range = NULL,
	.cmo_universal = NULL,
};
EXPORT_SYMBOL_GPL(noncoherent_cache_ops);

void arch_sync_dma_for_device(phys_addr_t paddr, size_t size,
			      enum dma_data_direction dir)
{
	void *vaddr = phys_to_virt(paddr);

	if (noncoherent_cache_ops.cmo_universal) {
		noncoherent_cache_ops.cmo_universal(vaddr, size, dir,
						    NON_COHERENT_SYNC_DMA_FOR_DEVICE);
		return;
	}

	switch (dir) {
	case DMA_TO_DEVICE:
		riscv_dma_noncoherent_clean(vaddr, size);
		break;
	case DMA_FROM_DEVICE:
		riscv_dma_noncoherent_clean(vaddr, size);
		break;
	case DMA_BIDIRECTIONAL:
		riscv_dma_noncoherent_flush(vaddr, size);
		break;
	default:
		break;
	}
}

void arch_sync_dma_for_cpu(phys_addr_t paddr, size_t size,
			   enum dma_data_direction dir)
{
	void *vaddr = phys_to_virt(paddr);

	if (noncoherent_cache_ops.cmo_universal) {
		noncoherent_cache_ops.cmo_universal(vaddr, size, dir,
						    NON_COHERENT_SYNC_DMA_FOR_CPU);
		return;
	}

	switch (dir) {
	case DMA_TO_DEVICE:
		break;
	case DMA_FROM_DEVICE:
	case DMA_BIDIRECTIONAL:
		riscv_dma_noncoherent_flush(vaddr, size);
		break;
	default:
		break;
	}
}

void arch_dma_prep_coherent(struct page *page, size_t size)
{
	void *flush_addr = page_address(page);

	if (noncoherent_cache_ops.cmo_universal) {
		noncoherent_cache_ops.cmo_universal(flush_addr, size, -1,
						    NON_COHERENT_DMA_PREP);
		return;
	}

	riscv_dma_noncoherent_flush(flush_addr, size);
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
	if (!ops)
		return;

	noncoherent_cache_ops = *ops;
}
EXPORT_SYMBOL_GPL(riscv_noncoherent_register_cache_ops);
