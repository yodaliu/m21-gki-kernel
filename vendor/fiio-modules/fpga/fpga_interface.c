// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO FPGA audio interface hal: forwards the "fpga module" calls of the DAC
 * drivers to the FPGA driver ("fpga_device module", gw-fpga) and exposes
 * debug controls in sysfs.
 */

#define pr_fmt(fmt) "[fiio_fpga_hal]: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <fiio/fiio_notify.h>
#include <fiio/fiio_attr.h>
#include <fiio/fiio_device.h>

#include "fpga_interface.h"

static struct fpga_interface_hal_data *g_fpga;

static int fpga_set_input_channel(enum fpga_channel chl)
{
	struct fpga_interface_hal_data *fpga = g_fpga;

	if (fpga->fpga_device && fpga->fpga_device->set_input_channel)
		fpga->fpga_device->set_input_channel(chl);

	return 0;
}

static int fpga_interface_hal_power(bool enable, int mode)
{
	struct fpga_interface_hal_data *fpga = g_fpga;

	if (fpga->fpga_device && fpga->fpga_device->ctrl_power) {
		fpga->fpga_public->on = enable;
		fpga->fpga_device->ctrl_power(enable);
	}

	return 0;
}

static void fpga_interface_init_xmos(void)
{
	struct fpga_interface_hal_data *fpga = g_fpga;

	if (fpga->fpga_device && fpga->fpga_device->init_xmos)
		fpga->fpga_device->init_xmos();
}

static void fpga_interface_fill_clk(bool enable)
{
	struct fpga_interface_hal_data *fpga = g_fpga;

	if (fpga->fpga_device && fpga->fpga_device->fill_clk)
		fpga->fpga_device->fill_clk(enable);
}

static void fpga_interface_set_mclk(enum fpga_dac_mclk_t mclk_sel)
{
	struct fpga_interface_hal_data *fpga = g_fpga;

	if (fpga->fpga_device && fpga->fpga_device->set_mclk)
		fpga->fpga_device->set_mclk(mclk_sel);
}

static void fpga_interface_enter_usb_xmos_mode(void)
{
	struct fpga_interface_hal_data *fpga = g_fpga;

	if (fpga->fpga_device && fpga->fpga_device->enter_usb_xmos_mode)
		fpga->fpga_device->enter_usb_xmos_mode();
}

static void fpga_interface_enter_local_playback_mode(void)
{
	struct fpga_interface_hal_data *fpga = g_fpga;

	if (fpga->fpga_device && fpga->fpga_device->enter_local_playback_mode)
		fpga->fpga_device->enter_local_playback_mode();
}

static void fpga_interface_config_xmos_spdif_rx(bool enable)
{
	struct fpga_interface_hal_data *fpga = g_fpga;

	if (fpga->fpga_device && fpga->fpga_device->config_xmos_spdif_rx)
		fpga->fpga_device->config_xmos_spdif_rx(enable);
}

static void fpga_interface_config_hdmi_code_fmt(enum fpga_hdmi_code_fmt fmt)
{
	struct fpga_interface_hal_data *fpga = g_fpga;

	if (fpga->fpga_device && fpga->fpga_device->config_hdmi_code_fmt)
		fpga->fpga_device->config_hdmi_code_fmt(fmt);
}

static int fpga_interface_config_spdif_tx(bool enable)
{
	struct fpga_interface_hal_data *fpga = g_fpga;

	if (fpga->fpga_device && fpga->fpga_device->config_spdif_tx)
		fpga->fpga_device->config_spdif_tx(enable);

	return 0;
}

static int fpga_interface_config_opt_tx(bool enable)
{
	struct fpga_interface_hal_data *fpga = g_fpga;

	if (fpga->fpga_device && fpga->fpga_device->config_opt_tx)
		fpga->fpga_device->config_opt_tx(enable);

	return 0;
}

