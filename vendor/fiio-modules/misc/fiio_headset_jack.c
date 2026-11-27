// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO headset jack detect driver.
 * Reconstructed from the stock FiiO M21 module.
 */
#define pr_fmt(fmt) "[fiio_headset]: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/iio/consumer.h>
#include <linux/workqueue.h>
#include <linux/kobject.h>
#include <fiio/fiio_device.h>
#include <fiio/fiio_attr.h>
#include <fiio/fiio_notify.h>
#include <fiio/fiio_headset_jack.h>

static struct headset_data *g_data;

static int dynamic_debug_modes;
module_param(dynamic_debug_modes, int, 0644);
MODULE_PARM_DESC(dynamic_debug_modes, "dynamic debug modes");

static void update_headset_type(struct headset_data *data)
{
	switch (data->output_type) {
	case 0:
		data->headset_type = 0x01;
		break;
	case 1:
		data->headset_type = 0x02;
		break;
	case 2:
		data->headset_type = 0x04;
		break;
	case 3:
		if ((data->headset_state & 0xc) == 0xc)
			data->headset_type = 0x28;
		else if (data->headset_state & 0x4)
			data->headset_type = 0x20;
		else if (data->headset_state & 0x8)
			data->headset_type = 0x08;
		break;
	case 4:
		if ((data->headset_state & 0xc) == 0xc)
			data->headset_type = 0x50;
		else if (data->headset_state & 0x4)
			data->headset_type = 0x40;
		else if (data->headset_state & 0x8)
			data->headset_type = 0x10;
		break;
	case 5:
	case 6:
	case 7:
		if ((data->headset_state & 0xa) == 0xa)
			data->headset_type = 0x88;
		else if (data->headset_state & 0x8)
			data->headset_type = 0x08;
		else if (data->headset_state & 0x2)
			data->headset_type = 0x80;
		else
			data->headset_type = 0;
		break;
	default:
		data->headset_type = 0;
		break;
	}
}

static void update_output_type(struct headset_data *data)
{
	if (!data)
		return;

	switch (data->output_type) {
	case 5:
	case 6:
	case 7:
		break;
	default:
		if (data->hs_valid == 0xc)
			data->output_type = data->balance_mode;
		else if (data->hs_valid == 3)
			data->output_type = data->headset_mode;
		else
			data->output_type = 8;
		break;
	}
	update_headset_type(data);
}

static void report_event_info(struct headset_data *data, char *buf1, char *buf2)
{
	struct device *dev = data->dev;
	char *envp[4];
	char *event_name = "KERNELEVENT=change";

	envp[0] = event_name;
	envp[1] = buf1;
	envp[2] = buf2;
	envp[3] = NULL;
	kobject_uevent_env(&dev->kobj, KOBJ_CHANGE, envp);
}

void report_headset_type_state(struct headset_data *data, int headset_type, int headset_state)
{
	char buf1[20] = { 0 };
	char buf2[20] = { 0 };

	sprintf(buf1, "HEADSET_TYPE=%d", headset_type);
	sprintf(buf2, "HEADSET_STATE=%d", headset_state);
	report_event_info(data, buf1, buf2);
}

static void report_alarm_info(struct headset_data *data, int alarm_type, int alarm_value)
{
	char buf1[20] = { 0 };
	char buf2[20] = { 0 };

	sprintf(buf1, "ALARM_TYPE=%d", alarm_type);
	sprintf(buf2, "ALARM_VALUE=%d", alarm_value);
	report_event_info(data, buf1, buf2);
	pr_err("%s: alarm_type = %d alarm_value = %d\n", __func__, alarm_type, alarm_value);
}

static void headset_status_notifier_call(struct headset_data *data)
{
	struct headset_notify_data notify_data;

	notify_data.event = HEADSET_STATE_CHANGE | HEADSET_OUTPUT_TYPE_CHANGE;
	notify_data.headset_state = data->headset_state;
	notify_data.output_type = data->output_type;
	notify_data.headset_type = data->headset_type;
	fiio_notifier_call_chain(notifier_headset, 0, &notify_data);
}

static void monitor_notifier_call(enum monitor_event_type event)
{
	struct monitor_notify_data notify_data;

	notify_data.event = event;
	fiio_notifier_call_chain(notifier_monitor, 0, &notify_data);
}

