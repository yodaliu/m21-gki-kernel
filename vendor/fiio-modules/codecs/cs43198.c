// SPDX-License-Identifier: GPL-2.0
/*
 * Cirrus Logic CS43198 DAC, driven through the FiiO dac core ("dac_dai module").
 * Up to four chips share one snd_dac_dai; the register map follows the
 * upstream cs43130 driver.
 */

#define pr_fmt(fmt) "[CS43198]: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <fiio/dac_dai.h>
#include <fiio/fiio_device.h>
#include <fiio/fiio_i2c.h>

struct cs43198_priv {
	struct snd_dac_dai *dac_dai;
	struct mutex clk_mutex;
	int clk_req;
	bool pll_bypass;
	unsigned int mclk;
	unsigned int mclk_int;
};

static struct cs43198_priv *g_cs43198;

static const struct reg_default cs43198_reg_defaults[] = {
	{ 0x010006, 0x06 },
	{ 0x01000b, 0x01 },
	{ 0x01000c, 0x00 },
	{ 0x01000d, 0x03 },
	{ 0x020000, 0xfe },
	{ 0x020052, 0x04 },
	{ 0x030001, 0x00 },
	{ 0x030002, 0x00 },
	{ 0x030003, 0x00 },
	{ 0x030004, 0x00 },
	{ 0x030005, 0x40 },
	{ 0x030008, 0x10 },
	{ 0x03000a, 0x80 },
	{ 0x03001b, 0x03 },
	{ 0x040002, 0x02 },
	{ 0x040003, 0x02 },
	{ 0x040004, 0x00 },
	{ 0x040010, 0x01 },
	{ 0x040011, 0x00 },
	{ 0x040012, 0x08 },
	{ 0x040013, 0x00 },
	{ 0x040014, 0x1f },
	{ 0x040015, 0x00 },
	{ 0x040016, 0x3f },
	{ 0x040017, 0x00 },
	{ 0x040018, 0x0c },
	{ 0x040019, 0x0a },
	{ 0x040020, 0x01 },
	{ 0x040021, 0x00 },
	{ 0x040022, 0x02 },
	{ 0x040023, 0x00 },
	{ 0x040024, 0x1f },
	{ 0x040025, 0x00 },
	{ 0x040026, 0x3f },
	{ 0x040027, 0x00 },
	{ 0x040028, 0x0c },
	{ 0x040029, 0x0a },
	{ 0x050000, 0x00 },
	{ 0x050001, 0x00 },
	{ 0x05000a, 0x06 },
	{ 0x05000b, 0x0e },
	{ 0x060000, 0x00 },
	{ 0x060001, 0x00 },
	{ 0x06000a, 0x06 },
	{ 0x06000b, 0x0e },
	{ 0x070000, 0x78 },
	{ 0x070001, 0x78 },
	{ 0x070002, 0xa8 },
	{ 0x070003, 0x00 },
	{ 0x070004, 0x02 },
	{ 0x070005, 0x00 },
	{ 0x070006, 0x40 },
	{ 0x080000, 0x30 },
	{ 0x090000, 0x02 },
	{ 0x090001, 0x78 },
	{ 0x090002, 0x78 },
	{ 0x090003, 0xa8 },
	{ 0x090004, 0x00 },
	{ 0x0b0000, 0x1e },
	{ 0x0d0000, 0x04 },
	{ 0x0e0000, 0x00 },
	{ 0x0e0003, 0x00 },
	{ 0x0e0004, 0x00 },
	{ 0x0f0010, 0xff },
	{ 0x0f0011, 0xff },
	{ 0x0f0012, 0xff },
	{ 0x0f0013, 0xff },
	{ 0x0f0014, 0xff },
};

static bool cs43198_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x0f0000 ... 0x0f0004:
	case 0x0e000d ... 0x0e000e:
	case 0x0e0010 ... 0x0e0011:
		return true;
	default:
		return false;
	}
}

