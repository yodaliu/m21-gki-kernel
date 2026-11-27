// SPDX-License-Identifier: GPL-2.0
/* I2C and regmap wrappers that count and log failed transfers. */
#define pr_fmt(fmt) "[fiio_i2c]: " fmt

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/gpio.h>

#include <fiio/fiio_i2c.h>

static int i2c_failed_count;

static int return_i2c_result(const char *name, int ret)
{
	if (ret < 0) {
		if (i2c_failed_count > 1000)
			i2c_failed_count = 0;
		i2c_failed_count++;
		pr_info("%s, name:%s, ret:%d, failed_count:%d\n", __func__, name,
			ret, i2c_failed_count);
	}

	return ret;
}

void set_fiio_i2c_failed_count(int count)
{
	i2c_failed_count = count;
}
EXPORT_SYMBOL(set_fiio_i2c_failed_count);

int get_fiio_i2c_failed_count(void)
{
	return i2c_failed_count;
}
EXPORT_SYMBOL(get_fiio_i2c_failed_count);

int fiio_i2c_transfer(struct i2c_client *client, struct i2c_msg *msgs, int num)
{
	return return_i2c_result(client->name,
				 i2c_transfer(client->adapter, msgs, num));
}
EXPORT_SYMBOL(fiio_i2c_transfer);

int fiio_i2c_master_send(struct i2c_client *client, const char *buf, int count)
{
	return return_i2c_result(client->name, i2c_master_send(client, buf, count));
}
EXPORT_SYMBOL(fiio_i2c_master_send);

int fiio_i2c_master_recv(struct i2c_client *client, char *buf, int count)
{
	return return_i2c_result(client->name, i2c_master_recv(client, buf, count));
}
EXPORT_SYMBOL(fiio_i2c_master_recv);

int fiio_i2c_smbus_write_byte_data(struct i2c_client *client, unsigned int reg,
				   unsigned int val)
{
	return return_i2c_result(client->name,
				 i2c_smbus_write_byte_data(client, reg, val));
}
EXPORT_SYMBOL(fiio_i2c_smbus_write_byte_data);

int fiio_i2c_smbus_read_byte_data(struct i2c_client *client, unsigned int reg)
{
	return return_i2c_result(client->name, i2c_smbus_read_byte_data(client, reg));
}
EXPORT_SYMBOL(fiio_i2c_smbus_read_byte_data);

int fiio_i2c_smbus_read_i2c_block_data(const struct i2c_client *client, u8 command,
				       u8 length, u8 *values)
{
	return return_i2c_result(client->name,
				 i2c_smbus_read_i2c_block_data(client, command, length, values));
}
EXPORT_SYMBOL(fiio_i2c_smbus_read_i2c_block_data);

int fiio_i2c_smbus_write_i2c_block_data(const struct i2c_client *client, u8 command,
					u8 length, const u8 *values)
{
	return return_i2c_result(client->name,
				 i2c_smbus_write_i2c_block_data(client, command, length, values));
}
EXPORT_SYMBOL(fiio_i2c_smbus_write_i2c_block_data);

int fiio_regmap_read(const char *name, struct regmap *map, unsigned int reg,
		     unsigned int *val)
{
	return return_i2c_result(name, regmap_read(map, reg, val));
}
EXPORT_SYMBOL(fiio_regmap_read);

int fiio_regmap_field_read(const char *name, struct regmap_field *field,
			   unsigned int *val)
{
	return return_i2c_result(name, regmap_field_read(field, val));
}
EXPORT_SYMBOL(fiio_regmap_field_read);

int fiio_regmap_raw_read(const char *name, struct regmap *map, unsigned int reg,
			 void *val, size_t val_len)
{
	return return_i2c_result(name, regmap_raw_read(map, reg, val, val_len));
}
EXPORT_SYMBOL(fiio_regmap_raw_read);

int fiio_regmap_write(const char *name, struct regmap *map, unsigned int reg,
		      unsigned int val)
{
	return return_i2c_result(name, regmap_write(map, reg, val));
}
EXPORT_SYMBOL(fiio_regmap_write);

int fiio_regmap_field_wirte(const char *name, struct regmap_field *field,
			    unsigned int val)
{
	return return_i2c_result(name, regmap_field_write(field, val));
}
EXPORT_SYMBOL(fiio_regmap_field_wirte);

int fiio_regmap_write_bits(const char *name, struct regmap *map, unsigned int reg,
			   unsigned int mask, unsigned int val)
{
	return return_i2c_result(name, regmap_write_bits(map, reg, mask, val));
}
EXPORT_SYMBOL(fiio_regmap_write_bits);

int fiio_regmap_update_bits(const char *name, struct regmap *map, unsigned int reg,
			    unsigned int mask, unsigned int val)
{
	return return_i2c_result(name, regmap_update_bits(map, reg, mask, val));
}
EXPORT_SYMBOL(fiio_regmap_update_bits);

int fiio_regmap_raw_write(const char *name, struct regmap *map, unsigned int reg,
			  const void *val, size_t val_len)
{
	return return_i2c_result(name, regmap_raw_write(map, reg, val, val_len));
}
EXPORT_SYMBOL(fiio_regmap_raw_write);
