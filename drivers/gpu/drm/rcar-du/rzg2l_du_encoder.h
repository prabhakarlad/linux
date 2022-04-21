/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * RZ/G2L Display Unit Encoder
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 *
 * Based on rcar_du_encoder.h
 */

#ifndef __RZG2L_DU_ENCODER_H__
#define __RZG2L_DU_ENCODER_H__

#include "rcar_du_encoder_lib.h"

int rzg2l_du_encoder_init(struct rcar_du_device *rcdu,
			  enum rcar_du_output output,
			  struct device_node *enc_node);

#endif /* __RZG2L_DU_ENCODER_H__ */
