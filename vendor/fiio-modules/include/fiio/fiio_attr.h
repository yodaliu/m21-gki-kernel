/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FIIO_ATTR_H
#define _FIIO_ATTR_H

#include <linux/device.h>

struct fiio_device_attribute {
	const struct device_attribute *attrs;
	unsigned int num_attr;
	/* on g_fiio_attrs_list */
	struct list_head list;
};

extern struct list_head g_fiio_attrs_list;

int fiio_device_attribute_register(const struct device_attribute *attrs,
				   unsigned int num_attr);
void fiio_device_attribute_unregister(const struct device_attribute *attrs);

#endif
