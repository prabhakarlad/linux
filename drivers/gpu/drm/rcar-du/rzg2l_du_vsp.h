/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * RZ/G2L Display Unit VSP-Based Compositor
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 *
 * Based on rcar_du_vsp.h
 */

#ifndef __RZG2L_DU_VSP_H__
#define __RZG2L_DU_VSP_H__

#include "rcar_du_vsp_lib.h"

#ifdef CONFIG_DRM_RCAR_VSP
int rzg2l_du_vsp_init(struct rcar_du_vsp *vsp, struct device_node *np,
		      unsigned int crtcs);
void rzg2l_du_vsp_enable(struct rcar_du_crtc *crtc);
#else
static inline int rzg2l_du_vsp_init(struct rcar_du_vsp *vsp,
				    struct device_node *np,
				    unsigned int crtcs)
{
	return -ENXIO;
}
static inline void rzg2l_du_vsp_enable(struct rcar_du_crtc *crtc) { };
#endif

#endif /* __RZG2L_DU_VSP_H__ */
