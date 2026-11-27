// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO wakeup module: while the screen is off and Wi-Fi is on, holds a wakelock
 * after a few suspend/resume cycles and keeps sending SYSTEM_POWER_SAME=ON
 * uevents until userspace releases it through sysfs.
 *
 * Reconstructed from the stock FiiO M21 FW 1.1.0 fiio_wakeup.ko
 * (drivers/fiio_modules/misc/fiio_wakeup.c).
 */
#define pr_fmt(fmt) "[fiio_driver]: " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/pm_wakeup.h>
#include <fiio/fiio_attr.h>
#include <fiio/fiio_utils.h>

#define FIIO_WAKEUP_NAME	"fiio_wakeup_module"
/* uevent period while the lock is held */
#define FIIO_WAKEUP_EVENT_MS	500
/* suspend cycles with the screen off before the lock is taken */
#define FIIO_WAKEUP_SUSPEND_MIN	3

struct fiio_wakeup_module {
	/* platform device, also the uevent source */
	struct device *dev;
	/* suspends since the screen went off */
	int screen_off_suspend_counter;
	/* sends SYSTEM_POWER_SAME=ON while send_event_flag is set */
	struct delayed_work event_work;
	/* screen_state: ON / OFF */
	bool is_screen_off;
	/* event_work keeps rescheduling itself */
	bool send_event_flag;
	/* active_wakeup: CONNECTED / DISCONNECTED */
	bool is_suspend_connected;
	/* active_wakeup: SCREEN_OFF_OPEN / SCREEN_OFF_CLOSE */
	bool is_wifi_open;
};
static struct fiio_wakeup_module *g_data;

/* held from resume until userspace writes RELEASE or ON */

static struct wakeup_source *fiiok_wakelock;
static int fiiok_wakelock_counter;

/* register the wakeup source that keeps the system awake */
static void fiio_init_wakeup_lock(void)
{
	fiiok_wakelock = wakeup_source_register(NULL, "fiio_wakelock_power");
	if (!fiiok_wakelock) {
		pr_err("[%s] fiio_wakelock_power register failed\n", __func__);
		return;
	}

	pr_err("%s:register wakelock fiio_wakelock_power successfully!\n", __func__);
}

/*
 * take the wakelock once; a second stay while it is held only logs.
 * stay_mode_name says who asked.
 */


static void fiio_wakeup_stay_lock(char *stay_mode_name)
{
	pr_err("%s: Enter stay mode : %s counter: %d\n", __func__,
	       stay_mode_name, fiiok_wakelock_counter);

	if (fiiok_wakelock_counter == 0) {
		__pm_stay_awake(fiiok_wakelock);
		fiiok_wakelock_counter++;
	} else {
		pr_err("%s: System already have wakelock,so you can't stay again!\n", __func__);
	}
}

/*
 * drop the wakelock if it is held; relax_mode_name says who asked.
 */



static void fiio_wakeup_relax_lock(char *relax_mode_name)
{
	pr_err("%s: Enter relax mode : %s counter: %d\n", __func__,
	       relax_mode_name, fiiok_wakelock_counter);

	if (fiiok_wakelock_counter > 0) {
		__pm_relax(fiiok_wakelock);
		fiiok_wakelock_counter--;
	} else {
		pr_err("%s: System have no wakelock to relax!\n", __func__);
	}
}

/*
 * log the active wakeup sources and return how many there are
 * beyond the three that are always active
 */



static int show_active_wakeup(void)
{
	int ret;
	char active_wakeup_sources[2048];
	char tmp_active_wakeup_sources[2048];
	/* pm_get_active_wakeup_sources() lists names separated by spaces */



	int i = 0, j;
	char tmp[100][32];
	char *p1;
	char *p = active_wakeup_sources;

	memset(active_wakeup_sources, 0, sizeof(active_wakeup_sources));
	pm_get_active_wakeup_sources(active_wakeup_sources, sizeof(active_wakeup_sources));
	memset(tmp, 0, sizeof(tmp));
	while ((p1 = strchr(p, ' ')) != NULL) {
		int len = strlen(p) - strlen(p1);

		strncpy(tmp[i], p, len > 31 ? 32 : len);
		p = p1 + 1;
		i++;
	}

	for (j = 0; j < i; j++)
		pr_err("wakeup_source[%d] = %s\n", j, tmp[j]);

	ret = i - 3;
	(void)tmp_active_wakeup_sources;

	return ret;
}

static ssize_t screen_state_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	int val;
	int ret;
	struct fiio_wakeup_module *data = g_data;

	ret = sprintf(buf, "%s\n", data->is_screen_off ? "OFF" : "ON");
	(void)val;
	return ret;
}

static ssize_t screen_state_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct fiio_wakeup_module *data = g_data;

	pr_err("%s: %s\n", __func__, buf);
	if (!strcmp(buf, "ON\n")) {
		data->is_screen_off = false;
		data->screen_off_suspend_counter = 0;
		fiio_wakeup_relax_lock(FIIO_WAKEUP_NAME);
		data->send_event_flag = false;
	} else {
		data->is_screen_off = true;
	}

	return count;
}

