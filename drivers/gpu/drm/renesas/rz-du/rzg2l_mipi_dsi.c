// SPDX-License-Identifier: GPL-2.0
/*
 * RZ/G2L MIPI DSI Encoder Driver
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 */
#include <linux/clk.h>
#include <linux/clk/renesas-rzv2h-dsi.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/units.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#include "rzg2l_mipi_dsi_regs.h"

struct rzg2l_mipi_dsi;

#define RZV2H_MIPI_DPHY_FOUT_MIN_IN_MEGA	(80 * MEGA)
#define RZV2H_MIPI_DPHY_FOUT_MAX_IN_MEGA	(1500 * MEGA)

#define RZ_MIPI_DSI_16BPP	BIT(0)
#define RZ_MIPI_DSI_HASLPCLK	BIT(1)

struct rzg2l_mipi_dsi_hw_info {
	int (*dphy_init)(struct rzg2l_mipi_dsi *dsi, unsigned long long hsfreq_mhz);
	void (*dphy_late_init)(struct rzg2l_mipi_dsi *dsi);
	void (*dphy_exit)(struct rzg2l_mipi_dsi *dsi);
	int (*dphy_conf_clks)(struct rzg2l_mipi_dsi *dsi, unsigned long mode_freq,
			      unsigned long long *hsfreq_mhz);
	unsigned int (*dphy_mode_clk_check)(struct rzg2l_mipi_dsi *dsi,
					    unsigned long mode_freq);
	const struct rzv2h_plldsi_div_limits *cpg_dsi_limits;
	u32 phy_reg_offset;
	u32 link_reg_offset;
	unsigned long max_dclk;
	unsigned long min_dclk;
	bool has_dphy_rstc;
	u8 features;
};

struct rzv2h_dsi_mode_calc {
	unsigned long mode_freq;
	unsigned long long mode_freq_hz;
};

struct rzg2l_mipi_dsi {
	struct device *dev;
	void __iomem *mmio;

	const struct rzg2l_mipi_dsi_hw_info *info;

	struct reset_control *rstc;
	struct reset_control *arstc;
	struct reset_control *prstc;

	struct mipi_dsi_host host;
	struct drm_bridge bridge;
	struct drm_bridge *next_bridge;

	struct clk *vclk;
	struct clk *lpclk;

	enum mipi_dsi_pixel_format format;
	unsigned int num_data_lanes;
	unsigned int lanes;
	unsigned long mode_flags;

	struct rzv2h_dsi_mode_calc mode_calc;
	struct rzv2h_plldsi_parameters dsi_parameters;
};

static const struct rzv2h_plldsi_div_limits rzv2h_plldsi_div_limits = {
	.m = { .min = 64, .max = 1023 },
	.p = { .min = 1, .max = 4 },
	.s = { .min = 0, .max = 5 },
	.k = { .min = -32768, .max = 32767 },
	.csdiv = { .min = 1, .max = 1 },
	.fvco = { .min = 1050 * MEGA, .max = 2100 * MEGA }
};

static inline struct rzg2l_mipi_dsi *
bridge_to_rzg2l_mipi_dsi(struct drm_bridge *bridge)
{
	return container_of(bridge, struct rzg2l_mipi_dsi, bridge);
}

static inline struct rzg2l_mipi_dsi *
host_to_rzg2l_mipi_dsi(struct mipi_dsi_host *host)
{
	return container_of(host, struct rzg2l_mipi_dsi, host);
}

struct rzg2l_mipi_dsi_timings {
	unsigned long hsfreq_max;
	u32 t_init;
	u32 tclk_prepare;
	u32 ths_prepare;
	u32 tclk_zero;
	u32 tclk_pre;
	u32 tclk_post;
	u32 tclk_trail;
	u32 ths_zero;
	u32 ths_trail;
	u32 ths_exit;
	u32 tlpx;
};

static const struct rzg2l_mipi_dsi_timings rzg2l_mipi_dsi_global_timings[] = {
	{
		.hsfreq_max = 80000,
		.t_init = 79801,
		.tclk_prepare = 8,
		.ths_prepare = 13,
		.tclk_zero = 33,
		.tclk_pre = 24,
		.tclk_post = 94,
		.tclk_trail = 10,
		.ths_zero = 23,
		.ths_trail = 17,
		.ths_exit = 13,
		.tlpx = 6,
	},
	{
		.hsfreq_max = 125000,
		.t_init = 79801,
		.tclk_prepare = 8,
		.ths_prepare = 12,
		.tclk_zero = 33,
		.tclk_pre = 15,
		.tclk_post = 94,
		.tclk_trail = 10,
		.ths_zero = 23,
		.ths_trail = 17,
		.ths_exit = 13,
		.tlpx = 6,
	},
	{
		.hsfreq_max = 250000,
		.t_init = 79801,
		.tclk_prepare = 8,
		.ths_prepare = 12,
		.tclk_zero = 33,
		.tclk_pre = 13,
		.tclk_post = 94,
		.tclk_trail = 10,
		.ths_zero = 23,
		.ths_trail = 16,
		.ths_exit = 13,
		.tlpx = 6,
	},
	{
		.hsfreq_max = 360000,
		.t_init = 79801,
		.tclk_prepare = 8,
		.ths_prepare = 10,
		.tclk_zero = 33,
		.tclk_pre = 4,
		.tclk_post = 35,
		.tclk_trail = 7,
		.ths_zero = 16,
		.ths_trail = 9,
		.ths_exit = 13,
		.tlpx = 6,
	},
	{
		.hsfreq_max = 720000,
		.t_init = 79801,
		.tclk_prepare = 8,
		.ths_prepare = 9,
		.tclk_zero = 33,
		.tclk_pre = 4,
		.tclk_post = 35,
		.tclk_trail = 7,
		.ths_zero = 16,
		.ths_trail = 9,
		.ths_exit = 13,
		.tlpx = 6,
	},
	{
		.hsfreq_max = 1500000,
		.t_init = 79801,
		.tclk_prepare = 8,
		.ths_prepare = 9,
		.tclk_zero = 33,
		.tclk_pre = 4,
		.tclk_post = 35,
		.tclk_trail = 7,
		.ths_zero = 16,
		.ths_trail = 9,
		.ths_exit = 13,
		.tlpx = 6,
	},
};

struct rzv2h_mipi_dsi_timings {
	unsigned long hsfreq;
	u16 value;
};

