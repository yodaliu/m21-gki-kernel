// SPDX-License-Identifier: GPL-2.0
/*
 * Awinic AW95016 16-bit I2C GPIO expander (stock drivers/input/misc/aw95016.c,
 * reconstructed from the FW 1.1.0 binary; struct layouts follow the camera.ko
 * DWARF copy of this driver, cam_aw95016/aw95016.c).
 *
 * The two chips (chip_index 2 = ex2, otherwise ex3) register their pins with
 * gpiolib-quectel and expose debugfs test files in aw95016_ex2 / aw95016_ex3.
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/gpiolib_quectel_kernel.h>

#define AW95016_DRIVER_VERSION		"V0.1.0"

#define AW95016_REG_INPUT_P0		0x00
#define AW95016_REG_INPUT_P1		0x01
#define AW95016_REG_OUTPUT_P0		0x02
#define AW95016_REG_OUTPUT_P1		0x03
#define AW95016_REG_CONFIG_P0		0x06
#define AW95016_REG_CONFIG_P1		0x07
#define AW95016_REG_DSR_P0		0x08
#define AW95016_REG_DSR1_P0		0x0a
#define AW95016_REG_PULL_EN_P0		0x0e
#define AW95016_REG_PULL_SEL_P0		0x10
#define AW95016_REG_INT_MASK_P0		0x12
#define AW95016_REG_INT_MASK_P1		0x13
#define AW95016_REG_INT_STATUS_P0	0x14
#define AW95016_REG_DOMD_P0		0x16
#define AW95016_REG_DOMD_P1		0x17
#define AW95016_REG_CHIPID		0x70
#define AW95016_REG_RESET		0x70

#define AW95016_CHIPID			0x80
#define AW95016_GPIO_NUM_MAX		16

enum aw95016_reset_flags {
	AW95016_SOFT_RESET,
	AW95016_HARD_RESET,
};

enum aw95016_in_output {
	AW95016_INPUT,
	AW95016_OUTPUT,
};

struct aw95016_gpio_irq_queue {
	unsigned int gpio_id;
	struct work_struct work;
	irq_func func;
	void *data;
};

struct gpio_container {
	unsigned int idx;
	unsigned int direction;
	unsigned int out_status;
	unsigned int out_level;
	unsigned int io_strength;
	unsigned int input_status;
	unsigned int input_en;
	unsigned int input_level;
};

struct aw95016 {
	struct i2c_client *client;
	struct device *dev;
	int irq_gpio;
	int irq_num;
	int rst_gpio;
	int gpio_num;
	unsigned char chipid;
	unsigned int gpio_mode;
	struct gpio_container *gpio;
	unsigned int reg;
	unsigned int chip_index;
	struct mutex mutex_lock;
	struct workqueue_struct *gpio_irq_workqueue;
	struct aw95016_gpio_irq_queue gpio_irq_queue[AW95016_GPIO_NUM_MAX];
	bool irq_wake_enable;
};

static int output_test_gpio = -1;
static struct work_struct test_work;
static struct workqueue_struct *test_workqueue;
static struct aw95016 *g_aw95016[4];
bool g_isInit;

static unsigned int i2c_write_byte(struct i2c_client *client,
				   unsigned char reg_addr, unsigned char reg_data)
{
	int ret;
	unsigned char buf[2] = {0};
	struct i2c_msg msg = {
		.addr = client->addr,
		.flags = 0,
		.len = 2,
		.buf = buf,
	};

	if (!client) {
		pr_err("msg %s i2c client is NULL\n", __func__);
		return -ENODEV;
	}

	buf[0] = reg_addr;
	buf[1] = reg_data;
	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0)
		pr_err("msg %s i2c write error: %d\n", __func__, ret);

	return ret;
}

static unsigned int i2c_read_byte(struct i2c_client *client,
				  unsigned char reg_addr, unsigned char *val)
{
	int ret;
	unsigned char buf[2] = {0};
	struct i2c_msg msgs[2] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 1,
			.buf = &reg_addr,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = buf,
		},
	};

	if (!client) {
		pr_err("msg %s i2c client is NULL\n", __func__);
		return -ENODEV;
	}

	ret = i2c_transfer(client->adapter, msgs, 2);
	if (ret < 0)
		pr_err("msg %s i2c read error: %d\n", __func__, ret);
	*val = buf[0];

	return ret;
}

static int i2c_write_multi_byte(struct i2c_client *client,
				unsigned char reg_addr, unsigned char *buf,
				unsigned int len)
{
	int ret;
	unsigned char *ptr;

	ptr = kmalloc(len + 1, GFP_KERNEL);
	if (!ptr)
		return -ENOMEM;

	ptr[0] = reg_addr;
	memcpy(ptr + 1, buf, len);
	ret = i2c_master_send(client, ptr, len + 1);
	if (ret < 0)
		pr_err("%s: i2c master send error\n", __func__);
	else
		ret = 0;
	kfree(ptr);

	return ret;
}

static unsigned int i2c_read_multi_byte(struct i2c_client *client,
					unsigned char reg_addr,
					unsigned char *buf, unsigned int len)
{
	int ret;
	unsigned char *data;
	struct i2c_msg msgs[2] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 1,
			.buf = &reg_addr,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = len,
		},
	};

	data = kmalloc(len, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	msgs[1].buf = data;

	if (!client) {
		pr_err("msg %s i2c client is NULL\n", __func__);
		return -ENODEV;
	}

	ret = i2c_transfer(client->adapter, msgs, 2);
	if (ret < 0)
		pr_err("msg %s i2c read error: %d\n", __func__, ret);
	if (buf)
		memcpy(buf, data, len);
	kfree(data);

	return ret;
}

/*
 * The per-pin loops below test the pin's idx but shift by the loop index, as
 * the stock driver does.
 */
static void aw95016_enbale_gpio_direction(struct aw95016 *aw95016)
{
	unsigned char reg_val[2] = {0};
	int i;

	i2c_read_multi_byte(aw95016->client, AW95016_REG_CONFIG_P0, reg_val, 2);
	for (i = 0; i < aw95016->gpio_num; i++) {
		if (aw95016->gpio[i].idx < 8) {
			if (aw95016->gpio[i].direction == 1)
				reg_val[0] |= 1 << i;
			else
				reg_val[0] &= ~(1 << i);
		} else {
			if (aw95016->gpio[i].direction == 1)
				reg_val[1] |= 1 << (i - 8);
			else
				reg_val[1] &= ~(1 << (i - 8));
		}
	}
	i2c_write_multi_byte(aw95016->client, AW95016_REG_CONFIG_P0, reg_val, 2);
}

static void aw95016_set_input_state(struct aw95016 *aw95016)
{
	unsigned char reg_val[2] = {0};
	int i;

	i2c_read_multi_byte(aw95016->client, AW95016_REG_INPUT_P0, reg_val, 2);
	for (i = 0; i < aw95016->gpio_num; i++) {
		if (aw95016->gpio[i].idx < 8) {
			if (aw95016->gpio[i].input_status == 1)
				reg_val[0] |= 1 << i;
			else
				reg_val[0] &= ~(1 << i);
		} else {
			if (aw95016->gpio[i].input_status == 1)
				reg_val[1] |= 1 << (i - 8);
			else
				reg_val[1] &= ~(1 << (i - 8));
		}
	}
	i2c_write_multi_byte(aw95016->client, AW95016_REG_INPUT_P0, reg_val, 2);
}

