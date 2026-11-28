// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO M21 power board: VBUS/OTG switching, PD voltage selection and
 * battery temperature compensation. Reconstructed from the stock module.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/interrupt.h>
#include <linux/extcon.h>
#include <linux/extcon-provider.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <fiio/fiio_device.h>
#include <fiio/fiio_notify.h>
#include "power_board_m21.h"

#define PB_printk(fmt, arg...) printk(KERN_ERR "[power_board]: " fmt, ##arg)

static struct power_board_priv *g_data;

void power_set_gpio_value(int gpio, int value)
{
	if (gpio_is_valid(gpio))
		gpio_direction_output(gpio, value);
	else
		PB_printk("%s: error, no gpio %d!\n", __func__, gpio);
}

int power_get_gpio_value(int gpio)
{
	int ret;

	if (gpio_is_valid(gpio)) {
		ret = gpio_get_value_cansleep(gpio);
	} else {
		PB_printk("%s: error, no gpio %d!\n", __func__, gpio);
		ret = -1;
	}

	return ret;
}

static int power_board_buck_charger_set_charge_current(unsigned int cur)
{
	struct power_board_priv *data = g_data;
	int ret;

	if (!data->buck_charger_module) {
		PB_printk("%s: buck_charger module not found\n", __func__);
		return -ENODEV;
	}

	ret = data->buck_charger_module->specific_buck_charger_set_charge_current(cur);
	if (ret < 0)
		PB_printk("%s: failed to set charge current\n", __func__);
	else
		PB_printk("%s:set charge current %d\n", __func__, cur);

	return ret;
}

static int power_board_buck_charger_set_limit_current(unsigned int cur)
{
	struct power_board_priv *data = g_data;
	int ret;

	if (!data->buck_charger_module) {
		PB_printk("%s: buck_charger module not found\n", __func__);
		return -ENODEV;
	}

	ret = data->buck_charger_module->specific_buck_charger_set_limit_current(cur);
	if (ret < 0)
		PB_printk("%s: failed to set charge limit current\n", __func__);
	else
		PB_printk("%s:set charge limit current %d\n", __func__, cur);

	return ret;
}

static void power_board_buck_charger_set_uac_mode(bool enable)
{
	struct power_board_priv *data = g_data;

	if (data->buck_charger_module && data->buck_charger_module->specific_buck_charger_set_uac_mode)
		data->buck_charger_module->specific_buck_charger_set_uac_mode(enable);
}

static int power_board_uac_charging_disabled(bool enable)
{
	struct power_board_priv *data = g_data;
	int ret = 0;

	power_board_buck_charger_set_uac_mode(enable);

	if (enable) {
		if (data->usb_device && !data->power_in_vbus)
			power_board_buck_charger_set_limit_current(10000);
		else if (data->power_in_vbus)
			power_board_buck_charger_set_limit_current(3000000);
	} else {
		power_board_buck_charger_set_limit_current(3000000);
	}

	data->uac_charging_disabled = enable;

	return ret;
}

static int power_board_get_charge_status_indicator(void)
{
	struct power_board_priv *data = g_data;
	enum led_status value = BLUE;

	if (!data->led_module) {
		PB_printk("%s: led_module module not found.\n", __func__);
		return -ENODEV;
	}

	data->led_module->get_indicator_status(&value);
	PB_printk("%s:set charge status indicator.\n", __func__);

	return value;
}

static int power_board_set_charge_status_indicator(enum led_status color)
{
	struct power_board_priv *data = g_data;
	int ret;

	if (!data->led_module) {
		PB_printk("%s: led_module module not found.\n", __func__);
		return -ENODEV;
	}

	data->led_module->set_indicator_status(color);
	ret = 0;
	PB_printk("%s:set charge status indicator.\n", __func__);

	return ret;
}

static int power_board_buck_charge_start_charge(bool enable)
{
	struct power_board_priv *data = g_data;
	int ret;

	if (!data->buck_charger_module) {
		PB_printk("%s: buck_charger module not found\n", __func__);
		return -ENODEV;
	}

	ret = data->buck_charger_module->specific_buck_charger_start_charge(enable);
	if (ret < 0) {
		PB_printk("%s:failed to start buck charge\n", __func__);
		return ret;
	}

	if (data->led_module) {
		data->led_module->is_charging = enable;
		PB_printk("%s:is_charging = %d\n", __func__, data->led_module->is_charging);
	}
	PB_printk("%s: start buck charge\n", __func__);

	return ret;
}