static const struct rzv2h_mipi_dsi_timings TCLKPRPRCTL[] = {
	{150000000UL, 0},
	{260000000UL, 1},
	{370000000UL, 2},
	{470000000UL, 3},
	{580000000UL, 4},
	{690000000UL, 5},
	{790000000UL, 6},
	{900000000UL, 7},
	{1010000000UL, 8},
	{1110000000UL, 9},
	{1220000000UL, 10},
	{1330000000UL, 11},
	{1430000000UL, 12},
	{1500000000UL, 13},
};

static const struct rzv2h_mipi_dsi_timings TCLKZEROCTL[] = {
	{90000000UL, 2},
	{110000000UL, 3},
	{130000000UL, 4},
	{150000000UL, 5},
	{180000000UL, 6},
	{210000000UL, 7},
	{230000000UL, 8},
	{240000000UL, 9},
	{250000000UL, 10},
	{270000000UL, 11},
	{290000000UL, 12},
	{310000000UL, 13},
	{340000000UL, 14},
	{360000000UL, 15},
	{380000000UL, 16},
	{410000000UL, 17},
	{430000000UL, 18},
	{450000000UL, 19},
	{470000000UL, 20},
	{500000000UL, 21},
	{520000000UL, 22},
	{540000000UL, 23},
	{570000000UL, 24},
	{590000000UL, 25},
	{610000000UL, 26},
	{630000000UL, 27},
	{660000000UL, 28},
	{680000000UL, 29},
	{700000000UL, 30},
	{730000000UL, 31},
	{750000000UL, 32},
	{770000000UL, 33},
	{790000000UL, 34},
	{820000000UL, 35},
	{840000000UL, 36},
	{860000000UL, 37},
	{890000000UL, 38},
	{910000000UL, 39},
	{930000000UL, 40},
	{950000000UL, 41},
	{980000000UL, 42},
	{1000000000UL, 43},
	{1020000000UL, 44},
	{1050000000UL, 45},
	{1070000000UL, 46},
	{1090000000UL, 47},
	{1110000000UL, 48},
	{1140000000UL, 49},
	{1160000000UL, 50},
	{1180000000UL, 51},
	{1210000000UL, 52},
	{1230000000UL, 53},
	{1250000000UL, 54},
	{1270000000UL, 55},
	{1300000000UL, 56},
	{1320000000UL, 57},
	{1340000000UL, 58},
	{1370000000UL, 59},
	{1390000000UL, 60},
	{1410000000UL, 61},
	{1430000000UL, 62},
	{1460000000UL, 63},
	{1480000000UL, 64},
	{1500000000UL, 65},
};

static const struct rzv2h_mipi_dsi_timings TCLKPOSTCTL[] = {
	{80000000UL, 6},
	{210000000UL, 7},
	{340000000UL, 8},
	{480000000UL, 9},
	{610000000UL, 10},
	{740000000UL, 11},
	{880000000UL, 12},
	{1010000000UL, 13},
	{1140000000UL, 14},
	{1280000000UL, 15},
	{1410000000UL, 16},
	{1500000000UL, 17},
};

static const struct rzv2h_mipi_dsi_timings TCLKTRAILCTL[] = {
	{140000000UL, 1},
	{250000000UL, 2},
	{370000000UL, 3},
	{480000000UL, 4},
	{590000000UL, 5},
	{710000000UL, 6},
	{820000000UL, 7},
	{940000000UL, 8},
	{1050000000UL, 9},
	{1170000000UL, 10},
	{1280000000UL, 11},
	{1390000000UL, 12},
	{1500000000UL, 13},
};

static const struct rzv2h_mipi_dsi_timings THSPRPRCTL[] = {
	{110000000UL, 0},
	{190000000UL, 1},
	{290000000UL, 2},
	{400000000UL, 3},
	{500000000UL, 4},
	{610000000UL, 5},
	{720000000UL, 6},
	{820000000UL, 7},
	{930000000UL, 8},
	{1030000000UL, 9},
	{1140000000UL, 10},
	{1250000000UL, 11},
	{1350000000UL, 12},
	{1460000000UL, 13},
	{1500000000UL, 14},
};

static const struct rzv2h_mipi_dsi_timings THSZEROCTL[] = {
	{180000000UL, 0},
	{240000000UL, 1},
	{290000000UL, 2},
	{350000000UL, 3},
	{400000000UL, 4},
	{460000000UL, 5},
	{510000000UL, 6},
	{570000000UL, 7},
	{620000000UL, 8},
	{680000000UL, 9},
	{730000000UL, 10},
	{790000000UL, 11},
	{840000000UL, 12},
	{900000000UL, 13},
	{950000000UL, 14},
	{1010000000UL, 15},
	{1060000000UL, 16},
	{1120000000UL, 17},
	{1170000000UL, 18},
	{1230000000UL, 19},
	{1280000000UL, 20},
	{1340000000UL, 21},
	{1390000000UL, 22},
	{1450000000UL, 23},
	{1500000000UL, 24},
};

static const struct rzv2h_mipi_dsi_timings THSTRAILCTL[] = {
	{100000000UL, 3},
	{210000000UL, 4},
	{320000000UL, 5},
	{420000000UL, 6},
	{530000000UL, 7},
	{640000000UL, 8},
	{750000000UL, 9},
	{850000000UL, 10},
	{960000000UL, 11},
	{1070000000UL, 12},
	{1180000000UL, 13},
	{1280000000UL, 14},
	{1390000000UL, 15},
	{1500000000UL, 16},
};

static const struct rzv2h_mipi_dsi_timings TLPXCTL[] = {
	{130000000UL, 0},
	{260000000UL, 1},
	{390000000UL, 2},
	{530000000UL, 3},
	{660000000UL, 4},
	{790000000UL, 5},
	{930000000UL, 6},
	{1060000000UL, 7},
	{1190000000UL, 8},
	{1330000000UL, 9},
	{1460000000UL, 10},
	{1500000000UL, 11},
};

static const struct rzv2h_mipi_dsi_timings THSEXITCTL[] = {
	{150000000UL, 1},
	{230000000UL, 2},
	{310000000UL, 3},
	{390000000UL, 4},
	{470000000UL, 5},
	{550000000UL, 6},
	{630000000UL, 7},
	{710000000UL, 8},
	{790000000UL, 9},
	{870000000UL, 10},
	{950000000UL, 11},
	{1030000000UL, 12},
	{1110000000UL, 13},
	{1190000000UL, 14},
	{1270000000UL, 15},
	{1350000000UL, 16},
	{1430000000UL, 17},
	{1500000000UL, 18},
};

