// SPDX-License-Identifier: GPL-2.0+
/*
 * RZ/G2L DU CRTCs
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 *
 * Based on rcar_du_crtc.c
 */

#include <linux/clk.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/sys_soc.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_vblank.h>

#include "rzg2l_du_crtc.h"
#include "rzg2l_du_drv.h"
#include "rzg2l_du_encoder.h"
#include "rzg2l_du_kms.h"
#include "rzg2l_du_vsp.h"
#include "rzg2l_du_regs.h"

/* -----------------------------------------------------------------------------
 * Hardware Setup
 */

static void rzg2l_du_crtc_set_display_timing(struct rcar_du_crtc *rcrtc)
{
	const struct drm_display_mode *mode = &rcrtc->crtc.state->adjusted_mode;
	struct rcar_du_device *rcdu = rcrtc->dev;
	unsigned long mode_clock = mode->clock * 1000;
	u32 ditr0, ditr1, ditr2, ditr3, ditr4, ditr5, pbcr0;
	struct clk *parent_clk;

	parent_clk = clk_get_parent(rcrtc->rzg2l_clocks.dclk);
	clk_set_rate(parent_clk, mode_clock);

	clk_prepare_enable(rcrtc->rzg2l_clocks.dclk);

	ditr0 = (DU_DITR0_DEMD_HIGH
		 | ((mode->flags & DRM_MODE_FLAG_PVSYNC) ? DU_DITR0_VSPOL : 0)
		 | ((mode->flags & DRM_MODE_FLAG_PHSYNC) ? DU_DITR0_HSPOL : 0));

	ditr1 = DU_DITR1_VSA(mode->vsync_end - mode->vsync_start)
		| DU_DITR1_VACTIVE(mode->vdisplay);

	ditr2 = DU_DITR2_VBP(mode->vtotal - mode->vsync_end)
		| DU_DITR2_VFP(mode->vsync_start - mode->vdisplay);

	ditr3 = DU_DITR3_HSA(mode->hsync_end - mode->hsync_start)
		| DU_DITR3_HACTIVE(mode->hdisplay);

	ditr4 = DU_DITR4_HBP(mode->htotal - mode->hsync_end)
		| DU_DITR4_HFP(mode->hsync_start - mode->hdisplay);

	ditr5 = DU_DITR5_VSFT(0) | DU_DITR5_HSFT(0);

	pbcr0 = DU_PBCR0_PB_DEP(0x1f);

	rcar_du_write(rcdu, DU_DITR0, ditr0);
	rcar_du_write(rcdu, DU_DITR1, ditr1);
	rcar_du_write(rcdu, DU_DITR2, ditr2);
	rcar_du_write(rcdu, DU_DITR3, ditr3);
	rcar_du_write(rcdu, DU_DITR4, ditr4);
	rcar_du_write(rcdu, DU_DITR5, ditr5);
	rcar_du_write(rcdu, DU_PBCR0, pbcr0);

	/* Enable auto resume when underrun */
	rcar_du_write(rcdu, DU_MCR1, DU_MCR1_PB_AUTOCLR);
}

/* -----------------------------------------------------------------------------
 * Page Flip
 */

void rzg2l_du_crtc_finish_page_flip(struct rcar_du_crtc *rcrtc)
{
	struct drm_pending_vblank_event *event;
	struct drm_device *dev = rcrtc->crtc.dev;
	unsigned long flags;

	spin_lock_irqsave(&dev->event_lock, flags);
	event = rcrtc->event;
	rcrtc->event = NULL;
	spin_unlock_irqrestore(&dev->event_lock, flags);

	if (!event)
		return;

	spin_lock_irqsave(&dev->event_lock, flags);
	drm_crtc_send_vblank_event(&rcrtc->crtc, event);
	wake_up(&rcrtc->flip_wait);
	spin_unlock_irqrestore(&dev->event_lock, flags);

	drm_crtc_vblank_put(&rcrtc->crtc);
}

static bool rzg2l_du_crtc_page_flip_pending(struct rcar_du_crtc *rcrtc)
{
	struct drm_device *dev = rcrtc->crtc.dev;
	unsigned long flags;
	bool pending;

	spin_lock_irqsave(&dev->event_lock, flags);
	pending = rcrtc->event;
	spin_unlock_irqrestore(&dev->event_lock, flags);

	return pending;
}