static int fiio_headset_jack_parse_dt(struct headset_data *data)
{
	struct device *dev = data->dev;
	struct device_node *np = dev->of_node;
	struct pinctrl *adc_pinctrl;
	struct pinctrl *gpio_pinctrl;
	int ret;
	int value;

	data->overload_det_en = false;
	data->thx_therm_det_en = false;
	value = 0;

	if (!np) {
		pr_info("[%s] np NULL\n", __func__);
		return -EINVAL;
	}

	gpio_pinctrl = pinctrl_get_select(dev, "det_gpio_init");
	if (IS_ERR(gpio_pinctrl))
		pr_info("[%s] det_gpio_init pinctrl not found\n", __func__);
	else
		pinctrl_put(gpio_pinctrl);

	adc_pinctrl = pinctrl_get_select(dev, "adc_gpio_init");
	if (IS_ERR(adc_pinctrl))
		pr_info("[%s] adc_gpio_init pinctrl not found\n", __func__);
	else
		pinctrl_put(adc_pinctrl);

	ret = of_property_read_u32_index(np, "hs-det-delay-ms", 0, &value);
	if (!ret) {
		data->hs_det_delay_ms = value;
		pr_err("[%s] hs_det_delay_ms = %d\n", __func__, data->hs_det_delay_ms);
	} else {
		data->hs_det_delay_ms = 166;
		pr_err("[%s] default value hs_det_delay_ms = %d\n", __func__, data->hs_det_delay_ms);
	}

	data->bal25_det_adc = devm_iio_channel_get(dev, "bal25_det");
	if (IS_ERR(data->bal25_det_adc)) {
		pr_info("[%s] bal25_det_adc get failed!\n", __func__);
		data->bal25_det_adc = NULL;
	} else {
		pr_err("[%s] bal25_det_adc get success!\n", __func__);
	}

	ret = of_property_read_u32_index(np, "bal25-det-threshold", 0, &value);
	if (!ret) {
		data->bal25_det_threshold = value;
		pr_err("[%s] bal25-det-threshold = %d\n", __func__, data->bal25_det_threshold);
	} else {
		pr_info("[%s] bal25-det-threshold get failed!\n", __func__);
		data->bal25_det_threshold = 0;
	}

	data->hs35_det_gpio = of_get_named_gpio(np, "hs35_det_gpio", 0);
	if (gpio_is_valid(data->hs35_det_gpio)) {
		ret = gpio_request(data->hs35_det_gpio, "hs35_det_gpio");
		if (ret < 0) {
			pr_info("[%s] hs35_det_gpio (%d) gpio request fail\n", __func__,
				data->hs35_det_gpio);
			return -EINVAL;
		}
		gpio_direction_input(data->hs35_det_gpio);
		pr_err("[%s] hs35_det_gpio :  %d\n", __func__, gpio_get_value(data->hs35_det_gpio));
	}

	data->hs35_det_adc = devm_iio_channel_get(dev, "hs35_det");
	if (IS_ERR(data->hs35_det_adc)) {
		pr_info("[%s] hs35_det_adc get failed!\n", __func__);
		data->hs35_det_adc = NULL;
	} else {
		pr_err("[%s] hs35_det_adc get success!\n", __func__);
	}

	ret = of_property_read_u32_index(np, "hs35-det-threshold", 0, &value);
	if (!ret) {
		data->hs35_det_threshold = value;
		pr_err("[%s] hs35-det-threshold = %d\n", __func__, data->hs35_det_threshold);
	} else {
		pr_info("[%s] hs35-det-threshold get failed!\n", __func__);
		data->hs35_det_threshold = 0;
	}

	ret = of_property_read_u32_index(np, "hs35-high-voltage-det", 0, &value);
	if (!ret) {
		data->is_high_voltage_det_hs35 = (value == 1);
		pr_err("[%s] hs35_det_threshold = %d\n", __func__, data->is_high_voltage_det_hs35);
	} else {
		pr_info("[%s] is_high_voltage_det_hs35 get failed!\n", __func__);
		data->is_high_voltage_det_hs35 = false;
	}

	data->bal44_det_gpio = of_get_named_gpio(np, "bal44_det", 0);
	if (gpio_is_valid(data->bal44_det_gpio)) {
		ret = gpio_request(data->bal44_det_gpio, "bal44_det_gpio");
		if (ret < 0) {
			pr_info("[%s] bal44_det_gpio (%d) gpio request fail\n", __func__,
				data->bal44_det_gpio);
			return -EINVAL;
		}
		gpio_direction_input(data->bal44_det_gpio);
		pr_err("[%s] bal44_det_gpio :  %d\n", __func__, gpio_get_value(data->bal44_det_gpio));
	}

	ret = of_property_read_u32_index(np, "bal44-det-threshold", 0, &value);
	if (!ret) {
		data->bal44_det_threshold = value;
		pr_err("[%s] bal44-det-threshold = %d\n", __func__, data->bal44_det_threshold);
	} else {
		pr_info("[%s] bal44-det-threshold get failed!\n", __func__);
		data->bal44_det_threshold = 0;
	}

	data->po635_det_gpio = of_get_named_gpio(np, "po635_det", 0);
	if (gpio_is_valid(data->po635_det_gpio)) {
		ret = gpio_request(data->po635_det_gpio, "po635_det_gpio");
		if (ret < 0) {
			pr_info("[%s] po635_det_gpio (%d) gpio request fail\n", __func__,
				data->po635_det_gpio);
			return -EINVAL;
		}
		gpio_direction_input(data->po635_det_gpio);
		pr_err("[%s] po635_det_gpio :  %d\n", __func__, gpio_get_value(data->po635_det_gpio));
	} else {
		data->po635_det_gpio = -1;
	}

	ret = of_property_read_u32_index(np, "po635-det-threshold", 0, &value);
	if (!ret) {
		data->po635_det_threshold = value;
		pr_err("[%s] po635-det-threshold = %d\n", __func__, data->po635_det_threshold);
	} else {
		pr_info("[%s] po635-det-threshold get failed!\n", __func__);
		data->po635_det_threshold = 0;
	}

	data->dc_det1 = devm_iio_channel_get(dev, "dc_det1");
	if (IS_ERR(data->dc_det1)) {
		pr_info("[%s] dc_det1 get failed!\n", __func__);
		data->dc_det1 = NULL;
	}

	data->dc_det2 = devm_iio_channel_get(dev, "dc_det2");
	if (IS_ERR(data->dc_det2)) {
		pr_info("[%s] dc_det2 get failed!\n", __func__);
		data->dc_det2 = NULL;
	}

	data->dc_det1_io = of_get_named_gpio(np, "dc_det1_io", 0);
	if (gpio_is_valid(data->dc_det1_io)) {
		ret = gpio_request(data->dc_det1_io, "dc_det1_gpio");
		if (ret < 0) {
			pr_info("[%s] dc_det1_io (%d) gpio request fail\n", __func__,
				data->dc_det1_io);
			return -EINVAL;
		}
		gpio_direction_input(data->dc_det1_io);
		pr_err("[%s] dc_det1_io :  %d\n", __func__, gpio_get_value(data->dc_det1_io));
	}

	ret = of_property_read_u32_index(np, "dc-det1-threshold", 0, &value);
	if (!ret) {
		data->dc_det1_threshold = value;
		pr_err("[%s] dc_det1_threshold = %d\n", __func__, data->dc_det1_threshold);
	} else {
		pr_info("[%s] dc_det1_threshold get failed!\n", __func__);
		data->dc_det1_threshold = 0;
	}

	data->dc_det2_io = of_get_named_gpio(np, "dc_det2_io", 0);
	if (gpio_is_valid(data->dc_det2_io)) {
		ret = gpio_request(data->dc_det2_io, "dc_det2_gpio");
		if (ret < 0) {
			pr_info("[%s] dc_det2_io (%d) gpio request fail\n", __func__,
				data->dc_det2_io);
			return -EINVAL;
		}
		gpio_direction_input(data->dc_det2_io);
		pr_err("[%s] dc_det2_io :  %d\n", __func__, gpio_get_value(data->dc_det2_io));
	}

	ret = of_property_read_u32_index(np, "dc-det2-threshold", 0, &value);
	if (!ret) {
		data->dc_det2_threshold = value;
		pr_err("[%s] dc_det2_threshold = %d\n", __func__, data->dc_det2_threshold);
	} else {
		pr_info("[%s] dc_det2_threshold get failed!\n", __func__);
		data->dc_det2_threshold = 0;
	}

	data->overload_det = of_get_named_gpio(np, "overload_det", 0);
	if (gpio_is_valid(data->overload_det)) {
		ret = gpio_request(data->overload_det, "overload_det_gpio");
		if (ret < 0) {
			pr_info("[%s] overload_det (%d) gpio request fail\n", __func__,
				data->overload_det);
			return -EINVAL;
		}
		data->overload_det_en = true;
		gpio_direction_input(data->overload_det);
		pr_err("[%s] overload_det :  %d\n", __func__, gpio_get_value(data->overload_det));
	}

	ret = of_property_read_u32_index(np, "overload-det-threshold", 0, &value);
	if (!ret) {
		data->overload_det_threshold = value;
		pr_err("[%s] overload_det_threshold = %d\n", __func__, data->overload_det_threshold);
	} else {
		pr_info("[%s] overload_det_threshold get failed!\n", __func__);
		data->overload_det_threshold = 0;
	}

	ret = of_property_read_u32_index(np, "thx-therm-det-en", 0, &value);
	if (!ret) {
		if (value == 1)
			data->thx_therm_det_en = true;
	} else {
		pr_info("[%s] thx_therm_det_en get failed!\n", __func__);
		data->thx_therm_det_en = false;
	}

	data->thx_therm_det = devm_iio_channel_get(dev, "thx_therm_det");
	if (IS_ERR(data->thx_therm_det)) {
		pr_info("[%s] thx_therm_det get failed!\n", __func__);
		data->thx_therm_det = NULL;
	}

	ret = of_property_read_u32_index(np, "thx-therm-det-alert", 0, &value);
	if (!ret) {
		data->thx_therm_det_alert_threshold = value;
		pr_err("[%s] thx_therm_det_alert_threshold = %d\n", __func__,
		       data->thx_therm_det_alert_threshold);
	} else {
		pr_info("[%s] thx_therm_det_alert_threshold get failed!\n", __func__);
		data->thx_therm_det_alert_threshold = 0;
	}

	ret = of_property_read_u32_index(np, "thx-therm-det-shutdown", 0, &value);
	if (!ret) {
		data->thx_therm_det_shutdown_threshold = value;
		pr_err("[%s] thx_therm_det_shutdown_threshold = %d\n", __func__,
		       data->thx_therm_det_shutdown_threshold);
	} else {
		pr_info("[%s] thx_therm_det_shutdown_threshold get failed!\n", __func__);
		data->thx_therm_det_shutdown_threshold = 0;
	}

	ret = of_property_read_u32_index(np, "thx-therm-det-alert-to-normal-value", 0, &value);
	if (!ret) {
		data->thx_therm_det_alert_to_normal_threshold = value;
		pr_err("[%s] thx_therm_det_alert_to_normal_threshold = %d\n", __func__,
		       data->thx_therm_det_alert_to_normal_threshold);
	} else {
		pr_info("[%s] thx_therm_det_alert_to_normal_threshold get failed!\n", __func__);
		data->thx_therm_det_alert_to_normal_threshold = 0;
	}

	ret = of_property_read_u32_index(np, "thx-therm-det-filter-count", 0, &value);
	if (!ret) {
		data->thx_therm_det_filter_count = value;
		pr_err("[%s] thx_therm_det_filter_count = %d\n", __func__,
		       data->thx_therm_det_filter_count);
	} else {
		pr_info("[%s] thx_therm_det_filter_count get failed!\n", __func__);
		data->thx_therm_det_filter_count = 10;
	}

	ret = of_property_read_u32_index(np, "po-dc-det-max-threshold", 0, &value);
	if (!ret) {
		data->po_dc_det_max_threshold = value;
		pr_err("[%s] po-dc-det-max-threshold = %d\n", __func__, data->po_dc_det_max_threshold);
	} else {
		pr_info("[%s] po-dc-det-max-threshold get failed!\n", __func__);
		data->po_dc_det_max_threshold = 0;
	}

	ret = of_property_read_u32_index(np, "po-dc-det-min-threshold", 0, &value);
	if (!ret) {
		data->po_dc_det_min_threshold = value;
		pr_err("[%s] po-dc-det-min-threshold = %d\n", __func__, data->po_dc_det_min_threshold);
	} else {
		pr_info("[%s] po-dc-det-min-thresholdd get failed!\n", __func__);
		data->po_dc_det_min_threshold = 0;
	}

	ret = of_property_read_u32_index(np, "po-dc-det-thread-value", 0, &value);
	if (!ret) {
		data->po_dc_det_thread_value = value;
		pr_err("[%s] po-dc-det-thread-value= %d\n", __func__, data->po_dc_det_thread_value);
	} else {
		pr_info("[%s] po-dc-det-thread-value get failed!\n", __func__);
		data->po_dc_det_thread_value = 0;
	}

	ret = of_property_read_u32_index(np, "bal-dc-det-max-threshold", 0, &value);
	if (!ret) {
		data->bal_dc_det_max_threshold = value;
		pr_err("[%s] bal-dc-det-max-threshold = %d\n", __func__, data->bal_dc_det_max_threshold);
	} else {
		pr_info("[%s] bal-dc-det-max-threshold get failed!\n", __func__);
		data->bal_dc_det_max_threshold = 0;
	}

	ret = of_property_read_u32_index(np, "bal-dc-det-min-threshold", 0, &value);
	if (!ret) {
		data->bal_dc_det_min_threshold = value;
		pr_err("[%s] bal-dc-det-min-threshold = %d\n", __func__, data->bal_dc_det_min_threshold);
	} else {
		pr_info("[%s] bal-dc-det-min-thresholdd get failed!\n", __func__);
		data->bal_dc_det_min_threshold = 0;
	}

	ret = of_property_read_u32_index(np, "bal-dc-det-thread-value", 0, &value);
	if (!ret) {
		data->bal_dc_det_thread_value = value;
		pr_err("[%s] bal-dc-det-thread-value= %d\n", __func__, data->bal_dc_det_thread_value);
	} else {
		pr_info("[%s] bal-dc-det-thread-value get failed!\n", __func__);
		data->bal_dc_det_thread_value = 0;
	}

	ret = of_property_read_u32_index(np, "dc-det-filter-count", 0, &value);
	if (!ret) {
		data->dc_det_filter_count = value;
		pr_err("[%s] dc-det-filter-count = %d\n", __func__, data->dc_det_filter_count);
	} else {
		pr_info("[%s] dc-det-filter-count get failed!\n", __func__);
		data->dc_det_filter_count = 5;
	}

	ret = of_property_read_u32_index(np, "overload-det-filter-count", 0, &value);
	if (!ret) {
		data->overload_det_filter_count = value;
		pr_err("[%s] overload-det-filter-count = %d\n", __func__,
		       data->overload_det_filter_count);
	} else {
		pr_info("[%s] overload-det-filter-count get failed!\n", __func__);
		data->overload_det_filter_count = 10;
	}

	return 0;
}

