/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * RZ/G2L Display Unit CRTCs
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 *
 * Based on rcar_du_crtc.h
 */

#ifndef __RZG2L_DU_CRTC_H__
#define __RZG2L_DU_CRTC_H__

#include "rcar_du_crtc.h"

struct rcar_du_format_info;
struct reset_control;

int rzg2l_du_crtc_create(struct rcar_du_device *rcdu);

void rzg2l_du_crtc_finish_page_flip(struct rcar_du_crtc *rcrtc);

int __rzg2l_du_crtc_plane_atomic_check(struct drm_plane *plane,
				       struct drm_plane_state *state,
				       const struct rcar_du_format_info **format);

#endif /* __RZG2L_DU_CRTC_H__ */