static void aw95016_set_output_state(struct aw95016 *aw95016)
{
	unsigned char reg_val[2] = {0};
	int i;

	i2c_read_multi_byte(aw95016->client, AW95016_REG_OUTPUT_P0, reg_val, 2);
	for (i = 0; i < aw95016->gpio_num; i++) {
		if (aw95016->gpio[i].idx < 8) {
			if (aw95016->gpio[i].out_level == 1)
				reg_val[0] |= 1 << i;
			else
				reg_val[0] &= ~(1 << i);
		} else {
			if (aw95016->gpio[i].out_level == 1)
				reg_val[1] |= 1 << (i - 8);
			else
				reg_val[1] &= ~(1 << (i - 8));
		}
	}
	i2c_write_multi_byte(aw95016->client, AW95016_REG_OUTPUT_P0, reg_val, 2);
}

static void aw95016_set_output_mode(struct aw95016 *aw95016)
{
	unsigned char reg_val[2] = {0};
	int i;

	i2c_read_multi_byte(aw95016->client, AW95016_REG_DOMD_P0, reg_val, 2);
	for (i = 0; i < aw95016->gpio_num; i++) {
		if (aw95016->gpio[i].idx < 8) {
			if (aw95016->gpio[i].out_status == 1)
				reg_val[0] |= 1 << i;
			else
				reg_val[0] &= ~(1 << i);
		} else {
			if (aw95016->gpio[i].out_status == 1)
				reg_val[1] |= 1 << (i - 8);
			else
				reg_val[1] &= ~(1 << (i - 8));
		}
	}
	i2c_write_multi_byte(aw95016->client, AW95016_REG_DOMD_P0, reg_val, 2);
}

static void aw95016_set_gpio_driver_strength(struct aw95016 *aw95016)
{
	unsigned char reg_val[4] = {0};
	int i;

	i2c_read_multi_byte(aw95016->client, AW95016_REG_DSR_P0, reg_val, 4);
	for (i = 0; i < aw95016->gpio_num; i++) {
		if (aw95016->gpio[i].idx < 8) {
			if (aw95016->gpio[i].io_strength == 1)
				reg_val[0] |= 1 << i;
			else
				reg_val[0] &= ~(1 << i);
		} else {
			if (aw95016->gpio[i].io_strength == 1)
				reg_val[1] |= 1 << (i - 8);
			else
				reg_val[1] &= ~(1 << (i - 8));
		}
	}
	i2c_write_multi_byte(aw95016->client, AW95016_REG_DSR_P0, reg_val, 4);
}

static void aw95016_enable_pull_resistance(struct aw95016 *aw95016)
{
	unsigned char reg_val[2] = {0};
	int i;

	i2c_read_multi_byte(aw95016->client, AW95016_REG_PULL_EN_P0, reg_val, 2);
	for (i = 0; i < aw95016->gpio_num; i++) {
		if (aw95016->gpio[i].idx < 8) {
			if (aw95016->gpio[i].input_en == 1)
				reg_val[0] |= 1 << i;
			else
				reg_val[0] &= ~(1 << i);
		} else {
			if (aw95016->gpio[i].input_en == 1)
				reg_val[1] |= 1 << (i - 8);
			else
				reg_val[1] &= ~(1 << (i - 8));
		}
	}
	i2c_write_multi_byte(aw95016->client, AW95016_REG_PULL_EN_P0, reg_val, 2);
}

static void aw95016_set_pull_resistance(struct aw95016 *aw95016)
{
	unsigned char reg_val[2] = {0};
	int i;

	i2c_read_multi_byte(aw95016->client, AW95016_REG_PULL_SEL_P0, reg_val, 2);
	for (i = 0; i < aw95016->gpio_num; i++) {
		if (aw95016->gpio[i].idx < 8) {
			if (aw95016->gpio[i].input_level == 1)
				reg_val[0] |= 1 << i;
			else
				reg_val[0] &= ~(1 << i);
		} else {
			if (aw95016->gpio[i].input_level == 1)
				reg_val[1] |= 1 << (i - 8);
			else
				reg_val[1] &= ~(1 << (i - 8));
		}
	}
	i2c_write_multi_byte(aw95016->client, AW95016_REG_PULL_SEL_P0, reg_val, 2);
}

static void aw95016_reset(struct aw95016 *aw95016, int flag)
{
	if (flag == AW95016_SOFT_RESET) {
		i2c_write_byte(aw95016->client, AW95016_REG_RESET, 0x00);
		mdelay(6);
	}
}

static int aw95016_read_chipid(struct i2c_client *client)
{
	unsigned char val = 0;
	unsigned char cnt = 5;

	do {
		i2c_read_byte(client, AW95016_REG_CHIPID, &val);
		pr_info("aw95016c_read_chipid val=0x%x\n", val);
		if (val == AW95016_CHIPID)
			return 0;
		mdelay(5);
	} while (--cnt);

	return -EINVAL;
}

static void aw95016_gpio_mode_init(struct aw95016 *aw95016)
{
	if (aw95016->gpio_mode == AW95016_INPUT) {
		aw95016_enbale_gpio_direction(aw95016);
		aw95016_set_input_state(aw95016);
		aw95016_enable_pull_resistance(aw95016);
		aw95016_set_pull_resistance(aw95016);
		pr_info("%s: gpio_mode set gpio_input successful \n", __func__);
	} else if (aw95016->gpio_mode == AW95016_OUTPUT) {
		aw95016_enbale_gpio_direction(aw95016);
		aw95016_set_output_state(aw95016);
		aw95016_set_output_mode(aw95016);
		aw95016_set_gpio_driver_strength(aw95016);
		pr_info("%s: gpio_mode set gpio_output successful \n", __func__);
	}
}

ssize_t extern_gpio_config(unsigned int chipId, unsigned int gpio,
			   unsigned int direction, unsigned int out_status,
			   unsigned int out_level, unsigned int io_strength,
			   unsigned int input_status, unsigned int input_en,
			   unsigned int input_level)
{
	struct aw95016 *aw95016;

	if (!g_isInit)
		pr_err("%s user befor init\n", __func__);

	if (chipId >= 2)
		return -1;
	aw95016 = g_aw95016[chipId];
	if (!aw95016)
		return -1;

	pr_err("msg %s*********** extern_gpio_config\n", __func__);
	pr_info("%s:CCCCCCCCC\n", __func__);
	aw95016->gpio[gpio].idx = gpio;
	aw95016->gpio[gpio].direction = direction;
	aw95016->gpio[gpio].out_status = out_status;
	aw95016->gpio[gpio].out_level = out_level;
	aw95016->gpio[gpio].io_strength = io_strength;
	aw95016->gpio[gpio].input_status = input_status;
	aw95016->gpio[gpio].input_en = input_en;
	aw95016->gpio[gpio].input_level = input_level;

	if (direction == AW95016_INPUT) {
		aw95016_enbale_gpio_direction(aw95016);
		aw95016_set_input_state(aw95016);
		aw95016_enable_pull_resistance(aw95016);
		aw95016_set_pull_resistance(aw95016);
	} else if (direction == AW95016_OUTPUT) {
		aw95016_enbale_gpio_direction(aw95016);
		aw95016_set_output_state(aw95016);
		aw95016_set_output_mode(aw95016);
		aw95016_set_gpio_driver_strength(aw95016);
	}

	return 0;
}
EXPORT_SYMBOL(extern_gpio_config);

