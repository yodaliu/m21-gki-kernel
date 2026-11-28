// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO charger manager.
 *
 * Reconstructed from the stock FiiO M21 module. Derived from the upstream
 * drivers/power/supply/charger-manager.c, with FiiO battery temperature
 * states, PD/PPS pump charging and desktop mode handling added.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/io.h>
#include <linux/module.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/sysfs.h>
#include <linux/of.h>
#include <linux/thermal.h>
#include <linux/pm_wakeup.h>
#include <linux/string.h>

#include <fiio/fiio_device.h>
#include <fiio/fiio_attr.h>
#include <fiio/fiio_notify.h>
#include <fiio/fiio_utils.h>
#include <fiio/led_common.h>
#include <fiio/power_board_common.h>
#include <fiio/charger_protocol_common.h>
#include <fiio/charge_chip_common.h>
#include <fiio/fuel_gauge_common.h>
#include <fiio/fiio_charger_manager_common.h>
#include "fiio-charger-manager.h"

#define fcm_printk(fmt, arg...) printk(KERN_ERR "[charger-manager]: " fmt, ##arg)

/*
 * Regard CM_JIFFIES_SMALL jiffies is small enough to ignore for
 * delayed works so that we can run delayed works with CM_JIFFIES_SMALL
 * without any delays.
 */
#define	FCM_JIFFIES_SMALL	(2)

/*
 * Regard FCM_RTC_SMALL (sec) is small enough to ignore error in invoking
 * rtc alarm.
 */
#define	FCM_RTC_SMALL		(2)

#define UEVENT_BUF_SIZE		32

#define NUM_CHARGER_PSY_OPTIONAL	(4)

struct fiio_charger_manager *g_fcm;

static const char * const default_event_names[] = {
	[FCM_EVENT_UNKNOWN] = "Unknown",
	[FCM_EVENT_BATT_FULL] = "Battery Full",
	[FCM_EVENT_BATT_IN] = "Battery Inserted",
	[FCM_EVENT_BATT_OUT] = "Battery Pulled Out",
	[FCM_EVENT_BATT_OVERHEAT] = "Battery Overheat",
	[FCM_EVENT_BATT_COLD] = "Battery Cold",
	[FCM_EVENT_EXT_PWR_IN_OUT] = "External Power Attach/Detach",
	[FCM_EVENT_CHG_START_STOP] = "Charging Start/Stop",
	[FCM_EVENT_OTHERS] = "Other battery events"
};

static LIST_HEAD(fcm_list);
static DEFINE_MUTEX(fcm_list_mtx);

static struct alarm *fcm_timer;

/* About normal (not suspended) monitoring */
static unsigned long polling_jiffy = ULONG_MAX; /* ULONG_MAX: no polling */
static unsigned long next_polling; /* Next appointed polling time */
static struct workqueue_struct *fcm_wq; /* init at driver add */
static struct delayed_work fcm_monitor_work; /* init at driver add */

static bool is_batt_present(struct fiio_charger_manager *fcm)
{
	union power_supply_propval val;
	struct power_supply *psy;
	bool present = false;
	int i, ret;

	switch (fcm->desc->battery_present) {
	case FCM_BATTERY_PRESENT:
		present = true;
		break;
	case FCM_NO_BATTERY:
		break;
	case FCM_FUEL_GAUGE:
		psy = power_supply_get_by_name(fcm->desc->psy_fuel_gauge);
		if (!psy)
			break;

		ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_PRESENT,
				&val);
		if (ret == 0 && val.intval)
			present = true;
		break;
	case FCM_CHARGER_STAT:
		for (i = 0; fcm->desc->psy_charger_stat[i]; i++) {
			psy = power_supply_get_by_name(
					fcm->desc->psy_charger_stat[i]);
			if (!psy)
				continue;

			ret = power_supply_get_property(psy,
				POWER_SUPPLY_PROP_PRESENT, &val);
			if (ret == 0 && val.intval) {
				present = true;
				break;
			}
		}
		break;
	}

	return present;
}

static bool check_plugined_state_change(struct fiio_charger_manager *fcm)
{
	struct charger_desc *desc = fcm->desc;
	static bool last_plugined_state;

	bool current_plugined = desc->qti_device || desc->vbus_plugin_fiio;
	bool state_changed = (current_plugined != last_plugined_state);

	if (state_changed)
		last_plugined_state = current_plugined;

	return state_changed;
}

static void report_usb_plug_event(struct fiio_charger_manager *fcm, bool plug)
{
	char buf1[40] = {0};

	if (check_plugined_state_change(fcm)) {
		fcm_printk("%s report usb plug status:%d\n", __func__, plug);
		sprintf(buf1, "USB_PLUG=%s", plug ? "ON" : "OFF");
		fiio_sendMessage(fcm->dev, buf1, NULL);
	}
}

static bool is_ext_pwr_online(struct fiio_charger_manager *fcm)
{
	union power_supply_propval val;
	struct power_supply *psy;
	bool online = false;
	int i, ret;

	/* If at least one of them has one, it's yes. */
	for (i = 0; fcm->desc->psy_charger_stat[i]; i++) {
		psy = power_supply_get_by_name(fcm->desc->psy_charger_stat[i]);
		if (!psy)
			continue;

		ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE,
				&val);
		if (ret == 0 && val.intval) {
			online = true;
			break;
		}
	}

	return online;
}

static int get_batt_uV(struct fiio_charger_manager *fcm, int *uV)
{
	union power_supply_propval val;
	struct power_supply *fuel_gauge;
	int ret;

	fuel_gauge = power_supply_get_by_name(fcm->desc->psy_fuel_gauge);
	if (!fuel_gauge)
		return -ENODEV;

	ret = power_supply_get_property(fuel_gauge,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
	if (ret)
		return ret;

	*uV = val.intval;
	return 0;
}

static int get_batt_uA(struct fiio_charger_manager *fcm, int *uA)
{
	union power_supply_propval val;
	struct power_supply *fuel_gauge;
	int ret;

	fuel_gauge = power_supply_get_by_name(fcm->desc->psy_fuel_gauge);
	if (!fuel_gauge)
		return -ENODEV;

	ret = power_supply_get_property(fuel_gauge,
				POWER_SUPPLY_PROP_CURRENT_NOW, &val);
	if (ret)
		return ret;

	*uA = val.intval;
	return 0;
}

static int get_batt_cap(struct fiio_charger_manager *fcm, int *cap)
{
	union power_supply_propval val;
	struct power_supply *fuel_gauge;
	int ret;

	fuel_gauge = power_supply_get_by_name(fcm->desc->psy_fuel_gauge);
	if (!fuel_gauge)
		return -ENODEV;

	ret = power_supply_get_property(fuel_gauge,
				POWER_SUPPLY_PROP_CAPACITY, &val);
	if (ret)
		return ret;

	*cap = fcm->desc->force_cap < 200 ? fcm->desc->force_cap : val.intval;
	return 0;
}

static bool is_charging(struct fiio_charger_manager *fcm)
{
	int i, ret;
	bool charging = false;
	struct power_supply *psy;
	union power_supply_propval val;

	/* If there is no battery, it cannot be charged */
	if (!is_batt_present(fcm)) {
		fcm_printk("%s no battery\n", __func__);
		return false;
	}

	/* If at least one of the charger is charging, return yes */
	for (i = 0; fcm->desc->psy_charger_stat[i]; i++) {
		psy = power_supply_get_by_name(fcm->desc->psy_charger_stat[i]);
		if (!psy)
			continue;

		ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_STATUS,
				&val);
		if (ret) {
			dev_warn(fcm->dev, "Cannot read STATUS value from %s\n",
				 fcm->desc->psy_charger_stat[i]);
			continue;
		}
		if (val.intval == POWER_SUPPLY_STATUS_FULL ||
				val.intval == POWER_SUPPLY_STATUS_DISCHARGING ||
				val.intval == POWER_SUPPLY_STATUS_NOT_CHARGING ||
				val.intval == POWER_SUPPLY_STATUS_UNKNOWN)
			continue;

		/* Then, this is charging. */
		charging = true;
		break;
	}

	return charging;
}

static bool is_full_charged(struct fiio_charger_manager *fcm)
{
	struct charger_desc *desc = fcm->desc;
	union power_supply_propval val;
	struct power_supply *fuel_gauge;
	struct power_supply *charger_chip;
	bool is_fullbatt_uV = false;
	bool is_fullbatt_uA = false;
	bool is_fullbatt_cap = false;
	int ret = 0;
	int uV, uA;

	/* If there is no battery, it cannot be charged */
	if (!is_batt_present(fcm)) {
		fcm_printk("%s battery not present\n", __func__);
		return false;
	}

	fuel_gauge = power_supply_get_by_name(fcm->desc->psy_fuel_gauge);
	if (!fuel_gauge) {
		fcm_printk("%s can't find fuel_gauge\n", __func__);
		return false;
	}

	charger_chip = power_supply_get_by_name(fcm->desc->psy_charger_stat[0]);
	if (!charger_chip) {
		fcm_printk("%s can't find charger_chip\n", __func__);
		return false;
	}

	ret = power_supply_get_property(charger_chip, POWER_SUPPLY_PROP_STATUS,
			&val);
	if (!ret && val.intval == POWER_SUPPLY_STATUS_FULL) {
		if (!desc->is_full)
			fcm_printk("%s Hardware judgment is full\n", __func__);
		desc->is_full = true;
		goto out;
	}

	/* Full, if the capacity is more than fullbatt_full_capacity */
	if (desc->fullbatt_full_capacity > 0) {
		val.intval = 0;

		/* Not full if capacity of fuel gauge isn't full */
		ret = power_supply_get_property(fuel_gauge,
				POWER_SUPPLY_PROP_CAPACITY, &val);
		if (!ret && val.intval >= desc->fullbatt_full_capacity)
			is_fullbatt_cap = true;
	}

	/* Full, if it's over the fullbatt voltage */
	if (desc->fullbatt_uV > 0) {
		ret = get_batt_uV(fcm, &uV);
		if (!ret && uV >= desc->fullbatt_uV)
			is_fullbatt_uV = true;
	}

	/* Full, if the charge current is below fullbatt current */
	if (desc->fullbatt_uA > 0) {
		ret = get_batt_uA(fcm, &uA);
		if (!ret) {
			is_fullbatt_uA = uA <= desc->fullbatt_uA && uA >= 0;
			if (is_fullbatt_uA && is_fullbatt_uV && is_fullbatt_cap &&
			    is_charging(fcm)) {
				if (!desc->is_full)
					fcm_printk("%s ichg %d batt_voltage %d batt_capacity %d Software judgment is full\n",
						   __func__, uA, uV, val.intval);
				desc->is_full = true;
			}
		}
	}

out:
	return desc->is_full;
}

static bool is_polling_required(struct fiio_charger_manager *fcm)
{
	switch (fcm->desc->polling_mode) {
	case FCM_POLL_DISABLE:
		return false;
	case FCM_POLL_ALWAYS:
		return true;
	case FCM_POLL_EXTERNAL_POWER_ONLY:
		return is_ext_pwr_online(fcm);
	case FCM_POLL_CHARGING_ONLY:
		return is_charging(fcm);
	default:
		dev_warn(fcm->dev, "Incorrect polling_mode (%d)\n",
			 fcm->desc->polling_mode);
	}

	return false;
}

