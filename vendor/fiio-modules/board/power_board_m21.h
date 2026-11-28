/* SPDX-License-Identifier: GPL-2.0 */
/* Layout from the stock DWARF. */
#ifndef _POWER_BOARD_M21_H
#define _POWER_BOARD_M21_H

#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <fiio/power_board_common.h>
#include <fiio/charge_chip_common.h>
#include <fiio/charger_protocol_common.h>
#include <fiio/led_common.h>
#include <fiio/anolog_board_common.h>
#include <fiio/fuel_gauge_common.h>

enum machine_state {
	CHARGING,
	DISCHARGING,
	DESKTOP_MODE,
	CHARGING_AND_PLAYING_500mV,
	CHARGING_AND_PLAYING_3000mV,
	CHARGING_AND_PLAYING_4600mV,
	BATTERY_TEMP_SUPER_COOL,
	OTHER_STATE,
};

enum gpiop_num {
	DC_BAT_SW,
	PD_VBUS_SW,
	OTG_SEL,
	BATTERY_ID,
	OTG_EN,
	GPIO_MAX_NUM,
};

char *gpiop_name[GPIO_MAX_NUM] = {
	"gpio,dc_bat_sw",
	"gpio,pd_vbus_sw",
	"gpio,otg_sel",
	"gpio,battery_id",
	"gpio,otg_en",
};

struct power_board_priv {
	int dc_bat_sw;
	int pd_voltage_gear;
	unsigned int gpio_ctrl[GPIO_MAX_NUM];
	struct device *dev;

	struct mutex lock;

	struct delayed_work power_get_module_work;
	struct delayed_work sw_event_work;
	struct delayed_work battery_temp_update_work;
	struct delayed_work otg_work;
	struct delayed_work boot_complete_work;
	struct delayed_work fast_charger_pdo_change_work;

	bool power_in_vbus;
	bool usb_host;
	bool usb_device;
	bool otg_enabled;
	bool is_desktop_mode;
	bool is_charging;

	struct notifier_block fiio_host_nb;
	struct notifier_block qti_host_nb;
	struct notifier_block extcon_nb;

	struct power_board_public_data *power_board_public;

	struct buck_charger_data *buck_charger_module;
	struct charger_protocol_fiio_data *charger_protocol_fiio_module;
	struct charger_protocol_qti_data *charger_protocol_qti_module;
	struct led_public_data *led_module;
	struct anolog_board_public_data *anolog_board_module;
	struct fuel_gauge_data *fuel_gauge_module;

	struct regulator_dev *otg_rdev;
	struct extcon_dev *extcon_dev;

	enum machine_state power_machine_state;

	int only_charging_temp_compensation;
	int low_load_temp_compensation;
	int medium_load_temp_compensation;
	int high_load_temp_compensation;
	int default_temp_compensation;

	int fast_speed_slope_coefficient;
	int medium_speed_slope_coefficient;
	int low_speed_slope_coefficient;
	int super_low_speed_slope_coefficient;
	int default_speed_slope_coefficient;

	int batt_temp_compensation_value;
	int batt_ajust_temp;

	int slope_select;
	int update_temp_count;
	bool uac_charging_disabled;
};

#endif
