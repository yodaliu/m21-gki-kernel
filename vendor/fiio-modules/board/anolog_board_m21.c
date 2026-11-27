// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO M21 analog board control (anolog_board_m21).
 * Reconstructed from the stock FiiO M21 module.
 */
#define pr_fmt(fmt) "[anolog_board]: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <fiio/fiio_device.h>
#include <fiio/fiio_attr.h>
#include <fiio/fiio_utils.h>
#include <fiio/fpga_common.h>
#include <fiio/dac_dai.h>
#include <fiio/anolog_board_common.h>
#include <fiio/mode_select_common.h>
#include <fiio/fiio_headset_jack.h>
#include "anolog_board_m21.h"

int anolog_board_dc_det_enable(bool on);

static int force_output_type;
static int delay_time;
static int delay_time_nos = 100;
static int delay_time1 = 1000;
static int dsd_pcm;

struct anolog_board_priv {
	struct device *dev;

	int dac_vol_db;
	int dc_det_value;

	unsigned int gpio_ctrl[GPIO_MAX_NUM];

	struct anolog_board_public_data *anolog_board_public;

	struct snd_dac_dai *dac_dai;
	struct fpga_public_data *fpga_module;
	struct mode_select_public_data *mode_select_module;
	struct headset_jack_public_data *headset_jack_module;
};

static struct anolog_board_priv *g_data;

static void anolog_board_update_volume_table(struct anolog_board_priv *data)
{
	struct snd_dac_dai *dac_dai = data->dac_dai;

	pr_err("%s pogain:%d output:%d\n", __func__, dac_dai->pogain,
	       data->anolog_board_public->current_output_type);

	switch (dac_dai->pogain) {
	case 0:
		dac_dai->volume_table = vol_table_po_gain_l;
		dac_set_gpio_value(data->gpio_ctrl[GPIO_THX_HVCC_SW], 0);
		break;
	case 1:
		dac_dai->volume_table = vol_table_po_gain_m;
		dac_set_gpio_value(data->gpio_ctrl[GPIO_THX_HVCC_SW], 0);
		break;
	case 2:
		if (data->anolog_board_public->current_output_type == 3) {
			dac_dai->volume_table = vol_table_bal_gain_h;
			dac_set_gpio_value(data->gpio_ctrl[GPIO_THX_HVCC_SW], 0);
			break;
		}
		fallthrough;
	default:
		dac_dai->volume_table = vol_table_po_gain_h;
		dac_set_gpio_value(data->gpio_ctrl[GPIO_THX_HVCC_SW], 0);
		break;
	case 3:
		dac_dai->volume_table = vol_table_po_gain_sh;
		dac_set_gpio_value(data->gpio_ctrl[GPIO_THX_HVCC_SW], 1);
		break;
	}
}

static void anolog_board_set_volume_db(unsigned int lvol, unsigned int rvol);

static void anolog_board_update_volume(void)
{
	struct anolog_board_priv *data = g_data;
	struct snd_dac_dai *dac_dai = data->dac_dai;
	int vol;

	if (!dac_dai->on)
		return;
	if (data->anolog_board_public->current_output_type == 2)
		return;

	if (dac_dai->gain_update)
		anolog_board_update_volume_table(data);

	if (data->anolog_board_public->current_output_type == 4 ||
	    data->anolog_board_public->current_output_type == 1) {
		vol = dac_dai->volume;
		dac_dai->volume_table = vol_table_po_gain_sh;
		if (dac_dai->lo_vol_adjustable == 0) {
			dac_dai->volumeL_DB = 6;
			dac_dai->volumeR_DB = 6;
		} else {
			dac_dai->volumeL_DB = dac_dai->volume_table[vol] + (vol ? 6 : 0);
			dac_dai->volumeR_DB = dac_dai->volumeL_DB;
		}
		pr_err("%s vol:%d lvolume:%d rvolume:%d output_type:%d lo_vol_adjustable:%d\n",
		       __func__, vol, dac_dai->volumeL_DB, dac_dai->volumeR_DB,
		       data->anolog_board_public->current_output_type, dac_dai->lo_vol_adjustable);
	} else {
		if (dac_dai->pogain > 4)
			dac_dai->pogain = 4;
		dac_dai->volumeL_DB = dac_dai->volume_table[dac_dai->volume] + gain_table[dac_dai->pogain];
		if (dac_dai->volumeL_DB >= dac_dai->volume_table[0])
			dac_dai->volumeL_DB = dac_dai->volume_table[0];
		dac_dai->volumeR_DB = dac_dai->volume_table[dac_dai->volume] + gain_table[dac_dai->pogain];
		if (dac_dai->volumeR_DB > dac_dai->volume_table[0])
			dac_dai->volumeR_DB = dac_dai->volume_table[0];
	}

	if (dac_dai->lrbalance > 20) {
		if (dac_dai->volumeL_DB < dac_dai->volume_table[0] - 20)
			dac_dai->volumeL_DB = dac_dai->lrbalance + dac_dai->volumeL_DB - 20;
	} else if (dac_dai->lrbalance != 20) {
		if (dac_dai->volumeR_DB < dac_dai->volume_table[0] - 20)
			dac_dai->volumeR_DB = dac_dai->volumeR_DB - dac_dai->lrbalance + 20;
	}

	if (dac_dai->on) {
		pr_err("%s, vol:%d, ldb:%d, rdb:%d\n", __func__, dac_dai->volume,
		       dac_dai->volumeL_DB, dac_dai->volumeR_DB);
		data->dac_vol_db = dac_dai->volumeL_DB;
		if (dac_dai && dac_dai->ops->set_volume_db)
			dac_dai->ops->set_volume_db(dac_dai, dac_dai->volumeL_DB, dac_dai->volumeR_DB);
	}
}

