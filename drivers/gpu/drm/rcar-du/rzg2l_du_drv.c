// SPDX-License-Identifier: GPL-2.0+
/*
 * RZ/G2L DU DRM driver
 *
 * Copyright (C) 2022 Renesas Electronics Corporation
 *
 * Based on rcar_du_drv.c
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <linux/wait.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_probe_helper.h>

#include "rzg2l_du_drv.h"
#include "rzg2l_du_kms.h"

/* -----------------------------------------------------------------------------
 * Device Information
 */

static const struct rcar_du_device_info rzg2l_du_r9a07g044_info = {
	.gen = 3,
	.features = RCAR_DU_FEATURE_CRTC_IRQ
		  | RCAR_DU_FEATURE_CRTC_CLOCK
		  | RCAR_DU_FEATURE_VSP1_SOURCE,
	.channels_mask = BIT(0),
	.routes = {
		[RCAR_DU_OUTPUT_DPAD0] = {
			.possible_crtcs = BIT(0),
			.port = 0,
		},
		[RCAR_DU_OUTPUT_DSI0] = {
			.possible_crtcs = BIT(0),
			.port = 1,
		},
	},
	.num_rpf = 2,
};

static const struct of_device_id rzg2l_du_of_table[] = {
	{ .compatible = "renesas,r9a07g044-du", .data = &rzg2l_du_r9a07g044_info },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, rzg2l_du_of_table);

const char *rzg2l_du_output_name(enum rcar_du_output output)
{
	static const char * const names[] = {
		[RCAR_DU_OUTPUT_DPAD0] = "DPAD0",
		[RCAR_DU_OUTPUT_DPAD1] = "DPAD1",
		[RCAR_DU_OUTPUT_DSI0] = "DSI0",
		[RCAR_DU_OUTPUT_DSI1] = "DSI1",
		[RCAR_DU_OUTPUT_HDMI0] = "HDMI0",
		[RCAR_DU_OUTPUT_HDMI1] = "HDMI1",
		[RCAR_DU_OUTPUT_LVDS0] = "LVDS0",
		[RCAR_DU_OUTPUT_LVDS1] = "LVDS1",
		[RCAR_DU_OUTPUT_TCON] = "TCON",
	};

	if (output != RCAR_DU_OUTPUT_DPAD0 && output != RCAR_DU_OUTPUT_DSI0)
		return "NOT SUPPORTED";

	if (output >= ARRAY_SIZE(names) || !names[output])
		return "UNKNOWN";

	return names[output];
}

/* -----------------------------------------------------------------------------
 * DRM operations
 */

DEFINE_DRM_GEM_DMA_FOPS(rzg2l_du_fops);

static const struct drm_driver rzg2l_du_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.dumb_create		= rcar_du_dumb_create,
	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle,
	.gem_prime_import_sg_table = rcar_du_gem_prime_import_sg_table,
	.gem_prime_mmap		= drm_gem_prime_mmap,
	.fops			= &rzg2l_du_fops,
	.name			= "rzg2l-du",
	.desc			= "Renesas RZ/G2L DU",
	.date			= "20220305",
	.major			= 1,
	.minor			= 0,
};

/* -----------------------------------------------------------------------------
 * Platform driver
 */

static int rzg2l_du_remove(struct platform_device *pdev)
{
	struct rcar_du_device *rcdu = platform_get_drvdata(pdev);
	struct drm_device *ddev = &rcdu->ddev;

	drm_dev_unregister(ddev);
	drm_atomic_helper_shutdown(ddev);

	drm_kms_helper_poll_fini(ddev);

	return 0;
}

static void rzg2l_du_shutdown(struct platform_device *pdev)
{
	struct rcar_du_device *rcdu = platform_get_drvdata(pdev);

	drm_atomic_helper_shutdown(&rcdu->ddev);
}

static int rzg2l_du_probe(struct platform_device *pdev)
{
	struct rcar_du_device *rcdu;
	int ret;

	if (drm_firmware_drivers_only())
		return -ENODEV;

	/* Allocate and initialize the RZ/G2L device structure. */
	rcdu = devm_drm_dev_alloc(&pdev->dev, &rzg2l_du_driver,
				  struct rcar_du_device, ddev);
	if (IS_ERR(rcdu))
		return PTR_ERR(rcdu);

	rcdu->dev = &pdev->dev;
	rcdu->info = of_device_get_match_data(rcdu->dev);

	platform_set_drvdata(pdev, rcdu);

	/* I/O resources */
	rcdu->mmio = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rcdu->mmio))
		return PTR_ERR(rcdu->mmio);

	/*
	 * When sourcing frames from a VSP the DU doesn't perform any memory
	 * access so set the DMA coherent mask to 40 bits to accept all buffers.
	 */
	ret = dma_coerce_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
	if (ret)
		return ret;

	/* DRM/KMS objects */
	ret = rzg2l_du_modeset_init(rcdu);
	if (ret < 0) {
		if (ret != -EPROBE_DEFER)
			dev_err(&pdev->dev,
				"failed to initialize DRM/KMS (%d)\n", ret);
		goto error;
	}

	/*
	 * Register the DRM device with the core and the connectors with
	 * sysfs.
	 */
	ret = drm_dev_register(&rcdu->ddev, 0);
	if (ret)
		goto error;

	DRM_INFO("Device %s probed\n", dev_name(&pdev->dev));

	drm_fbdev_generic_setup(&rcdu->ddev, 32);

	return 0;

error:
	drm_kms_helper_poll_fini(&rcdu->ddev);
	return ret;
}

static struct platform_driver rzg2l_du_platform_driver = {
	.probe		= rzg2l_du_probe,
	.remove		= rzg2l_du_remove,
	.shutdown	= rzg2l_du_shutdown,
	.driver		= {
		.name	= "rzg2l-du",
		.of_match_table = rzg2l_du_of_table,
	},
};

module_platform_driver(rzg2l_du_platform_driver);

MODULE_AUTHOR("Biju Das <biju.das.jz@bp.renesas.com>");
MODULE_DESCRIPTION("Renesas RZ/G2L DU DRM Driver");
MODULE_LICENSE("GPL");