static void rzg2l_du_crtc_wait_page_flip(struct rcar_du_crtc *rcrtc)
{
	struct rcar_du_device *rcdu = rcrtc->dev;

	if (wait_event_timeout(rcrtc->flip_wait,
			       !rzg2l_du_crtc_page_flip_pending(rcrtc),
			       msecs_to_jiffies(50)))
		return;

	dev_warn(rcdu->dev, "page flip timeout\n");

	rzg2l_du_crtc_finish_page_flip(rcrtc);
}

/* -----------------------------------------------------------------------------
 * Start/Stop and Suspend/Resume
 */

static void rzg2l_du_crtc_setup(struct rcar_du_crtc *rcrtc)
{
	/* Configure display timings and output routing */
	rzg2l_du_crtc_set_display_timing(rcrtc);

	/* Enable the VSP compositor. */
	rzg2l_du_vsp_enable(rcrtc);

	/* Turn vertical blanking interrupt reporting on. */
	drm_crtc_vblank_on(&rcrtc->crtc);
}

static int rzg2l_du_crtc_get(struct rcar_du_crtc *rcrtc)
{
	int ret;

	/*
	 * Guard against double-get, as the function is called from both the
	 * .atomic_enable() and .atomic_begin() handlers.
	 */
	if (rcrtc->initialized)
		return 0;

	ret = reset_control_deassert(rcrtc->rstc);
	if (ret < 0)
		goto error_reset;

	ret = clk_prepare_enable(rcrtc->rzg2l_clocks.aclk);
	if (ret < 0)
		goto error_reset;

	ret = clk_prepare_enable(rcrtc->rzg2l_clocks.pclk);
	if (ret < 0)
		goto error_clock;

	rzg2l_du_crtc_setup(rcrtc);
	rcrtc->initialized = true;

	return 0;

error_clock:
	clk_disable_unprepare(rcrtc->rzg2l_clocks.aclk);
error_reset:
	reset_control_assert(rcrtc->rstc);
	return ret;
}

static void rzg2l_du_crtc_put(struct rcar_du_crtc *rcrtc)
{
	clk_disable_unprepare(rcrtc->rzg2l_clocks.aclk);
	clk_disable_unprepare(rcrtc->rzg2l_clocks.pclk);
	clk_disable_unprepare(rcrtc->rzg2l_clocks.dclk);
	reset_control_assert(rcrtc->rstc);

	rcrtc->initialized = false;
}

static void rzg2l_du_start_stop(struct rcar_du_crtc *rcrtc, bool start)
{
	struct rcar_du_device *rcdu = rcrtc->dev;

	rcar_du_write(rcdu, DU_MCR0, start ? DU_MCR0_DI_EN : 0);
}

static void rzg2l_du_crtc_start(struct rcar_du_crtc *rcrtc)
{
	rzg2l_du_start_stop(rcrtc, true);
}

static void rzg2l_du_crtc_disable_planes(struct rcar_du_crtc *rcrtc)
{
	struct rcar_du_device *rcdu = rcrtc->dev;
	struct drm_crtc *crtc = &rcrtc->crtc;

	/* Make sure vblank interrupts are enabled. */
	drm_crtc_vblank_get(crtc);

	if (!wait_event_timeout(rcrtc->vblank_wait, rcrtc->vblank_count == 0,
				msecs_to_jiffies(100)))
		dev_warn(rcdu->dev, "vertical blanking timeout\n");

	drm_crtc_vblank_put(crtc);
}

static void rzg2l_du_crtc_stop(struct rcar_du_crtc *rcrtc)
{
	struct drm_crtc *crtc = &rcrtc->crtc;

	/*
	 * Disable all planes and wait for the change to take effect. This is
	 * required as the plane enable registers are updated on vblank, and no
	 * vblank will occur once the CRTC is stopped. Disabling planes when
	 * starting the CRTC thus wouldn't be enough as it would start scanning
	 * out immediately from old frame buffers until the next vblank.
	 *
	 * This increases the CRTC stop delay, especially when multiple CRTCs
	 * are stopped in one operation as we now wait for one vblank per CRTC.
	 * Whether this can be improved needs to be researched.
	 */
	rzg2l_du_crtc_disable_planes(rcrtc);

	/*
	 * Disable vertical blanking interrupt reporting. We first need to wait
	 * for page flip completion before stopping the CRTC as userspace
	 * expects page flips to eventually complete.
	 */
	rzg2l_du_crtc_wait_page_flip(rcrtc);
	drm_crtc_vblank_off(crtc);

	/* Disable the VSP compositor. */
	rcar_du_vsp_disable(rcrtc);

	rzg2l_du_start_stop(rcrtc, false);
}

/* -----------------------------------------------------------------------------
 * CRTC Functions
 */

