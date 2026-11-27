/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Helpers exported by fiio_primary_modules (fiio_utils.c and fiio_params.c).
 * The stock header name is unknown.
 */
#ifndef _FIIO_UTILS_H
#define _FIIO_UTILS_H

#include <linux/types.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <fiio/fiio_i2c.h>

void fiio_sendMessage(struct device *dev, char *buf1, char *buf2);
void report_playing_info(struct device *dev, int format, int sample_rate);
struct file *fiio_filp_open(const char *filename, int flags, umode_t mode);
s32 atoi(const char *psz_buf);
int htoi(const char *s);

void set_product_lcd_panel_name(const char *panel_name);
char *get_product_lcd_panel_name(void);

#endif