static bool cs43198_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x010000 ... 0x010006:
	case 0x01000b ... 0x01000d:
	case 0x020000:
	case 0x020052:
	case 0x030001 ... 0x030005:
	case 0x030008:
	case 0x03000a:
	case 0x03001b:
	case 0x040002 ... 0x040004:
	case 0x040010 ... 0x040019:
	case 0x040020 ... 0x040029:
	case 0x050000 ... 0x050001:
	case 0x05000a ... 0x05000b:
	case 0x060000 ... 0x060001:
	case 0x06000a ... 0x06000b:
	case 0x070000 ... 0x070006:
	case 0x080000:
	case 0x090000 ... 0x090004:
	case 0x0b0000:
	case 0x0d0000 ... 0x0d0001:
	case 0x0e0000:
	case 0x0e0003 ... 0x0e0004:
	case 0x0e000d ... 0x0e000e:
	case 0x0e0010 ... 0x0e0011:
	case 0x0e001a:
	case 0x0f0000 ... 0x0f0004:
	case 0x0f0010 ... 0x0f0014:
		return true;
	default:
		return false;
	}
}

static bool cs43198_precious_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x0f0000 ... 0x0f0004:
		return true;
	default:
		return false;
	}
}

static void cs43198_set_volume_db(struct snd_dac_dai *dac_dai, unsigned int lvol_db,
				  unsigned int rvol_db)
{
	if (!dac_dai->on)
		return;

	pr_err("%s, lvolume: 0x%02x, rvolume: 0x%02x, is_dsd = %d\n",
	       __func__, lvol_db, rvol_db, dac_dai->is_dsd);

	dac_dual_regmap_update_bits(dac_dai, 0x080000, 0x01, dac_dai->gain_spe == 1 ? 0x01 : 0x00);

	if (!dac_dai->is_dsd) {
		if (dac_dai->channel_mode == SPECIAL_MODE_2) {
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, 0x090002, lvol_db);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, 0x090002, lvol_db);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, 0x090001, rvol_db);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, 0x090001, rvol_db);
		} else if (dac_dai->channel_mode == SPECIAL_MODE_3) {
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, 0x090002, lvol_db);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, 0x090001, lvol_db);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, 0x090002, lvol_db);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, 0x090001, lvol_db);
			if (dac_dai->regmap_3) {
				fiio_regmap_write(dac_dai->name, dac_dai->regmap_3, 0x090002, rvol_db);
				fiio_regmap_write(dac_dai->name, dac_dai->regmap_3, 0x090001, rvol_db);
			}
			if (dac_dai->regmap_4) {
				fiio_regmap_write(dac_dai->name, dac_dai->regmap_4, 0x090002, rvol_db);
				fiio_regmap_write(dac_dai->name, dac_dai->regmap_4, 0x090001, rvol_db);
			}
		}
	} else {
		if (dac_dai->channel_mode == SPECIAL_MODE_2) {
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, 0x070001, lvol_db);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, 0x070001, lvol_db);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, 0x070000, rvol_db);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, 0x070000, rvol_db);
		} else if (dac_dai->channel_mode == SPECIAL_MODE_3) {
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, 0x070001, lvol_db);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, 0x070000, lvol_db);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, 0x070001, lvol_db);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, 0x070000, lvol_db);
			if (dac_dai->regmap_3) {
				fiio_regmap_write(dac_dai->name, dac_dai->regmap_3, 0x070001, rvol_db);
				fiio_regmap_write(dac_dai->name, dac_dai->regmap_3, 0x070000, rvol_db);
			}
			if (dac_dai->regmap_4) {
				fiio_regmap_write(dac_dai->name, dac_dai->regmap_4, 0x070001, rvol_db);
				fiio_regmap_write(dac_dai->name, dac_dai->regmap_4, 0x070000, rvol_db);
			}
		}
	}
}

