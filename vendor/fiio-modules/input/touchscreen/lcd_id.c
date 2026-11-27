// SPDX-License-Identifier: GPL-2.0
/* FiiO LCD ID: reads the panel ID straps and names the matching lcm-panel-node child. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <fiio/fiio_attr.h>
#include <fiio/fiio_utils.h>

struct lcd_id_data {
	struct device *dev;

	int lcd_id0;
	int lcd_id1;
	int lcd_id2;
	int lcd_id3;
};
static struct lcd_id_data *g_data;

static void update_lcd_information(struct lcd_id_data *data)
{
	struct device_node *lcm_panel_node;
	struct device_node *child;
	int lcd_id_value[4] = { 0 };
	int ret;

	/* the panel straps as read now */
	int hw_lcd_id_value[4] = {
		gpio_get_value(g_data->lcd_id0),
		gpio_get_value(g_data->lcd_id1),
		gpio_get_value(g_data->lcd_id2),
		gpio_get_value(g_data->lcd_id3),
	};

	printk("%s:hw_panel=%d:%d:%d:%d hw_panel-value=%d:%d:%d:%d!\n", __func__,
	       g_data->lcd_id0, g_data->lcd_id1, g_data->lcd_id2, g_data->lcd_id3,
	       hw_lcd_id_value[0], hw_lcd_id_value[1],
	       hw_lcd_id_value[2], hw_lcd_id_value[3]);

	lcm_panel_node = of_find_node_by_name(NULL, "lcm-panel-node");
	if (!lcm_panel_node) {
		printk("%s:can't find qcom,lcm-panel-node!\n", __func__);
		return;
	}

	for_each_available_child_of_node(lcm_panel_node, child) {
		const char *lcd_name;

		lcd_name = of_get_property(child, "panel-name", NULL);
		ret = of_property_read_u32_array(child, "panel-value",
						 lcd_id_value, 4);
		printk("%s:check lcd_name=%s %d:%d:%d:%d!\n", __func__, lcd_name,
		       lcd_id_value[0], lcd_id_value[1],
		       lcd_id_value[2], lcd_id_value[3]);
		if (hw_lcd_id_value[0] == lcd_id_value[0] &&
		    hw_lcd_id_value[1] == lcd_id_value[1] &&
		    hw_lcd_id_value[2] == lcd_id_value[2] &&
		    hw_lcd_id_value[3] == lcd_id_value[3]) {
			printk("%s:lcd_name=%s\n", __func__, lcd_name);
			set_product_lcd_panel_name(lcd_name);
			break;
		}
	}
}

static int lcd_id_init_gpio(struct lcd_id_data *data)
{
	struct device_node *np = data->dev->of_node;
	int ret;

	data->lcd_id0 = of_get_named_gpio(np, "lcd,id_0", 0);
	if (gpio_is_valid(data->lcd_id0)) {
		ret = gpio_request(data->lcd_id0, "lcd_id0");
		if (ret < 0) {
			pr_err("%s: lcd_id0 (%d) gpio request fail\n", __func__, data->lcd_id0);
			return ret;
		}
		gpio_direction_input(data->lcd_id0);
		printk("%s: lcd_id0[%d] :  %d\n", __func__, data->lcd_id0, gpio_get_value(data->lcd_id0));
	}

	data->lcd_id1 = of_get_named_gpio(np, "lcd,id_1", 0);
	if (gpio_is_valid(data->lcd_id1)) {
		ret = gpio_request(data->lcd_id1, "lcd_id1");
		if (ret < 0) {
			pr_err("%s: lcd_id1 (%d) gpio request fail\n", __func__, data->lcd_id1);
			return ret;
		}
		gpio_direction_input(data->lcd_id1);
		printk("%s: lcd_id1[%d] :  %d\n", __func__, data->lcd_id1, gpio_get_value(data->lcd_id1));
	}

	data->lcd_id2 = of_get_named_gpio(np, "lcd,id_2", 0);
	if (gpio_is_valid(data->lcd_id2)) {
		ret = gpio_request(data->lcd_id2, "lcd_id2");
		if (ret < 0) {
			pr_err("%s: lcd_id2 (%d) gpio request fail\n", __func__, data->lcd_id2);
			return ret;
		}
		gpio_direction_input(data->lcd_id2);
		printk("%s: lcd_id2[%d] :  %d\n", __func__, data->lcd_id2, gpio_get_value(data->lcd_id2));
	}

	data->lcd_id3 = of_get_named_gpio(np, "lcd,id_3", 0);
	if (gpio_is_valid(data->lcd_id3)) {
		ret = gpio_request(data->lcd_id3, "lcd_id3");
		if (ret < 0) {
			pr_err("%s: lcd_id3 (%d) gpio request fail\n", __func__, data->lcd_id3);
			return ret;
		}
		gpio_direction_input(data->lcd_id3);
		printk("%s: lcd_id3[%d] :  %d\n", __func__, data->lcd_id3, gpio_get_value(data->lcd_id3));
	}

	return 0;
}

