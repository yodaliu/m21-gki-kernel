/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FIIO_MODE_SELECT_COMMON_H
#define _FIIO_MODE_SELECT_COMMON_H

enum sel_usb_path {
	USB_PATH_MTP,
	USB_PATH_CHARGER,
	USB_PATH_XMOS,
	USB_PATH_BT_UPDATE,
};

/*
 * Audio source the system is working in.
 */
enum system_mode {
	SYSTEM_MODE_MTP,
	SYSTEM_MODE_XMOS,
	SYSTEM_MODE_BT,
	SYSTEM_MODE_BT_UPDATE,
	SYSTEM_MODE_SPDIF_RX_BYPASS,
	SYSTEM_MODE_SOC_UAC,
	SYSTEM_MODE_SOC_UAC_EXIT,
	SYSTEM_MODE_SOC_BT,
	SYSTEM_MODE_SOC_BT_EXIT,
	SYSTEM_MODE_BT_EXIT,
	SYSTEM_MODE_HDMI_IN,
	SYSTEM_MODE_HDMI_TX_ARC,
};

struct mode_select_public_data {
	int (*get_current_player_mode)(void);
	int (*get_current_xmos_mode)(void);
	void (*usb_path_ctrl)(enum sel_usb_path);
};

#endif