static enum fcm_bat_temp_state_levels
fcm_bat_temp_state_update(struct fiio_charger_manager *fcm)
{
	enum fcm_bat_temp_state_levels fcm_bat_temp_state;
	struct charger_desc *desc = fcm->desc;
	int temp = desc->bat_temp;
	int temp_low_extreme = -desc->temp_low_extreme;
	int ichrg_limit = 3000000;

	if (temp < temp_low_extreme) {
		fcm_bat_temp_state = FCM_BAT_TEMP_SUPER_COOL;
	} else if (temp < desc->temp_low_stop) {
		fcm_bat_temp_state = FCM_BAT_TEMP_COLD;
	} else if (temp < desc->temp_low_cold_limit) {
		fcm_bat_temp_state = FCM_BAT_TEMP_COOLER;
		desc->ichrg_current = desc->ichg_cold;
		desc->sink_tar_cur = desc->ichg_cold / 1000;
	} else if (temp < desc->temp_low_cool_limit) {
		fcm_bat_temp_state = FCM_BAT_TEMP_COOL;
		desc->ichrg_current = desc->ichg_cool;
		desc->sink_tar_cur = desc->ichg_cold / 1000;
	} else if (temp <= desc->temp_normal) {
		fcm_bat_temp_state = FCM_BAT_TEMP_NORMAL;
		desc->ichrg_current = desc->ichg_normal;
		desc->sink_tar_cur = desc->sink_tar_max_cur;
		if (temp > desc->pump_normal_temp_control) {
			desc->sink_tar_cur = desc->sink_tar_max_cur -
				(temp - desc->pump_normal_temp_control) * 60;
			fcm_printk("reduce the target charge current: %d bat_temp: %d\n",
				   desc->sink_req_cur, temp);
		}
	} else if (temp <= desc->temp_hight_limit) {
		fcm_bat_temp_state = FCM_BAT_TEMP_WARM;
		desc->ichrg_current = 3000000 /
			(desc->temp_hight_limit - desc->temp_normal) *
			(desc->temp_normal - temp) + 3000000;
		desc->sink_tar_cur = desc->ichg_warm / 1000;
	} else if (temp <= desc->temp_hight_stop) {
		fcm_bat_temp_state = FCM_BAT_TEMP_HOT;
		desc->ichrg_current = desc->ichg_hot;
		desc->sink_tar_cur = desc->ichg_hot / 1000;
	} else {
		fcm_bat_temp_state = FCM_BAT_TEMP_SUPER_HEAT;
	}

	fcm_printk("%s ichrg_limit = %d ichrg_current = %d", __func__,
		   ichrg_limit, desc->ichrg_current);

	return fcm_bat_temp_state;
}

static int fiio_set_limit_cur_and_charge_cur(struct fiio_charger_manager *fcm)
{
	struct charger_desc *desc = fcm->desc;
	int err = 0;

	if (desc->pd_type) {
		if (desc->fcm_bat_temp_state != FCM_BAT_TEMP_NORMAL) {
			desc->fcm_bat_temp_state = fcm_bat_temp_state_update(fcm);
		} else {
			desc->ichrg_current = desc->ichg_fast;
			fcm_printk("%s ichg_fast = %d\n", __func__, desc->ichg_fast);
		}
		desc->ichrg_limit = 3000000;
	} else {
		if (desc->fcm_bat_temp_state != FCM_BAT_TEMP_NORMAL)
			desc->fcm_bat_temp_state = fcm_bat_temp_state_update(fcm);
		else
			desc->ichrg_current = desc->ichg_unknow;
		desc->ichrg_limit = desc->ichg_unknow < 3000000 ?
				    desc->ichg_unknow : 3000000;
	}

	fcm_printk("%s ichrg_limit = %d ichrg_current = %d\n", __func__,
		   desc->ichrg_limit, desc->ichrg_current);

	if ((desc->vbus_plugin_fiio || desc->qti_device) && fcm->power_data) {
		if (fcm->power_data->buck_charger_set_limit_current) {
			err = fcm->power_data->buck_charger_set_limit_current(desc->ichrg_limit);
			if (err < 0) {
				fcm_printk("%s:failed to set buck limit current\n", __func__);
				return err;
			}
		}
		if (fcm->power_data->buck_charger_set_charge_current) {
			err = fcm->power_data->buck_charger_set_charge_current(desc->ichrg_current);
			if (err < 0) {
				fcm_printk("%s:failed to set buck charge current\n", __func__);
				return err;
			}
		}
		if (fcm->uac_disable_chg && desc->qti_device &&
		    fcm->power_data->set_uac_charging_disabled) {
			err = fcm->power_data->set_uac_charging_disabled(true);
			if (err < 0) {
				fcm_printk("%s:failed to set uac charging disabled\n", __func__);
				return err;
			}
		}
	}

	return 0;
}

static int charger_power_pd_fix_gear_handle(struct fiio_charger_manager *fcm)
{
	struct charger_desc *desc = fcm->desc;
	struct charger_pdo pdo_caps_received[20] = {0};
	int valtage[20] = {0};
	int max_cur[20] = {0};
	bool pdo_support = false;
	int index = 0;
	int i;

	if (desc->pd_type && fcm->power_data &&
	    fcm->power_data->get_adapter_power_capability) {
		for (i = 1; ; i++) {
			fcm->power_data->get_adapter_power_capability(i - 1, &valtage[i - 1],
								      &max_cur[i - 1]);
			if (valtage[i - 1] <= 0)
				break;
			pdo_caps_received[i - 1].pdo_voltage = valtage[i - 1];
			pdo_caps_received[i - 1].pdo_current = max_cur[i - 1];
			fcm_printk("%s Fixed index :%d  %dV, %dmA\n", __func__, i,
				   pdo_caps_received[i - 1].pdo_voltage,
				   pdo_caps_received[i - 1].pdo_current);
		}
		fcm_printk("%s Invalid pdo, exit the loop\n", __func__);

		for (index = i - 1; index > 0; index--) {
			fcm_printk("%s pdo_voltage = %d pdo_current = %d\n", __func__,
				   pdo_caps_received[index - 1].pdo_voltage,
				   pdo_caps_received[index - 1].pdo_current);
			if (pdo_caps_received[index - 1].pdo_voltage <= desc->fix_expect_the_power_gear) {
				desc->fix_num = index;
				desc->fix_ichg = pdo_caps_received[index - 1].pdo_current;
				if (fcm->power_data->fix_modulation_power) {
					pdo_support = fcm->power_data->fix_modulation_power(desc->fix_num,
											   desc->fix_ichg);
					fcm_printk("%s fix_num = %d fix_ichg = %d", __func__,
						   desc->fix_num, desc->fix_ichg);
					fcm_printk("%s pdo_support = %d ", __func__, pdo_support);
				}
				break;
			}
			fcm_printk("%s The current voltage %dV is higher than the requested valtage %dV\n",
				   __func__, valtage[index - 1],
				   desc->fix_expect_the_power_gear);
		}
	}

	return 0;
}

static int try_charger_enable(struct fiio_charger_manager *fcm, bool enable)
{
	struct charger_desc *desc = fcm->desc;
	int err = 0;

	/* Ignore if it's redundant command */
	if (enable == fcm->charger_enabled)
		return 0;

	err = fiio_set_limit_cur_and_charge_cur(fcm);
	if (err < 0)
		return err;

	if (enable) {
		if (fcm->emergency_stop)
			return -EAGAIN;

		/*
		 * Save start time of charging to limit
		 * maximum possible charging time.
		 */
		fcm->charging_start_time = ktime_to_ms(ktime_get());
		fcm->charging_end_time = 0;

		fcm_printk("%s buck_charge_enable.", __func__);
		if (fcm->power_data) {
			if (fcm->power_data->buck_charge_enable)
				err = fcm->power_data->buck_charge_enable(true);
			if (fcm->power_data && fcm->power_data->get_charge_status_indicator)
				desc->before_charging_color =
					fcm->power_data->get_charge_status_indicator();
			if (fcm->power_data && fcm->power_data->set_charge_status_indicator)
				fcm->power_data->set_charge_status_indicator(RED);
		}
	} else {
		/*
		 * Save end time of charging to maintain fully charged state
		 * of battery after full-batt.
		 */
		fcm->charging_start_time = 0;
		fcm->charging_end_time = ktime_to_ms(ktime_get());

		fcm_printk("%s buck_charge_disable.", __func__);
		if (fcm->power_data) {
			if (fcm->power_data->buck_charge_enable)
				err = fcm->power_data->buck_charge_enable(false);
			if (fcm->power_data && fcm->power_data->get_charge_status_indicator)
				desc->after_charging_color =
					fcm->power_data->get_charge_status_indicator();
			if (fcm->power_data && fcm->power_data->set_charge_status_indicator) {
				if (desc->after_charging_color == RED) {
					if (desc->before_charging_color == RED)
						fcm->power_data->set_charge_status_indicator(BLUE);
					else
						fcm->power_data->set_charge_status_indicator(desc->before_charging_color);
				} else {
					fcm->power_data->set_charge_status_indicator(desc->after_charging_color);
				}
			}
		}
		fcm_printk("%s unplug the charger!\n", __func__);
	}

	if (!err)
		fcm->charger_enabled = enable;

	power_supply_changed(fcm->charger_psy);

	return err;
}

static int try_charger_restart(struct fiio_charger_manager *fcm)
{
	int err;

	if (fcm->emergency_stop)
		return -EAGAIN;

	err = try_charger_enable(fcm, false);
	if (err)
		return err;

	return try_charger_enable(fcm, true);
}

static void uevent_notify(struct fiio_charger_manager *fcm, const char *event)
{
	static char env_str[UEVENT_BUF_SIZE + 1] = "";
	static char env_str_save[UEVENT_BUF_SIZE + 1] = "";

	if (event == NULL) {
		/* No messages pending */
		if (!env_str_save[0])
			return;

		strncpy(env_str, env_str_save, UEVENT_BUF_SIZE);
		kobject_uevent(&fcm->dev->kobj, KOBJ_CHANGE);
		env_str_save[0] = 0;

		return;
	}

	/* status not changed */
	if (!strncmp(env_str, event, UEVENT_BUF_SIZE))
		return;

	/* save the status and notify the update */
	strncpy(env_str, event, UEVENT_BUF_SIZE);
	kobject_uevent(&fcm->dev->kobj, KOBJ_CHANGE);

	dev_info(fcm->dev, "%s\n", event);
}

static void fullbatt_vchk(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct fiio_charger_manager *fcm = container_of(dwork,
			struct fiio_charger_manager, fullbatt_vchk_work);
	struct charger_desc *desc = fcm->desc;
	int batt_uV, err, diff;

	/* remove the appointment for fullbatt_vchk */
	fcm->fullbatt_vchk_jiffies_at = 0;

	if (!desc->fullbatt_vchkdrop_uV || !desc->fullbatt_vchkdrop_ms)
		return;

	err = get_batt_uV(fcm, &batt_uV);
	if (err) {
		dev_err(fcm->dev, "%s: get_batt_uV error(%d)\n", __func__, err);
		return;
	}

	diff = desc->fullbatt_uV - batt_uV;
	if (diff < 0)
		return;

	if (diff > desc->fullbatt_vchkdrop_uV) {
		try_charger_restart(fcm);
		desc->is_full = false;
		dev_info(fcm->dev, "VBATT dropped %duV after full-batt,greater than vchkdrop %d\n",
			 diff, desc->fullbatt_vchkdrop_uV);
		uevent_notify(fcm, "Recharging");
	}
}

static int check_charging_duration(struct fiio_charger_manager *fcm)
{
	struct charger_desc *desc = fcm->desc;
	u64 curr = ktime_to_ms(ktime_get());
	u64 duration;
	int ret = false;

	if (!desc->charging_max_duration_ms &&
			!desc->discharging_max_duration_ms)
		return ret;

	if (fcm->charger_enabled) {
		duration = curr - fcm->charging_start_time;

		if (duration > desc->charging_max_duration_ms) {
			dev_info(fcm->dev, "Charging duration exceed %ums\n",
				 desc->charging_max_duration_ms);
			uevent_notify(fcm, "Discharging");
			try_charger_enable(fcm, false);
			ret = true;
		}
	} else if (is_ext_pwr_online(fcm) && !fcm->charger_enabled) {
		duration = curr - fcm->charging_end_time;

		if (duration > desc->discharging_max_duration_ms &&
				is_ext_pwr_online(fcm)) {
			dev_info(fcm->dev, "Discharging duration exceed %ums\n",
				 desc->discharging_max_duration_ms);
			uevent_notify(fcm, "Recharging");
			try_charger_enable(fcm, true);
			ret = true;
		}
	}

	return ret;
}