static unsigned char power_board_fix_modulation_power(unsigned short pdo_vol, unsigned short pdo_cur)
{
	struct power_board_priv *data = g_data;
	unsigned char ret;

	data->pd_voltage_gear = pdo_vol;
	printk("%s pd_voltage_gear = %d\n", __func__, data->pd_voltage_gear);

	if (!data->charger_protocol_fiio_module) {
		PB_printk("%s: charger_protocol_fiio_module module not found\n", __func__);
		return -ENODEV;
	}

	ret = data->charger_protocol_fiio_module->fix_ajust_power(pdo_vol, pdo_vol);
	if (!ret)
		PB_printk("%s:fix regulation is not supported!\n", __func__);

	return ret;
}

void power_get_module_work_func(struct work_struct *work)
{
	struct power_board_priv *data = container_of(work, struct power_board_priv,
						     power_get_module_work.work);

	data->buck_charger_module = fiio_device_get_data_by_name("buck_charger module");
	if (!data->buck_charger_module)
		PB_printk("get buck_charger module failed!\n");

	data->led_module = fiio_device_get_data_by_name("led module");
	if (!data->led_module)
		PB_printk("get led module failed!\n");
}

static bool power_board_check_desktop_mode(void)
{
	struct power_board_priv *data = g_data;
	bool state = false;

	if (gpio_is_valid(data->gpio_ctrl[DC_BAT_SW]))
		state = gpio_get_value_cansleep(data->gpio_ctrl[DC_BAT_SW]) == 0;

	return state;
}

static int power_get_battery_temperature(void)
{
	struct power_board_priv *data = g_data;
	int temp = 0;

	if (data->fuel_gauge_module && data->fuel_gauge_module->specific_fuel_gauge_get_bat_temp)
		temp = data->fuel_gauge_module->specific_fuel_gauge_get_bat_temp();

	return temp;
}

static int battery_temp_compansetion(struct power_board_priv *data, int temp)
{
	enum machine_state power_machine_state;
	int output_type = 0;
	int output_gain = 0;
	int output_volume = 0;
	static int batt_temp_compensation_value;
	bool is_playing = false;
	bool is_charging = false;

	if (data->anolog_board_module) {
		is_playing = data->anolog_board_module->isPlaying;
		if (data->anolog_board_module->get_output_type)
			output_type = data->anolog_board_module->get_output_type();
		if (data->anolog_board_module->get_output_gain)
			output_gain = data->anolog_board_module->get_output_gain();
		if (data->anolog_board_module->get_output_volume)
			output_volume = data->anolog_board_module->get_output_volume();
	}

	if (data->buck_charger_module && data->buck_charger_module->specific_buck_check_charging)
		is_charging = data->buck_charger_module->specific_buck_check_charging();

	if (is_charging && is_playing) {
		if (output_type == 0) {
			if ((output_gain == 1 || output_gain == 2) &&
			    output_volume > 100 && output_volume < 121)
				power_machine_state = CHARGING_AND_PLAYING_3000mV;
			else
				power_machine_state = CHARGING_AND_PLAYING_500mV;
		} else if (output_type == 3) {
			if ((output_gain == 2 && output_volume > 80 && output_volume < 111) ||
			    (output_gain == 1 && output_volume > 90 && output_volume < 121))
				power_machine_state = CHARGING_AND_PLAYING_3000mV;
			else if (output_gain > 1 && output_volume > 110)
				power_machine_state = CHARGING_AND_PLAYING_4600mV;
			else if (output_gain == 0 && output_volume > 100 && output_volume < 121)
				power_machine_state = CHARGING_AND_PLAYING_3000mV;
			else
				power_machine_state = CHARGING_AND_PLAYING_500mV;
		} else {
			power_machine_state = CHARGING_AND_PLAYING_500mV;
		}
	} else if (data->is_desktop_mode) {
		power_machine_state = DESKTOP_MODE;
	} else if (is_charging && !is_playing) {
		power_machine_state = CHARGING;
	} else if (temp >= 0) {
		power_machine_state = DISCHARGING;
	} else {
		power_machine_state = BATTERY_TEMP_SUPER_COOL;
	}

	switch (power_machine_state) {
	case CHARGING:
	case DISCHARGING:
	case CHARGING_AND_PLAYING_500mV:
		data->slope_select = data->low_speed_slope_coefficient;
		break;
	case DESKTOP_MODE:
		data->slope_select = data->super_low_speed_slope_coefficient;
		break;
	case CHARGING_AND_PLAYING_3000mV:
		data->slope_select = data->medium_speed_slope_coefficient;
		break;
	case CHARGING_AND_PLAYING_4600mV:
		data->slope_select = data->fast_speed_slope_coefficient;
		break;
	default:
		data->slope_select = data->default_speed_slope_coefficient;
		goto uncompensation;
	}

	if (temp < 350)
		goto uncompensation;

	if (power_machine_state != data->power_machine_state) {
		data->update_temp_count = 0;
		data->power_machine_state = power_machine_state;
		goto compensation;
	}

	if (data->update_temp_count < 6) {
		data->update_temp_count++;
		if (data->update_temp_count == 5) {
			switch (power_machine_state) {
			case CHARGING:
				batt_temp_compensation_value = data->only_charging_temp_compensation;
				data->slope_select = data->low_speed_slope_coefficient;
				break;
			case DISCHARGING:
			case CHARGING_AND_PLAYING_500mV:
				batt_temp_compensation_value = data->low_load_temp_compensation;
				data->slope_select = data->low_speed_slope_coefficient;
				break;
			case DESKTOP_MODE:
				batt_temp_compensation_value = data->default_temp_compensation;
				data->slope_select = data->super_low_speed_slope_coefficient;
				break;
			case CHARGING_AND_PLAYING_3000mV:
				batt_temp_compensation_value = data->medium_load_temp_compensation;
				data->slope_select = data->medium_speed_slope_coefficient;
				break;
			case CHARGING_AND_PLAYING_4600mV:
				batt_temp_compensation_value = data->high_load_temp_compensation;
				data->slope_select = data->fast_speed_slope_coefficient;
				break;
			default:
				batt_temp_compensation_value = data->default_temp_compensation;
				data->slope_select = data->default_speed_slope_coefficient;
				break;
			}
			PB_printk("%s power_machine_state = %d\n", __func__, power_machine_state);
		}
	}

compensation:
	data->batt_temp_compensation_value = batt_temp_compensation_value;
	data->batt_ajust_temp = temp - batt_temp_compensation_value;

	return data->batt_ajust_temp;

uncompensation:
	batt_temp_compensation_value = data->default_temp_compensation;
	goto compensation;
}

