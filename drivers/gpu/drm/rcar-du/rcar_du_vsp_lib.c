// SPDX-License-Identifier: GPL-2.0+
/*
 * R-Car Display Unit VSP-Based Compositor Lib
 *
 * Copyright (C) 2015-2022 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>

#include <linux/dma-mapping.h>
#include <linux/of_platform.h>
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

static const u32 rcar_du_vsp_formats[] = {
	DRM_FORMAT_RGB332,
	DRM_FORMAT_ARGB4444,
	DRM_FORMAT_XRGB4444,
	DRM_FORMAT_ARGB1555,
	DRM_FORMAT_XRGB1555,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGRA8888,
	DRM_FORMAT_BGRX8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_YUYV,
	DRM_FORMAT_YVYU,
	DRM_FORMAT_NV12,
	DRM_FORMAT_NV21,
	DRM_FORMAT_NV16,
	DRM_FORMAT_NV61,
	DRM_FORMAT_YUV420,
	DRM_FORMAT_YVU420,
	DRM_FORMAT_YUV422,
	DRM_FORMAT_YVU422,
	DRM_FORMAT_YUV444,
	DRM_FORMAT_YVU444,
};

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

static struct drm_plane_state *
rcar_du_vsp_plane_atomic_duplicate_state(struct drm_plane *plane)
{
	struct rcar_du_vsp_plane_state *copy;

	if (WARN_ON(!plane->state))
		return NULL;

	copy = kzalloc(sizeof(*copy), GFP_KERNEL);
	if (copy == NULL)
		return NULL;

	__drm_atomic_helper_plane_duplicate_state(plane, &copy->state);

	return &copy->state;
}

static void rcar_du_vsp_plane_atomic_destroy_state(struct drm_plane *plane,
						   struct drm_plane_state *state)
{
	__drm_atomic_helper_plane_destroy_state(state);
	kfree(to_rcar_vsp_plane_state(state));
}

static void rcar_du_vsp_plane_reset(struct drm_plane *plane)
{
	struct rcar_du_vsp_plane_state *state;

	if (plane->state) {
		rcar_du_vsp_plane_atomic_destroy_state(plane, plane->state);
		plane->state = NULL;
	}

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state == NULL)
		return;

	__drm_atomic_helper_plane_reset(plane, &state->state);
}

static const struct drm_plane_funcs rcar_du_vsp_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.reset = rcar_du_vsp_plane_reset,
	.destroy = drm_plane_cleanup,
	.atomic_duplicate_state = rcar_du_vsp_plane_atomic_duplicate_state,
	.atomic_destroy_state = rcar_du_vsp_plane_atomic_destroy_state,
};

static void rcar_du_vsp_cleanup(struct drm_device *dev, void *res)
{
	struct rcar_du_vsp *vsp = res;
	unsigned int i;

	for (i = 0; i < vsp->num_planes; ++i) {
		struct rcar_du_vsp_plane *plane = &vsp->planes[i];

		drm_plane_cleanup(&plane->plane);
	}

	kfree(vsp->planes);

	put_device(vsp->vsp);
}

int rcar_du_lib_vsp_init(struct rcar_du_vsp *vsp, struct device_node *np,
			 unsigned int crtcs,
			 const struct drm_plane_helper_funcs *rcar_du_vsp_plane_helper_funcs)
{
	struct rcar_du_device *rcdu = vsp->dev;
	struct platform_device *pdev;
	unsigned int num_crtcs = hweight32(crtcs);
	unsigned int num_planes;
	unsigned int i;
	int ret;

	/* Find the VSP device and initialize it. */
	pdev = of_find_device_by_node(np);
	if (!pdev)
		return -ENXIO;

	vsp->vsp = &pdev->dev;

	ret = drmm_add_action_or_reset(&rcdu->ddev, rcar_du_vsp_cleanup, vsp);
	if (ret < 0)
		return ret;

	ret = vsp1_du_init(vsp->vsp);
	if (ret < 0)
		return ret;

	num_planes = rcdu->info->num_rpf;

	vsp->planes = kcalloc(num_planes, sizeof(*vsp->planes), GFP_KERNEL);
	if (!vsp->planes)
		return -ENOMEM;

	for (i = 0; i < num_planes; ++i) {
		enum drm_plane_type type = i < num_crtcs
					 ? DRM_PLANE_TYPE_PRIMARY
					 : DRM_PLANE_TYPE_OVERLAY;
		struct rcar_du_vsp_plane *plane = &vsp->planes[i];

		plane->vsp = vsp;
		plane->index = i;

		ret = drm_universal_plane_init(&rcdu->ddev, &plane->plane,
					       crtcs, &rcar_du_vsp_plane_funcs,
					       rcar_du_vsp_formats,
					       ARRAY_SIZE(rcar_du_vsp_formats),
					       NULL, type, NULL);
		if (ret < 0)
			return ret;

		drm_plane_helper_add(&plane->plane,
				     rcar_du_vsp_plane_helper_funcs);

		drm_plane_create_alpha_property(&plane->plane);
		drm_plane_create_zpos_property(&plane->plane, i, 0,
					       num_planes - 1);

		drm_plane_create_blend_mode_property(&plane->plane,
					BIT(DRM_MODE_BLEND_PIXEL_NONE) |
					BIT(DRM_MODE_BLEND_PREMULTI) |
					BIT(DRM_MODE_BLEND_COVERAGE));

		vsp->num_planes++;
	}

	return 0;
}