static void fiio_headset_jack_gpio_exit(struct headset_data *data)
{
	pr_err("[%s] enter ... \n", __func__);
	if (gpio_is_valid(data->hs35_det_gpio))
		gpio_free(data->hs35_det_gpio);
	if (gpio_is_valid(data->bal44_det_gpio))
		gpio_free(data->bal44_det_gpio);
	if (gpio_is_valid(data->po635_det_gpio))
		gpio_free(data->po635_det_gpio);
}

static void fiio_dc_det_work(struct work_struct *work)
{
	struct headset_data *data = container_of(work, struct headset_data, dc_det_work.work);
	struct device *dev = data->dev;
	int value = -1;
	static bool dc_det1_get;
	static int dc_det1_count;
	static int dc_det2_count;

	if (!data->dc_det_en)
		return;

	if (!data->dc_det1) {
		if (!dc_det1_get) {
			dc_det1_get = true;
			data->dc_det1 = devm_iio_channel_get(dev, "dc_det1");
			if (IS_ERR(data->dc_det1)) {
				pr_info("[%s] dc_det1 adc get failed again!\n", __func__);
				data->dc_det1 = NULL;
			}
		}
	} else {
		iio_read_channel_processed(data->dc_det1, &value);
		if (value > data->po_dc_det_thread_value) {
			if (data->data_private->allow_dc_det &&
			    (value < data->po_dc_det_min_threshold - data->po_dc_det_thread_value ||
			     value > data->po_dc_det_max_threshold + data->po_dc_det_thread_value)) {
				dc_det1_count++;
				if (dc_det1_count >= data->dc_det_filter_count) {
					dc_det1_count = 0;
					monitor_notifier_call(DC_ERROR);
					report_alarm_info(data, DC_DET_ERROR, 0);
					data->dc_det1_state = true;
				}
			} else if (value > data->po_dc_det_min_threshold + data->po_dc_det_thread_value ||
				   value < data->po_dc_det_max_threshold - data->po_dc_det_thread_value) {
				dc_det1_count = 0;
			}
		}
		if (data->dc_det1_state && data->hs_valid == 0) {
			data->dc_det1_state = false;
			report_alarm_info(data, NO_ALARM, 0);
		}
	}

	if (data->dc_det2 && data->hs_valid == 0xc) {
		iio_read_channel_processed(data->dc_det2, &value);
		if (value > data->bal_dc_det_thread_value) {
			if (data->data_private->allow_dc_det &&
			    (value < data->bal_dc_det_min_threshold - data->bal_dc_det_thread_value ||
			     value > data->bal_dc_det_max_threshold + data->bal_dc_det_thread_value)) {
				dc_det2_count++;
				if (dc_det2_count >= data->dc_det_filter_count) {
					dc_det2_count = 0;
					monitor_notifier_call(DC_ERROR);
					report_alarm_info(data, DC_DET_ERROR, 0);
					data->dc_det2_state = true;
				}
			} else if (value > data->bal_dc_det_min_threshold + data->bal_dc_det_thread_value ||
				   value < data->bal_dc_det_max_threshold - data->bal_dc_det_thread_value) {
				dc_det2_count = 0;
			}
		}
		if (data->dc_det2_state && data->hs_valid == 0) {
			data->dc_det2_state = false;
			report_alarm_info(data, NO_ALARM, 0);
		}
	}

	if (gpio_is_valid(data->dc_det1_io)) {
		if (data->data_private->allow_dc_det) {
			if (gpio_get_value(data->dc_det1_io) == data->dc_det1_threshold) {
				dc_det1_count++;
				if (dc_det1_count >= data->dc_det_filter_count) {
					dc_det1_count = 0;
					monitor_notifier_call(DC_ERROR);
					report_alarm_info(data, DC_DET_ERROR, 1);
					data->dc_det1_state = true;
				}
				pr_info("[%s] dc_det1_count %d!\n", __func__, dc_det1_count);
			} else {
				dc_det1_count = 0;
			}
		}
		if (data->dc_det1_state && data->hs_valid == 0) {
			data->dc_det1_state = false;
			report_alarm_info(data, NO_ALARM, 0);
		}
	}

	if (gpio_is_valid(data->dc_det2_io)) {
		if (data->data_private->allow_dc_det) {
			if (gpio_get_value(data->dc_det2_io) == data->dc_det2_threshold) {
				dc_det2_count++;
				if (dc_det2_count >= data->dc_det_filter_count) {
					dc_det2_count = 0;
					monitor_notifier_call(DC_ERROR);
					report_alarm_info(data, DC_DET_ERROR, 2);
					data->dc_det2_state = true;
				}
				pr_info("[%s] dc_det2_count %d!\n", __func__, dc_det2_count);
			} else {
				dc_det2_count = 0;
			}
		}
		if (data->dc_det2_state && data->hs_valid == 0) {
			data->dc_det2_state = false;
			report_alarm_info(data, NO_ALARM, 0);
		}
	}

	if (data->hs_valid == 0) {
		dc_det1_count = 0;
		dc_det2_count = 0;
	} else {
		schedule_delayed_work(&data->dc_det_work, msecs_to_jiffies(100));
	}
}

