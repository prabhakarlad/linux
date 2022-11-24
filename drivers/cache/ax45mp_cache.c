// SPDX-License-Identifier: GPL-2.0
/*
 * non-coherent cache functions for Andes AX45MP
 *
 * Copyright (C) 2022 Renesas Electronics Corp.
 */

#include <linux/cacheflush.h>
#include <linux/cacheinfo.h>
#include <linux/dma-direction.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>

#include <asm/cacheflush.h>
#include <asm/sbi.h>

/* L2 cache registers */
#define AX45MP_L2C_REG_CTL_OFFSET		0x8

#define AX45MP_L2C_REG_C0_CMD_OFFSET		0x40
#define AX45MP_L2C_REG_C0_ACC_OFFSET		0x48
#define AX45MP_L2C_REG_STATUS_OFFSET		0x80

/* D-cache operation */
#define AX45MP_CCTL_L1D_VA_INVAL		0
#define AX45MP_CCTL_L1D_VA_WB			1

/* L2 CCTL status */
#define AX45MP_CCTL_L2_STATUS_IDLE		0

/* L2 CCTL status cores mask */
#define AX45MP_CCTL_L2_STATUS_C0_MASK		0xf

/* L2 cache operation */
#define AX45MP_CCTL_L2_PA_INVAL			0x8
#define AX45MP_CCTL_L2_PA_WB			0x9

#define AX45MP_L2C_REG_PER_CORE_OFFSET		0x10
#define AX45MP_CCTL_L2_STATUS_PER_CORE_OFFSET	4

#define AX45MP_L2C_REG_CN_CMD_OFFSET(n)	\
	(AX45MP_L2C_REG_C0_CMD_OFFSET + ((n) * AX45MP_L2C_REG_PER_CORE_OFFSET))
#define AX45MP_L2C_REG_CN_ACC_OFFSET(n)	\
	(AX45MP_L2C_REG_C0_ACC_OFFSET + ((n) * AX45MP_L2C_REG_PER_CORE_OFFSET))
#define AX45MP_CCTL_L2_STATUS_CN_MASK(n)	\
	(AX45MP_CCTL_L2_STATUS_C0_MASK << ((n) * AX45MP_CCTL_L2_STATUS_PER_CORE_OFFSET))

#define AX45MP_CCTL_REG_UCCTLBEGINADDR_NUM	0x80b
#define AX45MP_CCTL_REG_UCCTLCOMMAND_NUM	0x80c

#define AX45MP_CACHE_LINE_SIZE			64

struct ax45mp_priv {
	void __iomem *l2c_base;
	u32 ax45mp_cache_line_size;
};

static struct ax45mp_priv *ax45mp_priv;
static DEFINE_STATIC_KEY_FALSE(ax45mp_l2c_configured);

/* L2 Cache operations */
static inline uint32_t ax45mp_cpu_l2c_get_cctl_status(void)
{
	return readl(ax45mp_priv->l2c_base + AX45MP_L2C_REG_STATUS_OFFSET);
}

/*
 * Software trigger CCTL operation (cache maintenance operations) by writing
 * to ucctlcommand and ucctlbeginaddr registers and write-back an L2 cache
 * entry.
 */
static void ax45mp_cpu_dcache_wb_range(void *start, void *end, int line_size)
{
	void __iomem *base = ax45mp_priv->l2c_base;
	int mhartid = smp_processor_id();
	unsigned long pa;

	while (end > start) {
		csr_write(AX45MP_CCTL_REG_UCCTLBEGINADDR_NUM, start);
		csr_write(AX45MP_CCTL_REG_UCCTLCOMMAND_NUM, AX45MP_CCTL_L1D_VA_WB);

		pa = virt_to_phys(start);
		writel(pa, base + AX45MP_L2C_REG_CN_ACC_OFFSET(mhartid));
		writel(AX45MP_CCTL_L2_PA_WB,
		       base + AX45MP_L2C_REG_CN_CMD_OFFSET(mhartid));
		while ((ax45mp_cpu_l2c_get_cctl_status() &
			AX45MP_CCTL_L2_STATUS_CN_MASK(mhartid)) !=
			AX45MP_CCTL_L2_STATUS_IDLE)
			;

		start += line_size;
	}
}

/*
 * Software trigger CCTL operation by writing to ucctlcommand and ucctlbeginaddr
 * registers and invalidate the L2 cache entry.
 */
static void ax45mp_cpu_dcache_inval_range(void *start, void *end, int line_size)
{
	void __iomem *base = ax45mp_priv->l2c_base;
	int mhartid = smp_processor_id();
	unsigned long pa;

	while (end > start) {
		csr_write(AX45MP_CCTL_REG_UCCTLBEGINADDR_NUM, start);
		csr_write(AX45MP_CCTL_REG_UCCTLCOMMAND_NUM, AX45MP_CCTL_L1D_VA_INVAL);

		pa = virt_to_phys(start);
		writel(pa, base + AX45MP_L2C_REG_CN_ACC_OFFSET(mhartid));
		writel(AX45MP_CCTL_L2_PA_INVAL,
		       base + AX45MP_L2C_REG_CN_CMD_OFFSET(mhartid));
		while ((ax45mp_cpu_l2c_get_cctl_status() &
			AX45MP_CCTL_L2_STATUS_CN_MASK(mhartid)) !=
			AX45MP_CCTL_L2_STATUS_IDLE)
			;

		start += line_size;
	}
}

