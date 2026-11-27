/* SPDX-License-Identifier: GPL-2.0 */
/* Layout from the stock DWARF. */
#ifndef _FIIO_DAC_CORE_H
#define _FIIO_DAC_CORE_H

#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/notifier.h>
#include <linux/pm_wakeup.h>
#include <sound/soc.h>
#include <fiio/dac_dai.h>
#include <fiio/dac_common.h>
#include <fiio/xmos_common.h>
#include <fiio/anolog_board_common.h>
#include <fiio/mode_select_common.h>
#include <fiio/led_common.h>
#include <fiio/volume_common.h>
#include <fiio/fiio_headset_jack.h>

struct snd_dac {
	const char *name;
	struct snd_dac_dai *dac_dai;
	int num_dai;
	bool is_wakeup_lock;
	struct wakeup_source *audiolink_wakelock;
	struct snd_soc_jack headset_jack;
	int save_guide_counter;
	int lopo_mode_setting;
	int headset_state;
	int headset_type;
	struct delayed_work snd_dac_change_output_mode_work;
	struct delayed_work snd_dac_volume_fade_in_work;
	struct delayed_work snd_dac_poweroff_delay_work;
	struct delayed_work snd_dac_save_guide_work;
	struct delayed_work snd_dac_dsd_unmute_delay_work;
	struct delayed_work snd_dac_pcm_unmute_delay_work;
	struct notifier_block headset_notifier;
	struct notifier_block player_notifier;
	struct notifier_block output_mode_event_notifier;
	struct notifier_block monitor_notifier;
	bool player_already_startup;
	bool is_playing_now;
	bool snd_trigger;
	bool is_local_playing;
	bool is_dac_mute;
	int dac_read_reg;
	struct dac_public_data *dac_public;
	struct xmos_public_data *xmos_module;
	struct anolog_board_public_data *anolog_board_module;
	struct mode_select_public_data *mode_select_module;
	struct aw22xxx_public_data *aw22xxx_module;
	struct volume_public_data *volume_module;
	struct headset_jack_public_data *headset_jack_module;
};

#endif
