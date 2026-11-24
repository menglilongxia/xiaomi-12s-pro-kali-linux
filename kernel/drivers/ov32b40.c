// SPDX-License-Identifier: GPL-2.0
/*
 * OmniVision OV32B40 32MP front camera sensor driver (Xiaomi 12S Pro)
 * Loads module-specific init/mode register tables from a CAMREGS1 firmware file
 * and streams RAW10 over 4 MIPI D-PHY lanes.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define OV32B40_REG_CHIP_ID		CCI_REG16(0x300b)
#define OV32B40_CHIP_ID_VAL		0x3242

#define OV32B40_REG_MODE_SELECT		CCI_REG8(0x0100)
#define OV32B40_MODE_STANDBY		0x00
#define OV32B40_MODE_STREAMING		0x01

#define OV32B40_REG_GROUP_HOLD		CCI_REG8(0x3208)
#define OV32B40_REG_EXPOSURE		CCI_REG24(0x3500)
#define OV32B40_REG_ANALOGUE_GAIN	CCI_REG16(0x3508)
#define OV32B40_REG_VTS			CCI_REG16(0x380e)
#define OV32B40_REG_TEST_PATTERN	CCI_REG8(0x5040)

#define OV32B40_MCLK_FREQ		19200000
#define OV32B40_MEDIA_BUS_FMT		MEDIA_BUS_FMT_SBGGR10_1X10

#define OV32B40_NATIVE_WIDTH		6528
#define OV32B40_NATIVE_HEIGHT		4896

static const char * const ov32b40_supply_names[] = {
	"vdda",
	"vddio",
	"vddd",
};

struct ov32b40_mode {
	u32 width;
	u32 height;
	u32 hts;
	u32 vts_def;
	u32 link_freq_idx;
	u64 pixel_rate;
	const char *table_name;
};

static const s64 ov32b40_link_freq_menu[] = {
	556000000ULL, /* 4-lane MIPI D-PHY ~1112 Mbps/lane */
};

static const struct ov32b40_mode ov32b40_modes[] = {
	{
		.width = 3264,
		.height = 2448,
		.hts = 5760,
		.vts_def = 2666,
		.link_freq_idx = 0,
		.pixel_rate = 444800000ULL,
		.table_name = "mode5",
	},
	{
		.width = 3264,
		.height = 1836,
		.hts = 5760,
		.vts_def = 2666,
		.link_freq_idx = 0,
		.pixel_rate = 444800000ULL,
		.table_name = "mode3",
	},
	{
		.width = 1632,
		.height = 918,
		.hts = 2880,
		.vts_def = 2666,
		.link_freq_idx = 0,
		.pixel_rate = 444800000ULL,
		.table_name = "mode4",
	},
	{
		.width = 6528,
		.height = 4896,
		.hts = 11520,
		.vts_def = 5332,
		.link_freq_idx = 0,
		.pixel_rate = 444800000ULL,
		.table_name = "mode0",
	},
};

static const char * const ov32b40_test_pattern_menu[] = {
	"Disabled",
	"Color Bars",
};

struct ov32b40 {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;
	struct clk *xclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(ov32b40_supply_names)];

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *gain;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;

	const struct ov32b40_mode *cur_mode;
	const struct firmware *fw;
	const char *fw_name;
};

static inline struct ov32b40 *to_ov32b40(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov32b40, sd);
}

static int ov32b40_write_camregs_table(struct ov32b40 *ov, const char *target)
{
	const u8 *p, *end;
	u32 n_tables, t;

	if (!ov->fw) {
		int ret = request_firmware(&ov->fw, ov->fw_name, ov->dev);

		if (ret) {
			dev_err(ov->dev, "failed to load %s: %d\n", ov->fw_name, ret);
			return ret;
		}
	}

	if (ov->fw->size < 12 || memcmp(ov->fw->data, "CAMREGS1", 8) != 0)
		return -EINVAL;

	p = ov->fw->data + 8;
	end = ov->fw->data + ov->fw->size;
	n_tables = get_unaligned_le32(p);
	p += 4;

	for (t = 0; t < n_tables; t++) {
		char name[17];
		u32 n_entries, i;

		if (p + 20 > end)
			return -EINVAL;
		memcpy(name, p, 16);
		name[16] = '\0';
		n_entries = get_unaligned_le32(p + 16);
		p += 20;

		if (p + (size_t)n_entries * 8 > end)
			return -EINVAL;

		if (strcmp(name, target) == 0) {
			for (i = 0; i < n_entries; i++) {
				u16 addr = get_unaligned_le16(p);
				u16 val = get_unaligned_le16(p + 2);
				u8 width = p[4];
				u16 delay_us = get_unaligned_le16(p + 6);
				u32 reg = (width == 2) ? CCI_REG16(addr) : CCI_REG8(addr);
				int ret;

				p += 8;
				if (addr == 0x0100 && val == 0x01)
					continue;
				ret = cci_write(ov->regmap, reg, val, NULL);
				if (ret) {
					dev_err(ov->dev, "table %s write 0x%04x=0x%x failed: %d\n",
						target, addr, val, ret);
					return ret;
				}
				if (delay_us)
					fsleep(delay_us);
			}
			return 0;
		}
		p += (size_t)n_entries * 8;
	}

	dev_err(ov->dev, "table '%s' not found in %s\n", target, ov->fw_name);
	return -ENOENT;
}

