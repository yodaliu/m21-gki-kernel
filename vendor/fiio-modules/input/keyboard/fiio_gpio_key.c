// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO GPIO key: an input device that reports the media/volume/power key
 * codes other FiiO drivers send on the notifier_gpio_key chain.
 *
 * Reconstructed from the stock FiiO M21 FW 1.1.0 fiio_gpio_key.ko
 * (drivers/fiio_modules/input/keyboard/fiio_gpio_key.c).
 */
#define pr_fmt(fmt) "[fiio_gpio_key]: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/of.h>

#include <fiio/fiio_notify.h>

struct fiio_gpio_key {
	struct platform_device *pdev;
	struct device *dev;
	struct input_dev *input;
	struct notifier_block gpio_key_notifier;
	int cur_key_code;
	int cur_key_status;
	struct delayed_work report_event_dwork;
};

static struct fiio_gpio_key *g_fiio_gpio_key;

int g_key_code_list[] = {
	KEY_POWER,
	KEY_VOLUMEDOWN,
	KEY_VOLUMEUP,
	KEY_PLAYPAUSE,
	KEY_NEXTSONG,
	KEY_PREVIOUSSONG,
};

int g_key_num = ARRAY_SIZE(g_key_code_list);

static void fiio_gpio_key_report_event_work(struct work_struct *work)
{
	struct fiio_gpio_key *fiio_gpio_key =
		container_of(to_delayed_work(work), struct fiio_gpio_key,
			     report_event_dwork);
	int code = fiio_gpio_key->cur_key_code;
	struct input_dev *input = fiio_gpio_key->input;
	int status = fiio_gpio_key->cur_key_status;

	if (status == GPIO_KEY_DOWN || status == DOWN_AND_UP) {
		input_report_key(input, code, 1);
		input_sync(input);
	}

	if (status == GPIO_KEY_UP || status == DOWN_AND_UP) {
		input_report_key(input, code, 0);
		input_sync(input);
	}
}

static int gpio_key_notifier_cb(struct notifier_block *self,
				unsigned long event, void *data)
{
	struct fiio_gpio_key *gpio_key =
		container_of(self, struct fiio_gpio_key, gpio_key_notifier);
	struct gpio_key_event_notify_data *notify_data = data;

	if (!notify_data)
		return 0;

	switch (notify_data->event) {
	case NOTIFY_EVENT_KEY_CODE:
		gpio_key->cur_key_code = notify_data->gpio_key_code;
		gpio_key->cur_key_status = notify_data->status;
		schedule_delayed_work(&gpio_key->report_event_dwork, 0);
		break;
	default:
		break;
	}

	return 0;
}

static int fiio_gpio_key_input_dev_init(struct fiio_gpio_key *fiio_gpio_key)
{
	int ret;
	int i;
	struct input_dev *input;
	struct device *dev = fiio_gpio_key->dev;

	input = devm_input_allocate_device(dev);
	if (!input) {
		pr_info("%s,failed to allocate input device\n", __func__);
		return -ENOMEM;
	}

	input->dev.parent = dev;
	input->name = "fiio_gpio_key";
	input->phys = "fiio_gpio_key/input0";
	input->id.bustype = BUS_ISA;
	input->id.vendor = 0x0001;
	input->id.product = 0x0001;
	input->id.version = 0x0100;

	set_bit(EV_KEY, input->evbit);
	for (i = 0; i < g_key_num; i++) {
		set_bit(g_key_code_list[i], input->keybit);
		pr_info("Key %d registed.\n", g_key_code_list[i]);
	}

	ret = input_register_device(input);
	if (ret) {
		pr_info("%s,Unable to register input device, error: %d\n",
			__func__, ret);
		return ret;
	}

	fiio_gpio_key->input = input;
	return ret;
}

static int fiio_gpio_key_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct fiio_gpio_key *fiio_gpio_key;
	int ret;

	pr_info("[%s] enter ... \n", __func__);

	fiio_gpio_key = kzalloc(sizeof(*fiio_gpio_key), GFP_KERNEL);
	if (!fiio_gpio_key)
		return -ENOMEM;

	platform_set_drvdata(pdev, fiio_gpio_key);
	fiio_gpio_key->pdev = pdev;
	fiio_gpio_key->dev = dev;

	ret = fiio_gpio_key_input_dev_init(fiio_gpio_key);
	if (ret < 0) {
		pr_info("[%s] input dev init failed \n", __func__);
		goto INPUT_DEV_INIT_FAILED;
	}

	g_fiio_gpio_key = fiio_gpio_key;
	fiio_gpio_key->gpio_key_notifier.notifier_call = gpio_key_notifier_cb;
	fiio_register_client(notifier_gpio_key, &fiio_gpio_key->gpio_key_notifier);
	INIT_DELAYED_WORK(&fiio_gpio_key->report_event_dwork,
			  fiio_gpio_key_report_event_work);

	pr_info("[%s]  fiio_gpio_key_probe completed\n", __func__);
	return 0;

INPUT_DEV_INIT_FAILED:
	kfree(fiio_gpio_key);
	return ret;
}

static int fiio_gpio_key_remove(struct platform_device *pdev)
{
	struct fiio_gpio_key *fiio_gpio_key = platform_get_drvdata(pdev);

	input_unregister_device(fiio_gpio_key->input);
	input_free_device(fiio_gpio_key->input);
	kfree(fiio_gpio_key);

	return 0;
}

static int fiio_gpio_key_suspend(struct platform_device *pdev, pm_message_t state)
{
	return 0;
}

static int fiio_gpio_key_resume(struct platform_device *pdev)
{
	return 0;
}

static struct of_device_id fiio_gpio_key_drivers_dt_ids[] = {
	{ .compatible = "fiio,fiio_gpio_key" },
	{},
};
MODULE_DEVICE_TABLE(of, fiio_gpio_key_drivers_dt_ids);

static struct platform_driver fiio_gpio_key_driver = {
	.probe = fiio_gpio_key_probe,
	.remove = fiio_gpio_key_remove,
	.suspend = fiio_gpio_key_suspend,
	.resume = fiio_gpio_key_resume,
	.driver = {
		.name = "fiio_gpio_key",
		.owner = THIS_MODULE,
		.of_match_table = fiio_gpio_key_drivers_dt_ids,
	},
};

static int __init fiio_gpio_key_init(void)
{
	return platform_driver_register(&fiio_gpio_key_driver);
}

static void __exit fiio_gpio_key_exit(void)
{
	platform_driver_unregister(&fiio_gpio_key_driver);
}

module_init(fiio_gpio_key_init);
module_exit(fiio_gpio_key_exit);

MODULE_AUTHOR("mxc@fiio.net");
MODULE_DESCRIPTION("SW DECODER DIRVER");
MODULE_LICENSE("GPL");