static void fiio_overload_det_work(struct work_struct *work)
{
	struct headset_data *data = container_of(work, struct headset_data, overload_det_work.work);
	static int over_load_count;

	if (!data->overload_det_en)
		return;

	if (gpio_is_valid(data->overload_det)) {
		if (data->data_private->allow_overload_det) {
			if (gpio_get_value(data->overload_det) == data->overload_det_threshold) {
				over_load_count++;
				if (over_load_count >= data->overload_det_filter_count) {
					over_load_count = 0;
					monitor_notifier_call(OVERLOAD_ERROR);
					report_alarm_info(data, OVERLOAD_DET_ERROR, 0);
					data->overload_det_state = true;
				}
				pr_info("[%s] over_load_count %d!\n", __func__, over_load_count);
			} else {
				over_load_count = 0;
			}
		}
		if (data->overload_det_state && data->hs_valid == 0) {
			data->overload_det_state = false;
			report_alarm_info(data, OVERLOAD_DET_ERROR, 0);
		}
	}

	if (data->hs_valid == 0)
		over_load_count = 0;
	else
		schedule_delayed_work(&data->overload_det_work, msecs_to_jiffies(100));
}

static void fiio_thx_therm_det_work(struct work_struct *work)
{
	struct headset_data *data = container_of(work, struct headset_data, thx_therm_det_work.work);
	struct device *dev = data->dev;
	int value = -1;
	static int thx_therm_alert_count;
	static int thx_therm_shutdown_count;
	static bool thx_therm_det_get;

	if (!data->thx_therm_det_en)
		return;

	if (!data->thx_therm_det) {
		if (!thx_therm_det_get) {
			thx_therm_det_get = true;
			data->thx_therm_det = devm_iio_channel_get(dev, "thx_therm_det");
			if (IS_ERR(data->thx_therm_det)) {
				pr_info("[%s] thx_therm_det get failed again!\n", __func__);
				data->thx_therm_det = NULL;
			}
		}
	} else if (data->data_private->allow_thx_therm_det || data->thx_therm_det_alert_en) {
		iio_read_channel_processed(data->thx_therm_det, &value);
		if (!data->thx_therm_det_alert_en) {
			if (value < data->thx_therm_det_shutdown_threshold) {
				thx_therm_shutdown_count++;
				if (thx_therm_shutdown_count >= data->thx_therm_det_filter_count) {
					thx_therm_shutdown_count = 0;
					report_alarm_info(data, TEMP_DET_ERROR, 1);
					data->thx_therm_det_alert_en = true;
				}
			} else if (value < data->thx_therm_det_alert_threshold) {
				thx_therm_alert_count++;
				if (thx_therm_alert_count >= data->thx_therm_det_filter_count) {
					thx_therm_alert_count = 0;
					monitor_notifier_call(HIGH_TEMP_ERROR);
					report_alarm_info(data, TEMP_DET_ERROR, 0);
					data->thx_therm_det_alert_en = true;
				}
			}
		} else if (value > data->thx_therm_det_alert_to_normal_threshold) {
			thx_therm_alert_count++;
			if (thx_therm_alert_count >= data->thx_therm_det_filter_count) {
				thx_therm_alert_count = 0;
				report_alarm_info(data, NO_ALARM, 0);
				data->thx_therm_det_alert_en = false;
			}
		}

		if (data->thx_therm_det_alert_en && data->hs_valid == 0) {
			data->thx_therm_det_alert_en = false;
			report_alarm_info(data, NO_ALARM, 0);
		}
	}

	if (data->hs_valid == 0) {
		thx_therm_shutdown_count = 0;
		thx_therm_alert_count = 0;
	} else {
		schedule_delayed_work(&data->thx_therm_det_work, msecs_to_jiffies(2000));
	}
}