static int power_board_compensated_battery_temperature(void)
{
	struct power_board_priv *data = g_data;
	int temp;

	temp = power_get_battery_temperature();
	temp = battery_temp_compansetion(data, temp);

	return temp;
}

static int power_board_compensated_battery_slope_select(void)
{
	struct power_board_priv *data = g_data;

	return data->slope_select;
}

static void power_set_cc_chip_only_device(bool enable)
{
	struct power_board_priv *data = g_data;

	if (data->charger_protocol_qti_module && data->charger_protocol_qti_module->set_cc_only_device)
		data->charger_protocol_qti_module->set_cc_only_device(enable);
}

static int power_board_get_charger_cc_state_fiio(void)
{
	struct power_board_priv *data = g_data;
	int cc_pin = 0;

	if (!data->charger_protocol_fiio_module) {
		PB_printk("%s: charger_protocol_fiio_module module not found\n", __func__);
		return -ENODEV;
	}

	if (data->charger_protocol_fiio_module->get_cc_state)
		cc_pin = data->charger_protocol_fiio_module->get_cc_state();
	PB_printk("%s cc_pin = %d\n", __func__, cc_pin);

	return cc_pin;
}

static int power_board_check_power_role_qti(void)
{
	struct power_board_priv *data = g_data;
	int power_role = 0;

	if (!data->charger_protocol_qti_module) {
		PB_printk("%s: charger_protocol_qti_module module not found\n", __func__);
		return -ENODEV;
	}

	if (data->charger_protocol_qti_module->check_power_role_qti)
		power_role = data->charger_protocol_qti_module->check_power_role_qti();
	PB_printk("%s power_role = %d\n", __func__, power_role);

	return power_role;
}

static int power_board_check_vbus_qti(void)
{
	struct power_board_priv *data = g_data;
	int vbus = 0;

	if (!data->charger_protocol_qti_module) {
		PB_printk("%s: charger_protocol_qti_module module not found\n", __func__);
		return -ENODEV;
	}

	if (data->charger_protocol_qti_module->check_vbus_qti)
		vbus = data->charger_protocol_qti_module->check_vbus_qti();
	PB_printk("%s vbus = %d\n", __func__, vbus);

	return vbus;
}

static void power_board_adapter_power_capability(int i, unsigned int *voltage, unsigned int *maxCurrent)
{
	struct power_board_priv *data = g_data;
	unsigned int charger_voltage = 0;
	unsigned int charger_maxCurrent = 0;

	if (!data->charger_protocol_fiio_module) {
		PB_printk("%s: charger_protocol_fiio_module module not found\n", __func__);
		return;
	}

	if (data->charger_protocol_fiio_module->adapter_power_capability)
		data->charger_protocol_fiio_module->adapter_power_capability(i, &charger_voltage,
									      &charger_maxCurrent);
	*voltage = charger_voltage;
	*maxCurrent = charger_maxCurrent;
}

