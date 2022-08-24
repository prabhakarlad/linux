// SPDX-License-Identifier: GPL-2.0+
/*
 * R-Car Display Unit Mode Setting Lib
 *
 * Copyright (C) 2013-2022 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>

#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/videodev2.h>

#include "rcar_du_drv.h"
#include "rcar_du_kms.h"
#include "rcar_du_regs.h"

/* -----------------------------------------------------------------------------
 * Format helpers
 */

static const struct rcar_du_format_info rcar_du_format_infos[] = {
	{
		.fourcc = DRM_FORMAT_RGB565,
		.v4l2 = V4L2_PIX_FMT_RGB565,
		.bpp = 16,
		.planes = 1,
		.hsub = 1,
		.pnmr = PnMR_SPIM_TP | PnMR_DDDF_16BPP,
		.edf = PnDDCR4_EDF_NONE,
	}, {
		.fourcc = DRM_FORMAT_ARGB1555,
		.v4l2 = V4L2_PIX_FMT_ARGB555,
		.bpp = 16,
		.planes = 1,
		.hsub = 1,
		.pnmr = PnMR_SPIM_ALP | PnMR_DDDF_ARGB,
		.edf = PnDDCR4_EDF_NONE,
	}, {
		.fourcc = DRM_FORMAT_XRGB1555,
		.v4l2 = V4L2_PIX_FMT_XRGB555,
		.bpp = 16,
		.planes = 1,
		.pnmr = PnMR_SPIM_ALP | PnMR_DDDF_ARGB,
		.edf = PnDDCR4_EDF_NONE,
	}, {
		.fourcc = DRM_FORMAT_XRGB8888,
		.v4l2 = V4L2_PIX_FMT_XBGR32,
		.bpp = 32,
		.planes = 1,
		.hsub = 1,
		.pnmr = PnMR_SPIM_TP | PnMR_DDDF_16BPP,
		.edf = PnDDCR4_EDF_RGB888,
	}, {
		.fourcc = DRM_FORMAT_ARGB8888,
		.v4l2 = V4L2_PIX_FMT_ABGR32,
		.bpp = 32,
		.planes = 1,
		.hsub = 1,
		.pnmr = PnMR_SPIM_ALP | PnMR_DDDF_16BPP,
		.edf = PnDDCR4_EDF_ARGB8888,
	}, {
		.fourcc = DRM_FORMAT_UYVY,
		.v4l2 = V4L2_PIX_FMT_UYVY,
		.bpp = 16,
		.planes = 1,
		.hsub = 2,
		.pnmr = PnMR_SPIM_TP_OFF | PnMR_DDDF_YC,
		.edf = PnDDCR4_EDF_NONE,
	}, {
		.fourcc = DRM_FORMAT_YUYV,
		.v4l2 = V4L2_PIX_FMT_YUYV,
		.bpp = 16,
		.planes = 1,
		.hsub = 2,
		.pnmr = PnMR_SPIM_TP_OFF | PnMR_DDDF_YC,
		.edf = PnDDCR4_EDF_NONE,
	}, {
		.fourcc = DRM_FORMAT_NV12,
		.v4l2 = V4L2_PIX_FMT_NV12M,
		.bpp = 12,
		.planes = 2,
		.hsub = 2,
		.pnmr = PnMR_SPIM_TP_OFF | PnMR_DDDF_YC,
		.edf = PnDDCR4_EDF_NONE,
	}, {
		.fourcc = DRM_FORMAT_NV21,
		.v4l2 = V4L2_PIX_FMT_NV21M,
		.bpp = 12,
		.planes = 2,
		.hsub = 2,
		.pnmr = PnMR_SPIM_TP_OFF | PnMR_DDDF_YC,
		.edf = PnDDCR4_EDF_NONE,
	}, {
		.fourcc = DRM_FORMAT_NV16,
		.v4l2 = V4L2_PIX_FMT_NV16M,
		.bpp = 16,
		.planes = 2,
		.hsub = 2,
		.pnmr = PnMR_SPIM_TP_OFF | PnMR_DDDF_YC,
		.edf = PnDDCR4_EDF_NONE,
	},
	/*
	 * The following formats are not supported on Gen2 and thus have no
	 * associated .pnmr or .edf settings.
	 */
	{
		.fourcc = DRM_FORMAT_RGB332,
		.v4l2 = V4L2_PIX_FMT_RGB332,
		.bpp = 8,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_ARGB4444,
		.v4l2 = V4L2_PIX_FMT_ARGB444,
		.bpp = 16,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_XRGB4444,
		.v4l2 = V4L2_PIX_FMT_XRGB444,
		.bpp = 16,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_RGBA4444,
		.v4l2 = V4L2_PIX_FMT_RGBA444,
		.bpp = 16,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_RGBX4444,
		.v4l2 = V4L2_PIX_FMT_RGBX444,
		.bpp = 16,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_ABGR4444,
		.v4l2 = V4L2_PIX_FMT_ABGR444,
		.bpp = 16,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_XBGR4444,
		.v4l2 = V4L2_PIX_FMT_XBGR444,
		.bpp = 16,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_BGRA4444,
		.v4l2 = V4L2_PIX_FMT_BGRA444,
		.bpp = 16,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_BGRX4444,
		.v4l2 = V4L2_PIX_FMT_BGRX444,
		.bpp = 16,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_RGBA5551,
		.v4l2 = V4L2_PIX_FMT_RGBA555,
		.bpp = 16,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_RGBX5551,
		.v4l2 = V4L2_PIX_FMT_RGBX555,
		.bpp = 16,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_ABGR1555,
		.v4l2 = V4L2_PIX_FMT_ABGR555,
		.bpp = 16,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_XBGR1555,
		.v4l2 = V4L2_PIX_FMT_XBGR555,
		.bpp = 16,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_BGRA5551,
		.v4l2 = V4L2_PIX_FMT_BGRA555,
		.bpp = 16,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_BGRX5551,
		.v4l2 = V4L2_PIX_FMT_BGRX555,
		.bpp = 16,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_BGR888,
		.v4l2 = V4L2_PIX_FMT_RGB24,
		.bpp = 24,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_RGB888,
		.v4l2 = V4L2_PIX_FMT_BGR24,
		.bpp = 24,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_RGBA8888,
		.v4l2 = V4L2_PIX_FMT_BGRA32,
		.bpp = 32,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_RGBX8888,
		.v4l2 = V4L2_PIX_FMT_BGRX32,
		.bpp = 32,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_ABGR8888,
		.v4l2 = V4L2_PIX_FMT_RGBA32,
		.bpp = 32,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_XBGR8888,
		.v4l2 = V4L2_PIX_FMT_RGBX32,
		.bpp = 32,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_BGRA8888,
		.v4l2 = V4L2_PIX_FMT_ARGB32,
		.bpp = 32,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_BGRX8888,
		.v4l2 = V4L2_PIX_FMT_XRGB32,
		.bpp = 32,
		.planes = 1,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_YVYU,
		.v4l2 = V4L2_PIX_FMT_YVYU,
		.bpp = 16,
		.planes = 1,
		.hsub = 2,
	}, {
		.fourcc = DRM_FORMAT_NV61,
		.v4l2 = V4L2_PIX_FMT_NV61M,
		.bpp = 16,
		.planes = 2,
		.hsub = 2,
	}, {
		.fourcc = DRM_FORMAT_YUV420,
		.v4l2 = V4L2_PIX_FMT_YUV420M,
		.bpp = 12,
		.planes = 3,
		.hsub = 2,
	}, {
		.fourcc = DRM_FORMAT_YVU420,
		.v4l2 = V4L2_PIX_FMT_YVU420M,
		.bpp = 12,
		.planes = 3,
		.hsub = 2,
	}, {
		.fourcc = DRM_FORMAT_YUV422,
		.v4l2 = V4L2_PIX_FMT_YUV422M,
		.bpp = 16,
		.planes = 3,
		.hsub = 2,
	}, {
		.fourcc = DRM_FORMAT_YVU422,
		.v4l2 = V4L2_PIX_FMT_YVU422M,
		.bpp = 16,
		.planes = 3,
		.hsub = 2,
	}, {
		.fourcc = DRM_FORMAT_YUV444,
		.v4l2 = V4L2_PIX_FMT_YUV444M,
		.bpp = 24,
		.planes = 3,
		.hsub = 1,
	}, {
		.fourcc = DRM_FORMAT_YVU444,
		.v4l2 = V4L2_PIX_FMT_YVU444M,
		.bpp = 24,
		.planes = 3,
		.hsub = 1,
	},
};

