// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO SW HOLD: the hold switch GPIO, reported as SW_HOLD=<state> uevents,
 * plus the per-key hold mask userspace writes to sw_hold_ctrl, which is
 * forwarded on the notifier_sw_hold chain.
 *
 * Reconstructed from the stock FiiO M21 FW 1.1.0 fiio_sw_hold.ko
 * (drivers/fiio_modules/input/keyboard/fiio_sw_hold.c).
 */
#define pr_fmt(fmt) "[SW HOLD]: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/kobject.h>

#include <fiio/fiio_attr.h>
#include <fiio/fiio_notify.h>

#define SW_HOLD_CHILDREN_NUM	10

/* bit n of hold_value, in sw_hold_notify_data bitfield order */
static char *children_hold_name[SW_HOLD_CHILDREN_NUM] = {
	"HOLD SW",
	"HOLD KEY PRE",
	"HOLD KEY NEXT",
	"HOLD KEY PAUSE",
	"HOLD KEY VOLUP",
	"HOLD KEY VOLDOWN",
	"HOLD KEY FUNC",
	"HOLD KEY TP",
	"HOLD KEY VOLKNOB",
	"HOLD KEY SCREENOFF",
};

struct sw_children_hold {
	unsigned int value;
	bool is_change;
};

struct sw_hold {
	struct platform_device *pdev;
	struct device *dev;
	struct delayed_work report_work;
	struct delayed_work notify_work;
	struct input_dev *input_dev;
	struct notifier_block fiio_system_status_notifier;
	int gpio;
	unsigned int hold_value;
	struct sw_children_hold node[SW_HOLD_CHILDREN_NUM];
};

static struct sw_hold *g_sw_hold;

/*
 * SW_HOLD=<value> uevent on the platform device
 */



void report_sw_hold(struct sw_hold *sw_hold, int value)
{
	char buf[25] = { 0 };
	char *envp[2] = { buf, NULL };

	sprintf(buf, "SW_HOLD=%d", value);
	kobject_uevent_env(&sw_hold->dev->kobj, KOBJ_CHANGE, envp);
}

/* asks userspace to re-read the hold state */


void report_sw_hold_key_check(void)
{
	char buf[25] = "SW_HOLD_KEY_CHECK=1";
	char *envp[2] = { buf, NULL };

	kobject_uevent_env(&g_sw_hold->dev->kobj, KOBJ_CHANGE, envp);
}
EXPORT_SYMBOL_GPL(report_sw_hold_key_check);

static int sw_hold_input_dev_init(struct sw_hold *sw_hold)
{
	int ret;
	struct input_dev *input;
	struct device *dev = sw_hold->dev;

	input = devm_input_allocate_device(dev);
	if (!input) {
		dev_err(dev, "%s,failed to allocate input device\n", __func__);
		return -ENOMEM;
	}

	input->dev.parent = dev;
	input->name = sw_hold->pdev->name;
	input->phys = "sw_hold";
	input_set_capability(input, EV_SW, SW_LID);

	ret = input_register_device(input);
	if (ret) {
		dev_err(dev, "%s,Unable to register input device, error: %d\n", __func__, ret);
		return ret;
	}

	sw_hold->input_dev = input;
	return 0;
}

static int sw_hold_gpio_init(struct sw_hold *sw_hold)
{
	int ret;

	sw_hold->gpio = of_get_named_gpio(sw_hold->dev->of_node, "gpios", 0);
	if (!gpio_is_valid(sw_hold->gpio))
		return -1;

	ret = gpio_request(sw_hold->gpio, "sw_hold_gpio");
	if (ret < 0) {
		pr_info("sw_hold_gpio request failed!!\n");
		return ret;
	}
	gpio_direction_input(sw_hold->gpio);

	return ret;
}

static void sw_hold_report_event(struct sw_hold *sw_hold)
{
	int state = gpio_get_value_cansleep(sw_hold->gpio);

	pr_info("[%s] state = %d \n", __func__, state);
	if (state < 0) {
		pr_info("failed to get sw hold gpio state: %d\n", state);
	} else {
		pr_info("sw hold gpio state: %d\n", state);
		report_sw_hold(sw_hold, state);
	}
}

