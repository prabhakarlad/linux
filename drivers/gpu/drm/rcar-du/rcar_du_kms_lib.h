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

struct dma_buf_attachment;
struct drm_device;
struct drm_file;
struct drm_gem_object;
struct drm_mode_create_dumb;
struct sg_table;

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

struct drm_gem_object *rcar_du_gem_prime_import_sg_table(struct drm_device *dev,
				struct dma_buf_attachment *attach,
				struct sg_table *sgt);

struct drm_framebuffer *
rcar_du_lib_fb_create(struct drm_device *dev, struct drm_file *file_priv,
		      const struct drm_mode_fb_cmd2 *mode_cmd);

const struct drm_mode_config_helper_funcs *
rcar_du_lib_mode_cfg_helper_fns(void);

int rcar_du_encoders_init(struct rcar_du_device *rcdu,
			  const char* (*out_name)(enum rcar_du_output output),
			  int (*encoder_init_fn)(struct rcar_du_device *rcdu,
						 enum rcar_du_output output,
						 struct device_node *enc_node));

#endif /* __RCAR_DU_KMS_LIB_H__ */
