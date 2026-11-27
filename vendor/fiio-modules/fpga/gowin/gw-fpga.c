// SPDX-License-Identifier: GPL-2.0
/*
 * Gowin FPGA (audio routing) on SPI: power rails, the slave serial
 * configuration sequence behind an fpga_manager, and the fpga_component
 * hooks fpga_interface uses to power it per playback mode.  The bitstream is
 * loaded through request_firmware with the name from the device tree or the
 * fiioconfig partition.
 */
#define pr_fmt(fmt) "[GW_FPGA]: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/spi/spi.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/firmware.h>
#include <linux/workqueue.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>
#include <fiio/fiio_attr.h>
#include <fiio/fiio_notify.h>
#include <fiio/fpga_common.h>

#include "gw_fpga_firmware.h"

static char driver_name[] = "gw_fpga";

/* id codes of the supported chips */
static int support_id_chip[] = {
	0x0100681b,
};

/* slave serial commands */
enum {
	ID_CODE = 0x11,
	USER_CODE = 0x13,
	STATUS_CODE = 0x41,
	RECONFIG_REPROGRAM = 0x3c,
	WRITE_ENABLE = 0x15,
	WRITE_DISABLE = 0x3a,
	WRITE_DATA = 0x3b,
	PROGRAM_SPI_FLASH = 0x16,
	INIT_ADDRESS = 0x12,
	ERASE_SRAM = 0x05,
	NOOP = 0x00,
};

typedef enum {
	POWER_OFF,
	POWER_ON,
	UPDATE_START,
	UPDATE_SUCCESS,
	UPDATE_FAILED,
} fpga_status;

struct gw_fpga_device;

struct gw_fpga_plat_data {
	struct device *dev;
	struct spi_device *spi_dev;
	int fpga_cs;
	int firmware_size;
	unsigned char *firmware_buffer;
	unsigned int firmware_version;
	fpga_status status;
	enum fpga_work_mode mode;
	struct gw_fpga_device *fpga_device;
	struct regulator *fpga_reg;
	struct notifier_block fpga_reg_notifier;
	struct pinctrl *fpga_pinctrl;
	char gw_fpga_fw_name[50];

	struct notifier_block fiio_control_fpga_config_notifier;
	struct delayed_work firmware_name_delay_work;
	struct work_struct update_fw_work;
	struct workqueue_struct *update_fw_wq;

	int gpio_1v2_en;
	int gpio_3v3_en;
	int code_1v8_en;
	int fpga_pwr_en;

	unsigned char *firmware_buffer_dma;
};

struct gw_fpga_device {
	struct gw_fpga_plat_data *pdata;
	void (*sspi_set_cs)(struct gw_fpga_plat_data *pdata, int level);
	int (*sspi_single_write)(struct gw_fpga_plat_data *pdata, unsigned char data);
	int (*sspi_bulk_write)(struct gw_fpga_plat_data *pdata, void *data, size_t count);
	int (*read_id_code)(struct gw_fpga_plat_data *pdata);
	int (*read_user_code)(struct gw_fpga_plat_data *pdata);
	int (*read_status_code)(struct gw_fpga_plat_data *pdata);
	int (*update_firmware)(struct gw_fpga_plat_data *pdata);
};

static struct gw_fpga_device *g_fpga_device;

void fpga_set_power(bool enable)
{
	int ret = 0;

	pr_err("%s power = %s", __func__, enable ? "on" : "off");

	if (gpio_is_valid(g_fpga_device->pdata->fpga_pwr_en)) {
		gpio_set_value(g_fpga_device->pdata->fpga_pwr_en, enable);
	} else {
		gpio_set_value(g_fpga_device->pdata->gpio_1v2_en, enable);
		gpio_set_value(g_fpga_device->pdata->gpio_3v3_en, enable);
		gpio_set_value(g_fpga_device->pdata->code_1v8_en, enable);
	}

	(void)ret;
}
EXPORT_SYMBOL_GPL(fpga_set_power);

bool fpga_get_power(void)
{
	int value;

	if (gpio_is_valid(g_fpga_device->pdata->fpga_pwr_en))
		value = gpio_get_value(g_fpga_device->pdata->fpga_pwr_en);
	else
		value = gpio_get_value(g_fpga_device->pdata->gpio_3v3_en);

	pr_err("%s power = %s\n", __func__, value == 1 ? "on" : "off");

	return value == 1;
}
EXPORT_SYMBOL_GPL(fpga_get_power);