static void power_board_match_pdo(int i)
{
	unsigned int voltage = 0;
	unsigned int maxCurrent = 0;

	power_board_adapter_power_capability(i - 1, &voltage, &maxCurrent);
	PB_printk("%s voltage = %d maxCurrent = %d\n", __func__, voltage, maxCurrent);
	if (voltage > 9) {
		PB_printk("%s  voltage great than 9V\n", __func__);
		return;
	}

	power_board_fix_modulation_power(i, i);
}

static void key_test_work(struct work_struct *work)
{
	struct power_board_priv *data = container_of(work, struct power_board_priv,
						     sw_event_work.work);
	struct desktop_mode_notify_data dc_bat_sw_event;
	int dc_state;

	PB_printk("%s\n", __func__);
	dc_state = gpio_get_value_cansleep(data->gpio_ctrl[DC_BAT_SW]) ? 1 : 0;
	PB_printk("%s dc_state = %d\n", __func__, dc_state);

	mutex_lock(&data->lock);
	fiio_notifier_call_chain(notifier_dmode_irq, dc_state, &dc_bat_sw_event);
	if (dc_state) {
		data->is_desktop_mode = false;
		PB_printk("%s turn off desktop mode\n", __func__);
	} else {
		data->is_desktop_mode = true;
		PB_printk("%s turn on desktop mode\n", __func__);
	}
	mutex_unlock(&data->lock);
}

static void power_board_gpio_exit(struct power_board_priv *data)
{
	int i;

	PB_printk("%s: GPIO_MAX_NUM = %d\n", __func__, GPIO_MAX_NUM);
	for (i = 0; i < GPIO_MAX_NUM; i++) {
		if (gpio_is_valid(data->gpio_ctrl[i]))
			gpio_free(data->gpio_ctrl[i]);
	}
}

static int power_board_gpio_init(struct power_board_priv *data)
{
	struct device *dev = data->dev;
	struct device_node *np = dev->of_node;
	int ret;
	int i;

	PB_printk("%s: GPIO_MAX_NUM = %d\n", __func__, GPIO_MAX_NUM);
	for (i = 0; i < GPIO_MAX_NUM; i++) {
		data->gpio_ctrl[i] = of_get_named_gpio(np, gpiop_name[i], 0);
		PB_printk("[%s] init data->gpio_ctrl[%d] %s %d\n", __func__, i, gpiop_name[i],
			  data->gpio_ctrl[i]);
		if (gpio_is_valid(data->gpio_ctrl[i])) {
			ret = gpio_request(data->gpio_ctrl[i], gpiop_name[i]);
			if (ret < 0) {
				PB_printk("[%s] gpio_request %s failed:%d\n", __func__, gpiop_name[i], ret);
			} else {
				if (i == BATTERY_ID || i == DC_BAT_SW)
					gpio_direction_input(data->gpio_ctrl[i]);
				else
					power_set_gpio_value(data->gpio_ctrl[i], 0);

				PB_printk("[%s] %s: %d\n", __func__, gpiop_name[i],
					  gpio_get_value_cansleep(data->gpio_ctrl[i]));
			}
		} else {
			PB_printk("[%s] of_get_named_gpio Failed :%d name:%s\n", __func__,
				  data->gpio_ctrl[i], gpiop_name[i]);
		}
	}

	return 0;
}

static irqreturn_t dc_bat_sw_irq_handler(int irq, void *dc_bat)
{
	struct power_board_priv *data = dc_bat;

	PB_printk("%s enter\n", __func__);
	mod_delayed_work(system_wq, &data->sw_event_work, msecs_to_jiffies(16));

	return IRQ_HANDLED;
}

static int dc_bat_sw_irq_registrationt(struct power_board_priv *data)
{
	int ret = 0;

	if (gpio_is_valid(data->gpio_ctrl[DC_BAT_SW]))
		ret = request_threaded_irq(gpio_to_irq(data->gpio_ctrl[DC_BAT_SW]), NULL,
					   dc_bat_sw_irq_handler,
					   IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					   "dc_bat_sw", data);

	return ret;
}

static bool power_board_enable_otg(bool enable)
{
	struct power_board_priv *data = g_data;
	int ret = 0;

	if (data->buck_charger_module)
		ret = data->buck_charger_module->specific_buck_otg_enable(enable);

	return ret;
}

static void power_board_charger_vbus_plugin_fiio_handle(struct power_board_priv *data)
{
	data->power_in_vbus = true;

	if (gpio_is_valid(data->gpio_ctrl[BATTERY_ID])) {
		if (gpio_get_value_cansleep(data->gpio_ctrl[BATTERY_ID]) == 0) {
			if (data->usb_host) {
				power_set_gpio_value(data->gpio_ctrl[PD_VBUS_SW], 0);
				power_board_enable_otg(true);
			}
		} else {
			power_set_gpio_value(data->gpio_ctrl[OTG_SEL], 1);
		}
	}
}