static void fpga_interface_hal_bypass(fpga_config_bypass ch, bool bypass)
{
	switch (ch) {
	case FPGA_CONFIG_SOC_I2S_BYPASS:
		g_fpga->soc_i2s_bypass = bypass;
		break;
	case FPGA_CONFIG_UAC_I2S_BYPASS:
		g_fpga->uac_i2s_bypass = bypass;
		break;
	case FPGA_CONFIG_BT_I2S_BYPASS:
		g_fpga->bt_i2s_bypass = bypass;
		break;
	case FPGA_CONFIG_SPDIF_I2S_BYPASS:
		g_fpga->spdif_i2s_bypass = bypass;
		break;
	case FPGA_CONFIG_OPT_I2S_BYPASS:
		g_fpga->opt_i2s_bypass = bypass;
		break;
	case FPGA_CONFIG_HDMI_I2S_BYPASS:
		g_fpga->hdmi_i2s_bypass = bypass;
		break;
	case FPGA_CONFIG_SPDIF_BYPASS:
		g_fpga->spdif_bmc_bypass = bypass;
		break;
	case FPGA_CONFIG_OPT_BYPASS:
		g_fpga->opt_bmc_bypass = bypass;
		break;
	default:
		break;
	}
}

static void fpga_interface_hal_hwinit(struct fpga_hwinit_params *hwinit_params)
{
	struct fpga_interface_hal_data *fpga = g_fpga;

	if (fpga->fpga_device && fpga->fpga_device->fpga_hwinit)
		fpga->fpga_device->fpga_hwinit(fpga->fpga_device, hwinit_params);
}

static void fpga_ctrl_power_by_mode(bool enable, int mode)
{
	struct fpga_interface_hal_data *fpga = g_fpga;

	if (fpga->fpga_device && fpga->fpga_device->power_by_mode) {
		fpga->fpga_public->on = enable;
		fpga->fpga_device->power_by_mode(fpga->fpga_device, enable, mode);
	}
}

static void fpga_interface_hal_config(struct fpga_config_params *fpga_params)
{
	struct fpga_interface_hal_data *fpga = g_fpga;

	if (fpga->fpga_device && fpga->fpga_device->fpga_config_full)
		fpga->fpga_device->fpga_config_full(fpga->fpga_device, fpga_params);
}

static int fpga_hw_params(int fmt, int rate, int bits)
{
	struct fpga_interface_hal_data *fpga = g_fpga;

	if (fpga->fpga_device && fpga->fpga_device->fpga_config)
		fpga->fpga_device->fpga_config(fpga->fpga_device, fmt, rate, bits);

	return 0;
}

static int fiio_control_fpga_ctrl_notifier_cb(struct notifier_block *self,
					      unsigned long event, void *data)
{
	struct fiio_control_fpga_ctrl_notify_data *notify_data = data;

	(void)notify_data;

	return 0;
}

static ssize_t fpga_power_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fpga_interface_hal_data *fpga = g_fpga;

	return sprintf(buf, "%s\n", fpga->fpga_public->on ? "on" : "off");
}

static ssize_t fpga_power_store(struct device *dev,
				struct device_attribute *attr, const char *buf, size_t size)
{
	if (!size)
		return 0;

	if (!strcmp(buf, "on\n"))
		fpga_interface_hal_power(true, 0);
	else if (!strcmp(buf, "off\n"))
		fpga_interface_hal_power(false, 0);

	return size;
}

static ssize_t fpga_bypass_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", g_fpga->uac_i2s_bypass);
}

static ssize_t fpga_bypass_store(struct device *dev,
				 struct device_attribute *attr, const char *buf, size_t size)
{
	if (!size)
		return size;

