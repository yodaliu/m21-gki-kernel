/* SPDX-License-Identifier: GPL-2.0 */
/* Layout from the stock DWARF. */
#ifndef _FIIO_ANOLOG_BOARD_COMMON_H
#define _FIIO_ANOLOG_BOARD_COMMON_H

#include <linux/types.h>
#include <fiio/fiio_notify.h>

struct anolog_board_public_data {
	int dsd_unmute_delaytime;
	int pcm_unmute_delaytime;
	bool is_muting;
	bool is_unmuting;
	bool is_muted;
	bool isNeedPowerChange;
	bool isNeedMute;
	bool is_spdif_tx_enable;
	bool is_pause_by_headset;
	enum fiio_output_mode_t audio_output_mode;
	int current_output_type;
	int new_output_type;
	bool isPlaying;
	void (*update_volume)(void);
	void (*set_volume_db)(unsigned int, unsigned int);
	void (*control_all_power)(bool);
	void (*hw_mute)(int);
	void (*mute_all)(int);
	void (*hw_params)(void);
	int (*get_output_type)(void);
	int (*get_output_gain)(void);
	int (*get_output_volume)(void);
};

#endif
