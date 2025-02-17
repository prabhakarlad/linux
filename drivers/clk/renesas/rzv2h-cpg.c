// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas RZ/V2H(P) Clock Pulse Generator
 *
 * Copyright (C) 2024 Renesas Electronics Corp.
 *
 * Based on rzg2l-cpg.c
 *
 * Copyright (C) 2015 Glider bvba
 * Copyright (C) 2013 Ideas On Board SPRL
 * Copyright (C) 2015 Renesas Electronics Corp.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/iopoll.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_clock.h>
#include <linux/pm_domain.h>
#include <linux/refcount.h>
#include <linux/reset-controller.h>
#include <linux/units.h>

#include <dt-bindings/clock/renesas-cpg-mssr.h>

#include "rzv2h-cpg.h"

#ifdef DEBUG
#define WARN_DEBUG(x)		WARN_ON(x)
#else
#define WARN_DEBUG(x)		do { } while (0)
#endif

#define EXTAL_FREQ_IN_MEGA_HZ	(24 * MEGA)

#define GET_CLK_ON_OFFSET(x)	(0x600 + ((x) * 4))
#define GET_CLK_MON_OFFSET(x)	(0x800 + ((x) * 4))
#define GET_RST_OFFSET(x)	(0x900 + ((x) * 4))
#define GET_RST_MON_OFFSET(x)	(0xA00 + ((x) * 4))

#define CPG_BUS_1_MSTOP		(0xd00)
#define CPG_BUS_MSTOP(m)	(CPG_BUS_1_MSTOP + ((m) - 1) * 4)

#define KDIV(val)		((s16)FIELD_GET(GENMASK(31, 16), (val)))
#define MDIV(val)		FIELD_GET(GENMASK(15, 6), (val))
#define PDIV(val)		FIELD_GET(GENMASK(5, 0), (val))
#define SDIV(val)		FIELD_GET(GENMASK(2, 0), (val))

#define DDIV_DIVCTL_WEN(shift)		BIT((shift) + 16)

#define GET_MOD_CLK_ID(base, index, bit)		\
			((base) + ((((index) * (16))) + (bit)))

#define CPG_CLKSTATUS0		(0x700)

#define PLL_STBY_RESETB		BIT(0)
#define PLL_STBY_RESETB_WEN	BIT(16)
#define PLL_MON_RESETB		BIT(0)
#define PLL_MON_LOCK		BIT(4)

#define PLL_CLK_ACCESS(n)	((n) & BIT(31) ? 1 : 0)
#define PLL_CLK1_OFFSET(n)	FIELD_GET(GENMASK(15, 0), (n))
#define PLL_CLK2_OFFSET(n)	(PLL_CLK1_OFFSET(n) + (0x4))
#define PLL_STBY_OFFSET(n)	(PLL_CLK1_OFFSET(n) - (0x4))
#define PLL_MON_OFFSET(n)	(PLL_STBY_OFFSET(n) + (0x10))

#define RZV2H_PLLFVCO_MIN		(1600 * MEGA)
#define RZV2H_PLLFVCO_MAX		(3200 * MEGA)
#define RZV2H_PLLFVCO_AVG		((RZV2H_PLLFVCO_MIN + RZV2H_PLLFVCO_MAX) / 2)
#define RZV2H_PLL_DIV_M_MIN		(64)
#define RZV2H_PLL_DIV_M_MAX		(533)

#define RZV2H_CPG_PLL_STBY_RESETB		BIT(0)
#define RZV2H_CPG_PLL_STBY_RESETB_WEN		BIT(16)
#define RZV2H_CPG_PLL_STBY_SSCG_EN_WEN		BIT(18)
#define RZV2H_CPG_PLL_MON_RESETB		BIT(0)
#define RZV2H_CPG_PLL_MON_LOCK			BIT(4)

/**
 * struct rzv2h_cpg_priv - Clock Pulse Generator Private Data
 *
 * @dev: CPG device
 * @base: CPG register block base address
 * @rmw_lock: protects register accesses
 * @clks: Array containing all Core and Module Clocks
 * @num_core_clks: Number of Core Clocks in clks[]
 * @num_mod_clks: Number of Module Clocks in clks[]
 * @resets: Array of resets
 * @num_resets: Number of Module Resets in info->resets[]
 * @last_dt_core_clk: ID of the last Core Clock exported to DT
 * @mstop_count: Array of mstop values
 * @rcdev: Reset controller entity
 */
struct rzv2h_cpg_priv {
	struct device *dev;
	void __iomem *base;
	spinlock_t rmw_lock;

	struct clk **clks;
	unsigned int num_core_clks;
	unsigned int num_mod_clks;
	struct rzv2h_reset *resets;
	unsigned int num_resets;
	unsigned int last_dt_core_clk;

	atomic_t *mstop_count;

	struct reset_controller_dev rcdev;
};

static long long int compute_ffout(int p, int s, int m, long int k);

#define rcdev_to_priv(x)	container_of(x, struct rzv2h_cpg_priv, rcdev)

struct pll_clk {
	struct rzv2h_cpg_priv *priv;
	void __iomem *base;
	struct clk_hw hw;
	unsigned int conf;
	unsigned int type;
};

#define to_pll(_hw)	container_of(_hw, struct pll_clk, hw)

/**
 * struct mod_clock - Module clock
 *
 * @priv: CPG private data
 * @mstop_data: mstop data relating to module clock
 * @hw: handle between common and hardware-specific interfaces
 * @no_pm: flag to indicate PM is not supported
 * @on_index: register offset
 * @on_bit: ON/MON bit
 * @mon_index: monitor register offset
 * @mon_bit: montor bit
 */
struct mod_clock {
	struct rzv2h_cpg_priv *priv;
	unsigned int mstop_data;
	struct clk_hw hw;
	bool no_pm;
	u8 on_index;
	u8 on_bit;
	s8 mon_index;
	u8 mon_bit;
};

#define to_mod_clock(_hw) container_of(_hw, struct mod_clock, hw)

/**
 * struct ddiv_clk - DDIV clock
 *
 * @priv: CPG private data
 * @div: divider clk
 * @mon: monitor bit in CPG_CLKSTATUS0 register
 */
struct ddiv_clk {
	struct rzv2h_cpg_priv *priv;
	struct clk_divider div;
	u8 mon;
};

#define to_ddiv_clock(_div) container_of(_div, struct ddiv_clk, div)

static int rzv2h_cpg_pll_clk_is_enabled(struct clk_hw *hw)
{
	struct pll_clk *pll_clk = to_pll(hw);
	struct rzv2h_cpg_priv *priv = pll_clk->priv;
	u32 mon_offset = PLL_MON_OFFSET(pll_clk->conf);
	u32 val;

	val = readl(priv->base + mon_offset);

	/* Ensure both RESETB and LOCK bits are set */
	return (val & (PLL_MON_RESETB | PLL_MON_LOCK)) ==
	       (PLL_MON_RESETB | PLL_MON_LOCK);
}