static void power_board_fast_charger_fiio_handle(struct power_board_priv *data)
{
	if (gpio_is_valid(data->gpio_ctrl[BATTERY_ID])) {
		if (gpio_get_value_cansleep(data->gpio_ctrl[BATTERY_ID]) == 0) {
			if ((data->usb_device || data->usb_host) && data->pd_voltage_gear == 2) {
				printk("%s USB3.0 connect to the charger or otg device,POWER IN reduce the voltage from 9V to 5V\n",
				       __func__);
				power_board_match_pdo(1);
			}
		} else {
			if (data->usb_device && data->pd_voltage_gear == 2) {
				printk("%s USB3.0 connect to the charger devices and POWER IN reduce the voltage from 9V to 5V\n",
				       __func__);
				power_board_match_pdo(1);
			}
		}
	}
}

static void power_board_charger_vbus_plugout_fiio_handle(struct power_board_priv *data)
{
	data->power_in_vbus = false;

	if (gpio_is_valid(data->gpio_ctrl[BATTERY_ID])) {
		if (gpio_get_value_cansleep(data->gpio_ctrl[BATTERY_ID]) == 0) {
			if (data->usb_host)
				power_board_enable_otg(true);
		} else {
			if (!data->usb_host)
				power_set_gpio_value(data->gpio_ctrl[OTG_SEL], 0);
			if (data->usb_device) {
				power_set_gpio_value(data->gpio_ctrl[PD_VBUS_SW], 1);
				msleep(100);
				power_set_gpio_value(data->gpio_ctrl[PD_VBUS_SW], 0);
			}
		}
	}
}

static void power_board_charger_vbus_plugin_qti_handle(struct power_board_priv *data)
{
	data->usb_device = true;

	if (data->power_in_vbus && data->pd_voltage_gear == 2) {
		printk("%s USB3.0 connect to charger and reduce the voltage from 9V to 5V\n", __func__);
		power_board_match_pdo(1);
	}
}

static void power_board_charger_vbus_plugout_qti_handle(struct power_board_priv *data)
{
	if (gpio_is_valid(data->gpio_ctrl[BATTERY_ID])) {
		if (gpio_get_value_cansleep(data->gpio_ctrl[BATTERY_ID]) == 0) {
			power_set_gpio_value(data->gpio_ctrl[OTG_SEL], 0);
			power_set_gpio_value(data->gpio_ctrl[PD_VBUS_SW], 0);
		} else if (!data->usb_host) {
			if (data->power_in_vbus && data->pd_voltage_gear == 1) {
				printk("%s Unplug the USB charger. If the POWER IN restores the voltage to 9V\n",
				       __func__);
				power_board_match_pdo(2);
			}
		} else if (!data->power_in_vbus) {
			power_set_gpio_value(data->gpio_ctrl[OTG_SEL], 0);
		}
	}

	data->usb_host = false;
	data->usb_device = false;
}

static void power_board_charger_vbus_otg_qti_handle(struct power_board_priv *data)
{
	power_set_gpio_value(data->gpio_ctrl[OTG_SEL], 1);

	if (gpio_is_valid(data->gpio_ctrl[BATTERY_ID])) {
		if (gpio_get_value_cansleep(data->gpio_ctrl[BATTERY_ID]) == 0) {
			if (!data->power_in_vbus) {
				power_set_gpio_value(data->gpio_ctrl[PD_VBUS_SW], 1);
				power_board_enable_otg(true);
			} else {
				power_board_enable_otg(false);
				if (data->pd_voltage_gear == 2) {
					printk("%s USB3.0 connect to the otg device and POWER IN reduce the voltage from 9V to 5V\n",
					       __func__);
					power_board_match_pdo(1);
				}
			}
		}
	}

	data->usb_host = true;
}

static int power_fiio_vbus_notifier(struct notifier_block *nb, unsigned long val,
				    void *priv)
{
	struct power_board_priv *data = container_of(nb, struct power_board_priv, fiio_host_nb);
	int ret = 0;

	switch (val) {
	case 0:
		power_board_charger_vbus_plugin_fiio_handle(data);
		break;
	case 1:
		power_board_charger_vbus_plugout_fiio_handle(data);
		break;
	case 2:
		queue_delayed_work(system_wq, &data->fast_charger_pdo_change_work,
				   msecs_to_jiffies(200));
		break;
	default:
		break;
	}

	return ret;
}