const struct rcar_du_format_info *rcar_du_format_info(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rcar_du_format_infos); ++i) {
		if (rcar_du_format_infos[i].fourcc == fourcc)
			return &rcar_du_format_infos[i];
	}

	return NULL;
}

/* -----------------------------------------------------------------------------
 * Frame buffer
 */

static const struct drm_gem_object_funcs rcar_du_gem_funcs = {
	.free = drm_gem_dma_object_free,
	.print_info = drm_gem_dma_object_print_info,
	.get_sg_table = drm_gem_dma_object_get_sg_table,
	.vmap = drm_gem_dma_object_vmap,
	.mmap = drm_gem_dma_object_mmap,
	.vm_ops = &drm_gem_dma_vm_ops,
};

struct drm_gem_object *rcar_du_gem_prime_import_sg_table(struct drm_device *dev,
				struct dma_buf_attachment *attach,
				struct sg_table *sgt)
{
	struct rcar_du_device *rcdu = to_rcar_du_device(dev);
	struct drm_gem_dma_object *dma_obj;
	struct drm_gem_object *gem_obj;
	int ret;

	if (!rcar_du_has(rcdu, RCAR_DU_FEATURE_VSP1_SOURCE))
		return drm_gem_dma_prime_import_sg_table(dev, attach, sgt);