static int rzv2h_cpg_pll_clk_enable(struct clk_hw *hw)
{
	bool enabled = rzv2h_cpg_pll_clk_is_enabled(hw);
	struct pll_clk *pll_clk = to_pll(hw);
	struct rzv2h_cpg_priv *priv = pll_clk->priv;
	u32 conf = pll_clk->conf;
	unsigned long flags = 0;
	u32 stby_offset;
	u32 mon_offset;
	u32 val;
	int ret;

	if (enabled)
		return 0;

	stby_offset = PLL_STBY_OFFSET(conf);
	mon_offset = PLL_MON_OFFSET(conf);

	val = PLL_STBY_RESETB_WEN | PLL_STBY_RESETB;
	spin_lock_irqsave(&priv->rmw_lock, flags);
	writel(val, priv->base + stby_offset);
	spin_unlock_irqrestore(&priv->rmw_lock, flags);

	/* ensure PLL is in normal mode */
	ret = readl_poll_timeout(priv->base + mon_offset, val,
				 (val & (PLL_MON_RESETB | PLL_MON_LOCK)) ==
				 (PLL_MON_RESETB | PLL_MON_LOCK), 0, 250000);
	if (ret)
		dev_err(priv->dev, "Failed to enable PLL 0x%x/%pC\n",
			stby_offset, hw->clk);

	return ret;
}

struct rzv2h_plldsi_div_hw_data {
	const struct clk_div_table *dtable;
	struct rzv2h_cpg_priv *priv;
	struct clk_hw hw;
	struct ddiv conf;
	u32 div;
};

#define to_plldsi_div_hw_data(_hw) \
			container_of(_hw, struct rzv2h_plldsi_div_hw_data, hw)

struct plls {
        int p;
        int s;
        int m;
        long int k;
};

static struct plls plls_best_mhz = { .p = -1, };
static u8 mhz_div;

unsigned long long rzv2h_cpg_plldsi_get_rate_mhz(void)
{
	unsigned long long rate_mhz;
	unsigned long long two_pow16 = (1 << 16);

	if (plls_best_mhz.p == -1)
		return 0;

	// ffout = ((m * 2^16 * 24000000 + k * 24000000) * 1000)/(2^16 * p * 2^s)
	rate_mhz = ((plls_best_mhz.m * two_pow16 * 24000000ULL + plls_best_mhz.k * 24000000ULL) * 1000ULL) /
		   (two_pow16 * plls_best_mhz.p * (1 << plls_best_mhz.s));

	pr_err("%s rate_mhz:%llu div:%u\n", __func__, rate_mhz, mhz_div);
	if (!mhz_div)
		return rate_mhz;

	return DIV_ROUND_CLOSEST_ULL(rate_mhz, mhz_div);
}
EXPORT_SYMBOL(rzv2h_cpg_plldsi_get_rate_mhz);

static unsigned long rzv2h_cpg_plldsi_div_recalc_rate(struct clk_hw *hw,
						      unsigned long parent_rate)
{
	struct rzv2h_plldsi_div_hw_data *dsi_div = to_plldsi_div_hw_data(hw);
	struct rzv2h_cpg_priv *priv = dsi_div->priv;
	struct ddiv ddiv = dsi_div->conf;
	u32 div;

	div = readl(priv->base + ddiv.offset);
	div >>= ddiv.shift;
	div &= ((2 << ddiv.width) - 1);

	div = dsi_div->dtable[div].div;

	return DIV_ROUND_CLOSEST_ULL(parent_rate, div);
}

static int rzv2h_cpg_plldsi_div_determine_rate(struct clk_hw *hw,
					       struct clk_rate_request *req)
{
	struct rzv2h_plldsi_div_hw_data *dsi_div = to_plldsi_div_hw_data(hw);

	/*
	 * Determine the rate and best parent rate for the PLLDSI divider clock.
	 *
	 * Adjust the requested clock rate (`req->rate`) to ensure it falls within
	 * the supported range of 5.44 MHz to 187.5 MHz. If the rate is below 12.5 MHz,
	 * a division factor of 6 is used; otherwise, a division factor of 2 is applied.
	 * The `best_parent_rate` is calculated accordingly based on the chosen division
	 * factor.
	 */
	req->rate = clamp(req->rate, 5440000UL, 187500000UL);

	if (req->rate < 12500000UL) {
		req->best_parent_rate = req->rate * 6;
		dsi_div->div = 6;
	} else {
		req->best_parent_rate = req->rate * 2;
		dsi_div->div = 2;
	}

	mhz_div = dsi_div->div;

	// pr_err("%s req_rate:%lu best_parent_rate:%lu dsi_div:%u\n", __func__,
		// req->rate, req->best_parent_rate, dsi_div->div);
	return 0;
};

static int rzv2h_cpg_plldsi_div_set_rate(struct clk_hw *hw,
					 unsigned long rate,
					 unsigned long parent_rate)
{
	struct rzv2h_plldsi_div_hw_data *dsi_div = to_plldsi_div_hw_data(hw);
	struct rzv2h_cpg_priv *priv = dsi_div->priv;
	struct ddiv ddiv = dsi_div->conf;
	const struct clk_div_table *clkt;
	u32 reg, shift, div;

	div = dsi_div->div;
	for (clkt = dsi_div->dtable; clkt->div; clkt++) {
		if (clkt->div == div)
			break;
	}

	if (!clkt->div && !clkt->val)
		return -EINVAL;

	shift = ddiv.shift;
	reg = readl(priv->base + ddiv.offset);
	reg &= ~(GENMASK(shift + ddiv.width, shift));

	writel(reg | (clkt->val << shift) |
	       DDIV_DIVCTL_WEN(shift), priv->base + ddiv.offset);

	return 0;
};

static const struct clk_ops rzv2h_cpg_plldsi_div_ops = {
	.recalc_rate = rzv2h_cpg_plldsi_div_recalc_rate,
	.determine_rate = rzv2h_cpg_plldsi_div_determine_rate,
	.set_rate = rzv2h_cpg_plldsi_div_set_rate,
};

static struct clk * __init
rzv2h_cpg_plldsi_div_clk_register(const struct cpg_core_clk *core,
				  struct rzv2h_cpg_priv *priv)
{
	struct rzv2h_plldsi_div_hw_data *clk_hw_data;
	struct clk **clks = priv->clks;
	struct clk_init_data init;
	const struct clk *parent;
	const char *parent_name;
	struct clk_hw *clk_hw;
	int ret;

	parent = clks[core->parent];
	if (IS_ERR(parent))
		return ERR_CAST(parent);

	clk_hw_data = devm_kzalloc(priv->dev, sizeof(*clk_hw_data), GFP_KERNEL);
	if (!clk_hw_data)
		return ERR_PTR(-ENOMEM);

	clk_hw_data->priv = priv;
	clk_hw_data->conf = core->cfg.ddiv;
	clk_hw_data->dtable = core->dtable;

	parent_name = __clk_get_name(parent);
	init.name = core->name;
	init.ops = &rzv2h_cpg_plldsi_div_ops;
	init.flags = CLK_SET_RATE_PARENT;
	init.parent_names = &parent_name;
	init.num_parents = 1;

	clk_hw = &clk_hw_data->hw;
	clk_hw->init = &init;

	ret = devm_clk_hw_register(priv->dev, clk_hw);
	if (ret)
		return ERR_PTR(ret);

	return clk_hw->clk;
}