static int fcm_temp_adjustment(struct fiio_charger_manager *fcm, int temp)
{
	struct charger_desc *desc = fcm->desc;
	static int temp_adjustment = 6000;
	int temp_now = temp * 10;
	int temperature_slope_1s = 0;

	if (fcm->power_data && fcm->power_data->slope_limit_value)
		temperature_slope_1s = fcm->power_data->slope_limit_value();

	temp_adjustment += temperature_slope_1s;
	if (temp_adjustment >= temp_now)
		temp_adjustment = temp_now;

	desc->temp_adjustment = temp_adjustment;

	return temp_adjustment / 10;
}

static int fcm_get_battery_temperature(struct fiio_charger_manager *fcm,
					int *temp)
{
	int ret = 0;
	int temp_compensation;
	struct charger_desc *desc = fcm->desc;

	if (!desc->measure_battery_temp)
		return -ENODEV;

	if (fcm->tzd_batt) {
		ret = thermal_zone_get_temp(fcm->tzd_batt, temp);
		if (!ret)
			/* Calibrate temperature unit */
			*temp /= 100;
	} else if (!strcmp(desc->temp_compensation_enable, "true") &&
		   fcm->power_data) {
		if (fcm->power_data->compensated_battery_temperature) {
			temp_compensation = fcm->power_data->compensated_battery_temperature();
			*temp = fcm_temp_adjustment(fcm, temp_compensation);
		}
	} else {
		struct power_supply *fuel_gauge;

		fuel_gauge = power_supply_get_by_name(desc->psy_fuel_gauge);
		if (!fuel_gauge)
			ret = -ENODEV;
		else
			ret = power_supply_get_property(fuel_gauge,
					POWER_SUPPLY_PROP_TEMP,
					(union power_supply_propval *)temp);
	}

	if (desc->force_temp_en)
		*temp = desc->force_temp;

	return ret;
}

static void fcm_bat_temp_state_change(struct fiio_charger_manager *fcm)
{
	struct charger_desc *desc = fcm->desc;
	enum fcm_bat_temp_state_levels fcm_bat_temp_state;
	int temp = desc->bat_temp;
	int temp_low_extreme = -desc->temp_low_extreme;
	static int temp_old = -1000;
	static int temp_confirm_count;
	static int temp_diff_warm;
	bool temp_confirm_en = true;

	desc->fcm_bat_temp_state_changed = false;

	if (temp > 2000 || temp < -2000) {
		fcm_printk("%s temp error, temp =  %d\n", __func__, temp);
		return;
	}

	if (temp_confirm_en && temp_old != -1000 &&
	    ((temp - temp_old) > 100 || (temp - temp_old) < -100)) {
		temp_confirm_count++;
		if (temp_confirm_count < 7) {
			fcm_printk("%s there is a temp saltus step, temp_old = %d, temp = %d, temp_confirm_count = %d\n",
				   __func__, temp_old, temp, temp_confirm_count);
			return;
		}
	}
	temp_old = temp;
	temp_confirm_count = 0;

	switch (desc->fcm_bat_temp_state) {
	case FCM_BAT_TEMP_SUPER_COOL:
		if (temp > temp_low_extreme + desc->temp_thrd)
			desc->fcm_bat_temp_state_changed = true;
		break;
	case FCM_BAT_TEMP_COLD:
		if (temp > desc->temp_low_stop + desc->temp_thrd ||
		    temp < temp_low_extreme - desc->temp_thrd)
			desc->fcm_bat_temp_state_changed = true;
		break;
	case FCM_BAT_TEMP_COOLER:
		desc->sink_tar_cur = desc->ichg_cold / 1000;
		if (temp < desc->temp_low_stop - desc->temp_thrd ||
		    temp > desc->temp_low_cold_limit + desc->temp_thrd)
			desc->fcm_bat_temp_state_changed = true;
		break;
	case FCM_BAT_TEMP_COOL:
		if (temp > desc->temp_low_cool_limit + desc->temp_thrd ||
		    temp < desc->temp_low_cold_limit - desc->temp_thrd)
			desc->fcm_bat_temp_state_changed = true;
		break;
	case FCM_BAT_TEMP_NORMAL:
		if (temp < desc->temp_low_cool_limit - desc->temp_thrd ||
		    temp > desc->temp_normal + desc->temp_thrd)
			desc->fcm_bat_temp_state_changed = true;
		desc->sink_tar_cur = desc->sink_tar_max_cur;
		if (temp > desc->pump_normal_temp_control)
			desc->sink_tar_cur = desc->sink_tar_max_cur -
				(temp - desc->pump_normal_temp_control) * 60;
		break;
	case FCM_BAT_TEMP_WARM:
		desc->sink_tar_cur = desc->ichg_warm / 1000;
		desc->ichrg_current = 3000000 /
			(desc->temp_hight_limit - desc->temp_normal) *
			(desc->temp_normal - temp) + 3000000;
		if (temp < desc->temp_normal - desc->temp_thrd ||
		    temp > desc->temp_hight_limit + desc->temp_thrd)
			desc->fcm_bat_temp_state_changed = true;
		if (temp_diff_warm != temp - desc->temp_normal) {
			temp_diff_warm = temp - desc->temp_normal;
			fiio_set_limit_cur_and_charge_cur(fcm);
		}
		break;
	case FCM_BAT_TEMP_HOT:
		desc->sink_tar_cur = desc->ichg_hot / 1000;
		if (temp < desc->temp_hight_limit - desc->temp_thrd ||
		    temp > desc->temp_hight_stop + desc->temp_thrd)
			desc->fcm_bat_temp_state_changed = true;
		break;
	case FCM_BAT_TEMP_SUPER_HEAT:
		if (temp < desc->temp_hight_stop - desc->temp_thrd)
			desc->fcm_bat_temp_state_changed = true;
		break;
	}

	if (desc->fcm_bat_temp_state_changed) {
		fcm_bat_temp_state = desc->fcm_bat_temp_state;
		desc->fcm_bat_temp_state = fcm_bat_temp_state_update(fcm);
		fiio_set_limit_cur_and_charge_cur(fcm);
		fcm_printk("%s old temp_state = %d  current temp_state = %d.", __func__,
			   fcm_bat_temp_state, desc->fcm_bat_temp_state);
	}
}

static int fcm_check_thermal_status(struct fiio_charger_manager *fcm)
{
	struct charger_desc *desc = fcm->desc;
	int temp, upper_stop, lower_stop, uppest_stop;
	int ret = 0;

	ret = fcm_get_battery_temperature(fcm, &temp);
	if (ret)
		return 0;

	upper_stop = desc->temp_hight_limit;
	uppest_stop = desc->temp_hight_stop;
	lower_stop = desc->temp_low_stop;
	desc->bat_temp = temp;

	if (fcm->emergency_stop) {
		upper_stop -= desc->temp_diff;
		lower_stop += desc->temp_diff;
	}

	if (temp >= uppest_stop) {
		if (desc->fcm_bat_temp_state != FCM_BAT_TEMP_SUPER_HEAT) {
			try_charger_enable(fcm, false);
			fcm_printk("%s Temperatures overheat!!!.", __func__);
		}
		desc->fcm_bat_temp_state = FCM_BAT_TEMP_SUPER_HEAT;
		ret = FCM_EVENT_BATT_OVERHEAT;
	} else if (temp >= upper_stop) {
		if (desc->fcm_bat_temp_state != FCM_BAT_TEMP_HOT) {
			try_charger_enable(fcm, false);
			fcm_printk("%s Temperatures heat!!!.", __func__);
		}
		desc->fcm_bat_temp_state = FCM_BAT_TEMP_HOT;
		ret = FCM_EVENT_BATT_OVERHEAT;
	} else if (temp <= lower_stop && temp > -desc->temp_low_extreme) {
		if (desc->fcm_bat_temp_state != FCM_BAT_TEMP_COLD) {
			try_charger_enable(fcm, false);
			fcm_printk("%s Temperatures cold!!!.", __func__);
		}
		desc->fcm_bat_temp_state = FCM_BAT_TEMP_COLD;
		ret = FCM_EVENT_BATT_COLD;
	} else if (temp < -desc->temp_low_extreme) {
		if (desc->fcm_bat_temp_state != FCM_BAT_TEMP_SUPER_COOL) {
			try_charger_enable(fcm, false);
			fcm_printk("%s Temperatures overcold!!!.", __func__);
		}
		desc->fcm_bat_temp_state = FCM_BAT_TEMP_SUPER_COOL;
		ret = FCM_EVENT_BATT_COLD;
	} else {
		fcm_bat_temp_state_change(fcm);
	}

	return ret;
}

static void turn_on_desktop_mode(struct fiio_charger_manager *fcm, bool enable)
{
	struct charger_desc *desc = fcm->desc;

	if (desc->is_desktop_mode == enable)
		return;

	if (fcm->power_data && fcm->power_data->set_mode_power_ctrl)
		fcm->power_data->set_mode_power_ctrl(POWER_BOARD_DESKTOP_MODE, enable);

	if (enable) {
		if (!fcm->charger_enabled) {
			try_charger_enable(fcm, true);
			fcm_printk("%s No charge, charge first\n", __func__);
		}
		desc->is_desktop_mode = true;
		fiio_sendMessage(fcm->dev, "DESKTOP_MODE=ON", NULL);
	} else {
		desc->is_desktop_mode = false;
		fiio_sendMessage(fcm->dev, "DESKTOP_MODE=OFF", NULL);
	}

	fcm_printk("%s enable = %d is_desktop_mode = %d\n", __func__, enable,
		   desc->is_desktop_mode);
}

static void _setup_polling(struct work_struct *work)
{
	unsigned long min = ULONG_MAX;
	struct fiio_charger_manager *fcm;
	bool keep_polling = false;
	unsigned long _next_polling;

	mutex_lock(&fcm_list_mtx);

	list_for_each_entry(fcm, &fcm_list, entry) {
		if (is_polling_required(fcm) && fcm->desc->polling_interval_ms) {
			keep_polling = true;

			if (min > fcm->desc->polling_interval_ms)
				min = fcm->desc->polling_interval_ms;

			if (fcm->desc->poll_count < 2) {
				min = 1000;
				fcm->desc->poll_count++;
			}
		}
	}

	polling_jiffy = msecs_to_jiffies(min);
	if (polling_jiffy <= FCM_JIFFIES_SMALL)
		polling_jiffy = FCM_JIFFIES_SMALL + 1;

	if (!keep_polling)
		polling_jiffy = ULONG_MAX;
	if (polling_jiffy == ULONG_MAX) {
		fcm_printk("%s go out\n", __func__);
		goto out;
	}

	WARN(fcm_wq == NULL, "charger-manager: workqueue not initialized"
			    ". try it later. %s\n", __func__);

	/*
	 * Use mod_delayed_work() iff the next polling interval should
	 * occur before the currently scheduled one.  If @fcm_monitor_work
	 * isn't active, the following mod_delayed_work() will queue it
	 * with the delay.
	 */
	_next_polling = jiffies + polling_jiffy;

	if (time_before(_next_polling, next_polling)) {
		mod_delayed_work(fcm_wq, &fcm_monitor_work, polling_jiffy);
		next_polling = _next_polling;
	} else {
		if (queue_delayed_work(fcm_wq, &fcm_monitor_work, polling_jiffy))
			next_polling = _next_polling;
	}
out:
	mutex_unlock(&fcm_list_mtx);
}
static DECLARE_WORK(setup_polling, _setup_polling);