	/* Create a DMA GEM buffer. */
	dma_obj = kzalloc(sizeof(*dma_obj), GFP_KERNEL);
	if (!dma_obj)
		return ERR_PTR(-ENOMEM);

	gem_obj = &dma_obj->base;
	gem_obj->funcs = &rcar_du_gem_funcs;

	drm_gem_private_object_init(dev, gem_obj, attach->dmabuf->size);
	dma_obj->map_noncoherent = false;

	ret = drm_gem_create_mmap_offset(gem_obj);
	if (ret) {
		drm_gem_object_release(gem_obj);
		kfree(dma_obj);
		return ERR_PTR(ret);
	}

	dma_obj->dma_addr = 0;
	dma_obj->sgt = sgt;

	return gem_obj;
}

int rcar_du_dumb_create(struct drm_file *file, struct drm_device *dev,
			struct drm_mode_create_dumb *args)
{
	struct rcar_du_device *rcdu = to_rcar_du_device(dev);
	unsigned int min_pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	unsigned int align;

	/*
	 * The R8A7779 DU requires a 16 pixels pitch alignment as documented,
	 * but the R8A7790 DU seems to require a 128 bytes pitch alignment.
	 */
	if (rcar_du_needs(rcdu, RCAR_DU_QUIRK_ALIGN_128B))
		align = 128;
	else
		align = 16 * args->bpp / 8;

	args->pitch = roundup(min_pitch, align);

	return drm_gem_dma_dumb_create_internal(file, dev, args);
}

struct drm_framebuffer *
rcar_du_lib_fb_create(struct drm_device *dev, struct drm_file *file_priv,
		      const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct rcar_du_device *rcdu = to_rcar_du_device(dev);
	const struct rcar_du_format_info *format;
	unsigned int chroma_pitch;
	unsigned int max_pitch;
	unsigned int align;
	unsigned int i;

	format = rcar_du_format_info(mode_cmd->pixel_format);
	if (format == NULL) {
		dev_dbg(dev->dev, "unsupported pixel format %p4cc\n",
			&mode_cmd->pixel_format);
		return ERR_PTR(-EINVAL);
	}

	if (rcdu->info->gen < 3) {
		/*
		 * On Gen2 the DU limits the pitch to 4095 pixels and requires
		 * buffers to be aligned to a 16 pixels boundary (or 128 bytes
		 * on some platforms).
		 */
		unsigned int bpp = format->planes == 1 ? format->bpp / 8 : 1;

		max_pitch = 4095 * bpp;

		if (rcar_du_needs(rcdu, RCAR_DU_QUIRK_ALIGN_128B))
			align = 128;
		else
			align = 16 * bpp;
	} else {
		/*
		 * On Gen3 the memory interface is handled by the VSP that
		 * limits the pitch to 65535 bytes and has no alignment
		 * constraint.
		 */
		max_pitch = 65535;
		align = 1;
	}

	if (mode_cmd->pitches[0] & (align - 1) ||
	    mode_cmd->pitches[0] > max_pitch) {
		dev_dbg(dev->dev, "invalid pitch value %u\n",
			mode_cmd->pitches[0]);
		return ERR_PTR(-EINVAL);
	}

	/*
	 * Calculate the chroma plane(s) pitch using the horizontal subsampling
	 * factor. For semi-planar formats, the U and V planes are combined, the
	 * pitch must thus be doubled.
	 */
	chroma_pitch = mode_cmd->pitches[0] / format->hsub;
	if (format->planes == 2)
		chroma_pitch *= 2;

	for (i = 1; i < format->planes; ++i) {
		if (mode_cmd->pitches[i] != chroma_pitch) {
			dev_dbg(dev->dev,
				"luma and chroma pitches are not compatible\n");
			return ERR_PTR(-EINVAL);
		}
	}

	return drm_gem_fb_create(dev, file_priv, mode_cmd);
}

