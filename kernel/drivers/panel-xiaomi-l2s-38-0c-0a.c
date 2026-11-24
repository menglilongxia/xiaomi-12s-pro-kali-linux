// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiaomi 12S Pro (unicorn) Samsung E5 38_0c_0a 1440x3200 120Hz DSC command-mode OLED panel
 * Derived directly from Unicorn's stock dtbo_a.img (qcom,mdss_dsi_l2s_38_0c_0a_dsc_cmd).
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define XIAOMI_L2S_MAX_BRIGHTNESS 2047

struct xiaomi_l2s {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct drm_connector *connector;
	struct gpio_desc *reset_gpio;
	struct regulator *vddio;
	struct regulator *vddd;
	struct regulator *vci;
};

static inline struct xiaomi_l2s *to_xiaomi_l2s(struct drm_panel *panel)
{
	return container_of(panel, struct xiaomi_l2s, panel);
}

/*
 * Vendor DSI link rate: 1.36 Gbit/s per lane across 4 lanes (clockrate 0x510ff400).
 * With RGB101010 (30 bpp) and DSC 8 bpp:
 *   compressed hdisplay = 1440 * 8 / 30 = 384 px
 *   target pclk = 1,360,000,000 * 4 / 30 = 181,333,333 Hz
 *   vtotal = 3200 + 24 + 8 + 24 = 3256
 */
#define XIAOMI_L2S_VTOTAL	(3200 + 24 + 8 + 24)
#define XIAOMI_L2S_PCLK		181333333
#define XIAOMI_L2S_HFP(fps)	(XIAOMI_L2S_PCLK / (XIAOMI_L2S_VTOTAL * (fps)) - 384 - 16 - 32)
#define XIAOMI_L2S_MODE(fps, preferred) {					\
	.clock = (1440 + XIAOMI_L2S_HFP(fps) + 16 + 32) *			\
		 XIAOMI_L2S_VTOTAL * (fps) / 1000,				\
	.hdisplay = 1440,							\
	.hsync_start = 1440 + XIAOMI_L2S_HFP(fps),				\
	.hsync_end = 1440 + XIAOMI_L2S_HFP(fps) + 16,				\
	.htotal = 1440 + XIAOMI_L2S_HFP(fps) + 16 + 32,				\
	.vdisplay = 3200,							\
	.vsync_start = 3200 + 24,						\
	.vsync_end = 3200 + 24 + 8,						\
	.vtotal = XIAOMI_L2S_VTOTAL,						\
	.width_mm = 70,								\
	.height_mm = 156,							\
	.type = DRM_MODE_TYPE_DRIVER | (preferred),				\
}

static const struct drm_display_mode xiaomi_l2s_modes[] = {
	XIAOMI_L2S_MODE(120, DRM_MODE_TYPE_PREFERRED),
	XIAOMI_L2S_MODE(60, 0),
};

