// SPDX-License-Identifier: GPL-2.0+
/*
 * FiiO dummy ASoC codec/platform: the dummy DAI from sound/soc/soc-utils.c,
 * renamed and bound through the device tree ("fiio,snd-soc-dummy").
 *
 * Reconstructed from the stock FiiO M21 FW 1.1.0 fiio_snd_soc_dummy_dai.ko
 * (drivers/fiio_modules/codecs/fiio_snd_soc_dummy_dai.c).
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

static const struct snd_pcm_hardware dummy_dma_hardware = {
	/* Random values to keep userspace happy when checking constraints */
	.info			= SNDRV_PCM_INFO_INTERLEAVED |
				  SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.buffer_bytes_max	= 128*1024,
	.period_bytes_min	= PAGE_SIZE,
	.period_bytes_max	= PAGE_SIZE*2,
	.periods_min		= 2,
	.periods_max		= 128,
};

static int dummy_dma_open(struct snd_soc_component *component,
			  struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = asoc_substream_to_rtd(substream);

	/* BE's dont need dummy params */
	if (!rtd->dai_link->no_pcm)
		snd_soc_set_runtime_hwparams(substream, &dummy_dma_hardware);

	return 0;
}

static const struct snd_soc_component_driver dummy_platform = {
	.open		= dummy_dma_open,
};

static const struct snd_soc_component_driver dummy_codec = {
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 1,
	.non_legacy_dai_naming	= 1,
};

#define STUB_RATES	SNDRV_PCM_RATE_8000_384000
#define STUB_FORMATS	(SNDRV_PCM_FMTBIT_S8 | \
			SNDRV_PCM_FMTBIT_U8 | \
			SNDRV_PCM_FMTBIT_S16_LE | \
			SNDRV_PCM_FMTBIT_U16_LE | \
			SNDRV_PCM_FMTBIT_S24_LE | \
			SNDRV_PCM_FMTBIT_S24_3LE | \
			SNDRV_PCM_FMTBIT_U24_LE | \
			SNDRV_PCM_FMTBIT_S32_LE | \
			SNDRV_PCM_FMTBIT_U32_LE | \
			SNDRV_PCM_FMTBIT_IEC958_SUBFRAME_LE)

/*
 * Select these from Sound Card Manually
 *	SND_SOC_POSSIBLE_DAIFMT_CBP_CFP
 *	SND_SOC_POSSIBLE_DAIFMT_CBP_CFC
 *	SND_SOC_POSSIBLE_DAIFMT_CBC_CFP
 *	SND_SOC_POSSIBLE_DAIFMT_CBC_CFC
 */
static u64 dummy_dai_formats =
	SND_SOC_POSSIBLE_DAIFMT_I2S	|
	SND_SOC_POSSIBLE_DAIFMT_RIGHT_J	|
	SND_SOC_POSSIBLE_DAIFMT_LEFT_J	|
	SND_SOC_POSSIBLE_DAIFMT_DSP_A	|
	SND_SOC_POSSIBLE_DAIFMT_DSP_B	|
	SND_SOC_POSSIBLE_DAIFMT_AC97	|
	SND_SOC_POSSIBLE_DAIFMT_PDM	|
	SND_SOC_POSSIBLE_DAIFMT_GATED	|
	SND_SOC_POSSIBLE_DAIFMT_CONT	|
	SND_SOC_POSSIBLE_DAIFMT_NB_NF	|
	SND_SOC_POSSIBLE_DAIFMT_NB_IF	|
	SND_SOC_POSSIBLE_DAIFMT_IB_NF	|
	SND_SOC_POSSIBLE_DAIFMT_IB_IF;

static const struct snd_soc_dai_ops dummy_dai_ops = {
	.auto_selectable_formats	= &dummy_dai_formats,
	.num_auto_selectable_formats	= 1,
};

/*
 * The dummy CODEC is only meant to be used in situations where there is no
 * actual hardware.
 *
 * If there is actual hardware even if it does not have a control bus
 * the hardware will still have constraints like supported samplerates, etc.
 * which should be modelled. And the data flow graph also should be modelled
 * using DAPM.
 */
static struct snd_soc_dai_driver dummy_dai = {
	.name = "fiio-snd-soc-dummy-dai",
	.playback = {
		.stream_name	= "Playback",
		.channels_min	= 1,
		.channels_max	= 384,
		.rates		= STUB_RATES,
		.formats	= STUB_FORMATS,
	},
	.capture = {
		.stream_name	= "Capture",
		.channels_min	= 1,
		.channels_max	= 384,
		.rates = STUB_RATES,
		.formats = STUB_FORMATS,
	 },
	.ops = &dummy_dai_ops,
};

int snd_soc_dai_is_dummy(struct snd_soc_dai *dai)
{
	if (dai->driver == &dummy_dai)
		return 1;
	return 0;
}

int snd_soc_component_is_dummy(struct snd_soc_component *component)
{
	return ((component->driver == &dummy_platform) ||
		(component->driver == &dummy_codec));
}

static int snd_soc_dummy_probe(struct platform_device *pdev)
{
	int ret;

	printk("%s:Enter!\n", __func__);
	ret = devm_snd_soc_register_component(&pdev->dev,
					      &dummy_codec, &dummy_dai, 1);
	if (ret < 0)
		return ret;

	ret = devm_snd_soc_register_component(&pdev->dev, &dummy_platform,
					      NULL, 0);
	printk("%s:Exit!\n", __func__);

	return ret;
}

static const struct of_device_id soc_dummy_dt_match[] = {
	{ .compatible = "fiio,snd-soc-dummy" },
	{}
};
MODULE_DEVICE_TABLE(of, soc_dummy_dt_match);

static struct platform_driver soc_dummy_driver = {
	.driver = {
		.name = "fiio-snd-soc-dummy",
		.of_match_table = soc_dummy_dt_match,
	},
	.probe = snd_soc_dummy_probe,
};

static int __init fiio_dummy_dai_init(void)
{
	return platform_driver_register(&soc_dummy_driver);
}

static void __exit fiio_dummy_dai_exit(void)
{
	platform_driver_unregister(&soc_dummy_driver);
}

module_init(fiio_dummy_dai_init);
module_exit(fiio_dummy_dai_exit);

MODULE_AUTHOR("pengweizhong@fiio.net");
MODULE_DESCRIPTION("fiio snd soc dummy dai driver");
MODULE_LICENSE("GPL v2");
