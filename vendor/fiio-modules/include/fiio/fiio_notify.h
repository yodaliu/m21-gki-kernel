/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FIIO_NOTIFY_H
#define _FIIO_NOTIFY_H

#include <linux/notifier.h>

/* one blocking notifier chain per event source */
enum fiio_notifier_type {
	notifier_headset,
	notifier_dac,
	notifier_light,
	notifier_player,
	notifier_charger,
	notifier_sw_hold,
	notifier_system_status,
	notifier_monitor,
	notifier_user_config,
	notifier_output_mode,
	notifier_fpga,
	notifier_misc_config,
	notifier_vbus_detection_qti,
	notifier_vbus_detection_fiio,
	notifier_pump_irq,
	notifier_dmode_irq,
	notifier_usb_otg,
	notifier_uart,
	notifier_gpio_key,
};

int fiio_register_client(enum fiio_notifier_type index, struct notifier_block *nb);
int fiio_unregister_client(enum fiio_notifier_type index, struct notifier_block *nb);
int fiio_notifier_call_chain(enum fiio_notifier_type index, unsigned long val, void *v);

/*
 * Event payloads passed as the data pointer of each chain (layouts and
 * values from the stock DWARF).
 */

enum headset_event_type {
	HEADSET_STATE_CHANGE = 1,
	HEADSET_OUTPUT_TYPE_CHANGE = 2,
	HEADSET_DC_ERROR = 4,
	HEADSET_OVERLOAD_ERROR = 8,
	HEADSET_TEMP_ERROR = 8,
};

struct headset_notify_data {
	enum headset_event_type event;
	int headset_state;
	int output_type;
	int headset_type;
};

enum dac_event_type {
	LOPO_MODE_SETTING_CHANGE = 1,
	DC_DETECE_CHANGE = 2,
	DAC_STARTUP_EVENT = 4,
	DAC_SHUTDOWN_EVENT = 8,
	DAC_MUTE_EVENT = 16,
	DAC_LOCAL_PLAY_EVENT = 32,
};

struct dac_notify_data {
	enum dac_event_type event;
	int lopo_mode_setting;
	int alloc_dc_det;
	bool is_local_playing;
	bool is_dac_mute;
};

enum player_event_type {
	PLAYER_STARTUP = 0,
	PLAYER_CONFIG_PARAMS = 1,
	PLAYER_SHUTDOWN = 2,
	PLAYER_MODE_CHANGE = 3,
	PLAYER_MUTE = 4,
	PLAYER_UNMUTE = 5,
	PLAYER_UAC_MODE_CHANGE = 6,
	PLAYER_SW_MUTE = 7,
	PLAYER_SHUTDOWN_NOW = 8,
};

struct player_notify_data {
	enum player_event_type event;
	int sample_rate;
	int stream_format;
	int stream_width;
	int delay_time;
	int shutdown_enable;
	int mode;
	int is_uac_ios_mode;
	int sw_mute_enable;
};

typedef enum {
	HOLD_SW_CHANGE_EVENT = 1,
	HOLD_KEY_PRE_CHANGE_EVENT = 2,
	HOLD_KEY_NEXT_CHANGE_EVENT = 4,
	HOLD_KEY_PAUSE_CHANGE_EVENT = 8,
	HOLD_KEY_VOLUP_CHANGE_EVENT = 16,
	HOLD_KEY_VOLDOWN_CHANGE_EVENT = 32,
	HOLD_KEY_FUNC_CHANGE_EVENT = 64,
	HOLD_KEY_TP_CHANGE_EVENT = 128,
	HOLD_KEY_VOLKNOB_CHANGE_EVENT = 256,
	HOLD_KEY_SCREENOFF_CHANGE_EVENT = 512,
} sw_hold_event;

struct sw_hold_notify_data {
	sw_hold_event event;
	unsigned int hold_sw : 1;
	unsigned int hold_key_pre : 1;
	unsigned int hold_key_next : 1;
	unsigned int hold_key_pause : 1;
	unsigned int hold_key_volup : 1;
	unsigned int hold_key_voldown : 1;
	unsigned int hold_key_func : 1;
	unsigned int hold_key_tp : 1;
	unsigned int hold_key_volknob : 1;
	unsigned int hold_key_screenoff : 1;
	unsigned int hold_reverse : 23;
};

enum fiio_system_status_event_type {
	EVENT_ANDROID_LOCKED_BOOT_COMPLETED = 2,
	EVENT_ANDROID_BOOT_COMPLETED = 4,
	EVENT_ANDROID_SYSTEM_MODE_CHANGE = 8,
};

struct fiio_system_status_notify_data {
	enum fiio_system_status_event_type event;
	int system_mode;
};

enum monitor_event_type {
	DC_ERROR = 0,
	OVERLOAD_ERROR = 1,
	HIGH_TEMP_ERROR = 2,
};

struct monitor_notify_data {
	enum monitor_event_type event;
};