static void anolog_board_set_volume_db(unsigned int lvol, unsigned int rvol)
{
	struct anolog_board_priv *data = g_data;
	struct snd_dac_dai *dac_dai = data->dac_dai;

	data->dac_vol_db = lvol;
	if (dac_dai && dac_dai->ops->set_volume_db)
		dac_dai->ops->set_volume_db(dac_dai, lvol, rvol);
}

int anolog_board_dc_det_enable(bool on)
{
	struct anolog_board_priv *data = g_data;

	if (!data || !data->dac_dai)
		return 0;

	switch (data->anolog_board_public->current_output_type) {
	case 0:
	case 3:
		break;
	default:
		on = false;
		break;
	}

	if (data->headset_jack_module) {
		pr_err("%s allow_dc_det = %d\n", __func__, data->headset_jack_module->allow_dc_det);
		if (data->headset_jack_module->allow_dc_det != on) {
			pr_err("dc_det_value change  : %d \n", on);
			if (on)
				mdelay(400);
			data->headset_jack_module->allow_dc_det = on;
		}
	}
	return 0;
}

static void anolog_board_hs35_det_disable(bool disable)
{
	struct anolog_board_priv *data = g_data;

	if (data->headset_jack_module) {
		pr_err("%s hs35_det_gpio_disable = %d\n", __func__,
		       data->headset_jack_module->hs35_det_gpio_disable);
		if (data->headset_jack_module->hs35_det_gpio_disable != disable) {
			pr_err("hs35_det_gpio_disable change  : %d \n", disable);
			data->headset_jack_module->hs35_det_gpio_disable = disable;
		}
	}
}

