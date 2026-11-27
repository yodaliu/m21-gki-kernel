// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO SPI test driver: fill a buffer with a walking-bit pattern through
 * sysfs and clock it out over the SPI device named "fiio,spitest".
 *
 * Reconstructed from the stock FiiO M21 FW 1.1.0 spi_test_driver.ko, which
 * was started from the AD7280A driver and keeps its module author and
 * description.
 */
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/of.h>

#include <fiio/fiio_attr.h>

/*
 * Pattern written to the tx buffer, then the buffers and the client.
 */
static const char g_data_array[8] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };
static int g_buf_len;
static char *g_buf_w;
static char *g_buf_r;
static struct spi_device *gp_client;

static void dump_data(struct device *dev)
{
	int i;

	dev_err(dev, "-----------tx------------\n");
	for (i = 0; i < g_buf_len; i++)
		dev_err(dev, "[%d] 0x%x", i, g_buf_w[i]);
	dev_err(dev, "\n-----------rx------------\n");
	for (i = 0; i < g_buf_len; i++)
		dev_err(dev, "[%d] 0x%x", i, g_buf_r[i]);
	dev_err(dev, "\n-------------------------\n");
}

/* "r": rx only, "t": tx only, "d": dump the buffers, else full duplex */

static ssize_t show_temp(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	int ret = 0;

	struct spi_transfer t = {
		.tx_buf = g_buf_w,
		.rx_buf = g_buf_r,
		.len = g_buf_len,
	};

	if (!gp_client) {
		dev_err(dev, "invalid gp_client\n");
		return 1;
	}

	if (!g_buf_r || !g_buf_w) {
		dev_err(dev, "You need do like 'echo 16 > set' to set the buf\n");
		return 1;
	}

	if (buf[0] == 'r') {
		t.tx_buf = NULL;
		dev_err(dev, "rx\n");
	} else if (buf[0] == 't') {
		t.rx_buf = NULL;
		dev_err(dev, "tx\n");
	} else if (buf[0] == 'd') {
		/* the buffers of the previous transfer */
		dump_data(dev);
	} else {
		dev_err(dev, "txrx:%c\n", buf[0]);
	}
	spi_sync_transfer(gp_client, &t, 1);
	dev_err(dev, "len:%d--rx_buf:%p--tx_buf:%p ret:%d\n", t.len, t.rx_buf,
		t.tx_buf, ret);

	/* then the same buffer as plain writes */
	spi_write(gp_client, g_buf_w, 1);
	spi_write(gp_client, g_buf_w, 1);
	spi_write(gp_client, g_buf_w, g_buf_len);

	return 1;
}

static ssize_t set_set(struct device *dev,
		       struct device_attribute *attr,
		       const char *buf, size_t count)
{
	int i;

	g_buf_len = simple_strtoul(buf, NULL, 10);
	dev_err(dev, "%s--%d value:%d\n", __func__, __LINE__, g_buf_len);
	if (!g_buf_len)
		return 1;
	if (g_buf_w) {
		kfree(g_buf_w);
		g_buf_w = NULL;
	}
	if (g_buf_r) {
		kfree(g_buf_r);
		g_buf_r = NULL;
	}
	g_buf_w = kzalloc(g_buf_len, GFP_KERNEL | GFP_DMA);
	if (!g_buf_w) {
		dev_err(dev, "%s--%d g_buf_w kmalloc error\n", __func__, __LINE__);
	} else {
		for (i = 0; i < g_buf_len; i++)
			g_buf_w[i] = g_data_array[i % 8];
	}

	/* rx buffer of the same size */
	g_buf_r = kzalloc(g_buf_len, GFP_KERNEL | GFP_DMA);
	if (!g_buf_r)
		dev_err(dev, "%s--%d g_buf_r kmalloc error\n", __func__, __LINE__);

	return count;
}


/* echo <len> > spitest_ctrl; echo <r|t|d|x> > spitest_temp */

static const struct device_attribute spitest_attributes[] = {
	__ATTR(spitest_ctrl, 0200, NULL, set_set),
	__ATTR(spitest_temp, 0200, NULL, show_temp),
};

static int spitest_probe(struct spi_device *spi)
{
	int ret;

	dev_err(&spi->dev, "%s--%d\n", __func__, __LINE__);
	gp_client = spi;
	ret = fiio_device_attribute_register(spitest_attributes,
					     ARRAY_SIZE(spitest_attributes));
	if (ret) {
		dev_err(&spi->dev, "%s--%d\n", __func__, __LINE__);
		return -1;
	}

	return 0;
}

static int spitest_remove(struct spi_device *spi)
{
	dev_err(&spi->dev, "%s--%d\n", __func__, __LINE__);
	fiio_device_attribute_unregister(spitest_attributes);
	return 0;
}

static const struct of_device_id spitest_dt_ids[] = {
	{ .compatible = "fiio,spitest" },
	{},
};

static struct spi_driver spitest_spi_driver = {
	.driver = {
		.name = "spitest",
		.owner = THIS_MODULE,
		.of_match_table = spitest_dt_ids,
	},
	.probe = spitest_probe,
	.remove = spitest_remove,
};

module_spi_driver(spitest_spi_driver);

MODULE_AUTHOR("Michael Hennerich <hennerich@blackfin.uclinux.org>");
MODULE_DESCRIPTION("Analog Devices AD7280A");
MODULE_LICENSE("GPL v2");