static void close_buck_charge_process(struct fiio_charger_manager *fcm)
{
	cancel_work_sync(&setup_polling);
	cancel_delayed_work_sync(&fcm_monitor_work);
	cancel_delayed_work(&fcm->fullbatt_vchk_work);
	fcm_printk("%s end buck charging process!\n", __func__);
}

static bool _fcm_monitor(struct fiio_charger_manager *fcm)
{
	struct charger_desc *desc = fcm->desc;
	int temp_alrt;
	bool is_desktop_mode = false;

	temp_alrt = fcm_check_thermal_status(fcm);

	/* It has been stopped already */
	if (temp_alrt && fcm->emergency_stop)
		return false;

	if (fcm->power_data && fcm->power_data->is_had_dmode &&
	    fcm->power_data->check_desktop_mode)
		is_desktop_mode = fcm->power_data->check_desktop_mode();

	/*
	 * Check temperature whether overheat or cold.
	 * If temperature is out of range normal state, stop charging.
	 */
	if (temp_alrt) {
		fcm->emergency_stop = temp_alrt;
		if (!try_charger_enable(fcm, false))
			uevent_notify(fcm, default_event_names[temp_alrt]);

	/*
	 * Check whole charging duration and discharging duration
	 * after full-batt.
	 */
	} else if (!fcm->emergency_stop && check_charging_duration(fcm)) {
		dev_dbg(fcm->dev,
			"Charging/Discharging duration is out of range\n");
	/*
	 * Check whether fully charged state to protect overcharge
	 * if charger-manager is charging for battery.
	 */
	} else if (is_full_charged(fcm)) {
		try_charger_enable(fcm, false);
		fullbatt_vchk(&fcm->fullbatt_vchk_work.work);
	} else if (fcm->power_data->is_had_dmode && !fcm->emergency_stop &&
		   fcm->charger_enabled && is_desktop_mode == true &&
		   !desc->low_battery) {
		turn_on_desktop_mode(fcm, true);
		try_charger_enable(fcm, false);
	} else if ((!desc->qti_fast_charge || desc->pd_type) &&
		   (!fcm->user_set_disable_chg || is_desktop_mode)) {
		fcm->emergency_stop = 0;
		if (is_ext_pwr_online(fcm)) {
			if (!desc->is_desktop_mode || desc->low_battery) {
				if (is_desktop_mode)
					turn_on_desktop_mode(fcm, true);
				if (!try_charger_enable(fcm, true))
					uevent_notify(fcm, "CHARGING");
			}
		}
	} else {
		try_charger_enable(fcm, false);
	}

	return true;
}

static bool fcm_monitor(void)
{
	bool stop = false;
	struct fiio_charger_manager *fcm;

	mutex_lock(&fcm_list_mtx);

	list_for_each_entry(fcm, &fcm_list, entry) {
		if (_fcm_monitor(fcm))
			stop = true;
	}

	mutex_unlock(&fcm_list_mtx);

	return stop;
}

static void fcm_monitor_poller(struct work_struct *work)
{
	fcm_monitor();
	schedule_work(&setup_polling);
}

static void fullbatt_handler(struct fiio_charger_manager *fcm)
{
	struct charger_desc *desc = fcm->desc;

	if (!desc->fullbatt_vchkdrop_uV || !desc->fullbatt_vchkdrop_ms)
		goto out;

	mod_delayed_work(fcm_wq, &fcm->fullbatt_vchk_work,
			 msecs_to_jiffies(desc->fullbatt_vchkdrop_ms));
	fcm->fullbatt_vchk_jiffies_at = jiffies + msecs_to_jiffies(
				       desc->fullbatt_vchkdrop_ms);

	if (fcm->fullbatt_vchk_jiffies_at == 0)
		fcm->fullbatt_vchk_jiffies_at = 1;

out:
	dev_info(fcm->dev, "EVENT_HANDLE: Battery Fully Charged\n");
	uevent_notify(fcm, default_event_names[FCM_EVENT_BATT_FULL]);
}

static void battout_handler(struct fiio_charger_manager *fcm)
{
	if (!is_batt_present(fcm)) {
		dev_emerg(fcm->dev, "Battery Pulled Out!\n");
		uevent_notify(fcm, default_event_names[FCM_EVENT_BATT_OUT]);
	} else {
		uevent_notify(fcm, "Battery Reinserted?");
	}
}

static void misc_event_handler(struct fiio_charger_manager *fcm,
			enum fcm_event_types type)
{
	if (is_polling_required(fcm) && fcm->desc->polling_interval_ms)
		schedule_work(&setup_polling);
	uevent_notify(fcm, default_event_names[type]);
}

static int fiio_charger_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct fiio_charger_manager *fcm = power_supply_get_drvdata(psy);
	struct charger_desc *desc = fcm->desc;
	struct power_supply *fuel_gauge = NULL;
	int ret = 0;

	val->intval = -ENODATA;
	if (!fcm)
		return -EPERM;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (is_charging(fcm))
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else if (is_ext_pwr_online(fcm))
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		switch (desc->fcm_bat_temp_state) {
		case FCM_BAT_TEMP_SUPER_COOL:
		case FCM_BAT_TEMP_SUPER_HEAT:
			val->intval = POWER_SUPPLY_HEALTH_DEAD;
			break;
		case FCM_BAT_TEMP_COLD:
		case FCM_BAT_TEMP_HOT:
			if (desc->vbus_plugin_fiio || desc->qti_device) {
				val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
				break;
			}
			fallthrough;
		default:
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
			break;
		}
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		if (is_batt_present(fcm))
			val->intval = 1;
		else
			val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		fuel_gauge = power_supply_get_by_name(fcm->desc->psy_fuel_gauge);
		if (!fuel_gauge) {
			ret = -ENODEV;
			break;
		}
		ret = power_supply_get_property(fuel_gauge,
				POWER_SUPPLY_PROP_TECHNOLOGY, val);
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		fuel_gauge = power_supply_get_by_name(fcm->desc->psy_fuel_gauge);
		if (!fuel_gauge) {
			ret = -ENODEV;
			break;
		}
		ret = power_supply_get_property(fuel_gauge,
				POWER_SUPPLY_PROP_CYCLE_COUNT, val);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = get_batt_uV(fcm, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		fuel_gauge = power_supply_get_by_name(fcm->desc->psy_fuel_gauge);
		if (!fuel_gauge) {
			ret = -ENODEV;
			break;
		}
		ret = power_supply_get_property(fuel_gauge,
				POWER_SUPPLY_PROP_CURRENT_NOW, val);
		break;
	case POWER_SUPPLY_PROP_TEMP:
	case POWER_SUPPLY_PROP_TEMP_AMBIENT:
		if (desc->force_temp_en) {
			val->intval = desc->force_temp;
			break;
		}
		fcm_get_battery_temperature(fcm, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (desc->force_cap < 200) {
			val->intval = desc->force_cap;
			fcm_printk("%s val->intval = %d\n", __func__, val->intval);
			break;
		}

		if (!is_batt_present(fcm)) {
			/* There is no battery. Assume 100% */
			val->intval = 100;
			break;
		}

		fuel_gauge = power_supply_get_by_name(fcm->desc->psy_fuel_gauge);
		if (!fuel_gauge) {
			ret = -ENODEV;
			break;
		}
		ret = power_supply_get_property(fuel_gauge,
				POWER_SUPPLY_PROP_CAPACITY, val);
		if (ret)
			break;

		if (val->intval > 100) {
			val->intval = 100;
			break;
		}
		if (val->intval < 0)
			val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		if (is_ext_pwr_online(fcm))
			val->intval = 1;
		else
			val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		if (is_full_charged(fcm))
			val->intval = 1;
		else
			val->intval = 0;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		if (is_charging(fcm)) {
			fuel_gauge = power_supply_get_by_name(
					fcm->desc->psy_fuel_gauge);
			if (!fuel_gauge) {
				ret = -ENODEV;
				break;
			}

			ret = power_supply_get_property(fuel_gauge,
						POWER_SUPPLY_PROP_CHARGE_NOW,
						val);
			if (ret) {
				val->intval = 1;
				ret = 0;
			} else {
				/* If CHARGE_NOW is supplied, use it */
				val->intval = (val->intval > 0) ?
						val->intval : 1;
			}
		} else {
			val->intval = 0;
		}
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT: {
		struct power_supply *charge_psy;
		int i;

		for (i = 0; fcm->desc->psy_charger_stat[i]; i++) {
			charge_psy = power_supply_get_by_name(fcm->desc->psy_charger_stat[i]);
			if (charge_psy)
				ret = power_supply_get_property(charge_psy,
						POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT, val);
		}
		break;
	}
	default:
		return -EINVAL;
	}

	return ret;
}

#define NUM_CHARGER_PSY_OPTIONAL	(4)
static enum power_supply_property default_charger_props[] = {
	/* Guaranteed to provide */
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	/*
	 * Optional properties are:
	 * POWER_SUPPLY_PROP_CHARGE_NOW,
	 * POWER_SUPPLY_PROP_CURRENT_NOW,
	 * POWER_SUPPLY_PROP_TEMP, and
	 * POWER_SUPPLY_PROP_TEMP_AMBIENT,
	 */
};

static const struct power_supply_desc psy_default = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = default_charger_props,
	.num_properties = ARRAY_SIZE(default_charger_props),
	.get_property = fiio_charger_get_property,
	.no_thermal = true,
};

static int fcm_init_thermal_data(struct fiio_charger_manager *fcm,
		struct power_supply *fuel_gauge)
{
	struct charger_desc *desc = fcm->desc;

	fcm->desc->measure_battery_temp = true;
#ifdef CONFIG_THERMAL
	if (desc->thermal_zone) {
		fcm->tzd_batt =
			thermal_zone_get_zone_by_name(desc->thermal_zone);
		if (IS_ERR(fcm->tzd_batt))
			return PTR_ERR(fcm->tzd_batt);
	}
#endif
	if (fcm->desc->measure_battery_temp) {
		/* NOTICE : Default allowable minimum charge temperature is 0 */
		if (!desc->temp_hight_stop)
			desc->temp_hight_stop = 500;
		if (!desc->temp_diff)
			desc->temp_diff = 50;
	}

	return 0;
}

static void start_buck_charge_process(struct fiio_charger_manager *fcm)
{
	fcm_monitor();
	schedule_work(&setup_polling);
	fcm_printk("%s start buck charging process!\n", __func__);
}

static int pump_charger_enable(struct fiio_charger_manager *fcm, bool enable)
{
	int err = 0;

	if (fcm->power_data && fcm->power_data->pump_charge_enable) {
		err = fcm->power_data->pump_charge_enable(enable);
		if (err < 0)
			fcm_printk("%s:failed to enable pump charge\n", __func__);
		else
			fcm_printk("%s:pump charger enable success\n", __func__);
	}

	return err;
}

static void close_pump_charge_process(struct fiio_charger_manager *fcm)
{
	cancel_delayed_work_sync(&fcm->fiio_pump_charge_work);
	pump_charger_enable(fcm, false);
	fcm_printk("%s end pump charging process!\n", __func__);
}