static const struct rzv2h_mipi_dsi_timings ULPSEXIT[] = {
	{1953125UL, 49},
	{3906250UL, 98},
	{7812500UL, 195},
	{15625000UL, 391},
};

static int rzv2h_dphy_find_timings_val(unsigned long freq,
				       const struct rzv2h_mipi_dsi_timings timings[],
				       unsigned int size)
{
	unsigned int i;

	for (i = 0; i < size; i++) {
		if (freq <= timings[i].hsfreq)
			break;
	}

	if (i == size)
		i -= 1;

	return timings[i].value;
};

static void rzg2l_mipi_dsi_phy_write(struct rzg2l_mipi_dsi *dsi, u32 reg, u32 data)
{
	iowrite32(data, dsi->mmio + dsi->info->phy_reg_offset + reg);
}

static void rzg2l_mipi_dsi_link_write(struct rzg2l_mipi_dsi *dsi, u32 reg, u32 data)
{
	iowrite32(data, dsi->mmio + dsi->info->link_reg_offset + reg);
}

static u32 rzg2l_mipi_dsi_phy_read(struct rzg2l_mipi_dsi *dsi, u32 reg)
{
	return ioread32(dsi->mmio + dsi->info->phy_reg_offset + reg);
}

static u32 rzg2l_mipi_dsi_link_read(struct rzg2l_mipi_dsi *dsi, u32 reg)
{
	return ioread32(dsi->mmio + dsi->info->link_reg_offset + reg);
}

/* -----------------------------------------------------------------------------
 * Hardware Setup
 */

static int rzg2l_mipi_dsi_dphy_init(struct rzg2l_mipi_dsi *dsi,
				    unsigned long long hsfreq_mhz)
{
	unsigned long hsfreq = DIV_ROUND_CLOSEST_ULL(hsfreq_mhz, KILO);
	const struct rzg2l_mipi_dsi_timings *dphy_timings;
	unsigned int i;
	u32 dphyctrl0;
	u32 dphytim0;
	u32 dphytim1;
	u32 dphytim2;
	u32 dphytim3;
	int ret;

	/* All DSI global operation timings are set with recommended setting */
	for (i = 0; i < ARRAY_SIZE(rzg2l_mipi_dsi_global_timings); ++i) {
		dphy_timings = &rzg2l_mipi_dsi_global_timings[i];
		if (hsfreq <= (dphy_timings->hsfreq_max * KILO))
			break;
	}

	/* Initializing DPHY before accessing LINK */
	dphyctrl0 = DSIDPHYCTRL0_CAL_EN_HSRX_OFS | DSIDPHYCTRL0_CMN_MASTER_EN |
		    DSIDPHYCTRL0_RE_VDD_DETVCCQLV18 | DSIDPHYCTRL0_EN_BGR;

	rzg2l_mipi_dsi_phy_write(dsi, DSIDPHYCTRL0, dphyctrl0);
	usleep_range(20, 30);

	dphyctrl0 |= DSIDPHYCTRL0_EN_LDO1200;
	rzg2l_mipi_dsi_phy_write(dsi, DSIDPHYCTRL0, dphyctrl0);
	usleep_range(10, 20);

	dphytim0 = DSIDPHYTIM0_TCLK_MISS(0) |
		   DSIDPHYTIM0_T_INIT(dphy_timings->t_init);
	dphytim1 = DSIDPHYTIM1_THS_PREPARE(dphy_timings->ths_prepare) |
		   DSIDPHYTIM1_TCLK_PREPARE(dphy_timings->tclk_prepare) |
		   DSIDPHYTIM1_THS_SETTLE(0) |
		   DSIDPHYTIM1_TCLK_SETTLE(0);
	dphytim2 = DSIDPHYTIM2_TCLK_TRAIL(dphy_timings->tclk_trail) |
		   DSIDPHYTIM2_TCLK_POST(dphy_timings->tclk_post) |
		   DSIDPHYTIM2_TCLK_PRE(dphy_timings->tclk_pre) |
		   DSIDPHYTIM2_TCLK_ZERO(dphy_timings->tclk_zero);
	dphytim3 = DSIDPHYTIM3_TLPX(dphy_timings->tlpx) |
		   DSIDPHYTIM3_THS_EXIT(dphy_timings->ths_exit) |
		   DSIDPHYTIM3_THS_TRAIL(dphy_timings->ths_trail) |
		   DSIDPHYTIM3_THS_ZERO(dphy_timings->ths_zero);

	rzg2l_mipi_dsi_phy_write(dsi, DSIDPHYTIM0, dphytim0);
	rzg2l_mipi_dsi_phy_write(dsi, DSIDPHYTIM1, dphytim1);
	rzg2l_mipi_dsi_phy_write(dsi, DSIDPHYTIM2, dphytim2);
	rzg2l_mipi_dsi_phy_write(dsi, DSIDPHYTIM3, dphytim3);

	ret = reset_control_deassert(dsi->rstc);
	if (ret < 0)
		return ret;

	udelay(1);

	return 0;
}

static void rzg2l_mipi_dsi_dphy_exit(struct rzg2l_mipi_dsi *dsi)
{
	u32 dphyctrl0;

	dphyctrl0 = rzg2l_mipi_dsi_phy_read(dsi, DSIDPHYCTRL0);

	dphyctrl0 &= ~(DSIDPHYCTRL0_EN_LDO1200 | DSIDPHYCTRL0_EN_BGR);
	rzg2l_mipi_dsi_phy_write(dsi, DSIDPHYCTRL0, dphyctrl0);

	reset_control_assert(dsi->rstc);
}

static int rzg2l_dphy_conf_clks(struct rzg2l_mipi_dsi *dsi, unsigned long mode_freq,
				unsigned long long *hsfreq_mhz)
{
	unsigned long vclk_rate;
	unsigned int bpp;

	clk_set_rate(dsi->vclk, mode_freq * KILO);
	/*
	 * Relationship between hsclk and vclk must follow
	 * vclk * bpp = hsclk * 8 * lanes
	 * where vclk: video clock (Hz)
	 *       bpp: video pixel bit depth
	 *       hsclk: DSI HS Byte clock frequency (Hz)
	 *       lanes: number of data lanes
	 *
	 * hsclk(bit) = hsclk(byte) * 8 = hsfreq
	 */
	bpp = mipi_dsi_pixel_format_to_bpp(dsi->format);
	vclk_rate = clk_get_rate(dsi->vclk);
	*hsfreq_mhz = DIV_ROUND_CLOSEST_ULL(vclk_rate * bpp * KILO * 1ULL,
					    dsi->lanes);

	return 0;
}