static int power_qti_host_notifier(struct notifier_block *nb, unsigned long val,
				   void *priv)
{
	struct power_board_priv *data = container_of(nb, struct power_board_priv, qti_host_nb);
	int ret = 0;

	switch (val) {
	case 0:
		power_board_charger_vbus_plugin_qti_handle(data);
		break;
	case 1:
		power_board_charger_vbus_plugout_qti_handle(data);
		break;
	case 4:
		power_board_charger_vbus_otg_qti_handle(data);
		break;
	default:
		break;
	}

	return ret;
}

void power_boot_complete_work_func(struct work_struct *work)
{
	struct power_board_priv *data = container_of(work, struct power_board_priv,
						     boot_complete_work.work);
	int power_in_vbus;
	int usb_host;
	int usb_device;

	power_in_vbus = power_board_get_charger_cc_state_fiio();
	usb_host = power_board_check_power_role_qti();
	usb_device = power_board_check_vbus_qti();

	if (power_in_vbus) {
		power_board_charger_vbus_plugin_fiio_handle(data);
		power_board_fast_charger_fiio_handle(data);
	}

	if (!usb_host && usb_device)
		power_board_charger_vbus_plugin_qti_handle(data);

	if (usb_host)
		power_board_charger_vbus_otg_qti_handle(data);
}

void power_fast_charger_pdo_change_work_func(struct work_struct *work)
{
	struct power_board_priv *data = container_of(work, struct power_board_priv,
						     fast_charger_pdo_change_work.work);

	power_board_fast_charger_fiio_handle(data);
}

static int power_parse_dt(struct power_board_priv *data)
{
	struct device_node *np = data->dev->of_node;

	of_property_read_u32(np, "power-battery-only-charging-temp-compensation",
			     &data->only_charging_temp_compensation);
	of_property_read_u32(np, "power-battery-low-load-temp-compensation",
			     &data->low_load_temp_compensation);
	of_property_read_u32(np, "power-battery-medium-load-temp-compensation",
			     &data->medium_load_temp_compensation);
	of_property_read_u32(np, "power-battery-high-load-temp-compensation",
			     &data->high_load_temp_compensation);
	of_property_read_u32(np, "power-battery-default-temp-compensation",
			     &data->default_temp_compensation);
	of_property_read_u32(np, "power-fast-speed-slope-coefficient",
			     &data->fast_speed_slope_coefficient);
	of_property_read_u32(np, "power-medium-speed-slope-coefficient",
			     &data->medium_speed_slope_coefficient);
	of_property_read_u32(np, "power-low-speed-slope-coefficient",
			     &data->low_speed_slope_coefficient);
	of_property_read_u32(np, "power-default-speed-slope-coefficient",
			     &data->default_speed_slope_coefficient);
	of_property_read_u32(np, "power-super_low-speed-slope-coefficient",
			     &data->super_low_speed_slope_coefficient);

	return 0;
}

static int power_enable_vbus(struct regulator_dev *rdev)
{
	struct power_board_priv *data = g_data;
	int ret = 0;

	printk("%s enter\n", __func__);
	power_set_gpio_value(data->gpio_ctrl[OTG_EN], 1);

	return ret;
}

static int power_disable_vbus(struct regulator_dev *rdev)
{
	struct power_board_priv *data = g_data;
	int ret = 0;

	printk("%s enter\n", __func__);
	power_set_gpio_value(data->gpio_ctrl[OTG_EN], 0);

	return ret;
}

static int power_is_enabled_vbus(struct regulator_dev *rdev)
{
	int ret = 0;

	printk("%s enter\n", __func__);

	return ret;
}

static struct regulator_ops power_vbus_ops = {
	.enable = power_enable_vbus,
	.disable = power_disable_vbus,
	.is_enabled = power_is_enabled_vbus,
};

static struct regulator_desc power_otg_rdesc = {
	.of_match = "usb-otg-vbus",
	.name = "usb-otg-vbus",
	.ops = &power_vbus_ops,
	.owner = THIS_MODULE,
	.type = REGULATOR_VOLTAGE,
	.fixed_uV = 5000000,
	.n_voltages = 1,
};

static int power_vbus_regulator_register(struct power_board_priv *data)
{
	struct regulator_config config = {};
	int ret = 0;

	printk("%s enter\n", __func__);
	config.dev = data->dev;
	config.driver_data = data;
	data->otg_rdev = devm_regulator_register(data->dev, &power_otg_rdesc, &config);
	data->otg_rdev->constraints->valid_ops_mask |= REGULATOR_CHANGE_STATUS;
	if (IS_ERR(data->otg_rdev)) {
		ret = PTR_ERR(data->otg_rdev);
		pr_info("%s: register otg regulator failed (%d)\n", __func__, ret);
	}

	return ret;
}

