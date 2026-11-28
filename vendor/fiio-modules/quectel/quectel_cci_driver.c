// SPDX-License-Identifier: GPL-2.0
/*
 * Quectel CCI bridge: camera.ko registers its CCI io functions here, and
 * other drivers use them through the quectel_cci_io_* exports. Drivers that
 * register a subdev are probed once the camera side has loaded.
 *
 * Reconstructed from the stock FiiO M21 FW 1.1.0 quectel_cci_driver.ko
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/mutex.h>

struct camera_io_master;
struct cam_sensor_i2c_reg_setting;

enum camera_sensor_i2c_type {
	CAMERA_SENSOR_I2C_TYPE_INVALID,
	CAMERA_SENSOR_I2C_TYPE_BYTE,
	CAMERA_SENSOR_I2C_TYPE_WORD,
	CAMERA_SENSOR_I2C_TYPE_3B,
	CAMERA_SENSOR_I2C_TYPE_DWORD,
	CAMERA_SENSOR_I2C_TYPE_MAX,
};

#include <linux/quectel_cci_cam.h>

struct quectel_cci_subdev {
	char *dev_name;
	struct list_head list;
	subdev_probe probe;
	void *data;
};

struct quectel_cci_res {
	bool load_result;
	struct quectel_cci_interface cci_interface;
	struct list_head subdev_list;
	struct mutex mutex_lock;
};

struct quectel_cci_res g_cci_res;

int32_t quectel_cci_io_dev_read(struct camera_io_master *io_master_info,
	uint32_t addr, uint32_t *data,
	enum camera_sensor_i2c_type addr_type,
	enum camera_sensor_i2c_type data_type)
{
	if (!g_cci_res.cci_interface.cci_io_dev_read) {
		pr_err("%s:%d cci_io_dev_read is NULL\n", __func__, __LINE__);
		return -ENOSYS;
	}
	return g_cci_res.cci_interface.cci_io_dev_read(io_master_info, addr, data,
						       addr_type, data_type);
}
EXPORT_SYMBOL(quectel_cci_io_dev_read);

int32_t quectel_cci_io_dev_read_seq(struct camera_io_master *io_master_info,
	uint32_t addr, uint8_t *data,
	enum camera_sensor_i2c_type addr_type,
	enum camera_sensor_i2c_type data_type,
	int32_t num_bytes)
{
	if (!g_cci_res.cci_interface.cci_io_dev_read_seq) {
		pr_err("%s:%d cci_io_dev_read_seq is NULL\n", __func__, __LINE__);
		return -ENOSYS;
	}
	return g_cci_res.cci_interface.cci_io_dev_read_seq(io_master_info, addr, data,
							   addr_type, data_type, num_bytes);
}
EXPORT_SYMBOL(quectel_cci_io_dev_read_seq);

int32_t quectel_cci_io_dev_write(struct camera_io_master *io_master_info,
	struct cam_sensor_i2c_reg_setting *write_setting)
{
	if (!g_cci_res.cci_interface.cci_io_dev_write) {
		pr_err("%s:%d cci_io_dev_write is NULL\n", __func__, __LINE__);
		return -ENOSYS;
	}
	return g_cci_res.cci_interface.cci_io_dev_write(io_master_info, write_setting);
}
EXPORT_SYMBOL(quectel_cci_io_dev_write);

int32_t quectel_cci_io_dev_write_continuous(struct camera_io_master *io_master_info,
	struct cam_sensor_i2c_reg_setting *write_setting,
	uint8_t cam_sensor_i2c_write_flag)
{
	if (!g_cci_res.cci_interface.cci_io_dev_write_continuous) {
		pr_err("%s:%d cci_io_dev_write_continuous is NULL\n", __func__, __LINE__);
		return -ENOSYS;
	}
	return g_cci_res.cci_interface.cci_io_dev_write_continuous(io_master_info, write_setting,
								   cam_sensor_i2c_write_flag);
}
EXPORT_SYMBOL(quectel_cci_io_dev_write_continuous);

int32_t quectel_cci_io_init(struct camera_io_master *io_master_info)
{
	if (!g_cci_res.cci_interface.cci_io_init) {
		pr_err("%s:%d cci_io_init is NULL\n", __func__, __LINE__);
		return -ENOSYS;
	}
	return g_cci_res.cci_interface.cci_io_init(io_master_info);
}
EXPORT_SYMBOL(quectel_cci_io_init);

int32_t quectel_cci_io_release(struct camera_io_master *io_master_info)
{
	if (!g_cci_res.cci_interface.cci_io_release) {
		pr_err("%s:%d cci_io_release is NULL\n", __func__, __LINE__);
		return -ENOSYS;
	}
	return g_cci_res.cci_interface.cci_io_release(io_master_info);
}
EXPORT_SYMBOL(quectel_cci_io_release);

bool quectel_cci_load_result(void)
{
	return g_cci_res.load_result;
}
EXPORT_SYMBOL(quectel_cci_load_result);

int quectel_register_cci_interface(struct quectel_cci_interface *cci_interface)
{
	struct quectel_cci_res *cci_res = &g_cci_res;
	struct quectel_cci_subdev *subdev;

	pr_err("%s:%d E\n", __func__, __LINE__);
	cci_res->cci_interface.cci_io_init = cci_interface->cci_io_init;
	cci_res->cci_interface.cci_io_release = cci_interface->cci_io_release;
	cci_res->cci_interface.cci_io_dev_write = cci_interface->cci_io_dev_write;
	cci_res->cci_interface.cci_io_dev_write_continuous = cci_interface->cci_io_dev_write_continuous;
	cci_res->cci_interface.cci_io_dev_read = cci_interface->cci_io_dev_read;
	cci_res->cci_interface.cci_io_dev_read_seq = cci_interface->cci_io_dev_read_seq;
	cci_res->cci_interface.test_func = cci_interface->test_func;

	mutex_lock(&cci_res->mutex_lock);
	cci_res->load_result = true;
	list_for_each_entry(subdev, &cci_res->subdev_list, list) {
		if (subdev->probe(subdev->data) < 0)
			pr_err("%s:%d device [%s] probe failed\n", __func__, __LINE__,
			       subdev->dev_name);
	}
	mutex_unlock(&cci_res->mutex_lock);
	return 0;
}
EXPORT_SYMBOL(quectel_register_cci_interface);

int quectel_cci_register_subdev(subdev_probe probe, const char *dev_name, void *data)
{
	struct quectel_cci_res *cci_res = &g_cci_res;
	struct quectel_cci_subdev *subdev;

	pr_err("%s:%d E\n", __func__, __LINE__);
	subdev = kzalloc(sizeof(*subdev), GFP_KERNEL);
	if (!subdev) {
		pr_err("%s:%d alloc cci_subdev mem failed\n", __func__, __LINE__);
		return -ENOMEM;
	}

	subdev->dev_name = kzalloc(strlen(dev_name), GFP_KERNEL);
	if (!subdev->dev_name) {
		pr_err("%s:%d alloc dev_name mem failed\n", __func__, __LINE__);
		return -ENOMEM;
	}
	subdev->probe = probe;
	subdev->data = data;
	memcpy(subdev->dev_name, dev_name, strlen(dev_name));

	mutex_lock(&cci_res->mutex_lock);
	if (cci_res->load_result) {
		mutex_unlock(&cci_res->mutex_lock);
		pr_err("%s:%d [WARN] cci already load success, please [%s] probe by oneself\n",
		       __func__, __LINE__, subdev->dev_name);
		kfree(subdev->dev_name);
		kfree(subdev);
		return -EAGAIN;
	}
	list_add_tail(&subdev->list, &cci_res->subdev_list);
	mutex_unlock(&cci_res->mutex_lock);
	return 0;
}
EXPORT_SYMBOL(quectel_cci_register_subdev);

static int __init quectel_cci_driver_init(void)
{
	struct quectel_cci_res *cci_res = &g_cci_res;

	pr_err("%s:%d E\n", __func__, __LINE__);
	memset(cci_res, 0, sizeof(*cci_res));
	mutex_init(&cci_res->mutex_lock);
	INIT_LIST_HEAD(&cci_res->subdev_list);
	return 0;
}

static void __exit quectel_cci_driver_exit(void)
{
	pr_err("%s:%d E\n", __func__, __LINE__);
}

module_init(quectel_cci_driver_init);
module_exit(quectel_cci_driver_exit);

MODULE_DESCRIPTION("kernel cci driver");
MODULE_LICENSE("GPL v2");