static int ov32b40_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov32b40 *ov = to_ov32b40(sd);
	int ret;

	gpiod_set_value_cansleep(ov->reset_gpio, 1);

	/* Unicorn powerSetting: VANA (1ms) -> VIO (1ms) -> VDIG (1ms) -> MCLK (1ms) -> RESET high (5ms) */
	ret = regulator_enable(ov->supplies[0].consumer);
	if (ret)
		return ret;
	usleep_range(1000, 1500);

	ret = regulator_enable(ov->supplies[1].consumer);
	if (ret)
		goto err_vdda;
	usleep_range(1000, 1500);

	ret = regulator_enable(ov->supplies[2].consumer);
	if (ret)
		goto err_vddio;
	usleep_range(1000, 1500);

	ret = clk_prepare_enable(ov->xclk);
	if (ret)
		goto err_vddd;
	usleep_range(1000, 1500);

	gpiod_set_value_cansleep(ov->reset_gpio, 0);
	usleep_range(5000, 7000);
	return 0;

err_vddd:
	regulator_disable(ov->supplies[2].consumer);
err_vddio:
	regulator_disable(ov->supplies[1].consumer);
err_vdda:
	regulator_disable(ov->supplies[0].consumer);
	return ret;
}

static int ov32b40_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct ov32b40 *ov = to_ov32b40(sd);

	gpiod_set_value_cansleep(ov->reset_gpio, 1);
	clk_disable_unprepare(ov->xclk);
	usleep_range(1000, 1500);
	regulator_disable(ov->supplies[2].consumer);
	regulator_disable(ov->supplies[1].consumer);
	regulator_disable(ov->supplies[0].consumer);
	return 0;
}

static int ov32b40_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov32b40 *ov = container_of(ctrl->handler, struct ov32b40, ctrl_handler);
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK) {
		u32 exp_max = ov->cur_mode->height + ctrl->val - 32;

		__v4l2_ctrl_modify_range(ov->exposure, 8, exp_max, 1,
					 min_t(u32, ov->exposure->val, exp_max));
	}

	if (!pm_runtime_get_if_in_use(ov->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = cci_write(ov->regmap, OV32B40_REG_EXPOSURE, ctrl->val, NULL);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(ov->regmap, OV32B40_REG_ANALOGUE_GAIN, ctrl->val, NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(ov->regmap, OV32B40_REG_VTS,
				ov->cur_mode->height + ctrl->val, NULL);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = cci_write(ov->regmap, OV32B40_REG_TEST_PATTERN,
				ctrl->val ? 0x80 : 0x00, NULL);
		break;
	default:
		break;
	}

	pm_runtime_put(ov->dev);
	return ret;
}

static const struct v4l2_ctrl_ops ov32b40_ctrl_ops = {
	.s_ctrl = ov32b40_set_ctrl,
};

static int ov32b40_init_controls(struct ov32b40 *ov)
{
	struct v4l2_ctrl_handler *hdl = &ov->ctrl_handler;
	struct v4l2_fwnode_device_properties props;
	const struct ov32b40_mode *mode = ov->cur_mode;
	u32 vblank_def = mode->vts_def - mode->height;
	u32 hblank = mode->hts - mode->width;
	int ret;

	v4l2_ctrl_handler_init(hdl, 10);

	ov->link_freq = v4l2_ctrl_new_int_menu(hdl, &ov32b40_ctrl_ops,
					       V4L2_CID_LINK_FREQ,
					       ARRAY_SIZE(ov32b40_link_freq_menu) - 1,
					       mode->link_freq_idx,
					       ov32b40_link_freq_menu);
	if (ov->link_freq)
		ov->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ov->pixel_rate = v4l2_ctrl_new_std(hdl, &ov32b40_ctrl_ops,
					   V4L2_CID_PIXEL_RATE,
					   100000000ULL, 1000000000ULL, 1,
					   mode->pixel_rate);
	if (ov->pixel_rate)
		ov->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ov->hblank = v4l2_ctrl_new_std(hdl, &ov32b40_ctrl_ops,
				       V4L2_CID_HBLANK, hblank, hblank, 1, hblank);
	if (ov->hblank)
		ov->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ov->vblank = v4l2_ctrl_new_std(hdl, &ov32b40_ctrl_ops,
				       V4L2_CID_VBLANK, 64, 0xffff - mode->height,
				       1, vblank_def);

	ov->exposure = v4l2_ctrl_new_std(hdl, &ov32b40_ctrl_ops,
					 V4L2_CID_EXPOSURE, 8, mode->vts_def - 32,
					 1, 1024);

	ov->gain = v4l2_ctrl_new_std(hdl, &ov32b40_ctrl_ops,
				     V4L2_CID_ANALOGUE_GAIN, 0x0100, 0x0f80,
				     1, 0x0400);

	v4l2_ctrl_new_std_menu_items(hdl, &ov32b40_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(ov32b40_test_pattern_menu) - 1,
				     0, 0, ov32b40_test_pattern_menu);

	if (hdl->error) {
		ret = hdl->error;
		goto err_free;
	}

	ret = v4l2_fwnode_device_parse(ov->dev, &props);
	if (ret)
		goto err_free;

	ret = v4l2_ctrl_new_fwnode_properties(hdl, &ov32b40_ctrl_ops, &props);
	if (ret)
		goto err_free;

	ov->sd.ctrl_handler = hdl;
	return 0;

err_free:
	v4l2_ctrl_handler_free(hdl);
	return ret;
}

static void ov32b40_fill_fmt(const struct ov32b40_mode *mode,
			     struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = OV32B40_MEDIA_BUS_FMT;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
}

static int ov32b40_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *fmt = v4l2_subdev_state_get_format(state, 0);

	ov32b40_fill_fmt(&ov32b40_modes[0], fmt);
	return 0;
}