/*
 * sysfs: one read-only node per ID strap, each returns the raw GPIO level.
 * The returned length counts the terminating NUL, as stock does.
 */
static ssize_t lcd_id0_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	sprintf(buf, "%d\n", gpio_get_value(g_data->lcd_id0));
	printk("%s, lcd_id0 %d is %s", __func__, g_data->lcd_id0, buf);
	return strlen(buf) + 1;
}

static ssize_t lcd_id1_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	sprintf(buf, "%d\n", gpio_get_value(g_data->lcd_id1));
	printk("%s, lcd_id1 %d is %s", __func__, g_data->lcd_id1, buf);
	return strlen(buf) + 1;
}

static ssize_t lcd_id2_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	sprintf(buf, "%d\n", gpio_get_value(g_data->lcd_id2));
	printk("%s, lcd_id2 %d is %s", __func__, g_data->lcd_id2, buf);
	return strlen(buf) + 1;
}

static ssize_t lcd_id3_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	sprintf(buf, "%d\n", gpio_get_value(g_data->lcd_id3));
	printk("%s, lcd_id3 %d is %s", __func__, g_data->lcd_id3, buf);
	return strlen(buf) + 1;
}

static const struct device_attribute lcd_id_attrs[] = {
	__ATTR(lcd_id0, 0444, lcd_id0_show, NULL),
	__ATTR(lcd_id1, 0444, lcd_id1_show, NULL),
	__ATTR(lcd_id2, 0444, lcd_id2_show, NULL),
	__ATTR(lcd_id3, 0444, lcd_id3_show, NULL),
};

static int lcd_id_probe(struct platform_device *pdev)
{
	struct lcd_id_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	printk("%s enter!\n", __func__);

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = dev;
	g_data = data;

	lcd_id_init_gpio(data);
	update_lcd_information(data);

	ret = fiio_device_attribute_register(lcd_id_attrs, ARRAY_SIZE(lcd_id_attrs));
	if (ret) {
		dev_err(dev, "Unable to export lcd_id sysfs, error: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, data);
	printk("%s end!\n", __func__);

	return 0;
}

static int lcd_id_remove(struct platform_device *pdev)
{
	struct lcd_id_data *data = platform_get_drvdata(pdev);

	printk("%s enter\n", __func__);
	fiio_device_attribute_unregister(lcd_id_attrs);

	if (gpio_is_valid(data->lcd_id0))
		gpio_free(data->lcd_id0);
	if (gpio_is_valid(data->lcd_id1))
		gpio_free(data->lcd_id1);
	if (gpio_is_valid(data->lcd_id2))
		gpio_free(data->lcd_id2);
	if (gpio_is_valid(data->lcd_id3))
		gpio_free(data->lcd_id3);

	/* stock frees the devm allocation by hand */
	kfree(data);

	return 0;
}

static void lcd_id_shutdown(struct platform_device *pdev)
{
	printk("%s enter!\n", __func__);
}

static struct of_device_id lcd_id_match_table[] = {
	{ .compatible = "fiio,lcd_id" },
	{},
};

static struct platform_driver lcd_id_driver = {
	.probe = lcd_id_probe,
	.remove = lcd_id_remove,
	.shutdown = lcd_id_shutdown,
	.driver = {
		.name = "lcd_id",
		.owner = THIS_MODULE,
		.of_match_table = lcd_id_match_table,
	},
};

static int __init lcd_id_init(void)
{
	return platform_driver_register(&lcd_id_driver);
}

static void __exit lcd_id_exit(void)
{
	platform_driver_unregister(&lcd_id_driver);
}

module_init(lcd_id_init);
module_exit(lcd_id_exit);

MODULE_DESCRIPTION("lcd_id driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("mxc@fiio.net");
