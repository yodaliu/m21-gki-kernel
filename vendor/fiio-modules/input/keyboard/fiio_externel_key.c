// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO external key detect: reports the accessory's keys on the
 * notifier_gpio_key chain (fiio_gpio_key turns them into input events)
 * and its hold / hall / long-press states as uevents.
 *
 * Reconstructed from the stock FiiO M21 FW 1.1.0 fiio_externel_key.ko
 * (drivers/fiio_modules/input/keyboard/fiio_externel_key.c).
 */
#define pr_fmt(fmt) "[EXT KEY]: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/iio/consumer.h>

#include <fiio/fiio_attr.h>
#include <fiio/fiio_notify.h>
#include <fiio/fiio_utils.h>

#include "fiio_externel_key.h"

static struct ext_key_data *g_ext_key_data;
static char buf1[28];
static char buf2[28];

/* GPIO_KEY_DOWN only presses; the other states also end the key sequence */
static void report_externel_key(int key_code, int down_status)
{
	struct gpio_key_event_notify_data key_event;
	struct ext_key_data *data = g_ext_key_data;
	key_event.event = NOTIFY_EVENT_KEY_CODE;
	key_event.status = down_status;
	key_event.gpio_key_code = key_code;
	fiio_notifier_call_chain(notifier_gpio_key, 0, &key_event);
	if (down_status != GPIO_KEY_DOWN) {
		data->adc_key_long_press = false;
		data->no_double_click = false;
		data->key_down = 0;
		data->downtime_count = 0;
		data->uptime_count = 0;
	}
}

static void report_externel_key_double_click(void)
{
	struct ext_key_data *data = g_ext_key_data;

	if (data->mark_key == KEY_NEXTSONG)
		strcpy(buf2, "EXT_KEY_TAPE_ADD=TRUE");
	else
		strcpy(buf2, "EXT_KEY_TAPE_RED=TRUE");
	strcpy(buf1, "EXT_KEY_TYPE=TRUE");
	fiio_sendMessage(data->dev, buf1, buf2);
	data->uptime_count = 0;
}

static void ext_key_long_press_work1_cancel(struct ext_key_data *data)
{
	int i;
	int ret;

	ret = work_busy(&data->long_press_work1.work);
	if (ret & WORK_BUSY_PENDING) {
		pr_err("%s: ext_key_long_press_work1_func WORK_BUSY_PENDING cancel it\n", __func__);
		cancel_delayed_work_sync(&data->long_press_work1);
	}

	if (ret & WORK_BUSY_RUNNING) {
		pr_err("%s: ext_key_long_press_work1_func WORK_BUSY_RUNNING wait it to finish\n", __func__);
		for (i = 0; i < 20; i++) {
			mdelay(10);
			if (!(work_busy(&data->long_press_work1.work) & WORK_BUSY_RUNNING))
				break;
		}
	}
}

static void ext_key_long_press_work2_cancel(struct ext_key_data *data)
{
	int i;
	int ret;

	ret = work_busy(&data->long_press_work2.work);
	if (ret & WORK_BUSY_PENDING) {
		pr_err("%s: ext_key_long_press_work2_func WORK_BUSY_PENDING cancel it\n", __func__);
		cancel_delayed_work_sync(&data->long_press_work2);
	}

	if (ret & WORK_BUSY_RUNNING) {
		pr_err("%s: ext_key_long_press_work2_func WORK_BUSY_RUNNING wait it to finish\n", __func__);
		for (i = 0; i < 20; i++) {
			mdelay(10);
			if (!(work_busy(&data->long_press_work2.work) & WORK_BUSY_RUNNING))
				break;
		}
	}
}

