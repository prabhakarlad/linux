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

struct rcar_du_device;

int rcar_du_modeset_init(struct rcar_du_device *rcdu);

#endif /* __RCAR_DU_KMS_H__ */
