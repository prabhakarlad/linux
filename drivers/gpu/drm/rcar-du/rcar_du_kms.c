// SPDX-License-Identifier: GPL-2.0+
/*
 * R-Car Display Unit Mode Setting
 *
 * Copyright (C) 2013-2015 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_device.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/wait.h>

#include "rcar_du_crtc.h"
#include "rcar_du_drv.h"
#include "rcar_du_encoder.h"
#include "rcar_du_kms.h"
#include "rcar_du_regs.h"
#include "rcar_du_vsp.h"
#include "rcar_du_writeback.h"

/* -----------------------------------------------------------------------------
 * Frame buffer
 */

static struct drm_framebuffer *
rcar_du_fb_create(struct drm_device *dev, struct drm_file *file_priv,
		  const struct drm_mode_fb_cmd2 *mode_cmd)
{
	return rcar_du_lib_fb_create(dev, file_priv, mode_cmd);
}

/* -----------------------------------------------------------------------------
 * Atomic Check and Update
 */

static int rcar_du_atomic_check(struct drm_device *dev,
				struct drm_atomic_state *state)
{
	struct rcar_du_device *rcdu = to_rcar_du_device(dev);
	int ret;

	ret = drm_atomic_helper_check(dev, state);
	if (ret)
		return ret;

	if (rcar_du_has(rcdu, RCAR_DU_FEATURE_VSP1_SOURCE))
		return 0;

	return rcar_du_atomic_check_planes(dev, state);
}

/* -----------------------------------------------------------------------------
 * Initialization
 */

static const struct drm_mode_config_funcs rcar_du_mode_config_funcs = {
	.fb_create = rcar_du_fb_create,
	.atomic_check = rcar_du_atomic_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int rcar_du_cmm_init(struct rcar_du_device *rcdu)
{
	const struct device_node *np = rcdu->dev->of_node;
	unsigned int i;
	int cells;

	cells = of_property_count_u32_elems(np, "renesas,cmms");
	if (cells == -EINVAL)
		return 0;

	if (cells > rcdu->num_crtcs) {
		dev_err(rcdu->dev,
			"Invalid number of entries in 'renesas,cmms'\n");
		return -EINVAL;
	}

	for (i = 0; i < cells; ++i) {
		struct platform_device *pdev;
		struct device_link *link;
		struct device_node *cmm;
		int ret;

		cmm = of_parse_phandle(np, "renesas,cmms", i);
		if (!cmm) {
			dev_err(rcdu->dev,
				"Failed to parse 'renesas,cmms' property\n");
			return -EINVAL;
		}

		if (!of_device_is_available(cmm)) {
			/* It's fine to have a phandle to a non-enabled CMM. */
			of_node_put(cmm);
			continue;
		}

		pdev = of_find_device_by_node(cmm);
		if (!pdev) {
			dev_err(rcdu->dev, "No device found for CMM%u\n", i);
			of_node_put(cmm);
			return -EINVAL;
		}

		of_node_put(cmm);

		/*
		 * -ENODEV is used to report that the CMM config option is
		 * disabled: return 0 and let the DU continue probing.
		 */
		ret = rcar_cmm_init(pdev);
		if (ret) {
			platform_device_put(pdev);
			return ret == -ENODEV ? 0 : ret;
		}

		rcdu->cmms[i] = pdev;

		/*
		 * Enforce suspend/resume ordering by making the CMM a provider
		 * of the DU: CMM is suspended after and resumed before the DU.
		 */
		link = device_link_add(rcdu->dev, &pdev->dev, DL_FLAG_STATELESS);
		if (!link) {
			dev_err(rcdu->dev,
				"Failed to create device link to CMM%u\n", i);
			return -EINVAL;
		}
	}

	return 0;
}

static void rcar_du_modeset_cleanup(struct drm_device *dev, void *res)
{
	struct rcar_du_device *rcdu = to_rcar_du_device(dev);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rcdu->cmms); ++i)
		platform_device_put(rcdu->cmms[i]);
}