static void anolog_board_hw_mute(int mute)
{
	struct anolog_board_priv *data = g_data;
	struct snd_dac_dai *dac_dai = data->dac_dai;

	pr_err("%s output_type = %s, set mute = %d, output_mode = %d\n", __func__,
	       output_name[data->anolog_board_public->current_output_type], mute,
	       data->anolog_board_public->audio_output_mode);
	pr_err("%s isNeedPowerChange = %d, isNeedMute = %d\n", __func__,
	       data->anolog_board_public->isNeedPowerChange,
	       data->anolog_board_public->isNeedMute);

	switch (data->anolog_board_public->current_output_type) {
	case 0:
		if (mute) {
			pr_err("mute po\n");
			if (dac_dai->ops && dac_dai->ops->sw_mute) {
				dac_dai->soft_mute = true;
				dac_dai->ops->sw_mute(dac_dai, mute);
			}
			mdelay(10);
			if (dac_dai->nos_status) {
				dac_set_gpio_value(data->gpio_ctrl[GPIO_OUT_EN], 0);
				mdelay(10);
			}
		} else {
			pr_err("unmute  po\n");
			if (!dac_dai->nos_status) {
				mdelay(delay_time ? 350 : 20);
			} else {
				mdelay(delay_time_nos);
				dac_set_gpio_value(data->gpio_ctrl[GPIO_OUT_EN], 1);
			}
			if (dac_dai->ops && dac_dai->ops->sw_mute) {
				dac_dai->soft_mute = false;
				dac_dai->ops->sw_mute(dac_dai, 0);
			}
		}
		break;
	case 1:
		if (mute) {
			pr_err("mute lo\n");
			if (dac_dai->ops && dac_dai->ops->sw_mute) {
				dac_dai->soft_mute = true;
				dac_dai->ops->sw_mute(dac_dai, mute);
			}
			mdelay(10);
			if (dac_dai->nos_status) {
				dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SEL], 1);
				mdelay(10);
			}
		} else {
			pr_err("unmute  lo\n");
			if (!dac_dai->nos_status) {
				mdelay(delay_time ? 350 : 20);
			} else {
				mdelay(delay_time_nos);
				dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SEL], 0);
			}
			if (dac_dai->ops && dac_dai->ops->sw_mute) {
				dac_dai->soft_mute = false;
				dac_dai->ops->sw_mute(dac_dai, 0);
			}
		}
		break;
	case 3:
		if (mute) {
			pr_err("mute bal po\n");
			if (dac_dai->ops && dac_dai->ops->sw_mute) {
				dac_dai->soft_mute = true;
				dac_dai->ops->sw_mute(dac_dai, mute);
			}
			mdelay(10);
		} else {
			pr_err("unmute bal po\n");
			if (dac_dai->ops && dac_dai->ops->sw_mute) {
				dac_dai->soft_mute = false;
				dac_dai->ops->sw_mute(dac_dai, 0);
			}
			if (dac_dai->nos_status || !dac_dai->dre_setting) {
				mdelay(delay_time_nos);
				dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_BAL], 1);
			}
		}
		break;
	case 4:
		if (mute) {
			pr_err("mute bal lo\n");
			if (dac_dai->ops && dac_dai->ops->sw_mute) {
				dac_dai->soft_mute = true;
				dac_dai->ops->sw_mute(dac_dai, mute);
			}
			mdelay(10);
			if (dac_dai->nos_status) {
				dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SEL], 1);
				mdelay(10);
			}
		} else {
			pr_err("unmute bal lo\n");
			if (!dac_dai->nos_status) {
				mdelay(delay_time ? 350 : 20);
			} else {
				mdelay(delay_time_nos);
				dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SEL], 0);
			}
			if (dac_dai->ops && dac_dai->ops->sw_mute) {
				dac_dai->soft_mute = false;
				dac_dai->ops->sw_mute(dac_dai, 0);
			}
		}
		break;
	default:
		if (mute) {
			mdelay(delay_time ? 350 : 20);
			pr_err("mute no-headset\n");
			if (dac_dai->ops && dac_dai->ops->sw_mute) {
				dac_dai->soft_mute = true;
				dac_dai->ops->sw_mute(dac_dai, mute);
			}
		}
		break;
	}

	data->anolog_board_public->isNeedMute = mute ? true : false;
	if (!mute)
		data->anolog_board_public->isNeedPowerChange = false;
	data->anolog_board_public->is_muting = false;
	data->anolog_board_public->is_unmuting = false;
	data->anolog_board_public->is_muted = mute ? true : false;
}

static void anolog_board_mute_all(int mute)
{
	struct anolog_board_priv *data = g_data;
	struct snd_dac_dai *dac_dai = data->dac_dai;

	pr_err("%s: audio_output_mode %d current_output_type %d\n", __func__,
	       data->anolog_board_public->audio_output_mode,
	       data->anolog_board_public->current_output_type);
	if (dac_dai->ops && dac_dai->ops->sw_mute)
		dac_dai->ops->sw_mute(dac_dai, 1);
	mdelay(10);
	data->anolog_board_public->is_muted = true;
}

