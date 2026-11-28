// SPDX-License-Identifier: GPL-2.0
/*
 * Cellwise CW221X fuel gauge driver, as modified by FiiO for the M21.
 * Reconstructed from the stock FiiO M21 module.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/power_supply.h>
#include <fiio/fiio_device.h>
#include <fiio/fiio_attr.h>
#include <fiio/fiio_i2c.h>
#include <fiio/fuel_gauge_common.h>

#define cw_printk(fmt, arg...) printk(KERN_ERR "[FG_CW221X]: " fmt, ##arg)

#define CWFG_ENABLE_LOG 1

#define REG_CHIP_ID		0x00
#define REG_VCELL_H		0x02
#define REG_VCELL_L		0x03
#define REG_SOC_INT		0x04
#define REG_SOC_DECIMAL		0x05
#define REG_TEMP		0x06
#define REG_MODE_CONFIG		0x08
#define REG_GPIO_CONFIG		0x0A
#define REG_SOC_ALERT		0x0B
#define REG_TEMP_MAX		0x0C
#define REG_TEMP_MIN		0x0D
#define REG_CURRENT_H		0x0E
#define REG_CURRENT_L		0x0F
#define REG_T_HOST_H		0xA0
#define REG_T_HOST_L		0xA1
#define REG_USER_CONF		0xA2
#define REG_CYCLE_H		0xA4
#define REG_CYCLE_L		0xA5
#define REG_SOH			0xA6
#define REG_IC_STATE		0xA7
#define REG_STB_CUR_H		0xA8
#define REG_STB_CUR_L		0xA9
#define REG_FW_VERSION		0xAB
#define REG_BAT_PROFILE		0x10

#define CONFIG_MODE_RESTART	0x30
#define CONFIG_MODE_ACTIVE	0x00
#define CONFIG_MODE_SLEEP	0xF0
#define CONFIG_UPDATE_FLG	0x80
#define IC_VCHIP_ID		0xA0
#define IC_READY_MARK		0x0C

#define GPIO_ENABLE_MIN_TEMP	0
#define GPIO_ENABLE_MAX_TEMP	0
#define GPIO_ENABLE_SOC_CHANGE	0
#define GPIO_SOC_IGNORE		0xFF
#define DEFINED_MAX_TEMP	450
#define DEFINED_MIN_TEMP	0

#define CWFG_NAME		"cw221X"
#define SIZE_OF_PROFILE		80
#define USER_RSENSE		(2 * 1000)

#define queue_delayed_work_time	1000
#define queue_start_work_time	50

#define CW_SLEEP_20MS		20
#define CW_SLEEP_10MS		10
#define CW_UI_FULL		100
#define COMPLEMENT_CODE_U16	0x8000
#define CW_SLEEP_100MS		100
#define CW_SLEEP_200MS		200
#define CW_SLEEP_COUNTS		50
#define CW_TRUE			1
#define CW_RETRY_COUNT		3
#define CW_VOL_UNIT		1000

#define CW221X_NOT_ACTIVE		1
#define CW221X_PROFILE_NOT_READY	2
#define CW221X_PROFILE_NEED_UPDATE	3

#define CW2215_MARK		0x80
#define CW2217_MARK		0x40
#define CW2218_MARK		0x00

static unsigned char config_profile_info[SIZE_OF_PROFILE];

struct cw_battery {
	struct i2c_client *client;

	struct workqueue_struct *cwfg_workqueue;
	struct delayed_work battery_delay_work;
	struct power_supply *cw_bat;

	int chip_id;
	int voltage;
	int ic_soc_h;
	int ic_soc_l;
	int ui_soc;
	int temp;
	long cw_current;
	int cycle;
	int soh;
	int fw_version;
	int batt_id;
	int batt_capacity;

	struct list_head list;
};

static LIST_HEAD(cw2217_device_list);
struct cw_battery *g_cw_bat;

static int cw_read(struct i2c_client *client, unsigned char reg, unsigned char buf[])
{
	int ret;

	ret = fiio_i2c_smbus_read_i2c_block_data(client, reg, 1, buf);
	if (ret < 0)
		cw_printk("IIC error %d\n", ret);
	return ret;
}

static int cw_write(struct i2c_client *client, unsigned char reg, unsigned char const buf[])
{
	int ret;

	ret = fiio_i2c_smbus_write_i2c_block_data(client, reg, 1, &buf[0]);
	if (ret < 0)
		cw_printk("IIC error %d\n", ret);
	return ret;
}

static int cw_read_word(struct i2c_client *client, unsigned char reg, unsigned char buf[])
{
	int ret;
	unsigned char reg_val[2] = { 0, 0 };
	unsigned int temp_val_buff;
	unsigned int temp_val_second;

	ret = fiio_i2c_smbus_read_i2c_block_data(client, reg, 2, reg_val);
	if (ret < 0)
		cw_printk("IIC error %d\n", ret);
	temp_val_buff = (reg_val[0] << 8) + reg_val[1];

	msleep(4);
	ret = fiio_i2c_smbus_read_i2c_block_data(client, reg, 2, reg_val);
	if (ret < 0)
		cw_printk("IIC error %d\n", ret);
	temp_val_second = (reg_val[0] << 8) + reg_val[1];

	if (temp_val_buff != temp_val_second) {
		msleep(4);
		ret = fiio_i2c_smbus_read_i2c_block_data(client, reg, 2, reg_val);
		if (ret < 0)
			cw_printk("IIC error %d\n", ret);
	}

	buf[0] = reg_val[0];
	buf[1] = reg_val[1];

	return ret;
}

static int cw_write_profile(struct i2c_client *client, unsigned char const buf[])
{
	int ret;
	int i;

	for (i = 0; i < SIZE_OF_PROFILE; i++) {
		ret = cw_write(client, REG_BAT_PROFILE + i, &buf[i]);
		if (ret < 0) {
			cw_printk("IIC error %d\n", ret);
			return ret;
		}
	}

	return ret;
}

static int cw221X_active(struct cw_battery *cw_bat)
{
	int ret;
	unsigned char reg_val = CONFIG_MODE_RESTART;

	ret = cw_write(cw_bat->client, REG_MODE_CONFIG, &reg_val);
	if (ret < 0)
		return ret;
	msleep(CW_SLEEP_20MS);

	reg_val = CONFIG_MODE_ACTIVE;
	ret = cw_write(cw_bat->client, REG_MODE_CONFIG, &reg_val);
	if (ret < 0)
		return ret;
	msleep(CW_SLEEP_10MS);

	return 0;
}

static int cw221X_sleep(struct cw_battery *cw_bat)
{
	int ret;
	unsigned char reg_val = CONFIG_MODE_RESTART;

	ret = cw_write(cw_bat->client, REG_MODE_CONFIG, &reg_val);
	if (ret < 0)
		return ret;
	msleep(CW_SLEEP_20MS);

	reg_val = CONFIG_MODE_SLEEP;
	ret = cw_write(cw_bat->client, REG_MODE_CONFIG, &reg_val);
	if (ret < 0)
		return ret;
	msleep(CW_SLEEP_10MS);

	return 0;
}

static int cw_get_chip_id(struct cw_battery *cw_bat)
{
	int ret;
	unsigned char reg_val;
	int chip_id;

	ret = cw_read(cw_bat->client, REG_CHIP_ID, &reg_val);
	if (ret < 0)
		return ret;

	chip_id = reg_val;
	cw_bat->chip_id = chip_id;

	return 0;
}

static int cw_get_voltage(struct cw_battery *cw_bat)
{
	int ret;
	unsigned char reg_val[2] = { 0, 0 };
	unsigned int voltage;

	ret = cw_read_word(cw_bat->client, REG_VCELL_H, reg_val);
	if (ret < 0)
		return ret;
	voltage = (reg_val[0] << 8) + reg_val[1];
	voltage = voltage * 5 / 16;
	cw_bat->voltage = voltage;

	return 0;
}

static int cw_get_capacity(struct cw_battery *cw_bat)
{
	int ret;
	unsigned char reg_val[2] = { 0, 0 };
	int ui_100 = CW_UI_FULL;
	int soc_h;
	int soc_l;
	int ui_soc;
	int remainder;

	ret = cw_read_word(cw_bat->client, REG_SOC_INT, reg_val);
	if (ret < 0)
		return ret;
	soc_h = reg_val[0];
	soc_l = reg_val[1];
	ui_soc = ((soc_h * 256 + soc_l) * 100) / (ui_100 * 256);
	remainder = (((soc_h * 256 + soc_l) * 100 * 100) / (ui_100 * 256)) % 100;
	if (ui_soc >= 100)
		ui_soc = 100;
	cw_bat->ic_soc_h = soc_h;
	cw_bat->ic_soc_l = soc_l;
	cw_bat->ui_soc = ui_soc;

	return 0;
}

int extern_get_cap(void)
{
	struct list_head *pos;
	struct cw_battery *cw_bat = NULL;

	list_for_each(pos, &cw2217_device_list) {
		cw_bat = list_entry(pos, struct cw_battery, list);
		if (!cw_bat) {
			printk(KERN_ERR "cw2217 device is not ready\n");
			return -1;
		}
	}

	cw_get_capacity(cw_bat);
	{
		int cap = cw_bat->ui_soc;

		return cap;
	}
}
EXPORT_SYMBOL_GPL(extern_get_cap);

static int cw_get_temp(struct cw_battery *cw_bat)
{
	int ret;
	unsigned char reg_val;
	int temp;
	static int temp_old = -1000;
	static int temp_confirm_count;
	bool cw_temp_confirm_en = true;

	ret = cw_read(cw_bat->client, REG_TEMP, &reg_val);
	if (ret < 0)
		return ret;

	if (reg_val == 0) {
		cw_printk("temp error, temp =  %d\n", (int)reg_val * 10 / 2 - 400);
		return 0;
	}
	temp = (int)reg_val * 10 / 2 - 400;

	if (cw_temp_confirm_en && temp_old != -1000 && ((temp - temp_old) > 100 || (temp - temp_old) < -100)) {
		temp_confirm_count++;
		if (temp_confirm_count < 7) {
			cw_printk("%s:there is a temp saltus step, temp_old = %d, temp = %d, temp_confirm_count = %d\n",
				  __func__, temp_old, temp, temp_confirm_count);
			return 0;
		}
	}
	temp_old = temp;
	temp_confirm_count = 0;
	cw_bat->temp = temp;

	return 0;
}

static long get_complement_code(unsigned short raw_code)
{
	long complement_code;
	int dir;

	if ((raw_code & COMPLEMENT_CODE_U16) != 0) {
		dir = -1;
		raw_code = (0xFFFF - raw_code) + 1;
	} else {
		dir = 1;
	}
	complement_code = (long)raw_code * dir;

	return complement_code;
}

static int cw_get_current(struct cw_battery *cw_bat)
{
	int ret;
	unsigned char reg_val[2] = { 0, 0 };
	long long cw_current;
	unsigned short current_reg;

	ret = cw_read_word(cw_bat->client, REG_CURRENT_H, reg_val);
	if (ret < 0)
		return ret;

	current_reg = (reg_val[0] << 8) + reg_val[1];
	cw_current = get_complement_code(current_reg);
	if ((cw_bat->fw_version & CW2215_MARK) != 0 || (cw_bat->fw_version & CW2217_MARK) != 0) {
		cw_current = cw_current * 1600 * 1000 / 2;
	} else if (cw_bat->fw_version != 0 && (cw_bat->fw_version & 0xC0) == CW2218_MARK) {
		cw_current = (long long)((int)cw_current * 3815 / 2) * 1000;
	} else {
		cw_bat->cw_current = 0;
		cw_printk("error! cw221x frimware read error!\n");
	}
	cw_bat->cw_current = cw_current / 1000;

	return 0;
}

static int cw_get_cycle_count(struct cw_battery *cw_bat)
{
	int ret;
	unsigned char reg_val[2] = { 0, 0 };
	int cycle;

	ret = cw_read_word(cw_bat->client, REG_CYCLE_H, reg_val);
	if (ret < 0)
		return ret;

	cycle = (reg_val[0] << 8) + reg_val[1];
	cw_bat->cycle = cycle / 16;

	return 0;
}

static int cw_get_soh(struct cw_battery *cw_bat)
{
	int ret;
	unsigned char reg_val;
	int soh;

	ret = cw_read(cw_bat->client, REG_SOH, &reg_val);
	if (ret < 0)
		return ret;

	soh = reg_val;
	cw_bat->soh = soh;

	return 0;
}

static int cw_get_fw_version(struct cw_battery *cw_bat)
{
	int ret;
	unsigned char reg_val;
	int fw_version;

	ret = cw_read(cw_bat->client, REG_FW_VERSION, &reg_val);
	if (ret < 0)
		return ret;

	fw_version = reg_val;
	cw_bat->fw_version = fw_version;

	return 0;
}

static int cw_update_data(struct cw_battery *cw_bat)
{
	int ret = 0;

	ret += cw_get_voltage(cw_bat);
	ret += cw_get_capacity(cw_bat);
	ret += cw_get_temp(cw_bat);
	ret += cw_get_current(cw_bat);
	ret += cw_get_cycle_count(cw_bat);
	ret += cw_get_soh(cw_bat);

	return ret;
}

static int cw_init_data(struct cw_battery *cw_bat)
{
	int ret = 0;

	ret = cw_get_fw_version(cw_bat);
	if (ret != 0)
		return ret;

	ret += cw_get_chip_id(cw_bat);
	ret += cw_get_voltage(cw_bat);
	ret += cw_get_capacity(cw_bat);
	ret += cw_get_temp(cw_bat);
	ret += cw_get_current(cw_bat);
	ret += cw_get_cycle_count(cw_bat);
	ret += cw_get_soh(cw_bat);
	cw_printk("chip_id = %d vol = %d  cur = %ld cap = %d temp = %d  fw_version = %d, soh:%d\n",
		  cw_bat->chip_id, cw_bat->voltage, cw_bat->cw_current, cw_bat->ui_soc,
		  cw_bat->temp, cw_bat->fw_version, cw_bat->soh);

	return ret;
}

static int cw_config_start_ic(struct cw_battery *cw_bat)
{
	int ret;
	unsigned char reg_val;
	int count = 0;

	ret = cw221X_sleep(cw_bat);
	if (ret < 0)
		return ret;

	ret = cw_write_profile(cw_bat->client, config_profile_info);
	if (ret < 0)
		return ret;

	reg_val = CONFIG_UPDATE_FLG;
	ret = cw_write(cw_bat->client, REG_SOC_ALERT, &reg_val);
	if (ret < 0)
		return ret;

	reg_val = 0;
	ret = cw_write(cw_bat->client, REG_GPIO_CONFIG, &reg_val);
	if (ret < 0)
		return ret;

	ret = cw221X_active(cw_bat);
	if (ret < 0)
		return ret;

	while (CW_TRUE) {
		msleep(CW_SLEEP_100MS);
		cw_read(cw_bat->client, REG_IC_STATE, &reg_val);
		if (IC_READY_MARK == (reg_val & IC_READY_MARK))
			break;
		count++;
		if (count >= CW_SLEEP_COUNTS) {
			cw221X_sleep(cw_bat);
			return -1;
		}
	}

	return 0;
}

static int cw221X_get_state(struct cw_battery *cw_bat)
{
	int ret;
	unsigned char reg_val;
	int i;
	int reg_profile;

	ret = cw_read(cw_bat->client, REG_MODE_CONFIG, &reg_val);
	if (ret < 0)
		return ret;
	if (reg_val != CONFIG_MODE_ACTIVE)
		return CW221X_NOT_ACTIVE;

	ret = cw_read(cw_bat->client, REG_SOC_ALERT, &reg_val);
	if (ret < 0)
		return ret;
	if ((reg_val & CONFIG_UPDATE_FLG) == 0x00)
		return CW221X_PROFILE_NOT_READY;

	for (i = 0; i < SIZE_OF_PROFILE; i++) {
		ret = cw_read(cw_bat->client, (REG_BAT_PROFILE + i), &reg_val);
		if (ret < 0)
			return ret;
		reg_profile = REG_BAT_PROFILE + i;
		if (config_profile_info[i] != reg_val)
			break;
	}
	if (i != SIZE_OF_PROFILE)
		return CW221X_PROFILE_NEED_UPDATE;

	return 0;
}

static int cw_init(struct cw_battery *cw_bat)
{
	int ret;

	ret = cw_get_chip_id(cw_bat);
	if (ret < 0) {
		cw_printk("iic read write error");
		return ret;
	}
	if (cw_bat->chip_id != IC_VCHIP_ID) {
		cw_printk("not cw221X\n");
		return -1;
	}

	ret = cw221X_get_state(cw_bat);
	if (ret < 0) {
		cw_printk("iic read write error");
		return ret;
	}

	if (ret != 0) {
		ret = cw_config_start_ic(cw_bat);
		if (ret < 0)
			return ret;
	}
	cw_printk("%s cw221X init success!\n", __func__);

	return 0;
}

static void cw_bat_work(struct work_struct *work)
{
	struct delayed_work *delay_work;
	struct cw_battery *cw_bat;
	int ret;
	static int old_cap;

	delay_work = container_of(work, struct delayed_work, work);
	cw_bat = container_of(delay_work, struct cw_battery, battery_delay_work);

	ret = cw_update_data(cw_bat);
	if (ret < 0)
		cw_printk(KERN_ERR "iic read error when update data");

	if (cw_bat->ui_soc != old_cap)
		power_supply_changed(cw_bat->cw_bat);
	old_cap = cw_bat->ui_soc;

	queue_delayed_work(cw_bat->cwfg_workqueue, &cw_bat->battery_delay_work,
			   msecs_to_jiffies(queue_delayed_work_time));
}

static int cw_get_battery_profile(struct device *dev, struct cw_battery *cw_bat)
{
	struct device_node *np = dev->of_node;
	const char *data;
	const char *data2;
	int ret, len, len2, batt_id_gpio;
	int capacity = 0, capacity2 = 0;

	cw_bat->batt_id = 0;
	cw_bat->batt_capacity = 0;

	of_property_read_u32(np, "cw,capacity", &capacity);
	of_property_read_u32(np, "cw,capacity2", &capacity2);

	batt_id_gpio = of_get_named_gpio(np, "cw,batt_id", 0);
	if (gpio_is_valid(batt_id_gpio)) {
		ret = gpio_request(batt_id_gpio, "cw,batt_id");
		if (ret < 0) {
			cw_printk("[%s] gpio_request cw,batt_id failed\n", __func__);
		} else {
			gpio_direction_input(batt_id_gpio);
			cw_bat->batt_id = gpio_get_value_cansleep(batt_id_gpio);
		}
	} else {
		cw_printk("[%s] gpio cw,batt_id failed\n", __func__);
	}

	data = of_get_property(np, "cw,fg_profile", &len);
	if (!data) {
		cw_printk("No profile data available\n");
		return -ENODATA;
	}

	data2 = of_get_property(np, "cw,fg_profile_2", &len2);
	if (!data2)
		cw_printk("No profile 2 data available\n");

	if (data2 && cw_bat->batt_id == 1) {
		memcpy(config_profile_info, data2, len2);
		if (capacity2 > 0)
			cw_bat->batt_capacity = capacity2;
	} else {
		memcpy(config_profile_info, data, len);
		if (capacity > 0)
			cw_bat->batt_capacity = capacity;
	}

	cw_printk("[%s] batt_id:%d, capacity:%d\n", __func__, cw_bat->batt_id, cw_bat->batt_capacity);

	return 0;
}

static int cw_battery_set_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   const union power_supply_propval *val)
{
	int ret = -EINVAL;

	switch (psp) {
	default:
		break;
	}

	return ret;
}

static int cw_battery_get_property(struct power_supply *psy,
				   enum power_supply_property psp,
				   union power_supply_propval *val)
{
	int ret = 0;
	struct cw_battery *cw_bat;

	cw_bat = power_supply_get_drvdata(psy);
	val->intval = -ENODATA;
	if (!cw_bat)
		return -1;

	switch (psp) {
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		val->intval = cw_bat->cycle;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = cw_bat->ui_soc;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = cw_bat->voltage <= 0 ? 0 : 1;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = cw_bat->voltage * CW_VOL_UNIT;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = cw_bat->cw_current;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = cw_bat->temp;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static enum power_supply_property cw_battery_properties[] = {
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_TEMP,
};

static int cw2217_fuel_gauge_bat_cap(void)
{
	struct cw_battery *cw_bat = g_cw_bat;
	int cap;

	cap = cw_bat->ui_soc;
	return cap;
}

static int cw2217_fuel_gauge_bat_cur(void)
{
	struct cw_battery *cw_bat = g_cw_bat;

	return cw_bat->cw_current;
}

static int cw2217_fuel_gauge_bat_val(void)
{
	struct cw_battery *cw_bat = g_cw_bat;
	int vol;

	vol = cw_bat->voltage * CW_VOL_UNIT;
	return vol;
}

static int cw2217_fuel_gauge_bat_temp(void)
{
	struct cw_battery *cw_bat = g_cw_bat;
	int temp;

	temp = cw_bat->temp;
	return temp;
}

static int cw2217_fuel_gauge_iic_test(void)
{
	struct cw_battery *cw_bat = g_cw_bat;
	int ret;
	unsigned char reg_val = CONFIG_UPDATE_FLG;

	ret = cw_write(cw_bat->client, REG_SOC_ALERT, &reg_val);
	return ret;
}

static ssize_t cw_batt_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cw_battery *cw_bat = g_cw_bat;

	return sprintf(buf, "%d\n", cw_bat->batt_id);
}

static ssize_t cw_batt_capacity_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cw_battery *cw_bat = g_cw_bat;

	return sprintf(buf, "%d\n", cw_bat->batt_capacity);
}

static ssize_t cw_batt_soh_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cw_battery *cw_bat = g_cw_bat;

	return sprintf(buf, "%d\n", cw_bat->soh);
}

static const struct device_attribute g_cw221x_attributes[] = {
	__ATTR(cw_batt_id, 0644, cw_batt_id_show, NULL),
	__ATTR(cw_batt_capacity, 0644, cw_batt_capacity_show, NULL),
	__ATTR(cw_batt_soh, 0644, cw_batt_soh_show, NULL),
};

static struct fuel_gauge_data cw2217_fuel_gauge_ops = {
	.specific_fuel_gauge_get_bat_cap = cw2217_fuel_gauge_bat_cap,
	.specific_fuel_gauge_get_bat_cur = cw2217_fuel_gauge_bat_cur,
	.specific_fuel_gauge_get_bat_val = cw2217_fuel_gauge_bat_val,
	.specific_fuel_gauge_get_bat_temp = cw2217_fuel_gauge_bat_temp,
	.specific_fuel_gauge_iic_test = cw2217_fuel_gauge_iic_test,
};

static struct fiio_device_driver fuel_gauge_driver = {
	.name = "fuel_gauge module",
	.private_data = &cw2217_fuel_gauge_ops,
};

static int cw221X_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	int ret;
	int loop = 0;
	struct cw_battery *cw_bat;
	struct power_supply_desc *psy_desc;
	struct power_supply_config psy_cfg = { 0 };

	cw_printk("%s enter \n", __func__);

	cw_bat = devm_kzalloc(&client->dev, sizeof(*cw_bat), GFP_KERNEL);
	if (!cw_bat) {
		cw_printk("%s : cw_bat create fail!\n", __func__);
		return -ENOMEM;
	}

	i2c_set_clientdata(client, cw_bat);
	cw_bat->client = client;
	g_cw_bat = cw_bat;

	ret = cw_get_battery_profile(&client->dev, cw_bat);
	if (ret) {
		cw_printk("%s  cw221X get battery profile fail!\n", __func__);
		return ret;
	}

	ret = cw_init(cw_bat);
	while ((loop++ < CW_RETRY_COUNT) && (ret != 0)) {
		msleep(CW_SLEEP_200MS);
		ret = cw_init(cw_bat);
	}
	if (ret) {
		cw_printk("%s : cw221X init fail!\n", __func__);
		return ret;
	}

	ret = cw_init_data(cw_bat);
	if (ret) {
		cw_printk("%s : cw221X init data fail!\n", __func__);
		return ret;
	}

	psy_desc = devm_kzalloc(&client->dev, sizeof(*psy_desc), GFP_KERNEL);
	if (!psy_desc)
		return -ENOMEM;
	psy_cfg.drv_data = cw_bat;
	psy_desc->name = CWFG_NAME "-bat";
	psy_desc->type = POWER_SUPPLY_TYPE_MAINS;
	psy_desc->properties = cw_battery_properties;
	psy_desc->num_properties = ARRAY_SIZE(cw_battery_properties);
	psy_desc->get_property = cw_battery_get_property;
	psy_desc->set_property = cw_battery_set_property;
	cw_bat->cw_bat = power_supply_register(&client->dev, psy_desc, &psy_cfg);
	if (IS_ERR(cw_bat->cw_bat)) {
		ret = PTR_ERR(cw_bat->cw_bat);
		cw_printk(KERN_ERR "failed to register battery: %d\n", ret);
		return ret;
	}

	cw_bat->cwfg_workqueue = create_singlethread_workqueue("cwfg_gauge");
	INIT_DELAYED_WORK(&cw_bat->battery_delay_work, cw_bat_work);
	queue_delayed_work(cw_bat->cwfg_workqueue, &cw_bat->battery_delay_work,
			   msecs_to_jiffies(queue_start_work_time));

	list_add(&cw_bat->list, &cw2217_device_list);

	fiio_device_register(&fuel_gauge_driver);
	ret = fiio_device_attribute_register(g_cw221x_attributes, ARRAY_SIZE(g_cw221x_attributes));
	if (ret)
		cw_printk("[%s] attribute register failed!!", __func__);

	cw_printk("%s cw221X driver probe success!\n", __func__);
	return 0;
}

static int cw221X_remove(struct i2c_client *client)
{
	fiio_device_attribute_unregister(g_cw221x_attributes);
	return 0;
}

#ifdef CONFIG_PM
static int cw_bat_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct cw_battery *cw_bat = i2c_get_clientdata(client);

	cancel_delayed_work(&cw_bat->battery_delay_work);
	return 0;
}

static int cw_bat_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct cw_battery *cw_bat = i2c_get_clientdata(client);

	queue_delayed_work(cw_bat->cwfg_workqueue, &cw_bat->battery_delay_work,
			   msecs_to_jiffies(20));
	return 0;
}

static const struct dev_pm_ops cw_bat_pm_ops = {
	.suspend = cw_bat_suspend,
	.resume = cw_bat_resume,
};
#endif

static const struct i2c_device_id cw221X_id_table[] = {
	{ CWFG_NAME, 0 },
	{ }
};

static struct of_device_id cw221X_match_table[] = {
	{ .compatible = "cellwise,cw221X", },
	{ },
};

static struct i2c_driver cw221X_driver = {
	.driver = {
		.name = CWFG_NAME,
#ifdef CONFIG_PM
		.pm = &cw_bat_pm_ops,
#endif
		.owner = THIS_MODULE,
		.of_match_table = cw221X_match_table,
	},
	.probe = cw221X_probe,
	.remove = cw221X_remove,
	.id_table = cw221X_id_table,
};

static int __init cw221X_init(void)
{
	i2c_add_driver(&cw221X_driver);
	return 0;
}

static void __exit cw221X_exit(void)
{
	i2c_del_driver(&cw221X_driver);
}

module_init(cw221X_init);
module_exit(cw221X_exit);

MODULE_AUTHOR("Cellwise FAE");
MODULE_DESCRIPTION("CW221X FGADC Device Driver V0.1");
MODULE_LICENSE("GPL");
