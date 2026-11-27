// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO config: small records kept on the "fiioconfig" partition, exposed as
 * sysfs attributes (through fiio_primary_modules) and as exported helpers for
 * the touchscreen, FPGA and DAC drivers.
 *
 * Partition layout (byte offset: length):
 *   0x0000: 10  power_off_charge_switch
 *   0x0800:  4  charging_disable_status
 *   0x0c00:  4  left DAC master trim index
 *   0x0c04:  4  right DAC master trim index
 *   0x1000:  4  cst812t touch firmware checksum
 *   0x1400: 50  FPGA firmware name
 *   0x1800: 50  serial number (SN)
 *   0x1c00: 50  NB number
 *   0x2000: 20  usb_boot_switch
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/fs.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <fiio/fiio_attr.h>

#define FIIO_CONFIG_PATH		"/dev/block/by-name/fiioconfig"

#define POWER_OFF_CHARGE_SWITCH_POS	0x0
#define POWER_OFF_CHARGE_SWITCH_LEN	10
#define CHARGING_DISABLE_POS		0x800
#define LEFT_DAC_TRIM_POS		0xc00
#define RIGHT_DAC_TRIM_POS		0xc04
#define CST812T_CHECKSUM_POS		0x1000
#define FPGA_FIRMWARE_NAME_POS		0x1400
#define SN_POS				0x1800
#define NB_POS				0x1c00
#define USB_BOOT_SWITCH_POS		0x2000
#define USB_BOOT_SWITCH_LEN		20
#define NAME_LEN			50

/* 0 on success (also when the read came up short), 1 if the partition can't be opened */
static int fiio_config_kernel_read_from_storage(unsigned char *pdata, unsigned int length, loff_t pos)
{
	int error = 0;
	unsigned int result_count;
	struct file *f;

	printk("enter %s\n", __func__);

	f = filp_open(FIIO_CONFIG_PATH, O_RDWR, 0);
	if (IS_ERR(f)) {
		printk(KERN_ERR "filp_open fail, error code (%d)\n", (int)PTR_ERR(f));
		error = 1;
		goto exit;
	}

	result_count = kernel_read(f, pdata, length, &pos);
	if (result_count == length)
		printk("length = %d --- result_count = %d  read contents is %s\n",
		       length, result_count, pdata);
	else
		printk("fiioconfig_read_from_storage ---- kernel_read error ----\n");

	filp_close(f, NULL);
	error = 0;
exit:
	return error;
}

/* bytes written, or 1 on error (0 only if nothing was written) */
static int fiio_config_kernel_write_to_storage(unsigned char *pdata, unsigned int length, loff_t pos)
{
	int error = 0;
	struct file *f;

	printk("enter %s\n", __func__);

	f = filp_open(FIIO_CONFIG_PATH, O_RDWR, 0);
	if (IS_ERR(f)) {
		error = 1;
		goto exit;
	}

	error = kernel_write(f, pdata, length, &pos);
	if (error < 0) {
		printk(KERN_ERR "fiioconfig_write_to_storage ---- kernel_write error ----\n");
		error = 1;
		goto exit;
	}

	filp_close(f, NULL);
exit:
	return error;
}

static ssize_t power_off_charge_switch_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int count;
	int err;
	int i;
	unsigned char pdata[POWER_OFF_CHARGE_SWITCH_LEN] = {0};

	err = fiio_config_kernel_read_from_storage(pdata, POWER_OFF_CHARGE_SWITCH_LEN,
						   POWER_OFF_CHARGE_SWITCH_POS);
	if (err) {
		count = sprintf(buf, "read fiioconfig fail!\n");
		return count;
	}

	for (i = 0; i < POWER_OFF_CHARGE_SWITCH_LEN; i++) {
		if (pdata[i] == '\n') {
			pdata[i] = '\0';
			break;
		}
	}

	count = sprintf(buf, "fiio config is %s\n", pdata);
	return count;
}

