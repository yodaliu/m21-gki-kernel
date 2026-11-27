// SPDX-License-Identifier: GPL-2.0
/*
 * Gowin FPGA audio routing over I2C: input channel, PCM/DSD/SPDIF format and
 * sample rate, DAC MCLK.  Registered as "fpga_device module" for fpga_interface.
 */

#define pr_fmt(fmt) "[gowin_fpga]: " fmt

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <fiio/fiio_notify.h>
#include <fiio/fiio_device.h>
#include <fiio/fiio_i2c.h>

#include "fpga_audio_interface.h"

static struct fpga_data *g_data;

static int fpga_reg[3];

static bool spdif_rx_bypass;

static int fpga_config_spdif_rx(bool enable)
{
	spdif_rx_bypass = enable;
	return 0;
}

static int fpga_i2c_read(u8 reg)
{
	int ret;

	ret = fiio_i2c_smbus_read_byte_data(g_data->client, reg);
	return ret;
}

static int fpga_i2c_write(u8 reg, u8 data)
{
	int ret;

	fpga_reg[reg - 1] = data;
	ret = fiio_i2c_smbus_write_byte_data(g_data->client, reg, data);
	printk("%s, reg = 0x%0.2x, data = 0x%0.2x, ret = %d\n", __func__, reg, data, ret);

	return ret;
}

static int fpga_i2c_update_bits(u8 reg, u8 mask, u8 bits)
{
	unsigned int old, new;
	int ret;

	old = fpga_reg[reg - 1];
	new = (old & ~mask) | (bits & mask);
	if (old == new)
		return 0;

	fpga_reg[reg - 1] = new;
	ret = fiio_i2c_smbus_write_byte_data(g_data->client, reg, new);
	printk("%s, reg = 0x%0.2x, old = 0x%0.2x, mask = 0x%0.2x, bits = 0x%0.2x, new = 0x%0.2x\n",
	       __func__, reg, old, mask, bits, new);

	return ret;
}

int fpga_dac_mclk;

static void fpga_set_mclk(enum fpga_dac_mclk_t mclk_sel)
{
	if (mclk_sel == AUDIO_DAC_MCLK_90) {
		printk("%s set dac_mclk 90/98MHz\n", __func__);
		fpga_dac_mclk = 0x80;
		fpga_i2c_update_bits(0x03, 0xe0, 0x80);
	} else if (mclk_sel == AUDIO_DAC_MCLK_45) {
		printk("%s set dac_mclk 45/49MHz\n", __func__);
		fpga_dac_mclk = 0x40;
		fpga_i2c_update_bits(0x03, 0xe0, 0x40);
	} else if (mclk_sel == AUDIO_DAC_MCLK_22) {
		printk("%s set dac_mclk 22/24MHz\n", __func__);
		fpga_dac_mclk = 0x20;
		fpga_i2c_update_bits(0x03, 0xe0, 0x20);
	}
}

static bool fill_clk_enable;

static void fpga_fill_clk(bool enable)
{
	printk("%s enable = %d\n", __func__, enable);
	if (enable)
		fpga_i2c_update_bits(0x01, 0x01, 0x01);
	else
		fpga_i2c_update_bits(0x01, 0x01, 0x00);
	fill_clk_enable = enable;
}

static bool spdif_tx_enable;

static int fpga_config_spdif_tx(bool enable)
{
	pr_err(" %s enable = %d\n", __func__, enable);
	if (enable)
		fpga_i2c_update_bits(0x01, 0x40, 0x40);
	else
		fpga_i2c_update_bits(0x01, 0x40, 0x00);
	spdif_tx_enable = enable;
	return 0;
}

static bool xmos_spdif_rx;

static void fpga_config_xmos_spdif_rx(bool enable)
{
	printk("%s xmos_spdif_rx = %d\n", __func__, enable);
	xmos_spdif_rx = enable;
}

static void fpga_config_hdmi_code_fmt(enum fpga_hdmi_code_fmt fmt)
{
	printk("%s hdmi_code_fmt = %d\n", __func__, fmt);
	/* stock writes a register past fpga_reg[]: the build traps here (UBSAN bounds) */
	fpga_i2c_update_bits(0x04, 0x03, fmt);
}