static unsigned int rzv2h_dphy_mode_clk_check(struct rzg2l_mipi_dsi *dsi,
					      unsigned long mode_freq)
{
	struct rzv2h_plldsi_parameters *dsi_parameters = &dsi->dsi_parameters;
	unsigned long long hsfreq_mhz, mode_freq_hz, mode_freq_mhz;
	struct rzv2h_plldsi_parameters cpg_dsi_parameters;
	unsigned int bpp, i;

	bpp = mipi_dsi_pixel_format_to_bpp(dsi->format);

	for (i = 0; i < 10; i += 1) {
		unsigned long hsfreq;
		bool parameters_found;

		mode_freq_hz = mode_freq * KILO + i;
		mode_freq_mhz = mode_freq_hz * KILO * 1ULL;
		parameters_found = rzv2h_dsi_get_pll_parameters_values(dsi->info->cpg_dsi_limits,
								       &cpg_dsi_parameters,
								       mode_freq_mhz);
		if (!parameters_found)
			continue;

		hsfreq_mhz = DIV_ROUND_CLOSEST_ULL(cpg_dsi_parameters.freq_mhz * bpp, dsi->lanes);
		parameters_found = rzv2h_dsi_get_pll_parameters_values(&rzv2h_plldsi_div_limits,
								       dsi_parameters,
								       hsfreq_mhz);
		if (!parameters_found)
			continue;

		if (abs(dsi_parameters->error_mhz) >= 500)
			continue;

		hsfreq = DIV_ROUND_CLOSEST_ULL(hsfreq_mhz, KILO);
		if (hsfreq >= RZV2H_MIPI_DPHY_FOUT_MIN_IN_MEGA &&
		    hsfreq <= RZV2H_MIPI_DPHY_FOUT_MAX_IN_MEGA) {
			dsi->mode_calc.mode_freq_hz = mode_freq_hz;
			dsi->mode_calc.mode_freq = mode_freq;
			return MODE_OK;
		}
	}

	return MODE_CLOCK_RANGE;
}

static int rzv2h_dphy_conf_clks(struct rzg2l_mipi_dsi *dsi, unsigned long mode_freq,
				unsigned long long *hsfreq_mhz)
{
	struct rzv2h_plldsi_parameters *dsi_parameters = &dsi->dsi_parameters;
	unsigned long status;

	if (dsi->mode_calc.mode_freq != mode_freq) {
		status = rzv2h_dphy_mode_clk_check(dsi, mode_freq);
		if (status != MODE_OK) {
			dev_err(dsi->dev, "No PLL parameters found for mode clk %lu\n",
				mode_freq);
			return -EINVAL;
		}
	}

	clk_set_rate(dsi->vclk, dsi->mode_calc.mode_freq_hz);
	*hsfreq_mhz = dsi_parameters->freq_mhz;

	return 0;
}

static int rzv2h_mipi_dsi_dphy_init(struct rzg2l_mipi_dsi *dsi,
				    unsigned long long hsfreq_mhz)
{
	struct rzv2h_plldsi_parameters *dsi_parameters = &dsi->dsi_parameters;
	unsigned long lpclk_rate = clk_get_rate(dsi->lpclk);
	u32 phytclksetr, phythssetr, phytlpxsetr, phycr;
	struct rzg2l_mipi_dsi_timings dphy_timings;
	unsigned long long hsfreq;
	u32 ulpsexit;

	hsfreq = DIV_ROUND_CLOSEST_ULL(hsfreq_mhz, KILO);

	if (dsi_parameters->freq_mhz == hsfreq_mhz)
		goto parameters_found;

	if (rzv2h_dsi_get_pll_parameters_values(&rzv2h_plldsi_div_limits,
						dsi_parameters, hsfreq_mhz))
		goto parameters_found;

	dev_err(dsi->dev, "No PLL parameters found for HSFREQ %lluHz\n", hsfreq);
	return -EINVAL;

parameters_found:
	dphy_timings.tclk_trail =
		rzv2h_dphy_find_timings_val(hsfreq, TCLKTRAILCTL,
					    ARRAY_SIZE(TCLKTRAILCTL));
	dphy_timings.tclk_post =
		rzv2h_dphy_find_timings_val(hsfreq, TCLKPOSTCTL,
					    ARRAY_SIZE(TCLKPOSTCTL));
	dphy_timings.tclk_zero =
		rzv2h_dphy_find_timings_val(hsfreq, TCLKZEROCTL,
					    ARRAY_SIZE(TCLKZEROCTL));
	dphy_timings.tclk_prepare =
		rzv2h_dphy_find_timings_val(hsfreq, TCLKPRPRCTL,
					    ARRAY_SIZE(TCLKPRPRCTL));
	dphy_timings.ths_exit =
		rzv2h_dphy_find_timings_val(hsfreq, THSEXITCTL,
					    ARRAY_SIZE(THSEXITCTL));
	dphy_timings.ths_trail =
		rzv2h_dphy_find_timings_val(hsfreq, THSTRAILCTL,
					    ARRAY_SIZE(THSTRAILCTL));
	dphy_timings.ths_zero =
		rzv2h_dphy_find_timings_val(hsfreq, THSZEROCTL,
					    ARRAY_SIZE(THSZEROCTL));
	dphy_timings.ths_prepare =
		rzv2h_dphy_find_timings_val(hsfreq, THSPRPRCTL,
					    ARRAY_SIZE(THSPRPRCTL));
	dphy_timings.tlpx =
		rzv2h_dphy_find_timings_val(hsfreq, TLPXCTL,
					    ARRAY_SIZE(TLPXCTL));
	ulpsexit =
		rzv2h_dphy_find_timings_val(lpclk_rate, ULPSEXIT,
					    ARRAY_SIZE(ULPSEXIT));

	phytclksetr = PHYTCLKSETR_TCLKTRAILCTL(dphy_timings.tclk_trail) |
		      PHYTCLKSETR_TCLKPOSTCTL(dphy_timings.tclk_post) |
		      PHYTCLKSETR_TCLKZEROCTL(dphy_timings.tclk_zero) |
		      PHYTCLKSETR_TCLKPRPRCTL(dphy_timings.tclk_prepare);
	phythssetr = PHYTHSSETR_THSEXITCTL(dphy_timings.ths_exit) |
		     PHYTHSSETR_THSTRAILCTL(dphy_timings.ths_trail) |
		     PHYTHSSETR_THSZEROCTL(dphy_timings.ths_zero) |
		     PHYTHSSETR_THSPRPRCTL(dphy_timings.ths_prepare);
	phytlpxsetr = rzg2l_mipi_dsi_phy_read(dsi, PHYTLPXSETR) & ~GENMASK(7, 0);
	phytlpxsetr |= PHYTLPXSETR_TLPXCTL(dphy_timings.tlpx);
	phycr = rzg2l_mipi_dsi_phy_read(dsi, PHYCR) & ~GENMASK(9, 0);
	phycr |= PHYCR_ULPSEXIT(ulpsexit);

	/* Setting all D-PHY Timings Registers */
	rzg2l_mipi_dsi_phy_write(dsi, PHYTCLKSETR, phytclksetr);
	rzg2l_mipi_dsi_phy_write(dsi, PHYTHSSETR, phythssetr);
	rzg2l_mipi_dsi_phy_write(dsi, PHYTLPXSETR, phytlpxsetr);
	rzg2l_mipi_dsi_phy_write(dsi, PHYCR, phycr);

	rzg2l_mipi_dsi_phy_write(dsi, PLLCLKSET0R,
				 PLLCLKSET0R_PLL_S(dsi_parameters->s) |
				 PLLCLKSET0R_PLL_P(dsi_parameters->p) |
				 PLLCLKSET0R_PLL_M(dsi_parameters->m));
	rzg2l_mipi_dsi_phy_write(dsi, PLLCLKSET1R, PLLCLKSET1R_PLL_K(dsi_parameters->k));
	udelay(20);

	rzg2l_mipi_dsi_phy_write(dsi, PLLENR, PLLENR_PLLEN);
	udelay(500);

	return 0;
}

