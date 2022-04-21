/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * RZ/G2L Display Unit DRM driver
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 *
 * Based on rcar_du_drv.h
 */

#ifndef __RZG2L_DU_DRV_H__
#define __RZG2L_DU_DRV_H__

#include "rcar_du_drv.h"

#include "rzg2l_du_crtc.h"
#include "rzg2l_du_vsp.h"

const char *rzg2l_du_output_name(enum rcar_du_output output);

#endif /* __RZG2L_DU_DRV_H__ */
