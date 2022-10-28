// SPDX-License-Identifier: GPL-2.0
/*
 * PMA setup and non-coherent cache functions for AX45MP
 *
 * Copyright (C) 2022 Renesas Electronics Corp.
 */

#include <linux/cacheflush.h>
#include <linux/cacheinfo.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>

#include <asm/sbi.h>

#include "ax45mp_sbi.h"

/* L2 cache registers */
#define AX45MP_L2C_REG_CTL_OFFSET		0x8
#define AX45MP_L2C_IPREPETCH_OFF		3
#define AX45MP_L2C_DPREPETCH_OFF		5
#define AX45MP_L2C_IPREPETCH_MSK		(3 << AX45MP_L2C_IPREPETCH_OFF)
#define AX45MP_L2C_DPREPETCH_MSK		(3 << AX45MP_L2C_DPREPETCH_OFF)
#define AX45MP_L2C_TRAMOCTL_OFF			8
#define AX45MP_L2C_TRAMICTL_OFF			10
#define AX45MP_L2C_TRAMOCTL_MSK			(3 << AX45MP_L2C_TRAMOCTL_OFF)
#define AX45MP_L2C_TRAMICTL_MSK			BIT(AX45MP_L2C_TRAMICTL_OFF)
#define AX45MP_L2C_DRAMOCTL_OFF			11
#define AX45MP_L2C_DRAMICTL_OFF			13
#define AX45MP_L2C_DRAMOCTL_MSK			(3 << AX45MP_L2C_DRAMOCTL_OFF)
#define AX45MP_L2C_DRAMICTL_MSK			BIT(AX45MP_L2C_DRAMICTL_OFF)

#define AX45MP_L2C_REG_C0_CMD_OFFSET		0x40
#define AX45MP_L2C_REG_C0_ACC_OFFSET		0x48
#define AX45MP_L2C_REG_STATUS_OFFSET		0x80

/* D-cache operation */
#define AX45MP_CCTL_L1D_VA_INVAL		0
#define AX45MP_CCTL_L1D_VA_WB			1

/* L2 cache */
#define AX45MP_L2_CACHE_CTL_CEN_MASK		1

/* L2 CCTL status */
#define AX45MP_CCTL_L2_STATUS_IDLE		0

/* L2 CCTL status cores mask */
#define AX45MP_CCTL_L2_STATUS_C0_MASK		0xf

/* L2 cache operation */
#define AX45MP_CCTL_L2_PA_INVAL			0x8
#define AX45MP_CCTL_L2_PA_WB			0x9

#define AX45MP_L2C_HPM_PER_CORE_OFFSET		0x8
#define AX45MP_L2C_REG_PER_CORE_OFFSET		0x10
#define AX45MP_CCTL_L2_STATUS_PER_CORE_OFFSET	4

#define AX45MP_L2C_REG_CN_CMD_OFFSET(n)	\
	(AX45MP_L2C_REG_C0_CMD_OFFSET + ((n) * AX45MP_L2C_REG_PER_CORE_OFFSET))
#define AX45MP_L2C_REG_CN_ACC_OFFSET(n)	\
	(AX45MP_L2C_REG_C0_ACC_OFFSET + ((n) * AX45MP_L2C_REG_PER_CORE_OFFSET))
#define AX45MP_CCTL_L2_STATUS_CN_MASK(n)	\
	(AX45MP_CCTL_L2_STATUS_C0_MASK << ((n) * AX45MP_CCTL_L2_STATUS_PER_CORE_OFFSET))

#define AX45MP_MICM_CFG_ISZ_OFFSET		6
#define AX45MP_MICM_CFG_ISZ_MASK		(0x7  << AX45MP_MICM_CFG_ISZ_OFFSET)

#define AX45MP_MDCM_CFG_DSZ_OFFSET		6
#define AX45MP_MDCM_CFG_DSZ_MASK		(0x7  << AX45MP_MDCM_CFG_DSZ_OFFSET)

#define AX45MP_CCTL_REG_UCCTLBEGINADDR_NUM	0x80b
#define AX45MP_CCTL_REG_UCCTLCOMMAND_NUM	0x80c

#define AX45MP_MCACHE_CTL_CCTL_SUEN_OFFSET	8
#define AX45MP_MMSC_CFG_CCTLCSR_OFFSET		16
#define AX45MP_MISA_20_OFFSET			20