static void rzv2h_mipi_dsi_dphy_late_init(struct rzg2l_mipi_dsi *dsi)
{
	udelay(220);
	rzg2l_mipi_dsi_phy_write(dsi, PHYRSTR, PHYRSTR_PHYMRSTN);
}

static void rzv2h_mipi_dsi_dphy_exit(struct rzg2l_mipi_dsi *dsi)
{
	rzg2l_mipi_dsi_phy_write(dsi, PLLENR, 0);
}

static int rzg2l_mipi_dsi_startup(struct rzg2l_mipi_dsi *dsi,
				  const struct drm_display_mode *mode)
{
	unsigned long long hsfreq_mhz;
	unsigned long hsfreq;
	u32 txsetr;
	u32 clstptsetr;
	u32 lptrnstsetr;
	u32 clkkpt;
	u32 clkbfht;
	u32 clkstpt;
	u32 golpbkt;
	int ret;

	ret = pm_runtime_resume_and_get(dsi->dev);
	if (ret < 0)
		return ret;

	ret = dsi->info->dphy_conf_clks(dsi, mode->clock, &hsfreq_mhz);
	if (ret < 0)
		goto err_phy;

	ret = dsi->info->dphy_init(dsi, hsfreq_mhz);
	if (ret < 0)
		goto err_phy;

	/* Enable Data lanes and Clock lanes */
	txsetr = TXSETR_DLEN | TXSETR_NUMLANEUSE(dsi->lanes - 1) | TXSETR_CLEN;
	rzg2l_mipi_dsi_link_write(dsi, TXSETR, txsetr);

	if (dsi->info->dphy_late_init)
		dsi->info->dphy_late_init(dsi);

	hsfreq = DIV_ROUND_CLOSEST_ULL(hsfreq_mhz, KILO);
	/*
	 * Global timings characteristic depends on high speed Clock Frequency
	 * Currently MIPI DSI-IF just supports maximum FHD@60 with:
	 * - videoclock = 148.5 (MHz)
	 * - bpp: maximum 24bpp
	 * - data lanes: maximum 4 lanes
	 * Therefore maximum hsclk will be 891 Mbps.
	 */
	if (hsfreq > 445500000) {
		clkkpt = 12;
		clkbfht = 15;
		clkstpt = 48;
		golpbkt = 75;
	} else if (hsfreq > 250000000) {
		clkkpt = 7;
		clkbfht = 8;
		clkstpt = 27;
		golpbkt = 40;
	} else {
		clkkpt = 8;
		clkbfht = 6;
		clkstpt = 24;
		golpbkt = 29;
	}

	clstptsetr = CLSTPTSETR_CLKKPT(clkkpt) | CLSTPTSETR_CLKBFHT(clkbfht) |
		     CLSTPTSETR_CLKSTPT(clkstpt);
	rzg2l_mipi_dsi_link_write(dsi, CLSTPTSETR, clstptsetr);

	lptrnstsetr = LPTRNSTSETR_GOLPBKT(golpbkt);
	rzg2l_mipi_dsi_link_write(dsi, LPTRNSTSETR, lptrnstsetr);

	return 0;

err_phy:
	dsi->info->dphy_exit(dsi);
	pm_runtime_put(dsi->dev);

	return ret;
}

static void rzg2l_mipi_dsi_stop(struct rzg2l_mipi_dsi *dsi)
{
	dsi->info->dphy_exit(dsi);
	pm_runtime_put(dsi->dev);
}

