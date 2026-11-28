/* SPDX-License-Identifier: GPL-2.0 */
/* Layout from the stock DWARF. */
#ifndef _FIIO_POWER_BOARD_COMMON_H
#define _FIIO_POWER_BOARD_COMMON_H

#include <linux/types.h>
#include <fiio/led_common.h>

enum power_switch_type {
	POWER_BOARD_BOOT_BAT_TYPE,
	POWER_BOARD_PD_VBUS_PWR_TYPE,
	POWER_BOARD_USB3_VBUS_PWR_TYPE,
};

enum select_power_mode {
	POWER_BOARD_ALL_PWR_OFF_MODE,
	POWER_BOARD_BOOT_BAT_MODE,
	POWER_BOARD_BOOT_BAT_LO_MODE,
	POWER_BOARD_SUPER_OVER_EAR_MODE,
	POWER_BOARD_DESKTOP_MODE,
};

struct power_board_public_data {
	bool is_had_dmode;

	int (*buck_charge_enable)(bool enable);
	int (*pump_charge_enable)(bool enable);
	int (*buck_charger_set_charge_current)(unsigned int cur);
	int (*buck_charger_set_limit_current)(unsigned int cur);
	int (*pump_get_ibus)(void);
	int (*pump_get_vbus)(void);
	int (*set_charge_status_indicator)(enum led_status color);
	int (*get_charge_status_indicator)(void);

	unsigned char (*pps_modulation_power)(unsigned short pdo_vol, unsigned short pdo_cur);
	unsigned char (*fix_modulation_power)(unsigned short pdo_vol, unsigned short pdo_cur);

	bool (*check_desktop_mode)(void);
	void (*set_mode_power_ctrl)(enum select_power_mode mode, bool enable);
	void (*boot_or_vbus_negative_voltage_sw)(enum power_switch_type type, bool enable);
	void (*boot_bat_or_vbus_voltage_sw)(enum power_switch_type type, bool enable);
	void (*get_adapter_power_capability)(int i, unsigned int *voltage, unsigned int *maxCurrent);

	int (*compensated_battery_temperature)(void);
	int (*slope_limit_value)(void);
	int (*get_charger_cc_state_fiio)(void);
	int (*get_charger_power_role_qti)(void);
	int (*get_charger_vbus_qti)(void);
	int (*set_uac_charging_disabled)(bool enable);
	void (*set_cc_chip_only_device)(bool enable);
};

#endif