/* -----------------------------------------------------------------------------
 * Atomic Check and Update
 */

static void rcar_du_atomic_commit_tail(struct drm_atomic_state *old_state)
{
	struct drm_device *dev = old_state->dev;
	struct rcar_du_device *rcdu = to_rcar_du_device(dev);
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc;
	unsigned int i;

	/*
	 * Store RGB routing to DPAD0 and DPAD1, the hardware will be configured
	 * when starting the CRTCs.
	 */
	rcdu->dpad1_source = -1;

	for_each_new_crtc_in_state(old_state, crtc, crtc_state, i) {
		struct rcar_du_crtc_state *rcrtc_state =
			to_rcar_crtc_state(crtc_state);
		struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);

		if (rcrtc_state->outputs & BIT(RCAR_DU_OUTPUT_DPAD0))
			rcdu->dpad0_source = rcrtc->index;

		if (rcrtc_state->outputs & BIT(RCAR_DU_OUTPUT_DPAD1))
			rcdu->dpad1_source = rcrtc->index;
	}

	/* Apply the atomic update. */
	drm_atomic_helper_commit_modeset_disables(dev, old_state);
	drm_atomic_helper_commit_planes(dev, old_state,
					DRM_PLANE_COMMIT_ACTIVE_ONLY);
	drm_atomic_helper_commit_modeset_enables(dev, old_state);

	drm_atomic_helper_commit_hw_done(old_state);
	drm_atomic_helper_wait_for_flip_done(dev, old_state);

	drm_atomic_helper_cleanup_planes(dev, old_state);
}

/* -----------------------------------------------------------------------------
 * Initialization
 */

static const struct drm_mode_config_helper_funcs rcar_du_mode_config_helper = {
	.atomic_commit_tail = rcar_du_atomic_commit_tail,
};

const struct drm_mode_config_helper_funcs *
rcar_du_lib_mode_cfg_helper_fns(void)
{
	return &rcar_du_mode_config_helper;
}

static int
rcar_du_encoders_init_one(struct rcar_du_device *rcdu,
			  enum rcar_du_output output,
			  struct of_endpoint *ep,
			  const char *output_name,
			  int (*rcar_du_encoder_init_fn)(struct rcar_du_device *r,
							 enum rcar_du_output op,
							 struct device_node *d))
{
	struct device_node *entity;
	int ret;

	/* Locate the connected entity and initialize the encoder. */
	entity = of_graph_get_remote_port_parent(ep->local_node);
	if (!entity) {
		dev_dbg(rcdu->dev, "unconnected endpoint %pOF, skipping\n",
			ep->local_node);
		return -ENODEV;
	}

	if (!of_device_is_available(entity)) {
		dev_dbg(rcdu->dev,
			"connected entity %pOF is disabled, skipping\n",
			entity);
		of_node_put(entity);
		return -ENODEV;
	}

	ret = rcar_du_encoder_init_fn(rcdu, output, entity);
	if (ret && ret != -EPROBE_DEFER && ret != -ENOLINK)
		dev_warn(rcdu->dev,
			 "failed to initialize encoder %pOF on output %s (%d), skipping\n",
			 entity, output_name, ret);

	of_node_put(entity);

	return ret;
}

int rcar_du_encoders_init(struct rcar_du_device *rcdu,
			  const char* (*out_name)(enum rcar_du_output output),
			  int (*encoder_init_fn)(struct rcar_du_device *rcdu,
						 enum rcar_du_output output,
						 struct device_node *enc_node))
{
	struct device_node *np = rcdu->dev->of_node;
	struct device_node *ep_node;
	unsigned int num_encoders = 0;

