// SPDX-License-Identifier: GPL-2.0
/*
 * Fitipower EK79007 MIPI-DSI panel driver.
 *
 * 1024x600, two data lanes, as fitted to the ESP32-P4-Function-EV-Board's
 * 7" HMI sub-board.
 *
 * The register sequence is the vendor one, taken from Espressif's
 * esp_lcd_ek79007 component (v2.0.2). It is short because the panel does
 * almost all of its own setup: a pad-control write selecting the lane count,
 * seven vendor registers, and sleep-out.
 *
 * Two things here are ESP32-P4 host requirements rather than panel
 * requirements, and both are load-bearing:
 *
 *   MIPI_DSI_MODE_LPM  - the P4 cannot send DCS in high-speed mode. IDF
 *                        confirms this by setting all twelve packet types to
 *                        MIPI_DSI_LL_TRANS_SPEED_LP. Without the flag every
 *                        command in this file goes out in HS and the panel
 *                        never sees it.
 *   PHSYNC | PVSYNC    - IDF drives the DPI sync polarities active high. Ask
 *                        for active low and the host's video state machine
 *                        never accepts a frame and sits in LP-Stop.
 *
 * The lane bit rate is not negotiated: the P4 D-PHY glue reports a fixed
 * 1 Gbps, which is what this panel wants at 2 lanes.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

/* Selects how many data lanes the panel expects. */
#define EK79007_PAD_CONTROL	0xb2
#define EK79007_DSI_2_LANE	0x10
#define EK79007_DSI_4_LANE	0x00

struct ek79007 {
	struct drm_panel	panel;
	struct mipi_dsi_device	*dsi;
	struct regulator	*supply;
	struct gpio_desc	*reset_gpio;
};

static inline struct ek79007 *to_ek79007(struct drm_panel *panel)
{
	return container_of(panel, struct ek79007, panel);
}

/*
 * 48000000 / (10 + 120 + 120 + 1024) / (1 + 20 + 10 + 600) = 60 Hz,
 * matching the EK79007_1024_600_PANEL_60HZ_CONFIG timings in IDF.
 */
static const struct drm_display_mode ek79007_mode = {
	.clock		= 48000,
	.hdisplay	= 1024,
	.hsync_start	= 1024 + 120,
	.hsync_end	= 1024 + 120 + 10,
	.htotal		= 1024 + 120 + 10 + 120,
	.vdisplay	= 600,
	.vsync_start	= 600 + 10,
	.vsync_end	= 600 + 10 + 1,
	.vtotal		= 600 + 10 + 1 + 20,
	.flags		= DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
	.width_mm	= 154,
	.height_mm	= 86,
};

static int ek79007_prepare(struct drm_panel *panel)
{
	struct ek79007 *ctx = to_ek79007(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	int ret;

	ret = regulator_enable(ctx->supply);
	if (ret)
		return ret;

	/*
	 * 10 ms asserted, 20 ms to settle. The vendor driver falls back to a
	 * DCS soft reset when no reset line is wired; this board has one, so
	 * only the hardware path is implemented.
	 */
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(10);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(20);

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, EK79007_PAD_CONTROL,
				     EK79007_DSI_2_LANE);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x80, 0x8b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x81, 0x78);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x82, 0x84);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x83, 0x88);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x84, 0xa8);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x85, 0xe3);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x86, 0x88);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	if (dsi_ctx.accum_err) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_disable(ctx->supply);
		return dsi_ctx.accum_err;
	}

	return 0;
}

static int ek79007_unprepare(struct drm_panel *panel)
{
	struct ek79007 *ctx = to_ek79007(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_disable(ctx->supply);

	return 0;
}

static int ek79007_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &ek79007_mode);
	if (!mode)
		return -ENOMEM;

	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static const struct drm_panel_funcs ek79007_panel_funcs = {
	.prepare	= ek79007_prepare,
	.unprepare	= ek79007_unprepare,
	.get_modes	= ek79007_get_modes,
};

static int ek79007_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ek79007 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(ctx->supply))
		return dev_err_probe(dev, PTR_ERR(ctx->supply),
				     "failed to get power regulator\n");

	/* Held in reset until prepare(); DT carries the active-low flag. */
	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset GPIO\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 2;
	dsi->format = MIPI_DSI_FMT_RGB565;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM | MIPI_DSI_CLOCK_NON_CONTINUOUS;

	drm_panel_init(&ctx->panel, dev, &ek79007_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);
	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach to DSI host\n");
	}

	return 0;
}

static void ek79007_remove(struct mipi_dsi_device *dsi)
{
	struct ek79007 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id ek79007_of_match[] = {
	{ .compatible = "fitipower,ek79007" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ek79007_of_match);

static struct mipi_dsi_driver ek79007_driver = {
	.probe	= ek79007_probe,
	.remove	= ek79007_remove,
	.driver	= {
		.name		= "panel-ek79007",
		.of_match_table	= ek79007_of_match,
	},
};
module_mipi_dsi_driver(ek79007_driver);

MODULE_DESCRIPTION("Fitipower EK79007 MIPI-DSI panel driver");
MODULE_LICENSE("GPL");
