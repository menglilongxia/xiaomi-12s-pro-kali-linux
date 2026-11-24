// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX707 50MP 1/1.28" Quad-Bayer image sensor driver (Xiaomi 12S Pro)
 * Loads module-specific init/mode register tables from a CAMREGS1 firmware file
 * and streams RAW10 over 3 MIPI C-PHY trios.
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

#define IMX707_REG_CHIP_ID		CCI_REG16(0x0016)
#define IMX707_CHIP_ID_MASK		0xffff
#define IMX707_CHIP_ID_VAL		0x0707

#define IMX707_REG_MODE_SELECT		CCI_REG8(0x0100)
#define IMX707_MODE_STANDBY		0x00
#define IMX707_MODE_STREAMING		0x01

#define IMX707_REG_GROUP_HOLD		CCI_REG8(0x0104)
#define IMX707_REG_EXPOSURE		CCI_REG16(0x0202)
#define IMX707_REG_ANALOGUE_GAIN	CCI_REG16(0x0204)
#define IMX707_REG_VTS			CCI_REG16(0x0340)
#define IMX707_REG_TEST_PATTERN		CCI_REG16(0x0600)

#define IMX707_MCLK_FREQ		19200000
#define IMX707_MEDIA_BUS_FMT		MEDIA_BUS_FMT_SRGGB10_1X10

static const char * const imx707_supply_names[] = {
	"vddio",
	"vdda2",
	"vdda",
	"vddd",
	"vaf",
};

struct imx707_mode {
	u32 width;
	u32 height;
	u32 hts;
	u32 vts_def;
	u32 link_freq_idx;
	u64 pixel_rate;
	const char *table_name;
};

static const s64 imx707_link_freq_menu[] = {
	857600000ULL, /* mode1/mode2/mode5: 1715.2 Msps C-PHY (V4L2 link_freq = sym_rate / 2) */
	737600000ULL, /* mode0 (50MP): 1475.2 Msps C-PHY (V4L2 link_freq = sym_rate / 2) */
};

static const struct imx707_mode imx707_modes[] = {
	{
		.width = 4096,
		.height = 3072,
		.hts = 13536,
		.vts_def = 6450,
		.link_freq_idx = 0,
		.pixel_rate = 1638400000ULL,
		.table_name = "mode1",
	},
	{
		.width = 4096,
		.height = 2304,
		.hts = 13536,
		.vts_def = 6450,
		.link_freq_idx = 0,
		.pixel_rate = 1638400000ULL,
		.table_name = "mode2",
	},
	{
		.width = 1920,
		.height = 1080,
		.hts = 9424,
		.vts_def = 2316,
		.link_freq_idx = 0,
		.pixel_rate = 1638400000ULL,
		.table_name = "mode5",
	},
	{
		.width = 8192,
		.height = 6144,
		.hts = 19008,
		.vts_def = 6482,
		.link_freq_idx = 1,
		.pixel_rate = 2156800000ULL,
		.table_name = "mode0",
	},
};

struct camregs_entry {
	__le16 addr;
	__le16 value;
	u8 width;
	u8 pad;
	__le16 delay_us;
} __packed;

struct imx707 {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;
	struct clk *xclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx707_supply_names)];
	const char *fw_name;
	const struct firmware *fw;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *gain;

	const struct imx707_mode *cur_mode;
};

static inline struct imx707 *to_imx707(struct v4l2_subdev *sd)
{
	return container_of(sd, struct imx707, sd);
}