	/*
	 * Iterate over the endpoints and create one encoder for each output
	 * pipeline.
	 */
	for_each_endpoint_of_node(np, ep_node) {
		enum rcar_du_output output;
		struct of_endpoint ep;
		unsigned int i;
		int ret;

		ret = of_graph_parse_endpoint(ep_node, &ep);
		if (ret < 0) {
			of_node_put(ep_node);
			return ret;
		}

		/* Find the output route corresponding to the port number. */
		for (i = 0; i < RCAR_DU_OUTPUT_MAX; ++i) {
			if (rcdu->info->routes[i].possible_crtcs &&
			    rcdu->info->routes[i].port == ep.port) {
				output = i;
				break;
			}
		}

		if (i == RCAR_DU_OUTPUT_MAX) {
			dev_warn(rcdu->dev,
				 "port %u references unexisting output, skipping\n",
				 ep.port);
			continue;
		}

		/* Process the output pipeline. */
		ret = rcar_du_encoders_init_one(rcdu, output, &ep,
						out_name(output),
						encoder_init_fn);
		if (ret < 0) {
			if (ret == -EPROBE_DEFER) {
				of_node_put(ep_node);
				return ret;
			}

			continue;
		}

		num_encoders++;
	}

	return num_encoders;
}

int rcar_du_properties_init(struct rcar_du_device *rcdu)
{
	/*
	 * The color key is expressed as an RGB888 triplet stored in a 32-bit
	 * integer in XRGB8888 format. Bit 24 is used as a flag to disable (0)
	 * or enable source color keying (1).
	 */
	rcdu->props.colorkey =
		drm_property_create_range(&rcdu->ddev, 0, "colorkey",
					  0, 0x01ffffff);
	if (rcdu->props.colorkey == NULL)
		return -ENOMEM;

	return 0;
}

int rcar_du_lib_vsps_init(struct rcar_du_device *rcdu,
			  int (*rcar_du_vsp_init_fn)(struct rcar_du_vsp *vsp,
						     struct device_node *np,
						     unsigned int crtcs))
{
	const struct device_node *np = rcdu->dev->of_node;
	const char *vsps_prop_name = "renesas,vsps";
	struct of_phandle_args args;
	struct {
		struct device_node *np;
		unsigned int crtcs_mask;
	} vsps[RCAR_DU_MAX_VSPS] = { { NULL, }, };
	unsigned int vsps_count = 0;
	unsigned int cells;
	unsigned int i;
	int ret;

	/*
	 * First parse the DT vsps property to populate the list of VSPs. Each
	 * entry contains a pointer to the VSP DT node and a bitmask of the
	 * connected DU CRTCs.
	 */
	ret = of_property_count_u32_elems(np, vsps_prop_name);
	if (ret < 0) {
		/* Backward compatibility with old DTBs. */
		vsps_prop_name = "vsps";
		ret = of_property_count_u32_elems(np, vsps_prop_name);
	}
	cells = ret / rcdu->num_crtcs - 1;
	if (cells > 1)
		return -EINVAL;

	for (i = 0; i < rcdu->num_crtcs; ++i) {
		unsigned int j;

		ret = of_parse_phandle_with_fixed_args(np, vsps_prop_name,
						       cells, i, &args);
		if (ret < 0)
			goto error;

		/*
		 * Add the VSP to the list or update the corresponding existing
		 * entry if the VSP has already been added.
		 */
		for (j = 0; j < vsps_count; ++j) {
			if (vsps[j].np == args.np)
				break;
		}

		if (j < vsps_count)
			of_node_put(args.np);
		else
			vsps[vsps_count++].np = args.np;

		vsps[j].crtcs_mask |= BIT(i);

		/*
		 * Store the VSP pointer and pipe index in the CRTC. If the
		 * second cell of the 'renesas,vsps' specifier isn't present,
		 * default to 0 to remain compatible with older DT bindings.
		 */
		rcdu->crtcs[i].vsp = &rcdu->vsps[j];
		rcdu->crtcs[i].vsp_pipe = cells >= 1 ? args.args[0] : 0;
	}

	/*
	 * Then initialize all the VSPs from the node pointers and CRTCs bitmask
	 * computed previously.
	 */
	for (i = 0; i < vsps_count; ++i) {
		struct rcar_du_vsp *vsp = &rcdu->vsps[i];

		vsp->index = i;
		vsp->dev = rcdu;

		ret = rcar_du_vsp_init_fn(vsp, vsps[i].np, vsps[i].crtcs_mask);
		if (ret < 0)
			goto error;
	}

	return 0;

error:
	for (i = 0; i < ARRAY_SIZE(vsps); ++i)
		of_node_put(vsps[i].np);

	return ret;
}