static int fpga_init_gpio(struct gw_fpga_device *data, struct device_node *np)
{
	int ret = 0;

	g_fpga_device->pdata->fpga_pwr_en = of_get_named_gpio(np, "gpio,fpga_pwr_en", 0);
	if (gpio_is_valid(g_fpga_device->pdata->fpga_pwr_en)) {
		ret = gpio_request(g_fpga_device->pdata->fpga_pwr_en, "fpga_pwr_en");
		if (ret) {
			gpio_free(g_fpga_device->pdata->fpga_pwr_en);
			pr_err(KERN_ERR "fpga_pwr_en request failed!!\n");
		} else {
			pr_err("%s: fpga_pwr_en = %d\n", __func__, g_fpga_device->pdata->fpga_pwr_en);
			gpio_direction_output(g_fpga_device->pdata->fpga_pwr_en, 0);
		}
	} else {
		pr_err("%s fpga_pwr_en not found!!\n", __func__);
	}

	g_fpga_device->pdata->gpio_1v2_en = of_get_named_gpio(np, "gpio,fpga_1v2_en", 0);
	if (gpio_is_valid(g_fpga_device->pdata->gpio_1v2_en)) {
		ret = gpio_request(g_fpga_device->pdata->gpio_1v2_en, "fpga_1v2_en");
		if (ret) {
			gpio_free(g_fpga_device->pdata->gpio_1v2_en);
			pr_err(KERN_ERR "fpga_1v2_en request failed!!\n");
			return ret;
		}
		pr_err("%s: fpga_1v2_en = %d\n", __func__, g_fpga_device->pdata->gpio_1v2_en);
		gpio_direction_output(g_fpga_device->pdata->gpio_1v2_en, 0);
	} else {
		pr_err("%s fpga_1v2_en not found!!\n", __func__);
	}

	g_fpga_device->pdata->gpio_3v3_en = of_get_named_gpio(np, "gpio,fpga_3v3_en", 0);
	if (gpio_is_valid(g_fpga_device->pdata->gpio_3v3_en)) {
		ret = gpio_request(g_fpga_device->pdata->gpio_3v3_en, "fpga_3v3_en");
		if (ret) {
			gpio_free(g_fpga_device->pdata->gpio_3v3_en);
			pr_err(KERN_ERR "fpga_3v3_en request failed!!\n");
			return ret;
		}
		pr_err("%s: fpga_3v3_en = %d\n", __func__, g_fpga_device->pdata->gpio_3v3_en);
		gpio_direction_output(g_fpga_device->pdata->gpio_3v3_en, 0);
		pr_err("value :  %d\n", gpio_get_value(g_fpga_device->pdata->gpio_3v3_en));
	} else {
		pr_err("%s fpga_3v3_en not found!!\n", __func__);
	}

	g_fpga_device->pdata->code_1v8_en = of_get_named_gpio(np, "gpio,code_1v8_en", 0);
	if (gpio_is_valid(g_fpga_device->pdata->code_1v8_en)) {
		ret = gpio_request(g_fpga_device->pdata->code_1v8_en, "code_1v8_en");
		if (ret) {
			gpio_free(g_fpga_device->pdata->code_1v8_en);
			pr_err(KERN_ERR "code_1v8_en request failed!!\n");
			return ret;
		}
		pr_err("%s: code_1v8_en = %d\n", __func__, g_fpga_device->pdata->code_1v8_en);
		gpio_direction_output(g_fpga_device->pdata->code_1v8_en, 0);
	} else {
		pr_err("%s code_1v8_en not found!!\n", __func__);
	}

	(void)data;
	return ret;
}

static int gw_spi_gpio_setup(int gpio, bool config, int dir, int state)
{
	int retval = 0;
	unsigned char buf[20];

	if (config) {
		snprintf(buf, PAGE_SIZE, "gw_spi_gpio_%u\n", gpio);

		retval = gpio_request(gpio, buf);
		if (retval) {
			printk(KERN_ERR "%s: Failed to get gpio %d (code: %d)", __func__, gpio, retval);
			return retval;
		}

		if (dir == 0)
			retval = gpio_direction_input(gpio);
		else
			retval = gpio_direction_output(gpio, state);
		if (retval) {
			printk(KERN_ERR "%s: Failed to set gpio %d direction", __func__, gpio);
			return retval;
		}
	} else {
		gpio_free(gpio);
	}

	return retval;
}

static int gw_spi_config_gpio(struct gw_fpga_plat_data *pdata)
{
	int retval;

	retval = gw_spi_gpio_setup(pdata->fpga_cs, true, 1, 0);
	if (retval < 0) {
		dev_err(pdata->dev, "%s: Failed to configure Led PWM GPIO\n", __func__);
		goto err_gpio_cs;
	}

	return 0;

err_gpio_cs:
	return retval;
}

static unsigned char *g_buf_w;
static unsigned char *g_buf_r;

/* the transfer goes through the DMA-safe buffer; payloads over 100 bytes get the WRITE_DATA command first */
static int fiio_spi_write(struct spi_device *spi, const void *buf, size_t len)
{
	int ret;

	if (len == 0)
		return -1;

	if (!g_fpga_device->pdata || !g_fpga_device->pdata->firmware_buffer_dma) {
		pr_err("%s:fail to kzalloc!", __func__);
		return 0;
	}

	if (len > 100) {
		g_fpga_device->pdata->firmware_buffer_dma[0] = WRITE_DATA;
		memcpy(g_fpga_device->pdata->firmware_buffer_dma + 1, buf, len);
		len += 1;
	} else {
		memcpy(g_fpga_device->pdata->firmware_buffer_dma, buf, len);
	}

	ret = spi_write(spi, g_fpga_device->pdata->firmware_buffer_dma, len);
	if (len == 2)
		pr_err("%s, len=%d ret=%d firmware_buffer_dma 0x%02x 0x%02x\n", __func__, (int)len, ret,
		       g_fpga_device->pdata->firmware_buffer_dma[0],
		       g_fpga_device->pdata->firmware_buffer_dma[1]);
	else if (len == 1)
		pr_err("%s, len=%d ret=%d firmware_buffer_dma 0x%02x\n", __func__, (int)len, ret,
		       g_fpga_device->pdata->firmware_buffer_dma[0]);
	else
		pr_err("%s, len=%d ret=%d\n", __func__, (int)len, ret);

	return ret;
}

