/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FIIO_LED_COMMON_H
#define _FIIO_LED_COMMON_H
enum led_status {
	BLUE,
	GREEN,
	RED,
	CYAN,
	PURPLE,
	YELLOW,
	WHILE,
	OTHER,
};

/* led.ko hands this out as its fiio_device private_data */
struct led_public_data {
	bool is_charging;
	void (*set_indicator_status)(enum led_status color);
	void (*get_indicator_status)(enum led_status *color);
};

struct aw22xxx_public_data {
	void (*volume_set_gain)(int gain);
	void (*led_iic_power_on)(void);
	int (*led_iic_test)(void);
};

#endif
