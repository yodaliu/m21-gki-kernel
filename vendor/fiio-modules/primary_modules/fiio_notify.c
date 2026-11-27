// SPDX-License-Identifier: GPL-2.0
/* Blocking notifier chains shared by the FiiO drivers. */
#include <linux/module.h>
#include <linux/notifier.h>

#include <fiio/fiio_notify.h>

static BLOCKING_NOTIFIER_HEAD(notifier_head_headset);
static BLOCKING_NOTIFIER_HEAD(notifier_head_dac);
static BLOCKING_NOTIFIER_HEAD(notifier_head_light);
static BLOCKING_NOTIFIER_HEAD(notifier_head_player);
static BLOCKING_NOTIFIER_HEAD(notifier_head_charger);
static BLOCKING_NOTIFIER_HEAD(notifier_head_sw_hold);
static BLOCKING_NOTIFIER_HEAD(notifier_head_system_status);
static BLOCKING_NOTIFIER_HEAD(notifier_head_monitor);
static BLOCKING_NOTIFIER_HEAD(notifier_head_user_config);
static BLOCKING_NOTIFIER_HEAD(notifier_head_output_mode);
static BLOCKING_NOTIFIER_HEAD(notifier_head_fpga);
static BLOCKING_NOTIFIER_HEAD(notifier_head_misc_config);
static BLOCKING_NOTIFIER_HEAD(notifier_head_vbus_detection_fiio);
static BLOCKING_NOTIFIER_HEAD(notifier_head_aw32271);
static BLOCKING_NOTIFIER_HEAD(notifier_head_vbus_detection_qti);
static BLOCKING_NOTIFIER_HEAD(notifier_head_dmode_irq);
static BLOCKING_NOTIFIER_HEAD(notifier_head_usb_otg);
static BLOCKING_NOTIFIER_HEAD(notifier_head_uart);
static BLOCKING_NOTIFIER_HEAD(notifier_head_gpio_key);

/* indexed by enum fiio_notifier_type; the pump_irq slot is the AW32271 chain */
static struct blocking_notifier_head * const fiio_notifier_head_list[] = {
	&notifier_head_headset,
	&notifier_head_dac,
	&notifier_head_light,
	&notifier_head_player,
	&notifier_head_charger,
	&notifier_head_sw_hold,
	&notifier_head_system_status,
	&notifier_head_monitor,
	&notifier_head_user_config,
	&notifier_head_output_mode,
	&notifier_head_fpga,
	&notifier_head_misc_config,
	&notifier_head_vbus_detection_qti,
	&notifier_head_vbus_detection_fiio,
	&notifier_head_aw32271,
	&notifier_head_dmode_irq,
	&notifier_head_usb_otg,
	&notifier_head_uart,
	&notifier_head_gpio_key,
};

int fiio_register_client(enum fiio_notifier_type index, struct notifier_block *nb)
{
	return blocking_notifier_chain_register(fiio_notifier_head_list[index], nb);
}
EXPORT_SYMBOL(fiio_register_client);

int fiio_unregister_client(enum fiio_notifier_type index, struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(fiio_notifier_head_list[index], nb);
}
EXPORT_SYMBOL(fiio_unregister_client);

int fiio_notifier_call_chain(enum fiio_notifier_type index, unsigned long val, void *v)
{
	return blocking_notifier_call_chain(fiio_notifier_head_list[index], val, v);
}
EXPORT_SYMBOL(fiio_notifier_call_chain);
