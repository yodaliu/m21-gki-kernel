// SPDX-License-Identifier: GPL-2.0
/* GPIO helpers and writes that go to every DAC of a snd_dac_dai. */
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/i2c.h>

#include <fiio/dac_dai.h>
#include <fiio/fiio_i2c.h>

int dac_get_gpio_value(int gpio)
{
	if (!gpio_is_valid(gpio)) {
		printk("%s: error, no gpio %d!\n", __func__, gpio);
		return 0;
	}

	return gpio_get_value_cansleep(gpio);
}
EXPORT_SYMBOL(dac_get_gpio_value);

void dac_set_gpio_value(int gpio, int value)
{
	if (!gpio_is_valid(gpio)) {
		printk("%s: error, no gpio %d!\n", __func__, gpio);
		return;
	}

	gpio_direction_output(gpio, value);
}
EXPORT_SYMBOL(dac_set_gpio_value);

void dac_set_gpio_value_cansleep(int gpio, int value)
{
	if (!gpio_is_valid(gpio)) {
		printk("%s: error, no gpio %d!\n", __func__, gpio);
		return;
	}

	gpio_set_value_cansleep(gpio, value);
}
EXPORT_SYMBOL(dac_set_gpio_value_cansleep);

int dac_dual_client_write(struct snd_dac_dai *dac_dai, u8 reg, u8 data)
{
	int ret = 0;

	if (!dac_dai) {
		printk("[%s] error: dac_dai is NULL\n", __func__);
		return -1;
	}

	if (dac_dai->client_l) {
		ret = fiio_i2c_smbus_write_byte_data(dac_dai->client_l, reg, data);
		if (ret < 0) {
			printk("[%s] write left dac error\n", __func__);
			return -1;
		}
	} else {
		printk("[%s] client_l is not ready!!!\n", __func__);
	}

	if (dac_dai->client_r) {
		ret = fiio_i2c_smbus_write_byte_data(dac_dai->client_r, reg, data);
		if (ret < 0) {
			printk("[%s] write right dac error\n", __func__);
			return -1;
		}
	} else {
		printk("[%s] client_r is not ready!!!\n", __func__);
	}

	if (dac_dai->client_3) {
		ret = fiio_i2c_smbus_write_byte_data(dac_dai->client_3, reg, data);
		if (ret < 0) {
			printk("[%s] write dac_3 error\n", __func__);
			return -1;
		}
	}

	if (dac_dai->client_4) {
		ret = fiio_i2c_smbus_write_byte_data(dac_dai->client_4, reg, data);
		if (ret < 0) {
			printk("[%s] write dac_4 error\n", __func__);
			return -1;
		}
	}

	return ret;
}
EXPORT_SYMBOL(dac_dual_client_write);

int dac_dual_regmap_write(struct snd_dac_dai *dac_dai, unsigned int reg, unsigned int val)
{
	int ret = 0;

	if (!dac_dai) {
		printk("[%s] error: dac_dai is NULL\n", __func__);
		return -1;
	}

	if (dac_dai->regmap_l) {
		ret = fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, reg, val);
		if (ret < 0) {
			printk("[%s] write left dac error\n", __func__);
			return -1;
		}
	} else {
		printk("[%s] regmap_l is not ready!!!\n", __func__);
	}

	if (dac_dai->regmap_r) {
		ret = fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, reg, val);
		if (ret < 0) {
			printk("[%s] write right dac error\n", __func__);
			return -1;
		}
	} else {
		printk("[%s] regmap_r is not ready!!!\n", __func__);
	}

	if (dac_dai->regmap_3) {
		ret = fiio_regmap_write(dac_dai->name, dac_dai->regmap_3, reg, val);
		if (ret < 0) {
			printk("[%s] write dac_3 error\n", __func__);
			return -1;
		}
	}

	if (dac_dai->regmap_4) {
		ret = fiio_regmap_write(dac_dai->name, dac_dai->regmap_4, reg, val);
		if (ret < 0) {
			printk("[%s] write dac_4 error\n", __func__);
			return -1;
		}
	}

	return ret;
}
EXPORT_SYMBOL(dac_dual_regmap_write);