static int fpga_set_pcm_mode(enum fpga_fmt_t fmt, int rate, int bits)
{
	u8 reg, data;

	printk("AUDIO_LINK: pcm mode, rate %d, bits %d\n", rate, bits);

	if (bits != 16 && bits != 24 && bits != 32) {
		printk("AUDIO_LINK: not surpport %d bits\n", bits);
		return -1;
	}

	reg = 0x01;
	data = 0x16;
	if (spdif_rx_bypass)
		data |= 0x80;
	data |= fill_clk_enable;
	if (spdif_tx_enable)
		data |= 0x40;
	if (xmos_spdif_rx)
		data ^= 0x80;
	fpga_i2c_write(reg, data);

	reg = 0x02;
	switch (rate) {
	case 44100:
		data = 0x41;
		break;
	case 48000:
		data = 0x81;
		break;
	case 88200:
		data = 0x42;
		break;
	case 96000:
		data = 0x82;
		break;
	case 176400:
		data = 0x44;
		break;
	case 192000:
		data = 0x84;
		break;
	case 352800:
		data = 0x48;
		break;
	case 384000:
		data = 0x88;
		break;
	case 705600:
		data = 0x50;
		break;
	case 768000:
		data = 0x90;
		break;
	default:
		printk("AUDIO_LINK: pcm mode don't surpport rate %d\n", rate);
		return -2;
	}
	fpga_i2c_write(reg, data);

	return 0;
}

static int fpga_set_spdif_tx_mode(enum fpga_fmt_t fmt, int rate, int bits)
{
	u8 data, reg;

	switch (fmt) {
	case FMT_SPDIF:
		printk("AUDIO_LINK: spdif pcm mode, rate %d, bits %d\n", rate, bits);
		data = 0x46;
		break;
	case FMT_SPDIFDOP:
		printk("AUDIO_LINK: spdif dop mode, rate %d, bits %d\n", rate, bits);
		data = 0x4e;
		break;
	case FMT_SPDIF_MUTE:
		printk("AUDIO_LINK: spdif pcm mute mode, rate %d, bits %d\n", rate, bits);
		data = 0x40;
		break;
	case FMT_SPDIFDOP_MUTE:
	default:
		printk("AUDIO_LINK: spdif dop mute mode, rate %d, bits %d\n", rate, bits);
		data = 0x48;
		break;
	}
	reg = 0x01;
	fpga_i2c_write(reg, data);

	reg = 0x02;
	switch (rate) {
	case 44100:
		data = 0x41;
		break;
	case 48000:
		data = 0x81;
		break;
	case 88200:
		data = 0x42;
		break;
	case 96000:
		data = 0x82;
		break;
	case 176400:
		data = 0x44;
		break;
	case 192000:
		data = 0x84;
		break;
	case 352800:
		data = 0x48;
		break;
	case 384000:
		data = 0x88;
		break;
	default:
		printk("AUDIO_LINK: spdif mode don't surpport rate %d\n", rate);
		return -2;
	}
	fpga_i2c_write(reg, data);

	return 0;
}

static int fpga_set_dsd_mode(enum fpga_fmt_t fmt, int rate, int bits)
{
	u8 reg, data;

	printk("AUDIO_LINK: dsd mode, rate %d, bits %d\n", rate, bits);

	reg = 0x01;
	data = 0x2e;
	if (spdif_rx_bypass)
		data |= 0x80;
	data |= fill_clk_enable;
	if (spdif_tx_enable)
		data |= 0x40;
	fpga_i2c_write(reg, data);

	reg = 0x02;
	switch (rate) {
	case 88200:
		data = 0x42;
		break;
	case 96000:
		data = 0x82;
		break;
	case 176400:
		data = 0x44;
		break;
	case 192000:
		data = 0x84;
		break;
	case 352800:
		data = 0x68;
		break;
	case 384000:
		data = 0xa8;
		break;
	case 705600:
		data = 0x70;
		break;
	case 768000:
		data = 0xb0;
		break;
	default:
		printk("AUDIO_LINK: dsd mode don't surpport rate %d\n", rate);
		return -2;
	}
	fpga_i2c_write(reg, data);

	return 0;
}

static int fpga_config(struct fpga_component *fpga_device, enum fpga_fmt_t fmt, int rate, int bits)
{
	int ret = 0;

	switch (fmt) {
	case FMT_PCM:
		ret = fpga_set_pcm_mode(fmt, rate, bits);
		break;
	case FMT_DSD:
		ret = fpga_set_dsd_mode(fmt, rate, bits);
		break;
	case FMT_SPDIF:
	case FMT_SPDIFDOP:
	case FMT_SPDIF_MUTE:
	case FMT_SPDIFDOP_MUTE:
		ret = fpga_set_spdif_tx_mode(fmt, rate, bits);
		break;
	default:
		printk("AUDIO_LINK: not surpport format %d\n", fmt);
		ret = -1;
		break;
	}

	return ret;
}

static void fpga_config_full(struct fpga_component *fpga_device, struct fpga_config_params *fpga_params)
{
	int config_bits;

	if (fpga_params->spdif_tx || fpga_params->opt_tx)
		spdif_rx_bypass = false;
	else
		spdif_rx_bypass = true;

	if (fpga_params->i2s_out_bits == FPGA_I2S_BIT_16)
		config_bits = 16;
	else if (fpga_params->i2s_out_bits == FPGA_I2S_BIT_24)
		config_bits = 24;
	else
		config_bits = 32;

	fpga_config(fpga_device, fpga_params->is_dsd ? FMT_DSD : FMT_PCM,
		    fpga_params->sample_rate, config_bits);
}

