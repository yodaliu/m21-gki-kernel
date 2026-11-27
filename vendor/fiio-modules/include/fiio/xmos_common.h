/* SPDX-License-Identifier: GPL-2.0 */
/* Layout from the stock DWARF. */
#ifndef _FIIO_XMOS_COMMON_H
#define _FIIO_XMOS_COMMON_H

#include <linux/types.h>

enum xmos_mode {
	XMOS_MODE_EXIT,
	XMOS_MODE_USB,
	XMOS_MODE_SPDIF_RX,
	XMOS_MODE_OPT_IN,
	XMOS_MODE_HDMI_TX_ARC,
	XMOS_MODE_USB_PEQ,
	XMOS_MODE_AES_IN,
};

struct xmos_public_data {
	void (*usb_path_enable)(bool);
	void (*power_ctrl)(bool);
	void (*power_set)(bool);
	void (*read_version)(void);
	bool (*power_on_state)(void);
	void (*set_startup)(bool);
	void (*reset_chip)(bool);
	void (*reset_flag)(void);
	void (*reset_power)(bool);
	void (*set_mode)(int);
	bool (*is_usb_plug)(void);
	void (*set_usb_plug)(bool);
	void (*set_ios_mode)(bool);
	void (*read_uart)(void);
};

#endif