static void xiaomi_l2s_reset(struct xiaomi_l2s *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(1000, 2000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
}

static int xiaomi_l2s_power_on(struct xiaomi_l2s *ctx)
{
	int ret;

	ret = regulator_enable(ctx->vddio);
	if (ret)
		return ret;
	usleep_range(1000, 2000);

	ret = regulator_enable(ctx->vddd);
	if (ret)
		goto err_vddio;
	usleep_range(1000, 2000);

	ret = regulator_enable(ctx->vci);
	if (ret)
		goto err_vddd;
	usleep_range(10000, 11000);

	return 0;

err_vddd:
	regulator_disable(ctx->vddd);
err_vddio:
	regulator_disable(ctx->vddio);
	return ret;
}

static void xiaomi_l2s_power_off(struct xiaomi_l2s *ctx)
{
	regulator_disable(ctx->vci);
	regulator_disable(ctx->vddd);
	regulator_disable(ctx->vddio);
}

static int xiaomi_l2s_on_120hz(struct xiaomi_l2s *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	static const u8 cmd_xiaomi_l2s_on_120hz_0[] = { 0x9e, 0x11, 0x00, 0x00, 0xab, 0x30, 0x80, 0x0c, 0x80, 0x05, 0xa0, 0x00, 0x19, 0x02, 0xd0, 0x02, 0xd0, 0x02, 0x00, 0x02, 0x68, 0x00, 0x20, 0x02, 0xbe, 0x00, 0x0a, 0x00, 0x0c, 0x04, 0x00, 0x03, 0x0d, 0x18, 0x00, 0x10, 0xf0, 0x07, 0x10, 0x20, 0x00, 0x06, 0x0f, 0x0f, 0x33, 0x0e, 0x1c, 0x2a, 0x38, 0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7b, 0x7d, 0x7e, 0x01, 0x02, 0x22, 0x00, 0x2a, 0x40, 0x2a, 0xbe, 0x3a, 0xfc, 0x3a, 0xfa, 0x3a, 0xf8, 0x3b, 0x38, 0x3b, 0x78, 0x3b, 0xb6, 0x4b, 0xf6, 0x4c, 0x34, 0x4c, 0x74, 0x5c, 0x74, 0x8c, 0xf4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, cmd_xiaomi_l2s_on_120hz_0, sizeof(cmd_xiaomi_l2s_on_120hz_0));
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x9d, 0x01);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x35, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2a, 0x00, 0x00, 0x05, 0x9f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2b, 0x00, 0x00, 0x0c, 0x7f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9, 0x41);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x06, 0xb9);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9, 0x0c, 0x9e, 0x0c, 0x9e, 0x00, 0x1d);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x08, 0xcb);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcb, 0x27);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x21, 0x82);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x10, 0xbd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x21, 0xbd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x07, 0x00, 0x0b, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x61, 0xbd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x1d);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x12, 0xbd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x60, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x16, 0xbd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x0d, 0x63);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x63, 0x08);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x0c, 0x63);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x63, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x53, 0x28);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x51, 0x03, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x47, 0xf4);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf4, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xed, 0x47, 0x05, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf1, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x22, 0xb9);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9, 0xb1, 0xa1);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x05, 0xf2);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf2, 0x52);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf1, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x02, 0x67, 0x1d);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x1d, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x52, 0x1f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x1f, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x53, 0x1f);
	static const u8 cmd_xiaomi_l2s_on_120hz_67[] = { 0x1f, 0x07, 0x01, 0x00, 0x02, 0xb9, 0x02, 0xba, 0x07, 0xa2, 0x07, 0xaa, 0x12, 0xc0, 0x12, 0xc1, 0x1a, 0xba, 0x1b, 0xc0, 0x1f, 0xe0, 0x00, 0x53, 0x00, 0xc3, 0x03, 0x43, 0x07, 0xa0, 0x07, 0xa1, 0x07, 0xa2, 0x07, 0xa6, 0x07, 0xa8, 0x07, 0xaa, 0x07, 0xac, 0x07, 0xae, 0x07, 0xb0, 0x07, 0xb2, 0x07, 0xb4, 0x07, 0xb6, 0x00, 0x03, 0x00, 0xa4, 0x00, 0xc0, 0x00, 0xd1, 0x01, 0x86, 0x03, 0xff, 0x07, 0xff, 0x30, 0x30, 0x35, 0x40, 0x35, 0x30, 0x40, 0x40, 0x35, 0x80, 0x80, 0x80, 0x0a, 0x0f, 0x14, 0x18, 0x1e, 0x3a, 0x78, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01, 0xb1 };
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, cmd_xiaomi_l2s_on_120hz_67, sizeof(cmd_xiaomi_l2s_on_120hz_67));
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x78, 0xbd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x23, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x14, 0xbd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);

	return dsi_ctx.accum_err;
}