#define AX45MP_MCACHE_CTL_CCTL_SUEN_MASK	(0x1 << AX45MP_MCACHE_CTL_CCTL_SUEN_OFFSET)
#define AX45MP_MMSC_CFG_CCTLCSR_MASK		(0x1 << AX45MP_MMSC_CFG_CCTLCSR_OFFSET)
#define AX45MP_MISA_20_MASK			(0x1 << AX45MP_MISA_20_OFFSET)

#define AX45MP_MAX_CACHE_LINE_SIZE		256

#define AX45MP_MAX_PMA_REGIONS			16

struct ax45mp_priv {
	void __iomem *l2c_base;
	u64 ax45mp_cache_line_size;
	bool l2cache_enabled;
	bool ucctl_ok;
};

static struct ax45mp_priv *ax45mp_priv;
static DEFINE_STATIC_KEY_FALSE(ax45mp_l2c_configured);

/* PMA setup */
static long ax45mp_sbi_set_pma(unsigned long start,
			       unsigned long size,
			       unsigned long flags,
			       unsigned int entry_id)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_ANDES, AX45MP_SBI_EXT_SET_PMA,
			start, size, entry_id, flags, 0, 0);

	return ret.value;
}

static int ax45mp_configure_pma_regions(struct device_node *np)
{
	const char *propname = "andestech,pma-regions";
	u32 start, size, flags;
	unsigned int entry_id;
	unsigned int i;
	int count;
	int ret;

	count = of_property_count_elems_of_size(np, propname, sizeof(u32) * 3);
	if (count < 0)
		return count;

	if (count > AX45MP_MAX_PMA_REGIONS)
		return -EINVAL;

	for (i = 0, entry_id = 0 ; entry_id < count ; i += 3, entry_id++) {
		of_property_read_u32_index(np, propname, i, &start);
		of_property_read_u32_index(np, propname, i + 1, &size);
		of_property_read_u32_index(np, propname, i + 2, &flags);
		ret = ax45mp_sbi_set_pma(start, size, flags, entry_id);
		if (!ret)
			pr_err("Failed to setup PMA region 0x%x - 0x%x flags: 0x%x",
			       start, start + size, flags);
	}

	return 0;
}

/* L2 Cache operations */
static uint32_t ax45mp_cpu_get_mcache_ctl_status(void)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_ANDES, AX45MP_SBI_EXT_GET_MCACHE_CTL_STATUS,
			0, 0, 0, 0, 0, 0);
	return ret.value;
}

static uint32_t ax45mp_cpu_get_micm_cfg_status(void)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_ANDES, AX45MP_SBI_EXT_GET_MICM_CTL_STATUS,
			0, 0, 0, 0, 0, 0);
	return ret.value;
}

static uint32_t ax45mp_cpu_get_mdcm_cfg_status(void)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_ANDES, AX45MP_SBI_EXT_GET_MDCM_CTL_STATUS,
			0, 0, 0, 0, 0, 0);
	return ret.value;
}

static uint32_t ax45mp_cpu_get_mmsc_cfg_status(void)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_ANDES, AX45MP_SBI_EXT_GET_MMSC_CTL_STATUS,
			0, 0, 0, 0, 0, 0);
	return ret.value;
}

static uint32_t ax45mp_cpu_get_misa_cfg_status(void)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_ANDES, AX45MP_SBI_EXT_GET_MISA_CTL_STATUS,
			0, 0, 0, 0, 0, 0);
	return ret.value;
}

static inline uint32_t ax45mp_cpu_l2c_get_cctl_status(void)
{
	return readl(ax45mp_priv->l2c_base + AX45MP_L2C_REG_STATUS_OFFSET);
}

static inline uint32_t ax45mp_cpu_l2c_ctl_status(void)
{
	return readl(ax45mp_priv->l2c_base + AX45MP_L2C_REG_CTL_OFFSET);
}