static void cs43198_reg_read(struct snd_dac_dai *dac_dai)
{
	unsigned int reg;
	unsigned int value_l = 0, value_r = 0;

	pr_err("======= read reg =======\n");

	for (reg = 0x010000; reg <= 0x0f0014; reg++) {
		if (!cs43198_readable_register(dac_dai->dev, reg))
			continue;

		fiio_regmap_read(dac_dai->name, dac_dai->regmap_l, reg, &value_l);
		fiio_regmap_read(dac_dai->name, dac_dai->regmap_r, reg, &value_r);
		if (dac_dai->channel_mode == SPECIAL_MODE_2) {
			fiio_regmap_read(dac_dai->name, dac_dai->regmap_r, reg, &value_r);
		} else if (dac_dai->channel_mode == SPECIAL_MODE_3) {
			if (dac_dai->regmap_3)
				fiio_regmap_read(dac_dai->name, dac_dai->regmap_3, reg, &value_r);
		}
		pr_err("read reg 0x%02x, value_l = 0x%02x ,value_r = 0x%02x\n", reg, value_l, value_r);
	}

	pr_err("======= read reg =======\n");
}

static void cs43198_sw_mute(struct snd_dac_dai *dac_dai, int mute)
{
	int value;

	(void)value;
	pr_err("%s enter mute : %s\n", __func__, mute == 1 ? "ON" : "OFF");

	if (!dac_dai->on)
		return;

	if (mute) {
		dac_dual_regmap_write(dac_dai, 0x090003, 0xaf);
		dac_dual_regmap_write(dac_dai, 0x070002, 0x8f);
	} else {
		if (dac_dai->is_dsd)
			dac_dual_regmap_write(dac_dai, 0x070002, 0x8c);
		else
			dac_dual_regmap_write(dac_dai, 0x090003, 0xac);
	}
}

static void cs43198_set_thd_c2(struct snd_dac_dai *dac_dai)
{
	pr_err("%s enter\n", __func__);
}

static void cs43198_set_lpf(struct snd_dac_dai *dac_dai)
{
	int value;

	if (!dac_dai->on)
		return;

	pr_err("[%s] lpf:%d\n", __func__, dac_dai->lpf_setting);

	if (dac_dai->is_dsd)
		return;

	dac_dai->nos_status = false;
	switch (dac_dai->lpf_setting) {
	case 0:
		value = 0x02;
		break;
	case 1:
		value = 0x82;
		break;
	case 2:
		value = 0x42;
		break;
	case 3:
		value = 0xc2;
		break;
	case 4:
		value = 0x06;
		break;
	case 5:
		value = 0x22;
		dac_dai->nos_status = true;
		break;
	default:
		pr_err("don't support this filt:%d...\n", dac_dai->lpf_setting);
		return;
	}
	dac_dual_regmap_write(dac_dai, 0x090000, value);
}

static uint8_t cs43198_calculation_rate_TO_ASP_register(struct snd_dac_dai *dac_dai)
{
	switch (dac_dai->sample_rate) {
	case 32000:
		return 0x00;
	case 44100:
		return 0x01;
	case 48000:
		return 0x02;
	case 88200:
		return 0x03;
	case 96000:
		return 0x04;
	case 176400:
		return 0x05;
	case 192000:
		return 0x06;
	case 352800:
		return 0x07;
	case 384000:
		return 0x08;
	case 705600:
	case 768000:
		return 0x00;
	default:
		pr_err("%s error rate...\n", __func__);
		return 0x00;
	}
}

static int cs43198_calculation_rate_TO_XSP_register(struct snd_dac_dai *dac_dai)
{
	int ret = 0;

	switch (dac_dai->sample_rate) {
	case 176400:
		ret = 0x04;
		break;
	case 352800:
		ret = 0x08;
		break;
	default:
		ret = 0x00;
		break;
	}

	pr_err("%s sample_rate:%d...\n", __func__, dac_dai->sample_rate);

	return ret;
}