int dac_dual_regmap_write_delay(struct snd_dac_dai *dac_dai, unsigned int reg,
				unsigned int val, unsigned int delay)
{
	int ret = 0;

	if (!dac_dai) {
		printk("[%s] error: dac_dai is NULL\n", __func__);
		return -1;
	}

	if (dac_dai->regmap_l) {
		ret = fiio_regmap_write(dac_dai->name, dac_dai->regmap_l, reg, val);
		if (ret < 0) {
			printk("[%s] write left dac error\n", __func__);
			return -1;
		}
		mdelay(delay);
	} else {
		printk("[%s] regmap_l is not ready!!!\n", __func__);
	}

	if (dac_dai->regmap_r) {
		ret = fiio_regmap_write(dac_dai->name, dac_dai->regmap_r, reg, val);
		if (ret < 0) {
			printk("[%s] write right dac error\n", __func__);
			return -1;
		}
		mdelay(delay);
	} else {
		printk("[%s] regmap_r is not ready!!!\n", __func__);
	}

	if (dac_dai->regmap_3) {
		ret = fiio_regmap_write(dac_dai->name, dac_dai->regmap_3, reg, val);
		if (ret < 0) {
			printk("[%s] write dac_3 error\n", __func__);
			return -1;
		}
		mdelay(delay);
	}

	if (dac_dai->regmap_4) {
		ret = fiio_regmap_write(dac_dai->name, dac_dai->regmap_4, reg, val);
		if (ret < 0) {
			printk("[%s] write dac_4 error\n", __func__);
			return -1;
		}
		mdelay(delay);
	}

	return ret;
}
EXPORT_SYMBOL(dac_dual_regmap_write_delay);

int dac_dual_regmap_update_bits(struct snd_dac_dai *dac_dai, unsigned int reg,
				unsigned int mask, unsigned int val)
{
	int ret = 0;

	if (!dac_dai) {
		printk("[%s] error: dac_dai is NULL\n", __func__);
		return -1;
	}

	if (dac_dai->regmap_l) {
		ret = fiio_regmap_update_bits(dac_dai->name, dac_dai->regmap_l, reg, mask, val);
		if (ret < 0) {
			printk("[%s] write left dac error\n", __func__);
			return -1;
		}
	} else {
		printk("[%s] regmap_l is not ready!!!\n", __func__);
	}

	if (dac_dai->regmap_r) {
		ret = fiio_regmap_update_bits(dac_dai->name, dac_dai->regmap_r, reg, mask, val);
		if (ret < 0) {
			printk("[%s] write right dac error\n", __func__);
			return -1;
		}
	} else {
		printk("[%s] regmap_r is not ready!!!\n", __func__);
	}

	if (dac_dai->regmap_3) {
		ret = fiio_regmap_update_bits(dac_dai->name, dac_dai->regmap_3, reg, mask, val);
		if (ret < 0) {
			printk("[%s] write dac_3 error\n", __func__);
			return -1;
		}
	}

	if (dac_dai->regmap_4) {
		ret = fiio_regmap_update_bits(dac_dai->name, dac_dai->regmap_4, reg, mask, val);
		if (ret < 0) {
			printk("[%s] write dac_4 error\n", __func__);
			return -1;
		}
	}

	return ret;
}
EXPORT_SYMBOL(dac_dual_regmap_update_bits);

int dac_dual_regmap_multi_reg_write(struct snd_dac_dai *dac_dai,
				    const struct reg_sequence *regs, int num_regs)
{
	int ret = 0;

	if (!dac_dai) {
		printk("[%s] error: dac_dai is NULL\n", __func__);
		return -1;
	}

	if (dac_dai->regmap_l) {
		ret = regmap_multi_reg_write(dac_dai->regmap_l, regs, num_regs);
		if (ret < 0) {
			printk("[%s] write left dac error\n", __func__);
			return -1;
		}
	} else {
		printk("[%s] regmap_l is not ready!!!\n", __func__);
	}

	if (dac_dai->regmap_r) {
		ret = regmap_multi_reg_write(dac_dai->regmap_r, regs, num_regs);
		if (ret < 0) {
			printk("[%s] write right dac error\n", __func__);
			return -1;
		}
	} else {
		printk("[%s] regmap_r is not ready!!!\n", __func__);
	}

	if (dac_dai->regmap_3) {
		ret = regmap_multi_reg_write(dac_dai->regmap_3, regs, num_regs);
		if (ret < 0) {
			printk("[%s] write dac_3 error\n", __func__);
			return -1;
		}
	}

	if (dac_dai->regmap_4) {
		ret = regmap_multi_reg_write(dac_dai->regmap_4, regs, num_regs);
		if (ret < 0) {
			printk("[%s] write dac_4 error\n", __func__);
			return -1;
		}
	}

	return ret;
}
EXPORT_SYMBOL(dac_dual_regmap_multi_reg_write);