static int imx707_write_camregs_table(struct imx707 *imx, const char *target)
{
	const u8 *data, *end;
	u32 n_tables, t;
	int ret = 0;

	if (!imx->fw) {
		ret = request_firmware(&imx->fw, imx->fw_name, imx->dev);
		if (ret) {
			dev_err(imx->dev, "failed to load %s: %d\n", imx->fw_name, ret);
			return ret;
		}
	}

	data = imx->fw->data;
	end = data + imx->fw->size;
	if (imx->fw->size < 12 || memcmp(data, "CAMREGS1", 8) != 0) {
		dev_err(imx->dev, "invalid camregs header in %s\n", imx->fw_name);
		return -EINVAL;
	}

	n_tables = get_unaligned_le32(data + 8);
	data += 12;

	for (t = 0; t < n_tables && data + 20 <= end; t++) {
		char name[17];
		u32 n_entries, i;
		size_t bytes;

		memcpy(name, data, 16);
		name[16] = '\0';
		n_entries = get_unaligned_le32(data + 16);
		data += 20;
		bytes = (size_t)n_entries * sizeof(struct camregs_entry);
		if (data + bytes > end)
			return -EINVAL;

		if (strcmp(name, target) == 0) {
			const struct camregs_entry *e = (const struct camregs_entry *)data;

			for (i = 0; i < n_entries; i++) {
				u16 addr = le16_to_cpu(e[i].addr);
				u16 val = le16_to_cpu(e[i].value);
				u16 dly = le16_to_cpu(e[i].delay_us);
				u32 reg = (e[i].width == 2) ? CCI_REG16(addr) : CCI_REG8(addr);

				ret = cci_write(imx->regmap, reg, val, NULL);
				if (ret) {
					dev_err(imx->dev, "%s: write 0x%04x=0x%x failed (%d)\n",
						target, addr, val, ret);
					return ret;
				}
				if (dly)
					usleep_range(dly, dly + 200);
			}
			return 0;
		}
		data += bytes;
	}

	dev_err(imx->dev, "table '%s' not found in %s\n", target, imx->fw_name);
	return -ENOENT;
}

static int imx707_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx707 *imx = to_imx707(sd);
	int ret;

	gpiod_set_value_cansleep(imx->reset_gpio, 1); /* assert reset (active-low -> low) */
	ret = regulator_bulk_enable(ARRAY_SIZE(imx707_supply_names), imx->supplies);
	if (ret)
		return ret;

	usleep_range(2000, 3000);
	ret = clk_prepare_enable(imx->xclk);
	if (ret) {
		regulator_bulk_disable(ARRAY_SIZE(imx707_supply_names), imx->supplies);
		return ret;
	}

	usleep_range(2000, 3000);
	gpiod_set_value_cansleep(imx->reset_gpio, 0); /* release reset -> high */
	usleep_range(8000, 10000);
	return 0;
}

static int imx707_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx707 *imx = to_imx707(sd);

	gpiod_set_value_cansleep(imx->reset_gpio, 1);
	clk_disable_unprepare(imx->xclk);
	regulator_bulk_disable(ARRAY_SIZE(imx707_supply_names), imx->supplies);
	return 0;
}

static int imx707_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx707 *imx = container_of(ctrl->handler, struct imx707, ctrl_handler);
	int ret = 0;

	if (!pm_runtime_get_if_in_use(imx->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret = cci_write(imx->regmap, IMX707_REG_EXPOSURE, ctrl->val, NULL);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(imx->regmap, IMX707_REG_ANALOGUE_GAIN, ctrl->val, NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(imx->regmap, IMX707_REG_VTS,
				imx->cur_mode->height + ctrl->val, NULL);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = cci_write(imx->regmap, IMX707_REG_TEST_PATTERN, ctrl->val, NULL);
		break;
	default:
		break;
	}

	pm_runtime_put(imx->dev);
	return ret;
}

static const struct v4l2_ctrl_ops imx707_ctrl_ops = {
	.s_ctrl = imx707_set_ctrl,
};

static const char * const imx707_test_patterns[] = {
	"Disabled",
	"Solid Color",
	"100% Color Bars",
	"Fade to Grey Color Bars",
	"PN9",
};

static int imx707_init_controls(struct imx707 *imx)
{
	struct v4l2_ctrl_handler *hdl = &imx->ctrl_handler;
	struct v4l2_fwnode_device_properties props;
	const struct imx707_mode *m = imx->cur_mode;
	int ret;

	v4l2_ctrl_handler_init(hdl, 10);

	imx->link_freq = v4l2_ctrl_new_int_menu(hdl, &imx707_ctrl_ops,
						V4L2_CID_LINK_FREQ,
						ARRAY_SIZE(imx707_link_freq_menu) - 1,
						m->link_freq_idx,
						imx707_link_freq_menu);
	if (imx->link_freq)
		imx->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx->pixel_rate = v4l2_ctrl_new_std(hdl, &imx707_ctrl_ops,
					    V4L2_CID_PIXEL_RATE,
					    100000000, 3000000000ULL, 1,
					    m->pixel_rate);
	imx->hblank = v4l2_ctrl_new_std(hdl, &imx707_ctrl_ops,
					V4L2_CID_HBLANK,
					m->hts - m->width, m->hts - m->width, 1,
					m->hts - m->width);
	if (imx->hblank)
		imx->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx->vblank = v4l2_ctrl_new_std(hdl, &imx707_ctrl_ops,
					V4L2_CID_VBLANK,
					128, 0xffff - m->height, 1,
					m->vts_def - m->height);
	imx->exposure = v4l2_ctrl_new_std(hdl, &imx707_ctrl_ops,
					  V4L2_CID_EXPOSURE,
					  8, m->vts_def - 48, 1, 3200);
	imx->gain = v4l2_ctrl_new_std(hdl, &imx707_ctrl_ops,
				      V4L2_CID_ANALOGUE_GAIN,
				      0, 960, 1, 512);
	v4l2_ctrl_new_std_menu_items(hdl, &imx707_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx707_test_patterns) - 1,
				     0, 0, imx707_test_patterns);

	ret = v4l2_fwnode_device_parse(imx->dev, &props);
	if (!ret)
		v4l2_ctrl_new_fwnode_properties(hdl, &imx707_ctrl_ops, &props);

	if (hdl->error) {
		ret = hdl->error;
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	imx->sd.ctrl_handler = hdl;
	return 0;
}

static int imx707_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;
	code->code = IMX707_MEDIA_BUS_FMT;
	return 0;
}

