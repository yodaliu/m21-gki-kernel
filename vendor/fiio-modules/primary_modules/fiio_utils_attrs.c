// SPDX-License-Identifier: GPL-2.0
/* Registry of sysfs attribute arrays, created on the FiiO device by its owner. */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/cdev.h>

#include <fiio/fiio_attr.h>

LIST_HEAD(g_fiio_attrs_list);
EXPORT_SYMBOL(g_fiio_attrs_list);

static bool is_initialized;
static struct mutex lock;

int fiio_device_attribute_register(const struct device_attribute *attrs,
				   unsigned int num_attr)
{
	struct fiio_device_attribute *fattr;

	if (!is_initialized) {
		mutex_init(&lock);
		is_initialized = true;
	}

	fattr = kzalloc(sizeof(*fattr), GFP_KERNEL);
	if (!fattr)
		return -ENOMEM;

	printk("[%s] fiio register attr %s!\n", __func__, attrs->attr.name);
	fattr->attrs = attrs;
	fattr->num_attr = num_attr;
	mutex_lock(&lock);
	list_add(&fattr->list, &g_fiio_attrs_list);
	mutex_unlock(&lock);

	return 0;
}
EXPORT_SYMBOL(fiio_device_attribute_register);

void fiio_device_attribute_unregister(const struct device_attribute *attrs)
{
	struct fiio_device_attribute *fattr;

	list_for_each_entry(fattr, &g_fiio_attrs_list, list) {
		if (fattr->attrs == attrs) {
			__list_del_entry(&fattr->list);
			kfree(fattr);
			return;
		}
	}
}
EXPORT_SYMBOL(fiio_device_attribute_unregister);