static void anolog_board_config_fpga(struct anolog_board_priv *data)
{
	struct snd_dac_dai *dac_dai = data->dac_dai;
	int fmt;

	switch (dac_dai->system_work_mode) {
	case SYSTEM_MODE_MTP:
		if (data->fpga_module && data->fpga_module->set_input_channel)
			data->fpga_module->set_input_channel(fpgachl_soc);
		break;
	case SYSTEM_MODE_XMOS:
	case SYSTEM_MODE_HDMI_TX_ARC:
		if (data->fpga_module && data->fpga_module->set_input_channel)
			data->fpga_module->set_input_channel(fpgachl_xmos);
		break;
	case SYSTEM_MODE_BT:
		if (data->fpga_module && data->fpga_module->set_input_channel)
			data->fpga_module->set_input_channel(fpgachl_bt);
		break;
	case SYSTEM_MODE_HDMI_IN:
		if (data->fpga_module && data->fpga_module->set_input_channel)
			data->fpga_module->set_input_channel(fpgachl_hdmi);
		if (data->fpga_module && data->fpga_module->hw_params)
			data->fpga_module->hw_params(8, dac_dai->sample_rate, dac_dai->bits);
		return;
	default:
		break;
	}

	if (data->anolog_board_public->current_output_type == 2) {
#line 392
		pr_err("[%s %d] config to spdif output.\n", __func__, __LINE__);
		fmt = dac_dai->is_dsd ? 4 : 0;
	} else {
		if (data->fpga_module && data->fpga_module->set_mclk)
			data->fpga_module->set_mclk(AUDIO_DAC_MCLK_22);
		fmt = dac_dai->is_dsd ? 3 : 1;
	}

	if (data->fpga_module && data->fpga_module->fill_clk)
		data->fpga_module->fill_clk(true);
	if (data->fpga_module && data->fpga_module->hw_params)
		data->fpga_module->hw_params(fmt, dac_dai->sample_rate, dac_dai->bits);
	if (data->fpga_module && data->fpga_module->fill_clk)
		data->fpga_module->fill_clk(false);
}

static void anolog_board_config_dac(struct anolog_board_priv *data)
{
	struct snd_dac_dai *dac_dai = data->dac_dai;

#line 420
	pr_err("[%s %d]\n", __func__, __LINE__);
	if (dac_dai) {
		if (dac_dai->ops->config_dac)
			dac_dai->ops->config_dac(dac_dai);
		if (dac_dai->ops->set_performance_mode)
			dac_dai->ops->set_performance_mode(dac_dai, dac_dai->system_work_mode);
		if (dac_dai->ops->set_lpf)
			dac_dai->ops->set_lpf(dac_dai);
		if (dac_dai->ops->set_dre)
			dac_dai->ops->set_dre(dac_dai);
	}
}

static void anolog_board_hw_params(void)
{
	struct anolog_board_priv *data = g_data;
	struct snd_dac_dai *dac_dai = data->dac_dai;
	int type;

#line 440
	pr_err("[%s %d]\n", __func__, __LINE__);
	anolog_board_config_fpga(data);

	if (data->anolog_board_public->current_output_type == 2)
		return;

	pr_err("%s, is_dsd %d \n", __func__, dac_dai->is_dsd);
	type = dac_dai->is_dsd ? 1 : 2;
	if (dsd_pcm != type) {
		delay_time_nos = delay_time1;
		dsd_pcm = type;
		delay_time = 1;
#line 452
		pr_err("%s %d, reset dac \n", __func__, __LINE__);
		if (data->anolog_board_public->current_output_type == 3 ||
		    data->anolog_board_public->current_output_type == 0) {
			dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_BAL], 0);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_PO], 0);
		}
		mdelay(50);
		dac_set_gpio_value(data->gpio_ctrl[GPIO_DAC_RST], 0);
		mdelay(50);
		dac_set_gpio_value(data->gpio_ctrl[GPIO_DAC_RST], 1);
		mdelay(20);
		if (data->anolog_board_public->current_output_type == 0) {
			dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_PO], 1);
		} else if (data->anolog_board_public->current_output_type == 3) {
			if (!dac_dai->nos_status && dac_dai->dre_setting)
				dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_BAL], 1);
		}
		anolog_board_config_dac(data);
	} else {
		delay_time = 0;
		delay_time_nos = 100;
		mdelay(50);
		if (dac_dai && dac_dai->ops->set_dac_sample_rate)
			dac_dai->ops->set_dac_sample_rate(dac_dai);
	}
}

static void anolog_board_pre_power_control(struct anolog_board_priv *data, bool on)
{
	struct snd_dac_dai *dac_dai = data->dac_dai;

	if (on) {
		if (!dac_dai->on) {
#line 486
			pr_err("%s[%d], goto pre power on\n", __func__, __LINE__);
			dac_set_gpio_value_cansleep(data->gpio_ctrl[GPIO_ANG_PWR], 1);
			mdelay(20);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SW_EN], 1);
		} else {
#line 491
			pr_err("%s[%d], already pre poweron\n", __func__, __LINE__);
		}
	} else {
#line 495
		pr_err("%s[%d], goto pre power off\n", __func__, __LINE__);
		dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SW_EN], 0);
		dac_set_gpio_value_cansleep(data->gpio_ctrl[GPIO_ANG_PWR], 0);
	}
}

