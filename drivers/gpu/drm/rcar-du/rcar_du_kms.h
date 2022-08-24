/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * R-Car Display Unit Mode Setting
 *
 * Copyright (C) 2013-2022 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#ifndef __RCAR_DU_KMS_H__
#define __RCAR_DU_KMS_H__

#include "rcar_du_kms_lib.h"

struct dma_buf_attachment;
struct drm_device;
struct drm_gem_object;
struct rcar_du_device;
struct sg_table;

int rcar_du_modeset_init(struct rcar_du_device *rcdu);

struct drm_gem_object *rcar_du_gem_prime_import_sg_table(struct drm_device *dev,
				struct dma_buf_attachment *attach,
				struct sg_table *sgt);

#endif /* __RCAR_DU_KMS_H__ */