	if (!strcmp(buf, "soc bypass on\n"))
		fpga_interface_hal_bypass(FPGA_CONFIG_SOC_I2S_BYPASS, true);
	else if (!strcmp(buf, "soc bypass off\n"))
		fpga_interface_hal_bypass(FPGA_CONFIG_SOC_I2S_BYPASS, false);
	else if (!strcmp(buf, "uac bypass on\n"))
		fpga_interface_hal_bypass(FPGA_CONFIG_UAC_I2S_BYPASS, true);
	else if (!strcmp(buf, "uac bypass off\n"))
		fpga_interface_hal_bypass(FPGA_CONFIG_UAC_I2S_BYPASS, false);
	else if (!strcmp(buf, "bt bypass on\n"))
		fpga_interface_hal_bypass(FPGA_CONFIG_BT_I2S_BYPASS, true);
	else if (!strcmp(buf, "bt bypass off\n"))
		fpga_interface_hal_bypass(FPGA_CONFIG_BT_I2S_BYPASS, false);
	else if (!strcmp(buf, "spdif bypass on\n"))
		fpga_interface_hal_bypass(FPGA_CONFIG_SPDIF_I2S_BYPASS, true);
	else if (!strcmp(buf, "spdif bypass off\n"))
		fpga_interface_hal_bypass(FPGA_CONFIG_SPDIF_I2S_BYPASS, false);
	else if (!strcmp(buf, "opt bypass on\n"))
		fpga_interface_hal_bypass(FPGA_CONFIG_OPT_I2S_BYPASS, true);
	else if (!strcmp(buf, "opt bypass off\n"))
		fpga_interface_hal_bypass(FPGA_CONFIG_OPT_I2S_BYPASS, false);
	else if (!strcmp(buf, "hdmi bypass on\n"))
		fpga_interface_hal_bypass(FPGA_CONFIG_HDMI_I2S_BYPASS, true);
	else if (!strcmp(buf, "hdmi bypass off\n"))
		fpga_interface_hal_bypass(FPGA_CONFIG_HDMI_I2S_BYPASS, false);
	else if (!strcmp(buf, "spdif bmc bypass on\n"))
		fpga_interface_hal_bypass(FPGA_CONFIG_SPDIF_BYPASS, true);
	else if (!strcmp(buf, "spdif bmc bypass off\n"))
		fpga_interface_hal_bypass(FPGA_CONFIG_SPDIF_BYPASS, false);
	else if (!strcmp(buf, "opt bmc bypass on\n"))
		fpga_interface_hal_bypass(FPGA_CONFIG_OPT_BYPASS, true);
	else if (!strcmp(buf, "opt bmc bypass off\n"))
		fpga_interface_hal_bypass(FPGA_CONFIG_OPT_BYPASS, false);

	return size;
}

static ssize_t fpga_hwinit_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "ch=%d imclkd=%d imclkt=%d dac1_mclk_en=%d dac1_mclk=%d dac2_mclk_en=%d dac2_mclk=%d p2d=%d p2d_t=%d one_line=%d mul_line=%d\n",
		       g_fpga->fpga_device->audio_channel,
		       g_fpga->fpga_device->input_mclk_direction,
		       g_fpga->fpga_device->input_mclk_type,
		       g_fpga->fpga_device->dac1_mclk_out_enable,
		       g_fpga->fpga_device->dac1_mclk_type,
		       g_fpga->fpga_device->dac2_mclk_out_enable,
		       g_fpga->fpga_device->dac2_mclk_type,
		       g_fpga->fpga_device->p2d_enable,
		       g_fpga->fpga_device->p2d_type,
		       g_fpga->fpga_device->soc_use_one_line_data,
		       g_fpga->fpga_device->dac_multiple_lines_data);
}