static void aw95016_gpio_irq_work_queue(struct work_struct *work)
{
	struct aw95016_gpio_irq_queue *gpio_irq_queue =
		container_of(work, struct aw95016_gpio_irq_queue, work);

#line 603
	pr_err("%s:%d gpio %d irq queue E\n", __func__, __LINE__, gpio_irq_queue->gpio_id);
	if (gpio_irq_queue->func)
		gpio_irq_queue->func(gpio_irq_queue->gpio_id, gpio_irq_queue->data);
}

static int aw95016_chip_set_gpio_direction(struct aw95016 *aw95016,
					   unsigned int gpio, int dir)
{
	int ret;
	unsigned char reg_val[2] = {0};

	ret = i2c_read_multi_byte(aw95016->client, AW95016_REG_CONFIG_P0, reg_val, 2);
	if (ret < 0)
		return ret;

	if (dir) {
		if (gpio < 8)
			reg_val[0] |= 1 << gpio;
		else
			reg_val[1] |= 1 << (gpio - 8);
	} else {
		if (gpio < 8)
			reg_val[0] &= ~(1 << gpio);
		else
			reg_val[1] &= ~(1 << (gpio - 8));
	}

	return i2c_write_multi_byte(aw95016->client, AW95016_REG_CONFIG_P0, reg_val, 2);
}

static int aw95016_chip_set_mode(struct aw95016 *aw95016, unsigned int gpio,
				 int mode)
{
	int ret;
	unsigned char reg_val[2] = {0};

	ret = i2c_read_multi_byte(aw95016->client, AW95016_REG_DOMD_P0, reg_val, 2);
	if (ret < 0)
		return ret;

	if (mode) {
		if (gpio < 8)
			reg_val[0] |= 1 << gpio;
		else
			reg_val[1] |= 1 << (gpio - 8);
	} else {
		if (gpio < 8)
			reg_val[0] &= ~(1 << gpio);
		else
			reg_val[1] &= ~(1 << (gpio - 8));
	}

	return i2c_write_multi_byte(aw95016->client, AW95016_REG_DOMD_P0, reg_val, 2);
}

static int aw95016_chip_set_gpio_level(struct aw95016 *aw95016,
				       unsigned int gpio, int level)
{
	int ret;
	unsigned char reg_val[2] = {0};

	ret = i2c_read_multi_byte(aw95016->client, AW95016_REG_OUTPUT_P0, reg_val, 2);
	if (ret < 0)
		return ret;

	if (level) {
		if (gpio < 8)
			reg_val[0] |= 1 << gpio;
		else
			reg_val[1] |= 1 << (gpio - 8);
	} else {
		if (gpio < 8)
			reg_val[0] &= ~(1 << gpio);
		else
			reg_val[1] &= ~(1 << (gpio - 8));
	}

	return i2c_write_multi_byte(aw95016->client, AW95016_REG_OUTPUT_P0, reg_val, 2);
}

static int aw95016_chip_get_gpio_level(struct aw95016 *aw95016,
				       unsigned int gpio)
{
	int ret;
	unsigned char reg_val[2] = {0};
	uint8_t dir;
	int level;

	ret = i2c_read_multi_byte(aw95016->client, AW95016_REG_CONFIG_P0, reg_val, 2);
	if (ret < 0)
		return ret;

	if (gpio < 8)
		dir = (reg_val[0] >> gpio) & 0x1;
	else
		dir = (reg_val[1] >> (gpio - 8)) & 0x1;

	if (dir == AW95016_OUTPUT) {
		ret = i2c_read_multi_byte(aw95016->client, AW95016_REG_OUTPUT_P0, reg_val, 2);
		if (ret < 0)
			return ret;
	} else {
		ret = i2c_read_multi_byte(aw95016->client, AW95016_REG_INPUT_P0, reg_val, 2);
		if (ret < 0)
			return ret;
	}

	if (gpio < 8)
		level = (reg_val[0] >> gpio) & 0x1;
	else
		level = (reg_val[1] >> (gpio - 8)) & 0x1;

	return level;
}

static int aw95016_chip_set_driver_strength(struct aw95016 *aw95016,
					    unsigned int gpio, int dsr)
{
	int ret;
	unsigned char reg_val[4] = {0};

	ret = i2c_read_multi_byte(aw95016->client, AW95016_REG_DSR_P0, reg_val, 4);
	if (ret < 0)
		return ret;

#line 726
	pr_err("%s:%d dsr 0x%x 0x%x 0x%x 0x%x\n", __func__, __LINE__,
	       reg_val[0], reg_val[1], reg_val[2], reg_val[3]);
	if (gpio < 4) {
		reg_val[0] &= ~(3 << (gpio * 2));
		reg_val[0] |= dsr << (gpio * 2);
	} else if (gpio < 8) {
		reg_val[1] &= ~(3 << (gpio * 2 - 8));
		reg_val[1] |= dsr << (gpio * 2 - 8);
	} else if (gpio < 12) {
		reg_val[2] &= ~(3 << (gpio * 2 - 16));
		reg_val[2] |= dsr << (gpio * 2 - 16);
	} else {
		reg_val[3] &= ~(3 << (gpio * 2 - 24));
		reg_val[3] |= dsr << (gpio * 2 - 24);
	}
#line 740
	pr_err("%s:%d dsr 0x%x 0x%x 0x%x 0x%x\n", __func__, __LINE__,
	       reg_val[0], reg_val[1], reg_val[2], reg_val[3]);

	return i2c_write_multi_byte(aw95016->client, AW95016_REG_DSR_P0, reg_val, 4);
}

static int aw95016_chip_set_interrupt_enable(struct aw95016 *aw95016,
					     unsigned int gpio, bool enable)
{
	int ret;
	unsigned char reg_val[2] = {0};

	ret = i2c_read_multi_byte(aw95016->client, AW95016_REG_INT_MASK_P0, reg_val, 2);
	if (ret < 0)
		return ret;

	if (enable) {
		if (gpio < 8)
			reg_val[0] &= ~(1 << gpio);
		else
			reg_val[1] &= ~(1 << (gpio - 8));
	} else {
		if (gpio < 8)
			reg_val[0] |= 1 << gpio;
		else
			reg_val[1] |= 1 << (gpio - 8);
	}

	return i2c_write_multi_byte(aw95016->client, AW95016_REG_INT_MASK_P0, reg_val, 2);
}

static int aw95016_chip_clear_interrupt(struct aw95016 *aw95016)
{
	int ret;
	unsigned char reg_val[2] = {0};

	ret = i2c_read_multi_byte(aw95016->client, AW95016_REG_INPUT_P0, reg_val, 2);

	return ret;
}

static struct aw95016 *aw95016_get_by_dev(struct device *dev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(g_aw95016); i++)
		if (g_aw95016[i] && g_aw95016[i]->dev == dev)
			return g_aw95016[i];

	return NULL;
}

