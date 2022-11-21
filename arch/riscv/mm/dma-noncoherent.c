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

static bool noncoherent_supported;

enum dma_coherent_ops {
	NON_COHERENT_SYNC_DMA_FOR_DEVICE = 0,
	NON_COHERENT_SYNC_DMA_FOR_CPU,
	NON_COHERENT_DMA_PREP
};

void rzfive_cmo(unsigned int cache_size, void *vaddr, size_t size,
			    int dir, int ops)
{
	if (ops == NON_COHERENT_DMA_PREP)
		return;

	if (ops == NON_COHERENT_SYNC_DMA_FOR_DEVICE) {
		switch (dir) {
		case DMA_FROM_DEVICE:
			ax45mp_cpu_dma_inval_range(vaddr, size);
			break;
		case DMA_TO_DEVICE:
		case DMA_BIDIRECTIONAL:
			ax45mp_cpu_dma_wb_range(vaddr, size);
			break;
		default:
			break;
		}
		return;
	}

	/* op == NON_COHERENT_SYNC_DMA_FOR_CPU */
	if (dir == DMA_BIDIRECTIONAL || dir == DMA_FROM_DEVICE)
		ax45mp_cpu_dma_inval_range(vaddr, size);
}
EXPORT_SYMBOL(rzfive_cmo);

void arch_sync_dma_for_device(phys_addr_t paddr, size_t size,
			      enum dma_data_direction dir)
{
	void *vaddr = phys_to_virt(paddr);

	switch (dir) {
	case DMA_FROM_DEVICE:
	case DMA_TO_DEVICE:
		ALT_CMO_OP(clean, vaddr, size, riscv_cbom_block_size, dir, NON_COHERENT_SYNC_DMA_FOR_DEVICE);
		break;
	case DMA_BIDIRECTIONAL:
		ALT_CMO_OP(flush, vaddr, size, riscv_cbom_block_size, dir, NON_COHERENT_SYNC_DMA_FOR_DEVICE);
		break;
	default:
		break;
	}
}

void arch_sync_dma_for_cpu(phys_addr_t paddr, size_t size,
			   enum dma_data_direction dir)
{
	void *vaddr = phys_to_virt(paddr);

	switch (dir) {
	case DMA_TO_DEVICE:
		break;
	case DMA_FROM_DEVICE:
	case DMA_BIDIRECTIONAL:
		ALT_CMO_OP(flush, vaddr, size, riscv_cbom_block_size, dir, NON_COHERENT_SYNC_DMA_FOR_CPU);
		break;
	default:
		break;
	}
}

void arch_dma_prep_coherent(struct page *page, size_t size)
{
	void *flush_addr = page_address(page);

	ALT_CMO_OP(flush, flush_addr, size, riscv_cbom_block_size, 0, NON_COHERENT_DMA_PREP);
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