static void power_otg_work_start(struct work_struct *work)
{
	struct delayed_work *otg_work = container_of(work, struct delayed_work, work);
	struct power_board_priv *data;
	int ret;

	if (!otg_work) {
		pr_err("Cann't get charge_detect_delayed_work\n");
		return;
	}

	data = container_of(otg_work, struct power_board_priv, otg_work);
	if (!data) {
		pr_err("Cann't get power_device\n");
		return;
	}

	if (data->otg_enabled) {
		ret = data->otg_rdev->desc->ops->enable(data->otg_rdev);
		if (!ret)
			data->otg_rdev->use_count = 1;
		else
			data->otg_rdev->use_count = 0;
	} else {
		ret = data->otg_rdev->desc->ops->disable(data->otg_rdev);
		if (!ret)
			data->otg_rdev->use_count = 0;
	}
}

static int extcon_notifier_fn(struct notifier_block *nb, unsigned long action, void *value)
{
	struct power_board_priv *data = container_of(nb, struct power_board_priv, extcon_nb);

	if (IS_ERR_OR_NULL(data->extcon_dev)) {
		pr_err("%s: NULL extcon_dev\n", __func__);
		return -ENODEV;
	}

	data->otg_enabled = !!action;
	queue_delayed_work(system_wq, &data->otg_work, msecs_to_jiffies(100));

	return 0;
}

static int power_determine_initial_status(struct power_board_priv *data)
{
	int ret;
	union extcon_property_value prop_val;

	data->extcon_dev = extcon_get_edev_by_phandle(data->dev, 0);
	if (IS_ERR_OR_NULL(data->extcon_dev)) {
		pr_err("get exton soc:qcom,usb_detect failed\n");
		return -ENODEV;
	}

	data->extcon_nb.notifier_call = extcon_notifier_fn;
	ret = devm_extcon_register_notifier(data->dev, data->extcon_dev, EXTCON_USB_HOST,
					    &data->extcon_nb);
	if (ret) {
		pr_err("%s: for EXTCON_USB_HOST failed\n", __func__);
		return ret;
	}

	ret = extcon_get_property(data->extcon_dev, EXTCON_USB_HOST, EXTCON_PROP_USB_SS,
				  &prop_val);
	if (ret) {
		pr_err("extcon_get_property initial failed\n");
		return ret;
	}

	data->otg_enabled = !!prop_val.intval;
	if (data->otg_enabled) {
		ret = data->otg_rdev->desc->ops->enable(data->otg_rdev);
		if (!ret)
			data->otg_rdev->use_count = 1;
		else
			data->otg_rdev->use_count = 0;
	} else {
		ret = data->otg_rdev->desc->ops->disable(data->otg_rdev);
		if (!ret)
			data->otg_rdev->use_count = 0;
	}

	return 0;
}

static void power_board_load_work(void)
{
	struct power_board_priv *data = g_data;

	PB_printk("%s: Enter!\n", __func__);

	data->buck_charger_module = fiio_device_get_data_by_name("buck_charger module");
	if (!data->buck_charger_module)
		PB_printk("get buck_charger module failed!\n");

	data->led_module = fiio_device_get_data_by_name("led module");
	if (!data->led_module)
		PB_printk("get led module failed!\n");

	data->charger_protocol_fiio_module = fiio_device_get_data_by_name("charger_protocol_fiio module");
	if (!data->charger_protocol_fiio_module)
		PB_printk("get charger_protocol_fiio module failed!\n");

	data->anolog_board_module = fiio_device_get_data_by_name("anolog_board module");
	if (!data->anolog_board_module)
		PB_printk("get anolog_board module failed!\n");

	data->fuel_gauge_module = fiio_device_get_data_by_name("fuel_gauge module");
	if (!data->fuel_gauge_module)
		PB_printk("get fuel_gauge_module module failed!\n");

	data->charger_protocol_qti_module = fiio_device_get_data_by_name("charger_protocol_qti module");
	if (!data->charger_protocol_qti_module)
		PB_printk("get charger_protocol_qti module failed!\n");
}

