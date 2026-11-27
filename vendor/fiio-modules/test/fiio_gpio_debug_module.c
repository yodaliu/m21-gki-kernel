// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO GPIO debug module: read, set and clear the GPIOs named in the device
 * tree through sysfs.
 *
 * Reconstructed from the stock FiiO M21 FW 1.1.0 fiio_gpio_debug_module.ko
 * (drivers/fiio_modules/test/fiio_gpio_debug_module.c, author Tiben).
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <fiio/fiio_attr.h>

struct fiio_gpio_info {
	struct fiio_gpio_info *next;
	char name[20];
	int gpio_num;
	int request_ref_cnt;
	int dir;
	int value;
};

struct fiio_gpio_debug_module {
	struct device *dev;
	int gpio_register_cnt;
	struct fiio_gpio_info *head;
	struct fiio_gpio_info *cur_read_gpio_info;
};

static struct fiio_gpio_debug_module *g_data;

static struct fiio_gpio_info *
get_gpio_by_name(struct fiio_gpio_debug_module *fiio_gpio_debug_module,
		 const char *name)
{
	struct fiio_gpio_info *info;
	char temp_name[128] = {0};
	int len;

	info = (struct fiio_gpio_info *)fiio_gpio_debug_module;
	if (!name || !info->name)
		return NULL;

	len = strlen(name);
	if (len > 128) {
		printk("%s, can't found (%s)gpio,gpio name too long, more than 128 bytes.\n",
		       __func__, name);
		return NULL;
	}

	len = strcspn(name, "\n");
	if (len > 0) {
		strncpy(temp_name, name, len);
		temp_name[len] = '\0';
	}

	for (; info; info = info->next) {
		if (!strcmp(temp_name, info->name))
			return info;
	}

	printk("%s, can't found (%s)gpio,maybe not register.\n", __func__,
	       temp_name);
	return NULL;
}

static ssize_t read_gpio_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct fiio_gpio_debug_module *data = g_data;
	int ret;
	int value = 0;

	if (data->cur_read_gpio_info) {
		ret = gpio_request(data->cur_read_gpio_info->gpio_num,
				   data->cur_read_gpio_info->name);
		if (ret == -EBUSY) {
			pr_err("%s: (%s) request busy.\n", __func__,
			       data->cur_read_gpio_info->name);
		} else if (ret < 0) {
			pr_err("%s: (%s)  request fail\n", __func__,
			       data->cur_read_gpio_info->name);
			goto out;
		}
		value = gpio_get_value_cansleep(data->cur_read_gpio_info->gpio_num);
		gpio_free(data->cur_read_gpio_info->gpio_num);
		printk("%s, %s = %d\n", __func__,
		       data->cur_read_gpio_info->name, value);
	}
out:
	return sprintf(buf, "%d\n", value);
}

static ssize_t read_gpio_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct fiio_gpio_debug_module *data = g_data;
	struct fiio_gpio_info *info;

	info = get_gpio_by_name((struct fiio_gpio_debug_module *)data->head, buf);
	printk("%s, buf = %s\n", __func__, buf);
	if (info)
		data->cur_read_gpio_info = info;
	read_gpio_show(dev, attr, (char *)buf);
	return count;
}

static ssize_t set_gpio_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct fiio_gpio_debug_module *data = g_data;
	struct fiio_gpio_info *info;
	int ret;

	printk("%s, buf = %s\n", __func__, buf);
	info = get_gpio_by_name((struct fiio_gpio_debug_module *)data->head, buf);
	if (!info)
		return count;

	if (!gpio_is_valid(info->gpio_num)) {
		printk("%s, buf = %s Error cant find gpio!\n", __func__, buf);
		return count;
	}

	ret = gpio_request(info->gpio_num, info->name);
	if (ret == -EBUSY) {
		pr_err("%s: (%s) request busy.\n", __func__, info->name);
	} else if (ret < 0) {
		pr_err("%s: (%s)  request fail\n", __func__, info->name);
		return count;
	}
	gpio_direction_output(info->gpio_num, 1);
	gpio_free(info->gpio_num);
	data->cur_read_gpio_info = info;
	read_gpio_show(dev, attr, (char *)buf);
	return count;
}