static void select_pps_gear(struct fiio_charger_manager *fcm)
{
	struct charger_desc *desc = fcm->desc;
	int i, j;

	if (fcm->power_data && fcm->power_data->pps_modulation_power)
		desc->pps_enable = fcm->power_data->pps_modulation_power(desc->sink_req_val,
									 desc->sink_req_cur);

	if (!desc->pps_enable) {
		fcm_printk("%s goto select pps gear \n", __func__);
		for (i = 0; i < 3; i++) {
			for (j = 0; j < 2; j++) {
				if (fcm->power_data && fcm->power_data->pps_modulation_power)
					desc->pps_enable = fcm->power_data->pps_modulation_power(
							desc->sink_req_val, desc->sink_req_cur);
				if (desc->pps_enable) {
					fcm_printk("%s sink_req_val=%dmV,sink_req_cur=%dmA\n", __func__,
						   desc->sink_req_val, desc->sink_req_cur);
					return;
				}
			}
			desc->sink_req_cur -= 1000;
			desc->sink_tar_cur -= 1000;
		}
		fcm_printk("%s pps not support!!! \n", __func__);
	}
}

static void pump_voltage_regulation_strategy(struct fiio_charger_manager *fcm)
{
	struct charger_desc *desc = fcm->desc;
	int fullbat_mV = desc->fullbatt_uV / 1000;

	if (desc->wq_cur < desc->sink_tar_cur && desc->wq_val < fullbat_mV) {
		if (desc->pump_amend_value < desc->pump_amend_value_max)
			desc->pump_amend_value += desc->pps_voltage_step_mV;
	} else if (desc->wq_cur > desc->sink_tar_cur) {
		desc->pump_amend_value -= desc->pps_voltage_step_mV;
	}

	if (desc->wq_val >= fullbat_mV) {
		if (desc->wq_cur < desc->pump_cc_off_current) {
			pump_charger_enable(fcm, false);
			start_buck_charge_process(fcm);
			close_pump_charge_process(fcm);
			fcm_printk("%s switch buck charge\n", __func__);
		} else {
			desc->pump_amend_value += (fullbat_mV - desc->wq_val) * 4;
		}
	}

	desc->sink_req_val = (desc->pump_amend_value + desc->wq_val * 2 > desc->sink_req_max_val) ?
		desc->sink_req_max_val : desc->pump_amend_value + desc->wq_val * 2;
}

void fiio_pump_charge_monitor_work(struct work_struct *work)
{
	struct fiio_charger_manager *fcm = container_of(work,
			struct fiio_charger_manager, fiio_pump_charge_work.work);
	struct charger_desc *desc = fcm->desc;
	int ret, temp_alrt;
	int uA, uV;
	bool is_desktop_mode = false;

	temp_alrt = fcm_check_thermal_status(fcm);
	if (temp_alrt) {
		pump_charger_enable(fcm, false);
		fcm_printk("%s temp abnormal,stop pump charge!!!\n", __func__);
		goto exit;
	}

	ret = get_batt_uA(fcm, &uA);
	if (ret < 0) {
		fcm_printk("%s failed to get battery current\n", __func__);
		goto exit;
	}

	ret = get_batt_uV(fcm, &uV);
	if (ret < 0) {
		fcm_printk("%s failed get battery voltage\n", __func__);
		goto exit;
	}

	desc->wq_val = uV / 1000;
	desc->wq_cur = uA / 1000;

	pump_voltage_regulation_strategy(fcm);
	select_pps_gear(fcm);

	if (fcm->power_data && fcm->power_data->is_had_dmode)
		is_desktop_mode = fcm->power_data->check_desktop_mode();

	if (!desc->pps_enable || is_desktop_mode) {
		pump_charger_enable(fcm, false);
		start_buck_charge_process(fcm);
		fcm_printk("%s goto buck charge process!!! \n", __func__);
		return;
	}

	if (!is_charging(fcm))
		pump_charger_enable(fcm, true);

exit:
	schedule_delayed_work(&fcm->fiio_pump_charge_work, msecs_to_jiffies(3000));
}

void fiio_get_battery_information_work(struct work_struct *work)
{
	struct fiio_charger_manager *fcm = container_of(work,
			struct fiio_charger_manager, get_battery_information_work.work);
	struct charger_desc *desc = fcm->desc;

	fcm_check_thermal_status(fcm);

	get_batt_cap(fcm, &desc->bat_capacity);

	if (desc->bat_capacity < 15) {
		desc->low_battery = true;
	} else if (desc->low_battery &&
		   desc->bat_capacity >= desc->user_set_disable_charge_level) {
		desc->low_battery = false;
		fcm_printk("%s Exit the low-power mode\n", __func__);
	}

	schedule_delayed_work(&fcm->get_battery_information_work, msecs_to_jiffies(1000));
}

void fiio_charger_status_monitor_work(struct work_struct *work)
{
	struct fiio_charger_manager *fcm = container_of(work,
			struct fiio_charger_manager, charger_status_monitor.work);
	struct power_supply *psy;
	union power_supply_propval val;
	int i;
	int ret;
	static int status;
	static int old_status;

	for (i = 0; fcm->desc->psy_charger_stat[i]; i++) {
		psy = power_supply_get_by_name(fcm->desc->psy_charger_stat[i]);
		if (!psy)
			continue;

		ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_STATUS,
				&val);
		if (ret) {
			dev_warn(fcm->dev, "Cannot read STATUS value from %s\n",
				 fcm->desc->psy_charger_stat[i]);
			continue;
		}
		status = val.intval;
	}

	if (status != old_status) {
		old_status = status;
		power_supply_changed(fcm->charger_psy);
		fcm_printk("%s The charging status changes.\n", __func__);
	}

	schedule_delayed_work(&fcm->charger_status_monitor, msecs_to_jiffies(1000));
}

static void select_charge_process(struct fiio_charger_manager *fcm)
{
	struct charger_desc *desc;
	int uV;
	int err;
	bool is_desktop_mode = false;

	if (!fcm->power_data) {
		fcm_printk("%s get power_board module failed. \n", __func__);
		return;
	}

	desc = fcm->desc;
	err = get_batt_uV(fcm, &uV);
	if (err) {
		dev_err(fcm->dev, "%s: get_batt_uV error(%d)\n", __func__, err);
		return;
	}

	if (fcm->power_data->is_had_dmode) {
		if (fcm->power_data->check_desktop_mode)
			is_desktop_mode = fcm->power_data->check_desktop_mode();
		fcm_printk("%s is_desktop_mode = %d \n", __func__, is_desktop_mode);
	}

	desc->is_full = false;
	desc->pump_amend_value = desc->pump_amend_value_start;
	fcm->charger_enabled = false;
	desc->sink_req_cur = desc->sink_req_max_cur;
	desc->sink_tar_cur = desc->sink_tar_max_cur;

	fcm_printk("%s has_pump = %d \n", __func__, desc->has_pump);
	if (desc->has_pump && uV > desc->pump_charge_low_voltage &&
	    uV < desc->pump_charge_hight_voltage && !is_desktop_mode) {
		schedule_delayed_work(&fcm->fiio_pump_charge_work, msecs_to_jiffies(3000));
		fcm_printk("%s start pump charging process!\n", __func__);
	} else {
		start_buck_charge_process(fcm);
	}
}

static void start_charge_process(struct fiio_charger_manager *fcm)
{
	struct charger_desc *desc = fcm->desc;

	if (!desc->disable_charge) {
		desc->fcm_bat_temp_state = fcm_bat_temp_state_update(fcm);
		select_charge_process(fcm);
	}

	__pm_stay_awake(fcm->fcm_wakelock);
}

static void exit_charge_process(struct fiio_charger_manager *fcm)
{
	struct charger_desc *desc = fcm->desc;

	turn_on_desktop_mode(fcm, false);
	try_charger_enable(fcm, false);
	close_buck_charge_process(fcm);
	close_pump_charge_process(fcm);

	if (fcm->power_data && fcm->power_data->set_charge_status_indicator) {
		if (desc->after_charging_color == RED) {
			if (desc->before_charging_color == RED)
				fcm->power_data->set_charge_status_indicator(BLUE);
			else
				fcm->power_data->set_charge_status_indicator(desc->before_charging_color);
		} else {
			fcm->power_data->set_charge_status_indicator(desc->after_charging_color);
		}
	}

	__pm_relax(fcm->fcm_wakelock);
}

static int fiio_vbus_detection_notifier_cb(struct notifier_block *self,
					   unsigned long event, void *data)
{
	struct fiio_charger_manager *fcm = container_of(self,
			struct fiio_charger_manager, fiio_vbus_notifier);
	struct charger_desc *desc = fcm->desc;

	switch (event) {
	case NOTIFY_EVENT_CHARGER_VBUS_PLUGIN_FIIO:
		fcm_printk("%s NOTIFY_EVENT_CHARGER_VBUS_PLUGIN_FIIO", __func__);
		desc->vbus_plugin_fiio = true;
		start_charge_process(fcm);
		report_usb_plug_event(fcm, true);
		power_supply_changed(fcm->charger_psy);
		if (fcm->uac_disable_chg && fcm->power_data &&
		    fcm->power_data->set_uac_charging_disabled)
			fcm->power_data->set_uac_charging_disabled(false);
		break;
	case NOTIFY_EVENT_CHARGER_VBUS_PLUGOUT_FIIO:
		fcm_printk("%s NOTIFY_EVENT_CHARGER_VBUS_PLUGOUT_FIIO", __func__);
		desc->vbus_plugin_fiio = false;
		desc->pd_type = false;
		desc->poll_count = 0;
		if (!desc->qti_device) {
			exit_charge_process(fcm);
			desc->low_battery = false;
		} else {
			fcm->charger_enabled = false;
			if (fcm->uac_disable_chg && fcm->power_data &&
			    fcm->power_data->set_uac_charging_disabled)
				fcm->power_data->set_uac_charging_disabled(true);
		}
		fiio_sendMessage(fcm->dev, "EXTERNAL_CHARGE_TYPE_PD=OFF", NULL);
		report_usb_plug_event(fcm, false);
		power_supply_changed(fcm->charger_psy);
		fcm_printk("%s unplug the charger!\n", __func__);
		break;
	case NOTIFY_EVENT_CHARGER_VBUS_TPYE_PD_FIIO:
		fcm_printk("%s Fast charger detected\n", __func__);
		desc->pd_type = true;
		if (desc->ichrg_current != desc->ichg_fast) {
			fcm_printk("%s The charging current will be updated to fast charging\n",
				   __func__);
			fiio_set_limit_cur_and_charge_cur(fcm);
		}
		charger_power_pd_fix_gear_handle(fcm);
		fiio_sendMessage(fcm->dev, "EXTERNAL_CHARGE_TYPE_PD=ON", NULL);
		break;
	default:
		break;
	}

	return 0;
}