static void cs43198_set_mono_mode(struct snd_dac_dai *dac_dai)
{
	dac_dual_regmap_write(dac_dai, 0x180009, 0x2f);
	dac_dual_regmap_write(dac_dai, 0x18000a, 0xb4);
	dac_dual_regmap_write(dac_dai, 0x18000b, 0x25);
	dac_dual_regmap_write(dac_dai, 0x18000c, 0x80);
	dac_dual_regmap_write(dac_dai, 0x18000f, 0x09);
	dac_dual_regmap_write(dac_dai, 0x180010, 0xfb);
	dac_dual_regmap_write(dac_dai, 0x180019, 0x2f);
	dac_dual_regmap_write(dac_dai, 0x18001a, 0xb4);
	dac_dual_regmap_write(dac_dai, 0x18001b, 0x25);
	dac_dual_regmap_write(dac_dai, 0x18001c, 0x80);
	dac_dual_regmap_write(dac_dai, 0x18001f, 0x93);
	dac_dual_regmap_write(dac_dai, 0x180020, 0xfd);
	dac_dual_regmap_write(dac_dai, 0x180029, 0x2f);
	dac_dual_regmap_write(dac_dai, 0x18002a, 0xb4);
	dac_dual_regmap_write(dac_dai, 0x18002b, 0x25);
	dac_dual_regmap_write(dac_dai, 0x18002c, 0x80);
	dac_dual_regmap_write(dac_dai, 0x18002f, 0x7c);
	dac_dual_regmap_write(dac_dai, 0x180030, 0xfd);
	dac_dual_regmap_write(dac_dai, 0x180039, 0x2f);
	dac_dual_regmap_write(dac_dai, 0x18003a, 0xb4);
	dac_dual_regmap_write(dac_dai, 0x18003b, 0x25);
	dac_dual_regmap_write(dac_dai, 0x18003c, 0x80);
	dac_dual_regmap_write(dac_dai, 0x18003f, 0x41);
	dac_dual_regmap_write(dac_dai, 0x180040, 0xfe);
}

