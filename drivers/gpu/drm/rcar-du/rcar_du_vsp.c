// SPDX-License-Identifier: GPL-2.0+
/*
 * R-Car Display Unit VSP-Based Compositor
 *
 * Copyright (C) 2015 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_crtc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_vblank.h>

#include <linux/bitops.h>
#include <linux/dma-mapping.h>
#include <linux/of_platform.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/videodev2.h>

#include <media/vsp1.h>

#include "rcar_du_drv.h"
#include "rcar_du_kms.h"
#include "rcar_du_vsp.h"
#include "rcar_du_writeback.h"

static void rcar_du_vsp_complete(void *private, unsigned int status, u32 crc)
{
	struct rcar_du_crtc *crtc = private;

	if (crtc->vblank_enable)
		drm_crtc_handle_vblank(&crtc->crtc);

	if (status & VSP1_DU_STATUS_COMPLETE)
		rcar_du_crtc_finish_page_flip(crtc);
	if (status & VSP1_DU_STATUS_WRITEBACK)
		rcar_du_writeback_complete(crtc);

	drm_crtc_add_crc_entry(&crtc->crtc, false, 0, &crc);
}

void rcar_du_vsp_enable(struct rcar_du_crtc *crtc)
{
	const struct drm_display_mode *mode = &crtc->crtc.state->adjusted_mode;
	struct rcar_du_device *rcdu = crtc->dev;
	struct vsp1_du_lif_config cfg = {
		.width = mode->hdisplay,
		.height = mode->vdisplay,
		.interlaced = mode->flags & DRM_MODE_FLAG_INTERLACE,
		.callback = rcar_du_vsp_complete,
		.callback_data = crtc,
	};
	struct rcar_du_plane_state state = {
		.state = {
			.alpha = DRM_BLEND_ALPHA_OPAQUE,
			.crtc = &crtc->crtc,
			.dst.x1 = 0,
			.dst.y1 = 0,
			.dst.x2 = mode->hdisplay,
			.dst.y2 = mode->vdisplay,
			.src.x1 = 0,
			.src.y1 = 0,
			.src.x2 = mode->hdisplay << 16,
			.src.y2 = mode->vdisplay << 16,
			.zpos = 0,
		},
		.format = rcar_du_format_info(DRM_FORMAT_ARGB8888),
		.source = RCAR_DU_PLANE_VSPD1,
		.colorkey = 0,
	};

	if (rcdu->info->gen >= 3)
		state.hwindex = (crtc->index % 2) ? 2 : 0;
	else
		state.hwindex = crtc->index % 2;

	__rcar_du_plane_setup(crtc->group, &state);

	vsp1_du_setup_lif(crtc->vsp->vsp, crtc->vsp_pipe, &cfg);
}

static int rcar_du_vsp_plane_atomic_check(struct drm_plane *plane,
					  struct drm_atomic_state *state)
{
	struct drm_plane_state *new_plane_state = drm_atomic_get_new_plane_state(state,
										 plane);
	struct rcar_du_vsp_plane_state *rstate = to_rcar_vsp_plane_state(new_plane_state);

	return __rcar_du_plane_atomic_check(plane, new_plane_state,
					    &rstate->format);
}

static const struct drm_plane_helper_funcs rcar_du_vsp_plane_helper_funcs = {
	.prepare_fb = rcar_du_vsp_plane_prepare_fb,
	.cleanup_fb = rcar_du_vsp_plane_cleanup_fb,
	.atomic_check = rcar_du_vsp_plane_atomic_check,
	.atomic_update = rcar_du_vsp_plane_atomic_update,
};

int rcar_du_vsp_init(struct rcar_du_vsp *vsp, struct device_node *np,
		     unsigned int crtcs)
{
	return rcar_du_lib_vsp_init(vsp, np, crtcs,
				    &rcar_du_vsp_plane_helper_funcs);
}
