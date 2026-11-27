// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO indicator LED: an RGB LED on three active-low GPIOs plus a supply GPIO.
 * Other FiiO drivers set the colour through the led_public_data ops handed out
 * as the "led module" fiio_device; userspace uses the sysfs attributes.
 *
 * Reconstructed from the stock FiiO M21 FW 1.1.0 led.ko
 * (drivers/fiio_modules/leds/led.c).
 */
#define pr_fmt(fmt) "[led]: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <fiio/fiio_attr.h>
#include <fiio/fiio_device.h>
#include <fiio/led_common.h>

/* the LED is lit while its GPIO is low */
struct led_device {
	struct device *dev;

	int led_green;
	int led_red;
	int led_blue;
	int led_vdd;

	enum led_status color;
	struct led_public_data *led_public_data;
	struct delayed_work led_suspend_delay_work;
	bool delay_work_lock;
	bool is_suspend;
	/* led_working_style: 0 keeps the LED off */
	bool led_working;
};

/*
 * GPIO levels per colour (blue/green/red, 0 = on):
 *   BLUE 0/1/1  GREEN 1/0/1  RED 1/1/0  CYAN 0/0/1
 *   PURPLE 0/1/0  YELLOW 1/0/0  WHILE 0/0/0  OTHER 1/1/1
 */

/* time the supply stays off after resume */
#define LED_RESUME_DELAY_MS	1500

struct led_device *g_data;

static void led_get_status(enum led_status *color)
{
	struct led_device *data = g_data;

	*color = data->color;
}

/*
 * led_color_set() drives the three colour GPIOs.  WHILE has no case and
 * leaves the LED as it is; with led_working cleared everything turns off.
 */

static void led_color_set(enum led_status color)
{
	struct led_device *data = g_data;

	if (!data->led_working)
		color = OTHER;

	switch (color) {
	case BLUE:
		gpio_direction_output(data->led_red, 1);
		gpio_direction_output(data->led_green, 1);
		gpio_direction_output(data->led_blue, 0);
		break;
	case GREEN:
		gpio_direction_output(data->led_blue, 1);
		gpio_direction_output(data->led_red, 1);
		gpio_direction_output(data->led_green, 0);
		break;
	case RED:
		gpio_direction_output(data->led_blue, 1);
		gpio_direction_output(data->led_green, 1);
		gpio_direction_output(data->led_red, 0);
		break;
	case CYAN:
		gpio_direction_output(data->led_red, 1);
		gpio_direction_output(data->led_blue, 0);
		gpio_direction_output(data->led_green, 0);
		break;
	case PURPLE:
		gpio_direction_output(data->led_green, 1);
		gpio_direction_output(data->led_blue, 0);
		gpio_direction_output(data->led_red, 0);
		break;
	case YELLOW:
		gpio_direction_output(data->led_blue, 1);
		gpio_direction_output(data->led_red, 0);
		gpio_direction_output(data->led_green, 0);
		break;
	case OTHER:
		gpio_direction_output(data->led_blue, 1);
		gpio_direction_output(data->led_red, 1);
		gpio_direction_output(data->led_green, 1);
		break;
	default:
		break;
	}
}

static void led_set_status(enum led_status color)
{
	struct led_device *data = g_data;

	data->color = color;
	pr_err("%s: color = %d\n", __func__, color);
	pr_err("%s: is_charging = %d\n", __func__, data->led_public_data->is_charging);

	/* while charging only RED gets through */
	if (color == RED || !data->led_public_data->is_charging)
		led_color_set(color);
}

static void led_open_regation(void)
{
	struct led_device *data = g_data;

	data->is_suspend = false;
	if (!data->delay_work_lock) {
		data->delay_work_lock = true;
		schedule_delayed_work(&data->led_suspend_delay_work,
				      msecs_to_jiffies(LED_RESUME_DELAY_MS));
	}
}

static void led_colse_regation(void)
{
	struct led_device *data = g_data;

	data->is_suspend = true;
	gpio_direction_output(data->led_vdd, 0);
}