static void sw_hold_report_work_func(struct work_struct *work)
{
	struct sw_hold *sw_hold = container_of(work, struct sw_hold, report_work.work);

	pr_info("[%s] enter \n", __func__);
	sw_hold_report_event(sw_hold);
}

static irqreturn_t sw_hold_irq_handler(int irq, void *data)
{
	struct sw_hold *sw_hold = data;

	pr_info("[%s] enter \n", __func__);
	mod_delayed_work(system_wq, &sw_hold->report_work, msecs_to_jiffies(15));
	return IRQ_HANDLED;
}

static int sw_hold_irq_registrationt(struct sw_hold *sw_hold)
{
	int ret;

	ret = request_threaded_irq(gpio_to_irq(sw_hold->gpio), NULL, sw_hold_irq_handler,
				   IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				   "sw_hold", sw_hold);
	return ret;
}

static int fiio_system_status_event_notifier_cb(struct notifier_block *self,
						unsigned long event, void *data)
{
	struct sw_hold *sw_hold = container_of(self, struct sw_hold, fiio_system_status_notifier);
	struct fiio_system_status_notify_data *notify_data = data;

	if (notify_data && (notify_data->event & EVENT_ANDROID_LOCKED_BOOT_COMPLETED))
		sw_hold_report_event(sw_hold);

	return 0;
}

static void sw_hold_notify_work_func(struct work_struct *work)
{
	struct sw_hold *sw_hold = container_of(work, struct sw_hold, notify_work.work);
	int ii;
	int event = 0;
	struct sw_hold_notify_data notify_data = { 0 };

	notify_data.hold_sw = sw_hold->node[0].value;
	notify_data.hold_key_pre = sw_hold->node[1].value;
	notify_data.hold_key_next = sw_hold->node[2].value;
	notify_data.hold_key_pause = sw_hold->node[3].value;
	notify_data.hold_key_volup = sw_hold->node[4].value;
	notify_data.hold_key_voldown = sw_hold->node[5].value;
	notify_data.hold_key_func = sw_hold->node[6].value;
	notify_data.hold_key_tp = sw_hold->node[7].value;
	notify_data.hold_key_volknob = sw_hold->node[8].value;
	notify_data.hold_key_screenoff = sw_hold->node[9].value;

	for (ii = 0; ii < SW_HOLD_CHILDREN_NUM; ii++) {
		if (sw_hold->node[ii].is_change) {
			event |= 1 << ii;
			sw_hold->node[ii].is_change = false;
		}
	}

	pr_info("%s, event : 0x%0x.\n", __func__, event);
	notify_data.event = event;
	fiio_notifier_call_chain(notifier_sw_hold, 0, &notify_data);
}

static void update_children_hold_value(struct sw_hold *sw_hold, int which)
{
	unsigned int new_value = (sw_hold->hold_value >> which) & 1;
	unsigned int old_value = sw_hold->node[which].value;

	if (new_value != old_value) {
		sw_hold->node[which].value = new_value;
		sw_hold->node[which].is_change = true;
	}
	pr_info("%s, (%s)children hold change(%s),value: %d --> %d.\n", __func__,
		children_hold_name[which], sw_hold->node[which].is_change ? "true" : "false",
		old_value, new_value);
}

static void parse_hold_value(struct sw_hold *sw_hold)
{
	update_children_hold_value(sw_hold, 0);
	update_children_hold_value(sw_hold, 1);
	update_children_hold_value(sw_hold, 2);
	update_children_hold_value(sw_hold, 3);
	update_children_hold_value(sw_hold, 4);
	update_children_hold_value(sw_hold, 5);
	update_children_hold_value(sw_hold, 6);
	update_children_hold_value(sw_hold, 7);
	update_children_hold_value(sw_hold, 8);
	update_children_hold_value(sw_hold, 9);
}

static ssize_t sw_hold_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct sw_hold *sw_hold = g_sw_hold;

	unsigned int val = 0;
	ssize_t result = kstrtouint(buf, 0, &val);

	if (result < 0)
		return result;

	pr_info("%s, hold_value : 0x%0x.\n", __func__, val);
	sw_hold->hold_value = val;
	parse_hold_value(sw_hold);
	schedule_delayed_work(&sw_hold->notify_work, 0);
	return count;
}