static void ax45mp_cpu_dma_inval_range(void *vaddr, size_t size)
{
	char cache_buf[2][AX45MP_CACHE_LINE_SIZE];
	unsigned long start = (unsigned long)vaddr;
	unsigned long end = start + size;
	unsigned long old_start = start;
	unsigned long old_end = end;
	unsigned long line_size;
	unsigned long flags;

	if (unlikely(start == end))
		return;

	line_size = ax45mp_priv->ax45mp_cache_line_size;

	memset(&cache_buf, 0x0, sizeof(cache_buf));
	start = start & (~(line_size - 1));
	end = ((end + line_size - 1) & (~(line_size - 1)));

	local_irq_save(flags);
	if (unlikely(start != old_start))
		memcpy(&cache_buf[0][0], (void *)start, line_size);

	if (unlikely(end != old_end))
		memcpy(&cache_buf[1][0], (void *)(old_end & (~(line_size - 1))), line_size);

	ax45mp_cpu_dcache_inval_range(vaddr, (void *)end, line_size);

	if (unlikely(start != old_start))
		memcpy((void *)start, &cache_buf[0][0], (old_start & (line_size - 1)));

	local_irq_restore(flags);
}

static void ax45mp_cpu_dma_wb_range(void *vaddr, size_t size)
{
	unsigned long start = (unsigned long)vaddr;
	unsigned long end = start + size;
	unsigned long line_size;
	unsigned long flags;

	line_size = ax45mp_priv->ax45mp_cache_line_size;
	local_irq_save(flags);
	start = start & (~(line_size - 1));
	ax45mp_cpu_dcache_wb_range(vaddr, (void *)end, line_size);
	local_irq_restore(flags);
}

void ax45mp_no_iocp_cmo(unsigned int cache_size, void *vaddr, size_t size, int dir, int ops)
{
	if (!static_branch_unlikely(&ax45mp_l2c_configured))
		return;

	/* We have nothing to do in case of NON_COHERENT_DMA_PREP */
	if (ops == NON_COHERENT_DMA_PREP)
		return;

	/*
	 * In case of DMA_FROM_DEVICE invalidate the L2 cache entries and
	 * in case of DMA_TO_DEVICE and DMA_BIDIRECTIONAL write-back an L2
	 * cache entries.
	 */
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

	/*
	 * ops == NON_COHERENT_SYNC_DMA_FOR_CPU
	 *
	 * in case of DMA_BIDIRECTIONAL and DMA_FROM_DEVICE invalidate the L2
	 * cache entries.
	 */
	if (dir == DMA_BIDIRECTIONAL || dir == DMA_FROM_DEVICE)
		ax45mp_cpu_dma_inval_range(vaddr, size);
}
EXPORT_SYMBOL(ax45mp_no_iocp_cmo);

static void ax45mp_get_l2_line_size(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	int ret;

	ret = of_property_read_u32(np, "cache-line-size", &ax45mp_priv->ax45mp_cache_line_size);
	if (ret) {
		dev_err(dev, "Failed to get cache-line-size, defaulting to 64 bytes\n");
		ax45mp_priv->ax45mp_cache_line_size = AX45MP_CACHE_LINE_SIZE;
	}

	if (ax45mp_priv->ax45mp_cache_line_size != AX45MP_CACHE_LINE_SIZE) {
		dev_err(dev, "Expected cache-line-size to be 64 bytes (found:%u). Defaulting to 64 bytes\n",
			ax45mp_priv->ax45mp_cache_line_size);
		ax45mp_priv->ax45mp_cache_line_size = AX45MP_CACHE_LINE_SIZE;
	}
}

static int ax45mp_l2c_probe(struct platform_device *pdev)
{
	ax45mp_priv = devm_kzalloc(&pdev->dev, sizeof(*ax45mp_priv), GFP_KERNEL);
	if (!ax45mp_priv)
		return -ENOMEM;

	ax45mp_priv->l2c_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ax45mp_priv->l2c_base))
		return PTR_ERR(ax45mp_priv->l2c_base);

	ax45mp_get_l2_line_size(pdev);

	static_branch_enable(&ax45mp_l2c_configured);

	return 0;
}

static const struct of_device_id ax45mp_cache_ids[] = {
	{ .compatible = "andestech,ax45mp-cache" },
	{ /* sentinel */ }
};

static struct platform_driver ax45mp_l2c_driver = {
	.driver = {
		.name = "ax45mp-l2c",
		.of_match_table = ax45mp_cache_ids,
	},
	.probe = ax45mp_l2c_probe,
};

static int __init ax45mp_cache_init(void)
{
	return platform_driver_register(&ax45mp_l2c_driver);
}
arch_initcall(ax45mp_cache_init);

MODULE_AUTHOR("Lad Prabhakar <prabhakar.mahadev-lad.rj@bp.renesas.com>");
MODULE_DESCRIPTION("Andes AX45MP L2 cache driver");
MODULE_LICENSE("GPL");