int __rzg2l_du_crtc_plane_atomic_check(struct drm_plane *plane,
				       struct drm_plane_state *state,
				       const struct rcar_du_format_info **format)
{
	struct drm_device *dev = plane->dev;
	struct drm_crtc_state *crtc_state;
	int ret;

	if (!state->crtc) {
		/*
		 * The visible field is not reset by the DRM core but only
		 * updated by drm_plane_helper_check_state(), set it manually.
		 */
		state->visible = false;
		*format = NULL;
		return 0;
	}

	crtc_state = drm_atomic_get_crtc_state(state->state, state->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	ret = drm_atomic_helper_check_plane_state(state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  true, true);
	if (ret < 0)
		return ret;

	if (!state->visible) {
		*format = NULL;
		return 0;
	}

	*format = rcar_du_format_info(state->fb->format->format);
	if (*format == NULL) {
		dev_dbg(dev->dev, "%s: unsupported format %08x\n", __func__,
			state->fb->format->format);
		return -EINVAL;
	}

	return 0;
}

static int rzg2l_du_crtc_atomic_check(struct drm_crtc *crtc,
				      struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state,
									  crtc);
	struct rcar_du_crtc_state *rstate = to_rcar_crtc_state(crtc_state);
	struct drm_encoder *encoder;

	/* Store the routes from the CRTC output to the DU outputs. */
	rstate->outputs = 0;

	drm_for_each_encoder_mask(encoder, crtc->dev,
				  crtc_state->encoder_mask) {
		struct rcar_du_encoder *renc;

		/* Skip the writeback encoder. */
		if (encoder->encoder_type == DRM_MODE_ENCODER_VIRTUAL)
			continue;

		renc = to_rcar_encoder(encoder);
		rstate->outputs |= BIT(renc->output);
	}

	return 0;
}

static void rzg2l_du_crtc_atomic_enable(struct drm_crtc *crtc,
					struct drm_atomic_state *state)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);

	rzg2l_du_crtc_get(rcrtc);

	rzg2l_du_crtc_start(rcrtc);
}

static void rzg2l_du_crtc_atomic_disable(struct drm_crtc *crtc,
					 struct drm_atomic_state *state)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);

	rzg2l_du_crtc_stop(rcrtc);
	rzg2l_du_crtc_put(rcrtc);

	spin_lock_irq(&crtc->dev->event_lock);
	if (crtc->state->event) {
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
	}
	spin_unlock_irq(&crtc->dev->event_lock);
}

static void rzg2l_du_crtc_atomic_begin(struct drm_crtc *crtc,
				       struct drm_atomic_state *state)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);

	WARN_ON(!crtc->state->enable);

	/*
	 * If a mode set is in progress we can be called with the CRTC disabled.
	 * We thus need to first get and setup the CRTC in order to configure
	 * planes. We must *not* put the CRTC in .atomic_flush(), as it must be
	 * kept awake until the .atomic_enable() call that will follow. The get
	 * operation in .atomic_enable() will in that case be a no-op, and the
	 * CRTC will be put later in .atomic_disable().
	 *
	 * If a mode set is not in progress the CRTC is enabled, and the
	 * following get call will be a no-op. There is thus no need to balance
	 * it in .atomic_flush() either.
	 */
	rzg2l_du_crtc_get(rcrtc);

	rcar_du_vsp_atomic_begin(rcrtc);
}

static void rzg2l_du_crtc_atomic_flush(struct drm_crtc *crtc,
				       struct drm_atomic_state *state)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);
	struct drm_device *dev = rcrtc->crtc.dev;
	unsigned long flags;

	if (crtc->state->event) {
		WARN_ON(drm_crtc_vblank_get(crtc) != 0);

		spin_lock_irqsave(&dev->event_lock, flags);
		rcrtc->event = crtc->state->event;
		crtc->state->event = NULL;
		spin_unlock_irqrestore(&dev->event_lock, flags);
	}

	rcar_du_vsp_atomic_flush(rcrtc);
}

static const struct drm_crtc_helper_funcs crtc_helper_funcs = {
	.atomic_check = rzg2l_du_crtc_atomic_check,
	.atomic_begin = rzg2l_du_crtc_atomic_begin,
	.atomic_flush = rzg2l_du_crtc_atomic_flush,
	.atomic_enable = rzg2l_du_crtc_atomic_enable,
	.atomic_disable = rzg2l_du_crtc_atomic_disable,
};

