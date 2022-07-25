/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * R-Car Display Unit Encoder
 *
 * Copyright (C) 2013-2022 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#ifndef __RCAR_DU_ENCODER_H__
#define __RCAR_DU_ENCODER_H__

#include "rcar_du_encoder_lib.h"

int rcar_du_encoder_init(struct rcar_du_device *rcdu,
			 enum rcar_du_output output,
			 struct device_node *enc_node);

#endif /* __RCAR_DU_ENCODER_H__ */