static void anolog_board_set_dac_power(struct anolog_board_priv *data, bool on)
{
	struct snd_dac_dai *dac_dai = data->dac_dai;

#line 505
	pr_err("%s[%d], on/off = %d\n", __func__, __LINE__, on);
	if (on) {
		if (data->fpga_module && data->fpga_module->power_by_mode)
			data->fpga_module->power_by_mode(true, 0);
		mdelay(10);
		dac_set_gpio_value(data->gpio_ctrl[GPIO_DAC_4V2_PWR], 1);
		mdelay(10);
		dac_set_gpio_value(data->gpio_ctrl[GPIO_DAC_PWR], 1);
		mdelay(10);
		dac_set_gpio_value(data->gpio_ctrl[GPIO_DAC_RST], 1);
		dac_dai->on = on;
	} else {
		dac_dai->on = on;
		report_playing_info(dac_dai->dev, 0, 0);
		dac_set_gpio_value(data->gpio_ctrl[GPIO_DAC_RST], 0);
		dac_set_gpio_value(data->gpio_ctrl[GPIO_DAC_PWR], 0);
		dac_set_gpio_value(data->gpio_ctrl[GPIO_DAC_4V2_PWR], 0);
		if (data->fpga_module && data->fpga_module->power_by_mode)
			data->fpga_module->power_by_mode(false, 0);
	}
}

static void anolog_board_post_power_control(struct anolog_board_priv *data, bool on)
{
	if (on) {
		if (data->dac_dai->pogain == 3)
			dac_set_gpio_value(data->gpio_ctrl[GPIO_THX_HVCC_SW], 1);
		else
			dac_set_gpio_value(data->gpio_ctrl[GPIO_THX_HVCC_SW], 0);
		dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_PO], 0);
		dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_BAL], 0);
		dac_set_gpio_value(data->gpio_ctrl[GPIO_OUT_EN], 1);
		dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SEL], 1);
		mdelay(20);

		switch (data->anolog_board_public->current_output_type) {
		case 0:
			data->dac_dai->soft_mute = true;
			dac_set_gpio_value(data->gpio_ctrl[GPIO_BAL_PWR_EN], 0);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_PO_PWR_EN], 1);
			mdelay(50);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SEL], 0);
			mdelay(50);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SEL], 1);
			mdelay(100);
			mdelay(10);
			break;
		case 1:
			data->dac_dai->soft_mute = true;
			dac_set_gpio_value(data->gpio_ctrl[GPIO_BAL_PWR_EN], 0);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_PO_PWR_EN], 1);
			mdelay(50);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_BAL], 1);
			mdelay(50);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SEL], 0);
			break;
		case 3:
			data->dac_dai->soft_mute = true;
			dac_set_gpio_value(data->gpio_ctrl[GPIO_PO_PWR_EN], 1);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_BAL_PWR_EN], 1);
			mdelay(50);
			break;
		case 4:
			data->dac_dai->soft_mute = true;
			anolog_board_hs35_det_disable(true);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_PO_PWR_EN], 1);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_BAL_PWR_EN], 1);
			mdelay(50);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SEL], 0);
			mdelay(10);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_PO], 0);
			break;
		default:
			break;
		}
	} else {
		dac_set_gpio_value(data->gpio_ctrl[GPIO_THX_HVCC_SW], 0);

		switch (data->anolog_board_public->current_output_type) {
		case 0:
			dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_PO], 0);
			mdelay(10);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_PO_PWR_EN], 0);
			mdelay(50);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_OUT_EN], 0);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SEL], 0);
			break;
		case 1:
			dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SEL], 1);
			mdelay(10);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_PO_PWR_EN], 0);
			mdelay(50);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_OUT_EN], 0);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SEL], 0);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_BAL], 0);
			break;
		case 3:
			dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_BAL], 0);
			mdelay(10);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_PO_PWR_EN], 0);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_BAL_PWR_EN], 0);
			mdelay(50);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_OUT_EN], 0);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SEL], 0);
			break;
		case 4:
			dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SEL], 1);
			mdelay(10);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_PO_PWR_EN], 0);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_BAL_PWR_EN], 0);
			mdelay(50);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_OUT_EN], 0);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SEL], 0);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_PO], 0);
			break;
		default:
			pr_err("power down no-headset\n");
			dac_set_gpio_value(data->gpio_ctrl[GPIO_OUT_EN], 1);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SEL], 1);
			mdelay(10);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_PO], 0);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_BAL], 0);
			mdelay(10);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_PO_PWR_EN], 0);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_BAL_PWR_EN], 0);
			mdelay(50);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_OUT_EN], 0);
			dac_set_gpio_value(data->gpio_ctrl[GPIO_LO_PO_SEL], 0);
			break;
		}
		anolog_board_hs35_det_disable(false);
	}

	anolog_board_dc_det_enable(on);
}