static struct power_board_public_data power_board_d = {
	.buck_charge_enable = power_board_buck_charge_start_charge,
	.buck_charger_set_charge_current = power_board_buck_charger_set_charge_current,
	.buck_charger_set_limit_current = power_board_buck_charger_set_limit_current,
	.set_charge_status_indicator = power_board_set_charge_status_indicator,
	.get_charge_status_indicator = power_board_get_charge_status_indicator,
	.fix_modulation_power = power_board_fix_modulation_power,
	.check_desktop_mode = power_board_check_desktop_mode,
	.get_adapter_power_capability = power_board_adapter_power_capability,
	.compensated_battery_temperature = power_board_compensated_battery_temperature,
	.slope_limit_value = power_board_compensated_battery_slope_select,
	.get_charger_cc_state_fiio = power_board_get_charger_cc_state_fiio,
	.get_charger_power_role_qti = power_board_check_power_role_qti,
	.get_charger_vbus_qti = power_board_check_vbus_qti,
	.set_uac_charging_disabled = power_board_uac_charging_disabled,
	.set_cc_chip_only_device = power_set_cc_chip_only_device,
};

static struct fiio_device_driver power_board_device_driver = {
	.name = "power_board module",
	.private_data = &power_board_d,
	.load_work = power_board_load_work,
};

static int power_board_platform_probe(struct platform_device *pdev)
{
	struct power_board_priv *data;
	struct device *dev = &pdev->dev;
	int ret;

#line 1119
	PB_printk("%s(%d)\n", __func__, __LINE__);

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto ERROR;
	}

	data->dev = dev;
	g_data = data;

	power_board_gpio_init(data);

	ret = dc_bat_sw_irq_registrationt(data);
	if (ret) {
		PB_printk("%s,request irq failed\n", __func__);
		goto REQUEST_FAILED;
	}

	INIT_DELAYED_WORK(&data->sw_event_work, key_test_work);

	data->power_board_public = &power_board_d;
	fiio_device_register(&power_board_device_driver);

	INIT_DELAYED_WORK(&data->power_get_module_work, power_get_module_work_func);
	queue_delayed_work(system_wq, &data->power_get_module_work, msecs_to_jiffies(4000));

	INIT_DELAYED_WORK(&data->boot_complete_work, power_boot_complete_work_func);
	queue_delayed_work(system_wq, &data->boot_complete_work, msecs_to_jiffies(15000));

	INIT_DELAYED_WORK(&data->fast_charger_pdo_change_work, power_fast_charger_pdo_change_work_func);

	data->fiio_host_nb.notifier_call = power_fiio_vbus_notifier;
	fiio_register_client(notifier_vbus_detection_fiio, &data->fiio_host_nb);
	data->qti_host_nb.notifier_call = power_qti_host_notifier;
	fiio_register_client(notifier_vbus_detection_qti, &data->qti_host_nb);

	data->power_in_vbus = false;
	data->usb_host = false;
	data->is_desktop_mode = false;
	data->is_charging = false;
	data->update_temp_count = 0;
	data->power_machine_state = OTHER_STATE;

	ret = power_vbus_regulator_register(data);
	if (ret < 0)
		dev_err(data->dev, "Failed to register otg\n");

	ret = power_determine_initial_status(data);
	if (ret)
		dev_err(data->dev, "sgm41510_determine_initial_status failed\n");

	power_parse_dt(data);

	INIT_DELAYED_WORK(&data->otg_work, power_otg_work_start);

#line 1184
	PB_printk("%s(%d) probe finished\n", __func__, __LINE__);
	return 0;

REQUEST_FAILED:
	gpio_free(data->gpio_ctrl[DC_BAT_SW]);
	cancel_delayed_work_sync(&data->sw_event_work);
	cancel_delayed_work_sync(&data->otg_work);
ERROR:
	return ret;
}

static int power_board_platform_remove(struct platform_device *pdev)
{
	struct power_board_priv *data = platform_get_drvdata(pdev);

	fiio_device_unregister(&power_board_device_driver);
	cancel_delayed_work_sync(&data->power_get_module_work);

	return 0;
}

static const struct of_device_id power_board_dt_match[] = {
	{ .compatible = "fiio,power_board" },
	{ },
};
MODULE_DEVICE_TABLE(of, power_board_dt_match);

static struct platform_driver power_board_driver = {
	.probe = power_board_platform_probe,
	.remove = power_board_platform_remove,
	.driver = {
		.name = "power_board",
		.owner = THIS_MODULE,
		.of_match_table = power_board_dt_match,
	},
};

static int __init power_board_init(void)
{
	int ret;

	PB_printk("[%s]", __func__);
	ret = platform_driver_register(&power_board_driver);
	if (ret)
#line 1229
		PB_printk("%s(%d) unable to register platform device!\n", __func__, __LINE__);

	return ret;
}

static void __exit power_board_exit(void)
{
	platform_driver_unregister(&power_board_driver);
	power_board_gpio_exit(g_data);
}

module_init(power_board_init);
module_exit(power_board_exit);

MODULE_AUTHOR("Aiden@fiio.net");
MODULE_DESCRIPTION("power board");
MODULE_LICENSE("GPL v2");