static fpga_hal_channel fpga_hwinit_get_ch(char *buf)
{
	if (!strcmp(buf, "soc"))
		return FPGA_AUDIO_HAL_CH_I2S_CH_SOC;
	else if (!strcmp(buf, "uac"))
		return FPGA_AUDIO_HAL_CH_I2S_CH_UAC;
	else if (!strcmp(buf, "bt"))
		return FPGA_AUDIO_HAL_CH_I2S_CH_BT;
	else if (!strcmp(buf, "spdif"))
		return FPGA_AUDIO_HAL_CH_I2S_CH_SPDIF;
	else if (!strcmp(buf, "opt"))
		return FPGA_AUDIO_HAL_CH_I2S_CH_OPT;
	else if (!strcmp(buf, "hdmi"))
		return FPGA_AUDIO_HAL_CH_I2S_CH_HDMI;
	else if (!strcmp(buf, "spdif_bmc"))
		return FPGA_AUDIO_HAL_CH_SPDIF_BMC;
	else if (!strcmp(buf, "opt_bmc"))
		return FPGA_AUDIO_HAL_CH_OPT_BMC;

	return FPGA_AUDIO_HAL_CH_I2S_CH_SOC;
}

static fpga_hal_mclk_direction fpga_hwinit_get_mclk_direction(char *buf)
{
	if (!strcmp(buf, "mclk_out"))
		return MCLK_OUT;
	else if (!strcmp(buf, "mclk_in"))
		return MCLK_IN;

	return MCLK_DISABLE;
}

static fpga_hal_mclk_type fpga_hwinit_get_mclk_type(char *buf)
{
	if (!strcmp(buf, "12M"))
		return MCLK_11_12MHz;
	else if (!strcmp(buf, "24M"))
		return MCLK_22_24MHz;
	else if (!strcmp(buf, "49M"))
		return MCLK_45_49MHz;

	return MCLK_90_98MHz;
}

static fpga_p2d_type fpga_hwinit_get_p2d_type(char *buf)
{
	if (!strcmp(buf, "dsd64"))
		return P2D_DSD64;
	else if (!strcmp(buf, "dsd128"))
		return P2D_DSD128;
	else if (!strcmp(buf, "dsd256"))
		return P2D_DSD256;

	return P2D_DSD512;
}

static ssize_t fpga_hwinit_store(struct device *dev,
				 struct device_attribute *attr, const char *buf, size_t size)
{
	fpga_hal_channel audio_ch;
	fpga_hal_mclk_direction input_mclk_direction;
	fpga_hal_mclk_type input_mclk_type;
	bool dac1_mclk_out_enable;
	fpga_hal_mclk_type dac1_mclk_type;
	bool dac2_mclk_out_enable;
	fpga_hal_mclk_type dac2_mclk_type;
	bool p2d_enable;
	fpga_p2d_type p2d_type;
	bool i2s_in_one_line_data;
	bool dac_mul_line_data;

	int ntokens = 0, i;
	const char *cp = buf;
	char config_buf[11][15] = { 0 };

	while ((cp = strchr(cp + 1, ' ')))
		ntokens++;

	pr_err("%s: buf=%s ntokens=%d\n", __func__, buf, ntokens);

	if (ntokens == 10) {
		sscanf(buf, "%s %s %s %s %s %s %s %s %s %s %s ",
		       config_buf[0], config_buf[1], config_buf[2], config_buf[3],
		       config_buf[4], config_buf[5], config_buf[6], config_buf[7],
		       config_buf[8], config_buf[9], config_buf[10]);

		for (i = 0; i < 11; i++)
			printk("config_buf[%d]=%s\n", i, config_buf[i]);

		audio_ch = fpga_hwinit_get_ch(config_buf[0]);
		input_mclk_direction = fpga_hwinit_get_mclk_direction(config_buf[1]);
		input_mclk_type = fpga_hwinit_get_mclk_type(config_buf[2]);

		if (!strcmp(config_buf[3], "mclk_out"))
			dac1_mclk_out_enable = true;
		else
			dac1_mclk_out_enable = false;
		dac1_mclk_type = fpga_hwinit_get_mclk_type(config_buf[4]);

		if (!strcmp(config_buf[5], "mclk_out"))
			dac2_mclk_out_enable = true;
		else
			dac2_mclk_out_enable = false;
		dac2_mclk_type = fpga_hwinit_get_mclk_type(config_buf[6]);

		if (!strcmp(config_buf[7], "p2d_off"))
			p2d_enable = false;
		else
			p2d_enable = true;
		p2d_type = fpga_hwinit_get_p2d_type(config_buf[8]);

		if (!strcmp(config_buf[9], "one_line_off"))
			i2s_in_one_line_data = false;
		else
			i2s_in_one_line_data = true;

		if (!strcmp(config_buf[10], "mul_line_off"))
			dac_mul_line_data = false;
		else
			dac_mul_line_data = true;

		{
			struct fpga_hwinit_params hwinit_params = {
				.audio_ch = audio_ch,
				.input_mclk_direction = input_mclk_direction,
				.input_mclk_type = input_mclk_type,
				.dac1_mclk_out_enable = dac1_mclk_out_enable,
				.dac1_mclk_type = dac1_mclk_type,
				.dac2_mclk_out_enable = dac2_mclk_out_enable,
				.dac2_mclk_type = dac2_mclk_type,
				.p2d_enable = p2d_enable,
				.p2d_type = p2d_type,
				.i2s_in_one_line_data = i2s_in_one_line_data,
				.dac_mul_line_data = dac_mul_line_data,
			};

			fpga_interface_hal_hwinit(&hwinit_params);
		}
	}