static void rzg2l_du_crtc_crc_init(struct rcar_du_crtc *rcrtc)
{
	const char **sources;
	unsigned int count;
	int i = -1;

	/* Reserve 1 for "auto" source. */
	count = rcrtc->vsp->num_planes + 1;

	sources = kmalloc_array(count, sizeof(*sources), GFP_KERNEL);
	if (!sources)
		return;

	sources[0] = kstrdup("auto", GFP_KERNEL);
	if (!sources[0])
		goto error;

	for (i = 0; i < rcrtc->vsp->num_planes; ++i) {
		struct drm_plane *plane = &rcrtc->vsp->planes[i].plane;
		char name[16];

		sprintf(name, "plane%u", plane->base.id);
		sources[i + 1] = kstrdup(name, GFP_KERNEL);
		if (!sources[i + 1])
			goto error;
	}

	rcrtc->sources = sources;
	rcrtc->sources_count = count;
	return;

error:
	while (i >= 0) {
		kfree(sources[i]);
		i--;
	}
	kfree(sources);
}

static void rzg2l_du_crtc_crc_cleanup(struct rcar_du_crtc *rcrtc)
{
	unsigned int i;

	if (!rcrtc->sources)
		return;

	for (i = 0; i < rcrtc->sources_count; i++)
		kfree(rcrtc->sources[i]);
	kfree(rcrtc->sources);

	rcrtc->sources = NULL;
	rcrtc->sources_count = 0;
}

static struct drm_crtc_state *
rzg2l_du_crtc_atomic_duplicate_state(struct drm_crtc *crtc)
{
	struct rcar_du_crtc_state *state;
	struct rcar_du_crtc_state *copy;

	if (WARN_ON(!crtc->state))
		return NULL;

	state = to_rcar_crtc_state(crtc->state);
	copy = kmemdup(state, sizeof(*state), GFP_KERNEL);
	if (!copy)
		return NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &copy->state);

	return &copy->state;
}

static void rzg2l_du_crtc_atomic_destroy_state(struct drm_crtc *crtc,
					       struct drm_crtc_state *state)
{
	__drm_atomic_helper_crtc_destroy_state(state);
	kfree(to_rcar_crtc_state(state));
}

static void rzg2l_du_crtc_cleanup(struct drm_crtc *crtc)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);

	rzg2l_du_crtc_crc_cleanup(rcrtc);

	return drm_crtc_cleanup(crtc);
}

static void rzg2l_du_crtc_reset(struct drm_crtc *crtc)
{
	struct rcar_du_crtc_state *state;

	if (crtc->state) {
		rzg2l_du_crtc_atomic_destroy_state(crtc, crtc->state);
		crtc->state = NULL;
	}

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return;

	state->crc.source = VSP1_DU_CRC_NONE;
	state->crc.index = 0;

	__drm_atomic_helper_crtc_reset(crtc, &state->state);
}

static int rzg2l_du_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);

	rcrtc->vblank_enable = true;

	return 0;
}

static void rzg2l_du_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);

	rcrtc->vblank_enable = false;
}

static int rzg2l_du_crtc_parse_crc_source(struct rcar_du_crtc *rcrtc,
					  const char *source_name,
					  enum vsp1_du_crc_source *source)
{
	unsigned int index;
	int ret;

	/*
	 * Parse the source name. Supported values are "plane%u" to compute the
	 * CRC on an input plane (%u is the plane ID), and "auto" to compute the
	 * CRC on the composer (VSP) output.
	 */

	if (!source_name) {
		*source = VSP1_DU_CRC_NONE;
		return 0;
	} else if (!strcmp(source_name, "auto")) {
		*source = VSP1_DU_CRC_OUTPUT;
		return 0;
	} else if (strstarts(source_name, "plane")) {
		unsigned int i;

		*source = VSP1_DU_CRC_PLANE;

		ret = kstrtouint(source_name + strlen("plane"), 10, &index);
		if (ret < 0)
			return ret;

		for (i = 0; i < rcrtc->vsp->num_planes; ++i) {
			if (index == rcrtc->vsp->planes[i].plane.base.id)
				return i;
		}
	}

	return -EINVAL;
}

static int rzg2l_du_crtc_verify_crc_source(struct drm_crtc *crtc,
					   const char *source_name,
					   size_t *values_cnt)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);
	enum vsp1_du_crc_source source;

	if (rzg2l_du_crtc_parse_crc_source(rcrtc, source_name, &source) < 0) {
		DRM_DEBUG_DRIVER("unknown source %s\n", source_name);
		return -EINVAL;
	}

	*values_cnt = 1;
	return 0;
}

static const char *const *
rzg2l_du_crtc_get_crc_sources(struct drm_crtc *crtc, size_t *count)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);

	*count = rcrtc->sources_count;
	return rcrtc->sources;
}

