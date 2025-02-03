// SPDX-License-Identifier: GPL-2.0+
/*
 * Renesas GBETH platform driver
 *
 * Copyright (C) 2025 Renesas Electronics Corporation
 */

 #include <linux/phy.h>
 #include <linux/time.h>

#include "dwmac4.h"
#include "stmmac_platform.h"

struct renesas_rzv2h_gbeth {
	struct device *dev;
	void __iomem *regs;

	struct clk *clk_tx;
	struct clk *clk_slave;
};

static const char *const renesas_rzv2h_gbeth_clks[] = {
	"pclk", "rx", "rx-180", "tx-180",
};

static void renesas_rzv2h_gbeth_fix_speed(void *priv, unsigned int speed, unsigned int mode)
{
	struct renesas_rzv2h_gbeth *gbeth = priv;
	unsigned long rate = 125000000;
	struct clk *parent;
	int err;

	rate = rgmii_clock(speed);
	if (rate < 0) {
		dev_err(gbeth->dev, "invalid speed %u\n", speed);
		return;
	}
	parent = clk_get_parent(clk_get_parent(gbeth->clk_tx));

	err = clk_set_rate(parent, rate);
	if (err < 0)
		dev_err(gbeth->dev, "failed to set tx rate %lu\n", rate);
}

static int renesas_rzv2h_gbeth_init(struct platform_device *pdev, void *priv)
{
	struct renesas_rzv2h_gbeth *gbeth = priv;
	unsigned long rate;
	u32 value;

	rate = clk_get_rate(gbeth->clk_slave);
	value = (rate / USEC_PER_SEC) - 1;
	writel(value, gbeth->regs + GMAC_1US_TIC_COUNTER);

	return 0;
}

static int renesas_rzv2h_gbeth_probe(struct platform_device *pdev)
{
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	struct renesas_rzv2h_gbeth *gbeth;
	struct device *dev = &pdev->dev;
	struct reset_control *rstc;
	struct clk_bulk_data *clks;
	unsigned int i;
	int err;

	err = stmmac_get_platform_resources(pdev, &stmmac_res);
	if (err)
		return dev_err_probe(dev, err,
				     "failed to get resources\n");

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return dev_err_probe(dev, PTR_ERR(plat_dat),
				     "dt configuration failed\n");

	gbeth = devm_kzalloc(dev, sizeof(*gbeth), GFP_KERNEL);
	if (!gbeth)
		return -ENOMEM;

	gbeth->clk_tx = devm_clk_get_enabled(&pdev->dev, "tx");
	if (IS_ERR(gbeth->clk_tx))
		return PTR_ERR(gbeth->clk_tx);

	gbeth->clk_slave = devm_clk_get_enabled(&pdev->dev, "pclk");
	if (IS_ERR(gbeth->clk_slave))
		return PTR_ERR(gbeth->clk_slave);

	clks = devm_kcalloc(dev, ARRAY_SIZE(renesas_rzv2h_gbeth_clks),
			    sizeof(*clks), GFP_KERNEL);
	if (!clks)
		return -ENOMEM;

	for (i = 0; i <  ARRAY_SIZE(renesas_rzv2h_gbeth_clks); i++)
		clks[i].id = renesas_rzv2h_gbeth_clks[i];

	err = devm_clk_bulk_get_all_enabled(dev, &clks);
	if (err < 0)
		return err;

	rstc = devm_reset_control_get_exclusive_deasserted(dev, NULL);
	if (IS_ERR(rstc))
		return PTR_ERR(rstc);

	usleep_range(2000, 4000);

	gbeth->dev = dev;
	gbeth->regs = stmmac_res.addr;
	plat_dat->bsp_priv = gbeth;
	plat_dat->fix_mac_speed = renesas_rzv2h_gbeth_fix_speed;
	plat_dat->flags |= STMMAC_FLAG_HWTSTAMP_CORRECT_LATENCY |
			   STMMAC_FLAG_EN_TX_LPI_CLOCKGATING |
			   STMMAC_FLAG_RX_CLK_RUNS_IN_LPI |
			   STMMAC_FLAG_SPH_DISABLE;
	/*
	 * TODO: Need to read the USERVER to determine 0/1
	 * STMMAC_FLAG_EXT_SNAPSHOT_EN for ETH0
	 * STMMAC_FLAG_INT_SNAPSHOT_EN for ETH1
	 * plat_dat->flags |= STMMAC_FLAG_EXT_SNAPSHOT_EN;
	 * plat_dat->flags |= STMMAC_FLAG_INT_SNAPSHOT_EN;
	 */
	plat_dat->init = renesas_rzv2h_gbeth_init;

	renesas_rzv2h_gbeth_init(pdev, gbeth);

	return stmmac_dvr_probe(dev, plat_dat, &stmmac_res);
}

static const struct of_device_id renesas_rzv2h_gbeth_match[] = {
	{ .compatible = "renesas,r9a09g057-gbeth", },
	{ }
};
MODULE_DEVICE_TABLE(of, renesas_rzv2h_gbeth_match);

static struct platform_driver renesas_rzv2h_gbeth_driver = {
	.probe  = renesas_rzv2h_gbeth_probe,
	.remove = stmmac_pltfr_remove,
	.driver = {
		.name           = "renesas-gbeth",
		.pm             = &stmmac_pltfr_pm_ops,
		.of_match_table = renesas_rzv2h_gbeth_match,
	},
};
module_platform_driver(renesas_rzv2h_gbeth_driver);

MODULE_AUTHOR("Lad Prabhakar <prabhakar.mahadev-lad.rj@bp.renesas.com>");
MODULE_DESCRIPTION("Renesas GBETH platform driver");
MODULE_LICENSE("GPL");