static int imx707_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(imx707_modes) || fse->code != IMX707_MEDIA_BUS_FMT)
		return -EINVAL;
	fse->min_width = imx707_modes[fse->index].width;
	fse->max_width = imx707_modes[fse->index].width;
	fse->min_height = imx707_modes[fse->index].height;
	fse->max_height = imx707_modes[fse->index].height;
	return 0;
}

static int imx707_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct imx707 *imx = to_imx707(sd);
	const struct imx707_mode *m;
	struct v4l2_mbus_framefmt *f;

	m = v4l2_find_nearest_size(imx707_modes, ARRAY_SIZE(imx707_modes),
				   width, height,
				   fmt->format.width, fmt->format.height);

	f = v4l2_subdev_state_get_format(state, fmt->pad);
	f->width = m->width;
	f->height = m->height;
	f->code = IMX707_MEDIA_BUS_FMT;
	f->field = V4L2_FIELD_NONE;
	f->colorspace = V4L2_COLORSPACE_RAW;
	f->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->format = *f;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		imx->cur_mode = m;
		__v4l2_ctrl_s_ctrl(imx->link_freq, m->link_freq_idx);
		__v4l2_ctrl_s_ctrl_int64(imx->pixel_rate, m->pixel_rate);
		__v4l2_ctrl_modify_range(imx->hblank, m->hts - m->width,
					 m->hts - m->width, 1, m->hts - m->width);
		__v4l2_ctrl_modify_range(imx->vblank, 128, 0xffff - m->height,
					 1, m->vts_def - m->height);
	}
	return 0;
}

static int imx707_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_mbus_framefmt *f = v4l2_subdev_state_get_format(state, 0);

	f->width = imx707_modes[0].width;
	f->height = imx707_modes[0].height;
	f->code = IMX707_MEDIA_BUS_FMT;
	f->field = V4L2_FIELD_NONE;
	f->colorspace = V4L2_COLORSPACE_RAW;
	f->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	return 0;
}

static int imx707_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 u32 pad, u64 streams_mask)
{
	struct imx707 *imx = to_imx707(sd);
	int ret;

	ret = pm_runtime_resume_and_get(imx->dev);
	if (ret < 0)
		return ret;

	ret = imx707_write_camregs_table(imx, "init");
	if (ret)
		goto err;

	ret = imx707_write_camregs_table(imx, imx->cur_mode->table_name);
	if (ret)
		goto err;

	ret = __v4l2_ctrl_handler_setup(&imx->ctrl_handler);
	if (ret)
		goto err;

	ret = cci_write(imx->regmap, IMX707_REG_MODE_SELECT, IMX707_MODE_STREAMING, NULL);
	if (ret)
		goto err;

	return 0;
err:
	pm_runtime_put(imx->dev);
	return ret;
}

static int imx707_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  u32 pad, u64 streams_mask)
{
	struct imx707 *imx = to_imx707(sd);

	cci_write(imx->regmap, IMX707_REG_MODE_SELECT, IMX707_MODE_STANDBY, NULL);
	pm_runtime_put(imx->dev);
	return 0;
}