static long rzv2h_cpg_plldsi_round_rate(struct clk_hw *hw,
					unsigned long rate,
					unsigned long *parent_rate)
{
	return clamp(rate, 25000000UL, 375000000UL);
}

static unsigned long rzv2h_cpg_plldsi_clk_recalc_rate(struct clk_hw *hw,
						      unsigned long parent_rate)
{
	struct pll_clk *pll_clk = to_pll(hw);
	struct rzv2h_cpg_priv *priv = pll_clk->priv;
	unsigned int val1, val2;
	long long int fout;
	u64 rate;

	val1 = readl(priv->base + PLL_CLK1_OFFSET(pll_clk->conf));
	val2 = readl(priv->base + PLL_CLK2_OFFSET(pll_clk->conf));


	rate = mul_u64_u32_shr(parent_rate, (MDIV(val1) << 16) + KDIV(val1),
		       16 + SDIV(val2));
	// fout = compute_ffout(PDIV(val1), SDIV(val2), MDIV(val1), KDIV(val1));
	// pr_err("%s parent_rate:%lu rate:%llu fout:%lld\n", __func__, parent_rate,
	       // DIV_ROUND_CLOSEST_ULL(rate, PDIV(val1)), fout);

	return DIV_ROUND_CLOSEST_ULL(rate, PDIV(val1));
}

static long long int compute_ffout(int p, int s, int m, long int k)
{
	long long int fvco, fref = 0, fout;
	long long int a_i, a_r, b_i, b_r;

	a_i = ((m * 65536) + k) / 65536;
	a_r = ((m * 65536) + k) % 65536;
	//a_d = a_r / 65536

	b_i = 24000000 / p;
	b_r = 24000000 % p;
	//b_d = b_r / p

	fref = b_i + b_r;
	//fvco = (m + (k / 65536))*(24000000 / p);
	// a * b = (a_i + a_d) * (b_i + b_d) = a_i * b_i + a_i * b_d + a_d * b_i
	// + a_d * b_d
	//
	// a_d = a_r / 65536
	// b_d = b_r / p
	fvco = a_i * b_i + ((a_i * b_r)/p + (a_r * b_i)/65536 + (a_r * b_r)/(65536*p));

	fout = fvco / (1 << s);

	return fout;
}

static long long int compute_fvco(int p, int s, int m, int k)
{
	long long int a_i, a_r, b_i, b_r;
	long long int fvco;

	a_i = ((m * 65536) + k) / 65536;
	a_r = ((m * 65536) + k) % 65536;
	//a_d = a_r / 65536

	b_i = 24000000 / p;
	b_r = 24000000 % p;
	//b_d = b_r / p

	//fvco = (m + (k / 65536))*(24000000 / p);
	// a * b = (a_i + a_d) * (b_i + b_d) = a_i * b_i + a_i * b_d + a_d * b_i
	// + a_d * b_d
	//
	// a_d = a_r / 65536
	// b_d = b_r / p
	fvco = a_i * b_i + ((a_i * b_r)/p + (a_r * b_i)/65536 + (a_r * b_r)/(65536*p));

	return fvco;
}

static int plls_valid(struct plls curr)
{
	if (curr.p == -1)
		return 0;

	return 1;
}

static struct plls get_best(struct plls curr_best,
			    struct plls curr,
			    long long int fout)
{
	long long int curr_best_fout_err, curr_fout_err;
	long long int curr_best_fout, curr_fout;
	long long int curr_fvco, curr_best_fvco;

	if (curr.p < 1 || curr.p > 4)
		return curr_best;

	if (curr.s < 0 || curr.s > 6)
		return curr_best;

	if (curr.m < 64 || curr.m > 533)
		return curr_best;

	if (curr.k < -32768 || curr.k > 32767)
		return curr_best;

	if (!plls_valid(curr_best))
		return curr;

	curr_fout = compute_ffout(curr.p, curr.s, curr.m, curr.k);
	curr_best_fout = compute_ffout(curr_best.p, curr_best.s, curr_best.m, curr_best.k);

	curr_fout_err = abs(curr_fout - fout);
	curr_best_fout_err = abs(curr_best_fout - fout);

	if (curr_fout_err < curr_best_fout_err)
		return curr;

	if (curr_fout_err > curr_best_fout_err)
		return curr_best;

	curr_fvco = compute_fvco(curr.p, curr.s, curr.m, curr.k);
	curr_best_fvco = compute_fvco(curr_best.p, curr_best.s, curr_best.m, curr_best.k);
	if (abs(curr_fvco - RZV2H_PLLFVCO_AVG) < abs(curr_best_fvco - RZV2H_PLLFVCO_AVG))
		return curr;

	return curr_best;
}


static long int compute_best_k(int p, int s, int m, long int k,
			     long long int fout,
			     long long int cur_fout)
{
	long long int _fout, _best_fout = cur_fout;
	long int _k, _best_k = k;

	/*
	 * FIXME: check if `k` is incremted only once and that works OK
	 */
	for (_k=k+1; _k <= 32767; _k++) {
		_fout = compute_ffout(p, s, m, _k);
		if (_best_fout < fout && _fout >= fout) {
		 	_best_fout = _fout;
		 	_best_k = _k;
		}
		if (abs(fout - _fout) > abs(fout - _best_fout)) {
			break;
		}
		_best_fout = _fout;
		_best_k = _k;
	}
#if 0
	for (_k=k-1; _k >= -32768; _k--) {
		_fout = compute_ffout(p, s, m, _k);
		if (abs(fout - _fout) > abs(fout - _best_fout)) {
			break;
		}
		_best_fout = _fout;
		_best_k = _k;
	}
#endif

	return _best_k;
}

static bool rzv2h_calculate_pll_dividers(long long fout, int *best_p,
					 int *best_s, int *best_m, long *best_k)
{
	int s, m0, m1, p;
	long long int fvco, osc, fref;
	long long int fvco_ref;
	long long int twopow16 = (1 << 16);
	long int k0, k1;
	long long int ffout = fout, ffout_diff, fvco_cal = 0, best_ffout_diff = fout;
	struct plls best = {.p = -1};

	/* Setting all PLL Registers */
	osc = EXTAL_FREQ_IN_MEGA_HZ;
	for (p = 4; p >= 1; p--) {
		for (s = 6; s >= 0; s--) {
			/* Check FVCO condition */
			fvco = fout * (1 << s);
#if 1
			if (fvco > RZV2H_PLLFVCO_MAX ||
			    fvco < RZV2H_PLLFVCO_MIN)
				continue;
#endif
			m0 = (fvco * p) / osc;
			m1 = m0 + 1;

			fref = 24000000 / p;

			if (!(m0 < RZV2H_PLL_DIV_M_MIN || m0 > RZV2H_PLL_DIV_M_MAX)) {
				k0 = ((fvco * twopow16) - (m0 * twopow16 * fref)) / fref;
				if (!(k0 < -32768 || k0 > 32767)) {
					struct plls pll0 = {.p = -1};

					k0 = compute_best_k(p, s, m0, k0, fout, compute_ffout(p, s, m0, k0));

					pll0.p = p;
					pll0.s = s;
					pll0.m = m0;
					pll0.k = k0;

					best = get_best(best, pll0, fout);
				}
			}

			if (!(m1 < RZV2H_PLL_DIV_M_MIN || m1 > RZV2H_PLL_DIV_M_MAX)) {
				k1 = ((fvco * twopow16) - (m1 * twopow16 * fref)) / fref;
				if (!(k1 < -32768 || k1 > 32767)) {
					struct plls pll1 = {.p = -1};

					k1 = compute_best_k(p, s, m1, k1, fout, compute_ffout(p, s, m1, k1));

					pll1.p = p;
					pll1.s = s;
					pll1.m = m1;
					pll1.k = k1;

					best = get_best(best, pll1, fout);
				}
			}
		}
	}

	if (!plls_valid(best))
		return false;

	*best_p = best.p;
	*best_s = best.s;
	*best_m = best.m;
	*best_k = best.k;

	plls_best_mhz.p = best.p;
	plls_best_mhz.s = best.s;
	plls_best_mhz.m = best.m;
	plls_best_mhz.k = best.k;

	return true;
}