/*
 * Request one LED GPIO and drive it high (off); -EBUSY is fine.
 */

static int led_gpio_init(struct led_device *data)
{
	struct device *dev = data->dev;
	struct device_node *np = dev->of_node;
	int ret;

	pr_err("%s: enter\n", __func__);

	data->led_green = of_get_named_gpio(np, "gpio,led_green", 0);
	if (gpio_is_valid(data->led_green)) {
		ret = gpio_request(data->led_green, "gpio,led_green");
		if (ret < 0 && ret != -EBUSY) {
			pr_err("%s: (led_green) pin request fail\n", __func__);
			return ret;
		}
		gpio_direction_output(data->led_green, 1);
	} else {
		pr_err("[%s] get gpio failed :%d name:%s\n", __func__, data->led_green, "gpio,led_green");
	}

	data->led_red = of_get_named_gpio(np, "gpio,led_red", 0);
	if (gpio_is_valid(data->led_red)) {
		ret = gpio_request(data->led_red, "gpio,led_red");
		if (ret < 0 && ret != -EBUSY) {
			pr_err("%s: (led_red) pin request fail\n", __func__);
			return ret;
		}
		gpio_direction_output(data->led_red, 1);
	} else {
		pr_err("[%s] get gpio failed :%d name:%s\n", __func__, data->led_red, "gpio,led_red");
	}

	data->led_blue = of_get_named_gpio(np, "gpio,led_blue", 0);
	if (gpio_is_valid(data->led_blue)) {
		ret = gpio_request(data->led_blue, "gpio,led_blue");
		if (ret < 0 && ret != -EBUSY) {
			pr_err("%s: (led_blue) pin request fail\n", __func__);
			return ret;
		}
		gpio_direction_output(data->led_blue, 1);
	} else {
		pr_err("[%s] get gpio failed :%d name:%s\n", __func__, data->led_blue, "gpio,led_blue");
	}

	data->led_vdd = of_get_named_gpio(np, "gpio,led_vdd", 0);
	if (gpio_is_valid(data->led_vdd)) {
		ret = gpio_request(data->led_vdd, "gpio,led_vdd");
		if (ret < 0 && ret != -EBUSY) {
			pr_err("%s: (led_vdd) pin request fail\n", __func__);
			return ret;
		}
		gpio_direction_output(data->led_vdd, 1);
	} else {
		pr_err("[%s] get gpio failed :%d name:%s\n", __func__, data->led_vdd, "gpio,led_vdd");
	}

	return 0;
}

static void led_suspend_delay_work_func(struct work_struct *work)
{
	struct led_device *data = container_of(work, struct led_device, led_suspend_delay_work.work);

	if (data) {
		if (!data->is_suspend)
			gpio_direction_output(data->led_vdd, 1);
		data->delay_work_lock = false;
	}
}

static ssize_t led_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct led_device *data = g_data;
	int red, green, blUe;

	blUe = gpio_get_value_cansleep(data->led_blue);
	green = gpio_get_value_cansleep(data->led_green);
	red = gpio_get_value_cansleep(data->led_red);

	if (!blUe && green && red)
		return sprintf(buf, "%s\n", "blUe");
	else if (blUe && !green && red)
		return sprintf(buf, "%s\n", "green");
	else if (blUe && green && !red)
		return sprintf(buf, "%s\n", "red");
	else if (!blUe && !green && red)
		return sprintf(buf, "%s\n", "cyan");
	else if (!blUe && green && !red)
		return sprintf(buf, "%s\n", "purple");
	else if (blUe && !green && !red)
		return sprintf(buf, "%s\n", "yellow");
	else if (!blUe && !green && !red)
		return sprintf(buf, "%s\n", "while");
	else
		return sprintf(buf, "%s\n", "other");
}

