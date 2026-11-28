/* SPDX-License-Identifier: GPL-2.0 */
/* Layout from the stock DWARF. */
#ifndef _FIIO_CHARGER_PROTOCOL_COMMON_H
#define _FIIO_CHARGER_PROTOCOL_COMMON_H
struct charger_protocol_fiio_data {
	bool pd_type;

	unsigned char (*pps_ajust_power)(unsigned short pdo_vol, unsigned short pdo_cur);
	unsigned char (*fix_ajust_power)(unsigned short pdo_vol, unsigned short pdo_cur);
	unsigned char (*pps_support_judgment)(void);
	int (*get_cc_state)(void);
	void (*charger_iic_power_on)(void);
	int (*charger_iic_test)(void);
	void (*adapter_power_capability)(int i, unsigned int *voltage, unsigned int *maxCurrent);
	void (*set_cc_only_device)(bool enable);
};

struct charger_protocol_qti_data {
	bool pd_type_qti;

	int (*check_power_role_qti)(void);
	int (*check_vbus_qti)(void);
	void (*set_cc_only_device)(bool enable);
};

#endif
