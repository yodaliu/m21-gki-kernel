/* SPDX-License-Identifier: GPL-2.0 */
/*
 * FiiO external key (the detachable tape/keypad accessory): one GPIO hold key,
 * a hall sensor that detects the accessory and an ADC resistor ladder for
 * its media keys.
 */
#ifndef _FIIO_EXTERNEL_KEY_H
#define _FIIO_EXTERNEL_KEY_H

#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/iio/consumer.h>

/* ADC readings (uV) of the accessory keys; above EXT_KEY_ADC_RELEASE nothing is pressed */
#define EXT_KEY_ADC_RELEASE		1700000
#define EXT_KEY_ADC_VOLUMEUP_MIN	1529000
#define EXT_KEY_ADC_VOLUMEUP_MAX	1689000
#define EXT_KEY_ADC_VOLUMEDOWN_MIN	1363000
#define EXT_KEY_ADC_VOLUMEDOWN_MAX	1523000
#define EXT_KEY_ADC_NEXTSONG_MIN	1170000
#define EXT_KEY_ADC_NEXTSONG_MAX	1330000
#define EXT_KEY_ADC_PREVIOUSSONG_MIN	983000
#define EXT_KEY_ADC_PREVIOUSSONG_MAX	1143000
#define EXT_KEY_ADC_PLAYPAUSE_MIN	805000
#define EXT_KEY_ADC_PLAYPAUSE_MAX	965000

/* det_work polls of a held key before it counts as a long press */
#define EXT_KEY_LONG_PRESS_COUNT	20
#define EXT_KEY_MY_FAV_COUNT		66

#define EXT_KEY_DET_PERIOD_MS		15
#define EXT_KEY_HOLD_LONGEXIT_MS	1000
#define EXT_KEY_HOLD_LONGSHUT_MS	5000

struct ext_key_data {
	struct platform_device *pdev;
	struct device *dev;
	struct delayed_work hold_irq_work;
	struct delayed_work hall_irq_work;
	struct delayed_work det_work;
	struct delayed_work long_press_work1;
	struct delayed_work long_press_work2;
	struct iio_channel *adc_det;
	int detsw_en;
	int outside_hold;
	int hall_det;
	int downtime_count;
	int uptime_count;
	int report_key;
	int gpio_irq;
	int hall_irq;
	int mark_key;
	int tape_auto_det_en;
	int tape_detected;
	/* 0 released, 1 pressed, 2 double click reported */
	int key_down;

	bool no_double_click;
	bool gpio_key_down;
	bool long_press_happen;
	bool hold_status;
	bool hall_status;
	bool adc_key_long_press;
};

#endif