static void ext_key_det_work_cancel(struct ext_key_data *data)
{
	int i;
	int ret;

	ret = work_busy(&data->det_work.work);
	if (ret & WORK_BUSY_PENDING) {
		pr_err("%s: ext_key_det_work_cancel WORK_BUSY_PENDING cancel it\n", __func__);
		cancel_delayed_work_sync(&data->det_work);
	}

	if (ret & WORK_BUSY_RUNNING) {
		pr_err("%s: ext_key_det_work_cancel WORK_BUSY_RUNNING wait it to finish\n", __func__);
		for (i = 0; i < 20; i++) {
			mdelay(10);
			if (!(work_busy(&data->det_work.work) & WORK_BUSY_RUNNING))
				break;
		}
	}
}

/*
 * Polls the key ladder while the accessory is attached. downtime_count and
 * uptime_count count the polls with a key held and released.
 */
static void ext_key_det_work_func(struct work_struct *work)
{
	struct ext_key_data *data = container_of(work, struct ext_key_data, det_work.work);
	struct device *dev = data->dev;
	int adc_value = 0;
	int ret;

	if (!data->adc_det) {
		pr_err("[%s] adc_det get fail \n", __func__);
		data->adc_det = devm_iio_channel_get(dev, "ext_key_det");
		if (IS_ERR(data->adc_det)) {
			pr_err("[%s] adc_det get failed again!\n", __func__);
			data->adc_det = NULL;
		}
		return;
	}

	ret = iio_read_channel_processed(data->adc_det, &adc_value);

	if (adc_value < EXT_KEY_ADC_RELEASE) {
		if (data->downtime_count == 1) {
			if (adc_value > EXT_KEY_ADC_VOLUMEUP_MIN && adc_value < EXT_KEY_ADC_VOLUMEUP_MAX) {
				data->report_key = KEY_VOLUMEUP;
				report_externel_key(data->report_key, GPIO_KEY_DOWN);
			} else if (adc_value > EXT_KEY_ADC_VOLUMEDOWN_MIN && adc_value < EXT_KEY_ADC_VOLUMEDOWN_MAX) {
				data->report_key = KEY_VOLUMEDOWN;
				report_externel_key(data->report_key, GPIO_KEY_DOWN);
			} else if (adc_value > EXT_KEY_ADC_NEXTSONG_MIN && adc_value < EXT_KEY_ADC_NEXTSONG_MAX) {
				data->report_key = KEY_NEXTSONG;
			} else if (adc_value > EXT_KEY_ADC_PREVIOUSSONG_MIN && adc_value < EXT_KEY_ADC_PREVIOUSSONG_MAX) {
				data->report_key = KEY_PREVIOUSSONG;
			} else if (adc_value > EXT_KEY_ADC_PLAYPAUSE_MIN && adc_value < EXT_KEY_ADC_PLAYPAUSE_MAX) {
				data->report_key = KEY_PLAYPAUSE;
			}
			data->mark_key = data->report_key;
		} else if (data->downtime_count == 0) {
			data->key_down = 1;
		} else if (data->uptime_count > 1) {
			/* pressed again soon after a release: double click */
			if (adc_value > EXT_KEY_ADC_NEXTSONG_MIN && adc_value < EXT_KEY_ADC_NEXTSONG_MAX) {
				if (data->mark_key == KEY_NEXTSONG) {
					data->key_down = 2;
					report_externel_key_double_click();
				}
			} else if (adc_value > EXT_KEY_ADC_PREVIOUSSONG_MIN && adc_value < EXT_KEY_ADC_PREVIOUSSONG_MAX) {
				if (data->mark_key == KEY_PREVIOUSSONG) {
					data->key_down = 2;
					report_externel_key_double_click();
				}
			}
		}

		data->downtime_count++;

		if (data->mark_key == KEY_PLAYPAUSE && !data->adc_key_long_press &&
		    data->downtime_count > EXT_KEY_MY_FAV_COUNT) {
			strcpy(buf1, "EXT_KEY_TYPE=TRUE");
			strcpy(buf2, "EXT_KEY_MY_FAV=TRUE");
			fiio_sendMessage(data->dev, buf1, buf2);
			data->adc_key_long_press = true;
		}

		if ((data->mark_key == KEY_PREVIOUSSONG || data->mark_key == KEY_NEXTSONG) &&
		    data->key_down == 1 && data->downtime_count > EXT_KEY_LONG_PRESS_COUNT) {
			data->no_double_click = true;
			report_externel_key(data->mark_key, GPIO_KEY_DOWN);
		}
	} else {
		if (data->key_down == 2) {
			data->adc_key_long_press = false;
			data->no_double_click = false;
			data->key_down = 0;
			data->downtime_count = 0;
			data->uptime_count = 0;
		} else if (data->key_down == 1) {
			if (data->uptime_count == 1) {
				if (data->mark_key == KEY_NEXTSONG || data->mark_key == KEY_PREVIOUSSONG) {
					if (data->no_double_click)
						report_externel_key(data->mark_key, GPIO_KEY_UP);
				} else if (data->mark_key != KEY_PLAYPAUSE) {
					report_externel_key(data->mark_key, GPIO_KEY_UP);
				} else if (!data->adc_key_long_press) {
					report_externel_key(KEY_PLAYPAUSE, DOWN_AND_UP);
				} else {
					data->adc_key_long_press = false;
					data->no_double_click = false;
					data->key_down = 0;
					data->downtime_count = 0;
					data->uptime_count = 0;
				}
			}

			if (data->key_down)
				data->uptime_count++;

			if ((data->mark_key == KEY_PREVIOUSSONG || data->mark_key == KEY_NEXTSONG) &&
			    data->uptime_count > EXT_KEY_LONG_PRESS_COUNT)
				report_externel_key(data->mark_key, DOWN_AND_UP);
		}
	}

	schedule_delayed_work(&data->det_work, msecs_to_jiffies(EXT_KEY_DET_PERIOD_MS));
}