static ssize_t led_status_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	enum led_status color;

	if (size) {
		pr_err("[%s] %s \n", __func__, buf);
		if (!strcmp(buf, "blue\n") || !strcmp(buf, "1"))
			color = BLUE;
		else if (!strcmp(buf, "green\n") || !strcmp(buf, "6"))
			color = GREEN;
		else if (!strcmp(buf, "red\n") || !strcmp(buf, "0"))
			color = RED;
		else if (!strcmp(buf, "cyan\n") || !strcmp(buf, "2"))
			color = CYAN;
		else if (!strcmp(buf, "purple\n") || !strcmp(buf, "3"))
			color = PURPLE;
		else if (!strcmp(buf, "yellow\n") || !strcmp(buf, "4"))
			color = YELLOW;
		else if (!strcmp(buf, "white\n") || !strcmp(buf, "5"))
			color = WHILE;
		else
			color = OTHER;
		led_set_status(color);
	}

	return size;
}

static ssize_t led_working_style_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct led_device *data = g_data;
	u32 working_style = 0;
	int ret;

	ret = kstrtouint(buf, 0, &working_style);
	if (ret)
		return ret;

	if (working_style == 1) {
		data->led_working = true;
		led_color_set(data->led_public_data->is_charging ? RED : data->color);
	} else if (working_style == 0) {
		led_color_set(OTHER);
		data->led_working = false;
	}

	return size;
}

static ssize_t led_color_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct led_device *data = g_data;
	int red, green, blUe;

	blUe = gpio_get_value_cansleep(data->led_blue);
	green = gpio_get_value_cansleep(data->led_green);
	red = gpio_get_value_cansleep(data->led_red);

	if (!blUe && green && red)
		return sprintf(buf, "%s\n", "blUe");
	else if (blUe && !green && red)
		return sprintf(buf, "%s\n", "green");
	else if (blUe && green && !red)
		return sprintf(buf, "%s\n", "red");
	else if (!blUe && !green && red)
		return sprintf(buf, "%s\n", "cyan");
	else if (!blUe && green && !red)
		return sprintf(buf, "%s\n", "purple");
	else if (blUe && !green && !red)
		return sprintf(buf, "%s\n", "yellow");
	else if (!blUe && !green && !red)
		return sprintf(buf, "%s\n", "while");
	else
		return sprintf(buf, "%s\n", "other");
}

static ssize_t led_color_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	enum led_status color;

	if (size) {
		pr_err("[%s] %s \n", __func__, buf);
		if (!strcmp(buf, "blue\n") || !strcmp(buf, "1"))
			color = BLUE;
		else if (!strcmp(buf, "green\n") || !strcmp(buf, "6"))
			color = GREEN;
		else if (!strcmp(buf, "red\n") || !strcmp(buf, "0"))
			color = RED;
		else if (!strcmp(buf, "cyan\n") || !strcmp(buf, "2"))
			color = CYAN;
		else if (!strcmp(buf, "purple\n") || !strcmp(buf, "3"))
			color = PURPLE;
		else if (!strcmp(buf, "yellow\n") || !strcmp(buf, "4"))
			color = YELLOW;
		else if (!strcmp(buf, "white\n") || !strcmp(buf, "5"))
			color = WHILE;
		else
			color = OTHER;
		led_set_status(color);
	}

	return size;
}

static ssize_t led_blue_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct led_device *data = g_data;

	return sprintf(buf, "%d\n", gpio_get_value_cansleep(data->led_blue));
}

static ssize_t led_blue_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct led_device *data = g_data;
	int val = 0;
	ssize_t result = kstrtouint(buf, 0, (unsigned int *)&val);

	if (result < 0)
		return result;

	gpio_direction_output(data->led_blue, val);
	return size;
}

static ssize_t led_red_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct led_device *data = g_data;

	return sprintf(buf, "%d\n", gpio_get_value_cansleep(data->led_red));
}

static ssize_t led_red_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct led_device *data = g_data;
	int val = 0;
	ssize_t result = kstrtouint(buf, 0, (unsigned int *)&val);

	if (result < 0)
		return result;

	gpio_direction_output(data->led_red, val);
	return size;
}

static ssize_t led_green_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct led_device *data = g_data;

	return sprintf(buf, "%d\n", gpio_get_value_cansleep(data->led_green));
}