static void rzg2l_mipi_dsi_set_display_timing(struct rzg2l_mipi_dsi *dsi,
					      const struct drm_display_mode *mode)
{
	u32 vich1ppsetr;
	u32 vich1vssetr;
	u32 vich1vpsetr;
	u32 vich1hssetr;
	u32 vich1hpsetr;
	int dsi_format;
	u32 delay[2];
	u8 index;

	/* Configuration for Pixel Packet */
	dsi_format = mipi_dsi_pixel_format_to_bpp(dsi->format);
	switch (dsi_format) {
	case 24:
		vich1ppsetr = VICH1PPSETR_DT_RGB24;
		break;
	case 18:
		vich1ppsetr = VICH1PPSETR_DT_RGB18;
		break;
	case 16:
		vich1ppsetr = VICH1PPSETR_DT_RGB16;
		break;
	}

	if ((dsi->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE) &&
	    !(dsi->mode_flags & MIPI_DSI_MODE_VIDEO_BURST))
		vich1ppsetr |= VICH1PPSETR_TXESYNC_PULSE;

	rzg2l_mipi_dsi_link_write(dsi, VICH1PPSETR, vich1ppsetr);

	/* Configuration for Video Parameters */
	vich1vssetr = VICH1VSSETR_VACTIVE(mode->vdisplay) |
		      VICH1VSSETR_VSA(mode->vsync_end - mode->vsync_start);
	vich1vssetr |= (mode->flags & DRM_MODE_FLAG_PVSYNC) ?
			VICH1VSSETR_VSPOL_HIGH : VICH1VSSETR_VSPOL_LOW;

	vich1vpsetr = VICH1VPSETR_VFP(mode->vsync_start - mode->vdisplay) |
		      VICH1VPSETR_VBP(mode->vtotal - mode->vsync_end);

	vich1hssetr = VICH1HSSETR_HACTIVE(mode->hdisplay) |
		      VICH1HSSETR_HSA(mode->hsync_end - mode->hsync_start);
	vich1hssetr |= (mode->flags & DRM_MODE_FLAG_PHSYNC) ?
			VICH1HSSETR_HSPOL_HIGH : VICH1HSSETR_HSPOL_LOW;

	vich1hpsetr = VICH1HPSETR_HFP(mode->hsync_start - mode->hdisplay) |
		      VICH1HPSETR_HBP(mode->htotal - mode->hsync_end);

	rzg2l_mipi_dsi_link_write(dsi, VICH1VSSETR, vich1vssetr);
	rzg2l_mipi_dsi_link_write(dsi, VICH1VPSETR, vich1vpsetr);
	rzg2l_mipi_dsi_link_write(dsi, VICH1HSSETR, vich1hssetr);
	rzg2l_mipi_dsi_link_write(dsi, VICH1HPSETR, vich1hpsetr);

	if (dsi->info->dphy_late_init)
		dsi->info->dphy_late_init(dsi);

	/*
	 * Configuration for Delay Value
	 * Delay value based on 2 ranges of video clock.
	 * 74.25MHz is videoclock of HD@60p or FHD@30p
	 */
	if (mode->clock > 74250) {
		delay[0] = 231;
		delay[1] = 216;
	} else {
		delay[0] = 220;
		delay[1] = 212;
	}

	if (dsi->mode_flags & MIPI_DSI_CLOCK_NON_CONTINUOUS)
		index = 0;
	else
		index = 1;

	rzg2l_mipi_dsi_link_write(dsi, VICH1SET1R,
				  VICH1SET1R_DLY(delay[index]));
}

static int rzg2l_mipi_dsi_start_hs_clock(struct rzg2l_mipi_dsi *dsi)
{
	bool is_clk_cont;
	u32 hsclksetr;
	u32 status;
	int ret;

	is_clk_cont = !(dsi->mode_flags & MIPI_DSI_CLOCK_NON_CONTINUOUS);

	/* Start HS clock */
	hsclksetr = HSCLKSETR_HSCLKRUN_HS | (is_clk_cont ?
					     HSCLKSETR_HSCLKMODE_CONT :
					     HSCLKSETR_HSCLKMODE_NON_CONT);
	rzg2l_mipi_dsi_link_write(dsi, HSCLKSETR, hsclksetr);

	if (is_clk_cont) {
		ret = read_poll_timeout(rzg2l_mipi_dsi_link_read, status,
					status & PLSR_CLLP2HS,
					2000, 20000, false, dsi, PLSR);
		if (ret < 0) {
			dev_err(dsi->dev, "failed to start HS clock\n");
			return ret;
		}
	}

	dev_dbg(dsi->dev, "Start High Speed Clock with %s clock mode",
		is_clk_cont ? "continuous" : "non-continuous");

	return 0;
}

static int rzg2l_mipi_dsi_stop_hs_clock(struct rzg2l_mipi_dsi *dsi)
{
	bool is_clk_cont;
	u32 status;
	int ret;

	is_clk_cont = !(dsi->mode_flags & MIPI_DSI_CLOCK_NON_CONTINUOUS);

	/* Stop HS clock */
	rzg2l_mipi_dsi_link_write(dsi, HSCLKSETR,
				  is_clk_cont ? HSCLKSETR_HSCLKMODE_CONT :
				  HSCLKSETR_HSCLKMODE_NON_CONT);

	if (is_clk_cont) {
		ret = read_poll_timeout(rzg2l_mipi_dsi_link_read, status,
					status & PLSR_CLHS2LP,
					2000, 20000, false, dsi, PLSR);
		if (ret < 0) {
			dev_err(dsi->dev, "failed to stop HS clock\n");
			return ret;
		}
	}

	return 0;
}

static int rzg2l_mipi_dsi_start_video(struct rzg2l_mipi_dsi *dsi)
{
	u32 vich1set0r;
	u32 status;
	int ret;

	/* Configuration for Blanking sequence and start video input*/
	vich1set0r = VICH1SET0R_HFPNOLP | VICH1SET0R_HBPNOLP |
		     VICH1SET0R_HSANOLP | VICH1SET0R_VSTART;
	rzg2l_mipi_dsi_link_write(dsi, VICH1SET0R, vich1set0r);

	ret = read_poll_timeout(rzg2l_mipi_dsi_link_read, status,
				status & VICH1SR_VIRDY,
				2000, 20000, false, dsi, VICH1SR);
	if (ret < 0)
		dev_err(dsi->dev, "Failed to start video signal input\n");

	return ret;
}

static int rzg2l_mipi_dsi_stop_video(struct rzg2l_mipi_dsi *dsi)
{
	u32 status;
	int ret;

	rzg2l_mipi_dsi_link_write(dsi, VICH1SET0R, VICH1SET0R_VSTPAFT);
	ret = read_poll_timeout(rzg2l_mipi_dsi_link_read, status,
				(status & VICH1SR_STOP) && (!(status & VICH1SR_RUNNING)),
				2000, 20000, false, dsi, VICH1SR);
	if (ret < 0)
		goto err;

	ret = read_poll_timeout(rzg2l_mipi_dsi_link_read, status,
				!(status & LINKSR_HSBUSY),
				2000, 20000, false, dsi, LINKSR);
	if (ret < 0)
		goto err;

	return 0;

err:
	dev_err(dsi->dev, "Failed to stop video signal input\n");
	return ret;
}

/* -----------------------------------------------------------------------------
 * Bridge
 */

static int rzg2l_mipi_dsi_attach(struct drm_bridge *bridge,
				 enum drm_bridge_attach_flags flags)
{
	struct rzg2l_mipi_dsi *dsi = bridge_to_rzg2l_mipi_dsi(bridge);

