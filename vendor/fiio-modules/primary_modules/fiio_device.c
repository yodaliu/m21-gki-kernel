// SPDX-License-Identifier: GPL-2.0
/* Registry of FiiO drivers, looked up by name. */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>

#include <fiio/fiio_device.h>

LIST_HEAD(device_list);
EXPORT_SYMBOL(device_list);

static bool is_initialized;
static struct mutex lock;

int fiio_device_register(struct fiio_device_driver *drv)
{
	struct fiio_device *fdev;

	if (!is_initialized) {
		mutex_init(&lock);
		is_initialized = true;
	}

	fdev = kzalloc(sizeof(*fdev), GFP_KERNEL);
	if (!fdev)
		return -ENOMEM;

	fdev->driver = drv;
	mutex_lock(&lock);
	list_add(&fdev->list, &device_list);
	mutex_unlock(&lock);

	return 0;
}
EXPORT_SYMBOL(fiio_device_register);

void fiio_device_unregister(struct fiio_device_driver *drv)
{
	struct fiio_device *fdev;

	list_for_each_entry(fdev, &device_list, list) {
		if (fdev->driver == drv) {
			__list_del_entry(&fdev->list);
			kfree(fdev);
			return;
		}
	}
}
EXPORT_SYMBOL(fiio_device_unregister);

struct fiio_device_driver *fiio_device_get_by_name(const char *name)
{
	struct fiio_device *fdev;

	list_for_each_entry(fdev, &device_list, list) {
		if (!strcmp(fdev->driver->name, name))
			return fdev->driver;
	}

	return NULL;
}
EXPORT_SYMBOL(fiio_device_get_by_name);

void *fiio_device_get_data_by_name(const char *name)
{
	struct fiio_device_driver *driver;

	driver = fiio_device_get_by_name(name);
	if (!driver)
		return NULL;

	return driver->private_data;
}
EXPORT_SYMBOL(fiio_device_get_data_by_name);