static int ov32b40_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;
	code->code = OV32B40_MEDIA_BUS_FMT;
	return 0;
}

static int ov32b40_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(ov32b40_modes))
		return -EINVAL;
	if (fse->code != OV32B40_MEDIA_BUS_FMT)
		return -EINVAL;

	fse->min_width = ov32b40_modes[fse->index].width;
	fse->max_width = ov32b40_modes[fse->index].width;
	fse->min_height = ov32b40_modes[fse->index].height;
	fse->max_height = ov32b40_modes[fse->index].height;
	return 0;
}

static int ov32b40_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *format)
{
	struct ov32b40 *ov = to_ov32b40(sd);
	const struct ov32b40_mode *mode;
	struct v4l2_mbus_framefmt *fmt;

	mode = v4l2_find_nearest_size(ov32b40_modes, ARRAY_SIZE(ov32b40_modes),
				      width, height,
				      format->format.width, format->format.height);
	ov32b40_fill_fmt(mode, &format->format);

	fmt = v4l2_subdev_state_get_format(state, format->pad);
	*fmt = format->format;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		u32 vblank_def = mode->vts_def - mode->height;
		u32 hblank = mode->hts - mode->width;

		ov->cur_mode = mode;
		__v4l2_ctrl_s_ctrl(ov->link_freq, mode->link_freq_idx);
		__v4l2_ctrl_s_ctrl_int64(ov->pixel_rate, mode->pixel_rate);
		__v4l2_ctrl_modify_range(ov->hblank, hblank, hblank, 1, hblank);
		__v4l2_ctrl_modify_range(ov->vblank, 64, 0xffff - mode->height,
					 1, vblank_def);
		__v4l2_ctrl_s_ctrl(ov->vblank, vblank_def);
	}

	return 0;
}

static int ov32b40_get_selection(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = OV32B40_NATIVE_WIDTH;
		sel->r.height = OV32B40_NATIVE_HEIGHT;
		return 0;
	default:
		return -EINVAL;
	}
}

static int ov32b40_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct ov32b40 *ov = to_ov32b40(sd);
	int ret;

	if (enable) {
		ret = pm_runtime_resume_and_get(ov->dev);
		if (ret < 0)
			return ret;

		ret = ov32b40_write_camregs_table(ov, "init");
		if (ret)
			goto err_rpm;

		ret = ov32b40_write_camregs_table(ov, ov->cur_mode->table_name);
		if (ret)
			goto err_rpm;

		ret = __v4l2_ctrl_handler_setup(&ov->ctrl_handler);
		if (ret)
			goto err_rpm;

		ret = cci_write(ov->regmap, OV32B40_REG_MODE_SELECT,
				OV32B40_MODE_STREAMING, NULL);
		if (ret)
			goto err_rpm;
	} else {
		cci_write(ov->regmap, OV32B40_REG_MODE_SELECT,
			  OV32B40_MODE_STANDBY, NULL);
		pm_runtime_put(ov->dev);
	}

	return 0;

