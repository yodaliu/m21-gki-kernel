// SPDX-License-Identifier: GPL-2.0
/* Product parameters: the LCD panel picked from the kernel command line. */
#include <linux/module.h>
#include <linux/string.h>

#include <fiio/fiio_utils.h>

struct product_params_data {
	char panel_name[128];
};

static struct product_params_data g_product_params_data;

void set_product_lcd_panel_name(const char *panel_name)
{
	strcpy(g_product_params_data.panel_name, panel_name);
}
EXPORT_SYMBOL(set_product_lcd_panel_name);

char *get_product_lcd_panel_name(void)
{
	return g_product_params_data.panel_name;
}
EXPORT_SYMBOL(get_product_lcd_panel_name);