/* keep __LINE__ in the logs as in stock */
#line 518
static void cs43198_config_dac(struct snd_dac_dai *dac_dai)
{
	int value = 0;

	pr_err("[%s %d] enter\n", __func__, __LINE__);

	if (dac_dai->is_dsd) {
		pr_err("[%s %d] dsd\n", __func__, __LINE__);

		if (dac_dai->soft_mute) {
			dac_dual_regmap_write(dac_dai, 0x070002, 0x8f);
			dac_dual_regmap_write(dac_dai, 0x090003, 0xaf);
		} else {
			dac_dual_regmap_write(dac_dai, 0x070002, 0x8c);
		}

		dac_dual_regmap_write(dac_dai, 0x020000, 0xfe);
		dac_dual_regmap_write(dac_dai, 0x020052, 0x04);

		fiio_regmap_read(dac_dai->name, dac_dai->regmap_l, 0x0f0000, &value);
		fiio_regmap_read(dac_dai->name, dac_dai->regmap_r, 0x0f0000, &value);
		if (dac_dai->regmap_3)
			fiio_regmap_read(dac_dai->name, dac_dai->regmap_3, 0x0f0000, &value);
		if (dac_dai->regmap_4)
			fiio_regmap_read(dac_dai->name, dac_dai->regmap_4, 0x0f0000, &value);

		dac_dual_regmap_write(dac_dai, 0x0f0010, 0xe1);
		dac_dual_regmap_write(dac_dai, 0x0b0000, 0x06);
		dac_dual_regmap_write(dac_dai, 0x080000, 0x30);
		dac_dual_regmap_write(dac_dai, 0x040002, 0x03);
		dac_dual_regmap_write(dac_dai, 0x030008, 0x08);
		dac_dual_regmap_write(dac_dai, 0x030002, 0x00);
		dac_dual_regmap_write(dac_dai, 0x030003, 0xf7);
		dac_dual_regmap_write(dac_dai, 0x030004, 0x06);
		dac_dual_regmap_write(dac_dai, 0x030005, 0x44);
		dac_dual_regmap_write(dac_dai, 0x03001b, 0x01);
		dac_dual_regmap_write(dac_dai, 0x03000a, 0x8b);
		dac_dual_regmap_write(dac_dai, 0x0f0010, 0xe1);
		dac_dual_regmap_write(dac_dai, 0x030001, 0x01);
		dac_dual_regmap_write(dac_dai, 0x070003, 0x00);

		value = cs43198_calculation_rate_TO_XSP_register(dac_dai);
		dac_dual_regmap_write(dac_dai, 0x070004, value | 0x13);

		if (dac_dai->channel_mode == SPECIAL_MODE_3) {
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, 0x070006, 0xc3);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, 0x070006, 0xcf);
			if (dac_dai->regmap_3)
				fiio_regmap_write(dac_dai->name, dac_dai->regmap_3, 0x070006, 0xc1);
			if (dac_dai->regmap_4)
				fiio_regmap_write(dac_dai->name, dac_dai->regmap_4, 0x070006, 0xcd);
		} else if (dac_dai->channel_mode == SPECIAL_MODE_2) {
			dac_dual_regmap_write(dac_dai, 0x06000a, 0x07);
			dac_dual_regmap_write(dac_dai, 0x06000b, 0x0f);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, 0x070006, 0xc2);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, 0x070006, 0xce);
		}

		dac_dual_regmap_write(dac_dai, 0x020000, 0x0a);
		fiio_regmap_read(dac_dai->name, dac_dai->regmap_l, 0x0f0000, &value);
		fiio_regmap_read(dac_dai->name, dac_dai->regmap_r, 0x0f0000, &value);
		fiio_regmap_read(dac_dai->name, dac_dai->regmap_l, 0x0f0001, &value);
		fiio_regmap_read(dac_dai->name, dac_dai->regmap_r, 0x0f0001, &value);
		dac_dual_regmap_write(dac_dai, 0x0f0011, 0xf8);
		fiio_regmap_read(dac_dai->name, dac_dai->regmap_l, 0x0f0000, &value);
		fiio_regmap_read(dac_dai->name, dac_dai->regmap_r, 0x0f0000, &value);
		dac_dual_regmap_write(dac_dai, 0x010006, 0x01);
	} else {
#line 604
		pr_err("[%s %d] pcm\n", __func__, __LINE__);

		if (dac_dai->soft_mute) {
			dac_dual_regmap_write(dac_dai, 0x070002, 0x8f);
			dac_dual_regmap_write(dac_dai, 0x090003, 0xaf);
		} else {
			dac_dual_regmap_write(dac_dai, 0x090003, 0xac);
		}

		dac_dual_regmap_write(dac_dai, 0x020000, 0xfe);
		dac_dual_regmap_write(dac_dai, 0x020052, 0x04);

		fiio_regmap_read(dac_dai->name, dac_dai->regmap_l, 0x0f0000, &value);
		fiio_regmap_read(dac_dai->name, dac_dai->regmap_r, 0x0f0000, &value);
		if (dac_dai->regmap_3)
			fiio_regmap_read(dac_dai->name, dac_dai->regmap_3, 0x0f0000, &value);
		if (dac_dai->regmap_4)
			fiio_regmap_read(dac_dai->name, dac_dai->regmap_4, 0x0f0000, &value);

		dac_dual_regmap_write(dac_dai, 0x0f0010, 0xe7);
		dac_dual_regmap_write(dac_dai, 0x0b0000, 0x06);
		dac_dual_regmap_write(dac_dai, 0x080000, 0x30);

		value = cs43198_calculation_rate_TO_ASP_register(dac_dai);
		dac_dual_regmap_write(dac_dai, 0x01000b, value);

		dac_dual_regmap_write(dac_dai, 0x040010, 0x01);
		dac_dual_regmap_write(dac_dai, 0x040011, 0x00);
		dac_dual_regmap_write(dac_dai, 0x040012, 0x08);
		dac_dual_regmap_write(dac_dai, 0x040013, 0x00);
		dac_dual_regmap_write(dac_dai, 0x040014, 0x1f);
		dac_dual_regmap_write(dac_dai, 0x040015, 0x00);
		dac_dual_regmap_write(dac_dai, 0x040016, 0x3f);
		dac_dual_regmap_write(dac_dai, 0x040017, 0x00);
		dac_dual_regmap_write(dac_dai, 0x040018, 0x0c);
		dac_dual_regmap_write(dac_dai, 0x040019, 0x0a);
		dac_dual_regmap_write(dac_dai, 0x050000, 0x00);
		dac_dual_regmap_write(dac_dai, 0x050001, 0x00);

		if (dac_dai->channel_mode == SPECIAL_MODE_3) {
			cs43198_set_mono_mode(dac_dai);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, 0x05000a, 0x07);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, 0x05000b, 0x07);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, 0x05000a, 0x07);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, 0x05000b, 0x07);
			if (dac_dai->regmap_3) {
				fiio_regmap_write(dac_dai->name, dac_dai->regmap_3, 0x05000a, 0x0f);
				fiio_regmap_write(dac_dai->name, dac_dai->regmap_3, 0x05000b, 0x0f);
			}
			if (dac_dai->regmap_4) {
				fiio_regmap_write(dac_dai->name, dac_dai->regmap_4, 0x05000a, 0x0f);
				fiio_regmap_write(dac_dai->name, dac_dai->regmap_4, 0x05000b, 0x0f);
			}
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, 0x090004, 0x04);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, 0x090004, 0x08);
			if (dac_dai->regmap_3)
				fiio_regmap_write(dac_dai->name, dac_dai->regmap_3, 0x090004, 0x04);
			if (dac_dai->regmap_4)
				fiio_regmap_write(dac_dai->name, dac_dai->regmap_4, 0x090004, 0x08);
		} else if (dac_dai->channel_mode == SPECIAL_MODE_2) {
			dac_dual_regmap_write(dac_dai, 0x05000a, 0x07);
			dac_dual_regmap_write(dac_dai, 0x05000b, 0x0f);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, 0x090004, 0x00);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, 0x090004, 0x0c);
		} else if (dac_dai->channel_mode == SPECIAL_MODE_1) {
			cs43198_set_mono_mode(dac_dai);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, 0x05000a, 0x07);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, 0x05000b, 0x07);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, 0x05000a, 0x0f);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, 0x05000b, 0x0f);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, 0x090004, 0x00);
			fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, 0x090004, 0x0c);
		}

		fiio_regmap_read(dac_dai->name, dac_dai->regmap_l, 0x0f0000, &value);
		fiio_regmap_read(dac_dai->name, dac_dai->regmap_r, 0x0f0000, &value);
		if (dac_dai->regmap_3)
			fiio_regmap_read(dac_dai->name, dac_dai->regmap_3, 0x0f0000, &value);
		if (dac_dai->regmap_4)
			fiio_regmap_read(dac_dai->name, dac_dai->regmap_4, 0x0f0000, &value);
		fiio_regmap_read(dac_dai->name, dac_dai->regmap_l, 0x0f0001, &value);
		fiio_regmap_read(dac_dai->name, dac_dai->regmap_r, 0x0f0001, &value);
		if (dac_dai->regmap_3)
			fiio_regmap_read(dac_dai->name, dac_dai->regmap_3, 0x0f0001, &value);
		if (dac_dai->regmap_4)
			fiio_regmap_read(dac_dai->name, dac_dai->regmap_4, 0x0f0001, &value);

		dac_dual_regmap_write(dac_dai, 0x0f0011, 0x07);

		fiio_regmap_read(dac_dai->name, dac_dai->regmap_l, 0x0f0000, &value);
		fiio_regmap_read(dac_dai->name, dac_dai->regmap_r, 0x0f0000, &value);
		if (dac_dai->regmap_3)
			fiio_regmap_read(dac_dai->name, dac_dai->regmap_3, 0x0f0000, &value);
		if (dac_dai->regmap_4)
			fiio_regmap_read(dac_dai->name, dac_dai->regmap_4, 0x0f0000, &value);

		dac_dual_regmap_write(dac_dai, 0x010006, 0x00);

		fiio_regmap_read(dac_dai->name, dac_dai->regmap_l, 0x020000, &value);
		dac_dual_regmap_write(dac_dai, 0x020000, value & ~0x40);
		fiio_regmap_read(dac_dai->name, dac_dai->regmap_l, 0x020000, &value);
		dac_dual_regmap_write(dac_dai, 0x020000, value & ~0x10);
	}
}