static char *headset_jack_get_headset_state_name(int headset_state)
{
	char headset_state_name[64];
	char buf[64];

	sprintf(headset_state_name, "%s%s%s%s%s",
		(headset_state & 0x1) ? "single 3.5mm, " : "",
		(headset_state & 0x2) ? "single 6.35mm, " : "",
		(headset_state & 0x4) ? "bal 2.5mm, " : "",
		(headset_state & 0x8) ? "bal 4.4mm, " : "",
		headset_state ? "" : "none");
	return strcpy(buf, headset_state_name);
}

static char *headset_jack_get_hs_valid_name(int hs_valid)
{
	char hs_valid_name[64];
	char *name = hs_valid_name;

	sprintf(name, "%s%s%s",
		hs_valid == 3 ? "single" : "",
		hs_valid == 0xc ? "balance" : "",
		hs_valid ? "" : "none");
	return name;
}

static void fiio_headset_jack_work(struct work_struct *work)
{
	struct headset_data *data = container_of(work, struct headset_data, jack_work.work);
	static int scan_time = 166;
	int value = -1;

	if (data->force_bal25 || data->force_hs35 || data->force_bal44 || data->force_po635)
		return;

	if (data->bal25_det_adc) {
		iio_read_channel_processed(data->bal25_det_adc, &value);
		if (value > data->bal25_det_threshold) {
			data->bal25in_det_filter_count++;
			data->bal25out_det_filter_count = 0;
			if (data->bal25in_det_filter_count > 3) {
				data->bal25in_det_filter_count = 0;
				data->headset_state |= 0x4;
			}
		} else {
			data->bal25in_det_filter_count = 0;
			data->bal25out_det_filter_count++;
			if (data->bal25out_det_filter_count > 3) {
				data->bal25out_det_filter_count = 0;
				data->headset_state &= ~0x4;
			}
		}
	}

	if (data->hs35_det_adc) {
		iio_read_channel_processed(data->hs35_det_adc, &value);
		if (value >= 0 &&
		    ((value < data->hs35_det_threshold && !data->is_high_voltage_det_hs35) ||
		     (value > data->hs35_det_threshold && data->is_high_voltage_det_hs35))) {
			data->hs35in_det_filter_count++;
			data->hs35out_det_filter_count = 0;
			if (data->hs35in_det_filter_count > 3) {
				data->hs35in_det_filter_count = 0;
				data->headset_state |= 0x1;
			}
		} else {
			data->hs35in_det_filter_count = 0;
			data->hs35out_det_filter_count++;
			if (data->hs35out_det_filter_count > 3) {
				data->hs35out_det_filter_count = 0;
				data->headset_state &= ~0x1;
			}
		}
	}

	if (gpio_is_valid(data->hs35_det_gpio)) {
		if (!data->data_private->hs35_det_gpio_disable) {
			gpio_direction_input(data->hs35_det_gpio);
			if (gpio_get_value(data->hs35_det_gpio) == data->hs35_det_threshold) {
				data->hs35in_det_filter_count++;
				data->hs35out_det_filter_count = 0;
				if (data->hs35in_det_filter_count > 3) {
					data->hs35in_det_filter_count = 0;
					data->headset_state |= 0x1;
				}
			} else {
				data->hs35in_det_filter_count = 0;
				data->hs35out_det_filter_count++;
				if (data->hs35out_det_filter_count > 3) {
					data->hs35out_det_filter_count = 0;
					data->headset_state &= ~0x1;
				}
			}
		} else {
			gpio_direction_output(data->hs35_det_gpio, 0);
		}
	}

	if (gpio_is_valid(data->bal44_det_gpio)) {
		if (gpio_get_value(data->bal44_det_gpio) == data->bal44_det_threshold) {
			data->bal44in_det_filter_count++;
			data->bal44out_det_filter_count = 0;
			if (data->bal44in_det_filter_count > 3) {
				data->bal44in_det_filter_count = 0;
				data->headset_state |= 0x8;
			}
		} else {
			data->bal44in_det_filter_count = 0;
			data->bal44out_det_filter_count++;
			if (data->bal44out_det_filter_count > 3) {
				data->bal44out_det_filter_count = 0;
				data->headset_state &= ~0x8;
			}
		}
	}

	if (gpio_is_valid(data->po635_det_gpio)) {
		if (gpio_get_value(data->po635_det_gpio) == data->po635_det_threshold) {
			data->hs635in_det_filter_count++;
			data->hs635out_det_filter_count = 0;
			if (data->hs635in_det_filter_count > 3) {
				data->hs635in_det_filter_count = 0;
				data->headset_state |= 0x2;
			}
		} else {
			data->hs635in_det_filter_count = 0;
			data->hs635out_det_filter_count++;
			if (data->hs635out_det_filter_count > 3) {
				data->hs635out_det_filter_count = 0;
				data->headset_state &= ~0x2;
			}
		}
	}

	if (dynamic_debug_modes == 1)
		pr_err("%s: data->po635_det_gpio=%d value=%d data->bal44_det_gpio=%d value=%d\n",
		       __func__, data->po635_det_gpio, gpio_get_value(data->bal44_det_gpio),
		       data->bal44_det_gpio, gpio_get_value(data->po635_det_gpio));

	if (data->headset_state_pre != data->headset_state) {
		if ((data->headset_state & 0x3) && (data->headset_state & 0xc)) {
			if (data->hs_valid == 0)
				data->hs_valid = 3;
		} else if (data->headset_state & 0x3) {
			data->hs_valid = 3;
		} else if (data->headset_state & 0xc) {
			data->hs_valid = 0xc;
		} else {
			data->hs_valid = 0;
		}

		if (data->dc_det_en) {
			if (data->hs_valid == 0) {
				if (data->dc_det1_state || data->dc_det2_state) {
					data->dc_det1_state = false;
					data->dc_det2_state = false;
					report_alarm_info(data, NO_ALARM, 0);
				}
			} else {
				schedule_delayed_work(&data->dc_det_work, msecs_to_jiffies(50));
			}
		}

		if (data->overload_det_en) {
			if (data->hs_valid == 0) {
				if (data->overload_det_state) {
					data->overload_det_state = false;
					report_alarm_info(data, NO_ALARM, 0);
				}
			} else {
				schedule_delayed_work(&data->overload_det_work, msecs_to_jiffies(800));
			}
		}

		if (data->thx_therm_det_en) {
			if (data->hs_valid == 0) {
				if (data->thx_therm_det_alert_en) {
					data->thx_therm_det_alert_en = false;
					report_alarm_info(data, NO_ALARM, 0);
				}
			} else {
				schedule_delayed_work(&data->thx_therm_det_work, msecs_to_jiffies(2000));
			}
		}

		update_output_type(data);
#line 950
		pr_err("%s %d, headset is %s, valid is %s\n", __func__, __LINE__,
		       headset_jack_get_headset_state_name(data->headset_state),
		       headset_jack_get_hs_valid_name(data->hs_valid));
		headset_status_notifier_call(data);
		report_headset_type_state(data, data->headset_type, data->headset_state);
		data->headset_state_pre = data->headset_state;
		data->pre_hs_valid = data->hs_valid;
	}

	if (dynamic_debug_modes == 1)
#line 961
		pr_err("%s %d, headset is %s, valid is %s\n", __func__, __LINE__,
		       headset_jack_get_headset_state_name(data->headset_state),
		       headset_jack_get_hs_valid_name(data->hs_valid));

	if (scan_time >= data->hs_det_delay_ms + 40 || scan_time < data->hs_det_delay_ms - 40)
		scan_time = data->hs_det_delay_ms - 40;
	else
		scan_time += 20;

	schedule_delayed_work(&data->jack_work, msecs_to_jiffies(scan_time));
}