int rcar_du_modeset_init(struct rcar_du_device *rcdu)
{
	static const unsigned int mmio_offsets[] = {
		DU0_REG_OFFSET, DU2_REG_OFFSET
	};

	struct drm_device *dev = &rcdu->ddev;
	struct drm_encoder *encoder;
	unsigned int dpad0_sources;
	unsigned int num_encoders;
	unsigned int num_groups;
	unsigned int swindex;
	unsigned int hwindex;
	unsigned int i;
	int ret;

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;

	ret = drmm_add_action(&rcdu->ddev, rcar_du_modeset_cleanup, NULL);
	if (ret)
		return ret;

	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;
	dev->mode_config.normalize_zpos = true;
	dev->mode_config.funcs = &rcar_du_mode_config_funcs;
	dev->mode_config.helper_private = rcar_du_lib_mode_cfg_helper_fns();

	if (rcdu->info->gen < 3) {
		dev->mode_config.max_width = 4095;
		dev->mode_config.max_height = 2047;
	} else {
		/*
		 * The Gen3 DU uses the VSP1 for memory access, and is limited
		 * to frame sizes of 8190x8190.
		 */
		dev->mode_config.max_width = 8190;
		dev->mode_config.max_height = 8190;
	}

	rcdu->num_crtcs = hweight8(rcdu->info->channels_mask);

	ret = rcar_du_properties_init(rcdu);
	if (ret < 0)
		return ret;

	/*
	 * Initialize vertical blanking interrupts handling. Start with vblank
	 * disabled for all CRTCs.
	 */
	ret = drm_vblank_init(dev, rcdu->num_crtcs);
	if (ret < 0)
		return ret;

	/* Initialize the groups. */
	num_groups = DIV_ROUND_UP(rcdu->num_crtcs, 2);

	for (i = 0; i < num_groups; ++i) {
		struct rcar_du_group *rgrp = &rcdu->groups[i];

		mutex_init(&rgrp->lock);

		rgrp->dev = rcdu;
		rgrp->mmio_offset = mmio_offsets[i];
		rgrp->index = i;
		/* Extract the channel mask for this group only. */
		rgrp->channels_mask = (rcdu->info->channels_mask >> (2 * i))
				   & GENMASK(1, 0);
		rgrp->num_crtcs = hweight8(rgrp->channels_mask);

		/*
		 * If we have more than one CRTCs in this group pre-associate
		 * the low-order planes with CRTC 0 and the high-order planes
		 * with CRTC 1 to minimize flicker occurring when the
		 * association is changed.
		 */
		rgrp->dptsr_planes = rgrp->num_crtcs > 1
				   ? (rcdu->info->gen >= 3 ? 0x04 : 0xf0)
				   : 0;

		if (!rcar_du_has(rcdu, RCAR_DU_FEATURE_VSP1_SOURCE)) {
			ret = rcar_du_planes_init(rgrp);
			if (ret < 0)
				return ret;
		}
	}

	/* Initialize the compositors. */
	if (rcar_du_has(rcdu, RCAR_DU_FEATURE_VSP1_SOURCE)) {
		ret = rcar_du_lib_vsps_init(rcdu, rcar_du_vsp_init);
		if (ret < 0)
			return ret;
	}

	/* Initialize the Color Management Modules. */
	ret = rcar_du_cmm_init(rcdu);
	if (ret)
		return ret;

	/* Create the CRTCs. */
	for (swindex = 0, hwindex = 0; swindex < rcdu->num_crtcs; ++hwindex) {
		struct rcar_du_group *rgrp;

		/* Skip unpopulated DU channels. */
		if (!(rcdu->info->channels_mask & BIT(hwindex)))
			continue;

		rgrp = &rcdu->groups[hwindex / 2];

		ret = rcar_du_crtc_create(rgrp, swindex++, hwindex);
		if (ret < 0)
			return ret;
	}

	/* Initialize the encoders. */
	ret = rcar_du_encoders_init(rcdu, rcar_du_output_name,
				    rcar_du_encoder_init);
	if (ret < 0)
		return ret;

	if (ret == 0) {
		dev_err(rcdu->dev, "error: no encoder could be initialized\n");
		return -EINVAL;
	}

	num_encoders = ret;

	/*
	 * Set the possible CRTCs and possible clones. There's always at least
	 * one way for all encoders to clone each other, set all bits in the
	 * possible clones field.
	 */
	list_for_each_entry(encoder, &dev->mode_config.encoder_list, head) {
		struct rcar_du_encoder *renc = to_rcar_encoder(encoder);
		const struct rcar_du_output_routing *route =
			&rcdu->info->routes[renc->output];

		encoder->possible_crtcs = route->possible_crtcs;
		encoder->possible_clones = (1 << num_encoders) - 1;
	}

	/* Create the writeback connectors. */
	if (rcdu->info->gen >= 3) {
		for (i = 0; i < rcdu->num_crtcs; ++i) {
			struct rcar_du_crtc *rcrtc = &rcdu->crtcs[i];

			ret = rcar_du_writeback_init(rcdu, rcrtc);
			if (ret < 0)
				return ret;
		}
	}

	/*
	 * Initialize the default DPAD0 source to the index of the first DU
	 * channel that can be connected to DPAD0. The exact value doesn't
	 * matter as it should be overwritten by mode setting for the RGB
	 * output, but it is nonetheless required to ensure a valid initial
	 * hardware configuration on Gen3 where DU0 can't always be connected to
	 * DPAD0.
	 */
	dpad0_sources = rcdu->info->routes[RCAR_DU_OUTPUT_DPAD0].possible_crtcs;
	rcdu->dpad0_source = ffs(dpad0_sources) - 1;

	drm_mode_config_reset(dev);

	drm_kms_helper_poll_init(dev);

	return 0;
}