	return drm_bridge_attach(bridge->encoder, dsi->next_bridge, bridge,
				 flags);
}

static void rzg2l_mipi_dsi_atomic_enable(struct drm_bridge *bridge,
					 struct drm_bridge_state *old_bridge_state)
{
	struct drm_atomic_state *state = old_bridge_state->base.state;
	struct rzg2l_mipi_dsi *dsi = bridge_to_rzg2l_mipi_dsi(bridge);
	const struct drm_display_mode *mode;
	struct drm_connector *connector;
	struct drm_crtc *crtc;
	int ret;

	connector = drm_atomic_get_new_connector_for_encoder(state, bridge->encoder);
	crtc = drm_atomic_get_new_connector_state(state, connector)->crtc;
	mode = &drm_atomic_get_new_crtc_state(state, crtc)->adjusted_mode;

	ret = rzg2l_mipi_dsi_startup(dsi, mode);
	if (ret < 0)
		return;

	rzg2l_mipi_dsi_set_display_timing(dsi, mode);

	ret = rzg2l_mipi_dsi_start_hs_clock(dsi);
	if (ret < 0)
		goto err_stop;

	ret = rzg2l_mipi_dsi_start_video(dsi);
	if (ret < 0)
		goto err_stop_clock;

	return;

err_stop_clock:
	rzg2l_mipi_dsi_stop_hs_clock(dsi);
err_stop:
	rzg2l_mipi_dsi_stop(dsi);
}

static void rzg2l_mipi_dsi_atomic_disable(struct drm_bridge *bridge,
					  struct drm_bridge_state *old_bridge_state)
{
	struct rzg2l_mipi_dsi *dsi = bridge_to_rzg2l_mipi_dsi(bridge);

	rzg2l_mipi_dsi_stop_video(dsi);
	rzg2l_mipi_dsi_stop_hs_clock(dsi);
	rzg2l_mipi_dsi_stop(dsi);
}

static enum drm_mode_status
rzg2l_mipi_dsi_bridge_mode_valid(struct drm_bridge *bridge,
				 const struct drm_display_info *info,
				 const struct drm_display_mode *mode)
{
	struct rzg2l_mipi_dsi *dsi = bridge_to_rzg2l_mipi_dsi(bridge);

	if (mode->clock > dsi->info->max_dclk)
		return MODE_CLOCK_HIGH;

	if (mode->clock < dsi->info->min_dclk)
		return MODE_CLOCK_LOW;

	if (dsi->info->dphy_mode_clk_check) {
		enum drm_mode_status status;

		status = dsi->info->dphy_mode_clk_check(dsi, mode->clock);
		if (status != MODE_OK)
			return status;
	}

	return MODE_OK;
}

static const struct drm_bridge_funcs rzg2l_mipi_dsi_bridge_ops = {
	.attach = rzg2l_mipi_dsi_attach,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.atomic_enable = rzg2l_mipi_dsi_atomic_enable,
	.atomic_disable = rzg2l_mipi_dsi_atomic_disable,
	.mode_valid = rzg2l_mipi_dsi_bridge_mode_valid,
};

/* -----------------------------------------------------------------------------
 * Host setting
 */

static int rzg2l_mipi_dsi_host_attach(struct mipi_dsi_host *host,
				      struct mipi_dsi_device *device)
{
	struct rzg2l_mipi_dsi *dsi = host_to_rzg2l_mipi_dsi(host);
	int ret;

	if (device->lanes > dsi->num_data_lanes) {
		dev_err(dsi->dev,
			"Number of lines of device (%u) exceeds host (%u)\n",
			device->lanes, dsi->num_data_lanes);
		return -EINVAL;
	}

	switch (mipi_dsi_pixel_format_to_bpp(device->format)) {
	case 24:
		break;
	case 18:
		break;
	case 16:
		if (!(dsi->info->features & RZ_MIPI_DSI_16BPP)) {
			dev_err(dsi->dev, "Unsupported format 0x%04x\n",
				device->format);
			return -EINVAL;
		}
		break;
	default:
		dev_err(dsi->dev, "Unsupported format 0x%04x\n", device->format);
		return -EINVAL;
	}

	dsi->lanes = device->lanes;
	dsi->format = device->format;
	dsi->mode_flags = device->mode_flags;

	dsi->next_bridge = devm_drm_of_get_bridge(dsi->dev, dsi->dev->of_node,
						  1, 0);
	if (IS_ERR(dsi->next_bridge)) {
		ret = PTR_ERR(dsi->next_bridge);
		dev_err(dsi->dev, "failed to get next bridge: %d\n", ret);
		return ret;
	}

	drm_bridge_add(&dsi->bridge);

	return 0;
}

static int rzg2l_mipi_dsi_host_detach(struct mipi_dsi_host *host,
				      struct mipi_dsi_device *device)
{
	struct rzg2l_mipi_dsi *dsi = host_to_rzg2l_mipi_dsi(host);

	drm_bridge_remove(&dsi->bridge);

	return 0;
}

static const struct mipi_dsi_host_ops rzg2l_mipi_dsi_host_ops = {
	.attach = rzg2l_mipi_dsi_host_attach,
	.detach = rzg2l_mipi_dsi_host_detach,
};

/* -----------------------------------------------------------------------------
 * Power Management
 */

static int __maybe_unused rzg2l_mipi_pm_runtime_suspend(struct device *dev)
{
	struct rzg2l_mipi_dsi *dsi = dev_get_drvdata(dev);

	reset_control_assert(dsi->prstc);
	reset_control_assert(dsi->arstc);

	return 0;
}

static int __maybe_unused rzg2l_mipi_pm_runtime_resume(struct device *dev)
{
	struct rzg2l_mipi_dsi *dsi = dev_get_drvdata(dev);
	int ret;

	ret = reset_control_deassert(dsi->arstc);
	if (ret < 0)
		return ret;

	ret = reset_control_deassert(dsi->prstc);
	if (ret < 0)
		reset_control_assert(dsi->arstc);

	return ret;
}

static const struct dev_pm_ops rzg2l_mipi_pm_ops = {
	SET_RUNTIME_PM_OPS(rzg2l_mipi_pm_runtime_suspend, rzg2l_mipi_pm_runtime_resume, NULL)
};

/* -----------------------------------------------------------------------------
 * Probe & Remove
 */

