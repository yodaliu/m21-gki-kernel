/* SPDX-License-Identifier: GPL-2.0 */
/*
 * FiiO device registry (fiio_primary_modules). Layouts from the stock DWARF.
 */
#ifndef _FIIO_DEVICE_H
#define _FIIO_DEVICE_H

#include <linux/list.h>

/*
 * A driver registers itself by name so others can find its private data.
 */
struct fiio_device_driver {
	const char *name;
	void *private_data;
	/* deferred work run by the owner */
	void (*load_work)(void);
};

/*
 * Registry entry.
 */
struct fiio_device {
	struct fiio_device_driver *driver;
	/* on device_list */
	struct list_head list;
};

extern struct list_head device_list;

int fiio_device_register(struct fiio_device_driver *drv);
void fiio_device_unregister(struct fiio_device_driver *drv);
struct fiio_device_driver *fiio_device_get_by_name(const char *name);
void *fiio_device_get_data_by_name(const char *name);

#endif