static int qti_vbus_detection_notifier_cb(struct notifier_block *self,
					  unsigned long event, void *data)
{
	struct fiio_charger_manager *fcm = container_of(self,
			struct fiio_charger_manager, qti_vbus_notifier);
	struct charger_desc *desc = fcm->desc;

	switch (event) {
	case NOTIFY_EVENT_CHARGER_VBUS_PLUGIN_QTI:
		fcm_printk("%s NOTIFY_EVENT_CHARGER_VBUS_PLUGIN_QTI", __func__);
		desc->qti_device = true;
		if (!desc->vbus_plugin_fiio)
			start_charge_process(fcm);
		report_usb_plug_event(fcm, true);
		power_supply_changed(fcm->charger_psy);
		break;
	case NOTIFY_EVENT_CHARGER_VBUS_PLUGOUT_QTI:
		fcm_printk("%s NOTIFY_EVENT_CHARGER_VBUS_PLUGOUT_QTI", __func__);
		if (desc->qti_device && !desc->vbus_plugin_fiio) {
			desc->low_battery = false;
			exit_charge_process(fcm);
			__pm_relax(fcm->fcm_wakelock);
		}
		desc->qti_fast_charge = false;
		desc->qti_host = false;
		desc->poll_count = 0;
		desc->qti_device = false;
		report_usb_plug_event(fcm, false);
		power_supply_changed(fcm->charger_psy);
		break;
	case NOTIFY_EVENT_CHARGER_VBUS_TYPE_FAST_CHAGRER_QTI:
		fcm_printk("%s NOTIFY_EVENT_CHARGER_VBUS_TYPE_FAST_CHAGRER_QTI", __func__);
		if (!desc->pd_type) {
			desc->poll_count = 0;
			desc->qti_fast_charge = true;
		}
		if (fcm->uac_disable_chg && fcm->power_data &&
		    fcm->power_data->set_uac_charging_disabled)
			fcm->power_data->set_uac_charging_disabled(true);
		desc->qti_device = true;
		__pm_stay_awake(fcm->fcm_wakelock);
		break;
	case NOTIFY_EVENT_CHARGER_VBUS_TYPE_ORDINARY_CHAGRER_QTI:
		fcm_printk("%s NOTIFY_EVENT_CHARGER_VBUS_TYPE_ORDINARY_CHAGRER_QTI", __func__);
		desc->qti_device = true;
		if (fcm->uac_disable_chg && fcm->power_data &&
		    fcm->power_data->set_uac_charging_disabled)
			fcm->power_data->set_uac_charging_disabled(true);
		__pm_stay_awake(fcm->fcm_wakelock);
		break;
	case NOTIFY_EVENT_CHARGER_VBUS_OTG_QTI:
		fcm_printk("%s NOTIFY_EVENT_CHARGER_VBUS_OTG_QTI", __func__);
		desc->qti_host = true;
		break;
	default:
		break;
	}

	return 0;
}

static int pump_irq_notifier_cb(struct notifier_block *self,
				unsigned long event, void *data)
{
	struct fiio_charger_manager *fcm = container_of(self,
			struct fiio_charger_manager, pump_irq_notifier);
	struct charger_desc *desc = fcm->desc;
	int uV, err;

	err = get_batt_uV(fcm, &uV);
	if (err) {
		dev_err(fcm->dev, "%s: get_batt_uV error(%d)\n", __func__, err);
		return 0;
	}

	if (event == 0) {
		fcm_printk("%s vdrop ovp pump_correction_value:%d", __func__,
			   desc->pump_amend_value);
		desc->pump_amend_value -= 120;
	} else if (event == 1) {
		fcm_printk("%s vdrop min pump_correction_value:%d", __func__,
			   desc->pump_amend_value);
		if (uV >= 4500000)
			desc->pump_amend_value += 80;
	}

	return 0;
}

static int dmode_irq_notifier_cb(struct notifier_block *self,
				 unsigned long event, void *data)
{
	struct fiio_charger_manager *fcm = container_of(self,
			struct fiio_charger_manager, dmode_irq_notifier);
	struct charger_desc *desc = fcm->desc;

	if (event == NOTIFY_EVENT_OPEN_DESKTOP_MODE) {
		if (desc->bat_capacity > 14 && desc->low_battery)
			desc->low_battery = false;
		if (desc->vbus_plugin_fiio || desc->qti_device) {
			turn_on_desktop_mode(fcm, true);
			if (!desc->low_battery)
				try_charger_enable(fcm, false);
		}
		fcm_printk("%s turn on dmode key", __func__);
	} else if (event == NOTIFY_EVENT_CLOSE_DESKTOP_MODE) {
		if (desc->vbus_plugin_fiio || desc->qti_device) {
			close_buck_charge_process(fcm);
			close_pump_charge_process(fcm);
			select_charge_process(fcm);
			turn_on_desktop_mode(fcm, false);
		}
		fcm_printk("%s vbus_plugin_fiio %d turn off dmode key", __func__,
			   desc->vbus_plugin_fiio);
	}

	return 0;
}

void fiio_boot_complete_work(struct work_struct *work)
{
	struct fiio_charger_manager *fcm = container_of(work,
			struct fiio_charger_manager, boot_complete_work.work);
	struct charger_desc *desc = fcm->desc;
	int vbus_plugin_fiio = 0;
	int vbus_plugin_qti = 0;
	int power_role_qti = 0;

	if (fcm->power_data) {
		if (fcm->power_data->get_charger_cc_state_fiio)
			vbus_plugin_fiio = fcm->power_data->get_charger_cc_state_fiio();
		if (fcm->power_data->get_charger_power_role_qti)
			power_role_qti = fcm->power_data->get_charger_power_role_qti();
		if (fcm->power_data->get_charger_power_role_qti)
			vbus_plugin_qti = fcm->power_data->get_charger_vbus_qti();
	}

	if (vbus_plugin_fiio)
		desc->vbus_plugin_fiio = true;
	else
		desc->vbus_plugin_fiio = false;

	if (vbus_plugin_qti && !power_role_qti) {
		desc->qti_device = true;
	} else if (power_role_qti) {
		desc->qti_host = true;
	} else {
		desc->qti_host = false;
		desc->qti_device = false;
	}

	if (desc->vbus_plugin_fiio || desc->qti_device)
		start_charge_process(fcm);
}

static ssize_t usb_voltage_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct fiio_charger_manager *fcm = dev_get_drvdata(dev);
	int value = 0;

	if (fcm->power_data && fcm->power_data->pump_get_vbus)
		value = fcm->power_data->pump_get_vbus();

	return sprintf(buf, "%d\n", value);
}

static ssize_t usb_current_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct fiio_charger_manager *fcm = dev_get_drvdata(dev);
	int value = 0;

	if (fcm->power_data && fcm->power_data->pump_get_ibus)
		value = fcm->power_data->pump_get_ibus();

	return sprintf(buf, "%d\n", value);
}

static ssize_t usb_type_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct fiio_charger_manager *fcm = dev_get_drvdata(dev);
	struct charger_desc *desc = fcm->desc;

	fcm->charger_protocol_fiio_module =
		fiio_device_get_data_by_name("charger_protocol_fiio module");
	if (!fcm->charger_protocol_fiio_module) {
		fcm_printk("%s get charger_protocol_fiio module failed. \n", __func__);
		return sprintf(buf, "%s\n", "Unknown");
	}

	return sprintf(buf, "%s\n", desc->pd_type ? "PD" : "Unknown");
}

static DEVICE_ATTR_RO(usb_voltage);
static DEVICE_ATTR_RO(usb_current);
static DEVICE_ATTR_RO(usb_type);

static struct attribute *fiio_charge_attributes[] = {
	&dev_attr_usb_voltage.attr,
	&dev_attr_usb_current.attr,
	&dev_attr_usb_type.attr,
	NULL
};

static const struct attribute_group fiio_charge_attr_group = {
	.attrs = fiio_charge_attributes,
};

static int fiio_get_psy(struct fiio_charger_manager *fcm)
{
	struct charger_desc *desc = fcm->desc;
	struct power_supply *fuel_gauge;
	int i = 0, ret = 0;

	desc->disable_charge = true;

	/* Counting index only */
	while (desc->psy_charger_stat[i])
		i++;

	/* Check if charger's supplies are present at probe */
	for (i = 0; desc->psy_charger_stat[i]; i++) {
		struct power_supply *psy;

		fcm_printk("%s power supply name: %s\n", __func__,
			   desc->psy_charger_stat[i]);
		psy = power_supply_get_by_name(desc->psy_charger_stat[i]);
		if (!psy) {
			fcm_printk("%s Cannot find power supply \"%s\"\n", __func__,
				   fcm->desc->psy_charger_stat[i]);
			ret--;
		}
	}

	fuel_gauge = power_supply_get_by_name(desc->psy_fuel_gauge);
	if (!fuel_gauge) {
		fcm_printk("%s Cannot find power supply \"%s\"\n", __func__,
			   desc->psy_fuel_gauge);
		ret--;
	}

	if (ret < 0)
		return ret;

	ret = fcm_init_thermal_data(fcm, fuel_gauge);
	if (ret) {
		dev_err(fcm->dev, "Failed to initialize thermal data\n");
		fcm->desc->measure_battery_temp = false;
	}

	desc->disable_charge = false;

	return ret;
}

static void fiio_charger_get_modules_work_func(struct work_struct *work)
{
	struct fiio_charger_manager *fcm = container_of(work,
			struct fiio_charger_manager, fiio_charger_get_modules_work.work);

	fcm_printk("%s: Enter!\n", __func__);

	fcm->power_data = fiio_device_get_data_by_name("power_board module");
	if (!fcm->power_data)
		fcm_printk("%s get power_board module failed. \n", __func__);

	fcm->charger_protocol_fiio_module =
		fiio_device_get_data_by_name("charger_protocol_fiio module");
	if (!fcm->charger_protocol_fiio_module)
		fcm_printk("%s get charger_protocol_fiio module failed!\n", __func__);

	fiio_get_psy(fcm);
}

static const struct of_device_id fiio_charger_manager_match[] = {
	{
		.compatible = "fiio,fiio_charge_manager",
	},
	{},
};
MODULE_DEVICE_TABLE(of, fiio_charger_manager_match);

static struct charger_desc *of_fcm_parse_desc(struct device *dev)
{
	struct charger_desc *desc;
	struct device_node *np = dev->of_node;
	u32 poll_mode = FCM_POLL_DISABLE;
	u32 battery_stat = FCM_NO_BATTERY;
	int num_chgs = 0;
	int ret;

