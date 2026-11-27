// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO UART debug module: write a line to, or read 8 bytes from, one of the
 * UART device nodes listed in the device tree through sysfs.
 *
 * Reconstructed from the stock FiiO M21 FW 1.1.0 fiio_uart_debug_module.ko
 * (drivers/fiio_modules/test/fiio_uart_debug_module.c, author Tiben).
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/string.h>

#include <fiio/fiio_attr.h>

#define UART_NUM 5

static char uart_path[UART_NUM][32];
static char uartid[UART_NUM][8];
static struct file *file_uart;
static char read_uart[32];

static int fiio_uart_open(int index)
{
	if (file_uart) {
		printk("[%s] filp already open\n", __func__);
		return -1;
	}

	file_uart = filp_open(uart_path[index], O_RDWR | O_NONBLOCK, 0666);
	if (IS_ERR(file_uart)) {
		pr_err("[%s] ERROR opening file(%d) with errno = %ld!\n",
		       __func__, index, PTR_ERR(file_uart));
		return PTR_ERR(file_uart);
	}

	printk("[%s] filp_open\n", __func__);
	return 0;
}

static int fiio_uart_read_open(int index)
{
	if (file_uart) {
		printk("[%s] filp already open\n", __func__);
		return -1;
	}

	file_uart = filp_open(uart_path[index], O_RDWR, 0666);
	if (IS_ERR(file_uart)) {
		pr_err("[%s] ERROR opening file(%d) with errno = %ld!\n",
		       __func__, index, PTR_ERR(file_uart));
		return PTR_ERR(file_uart);
	}

	printk("[%s] filp_open\n", __func__);
	return 0;
}

static void fiio_uart_write(const char *data)
{
	char buf[10];
	loff_t pos = 0;

	if (!file_uart) {
		printk("[%s] file_uart is NULL", __func__);
		return;
	}

	sprintf(buf, "%s\r\n", data);
	kernel_write(file_uart, buf, strlen(buf), &pos);
	printk("[%s] %s", __func__, buf);
}

static void fiio_uart_read(char *data)
{
	char buf[10];
	loff_t pos = 0;
	int ret;

	if (!file_uart)
		return;

	ret = kernel_read(file_uart, buf, 10, &pos);
	sprintf(data, "%.8s", buf);
	printk("[%s] read_data=[%s] ret=[%d]\n", __func__, data, ret);
}

static void fiio_uart_close(void)
{
	int ret;

	if (!file_uart) {
		printk("[%s] can't close\n", __func__);
		return;
	}

	ret = filp_close(file_uart, NULL);
	printk("[%s] filp_close ret=%d\n", __func__, ret);
	file_uart = NULL;
}

static ssize_t write_uart_data(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	char data[32];
	char id[32];
	char msg[32];
	int index = 0;
	int len;
	int i;

	strcpy(data, buf);
	len = strlen(data);
	data[len - 1] = '\0';
	sscanf(data, "%s %s", id, msg);

	for (i = 0; i < UART_NUM; i++) {
		if (!strcmp(id, uartid[i])) {
			index = i;
			printk("uart_path[%d] = %s\n", i, uart_path[i]);
		}
	}

	if (fiio_uart_open(index)) {
		printk("[%s] filp failed open\n", __func__);
		return -1;
	}
	fiio_uart_write(msg);
	fiio_uart_close();

	return count;
}

static ssize_t read_uart_data(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	char data[10] = {0};
	int index = 0;
	int len;
	int i;

	printk("read_uart_data input data is = %s\n", buf);

	if (!read_uart[0])
		strcpy(read_uart, buf);
	len = strlen(read_uart);
	read_uart[len - 1] = '\0';

	for (i = 0; i < UART_NUM; i++) {
		if (!strcmp(read_uart, uartid[i])) {
			index = i;
			pr_err("uart_path[%d] = %s\n", i, uart_path[i]);
		}
	}

	if (fiio_uart_read_open(index)) {
		printk("[%s] filp failed open\n", __func__);
		return -1;
	}
	fiio_uart_read(data);
	printk("[%s] read_data = %s", __func__, data);
	fiio_uart_close();

	return strlen(data) + 1;
}

static const struct device_attribute uart_debug_attributes[] = {
	__ATTR(uart_debug_write, 0200, NULL, write_uart_data),
	__ATTR(uart_debug_read, 0200, NULL, read_uart_data),
};

static int fiio_uart_debug_module_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *child;
	const char *uart_id, *uart_name;
	int i = 0;

	for_each_available_child_of_node(np, child) {
		uart_id = of_get_property(child, "uart-id", NULL);
		uart_name = of_get_property(child, "uart-name", NULL);
		strcpy(uart_path[i], uart_name);
		strcpy(uartid[i], uart_id);
		printk("uart_path[%d] = %s\n", i, uart_path[i]);
		printk("uartid[%d] = %s\n", i, uartid[i]);
		i++;
	}

	if (fiio_device_attribute_register(uart_debug_attributes,
					   ARRAY_SIZE(uart_debug_attributes))) {
		printk("attribute register failed!!");
		fiio_device_attribute_unregister(uart_debug_attributes);
		return -ENOMEM;
	}

	return 0;
}

static int fiio_uart_debug_module_remove(struct platform_device *pdev)
{
	fiio_device_attribute_unregister(uart_debug_attributes);
	return 0;
}

static const struct of_device_id fiio_uart_debug_module_match_table_device[] = {
	{ .compatible = "qcom,fiio-uart-debug-module" },
	{}
};

static struct platform_driver fiio_uart_debug_module_driver = {
	.probe = fiio_uart_debug_module_probe,
	.remove = fiio_uart_debug_module_remove,
	.driver = {
		.name = "qcom,fiio-uart-debug-module",
		.owner = THIS_MODULE,
		.of_match_table = fiio_uart_debug_module_match_table_device,
	},
};

int __init fiio_uart_debug_module_init(void)
{
	printk("%s\n", __func__);
	return platform_driver_register(&fiio_uart_debug_module_driver);
}

static void __exit fiio_uart_debug_module_exit(void)
{
	printk("%s\n", __func__);
	platform_driver_unregister(&fiio_uart_debug_module_driver);
}

module_init(fiio_uart_debug_module_init);
module_exit(fiio_uart_debug_module_exit);

MODULE_AUTHOR("Tiben");
MODULE_DESCRIPTION("fiio uart debug module");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
MODULE_IMPORT_NS(CRYPTO_INTERNAL);