static int rzv2h_cpg_plldsi_set_rate(struct clk_hw *hw,
				     unsigned long rate,
				     unsigned long parent_rate)
{
	struct pll_clk *pll_clk = to_pll(hw);
	struct rzv2h_cpg_priv *priv = pll_clk->priv;
	int pll_m, pll_p, pll_s;
	u32 conf = pll_clk->conf;
	unsigned long calc_rate;
	long pll_k;
	u32 val;
	int ret;

	if (rzv2h_calculate_pll_dividers(rate, &pll_p, &pll_s, &pll_m, &pll_k))
		goto found_dividers;

	dev_err(priv->dev, "failed to set %s to rate %lu\n", clk_hw_get_name(hw), rate);
	return -EINVAL;

found_dividers:
	calc_rate = compute_ffout(pll_p, pll_s, pll_m, pll_k);
	dev_err(priv->dev, "fout:%lu calc_fout:%lu pll_k: %ld, pll_m: %d, pll_p: %d, pll_s: %d\n",
		rate, calc_rate, pll_k, pll_m, pll_p, pll_s);

	/* Put PLL into standby mode */
	writel(RZV2H_CPG_PLL_STBY_RESETB_WEN, priv->base + PLL_STBY_OFFSET(conf));

	ret = readl_poll_timeout(priv->base + PLL_STBY_OFFSET(conf) + 0x10,
				 val, !(val & RZV2H_CPG_PLL_MON_LOCK),
				 100, 250000);
	if (ret) {
		dev_err(priv->dev, "failed to put PLLDSI to stanby mode");
		return ret;
	}

	/* Output clock setting 1 */
	writel(((s16)pll_k << 16) | (pll_m << 6) | (pll_p), priv->base + PLL_CLK1_OFFSET(conf));

	/* Output clock setting 2 */
	val = readl(priv->base + PLL_CLK2_OFFSET(conf));
	writel((val & ~GENMASK(2, 0)) | pll_s, priv->base + PLL_CLK2_OFFSET(conf));

	/* Put PLL to normal mode */
	writel(RZV2H_CPG_PLL_STBY_RESETB_WEN | RZV2H_CPG_PLL_STBY_RESETB,
	       priv->base + PLL_STBY_OFFSET(conf));

	/* PLL normal mode transition, output clock stability check */
	ret = readl_poll_timeout(priv->base + PLL_STBY_OFFSET(conf) + 0x10,
				 val, (val & RZV2H_CPG_PLL_MON_LOCK),
				 100, 250000);
	if (ret) {
		dev_err(priv->dev, "failed to put PLLDSI to normal mode");
		return ret;
	}

	return 0;
};

static const struct clk_ops rzv2h_cpg_plldsi_ops = {
	.recalc_rate = rzv2h_cpg_plldsi_clk_recalc_rate,
	.round_rate = rzv2h_cpg_plldsi_round_rate,
	.set_rate = rzv2h_cpg_plldsi_set_rate,
};

static struct clk * __init
rzv2h_cpg_plldsi_clk_register(const struct cpg_core_clk *core,
			      struct rzv2h_cpg_priv *priv)
{
	void __iomem *base = priv->base;
	struct device *dev = priv->dev;
	struct clk_init_data init;
	const struct clk *parent;
	const char *parent_name;
	struct pll_clk *pll_clk;
	int ret;

	parent = priv->clks[core->parent];
	if (IS_ERR(parent))
		return ERR_CAST(parent);

	pll_clk = devm_kzalloc(dev, sizeof(*pll_clk), GFP_KERNEL);
	if (!pll_clk)
		return ERR_PTR(-ENOMEM);

	parent_name = __clk_get_name(parent);
	init.name = core->name;
	init.ops = &rzv2h_cpg_plldsi_ops;
	init.flags = 0;
	init.parent_names = &parent_name;
	init.num_parents = 1;

	pll_clk->hw.init = &init;
	pll_clk->conf = core->cfg.conf;
	pll_clk->base = base;
	pll_clk->priv = priv;
	pll_clk->type = core->type;

	/* Disable SSC and turn on PLL clock when init */
	writel(RZV2H_CPG_PLL_STBY_RESETB_WEN | RZV2H_CPG_PLL_STBY_RESETB |
	       RZV2H_CPG_PLL_STBY_SSCG_EN_WEN, base + PLL_STBY_OFFSET(core->cfg.conf));

	ret = devm_clk_hw_register(dev, &pll_clk->hw);
	if (ret)
		return ERR_PTR(ret);

	return pll_clk->hw.clk;
}

static unsigned long rzv2h_cpg_pll_clk_recalc_rate(struct clk_hw *hw,
						   unsigned long parent_rate)
{
	struct pll_clk *pll_clk = to_pll(hw);
	struct rzv2h_cpg_priv *priv = pll_clk->priv;
	unsigned int clk1, clk2;
	u64 rate;

	if (!PLL_CLK_ACCESS(pll_clk->conf))
		return 0;

	clk1 = readl(priv->base + PLL_CLK1_OFFSET(pll_clk->conf));
	clk2 = readl(priv->base + PLL_CLK2_OFFSET(pll_clk->conf));

	rate = mul_u64_u32_shr(parent_rate, (MDIV(clk1) << 16) + KDIV(clk1),
			       16 + SDIV(clk2));

	return DIV_ROUND_CLOSEST_ULL(rate, PDIV(clk1));
}

static const struct clk_ops rzv2h_cpg_pll_ops = {
	.is_enabled = rzv2h_cpg_pll_clk_is_enabled,
	.enable = rzv2h_cpg_pll_clk_enable,
	.recalc_rate = rzv2h_cpg_pll_clk_recalc_rate,
};

