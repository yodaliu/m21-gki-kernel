#ifndef __GPIOLIB_QUECTEL_KERNEL_H__
#define __GPIOLIB_QUECTEL_KERNEL_H__

#include <linux/gpiolib_quectel_intf.h>

struct quectel_gpio_subdev {
	struct device *dev;
	const char *label;
	uint16_t n_gpio;
	int (*gpio_direction_input)(struct device *dev, unsigned int offset, unsigned int flags);
	int (*gpio_direction_output)(struct device *dev, unsigned int offset, int value, unsigned int flags);
	int (*gpio_set)(struct device *dev, unsigned int offset, int value);
	int (*gpio_get)(struct device *dev, unsigned int offset);
	int (*register_irq_queue)(struct device *dev, unsigned int offset, irq_func handler, unsigned int flags, void *data);
	int (*enable_irq)(struct device *dev, unsigned int offset);
	int (*disable_irq)(struct device *dev, unsigned int offset);
	int (*enable_irq_wake)(struct device *dev, unsigned int offset);
	int (*disable_irq_wake)(struct device *dev, unsigned int offset);
};

int quectel_gpio_register_dev(struct quectel_gpio_subdev *subdev);

#endif
