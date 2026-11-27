/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FIIO_TS_COMMON_H
#define _FIIO_TS_COMMON_H

/* one per touch panel driver; the panel that answers is marked vaild */
struct ts_device {
	char name[32];
	u8 addr;
	u8 i2c_adapter;
	u32 reset_gpio;
	char tp_vcc_name[32];
	bool used;
	bool default_flag;
	struct device *dev;
	/* LCD reset level the panel driver starts from */
	int lcd_reset_default_state;
	struct regulator *vcc_i2c;

	/* filled in by the touchscreen driver */
	int (*device_init)(struct ts_device *ts_device);
	void (*reset_ctrl)(struct ts_device *ts_device, int value);
	int (*ping_device)(struct ts_device *ts_device);
	void (*gesture_enable)(bool enable);
	/* called by the panel driver */
	void (*screen_off)(bool off);
	bool (*get_gesture_enable)(void);
	void (*dsi_panel_screen_off)(bool off);
};

struct ts_public_data {
	char *(*get_vaild_tp_name)(void);
	bool (*is_gesture_wakeup)(void);
	void (*set_screen_off)(bool off);
	bool is_abnormal;
	bool is_report_abnormal;
};

void ts_core_set_vaild_tp(struct ts_device *_ts_device, bool vaild);
struct ts_device *ts_core_get_vaild_tp(void);
struct i2c_client;
int try_to_ping(struct i2c_client *client, struct ts_device *ts_device);

#endif
