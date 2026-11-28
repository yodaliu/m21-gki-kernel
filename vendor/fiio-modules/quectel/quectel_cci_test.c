// SPDX-License-Identifier: GPL-2.0
/*
 * Test client of quectel_cci_driver and gpiolib-quectel: debugfs
 * "quectel-cci-test/quectel_cci_test" reads and writes back register 7 over
 * CCI, "quectel_gpio_test" drives the aw95016 test gpios.
 *
 * Reconstructed from the stock FiiO M21 FW 1.1.0 quectel_cci_test.ko
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include <linux/fs.h>

/* The camera-kernel types this test uses (cam_sensor_io.h, cam_sensor_cmn_header.h) */
#define CCI_MASTER 1

enum camera_sensor_i2c_type {
	CAMERA_SENSOR_I2C_TYPE_INVALID,
	CAMERA_SENSOR_I2C_TYPE_BYTE,
	CAMERA_SENSOR_I2C_TYPE_WORD,
	CAMERA_SENSOR_I2C_TYPE_3B,
	CAMERA_SENSOR_I2C_TYPE_DWORD,
	CAMERA_SENSOR_I2C_TYPE_MAX,
};

enum i2c_freq_mode {
	I2C_STANDARD_MODE,
	I2C_FAST_MODE,
	I2C_CUSTOM_MODE,
	I2C_FAST_PLUS_MODE,
	I2C_MAX_MODES,
};

enum cci_i2c_master_t {
	MASTER_0,
	MASTER_1,
	MASTER_MAX,
};

struct cam_sensor_cci_client {
	struct v4l2_subdev *cci_subdev;
	uint32_t freq;
	enum i2c_freq_mode i2c_freq_mode;
	enum cci_i2c_master_t cci_i2c_master;
	uint16_t sid;
	uint16_t cid;
	uint32_t timeout;
	uint16_t retries;
	uint16_t id_map;
	uint16_t cci_device;
};

struct camera_io_master {
	int master_type;
	struct i2c_client *client;
	struct cam_sensor_cci_client *cci_client;
	struct cam_sensor_spi_client *spi_client;
};

struct cam_sensor_i2c_reg_array {
	uint32_t reg_addr;
	uint32_t reg_data;
	uint32_t delay;
	uint32_t data_mask;
};

struct cam_sensor_i2c_reg_setting {
	struct cam_sensor_i2c_reg_array *reg_setting;
	uint32_t size;
	enum camera_sensor_i2c_type addr_type;
	enum camera_sensor_i2c_type data_type;
	unsigned short delay;
	uint8_t *read_buff;
	uint32_t read_buff_len;
};

#include <linux/quectel_cci_cam.h>
#include <linux/gpiolib_quectel_intf.h>

static struct camera_io_master *cci_test_master_info;
static struct device *g_dev;
static int g_test_data = 1;
static int gpio_id = -1;

int quectel_cci_test_func(void)
{
	pr_err("%s:%d test\n", __func__, __LINE__);
	return 0;
}
EXPORT_SYMBOL(quectel_cci_test_func);

static int quectel_cci_test_show(void *data, u64 *val)
{
	struct cam_sensor_i2c_reg_setting write_setting = {0};
	struct cam_sensor_i2c_reg_array reg_array;
	uint8_t reg = 2;
	int ret;

	pr_err("%s:%d data %p val %p\n", __func__, __LINE__, data, val);
	ret = quectel_cci_io_dev_read_seq(cci_test_master_info, 7, &reg,
					  CAMERA_SENSOR_I2C_TYPE_BYTE,
					  CAMERA_SENSOR_I2C_TYPE_BYTE, 1);
	pr_err("%s:%d read ret %d reg 0x%x\n", __func__, __LINE__, ret, reg);

	reg_array.reg_addr = 7;
	reg_array.reg_data = reg;
	reg_array.delay = 0;
	reg_array.data_mask = 0;
	write_setting.reg_setting = &reg_array;
	write_setting.size = 1;
	write_setting.addr_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
	write_setting.data_type = CAMERA_SENSOR_I2C_TYPE_BYTE;
	write_setting.delay = 0;
	ret = quectel_cci_io_dev_write(cci_test_master_info, &write_setting);
	pr_err("%s:%d write ret %d\n", __func__, __LINE__, ret);
	return 0;
}