err_rpm:
	pm_runtime_put(ov->dev);
	return ret;
}

static const struct v4l2_subdev_video_ops ov32b40_video_ops = {
	.s_stream = ov32b40_set_stream,
};

static const struct v4l2_subdev_pad_ops ov32b40_pad_ops = {
	.enum_mbus_code = ov32b40_enum_mbus_code,
	.enum_frame_size = ov32b40_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = ov32b40_set_fmt,
	.get_selection = ov32b40_get_selection,
};

static const struct v4l2_subdev_ops ov32b40_subdev_ops = {
	.video = &ov32b40_video_ops,
	.pad = &ov32b40_pad_ops,
};

static const struct v4l2_subdev_internal_ops ov32b40_internal_ops = {
	.init_state = ov32b40_init_state,
};

static int ov32b40_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ov32b40 *ov;
	u64 chip_id = 0;
	int i, ret;

	ov = devm_kzalloc(dev, sizeof(*ov), GFP_KERNEL);
	if (!ov)
		return -ENOMEM;

	ov->dev = dev;
	ov->cur_mode = &ov32b40_modes[0];

	if (device_property_read_string(dev, "firmware-name", &ov->fw_name))
		ov->fw_name = "qcom/sm8475/xiaomi/unicorn/ov32b40-front.camregs";

	ov->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(ov->xclk))
		return dev_err_probe(dev, PTR_ERR(ov->xclk), "failed to get xclk\n");

	clk_set_rate(ov->xclk, OV32B40_MCLK_FREQ);

	ov->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ov->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ov->reset_gpio), "failed to get reset gpio\n");

	for (i = 0; i < ARRAY_SIZE(ov32b40_supply_names); i++)
		ov->supplies[i].supply = ov32b40_supply_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ov32b40_supply_names), ov->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	ov->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(ov->regmap))
		return PTR_ERR(ov->regmap);

	v4l2_i2c_subdev_init(&ov->sd, client, &ov32b40_subdev_ops);
	ov->sd.internal_ops = &ov32b40_internal_ops;
	ov->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	ov->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ov->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = ov32b40_power_on(dev);
	if (ret)
		return ret;

	ret = cci_read(ov->regmap, OV32B40_REG_CHIP_ID, &chip_id, NULL);
	if (ret) {
		dev_err(dev, "failed to read chip ID: %d\n", ret);
		goto err_power;
	}
	if (chip_id != OV32B40_CHIP_ID_VAL) {
		dev_err(dev, "unexpected chip ID 0x%04llx (expected 0x%04x)\n",
			chip_id, OV32B40_CHIP_ID_VAL);
		ret = -ENODEV;
		goto err_power;
	}
	dev_info(dev, "OmniVision OV32B40 detected (chip_id=0x%04llx)\n", chip_id);

	ret = ov32b40_init_controls(ov);
	if (ret)
		goto err_power;

	ret = media_entity_pads_init(&ov->sd.entity, 1, &ov->pad);
	if (ret)
		goto err_ctrl;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	ret = v4l2_subdev_init_finalize(&ov->sd);
	if (ret)
		goto err_pm;

	ret = v4l2_async_register_subdev_sensor(&ov->sd);
	if (ret)
		goto err_subdev;

	return 0;

err_subdev:
	v4l2_subdev_cleanup(&ov->sd);
err_pm:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	media_entity_cleanup(&ov->sd.entity);
err_ctrl:
	v4l2_ctrl_handler_free(&ov->ctrl_handler);
err_power:
	ov32b40_power_off(dev);
	return ret;
}

static void ov32b40_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov32b40 *ov = to_ov32b40(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&ov->ctrl_handler);
	release_firmware(ov->fw);
	pm_runtime_disable(ov->dev);
	if (!pm_runtime_status_suspended(ov->dev))
		ov32b40_power_off(ov->dev);
	pm_runtime_set_suspended(ov->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(ov32b40_pm_ops, ov32b40_power_off, ov32b40_power_on, NULL);

static const struct of_device_id ov32b40_of_match[] = {
	{ .compatible = "ovti,ov32b40" },
	{ }
};
MODULE_DEVICE_TABLE(of, ov32b40_of_match);

static struct i2c_driver ov32b40_driver = {
	.driver = {
		.name = "ov32b40",
		.of_match_table = ov32b40_of_match,
		.pm = pm_ptr(&ov32b40_pm_ops),
	},
	.probe = ov32b40_probe,
	.remove = ov32b40_remove,
};
module_i2c_driver(ov32b40_driver);

MODULE_DESCRIPTION("OmniVision OV32B40 32MP front camera sensor driver");
MODULE_LICENSE("GPL");
