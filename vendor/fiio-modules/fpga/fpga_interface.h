/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FPGA_INTERFACE_H
#define _FPGA_INTERFACE_H

#include <linux/notifier.h>
#include <fiio/fpga_common.h>

struct fpga_interface_hal_data {
	struct device *dev;
	/* "fpga_device module" registered by gw-fpga */
	struct fpga_component *fpga_device;
	int num_dai;

	struct notifier_block fiio_control_fpga_config_notifier;

	bool opt_bmc_bypass;
	bool spdif_bmc_bypass;
	bool hdmi_i2s_bypass;
	bool opt_i2s_bypass;
	bool spdif_i2s_bypass;
	bool bt_i2s_bypass;
	bool uac_i2s_bypass;
	bool soc_i2s_bypass;

	bool fill_clk;

	bool fpga_is_poweron;

	/* exported to the DAC drivers as "fpga module" */
	struct fpga_public_data *fpga_public;
};

#endif
