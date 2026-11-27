/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FPGA_AUDIO_INTERFACE_H
#define _FPGA_AUDIO_INTERFACE_H

#include <linux/i2c.h>
#include <linux/notifier.h>
#include <linux/regulator/consumer.h>
#include <fiio/fpga_common.h>

/* exported by gw-fpga */
extern void fpga_ctrl_power(bool enable);
extern void fpga_ctrl_power_by_mode(struct fpga_component *fpga_device, bool enable, int mode);
extern void fpga_enter_usb_xmos_mode(void);
extern void fpga_enter_local_playback_mode(void);

struct fpga_data {
	struct i2c_client *client;
	struct device *dev;
	struct regulator *ldo_1v8;

	struct notifier_block fiio_control_fpga_config_notifier;
};

#endif
