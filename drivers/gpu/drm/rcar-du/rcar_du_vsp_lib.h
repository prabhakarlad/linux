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

struct drm_framebuffer;
struct rcar_du_vsp;
struct sg_table;

#ifdef CONFIG_DRM_RCAR_VSP
void rcar_du_vsp_disable(struct rcar_du_crtc *crtc);
void rcar_du_vsp_atomic_begin(struct rcar_du_crtc *crtc);
void rcar_du_vsp_atomic_flush(struct rcar_du_crtc *crtc);
int rcar_du_vsp_map_fb(struct rcar_du_vsp *vsp, struct drm_framebuffer *fb,
		       struct sg_table sg_tables[3]);
void rcar_du_vsp_unmap_fb(struct rcar_du_vsp *vsp, struct drm_framebuffer *fb,
			  struct sg_table sg_tables[3]);
int rcar_du_lib_vsp_init(struct rcar_du_vsp *vsp, struct device_node *np,
			 unsigned int crtcs,
			 const struct drm_plane_helper_funcs *rcar_du_vsp_plane_helper_funcs);
int rcar_du_vsp_plane_prepare_fb(struct drm_plane *plane,
				 struct drm_plane_state *state);
void rcar_du_vsp_plane_cleanup_fb(struct drm_plane *plane,
				  struct drm_plane_state *state);
#else
static inline void rcar_du_vsp_disable(struct rcar_du_crtc *crtc) { };
static inline void rcar_du_vsp_atomic_begin(struct rcar_du_crtc *crtc) { };
static inline void rcar_du_vsp_atomic_flush(struct rcar_du_crtc *crtc) { };
static inline int rcar_du_vsp_map_fb(struct rcar_du_vsp *vsp,
				     struct drm_framebuffer *fb,
				     struct sg_table sg_tables[3])
{
	return -ENXIO;
}
static inline void rcar_du_vsp_unmap_fb(struct rcar_du_vsp *vsp,
					struct drm_framebuffer *fb,
					struct sg_table sg_tables[3])
{
}
static inline int
rcar_du_lib_vsp_init(struct rcar_du_vsp *vsp, struct device_node *np,
		     unsigned int crtcs,
		     const struct drm_plane_helper_funcs *rcar_du_vsp_plane_helper_funcs)
{
	return -ENXIO;
}
static inline int rcar_du_vsp_plane_prepare_fb(struct drm_plane *plane,
					       struct drm_plane_state *state)
{
	return -ENXIO;
}
static inline void rcar_du_vsp_plane_cleanup_fb(struct drm_plane *plane,
						struct drm_plane_state *state)
{
}
#endif

#endif /* __RCAR_DU_VSP_LIB_H__ */