static void cs43198_set_sample_rate(struct snd_dac_dai *dac_dai)
{
	int value;

	pr_err("%s enter\n", __func__);

	if (!dac_dai->on)
		return;

	if (dac_dai->is_dsd) {
		value = cs43198_calculation_rate_TO_XSP_register(dac_dai);
		dac_dual_regmap_write(dac_dai, 0x070004, value | 0x13);
	} else {
		dac_dual_regmap_write(dac_dai, 0x070004, 0x00);
		value = cs43198_calculation_rate_TO_ASP_register(dac_dai);
		dac_dual_regmap_write(dac_dai, 0x01000b, value);
	}
}

static void cs43198_set_dre(struct snd_dac_dai *dac_dai)
{
	int value = 0;

	(void)value;
	pr_err("%s enter dac_dai->dre_setting %d\n", __func__, dac_dai->dre_setting);

	if (!dac_dai->on)
		return;

	if (dac_dai->dre_setting) {
		dac_dual_regmap_write(dac_dai, 0x010010, 0x99);
		dac_dual_regmap_write(dac_dai, 0x0c0001, 0x01);
		dac_dual_regmap_write(dac_dai, 0x0c0056, 0x0f);
		dac_dual_regmap_write(dac_dai, 0x010010, 0x00);
	} else {
		dac_dual_regmap_write(dac_dai, 0x010010, 0x99);
		dac_dual_regmap_write(dac_dai, 0x0c0001, 0x00);
		dac_dual_regmap_write(dac_dai, 0x010010, 0x00);
	}
}

