/* SPDX-License-Identifier: GPL-2.0 */
/*
 * I2C and regmap wrappers exported by fiio_primary_modules (fiio_i2c.c).
 * The stock header name is unknown.
 */
#ifndef _FIIO_I2C_H
#define _FIIO_I2C_H

#include <linux/i2c.h>
#include <linux/regmap.h>

void set_fiio_i2c_failed_count(int count);
int get_fiio_i2c_failed_count(void);
int fiio_i2c_transfer(struct i2c_client *client, struct i2c_msg *msgs, int num);
int fiio_i2c_master_send(struct i2c_client *client, const char *buf, int count);
int fiio_i2c_master_recv(struct i2c_client *client, char *buf, int count);
int fiio_i2c_smbus_write_byte_data(struct i2c_client *client, unsigned int reg,
				   unsigned int val);
int fiio_i2c_smbus_read_byte_data(struct i2c_client *client, unsigned int reg);
int fiio_i2c_smbus_read_i2c_block_data(const struct i2c_client *client, u8 command,
				       u8 length, u8 *values);
int fiio_i2c_smbus_write_i2c_block_data(const struct i2c_client *client, u8 command,
					u8 length, const u8 *values);
int fiio_regmap_read(const char *name, struct regmap *map, unsigned int reg,
		     unsigned int *val);
int fiio_regmap_field_read(const char *name, struct regmap_field *field,
			   unsigned int *val);
int fiio_regmap_raw_read(const char *name, struct regmap *map, unsigned int reg,
			 void *val, size_t val_len);
int fiio_regmap_write(const char *name, struct regmap *map, unsigned int reg,
		      unsigned int val);
int fiio_regmap_field_wirte(const char *name, struct regmap_field *field,
			    unsigned int val);
int fiio_regmap_write_bits(const char *name, struct regmap *map, unsigned int reg,
			   unsigned int mask, unsigned int val);
int fiio_regmap_update_bits(const char *name, struct regmap *map, unsigned int reg,
			    unsigned int mask, unsigned int val);
int fiio_regmap_raw_write(const char *name, struct regmap *map, unsigned int reg,
			  const void *val, size_t val_len);

#endif