static void anolog_board_close_dac_power_for_spdif(struct anolog_board_priv *data)
{
	struct snd_dac_dai *dac_dai = data->dac_dai;

#line 637
	pr_err("%s[%d] \n", __func__, __LINE__);
	dac_dai->on = false;
	dac_set_gpio_value_cansleep(data->gpio_ctrl[GPIO_ANG_PWR], 0);
	dac_set_gpio_value(data->gpio_ctrl[GPIO_DAC_RST], 0);
	dac_set_gpio_value(data->gpio_ctrl[GPIO_DAC_PWR], 1);
	anolog_board_post_power_control(data, false);
	if (data->fpga_module && data->fpga_module->power_by_mode)
		data->fpga_module->power_by_mode(true, 0);
}

static void anolog_board_control_all_power(bool on)
{
	struct anolog_board_priv *data = g_data;

#line 653
	pr_err("%s[%d], on/off = %d\n", __func__, __LINE__, on);
	data->anolog_board_public->isNeedMute = true;
	data->anolog_board_public->isNeedPowerChange = true;
	dsd_pcm = 0;

	if (data->anolog_board_public->current_output_type == 2 && on) {
		anolog_board_close_dac_power_for_spdif(data);
	} else if (on) {
		anolog_board_pre_power_control(data, true);
		anolog_board_set_dac_power(data, true);
		anolog_board_post_power_control(data, true);
		data->anolog_board_public->isPlaying = true;
	} else {
		anolog_board_post_power_control(data, false);
		anolog_board_set_dac_power(data, false);
		anolog_board_pre_power_control(data, false);
		data->anolog_board_public->isPlaying = false;
	}
}

static int anolog_board_current_output_type(void)
{
	struct anolog_board_priv *data = g_data;

	return data->anolog_board_public->current_output_type;
}

static int anolog_board_current_output_gain(void)
{
	struct anolog_board_priv *data = g_data;

	return data->dac_dai->pogain;
}

static int anolog_board_current_output_volume(void)
{
	struct anolog_board_priv *data = g_data;

	return data->dac_dai->volume;
}

static int anolog_board_gpio_init(struct anolog_board_priv *data)
{
	struct device *dev = data->dev;
	struct device_node *np = dev->of_node;
	int ret;
	int i;

	pr_err("%s: GPIO_MAX_NUM = %d\n", __func__, GPIO_MAX_NUM);
	for (i = 0; i < GPIO_MAX_NUM; i++) {
		data->gpio_ctrl[i] = of_get_named_gpio(np, gpio_name[i], 0);
		pr_err("[%s] init data->gpio_ctrl[%d] %s %d\n", __func__, i, gpio_name[i],
		       data->gpio_ctrl[i]);
		if (!gpio_is_valid(data->gpio_ctrl[i])) {
			pr_err("[%s] of_get_named_gpio Failed :%d name:%s\n", __func__,
			       data->gpio_ctrl[i], gpio_name[i]);
			return data->gpio_ctrl[i];
		}

		ret = gpio_request(data->gpio_ctrl[i], gpio_name[i]);
		if (ret == -EBUSY) {
			pr_err("[%s] gpio_request %s busy: %d.\n", __func__, gpio_name[i], ret);
		} else if (ret < 0) {
			pr_err("[%s] gpio_request %s failed: %d\n", __func__, gpio_name[i], ret);
			return ret;
		}
		dac_set_gpio_value(data->gpio_ctrl[i], 0);
		pr_err("[%s] %s: %d\n", __func__, gpio_name[i], dac_get_gpio_value(data->gpio_ctrl[i]));
	}

	return 0;
}

static void anolog_board_gpio_exit(struct anolog_board_priv *data)
{
	int i;

	pr_err("%s: GPIO_MAX_NUM = %d\n", __func__, GPIO_MAX_NUM);
	for (i = 0; i < GPIO_MAX_NUM; i++) {
		if (gpio_is_valid(data->gpio_ctrl[i]))
			gpio_free(data->gpio_ctrl[i]);
	}
}

static ssize_t anolog_board_dac_volume_show(struct device *dev,
					    struct device_attribute *attr, char *buf)
{
	struct anolog_board_priv *data = g_data;
	int volume = 255 - data->dac_vol_db;

