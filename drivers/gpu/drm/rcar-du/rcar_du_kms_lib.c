// SPDX-License-Identifier: GPL-2.0+
/*
 * R-Car Display Unit Mode Setting Lib
 *
 * Copyright (C) 2013-2022 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#include <drm/drm_device.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>

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