static int aw95016_gpio_direction_input(struct device *dev, unsigned int gpio,
					unsigned int flag)
{
	int ret;
	struct aw95016 *aw95016;
	uint8_t mode = flag & 0x1;

#line 805
	pr_err("%s:%d gpio %d E\n", __func__, __LINE__, gpio);
	aw95016 = aw95016_get_by_dev(dev);
	if (!aw95016)
		return -EPROBE_DEFER;

	mutex_lock(&aw95016->mutex_lock);
	ret = aw95016_chip_set_gpio_direction(aw95016, gpio, AW95016_INPUT);
	if (ret < 0) {
#line 813
		pr_err("%s:%d gpio %d set input failed\n", __func__, __LINE__, gpio);
		goto set_gpio_dir_input_failed;
	}
	ret = aw95016_chip_set_mode(aw95016, gpio, mode);
	if (ret < 0) {
#line 818
		pr_err("%s:%d gpio %d set mode failed\n", __func__, __LINE__, gpio);
		goto set_gpio_dir_input_failed;
	}
	mutex_unlock(&aw95016->mutex_lock);
	return 0;

set_gpio_dir_input_failed:
	mutex_unlock(&aw95016->mutex_lock);
	return ret;
}

static int aw95016_gpio_direction_output(struct device *dev, unsigned int gpio,
					 int value, unsigned int flag)
{
	int ret;
	struct aw95016 *aw95016;
	uint8_t mode = flag & 0x1;
	uint8_t dsr = (flag >> 4) & 0x3;

#line 836
	pr_err("%s:%d gpio %d value %d flag 0x%x dev %p E\n", __func__, __LINE__,
	       gpio, value, flag, dev);
	aw95016 = aw95016_get_by_dev(dev);
	if (!aw95016)
		return -EPROBE_DEFER;

	mutex_lock(&aw95016->mutex_lock);
	ret = aw95016_chip_set_gpio_direction(aw95016, gpio, AW95016_OUTPUT);
	if (ret < 0) {
#line 844
		pr_err("%s:%d gpio %d set output failed\n", __func__, __LINE__, gpio);
		goto set_gpio_dir_output_failed;
	}
	ret = aw95016_chip_set_mode(aw95016, gpio, mode);
	if (ret < 0) {
#line 849
		pr_err("%s:%d gpio %d set mode failed\n", __func__, __LINE__, gpio);
		goto set_gpio_dir_output_failed;
	}
	ret = aw95016_chip_set_driver_strength(aw95016, gpio, dsr);
	if (ret < 0) {
#line 854
		pr_err("%s:%d gpio %d set driver strength failed\n", __func__, __LINE__, gpio);
		goto set_gpio_dir_output_failed;
	}
	ret = aw95016_chip_set_gpio_level(aw95016, gpio, value);
	if (ret < 0) {
#line 859
		pr_err("%s:%d gpio %d set level failed\n", __func__, __LINE__, gpio);
		goto set_gpio_dir_output_failed;
	}
	mutex_unlock(&aw95016->mutex_lock);
	return 0;

set_gpio_dir_output_failed:
	mutex_unlock(&aw95016->mutex_lock);
	return ret;
}

static int aw95016_gpio_set(struct device *dev, unsigned int gpio, int value)
{
	int ret;
	struct aw95016 *aw95016;

#line 876
	pr_err("%s:%d gpio %d E\n", __func__, __LINE__, gpio);
	aw95016 = aw95016_get_by_dev(dev);
	if (!aw95016)
		return -EPROBE_DEFER;

	mutex_lock(&aw95016->mutex_lock);
	ret = aw95016_chip_set_gpio_level(aw95016, gpio, value);
	if (ret < 0) {
#line 884
		pr_err("%s:%d gpio %d set level failed\n", __func__, __LINE__, gpio);
		goto set_gpio_level_failed;
	}
	mutex_unlock(&aw95016->mutex_lock);
	return 0;

set_gpio_level_failed:
	mutex_unlock(&aw95016->mutex_lock);
	return ret;
}

static int aw95016_gpio_get(struct device *dev, unsigned int gpio)
{
	int gpio_level;
	struct aw95016 *aw95016;

#line 901
	pr_err("%s:%d gpio %d E\n", __func__, __LINE__, gpio);
	aw95016 = aw95016_get_by_dev(dev);
	if (!aw95016)
		return -EPROBE_DEFER;

	mutex_lock(&aw95016->mutex_lock);
	gpio_level = aw95016_chip_get_gpio_level(aw95016, gpio);
	if (gpio_level < 0)
#line 909
		pr_err("%s:%d gpio %d get level failed\n", __func__, __LINE__, gpio);
	mutex_unlock(&aw95016->mutex_lock);

	return gpio_level;
}

static int aw95016_register_irq_queue(struct device *dev, unsigned int gpio,
				      irq_func func, unsigned int flag, void *data)
{
	int ret;
	struct aw95016 *aw95016;
	uint8_t mode = flag & 0x1;

#line 927
	pr_err("%s:%d gpio %d E\n", __func__, __LINE__, gpio);
	aw95016 = aw95016_get_by_dev(dev);
	if (!aw95016)
		return -EPROBE_DEFER;

	mutex_lock(&aw95016->mutex_lock);
	ret = aw95016_chip_set_mode(aw95016, gpio, mode);
	if (ret < 0) {
#line 935
		pr_err("%s:%d gpio %d set mode failed\n", __func__, __LINE__, gpio);
		goto register_irq_queue_failed;
	}
	ret = aw95016_chip_set_interrupt_enable(aw95016, gpio, true);
	if (ret < 0) {
#line 940
		pr_err("%s:%d gpio %d set interrupt failed\n", __func__, __LINE__, gpio);
		goto register_irq_queue_failed;
	}
	ret = aw95016_chip_clear_interrupt(aw95016);
	if (ret < 0) {
#line 945
		pr_err("%s:%d gpio %d clear interrupt failed\n", __func__, __LINE__, gpio);
		goto register_irq_queue_failed;
	}
	mutex_unlock(&aw95016->mutex_lock);

	if (aw95016->gpio_irq_workqueue) {
		aw95016->gpio_irq_queue[gpio].gpio_id = gpio;
		aw95016->gpio_irq_queue[gpio].func = func;
		aw95016->gpio_irq_queue[gpio].data = data;
		INIT_WORK(&aw95016->gpio_irq_queue[gpio].work, aw95016_gpio_irq_work_queue);
	}
	return 0;

register_irq_queue_failed:
	mutex_unlock(&aw95016->mutex_lock);
	return ret;
}

static int aw95016_enable_irq(struct device *dev, unsigned int gpio)
{
	int ret;
	struct aw95016 *aw95016;

#line 966
	pr_err("%s:%d gpio %d E\n", __func__, __LINE__, gpio);
	aw95016 = aw95016_get_by_dev(dev);
	if (!aw95016)
		return -EPROBE_DEFER;

	mutex_lock(&aw95016->mutex_lock);
	ret = aw95016_chip_set_interrupt_enable(aw95016, gpio, true);
	mutex_unlock(&aw95016->mutex_lock);
	if (ret < 0) {
#line 975
		pr_err("%s:%d gpio %d set interrupt failed\n", __func__, __LINE__, gpio);
		return ret;
	}

	return 0;
}

