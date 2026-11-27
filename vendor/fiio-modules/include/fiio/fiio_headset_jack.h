/* SPDX-License-Identifier: GPL-2.0 */
/* Layout from the stock DWARF. */
#ifndef _FIIO_FIIO_HEADSET_JACK_H
#define _FIIO_FIIO_HEADSET_JACK_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/iio/consumer.h>
#include <linux/workqueue.h>
#include <linux/notifier.h>

static const char *output_name[9] = {
	"single po", "single lo", "spdif out", "balance po", "balance lo",
	"pre po", "pre lo", "pre all", "none out",
};

static const char *lopo_mode_setting_name[5] = {
	"PO", "LO", "SPDIF", "BAL.PO", "BAL.LO",
};

enum fiio_alarm_type {
	NO_ALARM,
	DC_DET_ERROR,
	OVERLOAD_DET_ERROR,
	TEMP_DET_ERROR,
};

struct headset_jack_public_data {
	bool allow_dc_det;
	bool allow_overload_det;
	bool hs35_det_gpio_disable;
	bool allow_thx_therm_det;
};

struct headset_data {
	struct device *dev;
	struct iio_channel *dc_det1;
	struct iio_channel *dc_det2;
	struct iio_channel *bal25_det_adc;
	struct iio_channel *hs35_det_adc;
	struct iio_channel *thx_therm_det;
	int hs35_det_gpio;
	int bal44_det_gpio;
	int po635_det_gpio;
	int dc_det1_io;
	int dc_det2_io;
	int overload_det;
	int hs_det_delay_ms;
	int hs35_det_threshold;
	int bal25_det_threshold;
	int bal44_det_threshold;
	int po635_det_threshold;
	int dc_det1_threshold;
	int dc_det2_threshold;
	int overload_det_threshold;
	int thx_therm_det_alert_threshold;
	int thx_therm_det_shutdown_threshold;
	int thx_therm_det_alert_to_normal_threshold;
	int po_dc_det_max_threshold;
	int po_dc_det_min_threshold;
	int po_dc_det_thread_value;
	int bal_dc_det_max_threshold;
	int bal_dc_det_min_threshold;
	int bal_dc_det_thread_value;
	int dc_det_filter_count;
	int overload_det_filter_count;
	int bal25in_det_filter_count;
	int bal25out_det_filter_count;
	int hs35in_det_filter_count;
	int hs35out_det_filter_count;
	int bal44in_det_filter_count;
	int bal44out_det_filter_count;
	int hs635in_det_filter_count;
	int hs635out_det_filter_count;
	int thx_therm_det_filter_count;
	int hs_valid;
	int pre_hs_valid;
	int headset_state;
	int headset_state_pre;
	int headset_type;
	int headset_mode;
	int balance_mode;
	int output_type;
	struct delayed_work jack_work;
	struct delayed_work dc_det_work;
	struct delayed_work overload_det_work;
	struct delayed_work thx_therm_det_work;
	struct delayed_work headset_notify_work;
	struct notifier_block dac_event_notifier;
	bool dc_det1_state;
	bool dc_det2_state;
	bool overload_det_state;
	bool dc_det_en;
	bool overload_det_en;
	bool thx_therm_det_en;
	bool thx_therm_det_alert_en;
	bool is_high_voltage_det_hs35;
	bool force_hs35;
	bool force_bal25;
	bool force_bal44;
	bool force_po635;
	struct delayed_work force_jack_work;
	struct notifier_block user_config_event_notifier;
	struct notifier_block output_mode_event_notifier;
	struct headset_jack_public_data *data_private;
};

#endif