	return size;
}

static ssize_t fpga_config_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	/* stock prints the hwinit fields under the config labels */
	return sprintf(buf, "dsd=%d out_format=%d imclkt=%d dac1_mclk_en=%d dac1_mclk=%d dac2_mclk_en=%d dac2_mclk=%d p2d=%d p2d_t=%d one_line=%d mul_line=%d\n",
		       g_fpga->fpga_device->audio_channel,
		       g_fpga->fpga_device->input_mclk_direction,
		       g_fpga->fpga_device->input_mclk_type,
		       g_fpga->fpga_device->dac1_mclk_out_enable,
		       g_fpga->fpga_device->dac1_mclk_type,
		       g_fpga->fpga_device->dac2_mclk_out_enable,
		       g_fpga->fpga_device->dac2_mclk_type,
		       g_fpga->fpga_device->p2d_enable,
		       g_fpga->fpga_device->p2d_type,
		       g_fpga->fpga_device->soc_use_one_line_data,
		       g_fpga->fpga_device->dac_multiple_lines_data);
}

static fpga_i2s_format fpga_config_get_i2s_format(char *buf)
{
	if (!strcmp(buf, "i2s_p"))
		return FPGA_I2S_FORMAT_P;
	else if (!strcmp(buf, "i2s_l"))
		return FPGA_I2S_FORMAT_L;
	else if (!strcmp(buf, "i2s_r"))
		return FPGA_I2S_FORMAT_R;

	return FPGA_I2S_FORMAT_P;
}

static fpga_i2s_bits fpga_config_get_i2s_bits(char *buf)
{
	if (!strcmp(buf, "16bit"))
		return FPGA_I2S_BIT_16;
	else if (!strcmp(buf, "24bit"))
		return FPGA_I2S_BIT_24;

	return FPGA_I2S_BIT_32;
}

static fpga_i2s_type fpga_config_get_i2s_type(char *buf)
{
	if (!strcmp(buf, "dsd"))
		return FPGA_AUDIO_DSD;
	else if (!strcmp(buf, "pcm"))
		return FPGA_AUDIO_PCM;
	else if (!strcmp(buf, "dop"))
		return FPGA_AUDIO_DOP;

	return FPGA_AUDIO_PCM;
}

static ssize_t fpga_config_store(struct device *dev,
				 struct device_attribute *attr, const char *buf, size_t size)
{
	bool is_dsd;
	int sample_rate;
	fpga_i2s_format i2s_out_format;
	fpga_i2s_bits i2s_out_bits;
	fpga_i2s_format i2s_in_format;
	fpga_i2s_bits i2s_in_bits;
	fpga_i2s_type i2s_in_type;
	bool spdif_tx;
	bool opt_tx;
	bool i2s_clk_out_en;
	bool i2s_data_out_en;

