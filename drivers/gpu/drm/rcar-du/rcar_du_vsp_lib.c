// SPDX-License-Identifier: GPL-2.0+
/*
 * R-Car Display Unit VSP-Based Compositor Lib
 *
 * Copyright (C) 2015-2022 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>

#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>

#include <media/vsp1.h>

#include "rcar_du_drv.h"
#include "rcar_du_writeback.h"

void rcar_du_vsp_disable(struct rcar_du_crtc *crtc)
{
	vsp1_du_setup_lif(crtc->vsp->vsp, crtc->vsp_pipe, NULL);
}

void rcar_du_vsp_atomic_begin(struct rcar_du_crtc *crtc)
{
	vsp1_du_atomic_begin(crtc->vsp->vsp, crtc->vsp_pipe);
}

void rcar_du_vsp_atomic_flush(struct rcar_du_crtc *crtc)
{
	struct vsp1_du_atomic_pipe_config cfg = { { 0, } };
	struct rcar_du_crtc_state *state;

	state = to_rcar_crtc_state(crtc->crtc.state);
	cfg.crc = state->crc;

	rcar_du_writeback_setup(crtc, &cfg.writeback);

	vsp1_du_atomic_flush(crtc->vsp->vsp, crtc->vsp_pipe, &cfg);
}

int rcar_du_vsp_map_fb(struct rcar_du_vsp *vsp, struct drm_framebuffer *fb,
		       struct sg_table sg_tables[3])
{
	struct rcar_du_device *rcdu = vsp->dev;
	unsigned int i, j;
	int ret;

	for (i = 0; i < fb->format->num_planes; ++i) {
		struct drm_gem_dma_object *gem = drm_fb_dma_get_gem_obj(fb, i);
		struct sg_table *sgt = &sg_tables[i];

		if (gem->sgt) {
			struct scatterlist *src;
			struct scatterlist *dst;

			/*
			 * If the GEM buffer has a scatter gather table, it has
			 * been imported from a dma-buf and has no physical
			 * address as it might not be physically contiguous.
			 * Copy the original scatter gather table to map it to
			 * the VSP.
			 */
			ret = sg_alloc_table(sgt, gem->sgt->orig_nents,
					     GFP_KERNEL);
			if (ret)
				goto fail;

			src = gem->sgt->sgl;
			dst = sgt->sgl;
			for (j = 0; j < gem->sgt->orig_nents; ++j) {
				sg_set_page(dst, sg_page(src), src->length,
					    src->offset);
				src = sg_next(src);
				dst = sg_next(dst);
			}
		} else {
			ret = dma_get_sgtable(rcdu->dev, sgt, gem->vaddr,
					      gem->dma_addr, gem->base.size);
			if (ret)
				goto fail;
		}

		ret = vsp1_du_map_sg(vsp->vsp, sgt);
		if (ret) {
			sg_free_table(sgt);
			goto fail;
		}
	}

	return 0;

fail:
	while (i--) {
		struct sg_table *sgt = &sg_tables[i];

		vsp1_du_unmap_sg(vsp->vsp, sgt);
		sg_free_table(sgt);
	}

	return ret;
}

void rcar_du_vsp_unmap_fb(struct rcar_du_vsp *vsp, struct drm_framebuffer *fb,
			  struct sg_table sg_tables[3])
{
	unsigned int i;

	for (i = 0; i < fb->format->num_planes; ++i) {
		struct sg_table *sgt = &sg_tables[i];

		vsp1_du_unmap_sg(vsp->vsp, sgt);
		sg_free_table(sgt);
	}
}