static int fiio_spi_write_then_read(struct spi_device *spi,
				    const void *txbuf, unsigned int n_tx,
				    void *rxbuf, unsigned int n_rx)
{
	int ret;

	if (g_buf_w) {
		kfree(g_buf_w);
		g_buf_w = NULL;
	}
	g_buf_w = kzalloc(n_tx, GFP_KERNEL | GFP_DMA);
	memcpy(g_buf_w, txbuf, n_tx);

	if (g_buf_r) {
		kfree(g_buf_r);
		g_buf_r = NULL;
	}
	g_buf_r = kzalloc(n_rx, GFP_KERNEL | GFP_DMA);

	ret = spi_write_then_read(spi, g_buf_w, n_tx, g_buf_r, n_rx);
	memcpy(rxbuf, g_buf_r, n_rx);

	pr_err("%s, n_tx=%d %02x %02x %02x %02x n_rx=%d ret=%d %02x %02x %02x %02x\n", __func__,
	       n_tx, g_buf_w[0], g_buf_w[1], g_buf_w[2], g_buf_w[3],
	       n_rx, ret, ((unsigned char *)rxbuf)[0], ((unsigned char *)rxbuf)[1],
	       ((unsigned char *)rxbuf)[2], ((unsigned char *)rxbuf)[3]);

	return ret;
}

/* raising CS also clocks out one dummy byte */
static void sspi_set_cs(struct gw_fpga_plat_data *pdata, int level)
{
	unsigned char data = 0;

	if (gpio_is_valid(pdata->fpga_cs)) {
		if (level) {
			gpio_set_value(pdata->fpga_cs, 1);
			fiio_spi_write(pdata->spi_dev, &data, 1);
		} else {
			gpio_set_value(pdata->fpga_cs, 0);
		}
		msleep(5);
	}
}

static int sspi_single_write(struct gw_fpga_plat_data *pdata, unsigned char data)
{
	struct gw_fpga_device *fpga_device = pdata->fpga_device;

	fpga_device->sspi_set_cs(pdata, 0);
	fiio_spi_write(pdata->spi_dev, &data, 1);
	fpga_device->sspi_set_cs(pdata, 1);

	return 0;
}

static int sspi_bulk_write(struct gw_fpga_plat_data *pdata, void *data, size_t count)
{
	int ret;
	struct gw_fpga_device *fpga_device = pdata->fpga_device;

	fpga_device->sspi_set_cs(pdata, 0);
	ret = fiio_spi_write(pdata->spi_dev, data, count);
	fpga_device->sspi_set_cs(pdata, 1);

	return ret;
}

static int sspi_read_code(struct gw_fpga_plat_data *pdata, unsigned char code_id)
{
	int return_value;
	unsigned char write_buffer[4] = { code_id, 0, 0, 0 };
	unsigned char read_buffer[4] = { 0 };
	struct gw_fpga_device *fpga_device = pdata->fpga_device;

	fpga_device->sspi_set_cs(pdata, 1);
	fpga_device->sspi_set_cs(pdata, 0);
	fiio_spi_write_then_read(pdata->spi_dev, write_buffer, 4, read_buffer, 4);
	return_value = read_buffer[0] << 24 | read_buffer[1] << 16 | read_buffer[2] << 8 | read_buffer[3];
	fpga_device->sspi_set_cs(pdata, 1);

	return return_value;
}

static int sspi_update_firmware(struct gw_fpga_plat_data *pdata)
{
	int ret;
	struct gw_fpga_device *fpga_device = pdata->fpga_device;
	unsigned char buffer[2] = { INIT_ADDRESS, NOOP };

	pr_err("start update firmware.\n");
	fpga_device->sspi_set_cs(pdata, 1);

	ret = fpga_device->sspi_bulk_write(pdata, buffer, 2);
	if (ret < 0) {
		pr_err("INIT_ADDRESS:Fail to update firmware.\n");
		return ret;
	}

	buffer[0] = WRITE_ENABLE;
	ret = fpga_device->sspi_bulk_write(pdata, buffer, 2);
	if (ret < 0) {
		pr_err("WRITE_ENABLE:Fail to update firmware.\n");
		return ret;
	}

	ret = fpga_device->sspi_bulk_write(pdata, pdata->firmware_buffer, pdata->firmware_size);
	if (ret < 0) {
		pr_err("WRITE_DATA:Fail to update firmware.\n");
		return ret;
	}

	buffer[0] = WRITE_DISABLE;
	ret = fpga_device->sspi_bulk_write(pdata, buffer, 2);
	if (ret < 0) {
		pr_err("WRITE_DISABLE:Fail to update firmware.\n");
		return ret;
	}

	pr_err("end update firmware.\n");
	return 0;
}

