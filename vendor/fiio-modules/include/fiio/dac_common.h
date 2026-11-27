/* SPDX-License-Identifier: GPL-2.0 */
/* Layout from the stock DWARF. */
#ifndef _FIIO_DAC_COMMON_H
#define _FIIO_DAC_COMMON_H

#include <linux/types.h>

struct dac_public_data {
	bool power_state;
	bool playing_state;
	bool mute_state;
	void (*dac_startup)(void);
	void (*dac_shutdown)(bool, int);
	void (*dac_hw_params)(int, int, int, int);
	void (*dac_mute)(bool);
	void (*dac_sw_mute)(bool);
	void (*dac_shutdown_now)(void);
};

#endif