static ssize_t clear_gpio_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct fiio_gpio_debug_module *data = g_data;
	struct fiio_gpio_info *info;
	int ret;

	info = get_gpio_by_name((struct fiio_gpio_debug_module *)data->head, buf);
	printk("%s, buf = %s\n", __func__, buf);
	if (!info)
		return count;

	ret = gpio_request(info->gpio_num, info->name);
	if (ret == -EBUSY) {
		pr_err("%s: (%s) request busy.\n", __func__, info->name);
	} else if (ret < 0) {
		pr_err("%s: (%s)  request fail\n", __func__, info->name);
		return count;
	}
	gpio_direction_output(info->gpio_num, 0);
	gpio_free(info->gpio_num);
	data->cur_read_gpio_info = info;
	read_gpio_show(dev, attr, (char *)buf);
	return count;
}

static const struct device_attribute gpio_debug_attributes[] = {
	__ATTR(gpio_debug_read, 0644, read_gpio_show, read_gpio_store),
	__ATTR(gpio_debug_set, 0200, NULL, set_gpio_store),
	__ATTR(gpio_debug_clear, 0200, NULL, clear_gpio_store),
};

static int fiio_gpio_debug_module_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *child;
	struct fiio_gpio_debug_module *data;
	struct fiio_gpio_info *info, *node, *current_node, *next_node;
	const char *name;
	int gpio_pin;
	int index;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	printk("%s\n", __func__);
	platform_set_drvdata(pdev, data);
	g_data = data;

	for_each_available_child_of_node(np, child) {
		name = of_get_property(child, "gpio-map-name", NULL);
		gpio_pin = of_get_named_gpio(child, "gpio-map-value", 0);
		if (!name || !gpio_is_valid(gpio_pin)) {
			printk("%s,invailed node(%s)\n", __func__, child->name);
			continue;
		}

		if (!strcmp(name, "usb_a_en"))
			gpio_direction_output(gpio_pin, 1);

		data->gpio_register_cnt++;
		info = kzalloc(sizeof(*info), GFP_KERNEL);
		info->gpio_num = gpio_pin;
		strcpy(info->name, name);

		if (!data->head) {
			current_node = (struct fiio_gpio_info *)&data->head;
		} else {
			next_node = data->head;
			do {
				current_node = next_node;
				next_node = current_node->next;
			} while (next_node);
		}
		current_node->next = info;
	}

	index = 0;
	for (node = data->head; node; node = node->next)
		printk("fiio_gpio_info[%d]: name = %s, gpio_num = %d\n", index++,
		       node->name, node->gpio_num);

	if (fiio_device_attribute_register(gpio_debug_attributes,
					   ARRAY_SIZE(gpio_debug_attributes))) {
		printk("attribute register failed!!");
		fiio_device_attribute_unregister(gpio_debug_attributes);
		return -ENOMEM;
	}

	return 0;
}

static int fiio_gpio_debug_module_remove(struct platform_device *pdev)
{
	fiio_device_attribute_unregister(gpio_debug_attributes);
	return 0;
}

static const struct of_device_id fiio_gpio_debug_module_match_table_device[] = {
	{ .compatible = "qcom,fiio-gpio-debug-module" },
	{}
};

static struct platform_driver fiio_gpio_debug_module_driver = {
	.probe = fiio_gpio_debug_module_probe,
	.remove = fiio_gpio_debug_module_remove,
	.driver = {
		.name = "qcom,fiio-gpio-debug-module",
		.owner = THIS_MODULE,
		.of_match_table = fiio_gpio_debug_module_match_table_device,
	},
};

int __init fiio_gpio_debug_module_init(void)
{
	printk("%s\n", __func__);
	return platform_driver_register(&fiio_gpio_debug_module_driver);
}

static void __exit fiio_gpio_debug_module_exit(void)
{
	printk("%s\n", __func__);
	platform_driver_unregister(&fiio_gpio_debug_module_driver);
}

module_init(fiio_gpio_debug_module_init);
module_exit(fiio_gpio_debug_module_exit);

MODULE_AUTHOR("Tiben");
MODULE_DESCRIPTION("fiio gpio debug module");
MODULE_LICENSE("GPL");
