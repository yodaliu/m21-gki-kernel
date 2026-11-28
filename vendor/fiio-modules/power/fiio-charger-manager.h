/* SPDX-License-Identifier: GPL-2.0 */
/*
 * FiiO charger manager, derived from drivers/power/supply/charger-manager.c.
 * Layouts from the stock DWARF.
 */
#ifndef _FIIO_CHARGER_MANAGER_H
#define _FIIO_CHARGER_MANAGER_H

#include <linux/power_supply.h>
#include <linux/extcon.h>
#include <linux/alarmtimer.h>
#include <fiio/led_common.h>

enum data_source {
	FCM_BATTERY_PRESENT,
	FCM_NO_BATTERY,
	FCM_FUEL_GAUGE,
	FCM_CHARGER_STAT,
};

enum polling_modes {
	FCM_POLL_DISABLE = 0,
	FCM_POLL_ALWAYS,
	FCM_POLL_EXTERNAL_POWER_ONLY,
	FCM_POLL_CHARGING_ONLY,
};

enum fcm_event_types {
	FCM_EVENT_UNKNOWN = 0,
	FCM_EVENT_BATT_FULL,
	FCM_EVENT_BATT_IN,
	FCM_EVENT_BATT_OUT,
	FCM_EVENT_BATT_OVERHEAT,
	FCM_EVENT_BATT_COLD,
	FCM_EVENT_EXT_PWR_IN_OUT,
	FCM_EVENT_CHG_START_STOP,
	FCM_EVENT_OTHERS,
};

enum fcm_bat_temp_state_levels {
	FCM_BAT_TEMP_SUPER_COOL,
	FCM_BAT_TEMP_COLD,
	FCM_BAT_TEMP_COOLER,
	FCM_BAT_TEMP_COOL,
	FCM_BAT_TEMP_NORMAL,
	FCM_BAT_TEMP_WARM,
	FCM_BAT_TEMP_HOT,
	FCM_BAT_TEMP_SUPER_HEAT,
};

struct charger_pdo {
	int pdo_voltage;
	int pdo_current;
};

struct charger_desc {
	const char *psy_name;

	enum polling_modes polling_mode;
	unsigned int polling_interval_ms;

	unsigned int fullbatt_vchkdrop_ms;
	unsigned int fullbatt_vchkdrop_uV;
	unsigned int fullbatt_uV;
	unsigned int fullbatt_uA;
	unsigned int fullbatt_full_capacity;

	enum data_source battery_present;

	const char **psy_charger_stat;

	const char *psy_fuel_gauge;

	const char *thermal_zone;

	int temp_low_extreme;
	int temp_low_stop;
	int temp_low_cold_limit;
	int temp_low_cool_limit;
	int temp_normal;
	int temp_hight_limit;
	int temp_hight_stop;
	int temp_diff;
	int temp_thrd;

	int bat_temp;
	int bat_capacity;
	enum fcm_bat_temp_state_levels fcm_bat_temp_state;
	enum led_status before_charging_color;
	enum led_status after_charging_color;

	bool measure_battery_temp;
	bool fcm_bat_temp_state_changed;
	bool pd_type;
	bool has_pump;
	bool pps_enable;
	bool is_full;
	bool is_desktop_mode;
	bool vbus_plugin_fiio;
	bool disable_charge;
	bool qti_fast_charge;
	bool qti_host;
	bool qti_device;

	u32 charging_max_duration_ms;
	u32 discharging_max_duration_ms;

	unsigned int ichrg_current;
	unsigned int ichrg_limit;
	unsigned int ichg_cold;
	unsigned int ichg_cool;
	unsigned int ichg_normal;
	unsigned int ichg_warm;
	unsigned int ichg_hot;

	unsigned int ichg_fast;
	unsigned int ichg_unknow;

	unsigned int fix_num;
	unsigned int fix_ichg;
	unsigned int fix_num_old;

	unsigned int fix_expect_the_power_gear;

	int wq_val;
	int wq_cur;
	int sink_req_cur;
	int sink_req_val;
	int sink_req_max_val;
	int sink_req_max_cur;
	int sink_tar_cur;
	int sink_tar_max_cur;
	int pump_amend_value;
	int pump_amend_value_start;
	int pump_amend_value_max;
	int pps_boost_voltage_step;
	int pps_drop_voltage_step;
	int pump_cc_off_current;
	int pump_normal_temp_control;
	int pump_charge_low_voltage;
	int pump_charge_hight_voltage;
	int pps_voltage_step_mV;
	int pump_charge_voltage;
	int poll_count;

	int temp_adjustment;

	int user_set_disable_charge_level;

	const char *temp_compensation_enable;

	int force_temp;
	int force_cap;
	bool force_temp_en;
	bool low_battery;
};

#define PSY_NAME_MAX	30

struct fiio_charger_manager {
	struct list_head entry;
	struct device *dev;
	struct charger_desc *desc;

	struct thermal_zone_device *tzd_batt;

	bool charger_enabled;
	bool user_set_disable_chg;
	bool uac_disable_chg;

	unsigned long fullbatt_vchk_jiffies_at;

	struct delayed_work fullbatt_vchk_work;
	struct delayed_work fiio_charger_get_modules_work;
	struct delayed_work fiio_pump_charge_work;
	struct delayed_work fiio_get_psy_work;
	struct delayed_work get_battery_information_work;
	struct delayed_work boot_complete_work;
	struct delayed_work charger_status_monitor;

	int emergency_stop;

	char psy_name_buf[PSY_NAME_MAX + 1];
	struct power_supply_desc charger_psy_desc;
	struct power_supply *charger_psy;

	u64 charging_start_time;
	u64 charging_end_time;

	struct power_board_public_data *power_data;
	struct charger_protocol_fiio_data *charger_protocol_fiio_module;
	struct buck_charger_data *buck_charger_module;
	struct fuel_gauge_data *fuel_gauge_module;
	struct fiio_manager_public_data *charge_public;

	struct notifier_block fiio_vbus_notifier;
	struct notifier_block pump_irq_notifier;
	struct notifier_block dmode_irq_notifier;
	struct notifier_block qti_vbus_notifier;

	struct wakeup_source *fcm_wakelock;
};

void fcm_notify_event(struct power_supply *psy, enum fcm_event_types type,
		      char *msg);

#endif