static int xiaomi_l2s_on_60hz(struct xiaomi_l2s *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	static const u8 cmd_xiaomi_l2s_on_60hz_0[] = { 0x9e, 0x11, 0x00, 0x00, 0xab, 0x30, 0x80, 0x0c, 0x80, 0x05, 0xa0, 0x00, 0x19, 0x02, 0xd0, 0x02, 0xd0, 0x02, 0x00, 0x02, 0x68, 0x00, 0x20, 0x02, 0xbe, 0x00, 0x0a, 0x00, 0x0c, 0x04, 0x00, 0x03, 0x0d, 0x18, 0x00, 0x10, 0xf0, 0x07, 0x10, 0x20, 0x00, 0x06, 0x0f, 0x0f, 0x33, 0x0e, 0x1c, 0x2a, 0x38, 0x46, 0x54, 0x62, 0x69, 0x70, 0x77, 0x79, 0x7b, 0x7d, 0x7e, 0x01, 0x02, 0x22, 0x00, 0x2a, 0x40, 0x2a, 0xbe, 0x3a, 0xfc, 0x3a, 0xfa, 0x3a, 0xf8, 0x3b, 0x38, 0x3b, 0x78, 0x3b, 0xb6, 0x4b, 0xf6, 0x4c, 0x34, 0x4c, 0x74, 0x5c, 0x74, 0x8c, 0xf4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, cmd_xiaomi_l2s_on_60hz_0, sizeof(cmd_xiaomi_l2s_on_60hz_0));
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x9d, 0x01);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x35, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2a, 0x00, 0x00, 0x05, 0x9f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2b, 0x00, 0x00, 0x0c, 0x7f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x06, 0xb9);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x08, 0xcb);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcb, 0x27);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x21, 0x82);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x10, 0xbd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x21, 0xbd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x03, 0x00, 0x06, 0x00, 0x09, 0x00, 0x0c, 0x00, 0x0f, 0x00, 0x15, 0x00, 0x21, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x61, 0xbd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x04, 0x00, 0x08, 0x00, 0x0c, 0x00, 0x10, 0x00, 0x74);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x12, 0xbd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x16, 0xbd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x21);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x60, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x0d, 0x63);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x63, 0x08);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x0c, 0x63);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x63, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x53, 0x28);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x51, 0x03, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x47, 0xf4);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf4, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xed, 0x47, 0x05, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf1, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x22, 0xb9);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9, 0xb1, 0xa1);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x05, 0xf2);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf2, 0x52);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf1, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3, 0x0c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x02, 0x67, 0x1d);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x1d, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0x5a, 0x5a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x52, 0x1f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x1f, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x53, 0x1f);
	static const u8 cmd_xiaomi_l2s_on_60hz_68[] = { 0x1f, 0x07, 0x01, 0x00, 0x02, 0xb9, 0x02, 0xba, 0x07, 0xa2, 0x07, 0xaa, 0x12, 0xc0, 0x12, 0xc1, 0x1a, 0xba, 0x1b, 0xc0, 0x1f, 0xe0, 0x00, 0x53, 0x00, 0xc3, 0x03, 0x43, 0x07, 0xa0, 0x07, 0xa1, 0x07, 0xa2, 0x07, 0xa6, 0x07, 0xa8, 0x07, 0xaa, 0x07, 0xac, 0x07, 0xae, 0x07, 0xb0, 0x07, 0xb2, 0x07, 0xb4, 0x07, 0xb6, 0x00, 0x03, 0x00, 0xa4, 0x00, 0xc0, 0x00, 0xd1, 0x01, 0x86, 0x03, 0xff, 0x07, 0xff, 0x30, 0x30, 0x35, 0x40, 0x35, 0x30, 0x40, 0x40, 0x35, 0x80, 0x80, 0x80, 0x0a, 0x0f, 0x14, 0x18, 0x1e, 0x3a, 0x78, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01, 0xb1 };
	mipi_dsi_dcs_write_buffer_multi(&dsi_ctx, cmd_xiaomi_l2s_on_60hz_68, sizeof(cmd_xiaomi_l2s_on_60hz_68));
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x00, 0x78, 0xbd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf7, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf0, 0xa5, 0xa5);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int xiaomi_l2s_off(struct xiaomi_l2s *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 100);

	return dsi_ctx.accum_err;
}

static bool xiaomi_l2s_is_60hz(struct xiaomi_l2s *ctx)
{
	if (!ctx->connector || !ctx->connector->state || !ctx->connector->state->crtc)
		return false;
	return drm_mode_vrefresh(&ctx->connector->state->crtc->state->mode) <= 65;
}