	int ntokens = 0, i;
	const char *cp = buf;
	char config_buf[11][15] = { 0 };

	while ((cp = strchr(cp + 1, ' ')))
		ntokens++;

	pr_err("%s: buf=%s ntokens=%d\n", __func__, buf, ntokens);

	if (ntokens == 10) {
		sscanf(buf, "%s %s %s %s %s %s %s %s %s %s %s ",
		       config_buf[0], config_buf[1], config_buf[2], config_buf[3],
		       config_buf[4], config_buf[5], config_buf[6], config_buf[7],
		       config_buf[8], config_buf[9], config_buf[10]);

		for (i = 0; i < 11; i++)
			printk("config_buf[%d]=%s\n", i, config_buf[i]);

		if (!strcmp(config_buf[0], "dsd"))
			is_dsd = true;
		else
			is_dsd = false;

		sample_rate = simple_strtoul(config_buf[1], NULL, 10);
		i2s_out_format = fpga_config_get_i2s_format(config_buf[2]);
		i2s_out_bits = fpga_config_get_i2s_bits(config_buf[3]);
		i2s_in_format = fpga_config_get_i2s_format(config_buf[4]);
		i2s_in_bits = fpga_config_get_i2s_bits(config_buf[5]);
		i2s_in_type = fpga_config_get_i2s_type(config_buf[6]);

		if (!strcmp(config_buf[7], "spdif_tx_off"))
			spdif_tx = false;
		else
			spdif_tx = true;

		if (!strcmp(config_buf[8], "opt_tx_off"))
			opt_tx = false;
		else
			opt_tx = true;

		if (!strcmp(config_buf[9], "clk_out_en"))
			i2s_clk_out_en = true;
		else
			i2s_clk_out_en = false;

		if (!strcmp(config_buf[10], "data_out_en"))
			i2s_data_out_en = true;
		else
			i2s_data_out_en = false;

		{
			struct fpga_config_params fpga_params = {
				.is_dsd = is_dsd,
				.sample_rate = sample_rate,
				.i2s_out_format = i2s_out_format,
				.i2s_out_bits = i2s_out_bits,
				.i2s_in_format = i2s_in_format,
				.i2s_in_bits = i2s_in_bits,
				.i2s_in_type = i2s_in_type,
				.spdif_tx = spdif_tx,
				.opt_tx = opt_tx,
				.i2s_clk_out_en = i2s_clk_out_en,
				.i2s_data_out_en = i2s_data_out_en,
			};

			fpga_interface_hal_config(&fpga_params);
		}
	}

	return size;
}

static ssize_t fpga_debug_store(struct device *dev,
				struct device_attribute *attr, const char *buf, size_t size)
{
	if (!size)
		return size;

	pr_err("%s, %s \n", __func__, buf);

	if (!strcmp(buf, "hdmi_code_fmt_0\n"))
		fpga_interface_config_hdmi_code_fmt(hdmi_code_fmt_0);
	else if (!strcmp(buf, "hdmi_code_fmt_1\n"))
		fpga_interface_config_hdmi_code_fmt(hdmi_code_fmt_1);
	else if (!strcmp(buf, "hdmi_code_fmt_2\n"))
		fpga_interface_config_hdmi_code_fmt(hdmi_code_fmt_2);
	else if (!strcmp(buf, "hdmi_code_fmt_3\n"))
		fpga_interface_config_hdmi_code_fmt(hdmi_code_fmt_3);
	else
		pr_err("%s error cmd: %s\n", __func__, buf);

	return size;
}

static const struct device_attribute fpga_hal_attrs[] = {
	__ATTR(fpga_power, 0644, fpga_power_show, fpga_power_store),
	__ATTR(fpga_bypass, 0644, fpga_bypass_show, fpga_bypass_store),
	__ATTR(fpga_hwinit, 0644, fpga_hwinit_show, fpga_hwinit_store),
	__ATTR(fpga_config, 0644, fpga_config_show, fpga_config_store),
	__ATTR(fpga_debug, 0200, NULL, fpga_debug_store),
};