	if (volume < 0)
		volume = 0;
	return sprintf(buf, "%d", volume);
}

static ssize_t anolog_board_dac_volume_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t size)
{
	struct anolog_board_priv *data = g_data;
	struct snd_dac_dai *dac_dai = data->dac_dai;
	int volume_db;
	int volume = atoi(buf);

	if (volume < 256) {
		volume_db = 255 - volume;
		pr_err("%s, %s, volume_db = 0x%02x\n", __func__, buf, volume_db);
		data->dac_vol_db = volume_db;
		if (dac_dai && dac_dai->ops->set_volume_db)
			dac_dai->ops->set_volume_db(dac_dai, volume_db, volume_db);
	} else {
		pr_err("%s, range[0~255] but now is %s\n", __func__, buf);
	}
	return size;
}

static ssize_t anolog_board_output_type(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d", force_output_type + 1);
}

static ssize_t anolog_board_debug_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t size)
{
	struct anolog_board_priv *data = g_data;
	struct snd_dac_dai *dac_dai;

	if (!size)
		return size;

	dac_dai = data->dac_dai;
	pr_err("%s, %s \n", __func__, buf);
	if (!strcmp(buf, "fpga_i2c\n")) {
		if (data->fpga_module && data->fpga_module->set_input_channel)
			data->fpga_module->set_input_channel(fpgachl_soc);
	} else if (!strcmp(buf, "dac_i2c\n")) {
		if (dac_dai && dac_dai->ops->set_lpf)
			dac_dai->ops->set_lpf(dac_dai);
	} else if (!strcmp(buf, "power_on\n")) {
		anolog_board_pre_power_control(data, false);
		anolog_board_set_dac_power(data, false);
		mdelay(500);
		anolog_board_pre_power_control(data, true);
		anolog_board_set_dac_power(data, true);
	} else if (!strcmp(buf, "power_off\n")) {
		anolog_board_pre_power_control(data, false);
		anolog_board_set_dac_power(data, false);
	} else if (!strcmp(buf, "po_on\n")) {
		force_output_type = 0;
		dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_PO], 1);
		dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_BAL], 0);
	} else if (!strcmp(buf, "bal_on\n")) {
		force_output_type = 1;
		dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_PO], 0);
		dac_set_gpio_value(data->gpio_ctrl[GPIO_OUTSEL_BAL], 1);
	} else {
		pr_err("%s error cmd: %s\n", __func__, buf);
	}
	return size;
}

static void anolog_board_load_work(void)
{
	struct anolog_board_priv *data = g_data;

	pr_err("%s: Enter!\n", __func__);

	data->fpga_module = fiio_device_get_data_by_name("fpga module");
	if (!data->fpga_module)
		pr_err("get fpga module failed!\n");

	data->mode_select_module = fiio_device_get_data_by_name("mode_select module");
	if (!data->mode_select_module)
		pr_err("get mode_select module failed!\n");

	data->dac_dai = fiio_device_get_data_by_name("dac_dai module");
	if (!data->dac_dai) {
		pr_err("get dac_dai module failed!\n");
	} else {
		data->dac_dai->system_work_mode_old = SYSTEM_MODE_MTP;
		data->dac_dai->channel_mode = SPECIAL_MODE_3;
		data->dac_dai->pogain = 2;
		data->dac_dai->pogain_bk = 2;
		data->dac_dai->volume_table = vol_table_po_gain_h;
	}

	data->headset_jack_module = fiio_device_get_data_by_name("headset_jack module");
	if (!data->headset_jack_module)
		pr_err("get headset_jack module failed!\n");
}

static ssize_t anolog_board_delay_ctrl_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t size)
{
	if (size) {
		pr_err("%s, %d \n", __func__, atoi(buf));
		pr_err("%s, %d \n", __func__, atoi(buf + 3));
		delay_time1 = atoi(buf);
		pr_err("%s,delay_time1 %d\n", __func__, delay_time1);
	}
	return size;
}

static ssize_t anolog_board_pause_by_headset_store(struct device *dev,
						   struct device_attribute *attr,
						   const char *buf, size_t size)
{
	struct anolog_board_priv *data = g_data;
	int headset_pause_status = atoi(buf);

	pr_err("%s, %s, headset_pause_status = %s\n", __func__, buf,
	       headset_pause_status ? "TRUE" : "FALSE");
	if (headset_pause_status == 1)
		data->anolog_board_public->is_pause_by_headset = true;
	else if (headset_pause_status == 0)
		data->anolog_board_public->is_pause_by_headset = false;
	return size;
}