static int fpga_set_inputchannel(enum fpga_channel chl)
{
	u8 reg, data;

	switch (chl) {
	case fpgachl_soc:
		reg = 0x03;
		data = fpga_dac_mclk + 0x01;
		printk("AUDIO_LINK: set input channel to soc\n");
		break;
	case fpgachl_xmos:
		reg = 0x03;
		data = fpga_dac_mclk + 0x02;
		printk("AUDIO_LINK: set input channel to xmos\n");
		break;
	case fpgachl_bt:
		reg = 0x03;
		data = fpga_dac_mclk + 0x04;
		printk("AUDIO_LINK: set input channel to bt\n");
		break;
	default:
		printk("AUDIO_LINK: wrong input channel\n");
		return -1;
	}
	fpga_i2c_write(reg, data);

	return 0;
}

static void fpga_init_xmos(void)
{
	uint64_t cmd = 0;

	(void)cmd;
	fpga_set_inputchannel(fpgachl_xmos);
	fpga_set_pcm_mode(FMT_PCM, 44100, 32);
}

static int fiio_control_fpga_ctrl_notifier_cb(struct notifier_block *self,
					      unsigned long event, void *data)
{
	struct fpga_data *fpga_data = container_of(self, struct fpga_data,
						   fiio_control_fpga_config_notifier);
	struct fiio_control_fpga_ctrl_notify_data *notify_data = data;
	int value;
	int i;

	(void)fpga_data;
	printk("%s ++++++++++000++++++ \n", __func__);

	if (notify_data && notify_data->event == FIIO_CONFIG_FPGA_CONTRL_READ_REG) {
		printk("%s ++++++++++++++++ \n", __func__);
		if (notify_data->reg == 0xff) {
			for (i = 0; i < 10; i++) {
				value = fpga_i2c_read(i);
				printk("%s:reg=0x%02x value=0x%02x\n", __func__, i, value);
			}
		} else {
			value = fpga_i2c_read(notify_data->reg);
			printk("%s:reg=0x%02x value=0x%02x\n", __func__, notify_data->reg, value);
		}
	}

	return 0;
}

static struct fpga_component fpga_public_d = {
	.name = "gw fpga",
	.ctrl_power = fpga_ctrl_power,
	.fpga_config = fpga_config,
	.fpga_config_full = fpga_config_full,
	.power_by_mode = fpga_ctrl_power_by_mode,
	.set_input_channel = fpga_set_inputchannel,
	.init_xmos = fpga_init_xmos,
	.fill_clk = fpga_fill_clk,
	.set_mclk = fpga_set_mclk,
	.enter_usb_xmos_mode = fpga_enter_usb_xmos_mode,
	.enter_local_playback_mode = fpga_enter_local_playback_mode,
	.config_xmos_spdif_rx = fpga_config_xmos_spdif_rx,
	.config_hdmi_code_fmt = fpga_config_hdmi_code_fmt,
	.config_spdif_rx = fpga_config_spdif_rx,
	.config_spdif_tx = fpga_config_spdif_tx,
};

static struct fiio_device_driver fpga_device_driver = {
	.name = "fpga_device module",
	.private_data = &fpga_public_d,
};

static int fpga_probe(struct i2c_client *client,
		      const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *np = dev->of_node;
	struct fpga_data *data;
	int ret = 0;

	printk("%s enter !!!\n", __func__);

	if (!np)
		return -ENODEV;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	data->dev = dev;
	g_data = data;

	data->fiio_control_fpga_config_notifier.notifier_call = fiio_control_fpga_ctrl_notifier_cb;
	fiio_register_client(notifier_fpga, &data->fiio_control_fpga_config_notifier);

	fiio_device_register(&fpga_device_driver);

	printk("%s finished !!!\n", __func__);

	return ret;
}

static int fpga_remove(struct i2c_client *client)
{
	fiio_device_unregister(&fpga_device_driver);
	return 0;
}

static const struct i2c_device_id fpga_id[] = {
	{ "fpga", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fpga_id);

static const struct of_device_id of_fpga_leds_match[] = {
	{ .compatible = "gowin,fpga_i2c", },
	{},
};
MODULE_DEVICE_TABLE(of, of_fpga_leds_match);

static struct i2c_driver fpga_driver = {
	.driver = {
		.name = "fpga_audio",
		.of_match_table = of_fpga_leds_match,
	},
	.probe = fpga_probe,
	.remove = fpga_remove,
	.id_table = fpga_id,
};

static int __init fpga_init(void)
{
	return i2c_add_driver(&fpga_driver);
}

static void __exit fpga_exit(void)
{
	i2c_del_driver(&fpga_driver);
}

module_init(fpga_init);
module_exit(fpga_exit);

MODULE_DESCRIPTION("FPGA AUDIO Driver");
MODULE_AUTHOR("mxc");
MODULE_LICENSE("GPL");