static struct clk * __init
rzv2h_cpg_pll_clk_register(const struct cpg_core_clk *core,
			   struct rzv2h_cpg_priv *priv,
			   const struct clk_ops *ops)
{
	void __iomem *base = priv->base;
	struct device *dev = priv->dev;
	struct clk_init_data init;
	const struct clk *parent;
	const char *parent_name;
	struct pll_clk *pll_clk;
	int ret;

	parent = priv->clks[core->parent];
	if (IS_ERR(parent))
		return ERR_CAST(parent);

	pll_clk = devm_kzalloc(dev, sizeof(*pll_clk), GFP_KERNEL);
	if (!pll_clk)
		return ERR_PTR(-ENOMEM);

	parent_name = __clk_get_name(parent);
	init.name = core->name;
	init.ops = ops;
	init.flags = 0;
	init.parent_names = &parent_name;
	init.num_parents = 1;

	pll_clk->hw.init = &init;
	pll_clk->conf = core->cfg.conf;
	pll_clk->base = base;
	pll_clk->priv = priv;
	pll_clk->type = core->type;

	ret = devm_clk_hw_register(dev, &pll_clk->hw);
	if (ret)
		return ERR_PTR(ret);

	return pll_clk->hw.clk;
}

static unsigned long rzv2h_ddiv_recalc_rate(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	struct clk_divider *divider = to_clk_divider(hw);
	unsigned int val;

	val = readl(divider->reg) >> divider->shift;
	val &= clk_div_mask(divider->width);

	return divider_recalc_rate(hw, parent_rate, val, divider->table,
				   divider->flags, divider->width);
}

static long rzv2h_ddiv_round_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long *prate)
{
	struct clk_divider *divider = to_clk_divider(hw);

	return divider_round_rate(hw, rate, prate, divider->table,
				  divider->width, divider->flags);
}

static int rzv2h_ddiv_determine_rate(struct clk_hw *hw,
				     struct clk_rate_request *req)
{
	struct clk_divider *divider = to_clk_divider(hw);

	return divider_determine_rate(hw, req, divider->table, divider->width,
				      divider->flags);
}

static inline int rzv2h_cpg_wait_ddiv_clk_update_done(void __iomem *base, u8 mon)
{
	u32 bitmask = BIT(mon);
	u32 val;

	if (mon == CSDIV_NO_MON)
		return 0;

	return readl_poll_timeout_atomic(base + CPG_CLKSTATUS0, val, !(val & bitmask), 10, 200);
}

static int rzv2h_ddiv_set_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long parent_rate)
{
	struct clk_divider *divider = to_clk_divider(hw);
	struct ddiv_clk *ddiv = to_ddiv_clock(divider);
	struct rzv2h_cpg_priv *priv = ddiv->priv;
	unsigned long flags = 0;
	int value;
	u32 val;
	int ret;

	value = divider_get_val(rate, parent_rate, divider->table,
				divider->width, divider->flags);
	if (value < 0)
		return value;

	spin_lock_irqsave(divider->lock, flags);

	ret = rzv2h_cpg_wait_ddiv_clk_update_done(priv->base, ddiv->mon);
	if (ret)
		goto ddiv_timeout;

	val = readl(divider->reg) | DDIV_DIVCTL_WEN(divider->shift);
	val &= ~(clk_div_mask(divider->width) << divider->shift);
	val |= (u32)value << divider->shift;
	writel(val, divider->reg);

	ret = rzv2h_cpg_wait_ddiv_clk_update_done(priv->base, ddiv->mon);
	if (ret)
		goto ddiv_timeout;

	spin_unlock_irqrestore(divider->lock, flags);

	return 0;

ddiv_timeout:
	spin_unlock_irqrestore(divider->lock, flags);
	return ret;
}

static const struct clk_ops rzv2h_ddiv_clk_divider_ops = {
	.recalc_rate = rzv2h_ddiv_recalc_rate,
	.round_rate = rzv2h_ddiv_round_rate,
	.determine_rate = rzv2h_ddiv_determine_rate,
	.set_rate = rzv2h_ddiv_set_rate,
};

static struct clk * __init
rzv2h_cpg_ddiv_clk_register(const struct cpg_core_clk *core,
			    struct rzv2h_cpg_priv *priv)
{
	struct ddiv cfg_ddiv = core->cfg.ddiv;
	struct clk_init_data init = {};
	struct device *dev = priv->dev;
	u8 shift = cfg_ddiv.shift;
	u8 width = cfg_ddiv.width;
	const struct clk *parent;
	const char *parent_name;
	struct clk_divider *div;
	struct ddiv_clk *ddiv;
	int ret;

	parent = priv->clks[core->parent];
	if (IS_ERR(parent))
		return ERR_CAST(parent);

	parent_name = __clk_get_name(parent);

	if ((shift + width) > 16)
		return ERR_PTR(-EINVAL);

	ddiv = devm_kzalloc(priv->dev, sizeof(*ddiv), GFP_KERNEL);
	if (!ddiv)
		return ERR_PTR(-ENOMEM);

	init.name = core->name;
	init.ops = &rzv2h_ddiv_clk_divider_ops;
	init.parent_names = &parent_name;
	init.num_parents = 1;

	ddiv->priv = priv;
	ddiv->mon = cfg_ddiv.monbit;
	div = &ddiv->div;
	div->reg = priv->base + cfg_ddiv.offset;
	div->shift = shift;
	div->width = width;
	div->flags = core->flag;
	div->lock = &priv->rmw_lock;
	div->hw.init = &init;
	div->table = core->dtable;

	ret = devm_clk_hw_register(dev, &div->hw);
	if (ret)
		return ERR_PTR(ret);

	return div->hw.clk;
}

static struct clk * __init
rzv2h_cpg_mux_clk_register(const struct cpg_core_clk *core,
			   struct rzv2h_cpg_priv *priv)
{
	struct smuxed mux = core->cfg.smux;
	const struct clk_hw *clk_hw;

	clk_hw = devm_clk_hw_register_mux(priv->dev, core->name,
					  core->parent_names, core->num_parents,
					  core->flag, priv->base + mux.offset,
					  mux.shift, mux.width,
					  core->mux_flags, &priv->rmw_lock);
	if (IS_ERR(clk_hw))
		return ERR_CAST(clk_hw);

	return clk_hw->clk;
}

static struct clk
*rzv2h_cpg_clk_src_twocell_get(struct of_phandle_args *clkspec,
			       void *data)
{
	unsigned int clkidx = clkspec->args[1];
	struct rzv2h_cpg_priv *priv = data;
	struct device *dev = priv->dev;
	const char *type;
	struct clk *clk;

	switch (clkspec->args[0]) {
	case CPG_CORE:
		type = "core";
		if (clkidx > priv->last_dt_core_clk) {
			dev_err(dev, "Invalid %s clock index %u\n", type, clkidx);
			return ERR_PTR(-EINVAL);
		}
		clk = priv->clks[clkidx];
		break;

	case CPG_MOD:
		type = "module";
		if (clkidx >= priv->num_mod_clks) {
			dev_err(dev, "Invalid %s clock index %u\n", type, clkidx);
			return ERR_PTR(-EINVAL);
		}
		clk = priv->clks[priv->num_core_clks + clkidx];
		break;

	default:
		dev_err(dev, "Invalid CPG clock type %u\n", clkspec->args[0]);
		return ERR_PTR(-EINVAL);
	}

	if (IS_ERR(clk))
		dev_err(dev, "Cannot get %s clock %u: %ld", type, clkidx,
			PTR_ERR(clk));
	else
		dev_dbg(dev, "clock (%u, %u) is %pC at %lu Hz\n",
			clkspec->args[0], clkspec->args[1], clk,
			clk_get_rate(clk));
	return clk;
}

