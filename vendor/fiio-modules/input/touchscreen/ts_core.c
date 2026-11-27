// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO touchscreen core: picks the touch panel that probed (ts_core_get_vaild_tp()
 * from fiio_primary_modules) and exposes its name, gesture and screen-off controls.
 */
#define pr_fmt(fmt) "[ts_core]: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <fiio/fiio_attr.h>
#include <fiio/fiio_device.h>
#include <fiio/ts_common.h>

#define EX_ERR(fmt, ...)	printk(KERN_ERR "[EX]: " fmt, ##__VA_ARGS__)

/* times ts_core_load_work() polls for a panel, 5 s apart */
#define TS_LOAD_TRY_MAX		5
#define TS_LOAD_RETRY_MS	5000

/* written by the panel drivers' probe through ts_core_get_vaild_tp() */


struct ts_core_pdata {
	struct device *dev;
	struct ts_device *device_used;
	struct ts_public_data *ts_public;
	struct delayed_work load_work;
};

static struct ts_core_pdata *g_data;

static char *get_vaild_tp_name(void)
{
	if (!g_data || !g_data->device_used)
		return "";

	return g_data->device_used->name;
}

static bool is_gesture_wakeup(void)
{
	if (!g_data || !g_data->device_used)
		return false;

	return g_data->device_used->get_gesture_enable();
}

static ssize_t touchscreen_name_show(struct device *dev, struct device_attribute *attr,
				     char *buf)
{
	struct ts_core_pdata *data = g_data;

	sprintf(buf, "%s\n", data->device_used ? data->device_used->name : "NULL");

	return strlen(buf) + 2;
}

static ssize_t gesture_enable_store(struct device *dev, struct device_attribute *attr,
				    const char *buf, size_t size)
{
	struct ts_core_pdata *data = g_data;

	if (data->device_used && data->device_used->gesture_enable) {
		if (buf[0] == '0') {
			pr_err("[%s] disable gesture !", __func__);
			data->device_used->gesture_enable(false);
		} else if (buf[0] == '1') {
			pr_err("[%s] enable gesture !", __func__);
			data->device_used->gesture_enable(true);
		}
	}

	return size;
}

static ssize_t screen_off_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t size)
{
	struct ts_core_pdata *data = g_data;

	if (data->device_used && data->device_used->screen_off) {
		if (buf[0] == '0') {
			pr_err("[%s] screen on !", __func__);
			data->device_used->screen_off(false);
		} else if (buf[0] == '1') {
			pr_err("[%s] screen off !", __func__);
			data->device_used->screen_off(true);
		}
	}

	return size;
}

static const struct device_attribute ts_core_attributes[] = {
	__ATTR(ts_touchscreen_name, 0444, touchscreen_name_show, NULL),
	__ATTR(ts_gesture_enable, 0200, NULL, gesture_enable_store),
	__ATTR(ts_screen_off, 0200, NULL, screen_off_store),
};

/*
 * Run once from the fiio_device load_work hook, then again from the delayed
 * work until a panel turns up or TS_LOAD_TRY_MAX tries have failed.
 */
static void ts_core_load_work(void)
{
	struct ts_core_pdata *data = g_data;
	static int try_count;

	pr_err("[%s] Enter!\n", __func__);

	data->device_used = ts_core_get_vaild_tp();
	if (data->device_used) {
		pr_err("[%s] get ts device success!\n", __func__);
	} else if (try_count < TS_LOAD_TRY_MAX) {
		try_count++;
		pr_err("[%s] get ts device failed, try %d!\n", __func__, try_count);
		schedule_delayed_work(&data->load_work, msecs_to_jiffies(TS_LOAD_RETRY_MS));
		return;
	} else {
		pr_err("[%s] get ts device failed, try %d!\n", __func__, TS_LOAD_TRY_MAX);
	}

	try_count = 0;
}

static void ts_core_load_delay_work(struct work_struct *work)
{
	pr_err("%s: Enter!\n", __func__);
	ts_core_load_work();
}

static struct ts_public_data ts_public_d = {
	.get_vaild_tp_name = get_vaild_tp_name,
	.is_gesture_wakeup = is_gesture_wakeup,
};

static struct fiio_device_driver ts_device_driver = {
	.name = "ts module",
	.private_data = &ts_public_d,
	.load_work = ts_core_load_work,
};

static int ts_core_driver_probe(struct platform_device *pdev)
{
	struct ts_core_pdata *data;
	int i = 0, ret;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;

	pr_err("[%s] enter!\n", __func__);

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	g_data = data;
	if (!data) {
		dev_err(dev, "led_logic_platform_data kzalloc failed.\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, data);
	data->dev = dev;

	ret = fiio_device_attribute_register(ts_core_attributes,
					     ARRAY_SIZE(ts_core_attributes));
	if (ret) {
		EX_ERR("attribute register failed!!");
		goto error;
	}
	EX_ERR("attribute register succeeded!!");

	INIT_DELAYED_WORK(&data->load_work, ts_core_load_delay_work);
	data->ts_public = &ts_public_d;

	fiio_device_register(&ts_device_driver);

	pr_err("[%s] exit!\n", __func__);

	(void)i;
	(void)np;

	return 0;












error:
	fiio_device_attribute_unregister(ts_core_attributes);
	return ret;
}

static int ts_core_driver_remove(struct platform_device *pdev)
{
	fiio_device_attribute_unregister(ts_core_attributes);
	fiio_device_unregister(&ts_device_driver);

	return 0;
}

static const struct of_device_id ts_core_dt_match[] = {
	{ .compatible = "fiio,ts_core" },
	{},
};

static struct platform_driver ts_core_driver = {
	.probe = ts_core_driver_probe,
	.remove = ts_core_driver_remove,
	.driver = {
		.name = "ts_core_driver",
		.owner = THIS_MODULE,
		.of_match_table = ts_core_dt_match,
	},
};

static int __init ts_core_init(void)
{
	int ret;

	ret = platform_driver_register(&ts_core_driver);
	return ret;
}

static void __exit ts_core_exit(void)
{
	platform_driver_unregister(&ts_core_driver);
}

module_init(ts_core_init);
module_exit(ts_core_exit);
MODULE_AUTHOR("mxc@fiio.net");
MODULE_DESCRIPTION("touch screen switch driver");
MODULE_LICENSE("GPL v2");
