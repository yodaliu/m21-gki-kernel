// SPDX-License-Identifier: GPL-2.0
/*
 * Quectel gpio layer: GPIO expanders (aw95016 and others) register their
 * ops here, and drivers address their pins through one global gpio number
 * space. debugfs "quectel-gpio" dumps the pin states.
 *
 * Reconstructed from the stock FiiO M21 FW 1.1.0 gpiolib-quectel.ko
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/debugfs.h>
#include <linux/gpiolib_quectel_kernel.h>

struct quectel_gpio_dev {
	uint16_t n_gpio;
	unsigned int base;
	struct quectel_gpio_subdev subdev;
	struct list_head list;
};

struct quectel_gpio_map {
	unsigned int gpio;
	uint8_t dir;
	uint8_t flags;
	uint8_t dsr;
	uint8_t level;
	struct quectel_gpio_dev *gpio_dev;
};

struct quectel_gpio_data {
	struct list_head dev_list;
	struct mutex mutex_lock;
	unsigned int gpio_num;
	struct quectel_gpio_map *gpio_map;
};

struct quectel_gpio_data g_gpio_data;
static bool debug_print_flag;

int quectel_get_named_gpio(struct device_node *np, const char *propname, int index)
{
	struct quectel_gpio_data *gpio_data = &g_gpio_data;
	struct quectel_gpio_dev *gpio_dev = NULL, *tmp;
	struct of_phandle_args args = {0};
	int ret;

	if (index < 0)
		return -EINVAL;

	ret = of_parse_phandle_with_args(np, propname, "#gpio-cells", index, &args);
	if (ret) {
		pr_err("%s: can't parse '%s' property of node '%pOF[%d]'\n",
		       __func__, propname, np, index);
		return -EINVAL;
	}

	mutex_lock(&gpio_data->mutex_lock);
	list_for_each_entry(tmp, &gpio_data->dev_list, list) {
		if (tmp->subdev.dev->of_node == args.np) {
			gpio_dev = tmp;
			break;
		}
	}
	mutex_unlock(&gpio_data->mutex_lock);

	if (!gpio_dev) {
		pr_err("%s:%d propname %s match gpio failed\n", __func__, __LINE__, propname);
		return -EPROBE_DEFER;
	}
	return gpio_dev->base + args.args[0];
}
EXPORT_SYMBOL(quectel_get_named_gpio);

int quectel_gpio_direction_input(unsigned gpio, unsigned int flags)
{
	struct quectel_gpio_data *gpio_data = &g_gpio_data;
	struct quectel_gpio_dev *gpio_dev;

	if (gpio >= gpio_data->gpio_num)
		return -EINVAL;
	gpio_dev = gpio_data->gpio_map[gpio].gpio_dev;
	if (!gpio_dev)
		return -ENOSYS;
	gpio_data->gpio_map[gpio].dir = 0;
	gpio_data->gpio_map[gpio].dsr = 0;
	gpio_data->gpio_map[gpio].flags = flags & 1;
	return gpio_dev->subdev.gpio_direction_input(gpio_dev->subdev.dev,
						     gpio - gpio_dev->base, flags);
}
EXPORT_SYMBOL(quectel_gpio_direction_input);

int quectel_gpio_direction_output(unsigned gpio, int value, unsigned int flags)
{
	struct quectel_gpio_data *gpio_data = &g_gpio_data;
	struct quectel_gpio_dev *gpio_dev;

	if (gpio >= gpio_data->gpio_num)
		return -EINVAL;
	gpio_dev = gpio_data->gpio_map[gpio].gpio_dev;
	if (!gpio_dev)
		return -ENOSYS;
	gpio_data->gpio_map[gpio].dir = 1;
	gpio_data->gpio_map[gpio].dsr = (flags >> 4) & 3;
	gpio_data->gpio_map[gpio].flags = flags & 1;
	gpio_data->gpio_map[gpio].level = value;
	return gpio_dev->subdev.gpio_direction_output(gpio_dev->subdev.dev,
						      gpio - gpio_dev->base, value, flags);
}
EXPORT_SYMBOL(quectel_gpio_direction_output);

int quectel_gpio_set(unsigned gpio, int value)
{
	struct quectel_gpio_data *gpio_data = &g_gpio_data;
	struct quectel_gpio_dev *gpio_dev;

	if (gpio >= gpio_data->gpio_num)
		return -EINVAL;
	gpio_dev = gpio_data->gpio_map[gpio].gpio_dev;
	if (!gpio_dev)
		return -ENOSYS;
	gpio_data->gpio_map[gpio].level = value;
	return gpio_dev->subdev.gpio_set(gpio_dev->subdev.dev, gpio - gpio_dev->base, value);
}
EXPORT_SYMBOL(quectel_gpio_set);

int quectel_gpio_get(unsigned gpio)
{
	struct quectel_gpio_data *gpio_data = &g_gpio_data;
	struct quectel_gpio_dev *gpio_dev;

	if (gpio >= gpio_data->gpio_num)
		return -EINVAL;
	gpio_dev = gpio_data->gpio_map[gpio].gpio_dev;
	if (!gpio_dev)
		return -ENOSYS;
	return gpio_dev->subdev.gpio_get(gpio_dev->subdev.dev, gpio - gpio_dev->base);
}
EXPORT_SYMBOL(quectel_gpio_get);

int quectel_request_gpio_irq(unsigned gpio, irq_func handler, unsigned int flags, void *data)
{
	struct quectel_gpio_data *gpio_data = &g_gpio_data;
	struct quectel_gpio_dev *gpio_dev;

	if (gpio >= gpio_data->gpio_num)
		return -EINVAL;
	gpio_dev = gpio_data->gpio_map[gpio].gpio_dev;
	if (!gpio_dev)
		return -ENOSYS;
	return gpio_dev->subdev.register_irq_queue(gpio_dev->subdev.dev, gpio - gpio_dev->base,
						   handler, flags, data);
}
EXPORT_SYMBOL(quectel_request_gpio_irq);

#define QUECTEL_GPIO_IRQ_OP(name)						\
int quectel_gpio_##name(unsigned gpio)					\
{										\
	struct quectel_gpio_data *gpio_data = &g_gpio_data;			\
	struct quectel_gpio_dev *gpio_dev;					\
										\
	if (gpio >= gpio_data->gpio_num)					\
		return -EINVAL;							\
	gpio_dev = gpio_data->gpio_map[gpio].gpio_dev;				\
	if (!gpio_dev)								\
		return -ENOSYS;							\
	return gpio_dev->subdev.name(gpio_dev->subdev.dev, gpio - gpio_dev->base); \
}										\
EXPORT_SYMBOL(quectel_gpio_##name)

QUECTEL_GPIO_IRQ_OP(enable_irq);
QUECTEL_GPIO_IRQ_OP(disable_irq);
QUECTEL_GPIO_IRQ_OP(enable_irq_wake);
QUECTEL_GPIO_IRQ_OP(disable_irq_wake);

int quectel_gpio_register_dev(struct quectel_gpio_subdev *subdev)
{
	struct quectel_gpio_data *gpio_data = &g_gpio_data;
	struct quectel_gpio_dev *gpio_dev;
	struct quectel_gpio_map *gpio_map;
	int i;

	pr_err("%s:%d E\n", __func__, __LINE__);
	gpio_dev = kzalloc(sizeof(*gpio_dev), GFP_KERNEL);
	if (!gpio_dev) {
		pr_err("%s:%d alloc gpio_dev mem failed\n", __func__, __LINE__);
		return -ENOMEM;
	}
	gpio_dev->n_gpio = subdev->n_gpio;
	gpio_dev->subdev.dev = subdev->dev;
	gpio_dev->subdev.label = subdev->label;
	gpio_dev->subdev.n_gpio = subdev->n_gpio;
	gpio_dev->subdev.gpio_direction_input = subdev->gpio_direction_input;
	gpio_dev->subdev.gpio_direction_output = subdev->gpio_direction_output;
	gpio_dev->subdev.gpio_set = subdev->gpio_set;
	gpio_dev->subdev.gpio_get = subdev->gpio_get;
	gpio_dev->subdev.register_irq_queue = subdev->register_irq_queue;
	gpio_dev->subdev.enable_irq = subdev->enable_irq;
	gpio_dev->subdev.disable_irq = subdev->disable_irq;
	gpio_dev->subdev.enable_irq_wake = subdev->enable_irq_wake;
	gpio_dev->subdev.disable_irq_wake = subdev->disable_irq_wake;

	mutex_lock(&gpio_data->mutex_lock);
	gpio_map = kzalloc(sizeof(*gpio_map) * (subdev->n_gpio + gpio_data->gpio_num), GFP_KERNEL);
	if (!gpio_map) {
		pr_err("%s:%d alloc gpio mmap mem failed\n", __func__, __LINE__);
		kfree(gpio_dev);
		mutex_unlock(&gpio_data->mutex_lock);
		return -ENOMEM;
	}
	pr_err("%s:%d device [%s] register gpio num %d\n", __func__, __LINE__,
	       dev_name(subdev->dev), subdev->n_gpio);

	gpio_dev->base = gpio_data->gpio_num;
	for (i = 0; i < gpio_data->gpio_num; i++) {
		gpio_map[i].gpio = gpio_data->gpio_map[i].gpio;
		gpio_map[i].gpio_dev = gpio_data->gpio_map[i].gpio_dev;
	}
	if (gpio_data->gpio_map)
		kfree(gpio_data->gpio_map);
	gpio_data->gpio_map = gpio_map;

	for (i = gpio_data->gpio_num; i < gpio_data->gpio_num + subdev->n_gpio; i++) {
		gpio_data->gpio_map[i].gpio = i;
		gpio_data->gpio_map[i].gpio_dev = gpio_dev;
	}
	gpio_data->gpio_num += subdev->n_gpio;
	list_add_tail(&gpio_dev->list, &gpio_data->dev_list);
	mutex_unlock(&gpio_data->mutex_lock);

	pr_err("%s:%d register [%s] gpio device successfully\n", __func__, __LINE__,
	       dev_name(subdev->dev));
	return 0;
}
EXPORT_SYMBOL(quectel_gpio_register_dev);

static int quectel_gpiolib_open(struct inode *inode, struct file *file)
{
	pr_err("%s:%d E\n", __func__, __LINE__);
	return 0;
}

static int quectel_gpiolib_release(struct inode *inode, struct file *file)
{
	pr_err("%s:%d E\n", __func__, __LINE__);
	return 0;
}

static ssize_t quectel_gpiolib_read(struct file *file, char __user *buf, size_t count,
				    loff_t *ppos)
{
	struct quectel_gpio_data *gpio_data = &g_gpio_data;
	struct quectel_gpio_dev *gpio_dev;
	struct quectel_gpio_map *map;
	ssize_t ret;
	char *tmp_buf;
	int len, i;

	pr_err("%s:%d len %ld E\n", __func__, __LINE__, count);
	tmp_buf = kzalloc(count, GFP_KERNEL);
	if (!tmp_buf) {
		pr_err("%s:%d alloc tmp_buf failed\n", __func__, __LINE__);
		return -ENOMEM;
	}

	snprintf(tmp_buf, count, "GPIO num  :  DIR  :  Level  :  DSR\n");
	len = strlen(tmp_buf);
	list_for_each_entry(gpio_dev, &gpio_data->dev_list, list) {
		snprintf(tmp_buf + len, count - len, "GPIO Chip:[%s][%s] ,GPIOs %d~%d:\n",
			 gpio_dev->subdev.label, dev_name(gpio_dev->subdev.dev), gpio_dev->base,
			 gpio_dev->base + gpio_dev->n_gpio - 1);
		len = strlen(tmp_buf);
		for (i = 0; i < gpio_dev->n_gpio; i++) {
			map = &gpio_data->gpio_map[gpio_dev->base + i];
			snprintf(tmp_buf + len, count - len, "gpio%d    :   %d   :    %d    :   %d\n",
				 i, map->dir, map->level, map->dsr);
			len = strlen(tmp_buf);
		}
		pr_err("%s:%d device name %s copy cnt %d\n", __func__, __LINE__,
		       dev_name(gpio_dev->subdev.dev), len);
	}

	if (copy_to_user(buf, tmp_buf, len)) {
		pr_err("%s:%d copy data failed\n", __func__, __LINE__);
		return -EFAULT;
	}
	kfree(tmp_buf);
	ret = debug_print_flag ? 0 : len;
	*ppos += len;
	debug_print_flag = !debug_print_flag;
	return ret;
}

static ssize_t quectel_gpiolib_write(struct file *file, const char __user *buf, size_t count,
				     loff_t *ppos)
{
	pr_err("%s:%d E\n", __func__, __LINE__);
	return 0;
}

static const struct file_operations quectel_gpiolib_fops = {
	.owner = THIS_MODULE,
	.open = quectel_gpiolib_open,
	.release = quectel_gpiolib_release,
	.read = quectel_gpiolib_read,
	.write = quectel_gpiolib_write,
};

static int __init quectel_gpio_driver_init(void)
{
	struct quectel_gpio_data *gpio_data = &g_gpio_data;
	struct dentry *dentry;

	pr_err("%s:%d E\n", __func__, __LINE__);
	memset(gpio_data, 0, sizeof(*gpio_data));
	mutex_init(&gpio_data->mutex_lock);
	INIT_LIST_HEAD(&gpio_data->dev_list);
	gpio_data->gpio_num = 0;
	gpio_data->gpio_map = NULL;

	dentry = debugfs_create_file("quectel-gpio", 0644, NULL, NULL, &quectel_gpiolib_fops);
	if (IS_ERR(dentry))
		pr_err("%s:%d create gpio debugfs failed\n", __func__, __LINE__);
	return 0;
}

static void __exit quectel_gpio_driver_exit(void)
{
	pr_err("%s:%d E\n", __func__, __LINE__);
}

module_init(quectel_gpio_driver_init);
module_exit(quectel_gpio_driver_exit);

MODULE_DESCRIPTION("quectel gpio driver");
MODULE_LICENSE("GPL");