static int read_id_code(struct gw_fpga_plat_data *pdata)
{
	int id_code = sspi_read_code(pdata, ID_CODE);

	return id_code;
}

static int read_user_code(struct gw_fpga_plat_data *pdata)
{
	int user_code = sspi_read_code(pdata, USER_CODE);

	return user_code;
}

static int read_status_code(struct gw_fpga_plat_data *pdata)
{
	int status_code = sspi_read_code(pdata, STATUS_CODE);

	return status_code;
}

/* fpga_mgr_load() of the configured firmware name, which ends up in fpga_mgr_write() */
static int fpga_update_firmware(struct gw_fpga_plat_data *pdata)
{
	struct gw_fpga_device *fpag_device = pdata->fpga_device;
	struct device *spi_dev = pdata->dev;
	struct fpga_manager *fpga_mgr;
	struct fpga_image_info info = { 0 };
	int ret = 0;

	fpga_mgr = fpga_mgr_get(spi_dev);
	if (IS_ERR(fpga_mgr)) {
		printk(KERN_ERR "failed to get fpga manager\n");
		return ret;
	}

	info.firmware_name = kzalloc(50, GFP_ATOMIC);
	strcpy(info.firmware_name, pdata->gw_fpga_fw_name);
	ret = fpga_mgr_load(fpga_mgr, &info);
	fpga_mgr_put(fpga_mgr);
	kfree(info.firmware_name);

	(void)fpag_device;
	return ret;
}

static ssize_t update_firmware(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct gw_fpga_device *fpag_device = g_fpga_device;

	pr_err("%s try to update the fpga firmware.\n", __func__);
	fpag_device->update_firmware(fpag_device->pdata);

	return count;
}

static ssize_t power_ctrl(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	pr_err("%s, %s \n", __func__, buf);

	if (!strcmp(buf, "enable\n"))
		fpga_set_power(true);
	else if (!strcmp(buf, "disable\n"))
		fpga_set_power(false);

	return count;
}

static int check_id_code(int id_code)
{
	int i;
	int ctype_entries = ARRAY_SIZE(support_id_chip);

	for (i = 0; i <= ctype_entries; i++) {
		if (id_code == support_id_chip[i]) {
			pr_err("+++ %x\t%x +++\n", id_code, support_id_chip[i]);
			return 0;
		}
	}

	return -1;
}

/* power the FPGA and load the bitstream, up to 10 tries, until the user code matches the firmware version */
static void fpga_reload_firmware(void)
{
	struct gw_fpga_device *fpag_device = g_fpga_device;
	struct gw_fpga_plat_data *pdata = g_fpga_device->pdata;
	int id_code;
	int user_code;
	int status_code = 0;
	int try_times = 10;
	int ret;

	pr_err("%s: Enter \n", __func__);

	while (try_times--) {
		fpga_set_power(true);
		msleep(50);
		pdata->status = POWER_ON;

		id_code = fpag_device->read_id_code(fpag_device->pdata);
		pr_err("%s: check 00001--1 id_code=%d\n", __func__, id_code);
		if (id_code <= 0) {
			pdata->status = UPDATE_FAILED;
			printk(KERN_ERR "%s: ERROR not support fpga,id code = 0x%x\n", __func__, id_code);
			return;
		}

		ret = check_id_code(id_code);
		if (ret < 0) {
			pdata->status = UPDATE_FAILED;
			return;
		}

		pdata->status = UPDATE_START;
		ret = fpag_device->update_firmware(fpag_device->pdata);
		if (ret < 0) {
			pdata->status = UPDATE_FAILED;
			break;
		}

		user_code = fpag_device->read_user_code(fpag_device->pdata);
		status_code = fpag_device->read_status_code(fpag_device->pdata);
		pr_err("%s: status_code=%d \n", __func__, status_code);
		if (user_code == pdata->firmware_version) {
			pdata->status = UPDATE_SUCCESS;
			pr_err("%s: update fpga firmware success,id code = 0x%x , user code = 0x%x , status code =0x%x\n",
			       __func__, id_code, user_code, status_code);
			break;
		}

		pr_err("%s: update fpga firmware failed,id code = 0x%x , user code = 0x%x , status code =0x%x\n",
		       __func__, id_code, user_code, status_code);
		fpga_set_power(false);
		pr_err("%s: try to reload the fpga firmware. times:%d \n", __func__, try_times + 1);
	}

	if (try_times < 0) {
		pdata->status = UPDATE_FAILED;
		pr_err("update fpga firmware failed,error code = 0x%x ", status_code);
	}

	pr_err("%s: Exit \n", __func__);
}

static void update_fw_work_func(struct work_struct *data)
{
	fpga_reload_firmware();
}