enum fiio_user_config_event_type {
	FIIO_CONFIG_MONITOR_DC_DET1 = 0,
	FIIO_CONFIG_MONITOR_DC_DET2 = 1,
	FIIO_CONFIG_MONITOR_OVERLOAD_DET = 2,
	FIIO_CONFIG_MONITOR_TEMP_DET = 3,
	FIIO_CONFIG_MONITOR_GET_CONFIG = 4,
	FIIO_CONFIG_MONITOR_FORCE_PO35 = 5,
	FIIO_CONFIG_MONITOR_FORCE_BAL25 = 6,
	FIIO_CONFIG_MONITOR_FORCE_BAL44 = 7,
	FIIO_CONFIG_MONITOR_FORCE_PO635 = 8,
	FIIO_CONFIG_MONITOR_FORCE_DEBUG_DC1 = 9,
	FIIO_CONFIG_MONITOR_FORCE_DEBUG_DC2 = 10,
	FIIO_CONFIG_MONITOR_FORCE_DEBUG_THX_TEMP = 11,
	FIIO_CONFIG_MONITOR_FORCE_DEBUG_THX_OVERLOAD = 12,
	FIIO_CONFIG_MONITOR_FORCE_DEBUG_DC1_VALUE = 13,
	FIIO_CONFIG_MONITOR_FORCE_DEBUG_DC2_VALUE = 14,
	FIIO_CONFIG_MONITOR_FORCE_DEBUG_THX_TEMP_VALUE = 15,
	FIIO_CONFIG_MONITOR_FORCE_DEBUG_THX_OVERLOAD_VALUE = 16,
	FIIO_CONFIG_MONITOR_FORCE_DEBUG_DC1_LOG = 17,
	FIIO_CONFIG_MONITOR_FORCE_DEBUG_DC2_LOG = 18,
	FIIO_CONFIG_MONITOR_FORCE_DEBUG_THX_TEMP_LOG = 19,
	FIIO_CONFIG_MONITOR_FORCE_DEBUG_THX_OVERLOAD_LOG = 20,
	FIIO_CONFIG_RGB_LED_CTRL = 21,
};

struct fiio_user_config_notify_data {
	enum fiio_user_config_event_type event;
	bool enable;
	int value;
	void (*call_back)(bool, bool, bool, bool);
};

enum notify_output_event_type {
	NOTIFY_EVENT_SPDIF_CONFIG = 0,
	NOTIFY_EVENT_OUTPUT_MODE_CONFIG = 1,
};

enum fiio_output_mode_t {
	FIIO_OUTPUT_MODE_NONE = 0,
	FIIO_OUTPUT_MODE_PO_PRE = 1,
	FIIO_OUTPUT_MODE_PRE = 2,
	FIIO_OUTPUT_MODE_LO = 3,
	FIIO_OUTPUT_MODE_PO = 4,
	FIIO_OUTPUT_MODE_BAL_PO = 5,
	FIIO_OUTPUT_MODE_BAL_LO = 6,
	FIIO_OUTPUT_MODE_ALL = 7,
};

struct notify_output_data {
	enum notify_output_event_type event;
	enum fiio_output_mode_t output_mode;
	bool spdif_tx_enable;
};

enum fiio_control_fpga_ctrl_event_type {
	FIIO_CONFIG_FPGA_CONTRL_GET_INFO = 0,
	FIIO_CONFIG_FPGA_CONTRL_POWER_ON = 1,
	FIIO_CONFIG_FPGA_CONTRL_READ_REG = 2,
};

struct fiio_control_fpga_ctrl_notify_data {
	enum fiio_control_fpga_ctrl_event_type event;
	bool enable;
	int reg;
	void (*call_back)(int, int, int);
};

enum fiio_vbus_charge_detection {
	NOTIFY_EVENT_CHARGER_VBUS_PLUGIN_FIIO = 0,
	NOTIFY_EVENT_CHARGER_VBUS_PLUGOUT_FIIO = 1,
	NOTIFY_EVENT_CHARGER_VBUS_TPYE_PD_FIIO = 2,
	NOTIFY_EVENT_CHARGER_VBUS_TYPE_UNKNOWN_FIIO = 3,
	NOTIFY_EVENT_CHARGER_VBUS_OTG_FIIO = 4,
};

struct fiio_vbus_charge_detection_data {
	enum fiio_vbus_charge_detection event;
	bool enable;
};

enum qti_vbus_charge_detection {
	NOTIFY_EVENT_CHARGER_VBUS_PLUGIN_QTI = 0,
	NOTIFY_EVENT_CHARGER_VBUS_PLUGOUT_QTI = 1,
	NOTIFY_EVENT_CHARGER_VBUS_TYPE_FAST_CHAGRER_QTI = 2,
	NOTIFY_EVENT_CHARGER_VBUS_TYPE_ORDINARY_CHAGRER_QTI = 3,
	NOTIFY_EVENT_CHARGER_VBUS_OTG_QTI = 4,
};

struct qti_vbus_charge_detection_data {
	enum qti_vbus_charge_detection event;
	bool enable;
};

enum desktop_mode_event_type {
	NOTIFY_EVENT_OPEN_DESKTOP_MODE = 0,
	NOTIFY_EVENT_CLOSE_DESKTOP_MODE = 1,
};

struct desktop_mode_notify_data {
	enum desktop_mode_event_type event;
	bool enable;
};

enum gpio_key_event_type {
	NOTIFY_EVENT_KEY_CODE = 0,
};

enum gpio_key_status {
	GPIO_KEY_DOWN = 0,
	GPIO_KEY_UP = 1,
	DOWN_AND_UP = 2,
};

struct gpio_key_event_notify_data {
	enum gpio_key_event_type event;
	enum gpio_key_status status;
	char gpio_key_code;
};

#endif