	desc = devm_kzalloc(dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return ERR_PTR(-ENOMEM);

	of_property_read_string(np, "fcm-name", &desc->psy_name);

	of_property_read_u32(np, "fcm-poll-mode", &poll_mode);
	desc->polling_mode = poll_mode;

	of_property_read_u32(np, "fcm-poll-interval",
				&desc->polling_interval_ms);

	of_property_read_u32(np, "fcm-fullbatt-vchkdrop-ms",
					&desc->fullbatt_vchkdrop_ms);
	of_property_read_u32(np, "fcm-fullbatt-vchkdrop-volt",
					&desc->fullbatt_vchkdrop_uV);
	of_property_read_u32(np, "fcm-fullbatt-voltage", &desc->fullbatt_uV);
	of_property_read_u32(np, "fcm-fullbatt-current", &desc->fullbatt_uA);
	of_property_read_u32(np, "fcm-fullbatt-capacity",
					&desc->fullbatt_full_capacity);

	of_property_read_u32(np, "fcm-battery-stat", &battery_stat);
	desc->battery_present = battery_stat;

	/* chargers */
	of_property_read_u32(np, "fcm-num-chargers", &num_chgs);
	if (num_chgs) {
		int i;

		/* Allocate empty bin at the tail of array */
		desc->psy_charger_stat = devm_kcalloc(dev,
						      num_chgs + 1,
						      sizeof(char *),
						      GFP_KERNEL);
		if (!desc->psy_charger_stat)
			return ERR_PTR(-ENOMEM);

		for (i = 0; i < num_chgs; i++)
			of_property_read_string_index(np, "fcm-chargers",
						      i, &desc->psy_charger_stat[i]);

		if (num_chgs == 2) {
			desc->has_pump = true;
			fcm_printk("%s: has_pump : %d\n", __func__, desc->has_pump);
		}
	}

	of_property_read_string(np, "fcm-fuel-gauge", &desc->psy_fuel_gauge);

	ret = of_property_read_u32(np, "fcm-battery-temp-low-extreme",
				   &desc->temp_low_extreme);
	if (IS_ERR_VALUE((long)ret)) {
		dev_err(dev, "id property fcm-battery-temp-low-extreme reading fail\n");
		desc->temp_low_extreme = 200;
	}
	of_property_read_u32(np, "fcm-battery-temp-low-stop", &desc->temp_low_stop);
	of_property_read_u32(np, "fcm-battery-temp-low-cold-limit",
			     &desc->temp_low_cold_limit);
	of_property_read_u32(np, "fcm-battery-temp-low-cool-limit",
			     &desc->temp_low_cool_limit);
	of_property_read_u32(np, "fcm-battery-temp-normal", &desc->temp_normal);
	of_property_read_u32(np, "fcm-battery-temp-hight-limit",
			     &desc->temp_hight_limit);
	ret = of_property_read_u32(np, "fcm-battery-temp-hight-stop",
				   &desc->temp_hight_stop);
	if (IS_ERR_VALUE((long)ret)) {
		dev_err(dev, "id property fcm-battery-temp-hight-stop reading fail\n");
		desc->temp_low_extreme = 600;
	}
	of_property_read_u32(np, "fcm-battery-temp-diff", &desc->temp_diff);
	of_property_read_u32(np, "fcm-battery-temp-thrd", &desc->temp_thrd);

	of_property_read_u32(np, "fcm-battery-temp-cold-current", &desc->ichg_cold);
	of_property_read_u32(np, "fcm-battery-temp-cool-current", &desc->ichg_cool);
	of_property_read_u32(np, "fcm-battery-temp-normal-current", &desc->ichg_normal);
	of_property_read_u32(np, "fcm-battery-temp-warm-current", &desc->ichg_warm);
	of_property_read_u32(np, "fcm-battery-temp-hot-current", &desc->ichg_hot);
	of_property_read_u32(np, "fcm-battery-temp-unknown-current", &desc->ichg_unknow);
	if (!desc->ichg_unknow)
		desc->ichg_unknow = 3000000;
	of_property_read_u32(np, "fcm-battery-temp-fast-current", &desc->ichg_fast);

	ret = of_property_read_u32(np, "fcm-fix-request-voltage-gear",
				   &desc->fix_expect_the_power_gear);
	if (ret < 0) {
		fcm_printk("%s fix_expect_the_power_gear = %d\n", __func__,
			   desc->fix_expect_the_power_gear);
		desc->fix_expect_the_power_gear = 5;
	}

	of_property_read_u32(np, "fcm-pps-request-voltage-max", &desc->sink_req_max_val);
	of_property_read_u32(np, "fcm-pps-request-current-max", &desc->sink_req_max_cur);
	of_property_read_u32(np, "fcm-pps-battery-target-current-max",
			     &desc->sink_tar_max_cur);
	of_property_read_u32(np, "fcm-pps-voltage-step-mV", &desc->pps_voltage_step_mV);
	of_property_read_u32(np, "fcm-pump-voltage-amend-value-max",
			     &desc->pump_amend_value_max);
	of_property_read_u32(np, "fcm-pump-voltage-amend-value-start",
			     &desc->pump_amend_value_start);
	of_property_read_u32(np, "fcm-pump-cc-off-current", &desc->pump_cc_off_current);
	of_property_read_u32(np, "fcm-pump-charge-low-voltage",
			     &desc->pump_charge_low_voltage);
	of_property_read_u32(np, "fcm-pump-charge-hight-voltage",
			     &desc->pump_charge_hight_voltage);
	of_property_read_u32(np, "fcm-pump-charge-voltage", &desc->pump_charge_voltage);
	fcm_printk("%s pump_charge_voltage = %d\n", __func__, desc->pump_charge_voltage);
	of_property_read_u32(np, "fcm-pump_normal_temp_control-value",
			     &desc->pump_normal_temp_control);

	/* battery charger overvoltage protection */
	of_property_read_u32(np, "fcm-charging-max",
				&desc->charging_max_duration_ms);
	of_property_read_u32(np, "fcm-discharging-max",
				&desc->discharging_max_duration_ms);

	of_property_read_string(np, "fcm-battery-temp-compensation-enable",
				&desc->temp_compensation_enable);
	if (!desc->temp_compensation_enable)
		desc->temp_compensation_enable = "false";

	return desc;
}

static inline struct charger_desc *fcm_get_drv_data(struct platform_device *pdev)
{
	if (pdev->dev.of_node)
		return of_fcm_parse_desc(&pdev->dev);
	return dev_get_platdata(&pdev->dev);
}

static enum alarmtimer_restart cm_timer_func(struct alarm *alarm, ktime_t now)
{
	return ALARMTIMER_NORESTART;
}

static ssize_t disable_chg_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct fiio_charger_manager *fcm = g_fcm;

	return sprintf(buf, "%d\n", fcm->user_set_disable_chg);
}

static ssize_t disable_chg_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t size)
{
	struct fiio_charger_manager *fcm = g_fcm;
	unsigned int val = 0;
	int ret;

	if (!fcm)
		return 0;

	ret = kstrtouint(buf, 0, &val);
	if (ret == 0) {
		fcm_printk("val = %#x.\n", val);
		if (val == 0)
			fcm->user_set_disable_chg = false;
		else if (val == 1)
			fcm->user_set_disable_chg = true;
		else
			fcm_printk("store parameter error.\n");
	}

	return size;
}

static ssize_t send_charge_type_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	struct fiio_charger_manager *fcm = g_fcm;
	struct charger_desc *desc;

	if (!size)
		return size;

	desc = fcm->desc;
	fcm_printk("%s, send pd type and deskmode type \n", __func__);
	fcm_printk("%s, desc->is_desktop_mode = %s desc->pd_type = %s\n", __func__,
		   desc->is_desktop_mode ? "ON" : "OFF",
		   desc->pd_type ? "ON" : "OFF");
	fiio_sendMessage(fcm->dev, desc->is_desktop_mode ?
			 "DESKTOP_MODE=ON" : "DESKTOP_MODE=OFF", NULL);
	fiio_sendMessage(fcm->dev, desc->pd_type ?
			 "EXTERNAL_CHARGE_TYPE_PD=ON" : "EXTERNAL_CHARGE_TYPE_PD=OFF", NULL);
	if (desc->qti_device)
		fiio_sendMessage(fcm->dev, "USB_PLUG=ON", NULL);

	return size;
}

static ssize_t temp_compensation_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct fiio_charger_manager *fcm = g_fcm;
	int temp_compansetion = fcm->desc->temp_adjustment / 10;

	return sprintf(buf, "%d\n", temp_compansetion);
}

static void fiio_test_temp(struct fiio_charger_manager *fcm,
			   bool test_temp_en, int temp)
{
	struct charger_desc *desc = fcm->desc;

	desc->force_temp = temp;
	desc->force_temp_en = test_temp_en;
	fcm_printk("%s test_temp_en = %d, force_temp = %d\n", __func__,
		   test_temp_en, temp);
	power_supply_changed(fcm->charger_psy);
}

static ssize_t force_temp_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct fiio_charger_manager *fcm = g_fcm;
	unsigned long long val = 0;
	int result;

	result = kstrtoull(buf, 0, &val);
	if (result < 0)
		return result;

	if (val > 120)
		fiio_test_temp(fcm, false, (int)val * 50 / 6 - 300);
	else
		fiio_test_temp(fcm, true, (int)val * 50 / 6 - 300);

	return count;
}

static ssize_t force_temp_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct fiio_charger_manager *fcm = g_fcm;
	struct charger_desc *desc;
	ssize_t count;

	if (!fcm) {
		count = sprintf(buf, "%s\n", "0");
	} else {
		desc = fcm->desc;
		count = sprintf(buf, "%d\n", desc->force_temp);
	}

	return count;
}

static ssize_t force_cap_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct fiio_charger_manager *fcm = g_fcm;
	struct charger_desc *desc = fcm->desc;
	unsigned int val = 0;
	int result;

	result = kstrtouint(buf, 0, &val);
	if (result < 0)
		return result;

	desc->force_cap = val;
	power_supply_changed(fcm->charger_psy);

	return count;
}

static ssize_t user_set_disable_charge_level_store(struct device *dev,
						   struct device_attribute *attr,
						   const char *buf, size_t count)
{
	struct fiio_charger_manager *fcm = g_fcm;
	struct charger_desc *desc = fcm->desc;
	unsigned long long val = 0;
	int result;

	result = kstrtoull(buf, 0, &val);
	if (result < 0)
		return result;

	if (val < 100)
		desc->user_set_disable_charge_level = val;
	else
		desc->user_set_disable_charge_level = 100;

	return count;
}

static ssize_t user_set_disable_charge_level_show(struct device *dev,
						  struct device_attribute *attr,
						  char *buf)
{
	struct fiio_charger_manager *fcm = g_fcm;
	struct charger_desc *desc;
	ssize_t count;

	if (!fcm) {
		count = sprintf(buf, "%s\n", "0");
	} else {
		desc = fcm->desc;
		count = sprintf(buf, "%d\n", desc->user_set_disable_charge_level);
	}

	return count;
}

static ssize_t uac_disable_chg_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct fiio_charger_manager *fcm = g_fcm;

	return sprintf(buf, "%d\n", fcm->uac_disable_chg);
}

static ssize_t uac_disable_chg_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t size)
{
	struct fiio_charger_manager *fcm = g_fcm;
	unsigned int val = 0;

	if (!fcm || kstrtouint(buf, 0, &val))
		return size;

	if (val == 0) {
		if (fcm->uac_disable_chg) {
			fcm_printk("[%s] uac_disable_chg false\n", __func__);
			fcm->uac_disable_chg = false;
			if (fcm->power_data && fcm->power_data->set_uac_charging_disabled)
				fcm->power_data->set_uac_charging_disabled(false);
			if (fcm->power_data && fcm->power_data->set_cc_chip_only_device)
				fcm->power_data->set_cc_chip_only_device(false);
		}
	} else if (val == 1 && !fcm->uac_disable_chg) {
		fcm_printk("[%s] uac_disable_chg true\n", __func__);
		fcm->uac_disable_chg = true;
		if (fcm->power_data && fcm->power_data->set_cc_chip_only_device)
			fcm->power_data->set_cc_chip_only_device(true);
		if (fcm->power_data && fcm->power_data->set_uac_charging_disabled)
			fcm->power_data->set_uac_charging_disabled(true);
	}

	return size;
}

static const struct device_attribute charge_manager_attributes[] = {
	__ATTR(user_set_disable_chg, 0644, disable_chg_show, disable_chg_store),
	__ATTR(send_charge_type, 0200, NULL, send_charge_type_store),
	__ATTR(temp_compensation, 0444, temp_compensation_show, NULL),
	__ATTR(fuel_gauge_force_temp, 0644, force_temp_show, force_temp_store),
	__ATTR(fuel_gauge_force_cap, 0200, NULL, force_cap_store),
	__ATTR(user_set_disable_charge_level, 0644,
	       user_set_disable_charge_level_show,
	       user_set_disable_charge_level_store),
	__ATTR(uac_disable_chg, 0644, uac_disable_chg_show, uac_disable_chg_store),
};

static void charge_load_work(void)
{
	struct fiio_charger_manager *fcm = g_fcm;

	fcm_printk("%s: Enter!\n", __func__);
	if (!fcm)
		fcm_printk("%s: fcm = NULL\n", __func__);

	fcm->power_data = fiio_device_get_data_by_name("power_board module");
	if (!fcm->power_data)
		fcm_printk("%s get power_board module failed. \n", __func__);

	fcm->charger_protocol_fiio_module =
		fiio_device_get_data_by_name("charger_protocol_fiio module");
	if (!fcm->charger_protocol_fiio_module)
		fcm_printk("%s get charger_protocol_fiio module failed!\n", __func__);
}

static struct fiio_manager_public_data fiio_charger_d = {
	.is_charging_fiio = false,
};

static struct fiio_device_driver fiio_charge_device_driver = {
	.name = "fiio-charger-manager module",
	.private_data = &fiio_charger_d,
	.load_work = charge_load_work,
};

