// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO primary module: shared registries, notifier chains and helpers used
 * by the other FiiO drivers. The module itself does nothing at load time.
 *
 * Reconstructed from the stock FiiO M21 FW 1.1.0 fiio_primary_modules.ko
 * (drivers/fiio_modules/primary_modules/).
 */
#include <linux/module.h>

static int __init fiio_primary_init(void)
{
	return 0;
}

static void __exit fiio_primary_exit(void)
{
}

module_init(fiio_primary_init);
module_exit(fiio_primary_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("pengweizhong@fiio.net");
MODULE_DESCRIPTION("fiio primary driver");