static int quectel_cci_test_store(void *data, u64 val)
{
	pr_err("%s:%d data %p val %d\n", __func__, __LINE__, data, val);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(quectel_cci_test, quectel_cci_test_show, quectel_cci_test_store, "%16llu");

static void quectel_gpio_irq_func(unsigned gpio, void *data)
{
	pr_err("%s:%d gpio %d\n", __func__, __LINE__, gpio);
}

static int quectel_gpio_test_show(void *data, u64 *val)
{
	int ret;

	pr_err("%s:%d data %p val %p\n", __func__, __LINE__, data, val);
	if (g_dev) {
		gpio_id = quectel_get_named_gpio(g_dev->of_node, "cci-test-gpio", 0);
		if (gpio_id < 0) {
			pr_err("%s:%d get [cci-test-gpio] failed gpio_id %d\n", __func__, __LINE__, gpio_id);
			return gpio_id;
		}
		pr_err("%s:%d get gpio id %d\n", __func__, __LINE__, gpio_id);
		ret = quectel_gpio_direction_output(gpio_id, 1, 1);
		if (ret < 0) {
			pr_err("%s:%d set gpio [%d] output failed ret %d\n", __func__, __LINE__, gpio_id, ret);
			return ret;
		}
	}
	return 0;
}

#define TEST_GET_GPIO(name)							\
	do {									\
		gpio_id = quectel_get_named_gpio(g_dev->of_node, name, 0);	\
		if (gpio_id < 0) {						\
			pr_err("%s:%d get [cci-test-gpio] failed gpio_id %d\n",	\
			       __func__, __LINE__, gpio_id);			\
			return gpio_id;						\
		}								\
		pr_err("%s:%d get gpio id %d\n", __func__, __LINE__, gpio_id);	\
	} while (0)

#define TEST_CHECK(ret)								\
	do {									\
		if ((ret) < 0) {						\
			pr_err("%s:%d set gpio [%d] output failed ret %d\n",	\
			       __func__, __LINE__, gpio_id, ret);		\
			return ret;						\
		}								\
	} while (0)

#define TEST_OUTPUT(name)						\
	do {								\
		if (g_dev) {						\
			TEST_GET_GPIO(name);				\
			ret = quectel_gpio_direction_output(gpio_id, 1, dsr << 4); \
			TEST_CHECK(ret);				\
		}							\
	} while (0)

#define TEST_INPUT(name)						\
	do {								\
		if (g_dev) {						\
			TEST_GET_GPIO(name);				\
			ret = quectel_gpio_direction_input(gpio_id, value); \
			TEST_CHECK(ret);				\
		}							\
	} while (0)

#define TEST_IRQ(name)							\
	do {								\
		if (g_dev) {						\
			TEST_GET_GPIO(name);				\
			ret = quectel_gpio_direction_input(gpio_id, value); \
			TEST_CHECK(ret);				\
			ret = quectel_request_gpio_irq(gpio_id, quectel_gpio_irq_func, value, NULL); \
			if (ret < 0)					\
				pr_err("%s:%d gpio %d request gpio irq failed\n", \
				       __func__, __LINE__, gpio_id);	\
		}							\
	} while (0)

/*
 * val bits 0-3: command, bit 4: value / irq flags, bits 8-9: drive strength.
 * 1-5 use aw95016-ext1-test-gpio, 8-10 ext2, 11-13 ext3.
 */
static int quectel_gpio_test_store(void *data, u64 val)
{
	unsigned int cmd = val & 0xf;
	unsigned int value = (val >> 4) & 1;
	unsigned int dsr = (val >> 8) & 3;
	uint8_t level;
	int ret;

	pr_err("%s:%d data %p val %d\n", __func__, __LINE__, data, val);
	switch (cmd) {
	case 1:
		TEST_OUTPUT("aw95016-ext1-test-gpio");
		break;
	case 2:
		if (gpio_id >= 0) {
			ret = quectel_gpio_set(gpio_id, value);
			if (ret < 0)
				pr_err("%s:%d set gpio [%d] status 1 failed ret %d\n",
				       __func__, __LINE__, gpio_id, ret);
		}
		break;
	case 3:
		TEST_INPUT("aw95016-ext1-test-gpio");
		break;
	case 4:
		if (gpio_id >= 0) {
			level = quectel_gpio_get(gpio_id);
			pr_err("%s:%d get gpio %d level %d\n", __func__, __LINE__, gpio_id, level);
		}
		break;
	case 5:
		TEST_IRQ("aw95016-ext1-test-gpio");
		break;
	case 6:
		if (gpio_id >= 0) {
			if (value)
				quectel_gpio_enable_irq(gpio_id);
			else
				quectel_gpio_disable_irq(gpio_id);
		}
		break;
	case 7:
		if (gpio_id >= 0) {
			if (value)
				quectel_gpio_enable_irq_wake(gpio_id);
			else
				quectel_gpio_disable_irq_wake(gpio_id);
		}
		break;
	case 8:
		TEST_OUTPUT("aw95016-ext2-test-gpio");
		break;
	case 9:
		TEST_INPUT("aw95016-ext2-test-gpio");
		break;
	case 10:
		TEST_IRQ("aw95016-ext2-test-gpio");
		break;
	case 11:
		TEST_OUTPUT("aw95016-ext3-test-gpio");
		break;
	case 12:
		TEST_INPUT("aw95016-ext3-test-gpio");
		break;
	default:
		if (cmd == 13)
			TEST_IRQ("aw95016-ext3-test-gpio");
		break;
	}
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(quectel_gpio_test, quectel_gpio_test_show, quectel_gpio_test_store, "%16llu");

static int quectel_cci_test_probe(void *data)
{
	int res;

	pr_err("%s:%d data %p %p\n", __func__, __LINE__, data, data);
	res = quectel_cci_io_init(cci_test_master_info);
	pr_err("%s:%d res %d\n", __func__, __LINE__, res);
	return 0;
}

static int quectel_cci_info_init(struct camera_io_master *master_info)
{
	struct cam_sensor_cci_client *cci_client;

	master_info->master_type = CCI_MASTER;
	cci_client = kzalloc(sizeof(*cci_client), GFP_KERNEL);
	master_info->cci_client = cci_client;
	if (!cci_client) {
		pr_err("%s:%d alloc cci_client mem failed\n", __func__, __LINE__);
		return -ENOMEM;
	}
	cci_client->cci_i2c_master = MASTER_1;
	cci_client->sid = 0x20;
	cci_client->retries = 3;
	cci_client->id_map = 0;
	cci_client->i2c_freq_mode = I2C_FAST_MODE;
	cci_client->cci_device = 0;
	return 0;
}

static int quectel_cci_test_platform_probe(struct platform_device *pdev)
{
	pr_err("%s:%d E\n", __func__, __LINE__);
	g_dev = &pdev->dev;
	return 0;
}

static int quectel_cci_test_platform_remove(struct platform_device *pdev)
{
	pr_err("%s:%d E\n", __func__, __LINE__);
	return 0;
}

static const struct of_device_id quectel_cci_test_dt_match[] = {
	{ .compatible = "quectel,cci-test" },
	{ }
};
MODULE_DEVICE_TABLE(of, quectel_cci_test_dt_match);

static struct platform_driver quectel_cci_test_register_driver = {
	.probe = quectel_cci_test_platform_probe,
	.remove = quectel_cci_test_platform_remove,
	.driver = {
		.name = "cci-test",
		.owner = THIS_MODULE,
		.of_match_table = quectel_cci_test_dt_match,
	},
};

static int __init quectel_cci_test_init(void)
{
	struct dentry *dir;
	int ret;

	pr_err("%s:%d E\n", __func__, __LINE__);
	cci_test_master_info = kzalloc(sizeof(*cci_test_master_info), GFP_KERNEL);
	if (!cci_test_master_info) {
		pr_err("%s:%d alloc cci_test_master_info mem failed\n", __func__, __LINE__);
		return -ENOMEM;
	}
	if (quectel_cci_info_init(cci_test_master_info)) {
		pr_err("%s:%d quectel_cci_info_init failed\n", __func__, __LINE__);
		return -ENOMEM;
	}

	ret = quectel_cci_load_result();
	pr_err("%s:%d cci load result %d\n", __func__, __LINE__, ret);
	if (ret) {
		quectel_cci_test_probe(&g_test_data);
	} else {
		ret = quectel_cci_register_subdev(quectel_cci_test_probe, "cci-test", &g_test_data);
		pr_err("%s:%d res %d\n", __func__, __LINE__, ret);
		if (ret == -EAGAIN)
			quectel_cci_test_probe(&g_test_data);
	}

	dir = debugfs_create_dir("quectel-cci-test", NULL);
	debugfs_create_file("quectel_cci_test", 0644, dir, NULL, &quectel_cci_test);
	ret = platform_driver_register(&quectel_cci_test_register_driver);
	if (ret)
		pr_err("%s:%d platform_driver_register failed\n", __func__, __LINE__);
	else
		debugfs_create_file("quectel_gpio_test", 0644, dir, NULL, &quectel_gpio_test);
	return 0;
}

static void __exit quectel_cci_test_exit(void)
{
}

module_init(quectel_cci_test_init);
module_exit(quectel_cci_test_exit);

MODULE_DESCRIPTION("kernel cci driver  test");
MODULE_LICENSE("GPL v2");
