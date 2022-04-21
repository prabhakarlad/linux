// SPDX-License-Identifier: GPL-2.0+
/*
 * RZ/G2L Display Unit Mode Setting
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 *
 * Based on rcar_du_kms.c
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/wait.h>

#include "rzg2l_du_crtc.h"
#include "rzg2l_du_drv.h"
#include "rzg2l_du_encoder.h"
#include "rzg2l_du_kms.h"
#include "rzg2l_du_vsp.h"
#include "rcar_du_writeback.h"

/* -----------------------------------------------------------------------------
 * Frame buffer
 */

static struct drm_framebuffer *
rzg2l_du_fb_create(struct drm_device *dev, struct drm_file *file_priv,
		   const struct drm_mode_fb_cmd2 *mode_cmd)
{
	return rcar_du_lib_fb_create(dev, file_priv, mode_cmd);
}

/* -----------------------------------------------------------------------------
 * Initialization
 */

static const struct drm_mode_config_funcs rzg2l_du_mode_config_funcs = {
	.fb_create = rzg2l_du_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

int rzg2l_du_modeset_init(struct rcar_du_device *rcdu)
{
	struct drm_device *dev = &rcdu->ddev;
	struct drm_encoder *encoder;
	unsigned int dpad0_sources;
	unsigned int num_encoders;
	unsigned int i;
	int ret;

	ret = drmm_mode_config_init(dev);
	if (ret)
		return ret;

	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;
	dev->mode_config.normalize_zpos = true;
	dev->mode_config.funcs = &rzg2l_du_mode_config_funcs;
	dev->mode_config.helper_private = rcar_du_lib_mode_cfg_helper_fns();

	/*
	 * The RZ/G2L DU uses the VSP1 for memory access, and is limited
	 * to frame sizes of 1920x1080.
	 */
	dev->mode_config.max_width = 1920;
	dev->mode_config.max_height = 1080;

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

	/* Initialize the compositors. */
	ret = rcar_du_lib_vsps_init(rcdu, rzg2l_du_vsp_init);
	if (ret < 0)
		return ret;

	/* Create the CRTCs. */
	ret = rzg2l_du_crtc_create(rcdu);
	if (ret < 0)
		return ret;

	/* Initialize the encoders. */
	ret = rcar_du_encoders_init(rcdu, rzg2l_du_output_name,
				    rzg2l_du_encoder_init);
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
	for (i = 0; i < rcdu->num_crtcs; ++i) {
		struct rcar_du_crtc *rcrtc = &rcdu->crtcs[i];

		ret = rcar_du_writeback_init(rcdu, rcrtc);
		if (ret < 0)
			return ret;
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