static int fiio_charger_manager_probe(struct platform_device *pdev)
{
	struct charger_desc *desc = fcm_get_drv_data(pdev);
	struct fiio_charger_manager *fcm;
	int ret;
	struct power_supply_config psy_cfg = {};

	fcm_printk("%s probe enter!\n", __func__);

	if (IS_ERR(desc)) {
		dev_err(&pdev->dev, "No platform data (desc) found\n");
		return PTR_ERR(desc);
	}

	fcm = devm_kzalloc(&pdev->dev, sizeof(*fcm), GFP_KERNEL);
	if (!fcm)
		return -ENOMEM;

	/* Basic Values. Unspecified are Null or 0 */
	fcm->dev = &pdev->dev;
	fcm->desc = desc;
	psy_cfg.drv_data = fcm;
	g_fcm = fcm;

	/* Initialize alarm timer */
	if (alarmtimer_get_rtcdev()) {
		fcm_timer = devm_kzalloc(fcm->dev, sizeof(*fcm_timer), GFP_KERNEL);
		if (!fcm_timer)
			return -ENOMEM;
		alarm_init(fcm_timer, ALARM_BOOTTIME, cm_timer_func);
	}

	/*
	 * Some of the following do not need to be errors.
	 * Users may intentionally ignore those features.
	 */
	if (desc->fullbatt_uV == 0 || desc->fullbatt_uA) {
		dev_info(&pdev->dev, "Ignoring full-battery voltage threshold as it is not supplied\n");
	}
	if (!desc->fullbatt_vchkdrop_ms || !desc->fullbatt_vchkdrop_uV) {
		dev_info(&pdev->dev, "Disabling full-battery voltage drop checking mechanism as it is not supplied\n");
		desc->fullbatt_vchkdrop_ms = 0;
		desc->fullbatt_vchkdrop_uV = 0;
	}
	if (desc->fullbatt_full_capacity == 0) {
		dev_info(&pdev->dev, "Ignoring full-battery full capacity threshold as it is not supplied\n");
	}

	if (!desc->psy_charger_stat || !desc->psy_charger_stat[0]) {
		dev_err(&pdev->dev, "No power supply defined\n");
		return -EINVAL;
	}

	if (!desc->psy_fuel_gauge) {
		dev_err(&pdev->dev, "No fuel gauge power supply defined\n");
		return -EINVAL;
	}

	if (fcm->desc->polling_mode != FCM_POLL_DISABLE &&
	    (desc->polling_interval_ms == 0 ||
	     msecs_to_jiffies(desc->polling_interval_ms) <= FCM_JIFFIES_SMALL)) {
		dev_err(&pdev->dev, "polling_interval_ms is too small\n");
		return -EINVAL;
	}

	if (!desc->charging_max_duration_ms ||
			!desc->discharging_max_duration_ms) {
		dev_info(&pdev->dev, "Cannot limit charging duration checking mechanism to prevent overcharge/overheat and control discharging duration\n");
		desc->charging_max_duration_ms = 0;
		desc->discharging_max_duration_ms = 0;
	}

	platform_set_drvdata(pdev, fcm);

	memcpy(&fcm->charger_psy_desc, &psy_default, sizeof(psy_default));

	if (!desc->psy_name)
		strncpy(fcm->psy_name_buf, psy_default.name, PSY_NAME_MAX);
	else
		strncpy(fcm->psy_name_buf, desc->psy_name, PSY_NAME_MAX);
	fcm->charger_psy_desc.name = fcm->psy_name_buf;

	/* Allocate for psy properties because they may vary */
	fcm->charger_psy_desc.properties = devm_kcalloc(&pdev->dev,
			     ARRAY_SIZE(default_charger_props) +
				NUM_CHARGER_PSY_OPTIONAL,
			     sizeof(enum power_supply_property), GFP_KERNEL);
	if (!fcm->charger_psy_desc.properties)
		return -ENOMEM;

	memcpy((void *)fcm->charger_psy_desc.properties, default_charger_props,
		sizeof(enum power_supply_property) *
		ARRAY_SIZE(default_charger_props));
	fcm->charger_psy_desc.num_properties = ARRAY_SIZE(default_charger_props);

	INIT_DELAYED_WORK(&fcm->fullbatt_vchk_work, fullbatt_vchk);
	INIT_DELAYED_WORK(&fcm->fiio_charger_get_modules_work,
			  fiio_charger_get_modules_work_func);
	INIT_DELAYED_WORK(&fcm->fiio_pump_charge_work, fiio_pump_charge_monitor_work);
	INIT_DELAYED_WORK(&fcm->get_battery_information_work,
			  fiio_get_battery_information_work);
	INIT_DELAYED_WORK(&fcm->boot_complete_work, fiio_boot_complete_work);
	INIT_DELAYED_WORK(&fcm->charger_status_monitor, fiio_charger_status_monitor_work);

	schedule_delayed_work(&fcm->fiio_charger_get_modules_work, msecs_to_jiffies(4000));
	schedule_delayed_work(&fcm->get_battery_information_work, msecs_to_jiffies(15000));
	schedule_delayed_work(&fcm->boot_complete_work, msecs_to_jiffies(15000));
	schedule_delayed_work(&fcm->charger_status_monitor, msecs_to_jiffies(15000));

	fcm->charger_psy = power_supply_register(&pdev->dev,
						 &fcm->charger_psy_desc,
						 &psy_cfg);
	if (IS_ERR(fcm->charger_psy)) {
		dev_err(&pdev->dev, "Cannot register charger-manager with name \"%s\"\n",
			fcm->charger_psy_desc.name);
		return PTR_ERR(fcm->charger_psy);
	}

	/* Add to the list */
	mutex_lock(&fcm_list_mtx);
	list_add(&fcm->entry, &fcm_list);
	mutex_unlock(&fcm_list_mtx);

	/*
	 * Charger-manager is capable of waking up the system from sleep
	 * when event is happened through fcm_notify_event()
	 */
	device_init_wakeup(&pdev->dev, true);
	device_set_wakeup_capable(&pdev->dev, false);

	fcm->charge_public = &fiio_charger_d;
	fiio_device_register(&fiio_charge_device_driver);

	fcm->fiio_vbus_notifier.notifier_call = fiio_vbus_detection_notifier_cb;
	fiio_register_client(notifier_vbus_detection_fiio, &fcm->fiio_vbus_notifier);
	fcm->qti_vbus_notifier.notifier_call = qti_vbus_detection_notifier_cb;
	fiio_register_client(notifier_vbus_detection_qti, &fcm->qti_vbus_notifier);
	fcm->pump_irq_notifier.notifier_call = pump_irq_notifier_cb;
	fiio_register_client(notifier_pump_irq, &fcm->pump_irq_notifier);
	fcm->dmode_irq_notifier.notifier_call = dmode_irq_notifier_cb;
	fiio_register_client(notifier_dmode_irq, &fcm->dmode_irq_notifier);

	desc->is_full = false;
	desc->fcm_bat_temp_state = FCM_BAT_TEMP_NORMAL;
	desc->vbus_plugin_fiio = false;
	desc->qti_fast_charge = false;
	desc->qti_host = false;
	desc->qti_device = false;
	desc->force_temp = 200;
	desc->force_cap = 200;
	desc->poll_count = 0;
	desc->before_charging_color = BLUE;
	desc->after_charging_color = BLUE;
	desc->force_temp_en = false;
	desc->low_battery = false;
	desc->ichrg_limit = 3000000;
	desc->user_set_disable_charge_level = 100;

	fcm->fcm_wakelock = wakeup_source_register(NULL, "fcm_wakelock");

	ret = sysfs_create_group(&fcm->dev->kobj, &fiio_charge_attr_group);
	if (ret < 0) {
		fcm_printk("failed to register sysfs. err: %d\n", ret);
		return ret;
	}

	ret = fiio_device_attribute_register(charge_manager_attributes,
					     ARRAY_SIZE(charge_manager_attributes));
	if (ret) {
		fcm_printk("[%s] attribute register failed!!", __func__);
		return -ENOMEM;
	}

	fcm_printk("%s probe sucessful!\n", __func__);

	return 0;
}

static int fiio_charger_manager_remove(struct platform_device *pdev)
{
	struct fiio_charger_manager *fcm = platform_get_drvdata(pdev);

	fiio_device_attribute_unregister(charge_manager_attributes);

	/* Remove from the list */
	mutex_lock(&fcm_list_mtx);
	list_del(&fcm->entry);
	mutex_unlock(&fcm_list_mtx);

	cancel_work_sync(&setup_polling);
	cancel_delayed_work_sync(&fcm_monitor_work);

	power_supply_unregister(fcm->charger_psy);

	try_charger_enable(fcm, false);

	return 0;
}

static const struct platform_device_id fiio_charger_manager_id[] = {
	{ "fiio_charge_manager", 0 },
	{ },
};
MODULE_DEVICE_TABLE(platform, fiio_charger_manager_id);

static int fcm_suspend_noirq(struct device *dev)
{
	return 0;
}

static int fcm_suspend_prepare(struct device *dev)
{
	return 0;
}

static void fcm_suspend_complete(struct device *dev)
{
	fcm_printk("%s end\n", __func__);
}

static const struct dev_pm_ops fiio_charger_manager_pm = {
	.prepare	= fcm_suspend_prepare,
	.suspend_noirq	= fcm_suspend_noirq,
	.complete	= fcm_suspend_complete,
};

static struct platform_driver fiio_charger_manager_driver = {
	.driver = {
		.name = "fiio-charger-manager",
		.pm = &fiio_charger_manager_pm,
		.of_match_table = fiio_charger_manager_match,
	},
	.probe = fiio_charger_manager_probe,
	.remove = fiio_charger_manager_remove,
	.id_table = fiio_charger_manager_id,
};

static int __init fiio_charger_manager_init(void)
{
	fcm_wq = create_freezable_workqueue("fiio_charger_manager");
	INIT_DELAYED_WORK(&fcm_monitor_work, fcm_monitor_poller);

	return platform_driver_register(&fiio_charger_manager_driver);
}
module_init(fiio_charger_manager_init);

static void __exit fiio_charger_manager_cleanup(void)
{
	destroy_workqueue(fcm_wq);
	fcm_wq = NULL;

	platform_driver_unregister(&fiio_charger_manager_driver);
}
module_exit(fiio_charger_manager_cleanup);

/**
 * fcm_notify_event - charger driver notify Charger Manager of charger event
 * @psy: pointer to instance of charger's power_supply
 * @type: type of charger event
 * @msg: optional message passed to uevent_notify function
 */
void fcm_notify_event(struct power_supply *psy, enum fcm_event_types type,
		      char *msg)
{
	struct fiio_charger_manager *fcm;
	bool found_power_supply = false;

	if (psy == NULL)
		return;

	mutex_lock(&fcm_list_mtx);
	list_for_each_entry(fcm, &fcm_list, entry) {
		if (match_string(fcm->desc->psy_charger_stat, -1,
				 psy->desc->name) >= 0) {
			found_power_supply = true;
			break;
		}
	}
	mutex_unlock(&fcm_list_mtx);

	if (!found_power_supply)
		return;

	switch (type) {
	case FCM_EVENT_BATT_FULL:
		fullbatt_handler(fcm);
		break;
	case FCM_EVENT_BATT_OUT:
		battout_handler(fcm);
		break;
	case FCM_EVENT_BATT_IN:
	case FCM_EVENT_EXT_PWR_IN_OUT ... FCM_EVENT_CHG_START_STOP:
		misc_event_handler(fcm, type);
		break;
	case FCM_EVENT_UNKNOWN:
	case FCM_EVENT_OTHERS:
		uevent_notify(fcm, msg ? msg : default_event_names[type]);
		break;
	default:
		dev_err(fcm->dev, "%s: type not specified\n", __func__);
		break;
	}
}

MODULE_AUTHOR("Aiden");
MODULE_DESCRIPTION("Charger Manager");
MODULE_LICENSE("GPL");