static void force_headset_jack_work(struct work_struct *work)
{
	struct headset_data *data = container_of(work, struct headset_data, force_jack_work.work);

	pr_err("%s: force_bal25 = %d force_hs35 = %d force_bal44 = %d, force_po635 = %d", __func__,
	       data->force_bal25, data->force_hs35, data->force_bal44, data->force_po635);

	data->headset_state = (data->headset_state & ~0xf) | data->force_hs35 |
			      data->force_bal25 << 2 | data->force_bal44 << 3 |
			      data->force_po635 << 1;
	if (data->headset_state_pre == data->headset_state)
		return;

	if ((data->headset_state & 0x3) && (data->headset_state & 0xc)) {
		if (data->hs_valid == 0)
			data->hs_valid = 3;
	} else if (data->headset_state & 0x3) {
		data->hs_valid = 3;
	} else if (data->headset_state & 0xc) {
		data->hs_valid = 0xc;
	} else {
		data->hs_valid = 0;
	}

	update_output_type(data);
	headset_status_notifier_call(data);
	report_headset_type_state(data, data->headset_type, data->headset_state);
	data->headset_state_pre = data->headset_state;
	data->pre_hs_valid = data->hs_valid;
#line 1027
	pr_err("%s %d, headset is %s, valid is %s\n", __func__, __LINE__,
	       headset_jack_get_headset_state_name(data->headset_state),
	       headset_jack_get_hs_valid_name(data->hs_valid));
}

