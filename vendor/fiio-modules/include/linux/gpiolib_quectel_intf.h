#ifndef __GPIOLIB_QUECTEL_INTF_H__
#define __GPIOLIB_QUECTEL_INTF_H__

#include <linux/types.h>

struct device;
struct device_node;

/*
 * GPIO interface exported by gpiolib-quectel. A gpio number is the index in
 * the global map built from every registered quectel_gpio_subdev, so it is
 * only stable for a fixed probe order.
 */

typedef void (*irq_func)(unsigned gpio, void *data);

int quectel_get_named_gpio(struct device_node *np, const char *propname, int index);
int quectel_gpio_direction_input(unsigned gpio, unsigned int flags);
int quectel_gpio_direction_output(unsigned gpio, int value, unsigned int flags);
int quectel_gpio_set(unsigned gpio, int value);
int quectel_gpio_get(unsigned gpio);
int quectel_request_gpio_irq(unsigned gpio, irq_func handler, unsigned int flags, void *data);
int quectel_gpio_enable_irq(unsigned gpio);
int quectel_gpio_disable_irq(unsigned gpio);
int quectel_gpio_enable_irq_wake(unsigned gpio);
int quectel_gpio_disable_irq_wake(unsigned gpio);

#endif