static int aw95016_disable_irq(struct device *dev, unsigned int gpio)
{
	int ret;
	struct aw95016 *aw95016;

#line 987
	pr_err("%s:%d gpio %d E\n", __func__, __LINE__, gpio);
	aw95016 = aw95016_get_by_dev(dev);
	if (!aw95016)
		return -EPROBE_DEFER;

	mutex_lock(&aw95016->mutex_lock);
	ret = aw95016_chip_set_interrupt_enable(aw95016, gpio, false);
	mutex_unlock(&aw95016->mutex_lock);
	if (ret < 0) {
#line 996
		pr_err("%s:%d gpio %d set interrupt failed\n", __func__, __LINE__, gpio);
		return ret;
	}

	return 0;
}

static int aw95016_enable_irq_wake(struct device *dev, unsigned int gpio)
{
	struct aw95016 *aw95016;

#line 1008
	pr_err("%s:%d gpio %d E\n", __func__, __LINE__, gpio);
	aw95016 = aw95016_get_by_dev(dev);
	if (!aw95016)
		return -EPROBE_DEFER;

	aw95016->irq_wake_enable = true;

	return 0;
}

static int aw95016_disable_irq_wake(struct device *dev, unsigned int gpio)
{
	struct aw95016 *aw95016;

#line 1021
	pr_err("%s:%d gpio %d E\n", __func__, __LINE__, gpio);
	aw95016 = aw95016_get_by_dev(dev);
	if (!aw95016)
		return -EPROBE_DEFER;

	aw95016->irq_wake_enable = false;

	return 0;
}

static int aw95016_create_work_queue_resource(struct aw95016 *aw95016)
{
	aw95016->gpio_irq_workqueue = create_singlethread_workqueue("aw95016-gpio-workqueue");
	if (!aw95016->gpio_irq_workqueue) {
#line 1034
		pr_err("%s:%d create gpio irq workqueue failed\n", __func__, __LINE__);
		return -ENOMEM;
	}

	return 0;
}

static irqreturn_t aw95016_irq_handler(int irq, void *data);

static int aw95016_create_gpio_interrupt_resource(struct aw95016 *aw95016,
						  struct device_node *np)
{
	int ret;

	aw95016->irq_wake_enable = false;
	aw95016->irq_gpio = of_get_named_gpio(np, "irq-gpio", 0);
	if (aw95016->irq_gpio < 0) {
		dev_err(aw95016->dev, "%s: get irq gpio failed\r\n", __func__);
		return -EINVAL;
	}

	ret = gpio_direction_input(aw95016->irq_gpio);
	if (ret < 0) {
		dev_err(aw95016->dev, "%s: gpio_direction_input failed\r\n", __func__);
		return ret;
	}

	aw95016->irq_num = gpio_to_irq(aw95016->irq_gpio);
	if (aw95016->irq_num < 0) {
		dev_err(aw95016->dev, "%s gpio to irq failed\r\n", __func__);
		return aw95016->irq_num;
	}
	pr_info("aw95016 irq_gpio %d irq num=%d\n", aw95016->irq_gpio, aw95016->irq_num);

	ret = aw95016_create_work_queue_resource(aw95016);
	if (ret)
		return ret;

	ret = request_threaded_irq(aw95016->irq_num, NULL, aw95016_irq_handler,
				   IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				   "aw95016", aw95016);
	if (ret) {
#line 1075
		pr_err("%s:%d request irq failed\n", __func__, __LINE__);
		return ret;
	}

	return 0;
}

static ssize_t gpio_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t len)
{
	unsigned int data[8] = {0};
	unsigned int chipId = 0;
	int err;
	struct aw95016 *aw95016 = dev_get_drvdata(dev);

	err = sscanf(buf, "%d %d %d %d %d %d %d %d %d", &chipId, &data[0], &data[1],
		     &data[2], &data[3], &data[4], &data[5], &data[6], &data[7]);
	if (err != 9)
		return -EINVAL;

	aw95016->gpio[data[0]].idx = data[0];
	aw95016->gpio[data[0]].direction = data[1];
	aw95016->gpio[data[0]].out_status = data[2];
	aw95016->gpio[data[0]].out_level = data[3];
	aw95016->gpio[data[0]].io_strength = data[4];
	aw95016->gpio[data[0]].input_status = data[5];
	aw95016->gpio[data[0]].input_en = data[6];
	aw95016->gpio[data[0]].input_level = data[7];
	aw95016_gpio_mode_init(aw95016);

	return len;
}

static ssize_t gpio_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	ssize_t len = 0;
	int i;
	struct aw95016 *aw95016 = dev_get_drvdata(dev);

	for (i = 0; i < aw95016->gpio_num; i++)
		len += snprintf(buf + len, PAGE_SIZE - len,
				"idx = %d, direction = %d\nout_status = %d, out_level = %d,io_strength = %d\ninput_status = %d, input_en = %d,input_level = %d\n",
				aw95016->gpio[i].idx, aw95016->gpio[i].direction,
				aw95016->gpio[i].out_status, aw95016->gpio[i].out_level,
				aw95016->gpio[i].io_strength, aw95016->gpio[i].input_status,
				aw95016->gpio[i].input_en, aw95016->gpio[i].input_level);

	return len;
}

/* registers 0x1a, 0x60, 0x61 and 0x70 are listed without being read */
static ssize_t reg_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	ssize_t len = 0;
	unsigned char reg_val = 0;
	int i;
	struct aw95016 *aw95016 = dev_get_drvdata(dev);

	for (i = 0; i < 0x18; i++) {
		i2c_read_byte(aw95016->client, i, &reg_val);
		len += snprintf(buf + len, PAGE_SIZE - len, "reg:0x%02x=0x%02x\n", i, reg_val);
	}
	len += snprintf(buf + len, PAGE_SIZE - len, "reg:0x%02x=0x%02x\n", 0x1a, reg_val);
	len += snprintf(buf + len, PAGE_SIZE - len, "reg:0x%02x=0x%02x\n", 0x60, reg_val);
	len += snprintf(buf + len, PAGE_SIZE - len, "reg:0x%02x=0x%02x\n", 0x61, reg_val);
	len += snprintf(buf + len, PAGE_SIZE - len, "reg:0x%02x=0x%02x\n", 0x70, reg_val);

	return len;
}

static DEVICE_ATTR(gpio, 0644, gpio_show, gpio_store);
static DEVICE_ATTR(reg, 0444, reg_show, NULL);

static struct attribute *aw95016_gpio_attributes[] = {
	&dev_attr_gpio.attr,
	&dev_attr_reg.attr,
	NULL
};

static struct attribute_group aw95016_gpio_attribute_group = {
	.attrs = aw95016_gpio_attributes,
};

static void aw95016_work_queue_test(struct work_struct *work)
{
	if (output_test_gpio >= 0) {
		gpio_set_value(output_test_gpio, 1);
		gpio_set_value(output_test_gpio, 0);
		gpio_set_value(output_test_gpio, 1);
		gpio_set_value(output_test_gpio, 0);
	}
#line 1168
	pr_err("%s:%d receive a signal\n", __func__, __LINE__);
}