long prev_time_s;

static void ext_key_hold_irq_work_func(struct work_struct *work)
{
}

static void ext_key_hall_irq_work_func(struct work_struct *work)
{
	struct ext_key_data *data = container_of(work, struct ext_key_data, hall_irq_work.work);
	int value = gpio_get_value(data->hall_det);

	ext_key_det_work_cancel(data);
	if (!value) {
		data->hall_status = false;
		gpio_direction_output(data->detsw_en, 0);
	} else {
		data->hall_status = true;
		gpio_direction_output(data->detsw_en, 1);
		schedule_delayed_work(&data->det_work, msecs_to_jiffies(EXT_KEY_DET_PERIOD_MS));
	}

	if (data->tape_auto_det_en) {
		strcpy(buf1, "EXT_KEY_TYPE=TRUE");
		sprintf(buf2, "EXT_KEY_HALL=%s", data->hall_status ? "TRUE" : "FALSE");
		fiio_sendMessage(data->dev, buf1, buf2);
	}
}

static void ext_key_long_press_work1_func(struct work_struct *work)
{
	struct ext_key_data *data = container_of(work, struct ext_key_data, long_press_work1.work);

	data->long_press_happen = true;
	strcpy(buf1, "EXT_KEY_TYPE=TRUE");
	strcpy(buf2, "EXT_KEY_HOLD_LONGEXIT=TRUE");
	fiio_sendMessage(data->dev, buf1, buf2);
}

static void ext_key_long_press_work2_func(struct work_struct *work)
{
	struct ext_key_data *data = container_of(work, struct ext_key_data, long_press_work2.work);

	data->gpio_key_down = false;
	strcpy(buf1, "EXT_KEY_TYPE=TRUE");
	strcpy(buf2, "EXT_KEY_HOLD_LONGSHUT=TRUE");
	fiio_sendMessage(data->dev, buf1, buf2);
}