static ssize_t power_off_charge_switch_store(struct device *dev, struct device_attribute *attr,
					     const char *buf, size_t count)
{
	int i;
	int err;
	unsigned char pdata[POWER_OFF_CHARGE_SWITCH_LEN] = {0};

	for (i = 0; i < POWER_OFF_CHARGE_SWITCH_LEN; i++) {
		if (buf[i] != '\n') {
			pdata[i] = buf[i];
		} else {
			pdata[i] = '\0';
			break;
		}
	}

	err = fiio_config_kernel_write_to_storage(pdata, POWER_OFF_CHARGE_SWITCH_LEN,
						  POWER_OFF_CHARGE_SWITCH_POS);
	if (!err)
		printk("write %s to fiioconfig fail!\n", pdata);
	else
		printk("write %s to fiioconfig success!\n", pdata);

	return count;
}

static ssize_t usb_boot_switch_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int count;
	int err;
	int i;
	unsigned char pdata[USB_BOOT_SWITCH_LEN] = {0};

	err = fiio_config_kernel_read_from_storage(pdata, USB_BOOT_SWITCH_LEN, USB_BOOT_SWITCH_POS);
	if (err) {
		count = sprintf(buf, "read fiioconfig fail!\n");
		return count;
	}

	for (i = 0; i < USB_BOOT_SWITCH_LEN; i++) {
		if (pdata[i] == '\n') {
			pdata[i] = '\0';
			break;
		}
	}

	count = sprintf(buf, "fiio config is %s\n", pdata);
	return count;
}

static ssize_t usb_boot_switch_store(struct device *dev, struct device_attribute *attr,
				     const char *buf, size_t count)
{
	int i;
	int err;
	unsigned char pdata[USB_BOOT_SWITCH_LEN] = {0};

	for (i = 0; i < USB_BOOT_SWITCH_LEN; i++) {
		if (buf[i] != '\n') {
			pdata[i] = buf[i];
		} else {
			pdata[i] = '\0';
			break;
		}
	}

	err = fiio_config_kernel_write_to_storage(pdata, USB_BOOT_SWITCH_LEN, USB_BOOT_SWITCH_POS);
	if (!err)
		printk("write %s to fiioconfig fail!\n", pdata);
	else
		printk("write %s to fiioconfig success!\n", pdata);

	return count;
}

static ssize_t charging_disable_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int count;
	int err;
	unsigned int charging_disable = 0;

	err = fiio_config_kernel_read_from_storage((unsigned char *)&charging_disable,
						   sizeof(charging_disable), CHARGING_DISABLE_POS);
	if (err)
		count = sprintf(buf, "error");
	else
		count = sprintf(buf, "%d", charging_disable);

	return count;
}

static ssize_t charging_disable_status_store(struct device *dev, struct device_attribute *attr,
					     const char *buf, size_t count)
{
	int err;
	unsigned int charging_disable = 0;
	ssize_t result;

	result = kstrtouint(buf, 0, &charging_disable);

	err = fiio_config_kernel_write_to_storage((unsigned char *)&charging_disable,
						  sizeof(charging_disable), CHARGING_DISABLE_POS);
	if (err)
		printk("write charging_disable = %d to fiioconfig success!\n", charging_disable);
	else
		printk("write charging_disable_status = %d to fiioconfig fail!\n", charging_disable);

	return count;
}

int write_cst812t_checksum_to_storage(unsigned char *pdata)
{
	int err;

	err = fiio_config_kernel_write_to_storage(pdata, 4, CST812T_CHECKSUM_POS);
	return err;
}
EXPORT_SYMBOL(write_cst812t_checksum_to_storage);

int read_cst812t_checksum_from_storage(unsigned char *pdata)
{
	int err;

	err = fiio_config_kernel_read_from_storage(pdata, 4, CST812T_CHECKSUM_POS);
	return err;
}
EXPORT_SYMBOL(read_cst812t_checksum_from_storage);

