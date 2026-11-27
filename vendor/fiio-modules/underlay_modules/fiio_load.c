// SPDX-License-Identifier: GPL-2.0
/*
 * Runs the load_work hook of every fiio_device on device_list
 * (fiio_primary_modules) when the fiio_load platform device probes.
 */
#define pr_fmt(fmt) "[fiio_load]: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <fiio/fiio_device.h>

#include "fiio_underlay.h"

static int fiio_load_modules(void)
{
	struct fiio_device *fdev;
	int ret = 0;

	list_for_each_entry(fdev, &device_list, list) {
		if (fdev->driver->load_work)
			fdev->driver->load_work();
	}

	return ret;
}

static int fiio_load_probe(struct platform_device *pdev)
{
	struct device *dev = NULL;
	int ret = 0;

	pr_err("%s start...", __func__);
	fiio_load_modules();
	pr_err("%s end ", __func__);

	(void)dev;
	return ret;
}

static int fiio_load_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id fiio_load_dt_match[] = {
	{ .compatible = "fiio,fiio_load" },
	{},
};
MODULE_DEVICE_TABLE(of, fiio_load_dt_match);

static struct platform_driver fiio_load_driver = {
	.probe = fiio_load_probe,
	.remove = fiio_load_remove,
	.driver = {
		.name = "fiio_load",
		.owner = THIS_MODULE,
		.of_match_table = fiio_load_dt_match,
	},
};

int fiio_load_init(void)
{
	int ret;

	ret = platform_driver_register(&fiio_load_driver);
	if (ret)
		printk(KERN_ERR "unable to register platform device\n");

	return ret;
}
EXPORT_SYMBOL(fiio_load_init);

void fiio_load_exit(void)
{
	platform_driver_unregister(&fiio_load_driver);
}
EXPORT_SYMBOL(fiio_load_exit);

MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
MODULE_IMPORT_NS(CRYPTO_INTERNAL);