static bool ax45mp_cpu_cache_controlable(void)
{
	return (((ax45mp_cpu_get_micm_cfg_status() & AX45MP_MICM_CFG_ISZ_MASK) ||
		 (ax45mp_cpu_get_mdcm_cfg_status() & AX45MP_MDCM_CFG_DSZ_MASK)) &&
		(ax45mp_cpu_get_misa_cfg_status() & AX45MP_MISA_20_MASK) &&
		(ax45mp_cpu_get_mmsc_cfg_status() & AX45MP_MMSC_CFG_CCTLCSR_MASK) &&
		(ax45mp_cpu_get_mcache_ctl_status() & AX45MP_MCACHE_CTL_CCTL_SUEN_MASK));
}

static void ax45mp_cpu_dcache_wb_range(void *start, void *end, int line_size)
{
	void __iomem *base = ax45mp_priv->l2c_base;
	unsigned long pa;
	int mhartid = 0;
#ifdef CONFIG_SMP
	mhartid = smp_processor_id();
#endif

	while (end > start) {
		if (ax45mp_priv->ucctl_ok) {
			csr_write(AX45MP_CCTL_REG_UCCTLBEGINADDR_NUM, start);
			csr_write(AX45MP_CCTL_REG_UCCTLCOMMAND_NUM, AX45MP_CCTL_L1D_VA_WB);
		}

		if (ax45mp_priv->l2cache_enabled) {
			pa = virt_to_phys(start);
			writel(pa, base + AX45MP_L2C_REG_CN_ACC_OFFSET(mhartid));
			writel(AX45MP_CCTL_L2_PA_WB,
			       base + AX45MP_L2C_REG_CN_CMD_OFFSET(mhartid));
			while ((ax45mp_cpu_l2c_get_cctl_status() &
				AX45MP_CCTL_L2_STATUS_CN_MASK(mhartid)) !=
				AX45MP_CCTL_L2_STATUS_IDLE)
				;
		}

		start += line_size;
	}
}

static void ax45mp_cpu_dcache_inval_range(void *start, void *end, int line_size)
{
	void __iomem *base = ax45mp_priv->l2c_base;
	unsigned long pa;
	int mhartid = 0;
#ifdef CONFIG_SMP
	mhartid = smp_processor_id();
#endif

	while (end > start) {
		if (ax45mp_priv->ucctl_ok) {
			csr_write(AX45MP_CCTL_REG_UCCTLBEGINADDR_NUM, start);
			csr_write(AX45MP_CCTL_REG_UCCTLCOMMAND_NUM, AX45MP_CCTL_L1D_VA_INVAL);
		}

		if (ax45mp_priv->l2cache_enabled) {
			pa = virt_to_phys(start);
			writel(pa, base + AX45MP_L2C_REG_CN_ACC_OFFSET(mhartid));
			writel(AX45MP_CCTL_L2_PA_INVAL,
			       base + AX45MP_L2C_REG_CN_CMD_OFFSET(mhartid));
			while ((ax45mp_cpu_l2c_get_cctl_status() &
				AX45MP_CCTL_L2_STATUS_CN_MASK(mhartid)) !=
				AX45MP_CCTL_L2_STATUS_IDLE)
				;
		}

		start += line_size;
	}
}

void ax45mp_cpu_dma_inval_range(void *vaddr, size_t size)
{
	char cache_buf[2][AX45MP_MAX_CACHE_LINE_SIZE];
	unsigned long start = (unsigned long)vaddr;
	unsigned long end = start + size;
	unsigned long old_start = start;
	unsigned long old_end = end;
	unsigned long line_size;
	unsigned long flags;

	if (static_branch_unlikely(&ax45mp_l2c_configured) && !ax45mp_priv)
		return;

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

	if (unlikely(end != old_end))
		memcpy((void *)(old_end + 1),
		       &cache_buf[1][(old_end & (line_size - 1)) + 1],
		       end - old_end - 1);

	local_irq_restore(flags);
}
EXPORT_SYMBOL(ax45mp_cpu_dma_inval_range);