static ssize_t anolog_board_dac_dre_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t size)
{
	struct anolog_board_priv *data = g_data;
	struct snd_dac_dai *dac_dai = data->dac_dai;
	int enable = atoi(buf);

	pr_err("%s, %s, dre_status = %s\n", __func__, buf, enable ? "TRUE" : "FALSE");
	dac_dai->dre_setting = (enable == 1);
	dac_dai->ops->set_dre(dac_dai);
	return size;
}

static const struct device_attribute anolog_board_attributes[] = {
	__ATTR(board_delay_ctrl, 0200, NULL, anolog_board_delay_ctrl_store),
	__ATTR(board_vol_dac, 0644, anolog_board_dac_volume_show, anolog_board_dac_volume_store),
	__ATTR(board_output_type, 0444, anolog_board_output_type, NULL),
	__ATTR(board_debug, 0200, NULL, anolog_board_debug_store),
	__ATTR(board_is_pause_by_headset, 0200, NULL, anolog_board_pause_by_headset_store),
	__ATTR(board_dac_dre_switch, 0200, NULL, anolog_board_dac_dre_store),
};

static struct anolog_board_public_data anolog_board_d = {
	.dsd_unmute_delaytime = 10,
	.pcm_unmute_delaytime = 10,
	.isNeedPowerChange = true,
	.isNeedMute = true,
	.is_pause_by_headset = true,
	.audio_output_mode = FIIO_OUTPUT_MODE_ALL,
	.current_output_type = 8,
	.new_output_type = 8,
	.update_volume = anolog_board_update_volume,
	.set_volume_db = anolog_board_set_volume_db,
	.control_all_power = anolog_board_control_all_power,
	.hw_mute = anolog_board_hw_mute,
	.mute_all = anolog_board_mute_all,
	.hw_params = anolog_board_hw_params,
	.get_output_type = anolog_board_current_output_type,
	.get_output_gain = anolog_board_current_output_gain,
	.get_output_volume = anolog_board_current_output_volume,
};

static struct fiio_device_driver anolog_board_device_driver = {
	.name = "anolog_board module",
	.private_data = &anolog_board_d,
	.load_work = anolog_board_load_work,
};

static int anolog_board_platform_probe(struct platform_device *pdev)
{
	struct anolog_board_priv *data;
	struct device *dev = &pdev->dev;
	int ret;

#line 954
	pr_err("%s(%d)\n", __func__, __LINE__);
	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		goto ERROR;

	g_data = data;
	data->dev = dev;

	ret = anolog_board_gpio_init(data);
	if (ret < 0) {
		pr_err("gpio init failed. \n");
		return ret;
	}

	data->anolog_board_public = &anolog_board_d;
	fiio_device_register(&anolog_board_device_driver);

	ret = fiio_device_attribute_register(anolog_board_attributes,
					     ARRAY_SIZE(anolog_board_attributes));
	if (ret) {
		printk("attribute register failed!!");
		goto ERROR;
	}

#line 978
	pr_err("%s(%d) probe finished\n", __func__, __LINE__);
	return 0;

ERROR:
	return -ENOMEM;
}

static int anolog_board_platform_remove(struct platform_device *pdev)
{
	pr_err("[%s] enter ... \n", __func__);
	fiio_device_attribute_unregister(anolog_board_attributes);
	fiio_device_unregister(&anolog_board_device_driver);
	return 0;
}

static const struct of_device_id anolog_board_dt_match[] = {
	{ .compatible = "fiio,anolog_board", },
	{ }
};
MODULE_DEVICE_TABLE(of, anolog_board_dt_match);

static struct platform_driver anolog_board_driver = {
	.driver = {
		.name = "anolog_board",
		.owner = THIS_MODULE,
		.of_match_table = anolog_board_dt_match,
	},
	.probe = anolog_board_platform_probe,
	.remove = anolog_board_platform_remove,
};

static int __init anolog_board_init(void)
{
	pr_err("[%s] enter ... \n", __func__);
	return platform_driver_register(&anolog_board_driver);
}

static void __exit anolog_board_exit(void)
{
	pr_err("[%s] enter ... \n", __func__);
	anolog_board_gpio_exit(g_data);
	kfree(g_data);
	platform_driver_unregister(&anolog_board_driver);
}

module_init(anolog_board_init);
module_exit(anolog_board_exit);

MODULE_LICENSE("GPL v2");

MODULE_AUTHOR("mxc@fiio.net");
MODULE_DESCRIPTION("anolog board");