static ssize_t active_wakeup_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	int val;
	struct fiio_wakeup_module *data = g_data;

	int ret = show_active_wakeup();

	(void)val; (void)data;
	return sprintf(buf, "active wakeup counter = %d\n", ret);
}

static ssize_t active_wakeup_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct fiio_wakeup_module *data = g_data;

	pr_err("%s: %s\n", __func__, buf);
	if (!strcmp(buf, "RELEASE\n")) {
		fiio_wakeup_relax_lock(FIIO_WAKEUP_NAME);
		data->send_event_flag = false;
	} else if (!strcmp(buf, "DISCONNECTED\n")) {
		data->is_suspend_connected = false;
	} else if (!strcmp(buf, "CONNECTED\n")) {
		data->is_suspend_connected = true;
	} else if (!strcmp(buf, "SCREEN_OFF_CLOSE\n")) {
		data->is_wifi_open = false;
	} else if (!strcmp(buf, "SCREEN_OFF_OPEN\n")) {
		data->is_wifi_open = true;
	} else {
		data->is_screen_off = true;
	}

	return count;
}

static const struct device_attribute fiio_wakeup_module_attributes[] = {
	__ATTR(screen_state, 0644, screen_state_show, screen_state_store),
	__ATTR(active_wakeup, 0644, active_wakeup_show, active_wakeup_store),
};

static void fiio_check_wakeup_work(struct work_struct *work)
{
	struct fiio_wakeup_module *data =
		container_of(to_delayed_work(work), struct fiio_wakeup_module, event_work);

	fiio_sendMessage(data->dev, "SYSTEM_POWER_SAME=ON", NULL);
	if (data->send_event_flag)
		schedule_delayed_work(&data->event_work, msecs_to_jiffies(FIIO_WAKEUP_EVENT_MS));
}

static int fiio_wakeup_module_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node;
	struct device_node *child;
	struct fiio_wakeup_module *data;
	int ret;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	pr_err("%s\n", __func__);
	data->dev = dev;
	data->screen_off_suspend_counter = 0;
	data->is_screen_off = false;
	data->send_event_flag = false;
	data->is_suspend_connected = false;
	data->is_wifi_open = false;
	platform_set_drvdata(pdev, data);
	g_data = data;

	fiio_init_wakeup_lock();
	INIT_DELAYED_WORK(&data->event_work, fiio_check_wakeup_work);

	ret = fiio_device_attribute_register(fiio_wakeup_module_attributes,
					     ARRAY_SIZE(fiio_wakeup_module_attributes));
	if (ret) {
		printk("attribute register failed!!");
		fiio_device_attribute_unregister(fiio_wakeup_module_attributes);
		return -ENOMEM;
	}

	(void)node; (void)child;
	return 0;
}

static int fiio_wakeup_module_remove(struct platform_device *pdev)
{
	struct device *dev = NULL;

	fiio_device_attribute_unregister(fiio_wakeup_module_attributes);
	(void)dev;
	return 0;
}

static int fiio_wakeup_module_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct fiio_wakeup_module *data = g_data;

	data->screen_off_suspend_counter++;
	pr_err("%s:data->screen_off_suspend_counter %d\n", __func__, data->screen_off_suspend_counter);
	if (data->screen_off_suspend_counter > 0xffff)
		data->screen_off_suspend_counter = 0;

	return 0;
}

static int fiio_wakeup_module_resume(struct platform_device *pdev)
{
	struct fiio_wakeup_module *data = g_data;

	pr_err("%s:data->screen_off_suspend_counter %d\n", __func__, data->screen_off_suspend_counter);
	if (data->screen_off_suspend_counter > FIIO_WAKEUP_SUSPEND_MIN &&
	    !data->is_suspend_connected && data->is_wifi_open) {
		fiio_wakeup_stay_lock(FIIO_WAKEUP_NAME);
		data->send_event_flag = true;
		schedule_delayed_work(&data->event_work, msecs_to_jiffies(FIIO_WAKEUP_EVENT_MS));
	}

	return 0;
}

static struct of_device_id fiio_wakeup_module_match_table_device[] = {
	{ .compatible = "qcom,fiio-wakeup-module" },
	{},
};

static struct platform_driver fiio_wakeup_module_driver = {
	.probe = fiio_wakeup_module_probe,
	.remove = fiio_wakeup_module_remove,
	.suspend = fiio_wakeup_module_suspend,
	.resume = fiio_wakeup_module_resume,
	.driver = {
		.name = "qcom,fiio-wakeup-module",
		.owner = THIS_MODULE,
		.of_match_table = fiio_wakeup_module_match_table_device,
	},
};

static int __init fiio_wakeup_module_init(void)
{
	printk("%s\n", __func__);
	return platform_driver_register(&fiio_wakeup_module_driver);
}

static void __exit fiio_wakeup_module_exit(void)
{
	printk("%s\n", __func__);
	platform_driver_unregister(&fiio_wakeup_module_driver);
}

module_init(fiio_wakeup_module_init);
module_exit(fiio_wakeup_module_exit);

MODULE_AUTHOR("pengweizhong@fiio.net");
MODULE_DESCRIPTION("fiio wakeup module");
MODULE_LICENSE("GPL");