static const struct regmap_config cs43198_regmap = {
	.reg_bits = 24,
	.pad_bits = 8,
	.val_bits = 8,

	.max_register = 0x190000,
	.reg_defaults = cs43198_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(cs43198_reg_defaults),
	.readable_reg = cs43198_readable_register,
	.precious_reg = cs43198_precious_register,
	.volatile_reg = cs43198_volatile_register,
	.cache_type = REGCACHE_RBTREE,
};

static const struct snd_dac_dai_ops cs43198_dac_dai_ops = {
	.set_thd_c2 = cs43198_set_thd_c2,
	.set_dac_sample_rate = cs43198_set_sample_rate,
	.read_reg_all = cs43198_reg_read,
	.config_dac = cs43198_config_dac,
	.sw_mute = cs43198_sw_mute,
	.set_volume_db = cs43198_set_volume_db,
	.set_lpf = cs43198_set_lpf,
	.set_dre = cs43198_set_dre,
};

static struct snd_dac_dai cs43198_dac_dai = {
	.name = "cs43198 dual",
	.ops = &cs43198_dac_dai_ops,
	.lpf_setting = 2,
	.lrbalance = 20,
	.dre_setting = true,
};

static struct fiio_device_driver dac_dai_driver = {
	.name = "dac_dai module",
	.private_data = &cs43198_dac_dai,
};

static int cs43198_i2c_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	struct cs43198_priv *cs43198;
	struct snd_dac_dai *dac_dai = &cs43198_dac_dai;
	struct device *dev = &client->dev;
	struct device_node *np = dev->of_node;
	unsigned int devid = 0;
	unsigned int dac_id = 0;
	unsigned int reg;
	int ret = 0;
	int i;

	(void)devid;
	(void)reg;
	(void)i;

	ret = of_property_read_u32_index(np, "id", 0, &dac_id);
	if (ret < 0) {
		dev_err(dev, "id property reading fail\n");
		goto ERROR;
	}
