/* SPDX-License-Identifier: GPL-2.0 */
/* Layout from the stock DWARF. */
#ifndef _FIIO_VOLUME_COMMON_H
#define _FIIO_VOLUME_COMMON_H

#include <linux/types.h>

enum volume_nju_status {
	NJU_POWER_OFF,
	NJU_NEED_CONFIG,
	NJU_LOCKED,
};

struct volume_public_data {
	int on;
	int volumeL_DB;
	int volumeR_DB;
	void (*init)(void);
	void (*set_volume_db)(struct volume_public_data *, unsigned int, unsigned int);
	int (*nju_iic_test)(void);
};

#endif
