// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO ADC debug module: get, release and read the IIO channels named in the
 * device tree through sysfs.
 *
 * Reconstructed from the stock FiiO M21 FW 1.1.0 fiio_adc_debug_module.ko
 * (drivers/fiio_modules/test/fiio_adc_debug_module.c, author Tiben).
 */
#define pr_fmt(fmt) "[fiio_adc]: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/iio/consumer.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <fiio/fiio_attr.h>

struct fiio_adc_info {
	struct fiio_adc_info *next;
	char name[20];
	int gpio_num;
	int request_ref_cnt;
	int dir;
	int value;
	struct iio_channel *channel;
};

struct fiio_adc_debug_module {
	struct device *dev;
	int channel_register_cnt;
	struct fiio_adc_info *head;
	struct fiio_adc_info *cur_read_gpio_info;
};

static struct fiio_adc_debug_module *g_data;

static struct fiio_adc_info *
get_gpio_by_name(struct fiio_adc_debug_module *fiio_adc_debug_module,
		 const char *name)
{
	struct fiio_adc_info *info;
	char temp_name[128] = {0};
	int len;

	info = (struct fiio_adc_info *)fiio_adc_debug_module;
	if (!name || !info->name)
		return NULL;

	len = strlen(name);
	if (len > 128) {
		pr_err("%s, can't found (%s)gpio,gpio name too long, more than 128 bytes.\n",
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

	pr_err("%s, can't found (%s)gpio,maybe not register.\n", __func__,
	       temp_name);
	return NULL;
}

static ssize_t set_adc_channel_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct fiio_adc_debug_module *data = g_data;
	struct fiio_adc_info *info;

	pr_err("%s, buf = %s\n", __func__, buf);
	info = get_gpio_by_name((struct fiio_adc_debug_module *)data->head, buf);
	if (!info)
		return count;

	info->channel = devm_iio_channel_get(dev, info->name);
	if (IS_ERR(info->channel))
		pr_err("%s failed to get %s input channel\n", __func__,
		       info->name);
	else
		pr_err("%s: (%s) iio_channel_get success\n", __func__,
		       info->name);
	return count;
}

static ssize_t release_adc_channel_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct fiio_adc_info *info;

	info = get_gpio_by_name((struct fiio_adc_debug_module *)g_data->head, buf);
	if (!info) {
		pr_err("%s: channel release fail\n", __func__);
		return count;
	}

	iio_channel_release(info->channel);
	pr_err("%s: (%s) release channel\n", __func__, info->name);
	return count;
}

static ssize_t read_adc_channel_raw_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct fiio_adc_debug_module *data = g_data;
	int ret;
	int val = 0;
	int value = 0;

	if (data->cur_read_gpio_info) {
		ret = iio_read_channel_processed(data->cur_read_gpio_info->channel,
						 &val);
		if (ret < 0)
			pr_err("%s: (%s) io_read_channel_raw error.\n", __func__,
			       data->cur_read_gpio_info->name);
		else {
			pr_err("%s: (%s) io_read_channel_raw success val=%d.\n",
			       __func__, data->cur_read_gpio_info->name, val);
			value = val;
		}
	}

	return sprintf(buf, "%d\n", value);
}

static ssize_t read_adc_channel_raw_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct fiio_adc_debug_module *data = g_data;
	struct fiio_adc_info *info;

	info = get_gpio_by_name((struct fiio_adc_debug_module *)data->head, buf);
	if (info)
		data->cur_read_gpio_info = info;
	return count;
}

static const struct device_attribute adc_debug_attributes[] = {
	__ATTR(adc_debug_set_channel, 0200, NULL, set_adc_channel_store),
	__ATTR(adc_debug_release_channel, 0200, NULL, release_adc_channel_store),
	__ATTR(adc_debug_read_channel_raw, 0644, read_adc_channel_raw_show,
	       read_adc_channel_raw_store),
};

static int fiio_adc_debug_module_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *child;
	struct fiio_adc_debug_module *data;
	struct fiio_adc_info *info, *node, *current_node, *next_node;
	struct iio_channel *channel;
	const char *name;
	int index;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	g_data = data;
	pr_err("%s\n", __func__);
	platform_set_drvdata(pdev, data);

	for_each_available_child_of_node(np, child) {
		name = of_get_property(child, "io-channel-names", NULL);
		if (!name) {
			pr_err("%s,invailed node(%s)\n", __func__, child->name);
			continue;
		}

		channel = devm_iio_channel_get(&pdev->dev, name);
		if (IS_ERR(channel)) {
			pr_err("%s channel unavailable, %d\n", name,
			       (int)PTR_ERR(channel));
		} else {
			pr_err("%s: %s channel get success\n", __func__, name);
			iio_channel_release(channel);
		}

		info = kzalloc(sizeof(*info), GFP_KERNEL);
		data->channel_register_cnt++;
		strcpy(info->name, name);

		if (!data->head) {
			current_node = (struct fiio_adc_info *)&data->head;
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
		pr_err("fiio_adc_info[%d]: name = %s", index++, node->name);

	if (fiio_device_attribute_register(adc_debug_attributes,
					   ARRAY_SIZE(adc_debug_attributes))) {
		pr_err("%s: attribute register failed!!", __func__);
		return -ENOMEM;
	}

	return 0;
}

static int fiio_adc_debug_module_remove(struct platform_device *pdev)
{
	fiio_device_attribute_unregister(adc_debug_attributes);
	return 0;
}

static const struct of_device_id fiio_adc_debug_module_match_table_device[] = {
	{ .compatible = "qcom,fiio-adc-debug-module" },
	{}
};

static struct platform_driver fiio_adc_debug_module_driver = {
	.probe = fiio_adc_debug_module_probe,
	.remove = fiio_adc_debug_module_remove,
	.driver = {
		.name = "qcom,fiio-adc-debug-module",
		.owner = THIS_MODULE,
		.of_match_table = fiio_adc_debug_module_match_table_device,
	},
};

int __init fiio_adc_debug_module_init(void)
{
	pr_err("%s\n", __func__);
	return platform_driver_register(&fiio_adc_debug_module_driver);
}

static void __exit fiio_adc_debug_module_exit(void)
{
	pr_err("%s\n", __func__);
	platform_driver_unregister(&fiio_adc_debug_module_driver);
}

module_init(fiio_adc_debug_module_init);
module_exit(fiio_adc_debug_module_exit);

MODULE_AUTHOR("Tiben");
MODULE_DESCRIPTION("fiio adc debug module");
MODULE_LICENSE("GPL");