/* outside hold key: a short press is KEY_POWER, long presses go to userspace */
static irqreturn_t externel_key_irq_handler(int irq, void *data)
{
	struct ext_key_data *ext_key_data = data;
	int value = gpio_get_value(ext_key_data->outside_hold);

	if (!value) {
		ext_key_data->gpio_key_down = true;
		schedule_delayed_work(&ext_key_data->long_press_work1,
				      msecs_to_jiffies(EXT_KEY_HOLD_LONGEXIT_MS));
		schedule_delayed_work(&ext_key_data->long_press_work2,
				      msecs_to_jiffies(EXT_KEY_HOLD_LONGSHUT_MS));
	} else if (ext_key_data->gpio_key_down) {
		ext_key_long_press_work1_cancel(ext_key_data);
		ext_key_long_press_work2_cancel(ext_key_data);
		ext_key_data->gpio_key_down = false;
		if (!ext_key_data->long_press_happen)
			report_externel_key(KEY_POWER, DOWN_AND_UP);
		ext_key_data->long_press_happen = false;
	}

	return IRQ_HANDLED;
}

static irqreturn_t externel_hall_irq_handler(int irq, void *data)
{
	struct ext_key_data *ext_key_data = data;

	mod_delayed_work(system_wq, &ext_key_data->hall_irq_work,
			 msecs_to_jiffies(EXT_KEY_DET_PERIOD_MS));
	return IRQ_HANDLED;
}

static int fiio_externel_key_gpio_init(struct ext_key_data *data)
{
	struct device *dev = data->dev;
	struct device_node *np = dev->of_node;
	int ret;

	data->adc_det = devm_iio_channel_get(dev, "ext_key_det");
	if (IS_ERR(data->adc_det)) {
		pr_err("[%s] adc_det get failed!\n", __func__);
		data->adc_det = NULL;
	}

	data->detsw_en = of_get_named_gpio(np, "detsw_en", 0);
	if (gpio_is_valid(data->detsw_en)) {
		ret = gpio_request(data->detsw_en, "detsw_en");
		if (ret < 0) {
			pr_err("%s: (detsw_en) pin request fail\n", __func__);
			return ret;
		}
		gpio_direction_output(data->detsw_en, 0);
	} else {
		pr_err("[%s] get gpio failed :%d name:%s\n", __func__, data->detsw_en, "detsw_en");
	}

	data->outside_hold = of_get_named_gpio(np, "outside_hold_gpio", 0);
	if (gpio_is_valid(data->outside_hold)) {
		ret = gpio_request(data->outside_hold, "outside_hold");
		if (ret < 0) {
			pr_err("[%s] outside_hold (%d) gpio request fail\n", __func__, data->outside_hold);
			return ret;
		}
		gpio_direction_input(data->outside_hold);
		data->gpio_irq = gpio_to_irq(data->outside_hold);
		/* the result is not checked */
		ret = request_threaded_irq(data->gpio_irq, NULL, externel_key_irq_handler,
				     IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				     "externel_key_hold", data);
		enable_irq_wake(data->gpio_irq);
	}

	data->hall_det = of_get_named_gpio(np, "hall_det_gpio", 0);
	if (gpio_is_valid(data->hall_det)) {
		ret = gpio_request(data->hall_det, "hall_det");
		if (ret < 0) {
			pr_err("[%s] hall_det (%d) gpio request fail\n", __func__, data->hall_det);
			return ret;
		}
		gpio_direction_input(data->hall_det);
		data->hall_irq = gpio_to_irq(data->hall_det);
		ret = request_threaded_irq(data->hall_irq, NULL, externel_hall_irq_handler,
				     IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				     "externel_key_hall", data);
		enable_irq_wake(data->hall_irq);
	}

	return 0;
}

static ssize_t send_tape_type_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct ext_key_data *data = g_ext_key_data;
	int value = gpio_get_value(data->hall_det);

	pr_err("[%s] enter \n", __func__);
	if (size) {
		if (value == 1) {
			data->hall_status = true;
			gpio_direction_output(data->detsw_en, 1);
			ext_key_det_work_cancel(data);
			schedule_delayed_work(&data->det_work, msecs_to_jiffies(EXT_KEY_DET_PERIOD_MS));
		} else {
			data->hall_status = false;
			gpio_direction_output(data->detsw_en, 0);
		}

		if (data->tape_auto_det_en) {
			strcpy(buf1, "EXT_KEY_TYPE=TRUE");
			sprintf(buf2, "EXT_KEY_HALL=%s", value == 1 ? "TRUE" : "FALSE");
			fiio_sendMessage(data->dev, buf1, buf2);
		}
	}

	return size;
}

