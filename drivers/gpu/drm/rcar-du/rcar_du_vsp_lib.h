/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * R-Car Display Unit VSP-Based Compositor Lib
 *
 * Copyright (C) 2015-2022 Renesas Electronics Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#ifndef __RCAR_DU_VSP_LIB_H__
#define __RCAR_DU_VSP_LIB_H__

#ifdef CONFIG_DRM_RCAR_VSP
void rcar_du_vsp_disable(struct rcar_du_crtc *crtc);
void rcar_du_vsp_atomic_begin(struct rcar_du_crtc *crtc);
#else
static inline void rcar_du_vsp_disable(struct rcar_du_crtc *crtc) { };
static inline void rcar_du_vsp_atomic_begin(struct rcar_du_crtc *crtc) { };
#endif

#endif /* __RCAR_DU_VSP_LIB_H__ */
