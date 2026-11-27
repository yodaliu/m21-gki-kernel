// SPDX-License-Identifier: GPL-2.0
/*
 * Touchscreen selection: several touchscreen drivers probe the same slot,
 * and the first one that matches the LCD panel on the command line (or
 * answers a ping) wins.
 */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/regulator/consumer.h>

#include <fiio/ts_common.h>
#include <fiio/fiio_utils.h>

struct ts_data {
	struct ts_device *device_used;
};

static struct ts_data *g_data;

void ts_core_set_vaild_tp(struct ts_device *_ts_device, bool vaild)
{
	if (!g_data)
		g_data = kzalloc(sizeof(*g_data), GFP_KERNEL);

	if (vaild) {
		g_data->device_used = _ts_device;
		printk("[%s] now had vaild tp (%s)!", __func__, _ts_device->name);
	} else {
		g_data->device_used = NULL;
		printk("[%s] tp exit (%s)!", __func__, _ts_device->name);
	}
	_ts_device->used = vaild;
}
EXPORT_SYMBOL(ts_core_set_vaild_tp);

struct ts_device *ts_core_get_vaild_tp(void)
{
	if (!g_data)
		g_data = kzalloc(sizeof(*g_data), GFP_KERNEL);

	return g_data->device_used;
}
EXPORT_SYMBOL(ts_core_get_vaild_tp);

static bool ts_core_had_vaild_tp(void)
{
	return ts_core_get_vaild_tp() != NULL;
}

/* "msm_drm.dsi_display0=<panel>:..." -> "<panel>" */
static const char *ts_core_get_lcd_name_from_bootargs(void)
{
	static char lcd_name[64];
	struct device_node *chosen;
	const char *bootargs = NULL;
	const char *start;
	char *end;

	lcd_name[0] = '\0';

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return NULL;

	if (of_property_read_string(chosen, "bootargs", &bootargs))
		return NULL;

	start = strstr(bootargs, "msm_drm.dsi_display0=");
	if (!start)
		return NULL;

	start += strlen("msm_drm.dsi_display0=");
	if (!*start)
		return NULL;

	strlcpy(lcd_name, start, sizeof(lcd_name));
	end = strchr(lcd_name, ':');
	if (!end)
		end = strchr(lcd_name, ' ');
	if (end)
		*end = '\0';

	return lcd_name[0] ? lcd_name : NULL;
}

#define TS_MATCH	0
#define TS_NOT_MATCH	1
#define TS_FALLBACK	2

static int ts_core_select_tp_by_lcd_cmdline(struct i2c_client *client,
					    struct ts_device *ts_device)
{
	struct device *dev = &client->dev;
	struct device_node *np = dev->of_node;
	const char *lcd_name = ts_core_get_lcd_name_from_bootargs();
	const char *support_lcd = NULL;
	int ret, i, count;

	ts_device->dev = dev;
	strcpy(ts_device->name, dev->driver->name);
	strcpy(ts_device->tp_vcc_name, "vcc_i2c");
	ts_device->default_flag = false;

	if (!lcd_name)
		return TS_FALLBACK;

	printk("[%s] lcd_name from bootargs = %s\n", __func__, lcd_name);

	if (!np) {
		printk("[%s] %s no device node, fallback to ping\n", __func__,
		       ts_device->name);
		return TS_FALLBACK;
	}

	count = of_property_count_strings(np, "fiio,lcd-panel-names");
	if (count <= 0) {
		printk("[%s] %s no lcd-panel mapping, fallback to ping\n", __func__,
		       ts_device->name);
		return TS_FALLBACK;
	}

	for (i = 0; i < count; i++) {
		ret = of_property_read_string_index(np, "fiio,lcd-panel-names", i,
						    &support_lcd);
		if (ret < 0)
			continue;
		if (!strcmp(lcd_name, support_lcd)) {
			printk("[%s] lcd %s match tp %s\n", __func__, lcd_name,
			       ts_device->name);
			set_product_lcd_panel_name(lcd_name);
			printk("[%s] set product lcd panel name = %s\n", __func__,
			       lcd_name);
			return TS_MATCH;
		}
	}

	printk("[%s] lcd %s not match tp %s\n", __func__, lcd_name, ts_device->name);
	return TS_NOT_MATCH;
}

static int ts_core_parse_dt_basic(struct i2c_client *client,
				  struct ts_device *ts_device)
{
	struct device *dev = &client->dev;
	struct device_node *np = dev->of_node;
	u32 temp_val = 0;
	int ret;

	ts_device->dev = dev;