static ssize_t tape_auto_detect_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct ext_key_data *data = g_ext_key_data;

	data->tape_auto_det_en = atoi(buf);
	pr_err("[%s] enter val:%d\n", __func__, data->tape_auto_det_en);

	return size;
}

static const struct device_attribute externel_keys_attributes[] = {
	__ATTR(send_tape_type, 0200, NULL, send_tape_type_store),
	__ATTR(tape_auto_detect, 0200, NULL, tape_auto_detect_store),
};

static int fiio_externel_key_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ext_key_data *data;
	int ret;

	pr_err("[%s] enter ... \n", __func__);

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->downtime_count = 0;
	data->key_down = 0;
	data->no_double_click = false;
	data->gpio_key_down = false;
	data->long_press_happen = false;
	data->tape_auto_det_en = 0;
	data->hold_status = true;
	data->hall_status = false;
	data->adc_key_long_press = false;

	g_ext_key_data = data;
	platform_set_drvdata(pdev, data);
	data->pdev = pdev;
	data->dev = dev;

	if (dev->of_node)
		fiio_externel_key_gpio_init(data);

	INIT_DELAYED_WORK(&data->hold_irq_work, ext_key_hold_irq_work_func);
	INIT_DELAYED_WORK(&data->hall_irq_work, ext_key_hall_irq_work_func);
	INIT_DELAYED_WORK(&data->det_work, ext_key_det_work_func);
	INIT_DELAYED_WORK(&data->long_press_work1, ext_key_long_press_work1_func);
	INIT_DELAYED_WORK(&data->long_press_work2, ext_key_long_press_work2_func);

	ret = fiio_device_attribute_register(externel_keys_attributes,
					     ARRAY_SIZE(externel_keys_attributes));
	if (ret) {
		printk("attribute register failed!!");
		return -ENOMEM;
	}

	pr_err("[%s] end ... \n", __func__);
	return 0;
}

static int fiio_externel_key_remove(struct platform_device *pdev)
{
	fiio_device_attribute_unregister(externel_keys_attributes);
	return 0;
}

static struct of_device_id fiio_externel_key_dt_ids[] = {
	{ .compatible = "fiio,external_key" },
	{},
};
MODULE_DEVICE_TABLE(of, fiio_externel_key_dt_ids);

static struct platform_driver fiio_externel_key_driver = {
	.probe = fiio_externel_key_probe,
	.remove = fiio_externel_key_remove,
	.driver = {
		.name = "fiio_externel_key",
		.owner = THIS_MODULE,
		.of_match_table = fiio_externel_key_dt_ids,
	},
};

/* the driver registers a second after load, from its own thread */
struct task_struct *externel_key_task;
int externel_key_thread_func(void *data)
{
	msleep(1000);
	platform_driver_register(&fiio_externel_key_driver);
	return 0;
}

static int __init fiio_externel_key_init(void)
{
	pr_err("[%s] enter ... \n", __func__);
	externel_key_task = kthread_create(externel_key_thread_func, NULL, "my_kthread");
	wake_up_process(externel_key_task);
	return 0;
}

static void __exit fiio_externel_key_exit(void)
{
	pr_err("[%s] enter ... \n", __func__);
	platform_driver_unregister(&fiio_externel_key_driver);
}

module_init(fiio_externel_key_init);
module_exit(fiio_externel_key_exit);

MODULE_AUTHOR("Hango");
MODULE_DESCRIPTION("FiiO Externel Key Detect Driver");
MODULE_LICENSE("GPL");