static ssize_t send_hold_type_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct sw_hold *sw_hold = g_sw_hold;

	pr_info("%s, enter.\n", __func__);
	if (size)
		sw_hold_report_event(sw_hold);

	return size;
}

static const struct device_attribute sw_hold_attrs[] = {
	__ATTR(sw_hold_ctrl, 0200, NULL, sw_hold_store),
	__ATTR(send_hold_type, 0200, NULL, send_hold_type_store),
};

static int sw_hold_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	struct sw_hold *sw_hold;
	int ret;

	pr_info("[%s] enter ... \n", __func__);

	sw_hold = kzalloc(sizeof(*sw_hold), GFP_KERNEL);
	if (!sw_hold)
		return -ENOMEM;

	platform_set_drvdata(pdev, sw_hold);
	sw_hold->pdev = pdev;
	sw_hold->dev = dev;
	g_sw_hold = sw_hold;

	INIT_DELAYED_WORK(&sw_hold->report_work, sw_hold_report_work_func);
	INIT_DELAYED_WORK(&sw_hold->notify_work, sw_hold_notify_work_func);

	ret = sw_hold_input_dev_init(sw_hold);
	if (ret) {
		pr_info("%s,input device register failed\n", __func__);
		goto INPUT_DEV_FAILED;
	}

	ret = sw_hold_gpio_init(sw_hold);
	if (ret) {
		pr_info("%s,gpio init failed\n", __func__);
		goto GPIO_INIT_FAILED;
	}

	ret = sw_hold_irq_registrationt(sw_hold);
	if (ret) {
		pr_info("%s,request irq failed\n", __func__);
		goto IRQ_REGISTER_FAILED;
	}

	ret = fiio_device_attribute_register(sw_hold_attrs, ARRAY_SIZE(sw_hold_attrs));
	if (ret) {
		pr_info("sysfs_create_group() failed!!");
		goto ATTR_FAILED;
	}

	sw_hold->fiio_system_status_notifier.notifier_call = fiio_system_status_event_notifier_cb;
	/* stock registers on chain 7 (notifier_monitor), not notifier_system_status */
	fiio_register_client(notifier_monitor, &sw_hold->fiio_system_status_notifier);

	pr_info("[%s] sucessful ... \n", __func__);
	return 0;

ATTR_FAILED:
	fiio_device_attribute_unregister(sw_hold_attrs);
	free_irq(gpio_to_irq(sw_hold->gpio), sw_hold);
IRQ_REGISTER_FAILED:
	gpio_free(sw_hold->gpio);
GPIO_INIT_FAILED:
	input_unregister_device(sw_hold->input_dev);
INPUT_DEV_FAILED:
	kfree(sw_hold);
	return ret;
}

static int sw_hold_remove(struct platform_device *pdev)
{
	return 0;
}

static int sw_hold_suspend(struct platform_device *pdev, pm_message_t state)
{
	return 0;
}
static int sw_hold_resume(struct platform_device *pdev)
{
	return 0;
}

static struct of_device_id sw_hold_drivers_dt_ids[] = {
	{ .compatible = "fiio,sw_hold" },
	{},
};
MODULE_DEVICE_TABLE(of, sw_hold_drivers_dt_ids);

static struct platform_driver sw_hold_driver = {
	.probe = sw_hold_probe,
	.remove = sw_hold_remove,
	.suspend = sw_hold_suspend,
	.resume = sw_hold_resume,
	.driver = {
		.name = "sw_hold",
		.owner = THIS_MODULE,
		.of_match_table = sw_hold_drivers_dt_ids,
	},
};

static int __init sw_hold_init(void)
{
	pr_info("[%s] enter ... \n", __func__);
	return platform_driver_register(&sw_hold_driver);
}

static void __exit sw_hold_exit(void)
{
	platform_driver_unregister(&sw_hold_driver);
}

module_init(sw_hold_init);
module_exit(sw_hold_exit);

MODULE_AUTHOR("Tiben");
MODULE_DESCRIPTION("SW HOLD DIRVER");
MODULE_LICENSE("GPL");
