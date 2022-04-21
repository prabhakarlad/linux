// SPDX-License-Identifier: GPL-2.0+
/*
 * RZ/G2L Display Unit VSP-Based Compositor
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 *
 * Based on rcar_du_vsp.c
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_crtc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_vblank.h>

#include <linux/bitops.h>
#include <linux/of_platform.h>
#include <linux/scatterlist.h>

#include <media/vsp1.h>

#include "rzg2l_du_drv.h"
#include "rzg2l_du_kms.h"
#include "rzg2l_du_vsp.h"
#include "rcar_du_writeback.h"

static void rzg2l_du_vsp_complete(void *private, unsigned int status, u32 crc)
{
	struct rcar_du_crtc *crtc = private;

	if (crtc->vblank_enable)
		drm_crtc_handle_vblank(&crtc->crtc);

	if (status & VSP1_DU_STATUS_COMPLETE)
		rzg2l_du_crtc_finish_page_flip(crtc);
	if (status & VSP1_DU_STATUS_WRITEBACK)
		rcar_du_writeback_complete(crtc);

	drm_crtc_add_crc_entry(&crtc->crtc, false, 0, &crc);
}

void rzg2l_du_vsp_enable(struct rcar_du_crtc *crtc)
{
	const struct drm_display_mode *mode = &crtc->crtc.state->adjusted_mode;
	struct vsp1_du_lif_config cfg = {
		.width = mode->hdisplay,
		.height = mode->vdisplay,
		.interlaced = mode->flags & DRM_MODE_FLAG_INTERLACE,
		.callback = rzg2l_du_vsp_complete,
		.callback_data = crtc,
	};

	vsp1_du_setup_lif(crtc->vsp->vsp, crtc->vsp_pipe, &cfg);
}

static int rzg2l_du_vsp_plane_atomic_check(struct drm_plane *plane,
					   struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state,
										 plane);
	struct rcar_du_vsp_plane_state *rstate = to_rcar_vsp_plane_state(new_plane_state);

	return __rzg2l_du_crtc_plane_atomic_check(plane, new_plane_state, &rstate->format);
}

static const struct drm_plane_helper_funcs rzg2l_du_vsp_plane_helper_funcs = {
	.prepare_fb = rcar_du_vsp_plane_prepare_fb,
	.cleanup_fb = rcar_du_vsp_plane_cleanup_fb,
	.atomic_check = rzg2l_du_vsp_plane_atomic_check,
	.atomic_update = rcar_du_vsp_plane_atomic_update,
};

int rzg2l_du_vsp_init(struct rcar_du_vsp *vsp, struct device_node *np,
		      unsigned int crtcs)
{
	return rcar_du_lib_vsp_init(vsp, np, crtcs,
				    &rzg2l_du_vsp_plane_helper_funcs);
}
