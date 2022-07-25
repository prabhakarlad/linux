// SPDX-License-Identifier: GPL-2.0+
/*
 * R-Car Display Unit Encoder
 *
 * Copyright (C) 2013-2022 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#include <linux/export.h>
#include <linux/of.h>

#include "rcar_du_drv.h"
#include "rcar_du_encoder.h"

/* -----------------------------------------------------------------------------
 * Encoder
 */

int rcar_du_encoder_init(struct rcar_du_device *rcdu,
			 enum rcar_du_output output,
			 struct device_node *enc_node)
{
	return rcar_du_lib_encoder_init(rcdu, output, enc_node,
					rcar_du_output_name(output));
}
