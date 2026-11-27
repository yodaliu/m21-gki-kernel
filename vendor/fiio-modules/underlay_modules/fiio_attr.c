// SPDX-License-Identifier: GPL-2.0
/*
 * Creates every attribute set on g_fiio_attrs_list (fiio_primary_modules)
 * on the fiio_attr platform device.
 */
#define pr_fmt(fmt) "[fiio_attr]: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <fiio/fiio_attr.h>

#include "fiio_underlay.h"

/*
 * A set that fails is rolled back and reported; the result is that of the
 * last set.
 */
static int fiio_device_attribute_create(struct device *dev)
{
	struct fiio_device_attribute *fattr;
	int error = 0;
	int i;

	list_for_each_entry(fattr, &g_fiio_attrs_list, list) {
		for (i = 0; i < fattr->num_attr; i++) {
			error = device_create_file(dev, &fattr->attrs[i]);
			if (error)
				break;
		}

		if (error) {
			printk("[%s] create file (%s) failed!\n", __func__, fattr->attrs->attr.name);
			while (--i >= 0)
				device_remove_file(dev, &fattr->attrs[i]);
		} else {
			printk("[%s] create file (%s) successed!\n", __func__, fattr->attrs->attr.name);
		}
	}

	return error;
}

static int fiio_device_attribute_remove(struct device *dev)
{
	struct fiio_device_attribute *fattr;
	int i;

	list_for_each_entry(fattr, &g_fiio_attrs_list, list) {
		for (i = 0; i < fattr->num_attr; i++)
			device_remove_file(dev, &fattr->attrs[i]);
	}

	return 0;
}

static int fiio_attr_remove(struct platform_device *pdev)
{
	fiio_device_attribute_remove(&pdev->dev);
	return 0;
}

static int fiio_attr_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret;

	pr_err("%s start...", __func__);

	ret = fiio_device_attribute_create(dev);
	if (ret)
		goto ATTR_ERROR;

	pr_err("%s end ", __func__);
	return 0;

ATTR_ERROR:
	pr_err("\033[40;31m attr register failed !!!\033[0m \n");
	fiio_device_attribute_remove(dev);
	return ret;
}

static const struct of_device_id fiio_attr_dt_match[] = {
	{ .compatible = "fiio,fiio_attr" },
	{},
};
MODULE_DEVICE_TABLE(of, fiio_attr_dt_match);

static struct platform_driver fiio_attr_driver = {
	.probe = fiio_attr_probe,
	.remove = fiio_attr_remove,
	.driver = {
		.name = "fiio_attr",
		.owner = THIS_MODULE,
		.of_match_table = fiio_attr_dt_match,
	},
};

int fiio_attr_init(void)
{
	int ret;

	ret = platform_driver_register(&fiio_attr_driver);
	if (ret)
		printk(KERN_ERR "unable to register platform device\n");

	return ret;
}
EXPORT_SYMBOL(fiio_attr_init);

void fiio_attr_exit(void)
{
	platform_driver_unregister(&fiio_attr_driver);
}
EXPORT_SYMBOL(fiio_attr_exit);

MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
MODULE_IMPORT_NS(CRYPTO_INTERNAL);