static ssize_t fiio_headset_type_show(struct device *dev, struct device_attribute *attr,
				      char *buf)
{
	return sprintf(buf, "%d\n", g_data->headset_type);
}

static ssize_t fiio_headset_debug_store(struct device *dev, struct device_attribute *attr,
					const char *buf, size_t size)
{
	struct headset_data *data = g_data;

	if (!size)
		return size;

	pr_err("[%s] %s \n", __func__, buf);
	if (!strcmp(buf, "dc_det_en\n")) {
		data->dc_det_en = true;
	} else if (!strcmp(buf, "dc_det_dis\n")) {
		data->dc_det_en = false;
	} else if (!strcmp(buf, "overload_det_en\n")) {
		data->overload_det_en = true;
	} else if (!strcmp(buf, "overload_det_dis\n")) {
		data->overload_det_en = false;
	} else if (!strcmp(buf, "thx_therm_det_en\n")) {
		data->thx_therm_det_en = true;
	} else if (!strcmp(buf, "thx_therm_det_dis\n")) {
		data->thx_therm_det_en = false;
	} else if (!strcmp(buf, "enable_dc_alarm\n")) {
		monitor_notifier_call(DC_ERROR);
		report_alarm_info(data, DC_DET_ERROR, 0);
		data->dc_det1_state = true;
	} else if (!strcmp(buf, "disable_dc_alarm\n")) {
		data->dc_det1_state = false;
		report_alarm_info(data, NO_ALARM, 0);
	} else if (!strcmp(buf, "enable_overload_alarm\n")) {
		monitor_notifier_call(OVERLOAD_ERROR);
		report_alarm_info(data, OVERLOAD_DET_ERROR, 3);
		data->dc_det1_state = true;
	} else if (!strcmp(buf, "disable_overload_alarm\n")) {
		data->overload_det_state = false;
		report_alarm_info(data, NO_ALARM, 0);
	} else if (!strcmp(buf, "enable_force_hs35\n")) {
		data->force_hs35 = true;
		data->force_bal25 = false;
		data->force_bal44 = false;
		data->force_po635 = false;
		schedule_delayed_work(&data->force_jack_work, msecs_to_jiffies(data->hs_det_delay_ms));
	} else if (!strcmp(buf, "disable_force_hs35\n")) {
		data->force_hs35 = false;
		schedule_delayed_work(&data->force_jack_work, msecs_to_jiffies(data->hs_det_delay_ms));
	} else if (!strcmp(buf, "enable_force_bal25\n")) {
		data->force_hs35 = false;
		data->force_bal25 = true;
		data->force_bal44 = false;
		data->force_po635 = false;
		schedule_delayed_work(&data->force_jack_work, msecs_to_jiffies(data->hs_det_delay_ms));
	} else if (!strcmp(buf, "disable_force_bal25\n")) {
		data->force_bal25 = false;
		schedule_delayed_work(&data->force_jack_work, msecs_to_jiffies(data->hs_det_delay_ms));
	} else if (!strcmp(buf, "enable_force_bal44\n")) {
		data->force_hs35 = false;
		data->force_bal25 = false;
		data->force_bal44 = true;
		data->force_po635 = false;
		schedule_delayed_work(&data->force_jack_work, msecs_to_jiffies(data->hs_det_delay_ms));
	} else if (!strcmp(buf, "disable_force_bal44\n")) {
		data->force_bal44 = false;
		schedule_delayed_work(&data->force_jack_work, msecs_to_jiffies(data->hs_det_delay_ms));
	} else if (!strcmp(buf, "enable_force_po635\n")) {
		data->force_hs35 = false;
		data->force_bal25 = false;
		data->force_bal44 = false;
		data->force_po635 = true;
		schedule_delayed_work(&data->force_jack_work, msecs_to_jiffies(data->hs_det_delay_ms));
	} else if (!strcmp(buf, "disable_force_po635\n")) {
		data->force_po635 = false;
		schedule_delayed_work(&data->force_jack_work, msecs_to_jiffies(data->hs_det_delay_ms));
	} else {
		pr_err("[%s] error cmd\n", __func__);
	}

	return size;
}

static const struct device_attribute headset_attrs[] = {
	__ATTR(headset_debug, 0200, NULL, fiio_headset_debug_store),
	__ATTR(headset_type, 0444, fiio_headset_type_show, NULL),
};

static void headset_notify_dwork(struct work_struct *work)
{
	struct headset_data *data = container_of(work, struct headset_data, headset_notify_work.work);

	headset_status_notifier_call(data);
}

static int dac_event_notifier_cb(struct notifier_block *self, uintptr_t event, void *data)
{
	struct headset_data *headset_data = container_of(self, struct headset_data, dac_event_notifier);
	struct dac_notify_data *notify_data = data;
	int value;

	if (notify_data && (notify_data->event & LOPO_MODE_SETTING_CHANGE)) {
		value = notify_data->lopo_mode_setting;
		if (value < 3)
			headset_data->headset_mode = value;
		else if (value < 5)
			headset_data->balance_mode = value;
		else
			headset_data->output_type = value;
		pr_err("%s, LOPO_MODE_SETTING_CHANGE, set lopo mode setting = %d.\n", __func__, value);
		pr_err("%s, headset_mode = (%s), balance_mode = (%s).\n", __func__,
		       output_name[headset_data->headset_mode], output_name[headset_data->balance_mode]);
		update_output_type(headset_data);
		report_headset_type_state(headset_data, headset_data->headset_type,
					  headset_data->headset_state);
		schedule_delayed_work(&headset_data->headset_notify_work, 0);
	}
	return 0;
}