/* queue the reload and wait up to 2 s for it to finish */
static void fpga_reload_firmware_en(void)
{
	struct gw_fpga_plat_data *pdata = g_fpga_device->pdata;
	int try_times = 100;

	queue_work(pdata->update_fw_wq, &pdata->update_fw_work);

	while (try_times-- > 0) {
		if (pdata->status > UPDATE_START)
			break;
		mdelay(20);
	}
}

void fpga_ctrl_power(bool enable)
{
	struct gw_fpga_plat_data *pdata = g_fpga_device->pdata;

	if (enable) {
		if (pdata->status == POWER_OFF && pdata->mode == LOCAL_PLAYBACK_MODE) {
			pdata->status = POWER_ON;
			fpga_reload_firmware_en();
		}
	} else {
		if (pdata->mode == LOCAL_PLAYBACK_MODE && pdata->status != POWER_OFF) {
			fpga_set_power(false);
			pdata->status = POWER_OFF;
		}
	}
}
EXPORT_SYMBOL(fpga_ctrl_power);

/* the USB XMOS mode keeps the FPGA as it is unless the caller asks for that mode */
void fpga_ctrl_power_by_mode(struct fpga_component *fpga_device, bool enable, int mode)
{
	struct gw_fpga_plat_data *pdata = g_fpga_device->pdata;

	pr_err("%s: pdata->mode:%d, mode:%d", __func__, pdata->mode, mode);

	if (pdata->mode == USB_XMOS_MODE && mode != USB_XMOS_MODE)
		return;

	if (enable) {
		if (pdata->status == POWER_OFF &&
		    (pdata->mode == LOCAL_PLAYBACK_MODE || pdata->mode == USB_XMOS_MODE)) {
			pdata->status = POWER_ON;
			fpga_reload_firmware_en();
		}
	} else {
		if ((pdata->mode == LOCAL_PLAYBACK_MODE || pdata->mode == USB_XMOS_MODE) &&
		    pdata->status != POWER_OFF) {
			fpga_set_power(false);
			pdata->status = POWER_OFF;
		}
	}
}
EXPORT_SYMBOL(fpga_ctrl_power_by_mode);

int get_current_fpga_status(void)
{
	struct gw_fpga_plat_data *pdata = g_fpga_device->pdata;

	return pdata->status;
}
EXPORT_SYMBOL(get_current_fpga_status);

void fpga_enter_usb_dac_mode(void)
{
	struct gw_fpga_plat_data *pdata = g_fpga_device->pdata;

	pdata->mode = USB_DAC_MODE;
}
EXPORT_SYMBOL(fpga_enter_usb_dac_mode);

void fpga_enter_local_playback_mode(void)
{
	struct gw_fpga_plat_data *pdata = g_fpga_device->pdata;

	pdata->mode = LOCAL_PLAYBACK_MODE;
}
EXPORT_SYMBOL(fpga_enter_local_playback_mode);

void fpga_enter_usb_xmos_mode(void)
{
	struct gw_fpga_plat_data *pdata = g_fpga_device->pdata;

	pdata->mode = USB_XMOS_MODE;
}
EXPORT_SYMBOL(fpga_enter_usb_xmos_mode);

static ssize_t fw_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gw_fpga_device *fpag_device = g_fpga_device;
	struct gw_fpga_plat_data *pdata = fpag_device->pdata;

	strcpy(buf, pdata->gw_fpga_fw_name);
	pr_err("fw_name is: %s\n", pdata->gw_fpga_fw_name);

	return strlen(buf) + 1;
}

/* a new name reloads the bitstream right away when in local playback */
void fw_name_update(const char *buf)
{
	struct gw_fpga_device *fpag_device = g_fpga_device;
	struct gw_fpga_plat_data *pdata = fpag_device->pdata;

	if (!strcmp(buf, pdata->gw_fpga_fw_name)) {
		pr_err("%s fw_name is the same: %s\n", __func__, buf);
		return;
	}

	pr_err("%s fw_name (%s) update to (%s)\n", __func__, pdata->gw_fpga_fw_name, buf);
	strcpy(pdata->gw_fpga_fw_name, buf);
	fpga_ctrl_power(false);
	msleep(50);
	fpga_ctrl_power(true);
}
EXPORT_SYMBOL_GPL(fw_name_update);

static ssize_t fw_name_sotre(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	fw_name_update(buf);

	return count;
}

static ssize_t reload_firmware(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	pr_err("%s, %s \n", __func__, buf);

	fpga_ctrl_power(false);
	msleep(50);
	fpga_ctrl_power(true);

	return count;
}

static ssize_t read_fpga_code(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	int id_code;
	int user_code;
	int status_code;
	struct gw_fpga_device *fpag_device = g_fpga_device;

	if (!strcmp(buf, "id\n")) {
		id_code = fpag_device->read_id_code(fpag_device->pdata);
		pr_err(KERN_INFO " read id code :0x%x \n", id_code);
	} else if (!strcmp(buf, "user\n")) {
		user_code = fpag_device->read_user_code(fpag_device->pdata);
		pr_err(KERN_INFO "read user code :0x%x \n", user_code);
	} else if (!strcmp(buf, "status\n")) {
		status_code = fpag_device->read_status_code(fpag_device->pdata);
		pr_err(KERN_INFO "read status code :0x%x \n", status_code);
	} else {
		id_code = fpag_device->read_id_code(fpag_device->pdata);
		user_code = fpag_device->read_user_code(fpag_device->pdata);
		status_code = fpag_device->read_status_code(fpag_device->pdata);
		pr_err(KERN_INFO " read id code :0x%x \n", id_code);
		pr_err(KERN_INFO "read user code :0x%x \n", user_code);
		pr_err(KERN_INFO "read status code :0x%x \n", status_code);
	}

	return count;
}