int write_fpga_firmware_name_to_storage(unsigned char *pdata)
{
	int err;

	err = fiio_config_kernel_write_to_storage(pdata, NAME_LEN, FPGA_FIRMWARE_NAME_POS);
	return err;
}
EXPORT_SYMBOL(write_fpga_firmware_name_to_storage);

int read_fpga_firmware_name_from_storage(unsigned char *pdata)
{
	int err;
	int i;

	err = fiio_config_kernel_read_from_storage(pdata, NAME_LEN, FPGA_FIRMWARE_NAME_POS);
	for (i = 0; i < NAME_LEN; i++) {
		if (pdata[i] == '\n') {
			pdata[i] = '\0';
			break;
		}
	}

	return err;
}
EXPORT_SYMBOL(read_fpga_firmware_name_from_storage);

static ssize_t fpga_firmware_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int count;
	int err;
	char name[NAME_LEN] = {0};

	err = read_fpga_firmware_name_from_storage(name);
	if (err)
		count = sprintf(buf, "error");
	else
		count = sprintf(buf, "%s", name);

	if (name[0] == '\0')
		printk("Not config fpga firmware name,use default firmware!\n");

	return count;
}

static ssize_t fpga_firmware_name_store(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t count)
{
	int err;

	err = write_fpga_firmware_name_to_storage((unsigned char *)buf);
	if (!err)
		printk("write firmware_name = %s to fiioconfig fail!\n", buf);
	else
		printk("write firmware_name = %s to fiioconfig success!\n", buf);

	return count;
}

int write_left_dac_master_trim_index_to_storage(unsigned char *pdata)
{
	int err;

	err = fiio_config_kernel_write_to_storage(pdata, 4, LEFT_DAC_TRIM_POS);
	return err;
}
EXPORT_SYMBOL(write_left_dac_master_trim_index_to_storage);

int read_left_dac_master_trim_index_from_storage(unsigned char *pdata)
{
	int err;

	err = fiio_config_kernel_read_from_storage(pdata, 4, LEFT_DAC_TRIM_POS);
	return err;
}
EXPORT_SYMBOL(read_left_dac_master_trim_index_from_storage);

int write_right_dac_master_trim_index_to_storage(unsigned char *pdata)
{
	int err;

	err = fiio_config_kernel_write_to_storage(pdata, 4, RIGHT_DAC_TRIM_POS);
	return err;
}
EXPORT_SYMBOL(write_right_dac_master_trim_index_to_storage);

int read_right_dac_master_trim_index_from_storage(unsigned char *pdata)
{
	int err;

	err = fiio_config_kernel_read_from_storage(pdata, 4, RIGHT_DAC_TRIM_POS);
	return err;
}
EXPORT_SYMBOL(read_right_dac_master_trim_index_from_storage);

static ssize_t sequence_number_read_SN(struct device *dev, struct device_attribute *attr, char *buf)
{
	int count;
	int err;
	int i;
	unsigned char pdata[NAME_LEN] = {0};

	printk("enter %s\n", __func__);

	err = fiio_config_kernel_read_from_storage(pdata, NAME_LEN, SN_POS);
	if (err) {
		count = sprintf(buf, "FiiOSN00000000\n");
		return count;
	}

	for (i = 0; i < NAME_LEN; i++) {
		if (pdata[i] == '\n')
			pdata[i] = '\0';
	}

	count = sprintf(buf, "%s\n", pdata);
	return count;
}

static ssize_t sequence_number_write_SN(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t count)
{
	int i;
	int err;
	loff_t pos;
	unsigned char pdata[NAME_LEN] = {0};

	printk("enter %s\n", __func__);
	printk("write %s to fiioconfig SN, strlen is %d!\n", buf, (int)strlen(buf));

	for (i = 0; i < NAME_LEN; i++) {
		if (buf[i] == '\n') {
			pdata[i] = '\0';
			break;
		}
		pdata[i] = buf[i];
	}

	pos = SN_POS;
	err = fiio_config_kernel_write_to_storage(pdata, NAME_LEN, pos);
	if (!err)
		printk("write %s to fiioconfig fail!\n", pdata);
	else
		printk("write %s to fiioconfig success!\n", pdata);

	return count;
}