static ssize_t led_green_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct led_device *data = g_data;
	int val = 0;
	ssize_t result = kstrtouint(buf, 0, (unsigned int *)&val);

	if (result < 0)
		return result;

	gpio_direction_output(data->led_green, val);
	return size;
}

/* led_regation: 1 powers the LED supply, anything else cuts it */
static ssize_t led_regation_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct led_device *data = g_data;
	int val = 0;
	ssize_t result = kstrtouint(buf, 0, (unsigned int *)&val);

	if (result < 0)
		return result;

	if (val == 1)
		gpio_direction_output(data->led_vdd, 1);
	else
		gpio_direction_output(data->led_vdd, 0);
	return size;
}

static const struct device_attribute led_attributes[] = {
	__ATTR(led_status, 0644, led_status_show, led_status_store),
	__ATTR(led_blue, 0644, led_blue_show, led_blue_store),
	__ATTR(led_red, 0644, led_red_show, led_red_store),
	__ATTR(led_green, 0644, led_green_show, led_green_store),
	__ATTR(led_regation, 0200, NULL, led_regation_store),
	__ATTR(led_working_style, 0200, NULL, led_working_style_store),
	__ATTR(led_color, 0644, led_color_show, led_color_store),
};

static struct led_public_data led_ops = {
	.is_charging = false,
	.set_indicator_status = led_set_status,
	.get_indicator_status = led_get_status,
};

static struct fiio_device_driver led_driver = {
	.name = "led module",
	.private_data = &led_ops,
};

static int leds_platform_probe(struct platform_device *pdev)
{
	struct led_device *data;
	struct device *dev = &pdev->dev;
	struct led_public_data *led_public_data = &led_ops;
	int ret;

	/* stock prints the line numbers of its own source */
	pr_err("%s(%d)\n", __func__, 580);

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		goto ERROR;

	data->dev = dev;
	g_data = data;
	data->led_public_data = led_public_data;

	ret = led_gpio_init(data);
	if (ret) {
		pr_err("gpio init failed. \n");
		return ret;
	}

	ret = fiio_device_attribute_register(led_attributes, ARRAY_SIZE(led_attributes));
	if (ret) {
		printk("attribute register failed!!");
		goto ERROR;
	}

	data->delay_work_lock = false;
	data->is_suspend = false;
	data->led_working = true;
	led_set_status(BLUE);

	fiio_device_register(&led_driver);
	INIT_DELAYED_WORK(&data->led_suspend_delay_work, led_suspend_delay_work_func);

	pr_err("%s(%d) probe finished\n", __func__, 624);
	return 0;

ERROR:
	return -ENOMEM;
}

static int leds_platform_remove(struct platform_device *pdev)
{
	pr_err("[%s] enter ... \n", __func__);
	fiio_device_attribute_unregister(led_attributes);
	return 0;
}

static int leds_platform_suspend(struct platform_device *pdev, pm_message_t state)
{
	led_colse_regation();
	return 0;
}

static int leds_platform_resume(struct platform_device *pdev)
{
	led_open_regation();
	return 0;
}

static const struct of_device_id leds_dt_match[] = {
	{ .compatible = "fiio,leds" },
	{},
};
MODULE_DEVICE_TABLE(of, leds_dt_match);

static struct platform_driver leds_driver = {
	.probe = leds_platform_probe,
	.remove = leds_platform_remove,
	.suspend = leds_platform_suspend,
	.resume = leds_platform_resume,
	.driver = {
		.name = "led",
		.owner = THIS_MODULE,
		.of_match_table = leds_dt_match,
	},
};

static int __init leds_init(void)
{
	pr_err("[%s] enter ... \n", __func__);
	return platform_driver_register(&leds_driver);
}

static void __exit leds_exit(void)
{
	pr_err("[%s] enter ... \n", __func__);
	platform_driver_unregister(&leds_driver);
}

module_init(leds_init);
module_exit(leds_exit);

MODULE_AUTHOR("Aiden@fiio.net");
MODULE_DESCRIPTION("led");
MODULE_LICENSE("GPL v2");