static ssize_t erase_firmware(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct gw_fpga_device *fpga_device = g_fpga_device;
	struct gw_fpga_plat_data *pdata = g_fpga_device->pdata;
	unsigned char buffer[2] = { ERASE_SRAM, NOOP };

	pr_err("start update firmware.\n");
	fpga_device->sspi_bulk_write(pdata, buffer, 2);

	return count;
}

static ssize_t fpga_input_func(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct gw_fpga_device *fpag_device = g_fpga_device;
	struct gw_fpga_plat_data *pdata = fpag_device->pdata;

	pdata->fpga_pinctrl = pinctrl_get_select(pdata->dev, "spi8_input");
	if (IS_ERR(pdata->fpga_pinctrl))
		pr_err("spi8_input not found\n");
	else
		pr_err("spi8_input found\n");

	return count;
}

static ssize_t fpga_spi_func(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct gw_fpga_device *fpag_device = g_fpga_device;
	struct gw_fpga_plat_data *pdata = fpag_device->pdata;

	pdata->fpga_pinctrl = pinctrl_get_select(pdata->dev, "spi8_function");
	if (!pdata->fpga_pinctrl)
		pr_err("spi8_input not found\n");
	else
		pr_err("spi8_input found\n");

	return count;
}

static ssize_t read_firmware_version(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gw_fpga_device *fpag_device = g_fpga_device;
	struct gw_fpga_plat_data *pdata = fpag_device->pdata;

	sprintf(buf, "0x%x\n", pdata->firmware_version);
	pr_err(KERN_INFO "fw version:0x%x\n", pdata->firmware_version);

	return strlen(buf) + 1;
}

int write_fpga_firmware_name_to_storage(unsigned char *pdata);

static ssize_t update_firmware_name_store(struct device *dev, struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct gw_fpga_device *fpag_device = g_fpga_device;
	struct gw_fpga_plat_data *pdata = fpag_device->pdata;
	int err;

	err = write_fpga_firmware_name_to_storage((unsigned char *)buf);
	if (!err) {
		printk("write firmware_name = %s to fiioconfig fail!\n", buf);
	} else {
		printk("write firmware_name = %s to fiioconfig success!\n", buf);
		strcpy(pdata->gw_fpga_fw_name, buf);
		fpga_ctrl_power(false);
		msleep(50);
		fpga_ctrl_power(true);
	}

	return count;
}

static const struct device_attribute fpga_device_attrs[] = {
	__ATTR(fpga_gw_fw_name, 0644, fw_name_show, fw_name_sotre),
	__ATTR(fpga_gw_update, 0200, NULL, update_firmware),
	__ATTR(fpga_gw_read_code, 0200, NULL, read_fpga_code),
	__ATTR(fpga_gw_input_func, 0200, NULL, fpga_input_func),
	__ATTR(fpga_gw_spi_func, 0200, NULL, fpga_spi_func),
	__ATTR(fpga_gw_erase, 0200, NULL, erase_firmware),
	__ATTR(fpga_gw_version, 0444, read_firmware_version, NULL),
	__ATTR(fpga_gw_power_ctrl, 0200, NULL, power_ctrl),
	__ATTR(fpga_gw_reload, 0200, NULL, reload_firmware),
	__ATTR(fpga_gw_update_firmware_name, 0200, NULL, update_firmware_name_store),
};

static enum fpga_mgr_states fpga_mgr_state(struct fpga_manager *mgr)
{
	pr_err("fpga_mgr_state.\n");
	return FPGA_MGR_STATE_UNKNOWN;
}

static int fpga_mgr_write_init(struct fpga_manager *mgr, struct fpga_image_info *info,
			       const char *buf, size_t count)
{
	return 0;
}

/* the firmware version is the big-endian word 26 bytes before the end of the bitstream */
static unsigned int cal_firmware_version(unsigned char *firmware_buffer, int firmware_size)
{
	unsigned int version_1, version_2, version_3, version_4;
	unsigned int version;

	version_1 = *(firmware_buffer + firmware_size - 26);
	version_2 = *(firmware_buffer + firmware_size - 25);
	version_3 = *(firmware_buffer + firmware_size - 24);
	version_4 = *(firmware_buffer + firmware_size - 23);
	version = version_1 << 24 | version_2 << 16 | version_3 << 8 | version_4;

	return version;
}