static void __init
rzv2h_cpg_register_core_clk(const struct cpg_core_clk *core,
			    struct rzv2h_cpg_priv *priv)
{
	struct clk *clk = ERR_PTR(-EOPNOTSUPP), *parent;
	unsigned int id = core->id, div = core->div;
	struct device *dev = priv->dev;
	const char *parent_name;
	struct clk_hw *clk_hw;

	WARN_DEBUG(id >= priv->num_core_clks);
	WARN_DEBUG(PTR_ERR(priv->clks[id]) != -ENOENT);

	switch (core->type) {
	case CLK_TYPE_IN:
		clk = of_clk_get_by_name(priv->dev->of_node, core->name);
		break;
	case CLK_TYPE_FF:
		WARN_DEBUG(core->parent >= priv->num_core_clks);
		parent = priv->clks[core->parent];
		if (IS_ERR(parent)) {
			clk = parent;
			goto fail;
		}

		parent_name = __clk_get_name(parent);
		clk_hw = devm_clk_hw_register_fixed_factor(dev, core->name,
							   parent_name, CLK_SET_RATE_PARENT,
							   core->mult, div);
		if (IS_ERR(clk_hw))
			clk = ERR_CAST(clk_hw);
		else
			clk = clk_hw->clk;
		break;
	case CLK_TYPE_PLL:
		clk = rzv2h_cpg_pll_clk_register(core, priv, &rzv2h_cpg_pll_ops);
		break;
	case CLK_TYPE_DDIV:
		clk = rzv2h_cpg_ddiv_clk_register(core, priv);
		break;
	case CLK_TYPE_SMUX:
		clk = rzv2h_cpg_mux_clk_register(core, priv);
		break;
	case CLK_TYPE_PLLDSI:
		clk = rzv2h_cpg_plldsi_clk_register(core, priv);
		break;
	case CLK_TYPE_PLLDSI_DIV:
		clk = rzv2h_cpg_plldsi_div_clk_register(core, priv);
		break;
	default:
		goto fail;
	}

	if (IS_ERR_OR_NULL(clk))
		goto fail;

	dev_dbg(dev, "Core clock %pC at %lu Hz\n", clk, clk_get_rate(clk));
	priv->clks[id] = clk;
	return;

fail:
	dev_err(dev, "Failed to register core clock %s: %ld\n",
		core->name, PTR_ERR(clk));
}

static void rzv2h_mod_clock_mstop_enable(struct rzv2h_cpg_priv *priv,
					 u32 mstop_data)
{
	unsigned long mstop_mask = FIELD_GET(BUS_MSTOP_BITS_MASK, mstop_data);
	u16 mstop_index = FIELD_GET(BUS_MSTOP_IDX_MASK, mstop_data);
	unsigned int index = (mstop_index - 1) * 16;
	atomic_t *mstop = &priv->mstop_count[index];
	unsigned long flags;
	unsigned int i;
	u32 val = 0;

	spin_lock_irqsave(&priv->rmw_lock, flags);
	for_each_set_bit(i, &mstop_mask, 16) {
		if (!atomic_read(&mstop[i]))
			val |= BIT(i) << 16;
		atomic_inc(&mstop[i]);
	}
	if (val)
		writel(val, priv->base + CPG_BUS_MSTOP(mstop_index));
	spin_unlock_irqrestore(&priv->rmw_lock, flags);
}

static void rzv2h_mod_clock_mstop_disable(struct rzv2h_cpg_priv *priv,
					  u32 mstop_data)
{
	unsigned long mstop_mask = FIELD_GET(BUS_MSTOP_BITS_MASK, mstop_data);
	u16 mstop_index = FIELD_GET(BUS_MSTOP_IDX_MASK, mstop_data);
	unsigned int index = (mstop_index - 1) * 16;
	atomic_t *mstop = &priv->mstop_count[index];
	unsigned long flags;
	unsigned int i;
	u32 val = 0;

	spin_lock_irqsave(&priv->rmw_lock, flags);
	for_each_set_bit(i, &mstop_mask, 16) {
		if (!atomic_read(&mstop[i]) ||
		    atomic_dec_and_test(&mstop[i]))
			val |= BIT(i) << 16 | BIT(i);
	}
	if (val)
		writel(val, priv->base + CPG_BUS_MSTOP(mstop_index));
	spin_unlock_irqrestore(&priv->rmw_lock, flags);
}

static int rzv2h_mod_clock_is_enabled(struct clk_hw *hw)
{
	struct mod_clock *clock = to_mod_clock(hw);
	struct rzv2h_cpg_priv *priv = clock->priv;
	u32 bitmask;
	u32 offset;

	if (clock->mon_index >= 0) {
		offset = GET_CLK_MON_OFFSET(clock->mon_index);
		bitmask = BIT(clock->mon_bit);
	} else {
		offset = GET_CLK_ON_OFFSET(clock->on_index);
		bitmask = BIT(clock->on_bit);
	}

	return readl(priv->base + offset) & bitmask;
}

static int rzv2h_mod_clock_endisable(struct clk_hw *hw, bool enable)
{
	bool enabled = rzv2h_mod_clock_is_enabled(hw);
	struct mod_clock *clock = to_mod_clock(hw);
	unsigned int reg = GET_CLK_ON_OFFSET(clock->on_index);
	struct rzv2h_cpg_priv *priv = clock->priv;
	u32 bitmask = BIT(clock->on_bit);
	struct device *dev = priv->dev;
	u32 value;
	int error;

	dev_dbg(dev, "CLK_ON 0x%x/%pC %s\n", reg, hw->clk,
		enable ? "ON" : "OFF");

	if (enabled == enable)
		return 0;

	value = bitmask << 16;
	if (enable) {
		value |= bitmask;
		writel(value, priv->base + reg);
		if (clock->mstop_data != BUS_MSTOP_NONE)
			rzv2h_mod_clock_mstop_enable(priv, clock->mstop_data);
	} else {
		if (clock->mstop_data != BUS_MSTOP_NONE)
			rzv2h_mod_clock_mstop_disable(priv, clock->mstop_data);
		writel(value, priv->base + reg);
	}

	if (!enable || clock->mon_index < 0)
		return 0;

	reg = GET_CLK_MON_OFFSET(clock->mon_index);
	bitmask = BIT(clock->mon_bit);
	error = readl_poll_timeout_atomic(priv->base + reg, value,
					  value & bitmask, 0, 10);
	if (error)
		dev_err(dev, "Failed to enable CLK_ON 0x%x/%pC\n",
			GET_CLK_ON_OFFSET(clock->on_index), hw->clk);

	return error;
}

static int rzv2h_mod_clock_enable(struct clk_hw *hw)
{
	return rzv2h_mod_clock_endisable(hw, true);
}

static void rzv2h_mod_clock_disable(struct clk_hw *hw)
{
	rzv2h_mod_clock_endisable(hw, false);
}

static const struct clk_ops rzv2h_mod_clock_ops = {
	.enable = rzv2h_mod_clock_enable,
	.disable = rzv2h_mod_clock_disable,
	.is_enabled = rzv2h_mod_clock_is_enabled,
};

