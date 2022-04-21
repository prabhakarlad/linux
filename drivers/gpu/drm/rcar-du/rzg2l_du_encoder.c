// SPDX-License-Identifier: GPL-2.0+
/*
 * RZ/G2L Display Unit Encoder
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 *
 * Based on rcar_du_encoder.c
 */

#include <linux/export.h>
#include <linux/of.h>

#include "rzg2l_du_drv.h"
#include "rzg2l_du_encoder.h"

/* -----------------------------------------------------------------------------
 * Encoder
 */

int rzg2l_du_encoder_init(struct rcar_du_device *rcdu,
			  enum rcar_du_output output,
			  struct device_node *enc_node)
{
	return rcar_du_lib_encoder_init(rcdu, output, enc_node,
					rzg2l_du_output_name(output));
}