void ax45mp_cpu_dma_wb_range(void *vaddr, size_t size)
{
	unsigned long start = (unsigned long)vaddr;
	unsigned long end = start + size;
	unsigned long line_size;
	unsigned long flags;

	if (static_branch_unlikely(&ax45mp_l2c_configured) && !ax45mp_priv)
		return;

	line_size = ax45mp_priv->ax45mp_cache_line_size;
	local_irq_save(flags);
	start = start & (~(line_size - 1));
	ax45mp_cpu_dcache_wb_range(vaddr, (void *)end, line_size);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(ax45mp_cpu_dma_wb_range);

static int ax45mp_configure_l2_cache(struct device_node *np)
{
	u8 ram_ctl[2];
	u32 cache_ctl;
	u32 prefetch;
	int ret;

	cache_ctl = ax45mp_cpu_l2c_ctl_status();

	/* Instruction and data fetch prefetch depth */
	ret = of_property_read_u32(np, "andestech,inst-prefetch", &prefetch);
	if (!ret) {
		cache_ctl &= ~AX45MP_L2C_IPREPETCH_MSK;
		cache_ctl |= (prefetch << AX45MP_L2C_IPREPETCH_OFF);
	}

	ret = of_property_read_u32(np, "andestech,data-prefetch", &prefetch);
	if (!ret) {
		cache_ctl &= ~AX45MP_L2C_DPREPETCH_MSK;
		cache_ctl |= (prefetch << AX45MP_L2C_DPREPETCH_OFF);
	}

	/* tag RAM and data RAM setup and output cycle */
	ret = of_property_read_u8_array(np, "andestech,tag-ram-ctl", ram_ctl, 2);
	if (!ret) {
		cache_ctl &= ~(AX45MP_L2C_TRAMOCTL_MSK | AX45MP_L2C_TRAMICTL_MSK);
		cache_ctl |= ram_ctl[0] << AX45MP_L2C_TRAMOCTL_OFF;
		cache_ctl |= ram_ctl[1] << AX45MP_L2C_TRAMICTL_OFF;
	}

	ret = of_property_read_u8_array(np, "andestech,data-ram-ctl", ram_ctl, 2);
	if (!ret) {
		cache_ctl &= ~(AX45MP_L2C_DRAMOCTL_MSK | AX45MP_L2C_DRAMICTL_MSK);
		cache_ctl |= ram_ctl[0] << AX45MP_L2C_DRAMOCTL_OFF;
		cache_ctl |= ram_ctl[1] << AX45MP_L2C_DRAMICTL_OFF;
	}

	writel(cache_ctl, ax45mp_priv->l2c_base + AX45MP_L2C_REG_CTL_OFFSET);

	ret = of_property_read_u64(np, "cache-line-size", &ax45mp_priv->ax45mp_cache_line_size);
	if (ret) {
		pr_err("Failed to get cache-line-size defaulting to 64 bytes\n");
		ax45mp_priv->ax45mp_cache_line_size = SZ_64;
	}

	ax45mp_priv->ucctl_ok = ax45mp_cpu_cache_controlable();
	ax45mp_priv->l2cache_enabled = ax45mp_cpu_l2c_ctl_status() & AX45MP_L2_CACHE_CTL_CEN_MASK;

	return 0;
}

static const struct of_device_id ax45mp_cache_ids[] = {
	{ .compatible = "andestech,ax45mp-cache" },
	{ /* sentinel */ }
};

static int ax45mp_l2c_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	int ret;

	ax45mp_priv = devm_kzalloc(&pdev->dev, sizeof(*ax45mp_priv), GFP_KERNEL);
	if (!ax45mp_priv)
		return -ENOMEM;

	ax45mp_priv->l2c_base = devm_of_iomap(&pdev->dev, pdev->dev.of_node, 0, NULL);
	if (!ax45mp_priv->l2c_base) {
		ret = -ENOMEM;
		goto l2c_err;
	}

	ret = ax45mp_configure_l2_cache(np);
	if (ret)
		goto l2c_err;

	ret = ax45mp_configure_pma_regions(np);
	if (ret)
		goto l2c_err;

	static_branch_disable(&ax45mp_l2c_configured);

	return 0;

l2c_err:
	devm_kfree(&pdev->dev, ax45mp_priv);
	ax45mp_priv = NULL;
	return ret;
}

static struct platform_driver ax45mp_l2c_driver = {
	.driver = {
		.name = "ax45mp-l2c",
		.of_match_table = ax45mp_cache_ids,
	},
	.probe = ax45mp_l2c_probe,
};

static int __init ax45mp_cache_init(void)
{
	static_branch_enable(&ax45mp_l2c_configured);
	return platform_driver_register(&ax45mp_l2c_driver);
}
arch_initcall(ax45mp_cache_init);
