/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FIIO_CHARGE_CHIP_COMMON_H
struct buck_charger_data {
	int (*specific_buck_charger_start_charge)(bool enable);
	int (*specific_buck_charger_enabled)(bool enable);
	int (*specific_buck_charger_set_charge_current)(unsigned int cur);
	int (*specific_buck_charger_set_limit_current)(unsigned int limit_cur);
	int (*specific_buck_otg_enable)(bool enable);
	int (*specific_buck_charger_iic_test)(void);
	void (*specific_buck_charger_set_uac_mode)(bool enable_uac);

	bool (*specific_buck_check_charging)(void);
};
#define _FIIO_CHARGE_CHIP_COMMON_H
#endif
