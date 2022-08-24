/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * R-Car Display Unit Mode Setting Lib
 *
 * Copyright (C) 2013-2022 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#ifndef __RCAR_DU_KMS_LIB_H__
#define __RCAR_DU_KMS_LIB_H__

#include <linux/types.h>

struct drm_device;
struct drm_file;
struct drm_mode_create_dumb;

struct rcar_du_format_info {
	u32 fourcc;
	u32 v4l2;
	unsigned int bpp;
	unsigned int planes;
	unsigned int hsub;
	unsigned int pnmr;
	unsigned int edf;
};

const struct rcar_du_format_info *rcar_du_format_info(u32 fourcc);

int rcar_du_dumb_create(struct drm_file *file, struct drm_device *dev,
			struct drm_mode_create_dumb *args);

#endif /* __RCAR_DU_KMS_LIB_H__ */
