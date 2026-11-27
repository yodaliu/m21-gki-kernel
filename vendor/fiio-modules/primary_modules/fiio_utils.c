// SPDX-License-Identifier: GPL-2.0
/* uevent helpers and string-to-number helpers. */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/ctype.h>
#include <linux/kobject.h>
#include <linux/string.h>

#include <fiio/fiio_utils.h>

void fiio_sendMessage(struct device *dev, char *buf1, char *buf2)
{
	char *envp[] = { "KERNELEVENT=change", buf1, buf2, NULL };

	kobject_uevent_env(&dev->kobj, KOBJ_CHANGE, envp);
}
EXPORT_SYMBOL(fiio_sendMessage);

void report_playing_info(struct device *dev, int format, int sample_rate)
{
	char buf1[20] = {0};
	char buf2[20] = {0};

	sprintf(buf1, "SAMPLE_RATE=%d", sample_rate);
	sprintf(buf2, "FORMAT=%d", format);
	fiio_sendMessage(dev, buf1, buf2);
}
EXPORT_SYMBOL(report_playing_info);

struct file *fiio_filp_open(const char *filename, int flags, umode_t mode)
{
	return NULL;
}
EXPORT_SYMBOL(fiio_filp_open);

/* decimal after a sign, hex with a trailing 'h', otherwise auto-detect */
s32 atoi(const char *psz_buf)
{
	const char *pch = psz_buf;
	s32 base = 0;

	while (isspace(*pch))
		pch++;

	if (*pch == '-' || *pch == '+') {
		base = 10;
		pch++;
	} else if (*pch && tolower(pch[strlen(pch) - 1]) == 'h') {
		base = 16;
	}

	return simple_strtoul(pch, NULL, base);
}
EXPORT_SYMBOL(atoi);

int htoi(const char *s)
{
	int i;
	int n = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		i = 2;
	else
		i = 0;

	for (; (s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'z') ||
	       (s[i] >= 'A' && s[i] <= 'Z'); ++i) {
		if (tolower(s[i]) > '9')
			n = 16 * n + (10 + tolower(s[i]) - 'a');
		else
			n = 16 * n + (tolower(s[i]) - '0');
	}

	return n;
}
EXPORT_SYMBOL(htoi);