static int rzg2l_mipi_dsi_probe(struct platform_device *pdev)
{
	unsigned int num_data_lanes;
	struct rzg2l_mipi_dsi *dsi;
	u32 txsetr;
	int ret;

	dsi = devm_kzalloc(&pdev->dev, sizeof(*dsi), GFP_KERNEL);
	if (!dsi)
		return -ENOMEM;

	platform_set_drvdata(pdev, dsi);
	dsi->dev = &pdev->dev;

	dsi->info = of_device_get_match_data(&pdev->dev);
	if (!dsi->info)
		return dev_err_probe(dsi->dev, -ENODEV,
				     "missing data info\n");

	ret = drm_of_get_data_lanes_count_ep(dsi->dev->of_node, 1, 0, 1, 4);
	if (ret < 0)
		return dev_err_probe(dsi->dev, ret,
				     "missing or invalid data-lanes property\n");

	num_data_lanes = ret;

	dsi->mmio = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dsi->mmio))
		return PTR_ERR(dsi->mmio);

	dsi->vclk = devm_clk_get(dsi->dev, "vclk");
	if (IS_ERR(dsi->vclk))
		return PTR_ERR(dsi->vclk);

	if (dsi->info->features & RZ_MIPI_DSI_HASLPCLK) {
		dsi->lpclk = devm_clk_get(dsi->dev, "lpclk");
		if (IS_ERR(dsi->lpclk))
			return PTR_ERR(dsi->lpclk);
	}

	if (dsi->info->has_dphy_rstc) {
		dsi->rstc = devm_reset_control_get_exclusive(dsi->dev, "rst");
		if (IS_ERR(dsi->rstc))
			return dev_err_probe(dsi->dev, PTR_ERR(dsi->rstc),
					     "failed to get rst\n");
	}

	dsi->arstc = devm_reset_control_get_exclusive(dsi->dev, "arst");
	if (IS_ERR(dsi->arstc))
		return dev_err_probe(&pdev->dev, PTR_ERR(dsi->arstc),
				     "failed to get arst\n");

	dsi->prstc = devm_reset_control_get_exclusive(dsi->dev, "prst");
	if (IS_ERR(dsi->prstc))
		return dev_err_probe(dsi->dev, PTR_ERR(dsi->prstc),
				     "failed to get prst\n");

	platform_set_drvdata(pdev, dsi);

	pm_runtime_enable(dsi->dev);

	ret = pm_runtime_resume_and_get(dsi->dev);
	if (ret < 0)
		goto err_pm_disable;

	/*
	 * TXSETR register can be read only after DPHY init. But during probe
	 * mode->clock and format are not available. So initialize DPHY with
	 * timing parameters for 80Mbps.
	 */
	ret = dsi->info->dphy_init(dsi, 80000000ULL * KILO);
	if (ret < 0)
		goto err_phy;

	txsetr = rzg2l_mipi_dsi_link_read(dsi, TXSETR);
	dsi->num_data_lanes = min(((txsetr >> 16) & 3) + 1, num_data_lanes);
	dsi->info->dphy_exit(dsi);
	pm_runtime_put(dsi->dev);

	/* Initialize the DRM bridge. */
	dsi->bridge.funcs = &rzg2l_mipi_dsi_bridge_ops;
	dsi->bridge.of_node = dsi->dev->of_node;

	/* Init host device */
	dsi->host.dev = dsi->dev;
	dsi->host.ops = &rzg2l_mipi_dsi_host_ops;
	ret = mipi_dsi_host_register(&dsi->host);
	if (ret < 0)
		goto err_pm_disable;

	return 0;

err_phy:
	dsi->info->dphy_exit(dsi);
	pm_runtime_put(dsi->dev);
err_pm_disable:
	pm_runtime_disable(dsi->dev);
	return ret;
}

static void rzg2l_mipi_dsi_remove(struct platform_device *pdev)
{
	struct rzg2l_mipi_dsi *dsi = platform_get_drvdata(pdev);

	mipi_dsi_host_unregister(&dsi->host);
	pm_runtime_disable(&pdev->dev);
}

RZV2H_CPG_PLL_DSI_LIMITS(rzv2h_cpg_pll_dsi_limits);

static const struct rzg2l_mipi_dsi_hw_info rzv2h_mipi_dsi_info = {
	.dphy_init = rzv2h_mipi_dsi_dphy_init,
	.dphy_late_init = rzv2h_mipi_dsi_dphy_late_init,
	.dphy_exit = rzv2h_mipi_dsi_dphy_exit,
	.dphy_mode_clk_check = rzv2h_dphy_mode_clk_check,
	.dphy_conf_clks = rzv2h_dphy_conf_clks,
	.cpg_dsi_limits = &rzv2h_cpg_pll_dsi_limits,
	.phy_reg_offset = 0x10000,
	.link_reg_offset = 0,
	.max_dclk = 187500,
	.min_dclk = 5440,
	.features = RZ_MIPI_DSI_16BPP,
};

static const struct rzg2l_mipi_dsi_hw_info rzg2l_mipi_dsi_info = {
	.dphy_init = rzg2l_mipi_dsi_dphy_init,
	.dphy_exit = rzg2l_mipi_dsi_dphy_exit,
	.dphy_conf_clks = rzg2l_dphy_conf_clks,
	.has_dphy_rstc = true,
	.link_reg_offset = 0x10000,
	.max_dclk = 148500,
	.min_dclk = 5803,
};

static const struct of_device_id rzg2l_mipi_dsi_of_table[] = {
	{ .compatible = "renesas,r9a09g057-mipi-dsi", .data = &rzv2h_mipi_dsi_info, },
	{ .compatible = "renesas,rzg2l-mipi-dsi", .data = &rzg2l_mipi_dsi_info, },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, rzg2l_mipi_dsi_of_table);

static struct platform_driver rzg2l_mipi_dsi_platform_driver = {
	.probe	= rzg2l_mipi_dsi_probe,
	.remove = rzg2l_mipi_dsi_remove,
	.driver	= {
		.name = "rzg2l-mipi-dsi",
		.pm = &rzg2l_mipi_pm_ops,
		.of_match_table = rzg2l_mipi_dsi_of_table,
	},
};

module_platform_driver(rzg2l_mipi_dsi_platform_driver);

MODULE_AUTHOR("Biju Das <biju.das.jz@bp.renesas.com>");
MODULE_DESCRIPTION("Renesas RZ/G2L MIPI DSI Encoder Driver");
MODULE_LICENSE("GPL");