static int fpga_mgr_write(struct fpga_manager *mgr, const char *buf, size_t count)
{
	int ret;
	struct gw_fpga_device *fpga_device = mgr->priv;
	struct gw_fpga_plat_data *pdata = fpga_device->pdata;

	pr_err("[%s] Enter!.\n", __func__);

	if (pdata->firmware_buffer) {
		kfree(pdata->firmware_buffer);
		pdata->firmware_buffer = NULL;
	}

	pdata->firmware_size = count;
	pdata->firmware_buffer = kzalloc(pdata->firmware_size, GFP_KERNEL);
	memcpy(pdata->firmware_buffer, buf, pdata->firmware_size);
	pdata->firmware_version = cal_firmware_version(pdata->firmware_buffer, pdata->firmware_size);
	pr_err("[%s] pdata->firmware_version=0x%08x pdata->firmware_size=%d\n", __func__,
	       pdata->firmware_version, pdata->firmware_size);

	ret = sspi_update_firmware(fpga_device->pdata);
	if (ret < 0) {
		printk(KERN_ERR "%s:fail to update fpga!\n", __func__);
		fpga_set_power(false);
		pdata->status = POWER_OFF;
	}

	pr_err("[%s] Exit!.\n", __func__);
	return 0;
}

static int fpga_mgr_write_complete(struct fpga_manager *mgr, struct fpga_image_info *info)
{
	pr_err("fpga_mgr_write_complete.\n");
	return 0;
}

static const struct fpga_manager_ops fpga_mgr_ops = {
	.state = fpga_mgr_state,
	.write_init = fpga_mgr_write_init,
	.write = fpga_mgr_write,
	.write_complete = fpga_mgr_write_complete,
};

static int fiio_control_fpga_ctrl_notifier_cb(struct notifier_block *self,
					      unsigned long event, void *data)
{
	struct gw_fpga_plat_data *gw_fpga_data =
		container_of(self, struct gw_fpga_plat_data, fiio_control_fpga_config_notifier);
	struct gw_fpga_device *fpag_device;
	struct fiio_control_fpga_ctrl_notify_data *notify_data = data;
	int id_code = 0;
	int user_code = 0;
	int status_code = 0;

	if (!notify_data)
		return 0;

	switch (notify_data->event) {
	case FIIO_CONFIG_FPGA_CONTRL_GET_INFO:
		fpag_device = gw_fpga_data->fpga_device;
		pr_err("%s: FIIO_CONFIG_FPGA_CONTRL_GET_INFO", __func__);
		id_code = fpag_device->read_id_code(fpag_device->pdata);
		user_code = fpag_device->read_user_code(fpag_device->pdata);
		status_code = fpag_device->read_status_code(fpag_device->pdata);
		pr_err(KERN_INFO " read id code :0x%x \n", id_code);
		pr_err(KERN_INFO "read user code :0x%x \n", user_code);
		pr_err(KERN_INFO "read status code :0x%x \n", status_code);
		break;
	default:
		break;
	}

	notify_data->call_back(id_code, user_code, status_code);
	return 0;
}

int read_fpga_firmware_name_from_storage(unsigned char *pdata);

/* the firmware name stored on fiioconfig overrides the device tree one; retried every 5 s until readable */
static void firmware_name_delay_func(struct work_struct *work)
{
	struct gw_fpga_plat_data *pdata =
		container_of(to_delayed_work(work), struct gw_fpga_plat_data, firmware_name_delay_work);
	int err;
	char firmware_name[50] = { 0 };

	err = read_fpga_firmware_name_from_storage((unsigned char *)firmware_name);
	if (err)
		schedule_delayed_work(&pdata->firmware_name_delay_work, msecs_to_jiffies(5000));
	else if (firmware_name[0] != '\0')
		strcpy(pdata->gw_fpga_fw_name, firmware_name);

	pr_err("%s:pdata->gw_fpga_fw_name=%s g_fpga_device->pdata->gw_fpga_fw_name=%s firmware_name=%s\n",
	       __func__, pdata->gw_fpga_fw_name, g_fpga_device->pdata->gw_fpga_fw_name, firmware_name);
}