static int rzg2l_du_crtc_set_crc_source(struct drm_crtc *crtc,
					const char *source_name)
{
	struct rcar_du_crtc *rcrtc = to_rcar_crtc(crtc);
	struct drm_modeset_acquire_ctx ctx;
	struct drm_crtc_state *crtc_state;
	struct drm_atomic_state *state;
	enum vsp1_du_crc_source source;
	unsigned int index;
	int ret;

	ret = rzg2l_du_crtc_parse_crc_source(rcrtc, source_name, &source);
	if (ret < 0)
		return ret;

	index = ret;

	/* Perform an atomic commit to set the CRC source. */
	drm_modeset_acquire_init(&ctx, 0);

	state = drm_atomic_state_alloc(crtc->dev);
	if (!state) {
		ret = -ENOMEM;
		goto unlock;
	}

	state->acquire_ctx = &ctx;

retry:
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (!IS_ERR(crtc_state)) {
		struct rcar_du_crtc_state *rcrtc_state;

		rcrtc_state = to_rcar_crtc_state(crtc_state);
		rcrtc_state->crc.source = source;
		rcrtc_state->crc.index = index;

		ret = drm_atomic_commit(state);
	} else {
		ret = PTR_ERR(crtc_state);
	}

	if (ret == -EDEADLK) {
		drm_atomic_state_clear(state);
		drm_modeset_backoff(&ctx);
		goto retry;
	}

	drm_atomic_state_put(state);

unlock:
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	return ret;
}

static const struct drm_crtc_funcs crtc_funcs_rzg2l = {
	.reset = rzg2l_du_crtc_reset,
	.destroy = rzg2l_du_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = rzg2l_du_crtc_atomic_duplicate_state,
	.atomic_destroy_state = rzg2l_du_crtc_atomic_destroy_state,
	.enable_vblank = rzg2l_du_crtc_enable_vblank,
	.disable_vblank = rzg2l_du_crtc_disable_vblank,
	.set_crc_source = rzg2l_du_crtc_set_crc_source,
	.verify_crc_source = rzg2l_du_crtc_verify_crc_source,
	.get_crc_sources = rzg2l_du_crtc_get_crc_sources,
};

/* -----------------------------------------------------------------------------
 * Initialization
 */

int rzg2l_du_crtc_create(struct rcar_du_device *rcdu)
{
	struct rcar_du_crtc *rcrtc = &rcdu->crtcs[0];
	struct drm_crtc *crtc = &rcrtc->crtc;
	struct drm_plane *primary;
	int ret;

	rcrtc->rstc = devm_reset_control_get_shared(rcdu->dev, NULL);
	if (IS_ERR(rcrtc->rstc)) {
		dev_err(rcdu->dev, "can't get cpg reset\n");
		return PTR_ERR(rcrtc->rstc);
	}

	rcrtc->rzg2l_clocks.aclk = devm_clk_get(rcdu->dev, "aclk");
	if (IS_ERR(rcrtc->rzg2l_clocks.aclk)) {
		dev_err(rcdu->dev, "no axi clock for DU\n");
		return PTR_ERR(rcrtc->rzg2l_clocks.aclk);
	}

	rcrtc->rzg2l_clocks.pclk = devm_clk_get(rcdu->dev, "pclk");
	if (IS_ERR(rcrtc->rzg2l_clocks.pclk)) {
		dev_err(rcdu->dev, "no peripheral clock for DU\n");
		return PTR_ERR(rcrtc->rzg2l_clocks.pclk);
	}

	rcrtc->rzg2l_clocks.dclk = devm_clk_get(rcdu->dev, "vclk");
	if (IS_ERR(rcrtc->rzg2l_clocks.dclk)) {
		dev_err(rcdu->dev, "no video clock for DU\n");
		return PTR_ERR(rcrtc->rzg2l_clocks.dclk);
	}

	init_waitqueue_head(&rcrtc->flip_wait);
	init_waitqueue_head(&rcrtc->vblank_wait);
	spin_lock_init(&rcrtc->vblank_lock);

	rcrtc->dev = rcdu;
	rcrtc->index = 0;

	primary = &rcrtc->vsp->planes[rcrtc->vsp_pipe].plane;

	ret = drm_crtc_init_with_planes(&rcdu->ddev, crtc, primary, NULL,
					&crtc_funcs_rzg2l,
					NULL);
	if (ret < 0)
		return ret;

	drm_crtc_helper_add(crtc, &crtc_helper_funcs);

	rzg2l_du_crtc_crc_init(rcrtc);

	return 0;
}