#line 841
	pr_err("%s(%d), dac_id = %d\n", __func__, __LINE__, dac_id);

	if (!g_cs43198) {
		g_cs43198 = kzalloc(sizeof(struct cs43198_priv), GFP_KERNEL);
		if (!g_cs43198)
			return -ENOMEM;
	}
	cs43198 = g_cs43198;
	i2c_set_clientdata(client, cs43198);

	switch (dac_id) {
	case 0:
		dac_dai->dev = dev;
		dac_dai->client_l = client;
		dac_dai->regmap_l = devm_regmap_init_i2c(client, &cs43198_regmap);
		if (IS_ERR(dac_dai->regmap_l)) {
			ret = PTR_ERR(dac_dai->regmap_l);
			dev_err(dev, "regmap init failed: %d\n", ret);
			goto ERROR;
		}
		pr_err("dac l finished. \n");
		break;
	case 1:
		dac_dai->client_r = client;
		dac_dai->regmap_r = devm_regmap_init_i2c(client, &cs43198_regmap);
		if (IS_ERR(dac_dai->regmap_r)) {
			ret = PTR_ERR(dac_dai->regmap_r);
			dev_err(dev, "regmap init failed: %d\n", ret);
			goto ERROR;
		}
		pr_err("dac r finished. \n");
		return 0;
	case 2:
		dac_dai->client_3 = client;
		dac_dai->regmap_3 = devm_regmap_init_i2c(client, &cs43198_regmap);
		if (IS_ERR(dac_dai->regmap_3)) {
			ret = PTR_ERR(dac_dai->regmap_3);
			dev_err(dev, "regmap init failed: %d\n", ret);
			goto ERROR;
		}
		pr_err("dac 3 finished. \n");
		return 0;
	case 3:
		dac_dai->client_4 = client;
		dac_dai->regmap_4 = devm_regmap_init_i2c(client, &cs43198_regmap);
		if (IS_ERR(dac_dai->regmap_4)) {
			ret = PTR_ERR(dac_dai->regmap_4);
			dev_err(dev, "regmap init failed: %d\n", ret);
			goto ERROR;
		}
		pr_err("dac 4 finished. \n");
		return 0;
	default:
		break;
	}

	cs43198->dac_dai = dac_dai;
	mutex_init(&cs43198->clk_mutex);

	fiio_device_register(&dac_dai_driver);

	pr_err("%s: exit \n", __func__);

	return 0;

ERROR:
	return ret;
}

static int cs43198_i2c_remove(struct i2c_client *client)
{
	pr_err("%s enter\n", __func__);
	fiio_device_unregister(&dac_dai_driver);
	return 0;
}

static const struct of_device_id cs43198_of_match[] = {
	{ .compatible = "cirrus,cs43198", },
	{},
};
MODULE_DEVICE_TABLE(of, cs43198_of_match);

static const struct i2c_device_id cs43198_i2c_id[] = {
	{ "cs43198", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, cs43198_i2c_id);

static struct i2c_driver cs43198_i2c_driver = {
	.driver = {
		.name = "cs43198",
		.owner = THIS_MODULE,
		.of_match_table = cs43198_of_match,
	},
	.probe = cs43198_i2c_probe,
	.remove = cs43198_i2c_remove,
	.id_table = cs43198_i2c_id,
};

static int __init cs43198_init(void)
{
	pr_err("[%s]", __func__);
	return i2c_add_driver(&cs43198_i2c_driver);
}

static void __exit cs43198_exit(void)
{
	i2c_del_driver(&cs43198_i2c_driver);
}

module_init(cs43198_init);
module_exit(cs43198_exit);

MODULE_AUTHOR("mxc@fiio.net");
MODULE_DESCRIPTION("Cirrus Logic CS43198 ALSA SoC Codec Driver");
MODULE_LICENSE("GPL");