static int gw_spi_probe(struct spi_device *spi)
{
	struct gw_fpga_device *fpga_device;
	struct gw_fpga_plat_data *pdata;
	struct device *dev = &spi->dev;
	struct device_node *np = dev->of_node;
	int retval;
	struct fpga_manager *fpga_mgr;
	struct pinctrl *fpga_pinctrl = NULL;
	const struct firmware *fw = NULL;
	int ret;
	const char *name;

	pr_err("[%s] enter ... \n", __func__);

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	if (!pdata) {
		dev_err(dev, "%s: Failed to allocate memory for fpga pdata.\n", __func__);
		return -ENOMEM;
	}

	fpga_device = kzalloc(sizeof(*fpga_device), GFP_KERNEL);
	if (!fpga_device) {
		dev_err(dev, "%s: Failed to allocate memory for fpga device\n", __func__);
		return -ENOMEM;
	}

	pdata->fpga_device = fpga_device;
	g_fpga_device = fpga_device;
	pdata->dev = dev;
	pdata->spi_dev = spi;
	pdata->status = POWER_OFF;

	fpga_device->pdata = pdata;
	fpga_device->sspi_set_cs = sspi_set_cs;
	fpga_device->sspi_single_write = sspi_single_write;
	fpga_device->sspi_bulk_write = sspi_bulk_write;
	fpga_device->read_id_code = read_id_code;
	fpga_device->read_user_code = read_user_code;
	fpga_device->read_status_code = read_status_code;
	fpga_device->update_firmware = fpga_update_firmware;

	pdata->fpga_cs = of_get_named_gpio(np, "qcom,fpga-cs-pin", 0);
	pr_err("fpga_cs = %d \n", pdata->fpga_cs);
	if (pdata->fpga_cs >= 0)
		retval = gw_spi_config_gpio(pdata);

	ret = fpga_init_gpio(fpga_device, np);

	name = of_get_property(np, "gw-fpga-fw-name", NULL);
	if (name)
		strcpy(pdata->gw_fpga_fw_name, name);
	else
		pdata->gw_fpga_fw_name[0] = '\0';

	fpga_mgr = devm_fpga_mgr_create(dev, "GW Slave Serial FPGA Manager", &fpga_mgr_ops, fpga_device);
	if (!fpga_mgr) {
		pr_err("gw_spi_probe GW Slave Serial FPGA Manager error.\n");
		return -ENOMEM;
	}
	fpga_mgr_register(fpga_mgr);

	pdata->update_fw_wq = alloc_ordered_workqueue("update_fw_wq", WQ_HIGHPRI);
	if (!pdata->update_fw_wq) {
		printk(KERN_ERR "%s: cread  update_fw_wq failed\n", __func__);
		return -ENOMEM;
	}

	pdata->fiio_control_fpga_config_notifier.notifier_call = fiio_control_fpga_ctrl_notifier_cb;
	INIT_WORK(&pdata->update_fw_work, update_fw_work_func);
	fiio_register_client(notifier_fpga, &pdata->fiio_control_fpga_config_notifier);

	ret = fiio_device_attribute_register(fpga_device_attrs, ARRAY_SIZE(fpga_device_attrs));
	if (ret) {
		dev_err(dev, "sysfs create error: %d\n", ret);
		fiio_device_attribute_unregister(fpga_device_attrs);
	}

	{
		int try_counter = 0;

		while (try_counter++ < 100) {
			pdata->firmware_buffer_dma = kzalloc(40960, GFP_KERNEL | GFP_DMA);
			if (pdata->firmware_buffer_dma)
				break;
			pr_err("%s:fail to kzalloc firmware_buffer_dma try_counter=%d!", __func__, try_counter);
			msleep(5);
		}
	}

	INIT_DELAYED_WORK(&pdata->firmware_name_delay_work, firmware_name_delay_func);
	schedule_delayed_work(&pdata->firmware_name_delay_work, msecs_to_jiffies(10000));

	pr_err("[%s] exit ... \n", __func__);

	(void)retval;
	(void)fpga_pinctrl;
	(void)fw;
	return 0;
}

static int gw_spi_remove(struct spi_device *spi)
{
	struct fpga_manager *mgr = spi_get_drvdata(spi);
	struct gw_fpga_device *fpga_device = mgr->priv;
	struct gw_fpga_plat_data *pdata = fpga_device->pdata;

	pr_err("[%s] enter ... \n", __func__);

	regulator_unregister_notifier(pdata->fpga_reg, &pdata->fpga_reg_notifier);
	fpga_mgr_unregister(mgr);
	fiio_device_attribute_unregister(fpga_device_attrs);

	if (pdata->firmware_buffer)
		kfree(pdata->firmware_buffer);
	if (pdata->firmware_buffer_dma)
		kfree(pdata->firmware_buffer_dma);
	if (pdata->fpga_cs >= 0)
		gpio_free(pdata->fpga_cs);
	if (fpga_device)
		kfree(fpga_device);
	if (pdata)
		kfree(pdata);

	return 0;
}

static const struct spi_device_id gw_id_table[] = {
	{ "qcom,gw-fpga", 0 },
	{},
};
MODULE_DEVICE_TABLE(spi, gw_id_table);

static struct of_device_id gw_of_match_table[] = {
	{ .compatible = "qcom,gw-fpga" },
	{},
};
MODULE_DEVICE_TABLE(of, gw_of_match_table);

static struct spi_driver gw_spi_driver = {
	.driver = {
		.name = driver_name,
		.owner = THIS_MODULE,
		.of_match_table = gw_of_match_table,
	},
	.probe = gw_spi_probe,
	.remove = gw_spi_remove,
	.id_table = gw_id_table,
};

static int __init gw_fpga_init(void)
{
	pr_err("[%s] enter ... \n", __func__);
	return spi_register_driver(&gw_spi_driver);
}

static void __exit gw_fpga_exit(void)
{
	pr_err("[%s] enter ... \n", __func__);
	spi_unregister_driver(&gw_spi_driver);
}

module_init(gw_fpga_init);
module_exit(gw_fpga_exit);

MODULE_DESCRIPTION("GW FPGA Driver Module");
MODULE_LICENSE("GPL");
