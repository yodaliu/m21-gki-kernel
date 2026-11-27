// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO underlay: waits until the core FiiO drivers have registered their
 * fiio_devices, then runs their load_work hooks (fiio_load.c) and creates
 * the sysfs attributes they registered (fiio_attr.c) on this device.
 * Also reports PM_RESUME=TRUE uevents.
 *
 * Reconstructed from the stock FiiO M21 FW 1.1.0 fiio_underlay_modules.ko
 * (drivers/fiio_modules/underlay_modules/).
 */
#define pr_fmt(fmt) "[fiio_underlay]: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/kobject.h>
#include <fiio/fiio_device.h>

#include "fiio_underlay.h"

/* poll period while waiting for the drivers below */
#define CHECK_DELAY_MS		20
/* settle time once they are all there */
#define CHECK_SETTLE_MS		3000

/*
 * The fiio_devices that must exist before the hooks run.
 */
static bool check_run_delay_task(void)
{
	if (!fiio_device_get_data_by_name("dac_dai module"))
		return false;

	if (!fiio_device_get_data_by_name("dac module"))
		return false;

	if (!fiio_device_get_data_by_name("fpga module"))
		return false;

	if (!fiio_device_get_data_by_name("fpga_device module"))
		return false;

	if (!fiio_device_get_data_by_name("fuel_gauge module"))
		return false;

	if (!fiio_device_get_data_by_name("fiio-charger-manager module"))
		return false;

	if (!fiio_device_get_data_by_name("power_board module"))
		return false;

	if (!fiio_device_get_data_by_name("anolog_board module"))
		return false;

	pr_err("[%s] return true\n", __func__);
	return true;
}

/*
 * Rescheduled every CHECK_DELAY_MS until check_run_delay_task() passes.
 */
struct delayed_work check_delay_work;
static void check_delay_work_func(struct work_struct *work)
{
	if (!check_run_delay_task()) {
		schedule_delayed_work(&check_delay_work, msecs_to_jiffies(CHECK_DELAY_MS));
		return;
	}

	mdelay(CHECK_SETTLE_MS);
	fiio_load_init();
	fiio_attr_init();
}

/* KOBJ_CHANGE uevent carrying buf */
void pm_status_event(struct device *dev, char *buf)
{
	char *envp[2] = { buf, NULL };

	pr_err("[%s] %s\n", __func__, buf);
	kobject_uevent_env(&dev->kobj, KOBJ_CHANGE, envp);
}

static int fiio_underlay_probe(struct platform_device *pdev)
{
	struct device *dev = NULL;
	int ret = 0;

	pr_err("%s start...", __func__);
	INIT_DELAYED_WORK(&check_delay_work, check_delay_work_func);
	schedule_delayed_work(&check_delay_work, msecs_to_jiffies(10));
	pr_err("%s end ", __func__);

	(void)dev;
	return ret;
}

static int underlay_suspend(struct device *dev)
{
	printk("[%s] enter\n", __func__);
	return 0;
}

static int underlay_resume(struct device *dev)
{
	printk("[%s] enter\n", __func__);
	pm_status_event(dev, "PM_RESUME=TRUE");
	return 0;
}

static SIMPLE_DEV_PM_OPS(underlay_pm_ops, underlay_suspend, underlay_resume);

static int fiio_underlay_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id fiio_underlay_dt_match[] = {
	{ .compatible = "fiio,fiio_underlay" },
	{},
};
MODULE_DEVICE_TABLE(of, fiio_underlay_dt_match);

static struct platform_driver fiio_underlay_driver = {
	.probe = fiio_underlay_probe,
	.remove = fiio_underlay_remove,
	.driver = {
		.name = "fiio_underlay",
		.owner = THIS_MODULE,
		.of_match_table = fiio_underlay_dt_match,
		.pm = &underlay_pm_ops,
	},
};

static int __init fiio_underlay_init(void)
{
	int ret;

	ret = platform_driver_register(&fiio_underlay_driver);
	if (ret)
		printk(KERN_ERR "unable to register platform device\n");

	return ret;
}
module_init(fiio_underlay_init);

static void __exit fiio_underlay_exit(void)
{
	platform_driver_unregister(&fiio_underlay_driver);
	fiio_load_exit();
	fiio_attr_exit();
}
module_exit(fiio_underlay_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("mxc@fiio.net");
MODULE_DESCRIPTION("fiio underlay driver");