	ts_device->reset_gpio = of_get_named_gpio(np, "reset-gpio", 0);
	if (gpio_is_valid(ts_device->reset_gpio)) {
		ret = gpio_request(ts_device->reset_gpio, "reset-gpio");
		if (ret < 0)
			printk("[%s] reset gpio_request failed:%d\n", __func__, ret);
		printk("%s:ts_device->reset_gpio=%d, value = %d\n", __func__,
		       ts_device->reset_gpio, gpio_get_value(ts_device->reset_gpio));
	} else {
		printk("%s:Unable to get reset_gpio", __func__);
	}

	of_property_read_u32(np, "reg", &temp_val);
	ts_device->addr = temp_val;
	printk("%s:ts_device->addr=0x%x\n", __func__, ts_device->addr);

	ts_device->i2c_adapter = client->adapter->nr;
	printk("%s:ts_device->i2c_adapter = %d\n", __func__, ts_device->i2c_adapter);

	temp_val = 0;
	of_property_read_u32(np, "default-flags", &temp_val);
	ts_device->default_flag = temp_val ? true : false;
	printk("%s:ts_device->default_flag = %s\n", __func__,
	       ts_device->default_flag ? "ture" : "false");

	strcpy(ts_device->name, dev->driver->name);
	printk("%s:ts_device->name=%s\n", __func__, ts_device->name);
	strcpy(ts_device->tp_vcc_name, "vcc_i2c");
	printk("%s:ts_device->tp_vcc_name=%s\n", __func__, ts_device->tp_vcc_name);

	return 0;
}

static void power_ctrl(struct ts_device *_ts_device, bool enable)
{
	int ret;

	if (enable) {
		printk("[%s] regulator enable %s!\n", __func__, _ts_device->tp_vcc_name);
		msleep(1);
		if (!IS_ERR_OR_NULL(_ts_device->vcc_i2c)) {
			ret = regulator_enable(_ts_device->vcc_i2c);
			printk("[%s] %s = %dmV\n", __func__, _ts_device->tp_vcc_name,
			       regulator_get_voltage(_ts_device->vcc_i2c));
			if (ret)
				printk("enable vcc_i2c regulator failed,ret=%d\n", ret);
		}
	} else {
		printk("regulator disable !\n");
		msleep(1);
		if (!IS_ERR_OR_NULL(_ts_device->vcc_i2c)) {
			ret = regulator_disable(_ts_device->vcc_i2c);
			if (ret)
				printk("disable vcc_i2c regulator failed,ret=%d\n", ret);
		}
	}
}

static void device_deinit(struct ts_device *_ts_device)
{
	if (!IS_ERR_OR_NULL(_ts_device->vcc_i2c)) {
		printk("regulator_put vcc_i2c\n");
		regulator_put(_ts_device->vcc_i2c);
	}

	if (gpio_is_valid(_ts_device->reset_gpio))
		gpio_free(_ts_device->reset_gpio);
}

static int ts_core_try_to_ping(struct ts_device *ts_device)
{
	int ret;

	printk("[%s] try to ping ts device(%s@0x%x)\n", __func__, ts_device->name,
	       ts_device->addr);
	ret = ts_device->device_init(ts_device);
	if (ret) {
		printk("[%s] init failed...\n", __func__);
		return ret;
	}

	power_ctrl(ts_device, true);
	mdelay(50);
	ts_device->reset_ctrl(ts_device, 1);
	mdelay(50);
	ret = ts_device->ping_device(ts_device);
	ts_device->reset_ctrl(ts_device, 0);
	power_ctrl(ts_device, false);
	device_deinit(ts_device);

	if (!ret) {
		printk("[%s] ping ts device(%s@0x%x) success, choose it.\n", __func__,
		       ts_device->name, ts_device->addr);
	} else if (ts_device->default_flag) {
		printk("[%s] ping ts device(%s@0x%x) failed, but default choose it.\n",
		       __func__, ts_device->name, ts_device->addr);
		ret = 0;
	} else {
		printk("[%s] ping ts device(%s@0x%x) failed, maybe not exit...\n",
		       __func__, ts_device->name, ts_device->addr);
	}

	return ret;
}

int try_to_ping(struct i2c_client *client, struct ts_device *ts_device)
{
	int ret;

	printk("[%s] enter!", __func__);

	if (ts_core_had_vaild_tp()) {
		printk("[%s] allready had vaild tp", __func__);
		return -ENXIO;
	}

	ret = ts_core_select_tp_by_lcd_cmdline(client, ts_device);
	if (ret == TS_NOT_MATCH)
		goto not_this;

	if (ret == TS_FALLBACK) {
		ts_core_parse_dt_basic(client, ts_device);
		if (ts_core_try_to_ping(ts_device))
			goto not_this;
	}

	ts_core_set_vaild_tp(ts_device, true);
	printk("[%s] exit!", __func__);
	return 0;

not_this:
	printk("[%s] not this touchscreen!", __func__);
	return -ENXIO;
}
EXPORT_SYMBOL(try_to_ping);