static irqreturn_t aw95016_irq_handler(int irq, void *data)
{
	struct aw95016 *aw95016 = data;
	unsigned char reg_val[2] = {0};
	unsigned long flags;
	int i;

	local_irq_save(flags);
	i2c_read_multi_byte(aw95016->client, AW95016_REG_INT_STATUS_P0, reg_val, 2);
	for (i = 0; i < AW95016_GPIO_NUM_MAX; i++) {
		if (i < 8) {
			if ((reg_val[0] >> i) & 0x1)
				queue_work(aw95016->gpio_irq_workqueue,
					   &aw95016->gpio_irq_queue[i].work);
		} else {
			if ((reg_val[1] >> (i - 8)) & 0x1)
				queue_work(aw95016->gpio_irq_workqueue,
					   &aw95016->gpio_irq_queue[i].work);
		}
	}
#line 1192
	pr_err("%s:%d receive a irq reg_val 0x%x 0x%x\n", __func__, __LINE__,
	       reg_val[0], reg_val[1]);
	local_irq_restore(flags);

	return IRQ_HANDLED;
}

static int aw95016_parse_dts(struct device *dev, struct aw95016 *aw95016,
			     struct device_node *node)
{
	int ret;
	unsigned int i;
	unsigned int reg = 0;
	struct device_node *gpio_node;
	struct device_node *temp;

	aw95016->rst_gpio = of_get_named_gpio(node, "reset-gpio", 0);
	if (!gpio_is_valid(aw95016->rst_gpio)) {
		dev_err(aw95016->dev, "%s: dts don't provide reset-gpio\n", __func__);
		return -EINVAL;
	}

	ret = gpio_request(aw95016->rst_gpio, "aw95016-reset");
	if (ret) {
		dev_err(&aw95016->client->dev, "%s: unable to request gpio [%d]\n",
			__func__, aw95016->rst_gpio);
		return ret;
	}

	ret = gpio_direction_output(aw95016->rst_gpio, 1);
	if (ret) {
		gpio_free(aw95016->rst_gpio);
		dev_err(aw95016->dev, "%s: unable to set direction of gpio\n", __func__);
		return ret;
	}

	output_test_gpio = of_get_named_gpio(node, "output-test-gpio", 0);
	if (gpio_is_valid(output_test_gpio)) {
		ret = gpio_direction_output(output_test_gpio, 0);
		if (ret) {
			gpio_free(output_test_gpio);
			dev_err(aw95016->dev, "%s: unable to set direction of output_test_gpio\n",
				__func__);
		}
	} else {
		dev_err(aw95016->dev, "%s: dts don't provide output_test_gpio\n", __func__);
	}

	test_workqueue = create_singlethread_workqueue("test_wq");
	if (test_workqueue)
		INIT_WORK(&test_work, aw95016_work_queue_test);

	of_property_read_u32(node, "gpio_mode", &aw95016->gpio_mode);
	if (aw95016->gpio_mode == AW95016_OUTPUT) {
		pr_info("%s:gpio_mode is output mode\n", __func__);
		aw95016->gpio_mode = AW95016_OUTPUT;
	} else {
		pr_info("%s:gpio_mode is input mode\n", __func__);
		aw95016->gpio_mode = AW95016_INPUT;
	}

	of_property_read_u32(node, "reg", &reg);
	aw95016->reg = reg;
	pr_err("aw95016 reg= %x, aw95016:%x\n", reg, aw95016);
	of_property_read_u32(node, "chip_index", &aw95016->chip_index);
	if (aw95016->chip_index == 2)
		g_aw95016[0] = aw95016;
	else
		g_aw95016[1] = aw95016;

	gpio_node = of_find_node_by_name(aw95016->dev->of_node, "gpio");
	if (!gpio_node) {
		dev_err(aw95016->dev, "%s: can't find gpio node return failed\n", __func__);
		ret = -1;
		goto err_id;
	}

	i = 0;
	for_each_child_of_node(gpio_node, temp)
		i++;
	aw95016->gpio_num = i;

	aw95016->gpio = devm_kzalloc(aw95016->dev,
				     sizeof(struct gpio_container) * aw95016->gpio_num,
				     GFP_KERNEL);
	if (!aw95016->gpio) {
		ret = -ENOMEM;
		goto err_id;
	}

	i = 0;
	for_each_child_of_node(gpio_node, temp) {
		of_property_read_u32(temp, "idx", &aw95016->gpio[i].idx);
		of_property_read_u32(temp, "direction", &aw95016->gpio[i].direction);
		of_property_read_u32(temp, "out_status", &aw95016->gpio[i].out_status);
		of_property_read_u32(temp, "out_level", &aw95016->gpio[i].out_level);
		of_property_read_u32(temp, "io_strength", &aw95016->gpio[i].io_strength);
		of_property_read_u32(temp, "input_status", &aw95016->gpio[i].input_status);
		of_property_read_u32(temp, "input_en", &aw95016->gpio[i].input_en);
		of_property_read_u32(temp, "input_level", &aw95016->gpio[i].input_level);
		i++;
	}

	ret = sysfs_create_group(&aw95016->dev->kobj, &aw95016_gpio_attribute_group);
	if (ret) {
		dev_err(aw95016->dev, "gpio sysfs failed ret: %d\n", ret);
		goto free_mem;
	}

	return 0;

free_mem:
	devm_kfree(aw95016->dev, aw95016->gpio);
err_id:
	devm_gpio_free(aw95016->dev, aw95016->irq_gpio);
	return ret;
}

static int gpio_temp(void *data, u64 *val)
{
	pr_err("aw95016 gpio_temp val %d\n", *val);
	return 0;
}

static int ex2_gpio_config(void *data, u64 val)
{
	unsigned int gpio, direction, out_status, out_level, io_strength;
	unsigned int input_status, input_en, input_level;

	pr_err("gpio_config value %d\n", val);
	if (val >= 0x1000) {
		pr_err("gpio_config value error\n");
		return -1;
	}

	gpio = val & 0xf;
	direction = (val >> 4) & 0x1;
	out_status = (val >> 5) & 0x1;
	out_level = (val >> 6) & 0x1;
	io_strength = (val >> 7) & 0x3;
	input_status = (val >> 9) & 0x1;
	input_en = (val >> 10) & 0x1;
	input_level = val >> 11;
	pr_err("gpio(%d) direction(%d) out_status(%d) out_level(%d) io_strength(%d) input_status(%d) input_en(%d) input_level(%d)",
	       gpio, direction, out_status, out_level, io_strength,
	       input_status, input_en, input_level);
	extern_gpio_config(0, gpio, direction, out_status, out_level, io_strength,
			   input_status, input_en, input_level);

	return 0;
}