static const struct v4l2_subdev_video_ops imx707_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops imx707_pad_ops = {
	.enum_mbus_code = imx707_enum_mbus_code,
	.enum_frame_size = imx707_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = imx707_set_fmt,
	.enable_streams = imx707_enable_streams,
	.disable_streams = imx707_disable_streams,
};

static const struct v4l2_subdev_ops imx707_subdev_ops = {
	.video = &imx707_video_ops,
	.pad = &imx707_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx707_internal_ops = {
	.init_state = imx707_init_state,
};

static int imx707_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct imx707 *imx;
	u64 chip_id = 0;
	int i, ret;

	imx = devm_kzalloc(dev, sizeof(*imx), GFP_KERNEL);
	if (!imx)
		return -ENOMEM;

	imx->dev = dev;
	imx->cur_mode = &imx707_modes[0];

	if (device_property_read_string(dev, "firmware-name", &imx->fw_name))
		imx->fw_name = "qcom/sm8475/xiaomi/unicorn/imx707-wide.camregs";

	imx->xclk = devm_clk_get(dev, NULL);
	if (IS_ERR(imx->xclk))
		return dev_err_probe(dev, PTR_ERR(imx->xclk), "failed to get xclk\n");

	clk_set_rate(imx->xclk, IMX707_MCLK_FREQ);

	imx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(imx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(imx->reset_gpio), "failed to get reset gpio\n");

	for (i = 0; i < ARRAY_SIZE(imx707_supply_names); i++)
		imx->supplies[i].supply = imx707_supply_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(imx707_supply_names), imx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	imx->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx->regmap))
		return PTR_ERR(imx->regmap);

	v4l2_i2c_subdev_init(&imx->sd, client, &imx707_subdev_ops);
	imx->sd.internal_ops = &imx707_internal_ops;
	imx->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	imx->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = imx707_power_on(dev);
	if (ret)
		return ret;

	ret = cci_read(imx->regmap, IMX707_REG_CHIP_ID, &chip_id, NULL);
	if (ret) {
		dev_err(dev, "failed to read chip ID: %d\n", ret);
		goto err_power;
	}
	if ((chip_id & IMX707_CHIP_ID_MASK) != IMX707_CHIP_ID_VAL) {
		dev_err(dev, "unexpected chip ID 0x%04llx (expected 0x%04x)\n",
			chip_id, IMX707_CHIP_ID_VAL);
		ret = -ENODEV;
		goto err_power;
	}
	dev_info(dev, "Sony IMX707 detected (chip_id=0x%04llx)\n", chip_id);

	ret = imx707_init_controls(imx);
	if (ret)
		goto err_power;

	ret = media_entity_pads_init(&imx->sd.entity, 1, &imx->pad);
	if (ret)
		goto err_ctrl;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	ret = v4l2_subdev_init_finalize(&imx->sd);
	if (ret)
		goto err_pm;

	ret = v4l2_async_register_subdev_sensor(&imx->sd);
	if (ret)
		goto err_subdev;

	return 0;

err_subdev:
	v4l2_subdev_cleanup(&imx->sd);
err_pm:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	media_entity_cleanup(&imx->sd.entity);
err_ctrl:
	v4l2_ctrl_handler_free(&imx->ctrl_handler);
err_power:
	imx707_power_off(dev);
	return ret;
}

static void imx707_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx707 *imx = to_imx707(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&imx->ctrl_handler);
	release_firmware(imx->fw);
	pm_runtime_disable(imx->dev);
	if (!pm_runtime_status_suspended(imx->dev))
		imx707_power_off(imx->dev);
	pm_runtime_set_suspended(imx->dev);
}

static DEFINE_RUNTIME_DEV_PM_OPS(imx707_pm_ops, imx707_power_off, imx707_power_on, NULL);

static const struct of_device_id imx707_of_match[] = {
	{ .compatible = "sony,imx707" },
	{ }
};
MODULE_DEVICE_TABLE(of, imx707_of_match);

static struct i2c_driver imx707_driver = {
	.driver = {
		.name = "imx707",
		.of_match_table = imx707_of_match,
		.pm = pm_ptr(&imx707_pm_ops),
	},
	.probe = imx707_probe,
	.remove = imx707_remove,
};
module_i2c_driver(imx707_driver);

MODULE_DESCRIPTION("Sony IMX707 50MP C-PHY Camera Sensor Driver");
MODULE_LICENSE("GPL");
