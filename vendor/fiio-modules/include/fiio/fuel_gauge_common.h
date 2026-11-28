/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FIIO_FUEL_GAUGE_COMMON_H
#define _FIIO_FUEL_GAUGE_COMMON_H
struct fuel_gauge_data {
	int (*specific_fuel_gauge_get_bat_cap)(void);
	int (*specific_fuel_gauge_get_bat_cur)(void);
	int (*specific_fuel_gauge_get_bat_val)(void);
	int (*specific_fuel_gauge_get_bat_temp)(void);
	int (*specific_fuel_gauge_iic_test)(void);
};

#endif