static void __init
rzv2h_cpg_register_mod_clk(const struct rzv2h_mod_clk *mod,
			   struct rzv2h_cpg_priv *priv)
{
	struct mod_clock *clock = NULL;
	struct device *dev = priv->dev;
	struct clk_init_data init;
	struct clk *parent, *clk;
	const char *parent_name;
	unsigned int id;
	int ret;

	id = GET_MOD_CLK_ID(priv->num_core_clks, mod->on_index, mod->on_bit);
	WARN_DEBUG(id >= priv->num_core_clks + priv->num_mod_clks);
	WARN_DEBUG(mod->parent >= priv->num_core_clks + priv->num_mod_clks);
	WARN_DEBUG(PTR_ERR(priv->clks[id]) != -ENOENT);

	parent = priv->clks[mod->parent];
	if (IS_ERR(parent)) {
		clk = parent;
		goto fail;
	}

	clock = devm_kzalloc(dev, sizeof(*clock), GFP_KERNEL);
	if (!clock) {
		clk = ERR_PTR(-ENOMEM);
		goto fail;
	}

	init.name = mod->name;
	init.ops = &rzv2h_mod_clock_ops;
	init.flags = CLK_SET_RATE_PARENT;
	if (mod->critical)
		init.flags |= CLK_IS_CRITICAL;

	parent_name = __clk_get_name(parent);
	init.parent_names = &parent_name;
	init.num_parents = 1;

	clock->on_index = mod->on_index;
	clock->on_bit = mod->on_bit;
	clock->mon_index = mod->mon_index;
	clock->mon_bit = mod->mon_bit;
	clock->no_pm = mod->no_pm;
	clock->priv = priv;
	clock->hw.init = &init;
	clock->mstop_data = mod->mstop_data;

	ret = devm_clk_hw_register(dev, &clock->hw);
	if (ret) {
		clk = ERR_PTR(ret);
		goto fail;
	}

	priv->clks[id] = clock->hw.clk;

	/*
	 * Ensure the module clocks and MSTOP bits are synchronized when they are
	 * turned ON by the bootloader. Enable MSTOP bits for module clocks that were
	 * turned ON in an earlier boot stage.
	 */
	if (clock->mstop_data != BUS_MSTOP_NONE &&
	    !mod->critical && rzv2h_mod_clock_is_enabled(&clock->hw)) {
		rzv2h_mod_clock_mstop_enable(priv, clock->mstop_data);
	} else if (clock->mstop_data != BUS_MSTOP_NONE && mod->critical) {
		unsigned long mstop_mask = FIELD_GET(BUS_MSTOP_BITS_MASK, clock->mstop_data);
		u16 mstop_index = FIELD_GET(BUS_MSTOP_IDX_MASK, clock->mstop_data);
		unsigned int index = (mstop_index - 1) * 16;
		atomic_t *mstop = &priv->mstop_count[index];
		unsigned long flags;
		unsigned int i;
		u32 val = 0;

		/*
		 * Critical clocks are turned ON immediately upon registration, and the
		 * MSTOP counter is updated through the rzv2h_mod_clock_enable() path.
		 * However, if the critical clocks were already turned ON by the initial
		 * bootloader, synchronize the atomic counter here and clear the MSTOP bit.
		 */
		spin_lock_irqsave(&priv->rmw_lock, flags);
		for_each_set_bit(i, &mstop_mask, 16) {
			if (atomic_read(&mstop[i]))
				continue;
			val |= BIT(i) << 16;
			atomic_inc(&mstop[i]);
		}
		if (val)
			writel(val, priv->base + CPG_BUS_MSTOP(mstop_index));
		spin_unlock_irqrestore(&priv->rmw_lock, flags);
	}

	return;

fail:
	dev_err(dev, "Failed to register module clock %s: %ld\n",
		mod->name, PTR_ERR(clk));
}

static int rzv2h_cpg_assert(struct reset_controller_dev *rcdev,
			    unsigned long id)
{
	struct rzv2h_cpg_priv *priv = rcdev_to_priv(rcdev);
	unsigned int reg = GET_RST_OFFSET(priv->resets[id].reset_index);
	u32 mask = BIT(priv->resets[id].reset_bit);
	u8 monbit = priv->resets[id].mon_bit;
	u32 value = mask << 16;

	dev_dbg(rcdev->dev, "assert id:%ld offset:0x%x\n", id, reg);

	writel(value, priv->base + reg);

	reg = GET_RST_MON_OFFSET(priv->resets[id].mon_index);
	mask = BIT(monbit);

	return readl_poll_timeout_atomic(priv->base + reg, value,
					 value & mask, 10, 200);
}

static int rzv2h_cpg_deassert(struct reset_controller_dev *rcdev,
			      unsigned long id)
{
	struct rzv2h_cpg_priv *priv = rcdev_to_priv(rcdev);
	unsigned int reg = GET_RST_OFFSET(priv->resets[id].reset_index);
	u32 mask = BIT(priv->resets[id].reset_bit);
	u8 monbit = priv->resets[id].mon_bit;
	u32 value = (mask << 16) | mask;

	dev_dbg(rcdev->dev, "deassert id:%ld offset:0x%x\n", id, reg);

	writel(value, priv->base + reg);

	reg = GET_RST_MON_OFFSET(priv->resets[id].mon_index);
	mask = BIT(monbit);

	return readl_poll_timeout_atomic(priv->base + reg, value,
					 !(value & mask), 10, 200);
}

static int rzv2h_cpg_reset(struct reset_controller_dev *rcdev,
			   unsigned long id)
{
	int ret;

	ret = rzv2h_cpg_assert(rcdev, id);
	if (ret)
		return ret;

	return rzv2h_cpg_deassert(rcdev, id);
}

static int rzv2h_cpg_status(struct reset_controller_dev *rcdev,
			    unsigned long id)
{
	struct rzv2h_cpg_priv *priv = rcdev_to_priv(rcdev);
	unsigned int reg = GET_RST_MON_OFFSET(priv->resets[id].mon_index);
	u8 monbit = priv->resets[id].mon_bit;

	return !!(readl(priv->base + reg) & BIT(monbit));
}

static const struct reset_control_ops rzv2h_cpg_reset_ops = {
	.reset = rzv2h_cpg_reset,
	.assert = rzv2h_cpg_assert,
	.deassert = rzv2h_cpg_deassert,
	.status = rzv2h_cpg_status,
};

static int rzv2h_cpg_reset_xlate(struct reset_controller_dev *rcdev,
				 const struct of_phandle_args *reset_spec)
{
	struct rzv2h_cpg_priv *priv = rcdev_to_priv(rcdev);
	unsigned int id = reset_spec->args[0];
	u8 rst_index = id / 16;
	u8 rst_bit = id % 16;
	unsigned int i;

	for (i = 0; i < rcdev->nr_resets; i++) {
		if (rst_index == priv->resets[i].reset_index &&
		    rst_bit == priv->resets[i].reset_bit)
			return i;
	}

	return -EINVAL;
}

static int rzv2h_cpg_reset_controller_register(struct rzv2h_cpg_priv *priv)
{
	priv->rcdev.ops = &rzv2h_cpg_reset_ops;
	priv->rcdev.of_node = priv->dev->of_node;
	priv->rcdev.dev = priv->dev;
	priv->rcdev.of_reset_n_cells = 1;
	priv->rcdev.of_xlate = rzv2h_cpg_reset_xlate;
	priv->rcdev.nr_resets = priv->num_resets;

	return devm_reset_controller_register(priv->dev, &priv->rcdev);
}