static int xiaomi_l2s_prepare(struct drm_panel *panel)
{
	struct xiaomi_l2s *ctx = to_xiaomi_l2s(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = xiaomi_l2s_power_on(ctx);
	if (ret) {
		dev_err(dev, "failed to enable supplies: %d\n", ret);
		return ret;
	}

	xiaomi_l2s_reset(ctx);

	if (xiaomi_l2s_is_60hz(ctx))
		ret = xiaomi_l2s_on_60hz(ctx);
	else
		ret = xiaomi_l2s_on_120hz(ctx);

	if (ret < 0) {
		dev_err(dev, "failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		xiaomi_l2s_power_off(ctx);
		return ret;
	}

	msleep(20);
	return 0;
}

static int xiaomi_l2s_unprepare(struct drm_panel *panel)
{
	struct xiaomi_l2s *ctx = to_xiaomi_l2s(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = xiaomi_l2s_off(ctx);
	if (ret < 0)
		dev_err(dev, "failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	xiaomi_l2s_power_off(ctx);
	return 0;
}

static int xiaomi_l2s_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	struct xiaomi_l2s *ctx = to_xiaomi_l2s(panel);
	struct drm_display_mode *mode;
	int i;

	ctx->connector = connector;
	for (i = 0; i < ARRAY_SIZE(xiaomi_l2s_modes); i++) {
		mode = drm_mode_duplicate(connector->dev, &xiaomi_l2s_modes[i]);
		if (!mode)
			return -ENOMEM;
		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
	}
	connector->display_info.width_mm = 70;
	connector->display_info.height_mm = 156;
	return ARRAY_SIZE(xiaomi_l2s_modes);
}

static const struct drm_panel_funcs xiaomi_l2s_panel_funcs = {
	.prepare = xiaomi_l2s_prepare,
	.unprepare = xiaomi_l2s_unprepare,
	.get_modes = xiaomi_l2s_get_modes,
};

static int xiaomi_l2s_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return ret < 0 ? ret : 0;
}

static const struct backlight_ops xiaomi_l2s_bl_ops = {
	.update_status = xiaomi_l2s_bl_update_status,
};

static struct backlight_device *
xiaomi_l2s_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 1024,
		.max_brightness = XIAOMI_L2S_MAX_BRIGHTNESS,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &xiaomi_l2s_bl_ops, &props);
}