static int ex3_gpio_config(void *data, u64 val)
{
	unsigned int gpio, direction, out_status, out_level, io_strength;
	unsigned int input_status, input_en, input_level;

	pr_err("gpio_config value %d\n", val);
	if (val >= 0x1000) {
		pr_err("gpio_config value error\n");
		return -1;
	}

	gpio = val & 0xf;
	direction = (val >> 4) & 0x1;
	out_status = (val >> 5) & 0x1;
	out_level = (val >> 6) & 0x1;
	io_strength = (val >> 7) & 0x3;
	input_status = (val >> 9) & 0x1;
	input_en = (val >> 10) & 0x1;
	input_level = val >> 11;
	pr_err("gpio(%d) direction(%d) out_status(%d) out_level(%d) io_strength(%d) input_status(%d) input_en(%d) input_level(%d)",
	       gpio, direction, out_status, out_level, io_strength,
	       input_status, input_en, input_level);
	extern_gpio_config(1, gpio, direction, out_status, out_level, io_strength,
			   input_status, input_en, input_level);

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(aw95016_ex2_gpio_config, gpio_temp, ex2_gpio_config, "%16llu");
DEFINE_SIMPLE_ATTRIBUTE(aw95016_ex3_gpio_config, gpio_temp, ex3_gpio_config, "%16llu");

/*
 * val bits 0-2: P1 pin, bit 8: direction (1 = output), bit 9: output level,
 * bit 10: open-drain/push-pull mode for inputs
 */
#define AW95016_GPIO_TEST_STORE(name, chip, v, l)				\
static int name(void *data, u64 val)					\
{									\
	struct aw95016 *aw95016 = g_aw95016[chip];			\
	uint8_t gpio_num = val & 0x7;					\
	uint8_t dir = (val >> 8) & 0x1;					\
	uint8_t level;							\
	uint8_t pmod;							\
	uint8_t reg = 0;						\
									\
	AW95016_TEST_BANNER;						\
	pr_err("%s:%d val 0x%x\n", __func__, (v), val);			\
	if (dir == 0) {							\
		pmod = (val >> 10) & 0x1;				\
		i2c_read_multi_byte(aw95016->client, AW95016_REG_CONFIG_P1, &reg, 1); \
		pr_err("%s:%d P1_DIR read reg = 0x%x\n", __func__, (l), reg); \
		reg &= ~(1 << gpio_num);				\
		pr_err("%s:%d  P1_DIR read reg = 0x%x\n", __func__, (l) + 2, reg); \
		i2c_write_multi_byte(aw95016->client, AW95016_REG_CONFIG_P1, &reg, 1); \
		i2c_read_multi_byte(aw95016->client, AW95016_REG_DOMD_P1, &reg, 1); \
		pr_err("%s:%d P1_DOMD read reg = 0x%x\n", __func__, (l) + 5, reg); \
		if (pmod)						\
			reg |= 1 << gpio_num;				\
		else							\
			reg &= ~(1 << gpio_num);			\
		pr_err("%s:%d P1_DOMD read reg = 0x%x\n", __func__, (l) + 10, reg); \
		i2c_write_multi_byte(aw95016->client, AW95016_REG_DOMD_P1, &reg, 1); \
		i2c_read_multi_byte(aw95016->client, AW95016_REG_INT_MASK_P1, &reg, 1); \
		pr_err("%s:%d P1_MAK read reg = 0x%x\n", __func__, (l) + 13, reg); \
		reg &= ~(1 << gpio_num);				\
		pr_err("%s:%d P1_MAK read reg = 0x%x\n", __func__, (l) + 15, reg); \
		i2c_write_multi_byte(aw95016->client, AW95016_REG_INT_MASK_P1, &reg, 1); \
		i2c_read_multi_byte(aw95016->client, AW95016_REG_INPUT_P1, &reg, 1); \
		pr_err("%s:%d P1_INPUT read reg = 0x%x\n", __func__, (l) + 19, reg); \
	}								\
	if (dir == 1) {							\
		level = (val >> 9) & 0x1;				\
		i2c_read_multi_byte(aw95016->client, AW95016_REG_CONFIG_P1, &reg, 1); \
		pr_err("%s:%d P1_DIR read reg = 0x%x\n", __func__, (l) + 22, reg); \
		reg |= 1 << gpio_num;					\
		pr_err("%s:%d  P1_DIR read reg = 0x%x\n", __func__, (l) + 24, reg); \
		i2c_write_multi_byte(aw95016->client, AW95016_REG_CONFIG_P1, &reg, 1); \
		i2c_read_multi_byte(aw95016->client, AW95016_REG_OUTPUT_P1, &reg, 1); \
		pr_err("%s:%d P1_OUTPUT read reg = 0x%x\n", __func__, (l) + 27, reg); \
		if (level)						\
			reg |= 1 << gpio_num;				\
		else							\
			reg &= ~(1 << gpio_num);			\
		pr_err("%s:%d  P1_OUTPUT read reg = 0x%x\n", __func__, (l) + 32, reg); \
		i2c_write_multi_byte(aw95016->client, AW95016_REG_OUTPUT_P1, &reg, 1); \
		i2c_read_multi_byte(aw95016->client, AW95016_REG_DOMD_P1, &reg, 1); \
		pr_err("%s:%d  P1_DOMD read reg = 0x%x\n", __func__, (l) + 35, reg); \
		reg &= ~(1 << gpio_num);				\
		pr_err("%s:%d  P1_DOMD read reg = 0x%x\n", __func__, (l) + 37, reg); \
		i2c_write_multi_byte(aw95016->client, AW95016_REG_DOMD_P1, &reg, 1); \
		i2c_read_multi_byte(aw95016->client, AW95016_REG_DSR1_P0, &reg, 1); \
		pr_err("%s:%d  P0_DSR1 read reg = 0x%x\n", __func__, (l) + 40, reg); \
		reg &= ~(1 << gpio_num);				\
		pr_err("%s:%d  P0_DSR1 read reg = 0x%x\n", __func__, (l) + 42, reg); \
		i2c_write_multi_byte(aw95016->client, AW95016_REG_DSR1_P0, &reg, 1); \
	}								\
									\
	return 0;							\
}

/* bit 8 selects P1_OUTPUT (1) or P1_INPUT (0); both print as P1_INPUT */
#define AW95016_GPIO_TEST_SHOW(name, chip, l)				\
static int name(void *data, u64 val)					\
{									\
	uint8_t dir = (val >> 8) & 0x1;					\
	uint8_t reg = 0;						\
									\
	if (dir == 0) {							\
		i2c_read_multi_byte(g_aw95016[chip]->client, AW95016_REG_INPUT_P1, &reg, 1); \
		pr_err("%s:%d P1_INPUT read reg = 0x%x\n", __func__, (l), reg); \
	} else {							\
		i2c_read_multi_byte(g_aw95016[chip]->client, AW95016_REG_OUTPUT_P1, &reg, 1); \
		pr_err("%s:%d P1_INPUT read reg = 0x%x\n", __func__, (l) + 3, reg); \
	}								\
									\
	return 0;							\
}

#define AW95016_TEST_BANNER pr_err("%s---------------------%d", __func__, 1314)
AW95016_GPIO_TEST_STORE(aw95016_ext2_gpio_test_store, 0, 1319, 1324)
#undef AW95016_TEST_BANNER
#define AW95016_TEST_BANNER do { } while (0)
AW95016_GPIO_TEST_STORE(aw95016_ext3_gpio_test_store, 1, 1424, 1428)
#undef AW95016_TEST_BANNER
AW95016_GPIO_TEST_SHOW(aw95016_ext2_gpio_test_show, 0, 1390)
AW95016_GPIO_TEST_SHOW(aw95016_ext3_gpio_test_show, 1, 1494)

DEFINE_SIMPLE_ATTRIBUTE(aw95016_ext2_gpio_set_test, NULL, aw95016_ext2_gpio_test_store, "%16llu");
DEFINE_SIMPLE_ATTRIBUTE(aw95016_ext2_gpio_get_test, NULL, aw95016_ext2_gpio_test_show, "%16llu");
DEFINE_SIMPLE_ATTRIBUTE(aw95016_ext3_gpio_set_test, NULL, aw95016_ext3_gpio_test_store, "%16llu");
DEFINE_SIMPLE_ATTRIBUTE(aw95016_ext3_gpio_get_test, NULL, aw95016_ext3_gpio_test_show, "%16llu");

static int aw95016_i2c_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct device_node *np = client->dev.of_node;
	struct quectel_gpio_subdev subdev = {0};
	struct aw95016 *aw95016;
	struct dentry *debugfs;
	int ret = 0;

#line 1597
	pr_err("common %s %d enter, %s\n", __func__, __LINE__, AW95016_DRIVER_VERSION);
	g_isInit = false;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "I2C transfer not Supported\n");
		return -EIO;
	}

	aw95016 = devm_kzalloc(dev, sizeof(*aw95016), GFP_KERNEL);
	if (!aw95016)
		return -ENOMEM;

	mutex_init(&aw95016->mutex_lock);
	aw95016->client = client;
	aw95016->dev = dev;
	i2c_set_clientdata(client, aw95016);

	if (np) {
		ret = aw95016_parse_dts(dev, aw95016, np);
		if (ret) {
			dev_err(dev, "%s: parse dts failed\n", __func__);
			goto err_parse;
		}
		pr_info("%s: parse dts successful\n", __func__);
	} else {
		dev_err(dev, "%s: np is NULL\n", __func__);
	}

	aw95016_reset(aw95016, AW95016_SOFT_RESET);
	if (aw95016_read_chipid(client) < 0) {
		dev_err(dev, "%s: read_chipid error\n", __func__);
		goto err_free;
	}
	pr_info("%s:read_chipid successful \n", __func__);

	aw95016_gpio_mode_init(aw95016);
