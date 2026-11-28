#ifndef __QUECTEL_CCI_CAM_H__
#define __QUECTEL_CCI_CAM_H__

struct quectel_cci_interface {
	int32_t (*cci_io_init)(struct camera_io_master *io_master_info);
	int32_t (*cci_io_release)(struct camera_io_master *io_master_info);
	int32_t (*cci_io_dev_write)(struct camera_io_master *io_master_info, struct cam_sensor_i2c_reg_setting *write_setting);
	int32_t (*cci_io_dev_write_continuous)(struct camera_io_master *io_master_info, struct cam_sensor_i2c_reg_setting *write_setting, uint8_t cam_sensor_i2c_write_flag);
	int32_t (*cci_io_dev_read)(struct camera_io_master *io_master_info, uint32_t addr, uint32_t *data, enum camera_sensor_i2c_type addr_type, enum camera_sensor_i2c_type data_type);
	int32_t (*cci_io_dev_read_seq)(struct camera_io_master *io_master_info, uint32_t addr, uint8_t *data, enum camera_sensor_i2c_type addr_type, enum camera_sensor_i2c_type data_type, int32_t num_bytes);
	int (*test_func)(int);
};

int quectel_register_cci_interface(struct quectel_cci_interface *cci_interface);
typedef int (*subdev_probe)(void *data);

int quectel_cci_register_subdev(subdev_probe probe, const char *dev_name, void *data);
bool quectel_cci_load_result(void);
int32_t quectel_cci_io_init(struct camera_io_master *io_master_info);
int32_t quectel_cci_io_release(struct camera_io_master *io_master_info);
int32_t quectel_cci_io_dev_write(struct camera_io_master *io_master_info,
	struct cam_sensor_i2c_reg_setting *write_setting);
int32_t quectel_cci_io_dev_write_continuous(struct camera_io_master *io_master_info,
	struct cam_sensor_i2c_reg_setting *write_setting,
	uint8_t cam_sensor_i2c_write_flag);
int32_t quectel_cci_io_dev_read(struct camera_io_master *io_master_info,
	uint32_t addr, uint32_t *data,
	enum camera_sensor_i2c_type addr_type,
	enum camera_sensor_i2c_type data_type);
int32_t quectel_cci_io_dev_read_seq(struct camera_io_master *io_master_info,
	uint32_t addr, uint8_t *data,
	enum camera_sensor_i2c_type addr_type,
	enum camera_sensor_i2c_type data_type,
	int32_t num_bytes);

#endif