static void fpga_load_work(void)
{
	struct fpga_interface_hal_data *data = g_fpga;

	pr_err("%s: Enter!\n", __func__);

	data->fpga_device = fiio_device_get_data_by_name("fpga_device module");
	if (!data->fpga_device)
		pr_err("get fpga_device module failed!\n");
}

static struct fpga_public_data fpga_public_d = {
	.power_by_mode = fpga_ctrl_power_by_mode,
	.hw_params = fpga_hw_params,
	.set_input_channel = fpga_set_input_channel,
	.init_xmos = fpga_interface_init_xmos,
	.fill_clk = fpga_interface_fill_clk,
	.set_mclk = fpga_interface_set_mclk,
	.enter_usb_xmos_mode = fpga_interface_enter_usb_xmos_mode,
	.enter_local_playback_mode = fpga_interface_enter_local_playback_mode,
	.config_xmos_spdif_rx = fpga_interface_config_xmos_spdif_rx,
	.config_hdmi_code_fmt = fpga_interface_config_hdmi_code_fmt,
	.config_spdif_tx = fpga_interface_config_spdif_tx,
	.config_opt_tx = fpga_interface_config_opt_tx,
};

static struct fiio_device_driver fpga_device_driver = {
	.name = "fpga module",
	.private_data = &fpga_public_d,
	.load_work = fpga_load_work,
};

static int fpga_interface_hal_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct fpga_interface_hal_data *data;

	pr_err("[%s] enter ... \n", __func__);

	if (!np)
		return -ENODEV;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	data->fiio_control_fpga_config_notifier.notifier_call = fiio_control_fpga_ctrl_notifier_cb;
	fiio_register_client(notifier_fpga, &data->fiio_control_fpga_config_notifier);
	g_fpga = data;

	ret = fiio_device_attribute_register(fpga_hal_attrs, ARRAY_SIZE(fpga_hal_attrs));
	if (ret) {
		dev_err(dev, "sysfs create error: %d\n", ret);
		goto ERROR;
	}

	data->fpga_public = &fpga_public_d;
	fiio_device_register(&fpga_device_driver);

	pr_err("[%s] exit ... \n", __func__);

	return 0;

ERROR:
	fiio_device_attribute_unregister(fpga_hal_attrs);
	kfree(data);
	return ret;
}

static int fpga_interface_hal_remove(struct platform_device *pdev)
{
	pr_err("[%s] enter ... \n", __func__);

	fiio_device_attribute_unregister(fpga_hal_attrs);
	fiio_device_unregister(&fpga_device_driver);

	return 0;
}

static const struct of_device_id fpga_interface_hal_dt_match[] = {
	{ .compatible = "fiio,fpga-audio-interface-hal" },
	{},
};

static struct platform_driver fpga_interface_hal_driver = {
	.probe = fpga_interface_hal_probe,
	.remove = fpga_interface_hal_remove,
	.driver = {
		.name = "fpga_audio_interface_hal",
		.owner = THIS_MODULE,
		.suppress_bind_attrs = true,
		.of_match_table = fpga_interface_hal_dt_match,
	},
};

static int __init fpga_interface_hal_init(void)
{
	pr_err("[%s] enter ... \n", __func__);

	return platform_driver_register(&fpga_interface_hal_driver);
}

static void __exit fpga_interface_hal_exit(void)
{
	pr_err("[%s] enter ... \n", __func__);

	platform_driver_unregister(&fpga_interface_hal_driver);
}

module_init(fpga_interface_hal_init);
module_exit(fpga_interface_hal_exit);

MODULE_AUTHOR("pengweizhong, pengweizhong@fiio.net");
MODULE_DESCRIPTION("FiiO FPGA Audio Interface hal modules");
MODULE_LICENSE("GPL");
MODULE_ALIAS("fiio:fpga-audio-interface-hal");