#line 1637
	pr_err("%s:%d,chip_index=%d", __func__, __LINE__, aw95016->chip_index);
	dev_err(dev, "%s:%d,chip_index=%d", __func__, __LINE__, aw95016->chip_index);
	if (aw95016->chip_index == 2) {
		debugfs = debugfs_create_dir("aw95016_ex2", NULL);
		debugfs_create_file("gpio_config", 0644, debugfs, NULL, &aw95016_ex2_gpio_config);
		debugfs_create_file("aw95016_gpio_set", 0644, debugfs, NULL, &aw95016_ext2_gpio_set_test);
		debugfs_create_file("aw95016_gpio_get", 0644, debugfs, NULL, &aw95016_ext2_gpio_get_test);
#line 1645
		dev_err(dev, "%s:%d,chip_index=%d", __func__, __LINE__, aw95016->chip_index);
	} else {
		debugfs = debugfs_create_dir("aw95016_ex3", NULL);
		debugfs_create_file("gpio_config", 0644, debugfs, NULL, &aw95016_ex3_gpio_config);
		debugfs_create_file("aw95016_gpio_set", 0644, debugfs, NULL, &aw95016_ext3_gpio_set_test);
		debugfs_create_file("aw95016_gpio_get", 0644, debugfs, NULL, &aw95016_ext3_gpio_get_test);
#line 1652
		dev_err(dev, "%s:%d,chip_index=%d", __func__, __LINE__, aw95016->chip_index);
	}

	ret = aw95016_create_gpio_interrupt_resource(aw95016, np);
	if (ret) {
		dev_err(dev, "%s: aw95016_create_gpio_interrupt_resource failed\n", __func__);
		goto err_free;
	}
#line 1661
	pr_info("%s:%d aw95016 successful gpio num %d\n", __func__, __LINE__, aw95016->gpio_num);

	ret = of_property_read_string(np, "label", &subdev.label);
	if (ret) {
		dev_err(dev, "%s: get label failed\n", __func__);
		goto err_free;
	}
	subdev.dev = dev;
	subdev.n_gpio = aw95016->gpio_num;
	subdev.gpio_direction_input = aw95016_gpio_direction_input;
	subdev.gpio_direction_output = aw95016_gpio_direction_output;
	subdev.gpio_set = aw95016_gpio_set;
	subdev.gpio_get = aw95016_gpio_get;
	subdev.register_irq_queue = aw95016_register_irq_queue;
	subdev.enable_irq = aw95016_enable_irq;
	subdev.disable_irq = aw95016_disable_irq;
	subdev.enable_irq_wake = aw95016_enable_irq_wake;
	subdev.disable_irq_wake = aw95016_disable_irq_wake;
	ret = quectel_gpio_register_dev(&subdev);
	if (ret < 0) {
		dev_err(dev, "%s: quectel_gpio_register_dev failed\n", __func__);
		goto err_free;
	}

	g_isInit = true;
	return 0;

err_free:
	if (aw95016->gpio_irq_workqueue)
		destroy_workqueue(aw95016->gpio_irq_workqueue);
err_parse:
	devm_kfree(dev, aw95016);
	g_isInit = false;
	return ret;
}

static int aw95016_i2c_remove(struct i2c_client *client)
{
	struct aw95016 *aw95016 = i2c_get_clientdata(client);

	devm_kfree(aw95016->dev, aw95016);

	return 0;
}

/* the stock driver prints "%s:%d E" here without arguments */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat"
static int aw95016_suspend(struct device *dev)
{
	struct aw95016 *aw95016 = dev_get_drvdata(dev);

	pr_err("%s:%d E\n");
	if (!aw95016) {
		pr_err("%s aw95016 is null\n", __func__);
		return -ENOMEM;
	}
	if (aw95016->irq_wake_enable)
		enable_irq_wake(aw95016->irq_num);
	aw95016_gpio_mode_init(aw95016);

	return 0;
}

static int aw95016_resume(struct device *dev)
{
	struct aw95016 *aw95016 = dev_get_drvdata(dev);

	pr_err("%s:%d E\n");
	if (!aw95016) {
		pr_err("%s aw95016 is null\n", __func__);
		return -ENOMEM;
	}
	if (aw95016->irq_wake_enable)
		disable_irq_wake(aw95016->irq_num);
	aw95016_gpio_mode_init(aw95016);

	return 0;
}
#pragma clang diagnostic pop

static SIMPLE_DEV_PM_OPS(aw95016_pm_ops, aw95016_suspend, aw95016_resume);

static const struct i2c_device_id aw95016_i2c_id[] = {
	{ "awinic,aw95016", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw95016_i2c_id);

static const struct of_device_id aw95016_dt_match[] = {
	{ .compatible = "awinic,aw95016" },
	{ }
};

static struct i2c_driver aw95016_i2c_driver = {
	.driver = {
		.name = "aw95016",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(aw95016_dt_match),
		.pm = &aw95016_pm_ops,
	},
	.probe = aw95016_i2c_probe,
	.remove = aw95016_i2c_remove,
	.id_table = aw95016_i2c_id,
};

static int __init aw95016_i2c_init(void)
{
	int ret;

#line 1770
	pr_err("%s:%d E\n", __func__, __LINE__);
	ret = i2c_add_driver(&aw95016_i2c_driver);
	if (ret)
		pr_err("fail to add aw95016 device into i2c\n");

	return ret;
}

static void __exit aw95016_i2c_exit(void)
{
	i2c_del_driver(&aw95016_i2c_driver);
}

module_init(aw95016_i2c_init);
module_exit(aw95016_i2c_exit);

MODULE_AUTHOR("zhangzetao@awinic.com");
MODULE_DESCRIPTION("aw95016 driver");
MODULE_LICENSE("GPL");