static int xiaomi_l2s_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct xiaomi_l2s *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct xiaomi_l2s, panel,
				   &xiaomi_l2s_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->vddio = devm_regulator_get(dev, "vddio");
	if (IS_ERR(ctx->vddio))
		return dev_err_probe(dev, PTR_ERR(ctx->vddio), "failed to get vddio\n");
	ctx->vddd = devm_regulator_get(dev, "vddd");
	if (IS_ERR(ctx->vddd))
		return dev_err_probe(dev, PTR_ERR(ctx->vddd), "failed to get vddd\n");
	ctx->vci = devm_regulator_get(dev, "vci");
	if (IS_ERR(ctx->vci))
		return dev_err_probe(dev, PTR_ERR(ctx->vci), "failed to get vci\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio), "failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB101010;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = xiaomi_l2s_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight), "failed to create backlight\n");

	ret = devm_drm_panel_add(dev, &ctx->panel);
	if (ret)
		return ret;

	dsi->dsc = &ctx->dsc;
	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;
	ctx->dsc.slice_height = 25;
	ctx->dsc.slice_width = 720;
	ctx->dsc.slice_count = 1440 / ctx->dsc.slice_width;
	ctx->dsc.bits_per_component = 10;
	ctx->dsc.bits_per_pixel = 128;
	ctx->dsc.block_pred_enable = 1;
	ctx->dsc.first_line_bpg_offset = 12;
	ctx->dsc.flatness_min_qp = 7;
	ctx->dsc.flatness_max_qp = 16;
	ctx->dsc.rc_model_size = 8192;
	ctx->dsc.rc_edge_factor = 6;
	ctx->dsc.rc_quant_incr_limit0 = 15;
	ctx->dsc.rc_quant_incr_limit1 = 15;
	ctx->dsc.rc_tgt_offset_high = 3;
	ctx->dsc.rc_tgt_offset_low = 3;
	ctx->dsc.initial_xmit_delay = 512;
	ctx->dsc.initial_offset = 6144;
	ctx->dsc.rc_buf_thresh[0] = 896;
	ctx->dsc.rc_buf_thresh[1] = 1792;
	ctx->dsc.rc_buf_thresh[2] = 2688;
	ctx->dsc.rc_buf_thresh[3] = 3584;
	ctx->dsc.rc_buf_thresh[4] = 4480;
	ctx->dsc.rc_buf_thresh[5] = 5376;
	ctx->dsc.rc_buf_thresh[6] = 6272;
	ctx->dsc.rc_buf_thresh[7] = 6720;
	ctx->dsc.rc_buf_thresh[8] = 7168;
	ctx->dsc.rc_buf_thresh[9] = 7616;
	ctx->dsc.rc_buf_thresh[10] = 7744;
	ctx->dsc.rc_buf_thresh[11] = 7872;
	ctx->dsc.rc_buf_thresh[12] = 8000;
	ctx->dsc.rc_buf_thresh[13] = 8064;
	ctx->dsc.rc_range_params[0] = (struct drm_dsc_rc_range_parameters){ .range_min_qp = 0, .range_max_qp = 4, .range_bpg_offset = 2 };
	ctx->dsc.rc_range_params[1] = (struct drm_dsc_rc_range_parameters){ .range_min_qp = 4, .range_max_qp = 8, .range_bpg_offset = 0 };
	ctx->dsc.rc_range_params[2] = (struct drm_dsc_rc_range_parameters){ .range_min_qp = 5, .range_max_qp = 9, .range_bpg_offset = 0 };
	ctx->dsc.rc_range_params[3] = (struct drm_dsc_rc_range_parameters){ .range_min_qp = 5, .range_max_qp = 10, .range_bpg_offset = 62 };
	ctx->dsc.rc_range_params[4] = (struct drm_dsc_rc_range_parameters){ .range_min_qp = 7, .range_max_qp = 11, .range_bpg_offset = 60 };
	ctx->dsc.rc_range_params[5] = (struct drm_dsc_rc_range_parameters){ .range_min_qp = 7, .range_max_qp = 11, .range_bpg_offset = 58 };
	ctx->dsc.rc_range_params[6] = (struct drm_dsc_rc_range_parameters){ .range_min_qp = 7, .range_max_qp = 11, .range_bpg_offset = 56 };
	ctx->dsc.rc_range_params[7] = (struct drm_dsc_rc_range_parameters){ .range_min_qp = 7, .range_max_qp = 12, .range_bpg_offset = 56 };
	ctx->dsc.rc_range_params[8] = (struct drm_dsc_rc_range_parameters){ .range_min_qp = 7, .range_max_qp = 13, .range_bpg_offset = 56 };
	ctx->dsc.rc_range_params[9] = (struct drm_dsc_rc_range_parameters){ .range_min_qp = 7, .range_max_qp = 14, .range_bpg_offset = 54 };
	ctx->dsc.rc_range_params[10] = (struct drm_dsc_rc_range_parameters){ .range_min_qp = 9, .range_max_qp = 15, .range_bpg_offset = 54 };
	ctx->dsc.rc_range_params[11] = (struct drm_dsc_rc_range_parameters){ .range_min_qp = 9, .range_max_qp = 16, .range_bpg_offset = 52 };
	ctx->dsc.rc_range_params[12] = (struct drm_dsc_rc_range_parameters){ .range_min_qp = 9, .range_max_qp = 17, .range_bpg_offset = 52 };
	ctx->dsc.rc_range_params[13] = (struct drm_dsc_rc_range_parameters){ .range_min_qp = 11, .range_max_qp = 17, .range_bpg_offset = 52 };
	ctx->dsc.rc_range_params[14] = (struct drm_dsc_rc_range_parameters){ .range_min_qp = 17, .range_max_qp = 19, .range_bpg_offset = 52 };

	return devm_mipi_dsi_attach(dev, dsi);
}

static const struct of_device_id xiaomi_l2s_of_match[] = {
	{ .compatible = "xiaomi,l2s-38-0c-0a" },
	{ }
};
MODULE_DEVICE_TABLE(of, xiaomi_l2s_of_match);

static struct mipi_dsi_driver xiaomi_l2s_driver = {
	.probe = xiaomi_l2s_probe,
	.driver = {
		.name = "panel-xiaomi-l2s-38-0c-0a",
		.of_match_table = xiaomi_l2s_of_match,
	},
};
module_mipi_dsi_driver(xiaomi_l2s_driver);

MODULE_DESCRIPTION("DRM driver for Xiaomi 12S Pro (unicorn) Samsung E5 38_0c_0a 1440x3200 120Hz OLED");
MODULE_LICENSE("GPL");