static ssize_t sequence_number_read_NB(struct device *dev, struct device_attribute *attr, char *buf)
{
	int count;
	int err;
	int i;
	unsigned char pdata[NAME_LEN] = {0};

	printk("enter %s\n", __func__);

	err = fiio_config_kernel_read_from_storage(pdata, NAME_LEN, NB_POS);
	if (err) {
		count = sprintf(buf, "FiiONB00000000\n");
		return count;
	}

	for (i = 0; i < NAME_LEN; i++) {
		if (pdata[i] == '\n')
			pdata[i] = '\0';
	}

	count = sprintf(buf, "%s\n", pdata);
	return count;
}

static ssize_t sequence_number_write_NB(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t count)
{
	int i;
	int err;
	loff_t pos;
	unsigned char pdata[NAME_LEN] = {0};

	printk("enter %s\n", __func__);
	printk("write %s to fiioconfig NB, strlen is %d!\n", buf, (int)strlen(buf));

	for (i = 0; i < NAME_LEN; i++) {
		if (buf[i] == '\n') {
			pdata[i] = '\0';
			break;
		}
		pdata[i] = buf[i];
	}

	pos = NB_POS;
	err = fiio_config_kernel_write_to_storage(pdata, NAME_LEN, pos);
	if (!err)
		printk("write %s to fiioconfig fail!\n", pdata);
	else
		printk("write %s to fiioconfig success!\n", pdata);

	return count;
}

/* disable_battery_charge_level and charging_disable_status are the same record */
static const struct device_attribute fiio_config_attrs[] = {
	__ATTR(disable_battery_charge_level, 0644, charging_disable_status_show, charging_disable_status_store),
	__ATTR(power_off_charge_switch, 0644, power_off_charge_switch_show, power_off_charge_switch_store),
	__ATTR(usb_boot_switch, 0644, usb_boot_switch_show, usb_boot_switch_store),
	__ATTR(charging_disable_status, 0644, charging_disable_status_show, charging_disable_status_store),
	__ATTR(fpga_firmware_name, 0644, fpga_firmware_name_show, fpga_firmware_name_store),
	__ATTR(config_sn, 0644, sequence_number_read_SN, sequence_number_write_SN),
	__ATTR(config_nb, 0644, sequence_number_read_NB, sequence_number_write_NB),
};

static int fiio_config_probe(struct platform_device *pdev)
{
	int ret;

	ret = fiio_device_attribute_register(fiio_config_attrs, ARRAY_SIZE(fiio_config_attrs));
	if (ret)
		dev_err(&pdev->dev, "sysfs create error: %d\n", ret);

	return ret;
}

static int fiio_config_remove(struct platform_device *pdev)
{
	fiio_device_attribute_unregister(fiio_config_attrs);
	return 0;
}

static const struct of_device_id fiio_config_dt_ids[] = {
	{ .compatible = "fiio,config" },
	{},
};
MODULE_DEVICE_TABLE(of, fiio_config_dt_ids);

static struct platform_driver fiio_config_driver = {
	.probe = fiio_config_probe,
	.remove = fiio_config_remove,
	.driver = {
		.name = "fiio_config",
		.owner = THIS_MODULE,
		.of_match_table = fiio_config_dt_ids,
	},
};

static int __init fiio_config_init(void)
{
	return platform_driver_register(&fiio_config_driver);
}

static void __exit fiio_config_exit(void)
{
	platform_driver_unregister(&fiio_config_driver);
}

module_init(fiio_config_init);
module_exit(fiio_config_exit);

MODULE_AUTHOR("pengweizhong@fiio.net");
MODULE_DESCRIPTION("FiiO Config Driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
MODULE_IMPORT_NS(CRYPTO_INTERNAL);