/**
 * struct rzv2h_cpg_pd - RZ/V2H power domain data structure
 * @priv: pointer to CPG private data structure
 * @genpd: generic PM domain
 */
struct rzv2h_cpg_pd {
	struct rzv2h_cpg_priv *priv;
	struct generic_pm_domain genpd;
};

static bool rzv2h_cpg_is_pm_clk(struct rzv2h_cpg_pd *pd,
				const struct of_phandle_args *clkspec)
{
	if (clkspec->np != pd->genpd.dev.of_node || clkspec->args_count != 2)
		return false;

	switch (clkspec->args[0]) {
	case CPG_MOD: {
		struct rzv2h_cpg_priv *priv = pd->priv;
		unsigned int id = clkspec->args[1];
		struct mod_clock *clock;

		if (id >= priv->num_mod_clks)
			return false;

		if (priv->clks[priv->num_core_clks + id] == ERR_PTR(-ENOENT))
			return false;

		clock = to_mod_clock(__clk_get_hw(priv->clks[priv->num_core_clks + id]));

		return !clock->no_pm;
	}

	case CPG_CORE:
	default:
		return false;
	}
}

static int rzv2h_cpg_attach_dev(struct generic_pm_domain *domain, struct device *dev)
{
	struct rzv2h_cpg_pd *pd = container_of(domain, struct rzv2h_cpg_pd, genpd);
	struct device_node *np = dev->of_node;
	struct of_phandle_args clkspec;
	bool once = true;
	struct clk *clk;
	unsigned int i;
	int error;

	for (i = 0; !of_parse_phandle_with_args(np, "clocks", "#clock-cells", i, &clkspec); i++) {
		if (!rzv2h_cpg_is_pm_clk(pd, &clkspec)) {
			of_node_put(clkspec.np);
			continue;
		}

		if (once) {
			once = false;
			error = pm_clk_create(dev);
			if (error) {
				of_node_put(clkspec.np);
				goto err;
			}
		}
		clk = of_clk_get_from_provider(&clkspec);
		of_node_put(clkspec.np);
		if (IS_ERR(clk)) {
			error = PTR_ERR(clk);
			goto fail_destroy;
		}

		error = pm_clk_add_clk(dev, clk);
		if (error) {
			dev_err(dev, "pm_clk_add_clk failed %d\n",
				error);
			goto fail_put;
		}
	}

	return 0;

fail_put:
	clk_put(clk);

fail_destroy:
	pm_clk_destroy(dev);
err:
	return error;
}

static void rzv2h_cpg_detach_dev(struct generic_pm_domain *unused, struct device *dev)
{
	if (!pm_clk_no_clocks(dev))
		pm_clk_destroy(dev);
}

static void rzv2h_cpg_genpd_remove_simple(void *data)
{
	pm_genpd_remove(data);
}

static int __init rzv2h_cpg_add_pm_domains(struct rzv2h_cpg_priv *priv)
{
	struct device *dev = priv->dev;
	struct device_node *np = dev->of_node;
	struct rzv2h_cpg_pd *pd;
	int ret;

	pd = devm_kzalloc(dev, sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return -ENOMEM;

	pd->genpd.name = np->name;
	pd->priv = priv;
	pd->genpd.flags |= GENPD_FLAG_ALWAYS_ON | GENPD_FLAG_PM_CLK | GENPD_FLAG_ACTIVE_WAKEUP;
	pd->genpd.attach_dev = rzv2h_cpg_attach_dev;
	pd->genpd.detach_dev = rzv2h_cpg_detach_dev;
	ret = pm_genpd_init(&pd->genpd, &pm_domain_always_on_gov, false);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(dev, rzv2h_cpg_genpd_remove_simple, &pd->genpd);
	if (ret)
		return ret;

	return of_genpd_add_provider_simple(np, &pd->genpd);
}

static void rzv2h_cpg_del_clk_provider(void *data)
{
	of_clk_del_provider(data);
}

static int __init rzv2h_cpg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const struct rzv2h_cpg_info *info;
	struct rzv2h_cpg_priv *priv;
	unsigned int nclks, i;
	struct clk **clks;
	int error;

	info = of_device_get_match_data(dev);

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	spin_lock_init(&priv->rmw_lock);

	priv->dev = dev;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	nclks = info->num_total_core_clks + info->num_hw_mod_clks;
	clks = devm_kmalloc_array(dev, nclks, sizeof(*clks), GFP_KERNEL);
	if (!clks)
		return -ENOMEM;

	priv->mstop_count = devm_kcalloc(dev, info->num_mstop_bits,
					 sizeof(*priv->mstop_count), GFP_KERNEL);
	if (!priv->mstop_count)
		return -ENOMEM;

	priv->resets = devm_kmemdup(dev, info->resets, sizeof(*info->resets) *
				    info->num_resets, GFP_KERNEL);
	if (!priv->resets)
		return -ENOMEM;

	dev_set_drvdata(dev, priv);
	priv->clks = clks;
	priv->num_core_clks = info->num_total_core_clks;
	priv->num_mod_clks = info->num_hw_mod_clks;
	priv->last_dt_core_clk = info->last_dt_core_clk;
	priv->num_resets = info->num_resets;

	for (i = 0; i < nclks; i++)
		clks[i] = ERR_PTR(-ENOENT);

	for (i = 0; i < info->num_core_clks; i++)
		rzv2h_cpg_register_core_clk(&info->core_clks[i], priv);

	for (i = 0; i < info->num_mod_clks; i++)
		rzv2h_cpg_register_mod_clk(&info->mod_clks[i], priv);

	error = of_clk_add_provider(np, rzv2h_cpg_clk_src_twocell_get, priv);
	if (error)
		return error;

	error = devm_add_action_or_reset(dev, rzv2h_cpg_del_clk_provider, np);
	if (error)
		return error;

	error = rzv2h_cpg_add_pm_domains(priv);
	if (error)
		return error;

	error = rzv2h_cpg_reset_controller_register(priv);
	if (error)
		return error;

	return 0;
}

static const struct of_device_id rzv2h_cpg_match[] = {
#ifdef CONFIG_CLK_R9A09G057
	{
		.compatible = "renesas,r9a09g057-cpg",
		.data = &r9a09g057_cpg_info,
	},
#endif
#ifdef CONFIG_CLK_R9A09G047
	{
		.compatible = "renesas,r9a09g047-cpg",
		.data = &r9a09g047_cpg_info,
	},
#endif
	{ /* sentinel */ }
};

static struct platform_driver rzv2h_cpg_driver = {
	.driver		= {
		.name	= "rzv2h-cpg",
		.of_match_table = rzv2h_cpg_match,
	},
};

static int __init rzv2h_cpg_init(void)
{
	return platform_driver_probe(&rzv2h_cpg_driver, rzv2h_cpg_probe);
}

subsys_initcall(rzv2h_cpg_init);

MODULE_DESCRIPTION("Renesas RZ/V2H CPG Driver");