static int user_config_event_notifier_cb(struct notifier_block *self, uintptr_t event, void *data)
{
	struct fiio_user_config_notify_data *notify_data = data;

	if (notify_data) {
		if (notify_data->event == FIIO_CONFIG_MONITOR_FORCE_BAL44) {
			pr_err("%s: FIIO_CONFIG_MONITOR_FORCE_BAL44 notify_data->enable=%d", __func__,
			       notify_data->enable);
			container_of(self, struct headset_data, user_config_event_notifier)->force_bal44 =
				notify_data->enable;
		} else if (notify_data->event == FIIO_CONFIG_MONITOR_GET_CONFIG) {
			pr_err("%s: FIIO_CONFIG_MONITOR_GET_CONFIG", __func__);
		} else if (notify_data->event == FIIO_CONFIG_MONITOR_FORCE_PO635) {
			pr_err("%s: FIIO_CONFIG_MONITOR_FORCE_PO635 notify_data->enable=%d", __func__,
			       notify_data->enable);
			container_of(self, struct headset_data, user_config_event_notifier)->force_po635 =
				notify_data->enable;
		}
	}
	return 0;
}

static int notify_output_mode_event_notifier_cb(struct notifier_block *self, uintptr_t event,
						void *data)
{
	struct headset_data *headset_data =
		container_of(self, struct headset_data, output_mode_event_notifier);
	struct notify_output_data *notify_data = data;

	if (notify_data && notify_data->event == NOTIFY_EVENT_OUTPUT_MODE_CONFIG) {
		if (notify_data->output_mode == FIIO_OUTPUT_MODE_PO_PRE ||
		    notify_data->output_mode == FIIO_OUTPUT_MODE_PO)
			headset_data->output_type = 5;
		else
			headset_data->output_type = 6;
		schedule_delayed_work(&headset_data->jack_work, msecs_to_jiffies(166));
	}
	pr_err("%s: headset_data->output_type=%d\n", __func__, headset_data->output_type);
	return 0;
}

static struct headset_jack_public_data headset_data_private;

static struct fiio_device_driver headset_jack_driver = {
	.name = "headset_jack module",
	.private_data = &headset_data_private,
};

static int fiio_headset_jack_probe(struct platform_device *pdev)
{
	struct headset_data *data;
	struct headset_jack_public_data *public_data;
	int ret;

	pr_err("[%s] enter ... \n", __func__);
	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	platform_set_drvdata(pdev, data);
	g_data = data;
	data->dev = &pdev->dev;

	ret = fiio_headset_jack_parse_dt(data);
	if (ret) {
		dev_err(&pdev->dev, "[%s] Failed to get headset_jack devices dt\n", __func__);
		goto err_pdata_free;
	}

	data->balance_mode = 8;
	data->output_type = 8;
	data->headset_type = 0;
	data->headset_mode = 8;
	data->hs_valid = 0;
	data->pre_hs_valid = 0;
	data->headset_state = 0;
	data->headset_state_pre = 0;
	data->dc_det_en = true;
	data->bal25in_det_filter_count = 0;
	data->bal25out_det_filter_count = 0;
	data->hs35in_det_filter_count = 0;
	data->hs35out_det_filter_count = 0;
	data->bal44in_det_filter_count = 0;
	data->bal44out_det_filter_count = 0;
	data->hs635in_det_filter_count = 0;
	data->hs635out_det_filter_count = 0;

	public_data = &headset_data_private;
	data->data_private = public_data;

	data->dac_event_notifier.notifier_call = dac_event_notifier_cb;
	fiio_register_client(notifier_dac, &data->dac_event_notifier);
	data->user_config_event_notifier.notifier_call = user_config_event_notifier_cb;
	fiio_register_client(notifier_user_config, &data->user_config_event_notifier);
	data->output_mode_event_notifier.notifier_call = notify_output_mode_event_notifier_cb;
	fiio_register_client(notifier_output_mode, &data->output_mode_event_notifier);

	ret = fiio_device_attribute_register(headset_attrs, ARRAY_SIZE(headset_attrs));
	if (ret)
		dev_err(&pdev->dev, "sysfs create error: %d\n", ret);

	INIT_DELAYED_WORK(&data->jack_work, fiio_headset_jack_work);
	schedule_delayed_work(&data->jack_work, msecs_to_jiffies(166));
	INIT_DELAYED_WORK(&data->dc_det_work, fiio_dc_det_work);
	INIT_DELAYED_WORK(&data->overload_det_work, fiio_overload_det_work);
	INIT_DELAYED_WORK(&data->thx_therm_det_work, fiio_thx_therm_det_work);
	INIT_DELAYED_WORK(&data->headset_notify_work, headset_notify_dwork);
	INIT_DELAYED_WORK(&data->force_jack_work, force_headset_jack_work);

	fiio_device_register(&headset_jack_driver);
	pr_err("[%s] end ... \n", __func__);
	return ret;

err_pdata_free:
	kfree(data);
	return ret;
}

static int fiio_headset_jack_remove(struct platform_device *pdev)
{
	struct headset_data *data = platform_get_drvdata(pdev);

	pr_err("[%s] enter ... \n", __func__);
	fiio_device_attribute_unregister(headset_attrs);
	cancel_delayed_work_sync(&data->jack_work);
	return 0;
}

static struct of_device_id fiio_headset_jack_dt_ids[] = {
	{ .compatible = "fiio,jack_det", },
	{ }
};
MODULE_DEVICE_TABLE(of, fiio_headset_jack_dt_ids);

static struct platform_driver fiio_headset_jack_driver = {
	.probe = fiio_headset_jack_probe,
	.remove = fiio_headset_jack_remove,
	.driver = {
		.name = "fiio_headset_jack",
		.owner = THIS_MODULE,
		.of_match_table = fiio_headset_jack_dt_ids,
	},
};

static int __init fiio_headset_jack_init(void)
{
	pr_err("[%s] enter ... \n", __func__);
	return platform_driver_register(&fiio_headset_jack_driver);
}

static void __exit fiio_headset_jack_exit(void)
{
	pr_err("[%s] enter ... \n", __func__);
	fiio_headset_jack_gpio_exit(g_data);
	kfree(g_data);
	platform_driver_unregister(&fiio_headset_jack_driver);
}

module_init(fiio_headset_jack_init);
module_exit(fiio_headset_jack_exit);

MODULE_AUTHOR("Jeff");
MODULE_DESCRIPTION("FiiO Head Jack Detect Driver");
MODULE_LICENSE("GPL");
