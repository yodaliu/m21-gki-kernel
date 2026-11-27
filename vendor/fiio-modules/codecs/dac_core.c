// SPDX-License-Identifier: GPL-2.0
/*
 * FiiO ALSA DAC core (dac_core_module).
 * Reconstructed from the stock FiiO M21 module.
 */
#define pr_fmt(fmt) "[dac_core]: " fmt

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/workqueue.h>
#include <linux/pm_wakeup.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <fiio/fiio_notify.h>
#include <fiio/fiio_device.h>
#include <fiio/fiio_attr.h>
#include <fiio/fiio_i2c.h>
#include <fiio/fiio_utils.h>
#include <fiio/dac_core.h>

static int soc_i2s_use_one_line;
module_param(soc_i2s_use_one_line, int, 0644);

static struct snd_dac *g_dac;

static void snd_dac_cancel_poweroff(struct snd_dac *dac)
{
	int ret;
	int i;

	ret = work_busy(&dac->snd_dac_poweroff_delay_work.work);
	if (ret & WORK_BUSY_PENDING) {
		pr_err("%s: snd_dac_poweroff_delay_work WORK_BUSY_PENDING cancel it\n", __func__);
		cancel_delayed_work_sync(&dac->snd_dac_poweroff_delay_work);
	}
	if (ret & WORK_BUSY_RUNNING) {
		pr_err("%s: snd_dac_poweroff_delay_work WORK_BUSY_RUNNING wait it to finish\n", __func__);
		for (i = 0; i < 20; i++) {
			pr_err("%s: check snd_dac_poweroff_delay_work WORK_BUSY_RUNNING wait it to finish i = %d\n",
			       __func__, i);
			mdelay(10);
			if (!(work_busy(&dac->snd_dac_poweroff_delay_work.work) & WORK_BUSY_RUNNING))
				break;
		}
	}
}

static bool is_xmos_hdmi_power_on(struct snd_dac_dai *dac_dai)
{
	struct snd_dac *dac = g_dac;
	bool ret = false;

	if (dac->xmos_module && dac->xmos_module->power_on_state) {
		if (dac->xmos_module->power_on_state() &&
		    dac_dai->system_work_mode == SYSTEM_MODE_XMOS) {
			pr_err("%s xmos or hdmi is power on\n", __func__);
			ret = true;
		}
	}
	return ret;
}

static void snd_dac_report_dac_state(enum dac_event_type type)
{
	struct dac_notify_data notify_data = { 0 };
	struct snd_dac *dac = g_dac;

	notify_data.event = type;
	notify_data.is_local_playing = dac->is_local_playing;
	notify_data.is_dac_mute = dac->is_dac_mute;
	fiio_notifier_call_chain(notifier_dac, 0, &notify_data);
}

static void snd_dac_dsd_cancel_unmute_work(struct snd_dac *dac)
{
	int ret;
	int i;

	ret = work_busy(&dac->snd_dac_dsd_unmute_delay_work.work);
	if (ret & WORK_BUSY_PENDING) {
		pr_err(KERN_WARNING "%s: snd_dac_dsd_unmute_delay_work WORK_BUSY_PENDING cancel it\n", __func__);
		cancel_delayed_work_sync(&dac->snd_dac_dsd_unmute_delay_work);
	}
	if (ret & WORK_BUSY_RUNNING) {
		pr_err("%s: snd_dac_dsd_unmute_delay_work WORK_BUSY_RUNNING wait it to finish\n", __func__);
		_Pragma("clang loop unroll(disable)")
		for (i = 0; i < 10; i++) {
			pr_err("%s: snd_dac_dsd_unmute_delay_work WORK_BUSY_RUNNING wait it to finish i = %d\n",
			       __func__, i);
			mdelay(5);
			ret = work_busy(&dac->snd_dac_dsd_unmute_delay_work.work);
			if (!(ret & WORK_BUSY_RUNNING))
				break;
		}
	}
}

static void snd_dac_pcm_cancel_unmute_work(struct snd_dac *dac)
{
	int ret;
	int i;

	ret = work_busy(&dac->snd_dac_pcm_unmute_delay_work.work);
	if (ret & WORK_BUSY_PENDING) {
		pr_err(KERN_WARNING "%s: snd_dac_pcm_unmute_delay_work WORK_BUSY_PENDING cancel it\n", __func__);
		cancel_delayed_work_sync(&dac->snd_dac_pcm_unmute_delay_work);
	}
	if (ret & WORK_BUSY_RUNNING) {
		pr_err("%s: snd_dac_pcm_unmute_delay_work WORK_BUSY_RUNNING wait it to finish\n", __func__);
		_Pragma("clang loop unroll(disable)")
		for (i = 0; i < 10; i++) {
			pr_err("%s: snd_dac_pcm_unmute_delay_work WORK_BUSY_RUNNING wait it to finish i = %d\n",
			       __func__, i);
			mdelay(5);
			ret = work_busy(&dac->snd_dac_pcm_unmute_delay_work.work);
			if (!(ret & WORK_BUSY_RUNNING))
				break;
		}
	}
}

static int snd_dac_dai_mute(struct snd_dac *dac, int mute)
{
	struct snd_dac_dai *dac_dai = dac->dac_dai;
	int ret = 0;

	pr_err("%s mute:%s\n", __func__, mute ? "ON" : "OFF");

	if (mute) {
		snd_dac_dsd_cancel_unmute_work(dac);
		snd_dac_pcm_cancel_unmute_work(dac);
		if (dac->anolog_board_module)
			dac->anolog_board_module->is_unmuting = false;
		if (dac->anolog_board_module) {
			if (dac->anolog_board_module->is_muting) {
				pr_err("%s allready muting!\n", __func__);
				return ret;
			}
			if (dac->anolog_board_module->is_muted) {
				pr_err("%s allready muted!\n", __func__);
				return ret;
			}
			dac->anolog_board_module->is_muting = true;
		}
	} else {
		if (dac->anolog_board_module) {
			if (dac->anolog_board_module->is_unmuting) {
				pr_err("%s allready ummuting!\n", __func__);
				return ret;
			}
			if (dac->anolog_board_module->is_muting) {
				pr_err("%s allready muting!\n", __func__);
				return ret;
			}
			if (!dac->anolog_board_module->is_muted) {
				pr_err("%s allready unmuted!\n", __func__);
				return ret;
			}
			dac->anolog_board_module->is_unmuting = true;
		}
	}

	if (dac_dai->system_work_mode == SYSTEM_MODE_MTP && !dac_dai->is_dsd)
		dac->is_dac_mute = (mute == 1);

	if (mute == 1) {
		if (dac->anolog_board_module && dac->anolog_board_module->hw_mute)
			dac->anolog_board_module->hw_mute(1);
		report_playing_info(dac_dai->dev, 0, 0);
	} else if (mute == 0) {
		if (dac_dai->is_dsd)
			schedule_delayed_work(&dac->snd_dac_dsd_unmute_delay_work,
					      msecs_to_jiffies(dac->anolog_board_module->dsd_unmute_delaytime));
		else if (dac_dai->system_work_mode == SYSTEM_MODE_HDMI_TX_ARC ||
			 dac_dai->system_work_mode == SYSTEM_MODE_XMOS)
			schedule_delayed_work(&dac->snd_dac_pcm_unmute_delay_work,
					      msecs_to_jiffies(dac->anolog_board_module->dsd_unmute_delaytime));
		else
			schedule_delayed_work(&dac->snd_dac_pcm_unmute_delay_work,
					      msecs_to_jiffies(dac->anolog_board_module->dsd_unmute_delaytime));
	}

	return ret;
}

static void snd_dac_shutdown_time(struct snd_dac *dac, int delay_time)
{
	int ret;

	pr_err("%s delay_time:%d\n", __func__, delay_time);
	dac->is_playing_now = false;
	dac->dac_public->power_state = false;
	dac->dac_public->playing_state = false;
	dac->is_local_playing = false;
	dac->is_dac_mute = true;
	snd_dac_cancel_poweroff(dac);
	schedule_delayed_work(&dac->snd_dac_poweroff_delay_work, msecs_to_jiffies(delay_time));

	ret = work_busy(&dac->snd_dac_save_guide_work.work);
	if (ret & WORK_BUSY_PENDING) {
		pr_err("snd_dac_save_guide_work WORK_BUSY_PENDING cancel it\n");
		cancel_delayed_work_sync(&dac->snd_dac_save_guide_work);
	}
	snd_dac_report_dac_state(DAC_SHUTDOWN_EVENT);
}

static void snd_dac_shutdown_extern(struct snd_pcm_substream *substream,
				    struct snd_soc_dai *dai, bool is_extern)
{
	struct snd_dac *dac;

#line 274
	pr_err("%s[%d] is_extern:%d \n", __func__, __LINE__, is_extern);
	dac = dai ? snd_soc_component_get_drvdata(dai->component) : g_dac;

	if (is_xmos_hdmi_power_on(dac->dac_dai) && !is_extern)
		return;

	if (dac->dac_dai->system_work_mode == SYSTEM_MODE_SPDIF_RX_BYPASS)
		pr_err("%s can't shutdown for spdif rx bypass mode!\n", __func__);
	else
		snd_dac_shutdown_time(dac, 20000);
}

static void snd_dac_volume_fade_in_do_work(struct snd_dac *dac)
{
	int ret;
	int i;

	ret = work_busy(&dac->snd_dac_volume_fade_in_work.work);
	if (ret & WORK_BUSY_PENDING) {
		pr_err(KERN_WARNING "%s: snd_dac_volume_fade_in_work WORK_BUSY_PENDING cancel it\n", __func__);
		cancel_delayed_work_sync(&dac->snd_dac_volume_fade_in_work);
	}
	if (ret & WORK_BUSY_RUNNING) {
		pr_err("%s: snd_dac_volume_fade_in_work WORK_BUSY_RUNNING wait it to finish\n", __func__);
		_Pragma("clang loop unroll(disable)")
		for (i = 0; i < 10; i++) {
			pr_err("%s: snd_dac_volume_fade_in_work WORK_BUSY_RUNNING wait it to finish i = %d\n",
			       __func__, i);
			mdelay(5);
			ret = work_busy(&dac->snd_dac_volume_fade_in_work.work);
			if (!(ret & WORK_BUSY_RUNNING))
				break;
		}
	}
	queue_delayed_work(system_wq, &dac->snd_dac_volume_fade_in_work, msecs_to_jiffies(100));
}

static int snd_dac_startup(struct snd_pcm_substream *substream,
			   struct snd_soc_dai *dai)
{
	struct snd_dac *dac = g_dac;
	struct snd_dac_dai *dac_dai;
	int ret = 0;
	int new_output_type;

	if (dai)
		dac = snd_soc_component_get_drvdata(dai->component);
	if (!dac || !dac->dac_dai)
		return -1;
	dac_dai = dac->dac_dai;

	dac->is_playing_now = true;
	new_output_type = dac->anolog_board_module ?
			  dac->anolog_board_module->new_output_type : 8;
#line 365
	pr_err("%s(%d) new_output_type: %s, is_wakeup_lock: %d, headset_state: %d, system_mode_old: %d, system_work_mode: %d\n",
	       __func__, __LINE__, output_name[new_output_type], dac->is_wakeup_lock,
	       dac->headset_state, dac_dai->system_work_mode_old, dac_dai->system_work_mode);

	if (dac_dai->system_work_mode_old != dac_dai->system_work_mode ||
	    dac_dai->system_work_mode == SYSTEM_MODE_HDMI_TX_ARC) {
		dac_dai->system_work_mode_old = dac_dai->system_work_mode;
		if (dac->anolog_board_module) {
			dac->anolog_board_module->isNeedMute = true;
			dac->anolog_board_module->isNeedPowerChange = true;
		}
	}

	if (!dac->is_wakeup_lock) {
		dac->is_wakeup_lock = true;
		__pm_stay_awake(dac->audiolink_wakelock);
	}
	snd_dac_cancel_poweroff(dac);

	if (dac->anolog_board_module) {
		dac->anolog_board_module->current_output_type = dac->anolog_board_module->new_output_type;
		pr_err("%s, update current_output_type = %s", __func__,
		       output_name[dac->anolog_board_module->current_output_type]);
		if (dac->anolog_board_module) {
			if (dac->anolog_board_module->isNeedMute && dac->anolog_board_module->mute_all)
				dac->anolog_board_module->mute_all(1);
			if (dac->anolog_board_module && dac->anolog_board_module->isNeedPowerChange) {
				pr_err("[%s] power need change, so power restart!!!\n", __func__);
				if (dac->anolog_board_module && dac->anolog_board_module->control_all_power)
					dac->anolog_board_module->control_all_power(true);
			}
		}
	}

	if (dac->headset_state == 0 && dac->anolog_board_module &&
	    dac->anolog_board_module->is_pause_by_headset)
		queue_delayed_work(system_wq, &dac->snd_dac_save_guide_work, msecs_to_jiffies(5000));

	dac->dac_public->power_state = true;
	dac->dac_public->playing_state = true;
	snd_dac_report_dac_state(DAC_STARTUP_EVENT);

	return ret;
}

static int snd_dac_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
#line 411
	pr_err("%s(%d),  fmt = 0x%x. \n", __func__, __LINE__, fmt);
	return 0;
}

static int snd_dac_set_dai_sysclk(struct snd_soc_dai *dai, int clk_id,
				  unsigned int freq, int dir)
{
#line 418
	pr_err("%s(%d)\n", __func__, __LINE__);
	return 0;
}

static int snd_dac_set_dai_mute(struct snd_soc_dai *dai, int mute, int stream)
{
	struct snd_dac *dac;

	pr_err("%s mute:%s\n", __func__, mute ? "ON" : "OFF");
	dac = dai ? snd_soc_component_get_drvdata(dai->component) : g_dac;

	if (mute == 0 || dac->dac_dai->system_work_mode != SYSTEM_MODE_SPDIF_RX_BYPASS) {
		snd_dac_dai_mute(dac, mute);
		if (is_xmos_hdmi_power_on(dac->dac_dai))
			return 0;
		snd_dac_dai_mute(dac, mute);
	} else {
		pr_err("%s can't mute for spdif rx bypass mode!\n", __func__);
	}

	return 0;
}

static int snd_dac_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct snd_dac *dac;
	struct snd_dac_dai *dac_dai;

#line 455
	pr_err("[%s %d]\n", __func__, __LINE__);
	if (dai) {
		dac = snd_soc_component_get_drvdata(dai->component);
		dac_dai = dac->dac_dai;
#line 464
		pr_err("%s(%d) 00 sample_rate:%d,format=%d,bits=%d mode=%d dac_dai->user_format:%d\n",
		       __func__, __LINE__, dac_dai->sample_rate, dac_dai->format, dac_dai->bits,
		       dac_dai->system_work_mode, dac_dai->user_format);
		dac_dai->sample_rate = params_rate(params);
		if (dac_dai->user_format)
			dac_dai->format = dac_dai->user_format;
		else
			dac_dai->format = params_format(params);
		dac_dai->bits = snd_pcm_format_width(params_format(params));
	} else {
		dac = g_dac;
		dac_dai = g_dac->dac_dai;
	}

	switch (dac_dai->format) {
	case SNDRV_PCM_FORMAT_S16_LE:
	case SNDRV_PCM_FORMAT_U16_LE:
	case SNDRV_PCM_FORMAT_S24_LE:
	case SNDRV_PCM_FORMAT_U24_LE:
	case SNDRV_PCM_FORMAT_S32_LE:
	case SNDRV_PCM_FORMAT_U32_LE:
		dac_dai->is_dsd = false;
		break;
	case SNDRV_PCM_FORMAT_DSD_U16_LE:
	case SNDRV_PCM_FORMAT_DSD_U32_LE:
		dac_dai->is_dsd = true;
		break;
	default:
		break;
	}

	if (soc_i2s_use_one_line == 1)
		dac_dai->sample_rate = dac_dai->sample_rate * 2;

#line 497
	pr_err("%s(%d) sample_rate:%d,format=%d,bits=%d mode=%d\n", __func__, __LINE__,
	       dac_dai->sample_rate, dac_dai->format, dac_dai->bits, dac_dai->system_work_mode);

	if (dac->anolog_board_module && dac->anolog_board_module->hw_params)
		dac->anolog_board_module->hw_params();

	if (dac_dai->system_work_mode == SYSTEM_MODE_MTP && !dac_dai->is_dsd)
		dac->is_local_playing = true;
	else
		dac->is_local_playing = false;
	snd_dac_report_dac_state(DAC_LOCAL_PLAY_EVENT);

	dac_dai->gain_update = true;
	if (dac->anolog_board_module && dac->anolog_board_module->update_volume)
		dac->anolog_board_module->update_volume();

	dac_dai->ops->set_thd_c2(dac_dai);
	report_playing_info(dac_dai->dev, dac_dai->is_dsd, dac_dai->sample_rate);

	return 0;
}

static int snd_dac_trigger(struct snd_pcm_substream *substream, int cmd,
			   struct snd_soc_dai *dai)
{
	g_dac->snd_trigger = cmd ? true : false;
	return 0;
}

static void snd_dac_shutdown(struct snd_pcm_substream *substream,
			     struct snd_soc_dai *dai)
{
	snd_dac_shutdown_extern(substream, dai, false);
}

static int snd_dac_probe(struct snd_soc_component *component)
{
	struct snd_dac *dac = snd_soc_component_get_drvdata(component);
	struct snd_dac_dai *dac_dai = dac->dac_dai;
	int ret;

#line 567
	pr_err("%s(%d)\n", __func__, __LINE__);
	dac->audiolink_wakelock = wakeup_source_register(dac_dai->dev, "audiolink_wakelock");
	if (!dac->audiolink_wakelock)
#line 571
		pr_err("audiolink_wakelock register failed  %s[%d] \n", __func__, __LINE__);

	ret = snd_soc_card_jack_new(component->card, "ESS Headset Jack", SND_JACK_HEADSET,
				    &dac->headset_jack, NULL, 0);
	if (ret < 0)
		pr_err("snd_soc_card_jack_new FAILED! ret:%d \n", ret);

	return ret;
}

static int snd_dac_set_bias_level(struct snd_soc_component *component,
				  enum snd_soc_bias_level level)
{
#line 585
	pr_err("%s(%d), level = %d \n", __func__, __LINE__, level);
	return 0;
}

static void snd_dac_remove(struct snd_soc_component *component)
{
#line 602
	pr_err("%s(%d)\n", __func__, __LINE__);
	snd_dac_set_bias_level(component, SND_SOC_BIAS_OFF);
}

static int snd_dac_suspend(struct snd_soc_component *component)
{
	snd_dac_set_bias_level(component, SND_SOC_BIAS_OFF);
#line 611
	pr_err("%s(%d)\n", __func__, __LINE__);
	return 0;
}

static int snd_dac_resume(struct snd_soc_component *component)
{
	snd_dac_set_bias_level(component, SND_SOC_BIAS_ON);
#line 618
	pr_err("%s(%d)\n", __func__, __LINE__);
	return 0;
}

static const struct snd_soc_dapm_widget fiio_snd_dummy_dapm_widgets[] = {
};

static const struct snd_soc_dapm_route fiio_snd_dummy_audio_map[] = {
};

static int snd_dac_kcontrol_get_volume(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_dac *dac = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = dac->dac_dai->volume;
	return 0;
}

static int snd_dac_kcontrol_put_volume(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_dac *dac = snd_soc_component_get_drvdata(component);
	struct snd_dac_dai *dac_dai = dac->dac_dai;

	if (ucontrol->value.integer.value[0] == dac_dai->volume) {
		pr_err("%s, volume(%d) is same, update failed\n", __func__, dac_dai->volume);
		return 0;
	}
	dac_dai->volume = ucontrol->value.integer.value[0];
	pr_err("%s, value = %d\n", __func__, dac_dai->volume);
	if (dac->aw22xxx_module && dac->aw22xxx_module->volume_set_gain)
		dac->aw22xxx_module->volume_set_gain(dac_dai->volume);
	dac_dai->gain_update = false;
	if (dac->anolog_board_module && dac->anolog_board_module->update_volume)
		dac->anolog_board_module->update_volume();
	return 0;
}

static int snd_dac_line_out_volume_get(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_dac *dac = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = dac->dac_dai->lo_vol_adjustable;
	return 0;
}

static int snd_dac_line_out_volume_put(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_dac *dac = snd_soc_component_get_drvdata(component);
	struct snd_dac_dai *dac_dai = dac->dac_dai;

	if (ucontrol->value.integer.value[0] == dac_dai->lo_vol_adjustable) {
		pr_err("%s, volume(%d) is same, update failed\n", __func__, dac_dai->lo_vol_adjustable);
		return 0;
	}
	dac_dai->lo_vol_adjustable = ucontrol->value.integer.value[0];
	pr_err("[%s] %d\n", __func__, dac_dai->lo_vol_adjustable);
	dac_dai->gain_update = false;
	if (dac->anolog_board_module && dac->anolog_board_module->update_volume)
		dac->anolog_board_module->update_volume();
	return 0;
}

static int snd_dac_lr_balance_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_dac *dac = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = dac->dac_dai->lrbalance;
	return 0;
}

static int snd_dac_lr_balance_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_dac *dac = snd_soc_component_get_drvdata(component);
	struct snd_dac_dai *dac_dai = dac->dac_dai;

	if (ucontrol->value.integer.value[0] == dac_dai->lrbalance) {
		pr_err("%s, lrbalance(%d) is same, update failed\n", __func__, dac_dai->lrbalance);
		return 0;
	}
	dac_dai->lrbalance = ucontrol->value.integer.value[0];
	pr_err("[%s] %ld\n", __func__, ucontrol->value.integer.value[0]);
	dac_dai->gain_update = false;
	if (dac->anolog_board_module && dac->anolog_board_module->update_volume)
		dac->anolog_board_module->update_volume();
	return 0;
}

static int snd_dac_po_gain_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_dac *dac = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = dac->dac_dai->pogain;
	return 0;
}

static int snd_dac_po_gain_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_dac *dac = snd_soc_component_get_drvdata(component);
	struct snd_dac_dai *dac_dai = dac->dac_dai;
	long val = ucontrol->value.integer.value[0];

	if (val == dac_dai->pogain) {
		pr_err("%s, pogain(%d) is same, update failed\n", __func__, dac_dai->pogain);
		return 0;
	}
	if ((dac_dai->pogain == 4 && val != 4) || (dac_dai->pogain != 4 && val == 4))
		dac_dai->gain_pwr_update = true;
	else
		dac_dai->gain_pwr_update = false;
	dac_dai->pogain = val;
	pr_err("[%s] %ld\n", __func__, val);
	dac_dai->gain_update = true;
	if (dac->volume_module && dac->volume_module->on)
		dac->volume_module->on = NJU_NEED_CONFIG;
	if (dac->anolog_board_module && dac->anolog_board_module->update_volume)
		dac->anolog_board_module->update_volume();
	if (dac_dai->pogain < 4)
		dac_dai->pogain_bk = dac_dai->pogain;
	return 0;
}

static int snd_dac_get_volsw_lpf(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_dac *dac = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = dac->dac_dai->lpf_setting;
	return 0;
}

static int snd_dac_put_volsw_lpf(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_dac *dac = snd_soc_component_get_drvdata(component);
	struct snd_dac_dai *dac_dai = dac->dac_dai;

	dac_dai->lpf_setting = ucontrol->value.integer.value[0];
	pr_err("%s, lpf_setting = 0x%x \n", __func__, dac_dai->lpf_setting);
	dac_dai->ops->set_lpf(dac_dai);
	return 0;
}

static int snd_dac_get_audio_change(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int snd_dac_put_audio_change(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_dac *dac = snd_soc_component_get_drvdata(component);
	int value = ucontrol->value.integer.value[0];
	char buf[25] = { 0 };

	if (value == 1) {
		sprintf(buf, "fiio_audio_change=%d", value);
		fiio_sendMessage(dac->dac_dai->dev, buf, NULL);
	}
	return 0;
}

static int snd_dac_standby_status_get(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	return 1;
}

static int snd_dac_get_lopo_select(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_dac *dac = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = dac->lopo_mode_setting;
	return 0;
}

static int snd_dac_put_lopo_select(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_dac *dac = snd_soc_component_get_drvdata(component);
	int val = ucontrol->value.integer.value[0];

	dac->lopo_mode_setting = val;
	pr_err("[%s] lopo_mode_setting: %s\n", __func__, lopo_mode_setting_name[val]);

	if (dac->anolog_board_module && dac->lopo_mode_setting < 8) {
		switch (dac->headset_state) {
		case 1:
		case 3:
			if (dac->lopo_mode_setting < 3)
				goto mute;
			break;
		case 8:
		case 12:
			if (dac->lopo_mode_setting == 3 || dac->lopo_mode_setting == 4)
				goto mute;
			break;
		default:
			break;
		}
		goto out;
mute:
		if (dac->anolog_board_module->mute_all)
			dac->anolog_board_module->mute_all(1);
	}
out:
	{
		struct dac_notify_data notify_data = {
			.event = LOPO_MODE_SETTING_CHANGE,
			.lopo_mode_setting = dac->lopo_mode_setting,
		};

		fiio_notifier_call_chain(notifier_dac, 0, &notify_data);
	}
	return 0;
}

static int snd_dac_get_thd_c2(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int snd_dac_put_thd_c2(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_dac *dac = snd_soc_component_get_drvdata(component);
	struct snd_dac_dai *dac_dai = dac->dac_dai;

	dac_dai->thd_c2 = ucontrol->value.integer.value[0];
	pr_err("[%s] thd c2:%d\n", __func__, dac_dai->thd_c2);
	dac_dai->ops->set_thd_c2(dac->dac_dai);
	return 0;
}

static int snd_get_audio_format(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int snd_put_audio_format(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_dac *dac = snd_soc_component_get_drvdata(component);
	struct snd_dac_dai *dac_dai = dac->dac_dai;
	snd_pcm_format_t audio_format = ucontrol->value.integer.value[0];

	pr_err("[%s] set audio format:%d\n", __func__, audio_format);
	dac_dai->user_format = audio_format;
	return 0;
}

static int snd_get_audio_mute(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	return 0;
}

static int snd_put_audio_mute(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_dac *dac = snd_soc_component_get_drvdata(component);
	struct snd_dac_dai *dac_dai = dac->dac_dai;
	int value;

	if (dac_dai) {
		value = ucontrol->value.integer.value[0];
		pr_err("%s, value = %d\n", __func__, value);
		if (dac_dai->ops && dac_dai->ops->sw_mute)
			dac_dai->ops->sw_mute(dac_dai, 1);
	}
	return 0;
}

static const struct snd_kcontrol_new snd_dac_controls[] = {
	SOC_SINGLE_EXT("DAC Playback Volume", SND_SOC_NOPM, 0, 255, 1,
		       snd_dac_kcontrol_get_volume, snd_dac_kcontrol_put_volume),
	SOC_SINGLE_EXT("DAC line Out Volume", SND_SOC_NOPM, 0, 255, 0,
		       snd_dac_line_out_volume_get, snd_dac_line_out_volume_put),
	SOC_SINGLE_EXT("DAC LR Balance", SND_SOC_NOPM, 0, 255, 0,
		       snd_dac_lr_balance_get, snd_dac_lr_balance_put),
	SOC_SINGLE_EXT("DAC PO Gain", SND_SOC_NOPM, 0, 255, 0,
		       snd_dac_po_gain_get, snd_dac_po_gain_put),
	SOC_SINGLE_EXT("DAC Digital Filter Mode", 0, 0, 4, 0,
		       snd_dac_get_volsw_lpf, snd_dac_put_volsw_lpf),
	SOC_SINGLE_EXT("FiiO Audio Params Change", 0, 0, 1, 0,
		       snd_dac_get_audio_change, snd_dac_put_audio_change),
	SOC_SINGLE_MULTI_EXT("FiiO Get Standby status", 0, 0, 0xffff, 0, 3,
			     snd_dac_standby_status_get, NULL),
	SOC_SINGLE_EXT("Audio POLO Mode", SND_SOC_NOPM, 0, 255, 0,
		       snd_dac_get_lopo_select, snd_dac_put_lopo_select),
	SOC_SINGLE_EXT("DAC THD C2", 0, 0, 0x10000, 0,
		       snd_dac_get_thd_c2, snd_dac_put_thd_c2),
	SOC_SINGLE_EXT("Audio Format", 0, 0, 255, 0,
		       snd_get_audio_format, snd_put_audio_format),
	SOC_SINGLE_EXT("DAC MUTE", 0, 0, 255, 0,
		       snd_get_audio_mute, snd_put_audio_mute),
};

static void snd_dac_cancel_change_output_mode_work(struct snd_dac *dac)
{
	int ret;
	int i;

	ret = work_busy(&dac->snd_dac_change_output_mode_work.work);
	if (ret & WORK_BUSY_PENDING)
		cancel_delayed_work_sync(&dac->snd_dac_change_output_mode_work);
	if (ret & WORK_BUSY_RUNNING) {
		for (i = 0; i < 20; i++) {
			pr_err("%s: check snd_dac_change_output_mode_work WORK_BUSY_RUNNING wait it to finish i = %d\n",
			       __func__, i);
			mdelay(10);
			if (!(work_busy(&dac->snd_dac_change_output_mode_work.work) & WORK_BUSY_RUNNING))
				break;
		}
	}
}

static void snd_dac_change_output_mode_work_func(struct work_struct *work)
{
	pr_err("%s enter\n", __func__);
	snd_dac_startup(NULL, NULL);
	snd_dac_hw_params(NULL, NULL, NULL);
	snd_dac_set_dai_mute(NULL, 0, 0);
	pr_err("%s end\n", __func__);
}

static int headset_notifier_cb(struct notifier_block *self,
			       unsigned long event, void *data)
{
	struct snd_dac *dac = container_of(self, struct snd_dac, headset_notifier);
	struct snd_dac_dai *dac_dai = dac->dac_dai;
	struct headset_notify_data *notify_data = data;

	if (!notify_data)
		return 0;

	if (notify_data->event & HEADSET_STATE_CHANGE) {
		pr_err("%s, HEADSET_STATE_CHANGE, headset_state= %d.\n", __func__,
		       notify_data->headset_state);
		dac->headset_state = notify_data->headset_state;
	}

	if (notify_data->event & HEADSET_OUTPUT_TYPE_CHANGE) {
		pr_err("%s, HEADSET_OUTPUT_TYPE_CHANGE, new output_type= %s, new headset_type = %d.\n",
		       __func__, output_name[notify_data->output_type], notify_data->headset_type);
		if (dac->anolog_board_module)
			dac->anolog_board_module->new_output_type = notify_data->output_type;
		dac->headset_type = notify_data->headset_type;

		if (dac->anolog_board_module &&
		    dac->anolog_board_module->new_output_type != dac->anolog_board_module->current_output_type) {
			dac->anolog_board_module->isNeedMute = true;
			dac->anolog_board_module->isNeedPowerChange = true;
		}

		if (dac->anolog_board_module) {
			if (notify_data->output_type != 8 &&
			    notify_data->output_type != dac->anolog_board_module->current_output_type &&
			    dac->is_playing_now) {
				snd_dac_cancel_change_output_mode_work(dac);
				dac->anolog_board_module->current_output_type = notify_data->output_type;
				queue_delayed_work(system_wq, &dac->snd_dac_change_output_mode_work, 0);
				mdelay(20);
			}
			dac->anolog_board_module->current_output_type = notify_data->output_type;

			switch (notify_data->output_type) {
			case 0:
				dac->anolog_board_module->audio_output_mode = FIIO_OUTPUT_MODE_PO;
				break;
			case 1:
				dac->anolog_board_module->audio_output_mode = FIIO_OUTPUT_MODE_LO;
				break;
			case 3:
				dac->anolog_board_module->audio_output_mode = FIIO_OUTPUT_MODE_BAL_PO;
				break;
			case 4:
				dac->anolog_board_module->audio_output_mode = FIIO_OUTPUT_MODE_BAL_LO;
				break;
			default:
				break;
			}

			if (dac->anolog_board_module->current_output_type == 8)
				report_playing_info(dac_dai->dev, 0, 0);

			if (dac->headset_jack_module && dac->headset_jack_module->hs35_det_gpio_disable) {
				pr_err("%s set hs35_det_gpio_disable to false\n", __func__);
				dac->headset_jack_module->hs35_det_gpio_disable = false;
			}
		}
	}

	return 0;
}

static int player_notifier_cb(struct notifier_block *self,
			      unsigned long event, void *data)
{
	struct snd_dac *dac = container_of(self, struct snd_dac, player_notifier);
	struct snd_dac_dai *dac_dai = dac->dac_dai;
	struct player_notify_data *notify_data = data;
	struct snd_pcm_substream *substream = NULL;
	struct snd_pcm_hw_params *params = NULL;
	struct snd_soc_dai *dai = NULL;
	int stream_rate, stream_format, stream_width;
	int enable, mute;

	if (!notify_data)
		return 0;

	switch (notify_data->event) {
	case PLAYER_STARTUP:
		pr_err("%s, PLAYER_STARTUP.\n", __func__);
		snd_dac_startup(substream, dai);
		dac->player_already_startup = true;
		break;
	case PLAYER_CONFIG_PARAMS:
		pr_err("%s, PLAYER_CONFIG_PARAMS.\n", __func__);
		snd_dac_dsd_cancel_unmute_work(dac);
		snd_dac_pcm_cancel_unmute_work(dac);
		if (dac->anolog_board_module)
			dac->anolog_board_module->is_unmuting = false;
		if (!dac->player_already_startup) {
			snd_dac_startup(substream, dai);
			dac->player_already_startup = true;
		} else {
			snd_dac_cancel_poweroff(dac);
			dac->is_playing_now = true;
		}
		stream_rate = notify_data->sample_rate;
		stream_format = notify_data->stream_format;
		stream_width = notify_data->stream_width;
		if (dac->mode_select_module && dac->mode_select_module->get_current_xmos_mode)
			dac->mode_select_module->get_current_xmos_mode();
		dac_dai->format = stream_format;
		dac_dai->sample_rate = stream_rate;
		dac_dai->bits = stream_width;
		snd_dac_hw_params(substream, params, dai);
		snd_dac_dai_mute(dac, 0);
		dac_dai->ops->sw_mute(dac_dai, 0);
		break;
	case PLAYER_SHUTDOWN:
		enable = notify_data->shutdown_enable;
		pr_err("%s, PLAYER_SHUTDOWN enable=%d.\n", __func__, enable);
		if (enable) {
			dac->player_already_startup = false;
			dac_dai->ops->sw_mute(dac_dai, 1);
			snd_dac_dai_mute(dac, 1);
			snd_dac_shutdown_extern(substream, dai, true);
		}
		break;
	case PLAYER_MODE_CHANGE:
		pr_err("%s, PLAYER_MODE_CHANGE mode=%d.\n", __func__, notify_data->mode);
		if (notify_data->mode == SYSTEM_MODE_SOC_UAC_EXIT) {
			if (dac_dai->system_work_mode == SYSTEM_MODE_SOC_UAC)
				dac_dai->system_work_mode = SYSTEM_MODE_MTP;
		} else if (notify_data->mode == SYSTEM_MODE_SOC_BT_EXIT) {
			if (dac_dai->system_work_mode == SYSTEM_MODE_SOC_BT)
				dac_dai->system_work_mode = SYSTEM_MODE_MTP;
		} else if (notify_data->mode == SYSTEM_MODE_BT_EXIT) {
			if (dac_dai->system_work_mode == SYSTEM_MODE_BT ||
			    dac_dai->system_work_mode == SYSTEM_MODE_BT_UPDATE)
				dac_dai->system_work_mode = SYSTEM_MODE_MTP;
		} else if (dac_dai->system_work_mode != notify_data->mode) {
			dac_dai->system_work_mode_old = dac_dai->system_work_mode;
			dac_dai->system_work_mode = notify_data->mode;
		}
		break;
	case PLAYER_MUTE:
		pr_err("%s, PLAYER_MUTE.\n", __func__);
		snd_dac_dai_mute(dac, 1);
		break;
	case PLAYER_SW_MUTE:
		pr_err("%s, PLAYER_SW_MUTE.\n", __func__);
		mute = notify_data->sw_mute_enable == 1;
		dac_dai->ops->sw_mute(dac_dai, mute);
		break;
	case PLAYER_SHUTDOWN_NOW:
		pr_err("%s, PLAYER_SHUTDOWN_NOW \n", __func__);
		dac->player_already_startup = false;
		dac_dai->ops->sw_mute(dac_dai, 1);
		snd_dac_dai_mute(dac, 1);
		snd_dac_shutdown_time(dac, 3000);
		break;
	default:
		break;
	}

	return 0;
}

static int notify_output_mode_event_notifier_cb(struct notifier_block *self,
						unsigned long event, void *data)
{
	struct snd_dac *dac = container_of(self, struct snd_dac, output_mode_event_notifier);
	struct snd_dac_dai *dac_dai = dac->dac_dai;
	struct notify_output_data *notify_data = data;
	bool is_spdif_tx_enable = false;
	bool is_pause_by_headset = false;
	enum fiio_output_mode_t audio_output_mode = FIIO_OUTPUT_MODE_NONE;
	bool zero_volume = true;

	if (!notify_data)
		return 0;
	if (notify_data->event != NOTIFY_EVENT_OUTPUT_MODE_CONFIG)
		return 0;

	pr_err("%s, NOTIFY_EVENT_OUTPUT_MODE_CONFIG.\n", __func__);
	if (dac->anolog_board_module) {
		dac->anolog_board_module->audio_output_mode = notify_data->output_mode;
		if (dac->anolog_board_module) {
			if (zero_volume && dac->anolog_board_module->set_volume_db)
				dac->anolog_board_module->set_volume_db(dac_dai->volume_table[0],
									dac_dai->volume_table[0]);
			if (dac->anolog_board_module && dac->anolog_board_module->mute_all)
				dac->anolog_board_module->mute_all(1);
		}
	}

	dac_dai->gain_update = false;
	if (dac->anolog_board_module) {
		if (dac->anolog_board_module->update_volume)
			dac->anolog_board_module->update_volume();
		if (dac->anolog_board_module) {
			if (zero_volume && dac->anolog_board_module->set_volume_db)
				dac->anolog_board_module->set_volume_db(dac_dai->volume_table[0],
									dac_dai->volume_table[0]);
			if (dac->anolog_board_module) {
				is_spdif_tx_enable = dac->anolog_board_module->is_spdif_tx_enable;
				is_pause_by_headset = dac->anolog_board_module->is_pause_by_headset;
				audio_output_mode = dac->anolog_board_module->audio_output_mode;
			}
		}
	}

	pr_err("%s: audio_output_mode=%d is_playing_now=%d is_pause_by_headset=%d is_spdif_tx_enable=%d mode=%d, trigger=%d",
	       __func__, audio_output_mode, dac->is_playing_now, is_pause_by_headset,
	       is_spdif_tx_enable, dac_dai->system_work_mode, dac->snd_trigger);

	if (!dac->anolog_board_module)
		return 0;

	audio_output_mode = dac->anolog_board_module->audio_output_mode;
	if (audio_output_mode == FIIO_OUTPUT_MODE_PO && dac->headset_state &&
	    (dac->is_playing_now ||
	     (dac->anolog_board_module->is_spdif_tx_enable && dac->snd_trigger))) {
		mdelay(400);
	} else if (audio_output_mode != FIIO_OUTPUT_MODE_PO &&
		   (dac->is_playing_now ||
		    (dac->anolog_board_module->is_spdif_tx_enable && dac->snd_trigger)) &&
		   (audio_output_mode == FIIO_OUTPUT_MODE_PO_PRE ||
		    audio_output_mode == FIIO_OUTPUT_MODE_PRE ||
		    audio_output_mode == FIIO_OUTPUT_MODE_LO)) {
		mdelay(400);
	} else if (audio_output_mode != FIIO_OUTPUT_MODE_PO &&
		   (dac->is_playing_now ||
		    (dac->anolog_board_module->is_spdif_tx_enable && dac->snd_trigger)) &&
		   (dac_dai->system_work_mode == SYSTEM_MODE_SPDIF_RX_BYPASS ||
		    dac_dai->system_work_mode == SYSTEM_MODE_SOC_UAC ||
		    dac_dai->system_work_mode == SYSTEM_MODE_SOC_BT)) {
		mdelay(400);
	} else if (audio_output_mode == FIIO_OUTPUT_MODE_PO && !dac->headset_state &&
		   (dac->is_playing_now ||
		    (dac->anolog_board_module->is_spdif_tx_enable && dac->snd_trigger)) &&
		   !dac->anolog_board_module->is_pause_by_headset &&
		   audio_output_mode != FIIO_OUTPUT_MODE_PRE &&
		   audio_output_mode != FIIO_OUTPUT_MODE_LO) {
		mdelay(400);
	} else {
		goto set_volume;
	}
	goto unmute;

set_volume:

	if (dac->anolog_board_module->set_volume_db)
		dac->anolog_board_module->set_volume_db(dac_dai->volumeL_DB, dac_dai->volumeR_DB);
	return 0;

unmute:
	if (dac->anolog_board_module)
		dac->anolog_board_module->is_muted = true;
	snd_dac_dai_mute(dac, 0);
	snd_dac_volume_fade_in_do_work(dac);
	return 0;
}

static int monitor_notifier_cb(struct notifier_block *self,
			       unsigned long event, void *data)
{
	struct snd_dac *dac = container_of(self, struct snd_dac, monitor_notifier);
	struct monitor_notify_data *notify_data = data;

	if (!notify_data)
		return 0;

	switch (notify_data->event) {
	case DC_ERROR:
		pr_err("%s, DC_ERROR\n", __func__);
		snd_dac_shutdown_time(dac, 1);
		if (dac->headset_jack_module && dac->headset_jack_module->allow_overload_det)
			dac->headset_jack_module->allow_overload_det = false;
		break;
	case OVERLOAD_ERROR:
		pr_err("%s, OVERLOAD_ERROR\n", __func__);
		snd_dac_shutdown_time(dac, 1);
		if (dac->headset_jack_module && dac->headset_jack_module->allow_dc_det)
			dac->headset_jack_module->allow_dc_det = false;
		break;
	case HIGH_TEMP_ERROR:
		pr_err("%s, HIGH_TEMP_ERROR\n", __func__);
		snd_dac_shutdown_time(dac, 1);
		break;
	default:
		break;
	}

	return 0;
}

static void snd_dac_poweroff_work_func(struct work_struct *work)
{
	struct snd_dac *dac = container_of(work, struct snd_dac, snd_dac_poweroff_delay_work.work);

	pr_err("%s: to disable power for dac dac->is_wakeup_lock=%d\n", __func__,
	       dac->is_wakeup_lock);
	if (dac->anolog_board_module) {
		dac->anolog_board_module->isNeedMute = true;
		dac->anolog_board_module->isNeedPowerChange = true;
		if (dac->anolog_board_module) {
			if (dac->anolog_board_module->mute_all)
				dac->anolog_board_module->mute_all(1);
			if (dac->anolog_board_module && dac->anolog_board_module->control_all_power)
				dac->anolog_board_module->control_all_power(false);
		}
	}

	if (dac->is_wakeup_lock) {
		__pm_relax(dac->audiolink_wakelock);
		dac->is_wakeup_lock = false;
	}
}

static void snd_dac_save_guide_work_func(struct work_struct *work)
{
	struct snd_dac *dac = container_of(work, struct snd_dac, snd_dac_save_guide_work.work);

	if (dac->headset_state == 0) {
		dac->save_guide_counter++;
		if (dac->save_guide_counter >= 24) {
			pr_err("%s save_guide_counter >= %d trigger stop!!!\n", __func__,
			       dac->save_guide_counter);
			dac->save_guide_counter = 0;
			fiio_sendMessage(dac->dac_dai->dev, "AUDIO_TRIGGER=PAUSE", NULL);
		} else {
			pr_err("%s save_guide_counter : %d go on counting!!!\n", __func__,
			       dac->save_guide_counter);
		}
		queue_delayed_work(system_wq, &dac->snd_dac_save_guide_work, msecs_to_jiffies(5000));
	} else {
		pr_err("%s hs_valid:%d clear all flag!!\n", __func__, dac->headset_state);
		dac->save_guide_counter = 0;
	}
}

static void snd_dac_dsd_unmute_work_func(struct work_struct *work)
{
	struct snd_dac *dac = container_of(work, struct snd_dac, snd_dac_dsd_unmute_delay_work.work);

	pr_err("%s: Enter!\n", __func__);
	if (dac->anolog_board_module && dac->anolog_board_module->hw_mute)
		dac->anolog_board_module->hw_mute(0);
}

static void snd_dac_pcm_unmute_work_func(struct work_struct *work)
{
	struct snd_dac *dac = container_of(work, struct snd_dac, snd_dac_pcm_unmute_delay_work.work);

	pr_err("%s: Enter!\n", __func__);
	if (dac->anolog_board_module && dac->anolog_board_module->hw_mute)
		dac->anolog_board_module->hw_mute(0);
}

static void snd_dac_volume_fade_in_func(struct work_struct *work)
{
	struct snd_dac *dac = container_of(work, struct snd_dac, snd_dac_volume_fade_in_work.work);

	pr_err("%s: Enter!\n", __func__);
	dac->dac_dai->ops->volume_fade_in(dac->dac_dai);
}

static void snd_dac_load_work(void)
{
	struct snd_dac *dac = g_dac;

	pr_err("%s: Enter!\n", __func__);

	dac->anolog_board_module = fiio_device_get_data_by_name("anolog_board module");
	if (!dac->anolog_board_module)
		pr_err("get anolog_board module failed!\n");

	dac->mode_select_module = fiio_device_get_data_by_name("mode_select module");
	if (!dac->mode_select_module)
		pr_err("get mode_select module failed!\n");

	dac->xmos_module = fiio_device_get_data_by_name("xmos module");
	if (!dac->xmos_module)
		pr_err("get xmos module failed!\n");

	dac->aw22xxx_module = fiio_device_get_data_by_name("aw22xxx module");
	if (!dac->aw22xxx_module)
		pr_err("get aw22xxx module failed!\n");

	dac->volume_module = fiio_device_get_data_by_name("volume module");
	if (!dac->volume_module)
		pr_err("get volume module failed!\n");

	dac->headset_jack_module = fiio_device_get_data_by_name("headset_jack module");
	if (!dac->headset_jack_module)
		pr_err("get headset_jack module failed!\n");

	dac->dac_dai = fiio_device_get_data_by_name("dac_dai module");
	if (!dac->dac_dai) {
		pr_err("get dac_dai module failed!\n");
	} else {
		dac->dac_dai->format = 0;
		dac->dac_dai->lo_vol_adjustable = 0;
	}
}

static void snd_dac_shutdown_now_gpl(void)
{
	struct player_notify_data notify_data;

	printk("%s:Enter ++++++++++++++++++++\n", __func__);
	memset(&notify_data, 0, sizeof(notify_data));
	notify_data.event = PLAYER_SHUTDOWN_NOW;
	fiio_notifier_call_chain(notifier_player, 0, &notify_data);
}

static void snd_dac_startup_gpl(void)
{
	struct player_notify_data notify_data;

	printk("%s:Enter ++++++++++++++++++++\n", __func__);
	memset(&notify_data, 0, sizeof(notify_data));
	notify_data.event = PLAYER_STARTUP;
	fiio_notifier_call_chain(notifier_player, 0, &notify_data);
}

static void snd_dac_shutdown_gpl(bool enable, int delayms)
{
	struct player_notify_data notify_data;

	printk("%s:Enter ++++++++++++++++++++enable=%d delayms=%d\n", __func__, enable, delayms);
	memset(&notify_data, 0, sizeof(notify_data));
	notify_data.event = PLAYER_SHUTDOWN;
	notify_data.delay_time = delayms;
	notify_data.shutdown_enable = enable;
	fiio_notifier_call_chain(notifier_player, 0, &notify_data);
}

static void snd_dac_hw_params_gpl(int stream_format, int stream_rate,
				  int stream_width, int mode)
{
	struct player_notify_data notify_data;

	printk("%s:Enter ++++++++++++++++++++\n", __func__);
	memset(&notify_data, 0, sizeof(notify_data));
	notify_data.event = PLAYER_CONFIG_PARAMS;
	notify_data.sample_rate = stream_rate;
	notify_data.stream_format = stream_format;
	notify_data.stream_width = stream_width;
	notify_data.mode = mode;
	fiio_notifier_call_chain(notifier_player, 0, &notify_data);
}

static void snd_dac_mute(bool enable)
{
	struct player_notify_data notify_data;

	printk("%s:Enter ++++++++++++++++++++\n", __func__);
	memset(&notify_data, 0, sizeof(notify_data));
	notify_data.event = PLAYER_MUTE;
	fiio_notifier_call_chain(notifier_player, 0, &notify_data);
}

static void snd_dac_sw_mute(bool enable)
{
	struct player_notify_data notify_data;

	printk("%s:Enter +++++++++++++++++++\n", __func__);
	memset(&notify_data, 0, sizeof(notify_data));
	notify_data.event = PLAYER_SW_MUTE;
	notify_data.sw_mute_enable = enable;
	fiio_notifier_call_chain(notifier_player, 0, &notify_data);
}

static ssize_t snd_dac_volume_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct snd_dac_dai *dac_dai = g_dac->dac_dai;

	return sprintf(buf, "%d\n", dac_dai->volume);
}

static void snd_dac_set_ctrl(const char *buf, size_t size)
{
	struct snd_dac *dac = g_dac;
	struct snd_dac_dai *dac_dai = dac->dac_dai;
	int value = 0, value2 = 0;
	char cmd[10];
	int val;
	s32 data;

	if (!dac_dai) {
		pr_err("%s, error: dac_dai is null\n", __func__);
		return;
	}

	memset(cmd, 0, sizeof(cmd));
	if (!size)
		return;

	pr_err("%s, %s \n", __func__, buf);
	if (!strcmp(buf, "route_po\n")) {
	} else if (!strcmp(buf, "route_lo\n")) {
	} else if (!strcmp(buf, "route_ba\n")) {
	} else if (!strcmp(buf, "route_balo\n")) {
	} else if (!strcmp(buf, "dac_reconfig\n")) {
		if (dac_dai->ops->config_dac)
			dac_dai->ops->config_dac(dac_dai);
	} else if (!strcmp(buf, "read_all_regs\n")) {
		if (dac_dai->ops->read_reg_all)
			dac_dai->ops->read_reg_all(dac_dai);
	} else {
		sprintf(cmd, "%.1s", buf);
		if (!strcmp(cmd, "2")) {
			val = atoi(buf);
			dac_dai->gain_update = false;
			dac_dai->volume = val % 1000 - 100;
			if (dac->anolog_board_module && dac->anolog_board_module->update_volume)
				dac->anolog_board_module->update_volume();
		} else if (!strcmp(cmd, "7")) {
			int reg = htoi(buf) % 0x1000000 - 0x100000;

			fiio_regmap_read(dac_dai->name, dac_dai->regmap_l, reg, (unsigned int *)&value);
			fiio_regmap_read(dac_dai->name, dac_dai->regmap_r, reg, (unsigned int *)&value2);
			pr_err("[%s] read dac reg = 0x%x, value_l = 0x%02x, value_r = 0x%02x\n",
			       __func__, reg, value, value2);
		} else if (!strcmp(cmd, "8")) {
			data = atoi(buf);
			{
				int reg = (data % 100000000) / 1000000 - 10;
				int bit = (data % 1000000) / 1000 - 100;

				val = data % 1000 - 100;
				pr_err("[%s] data = %d, write dac reg = 0x%02x, bit = 0x%02x, val = 0x%02x\n",
				       __func__, data, reg, bit, val);
				fiio_regmap_write_bits(dac_dai->name, dac_dai->regmap_l, reg, bit, val);
				fiio_regmap_write_bits(dac_dai->name, dac_dai->regmap_r, reg, bit, val);
			}
		} else if (!strcmp(cmd, "9")) {
			sprintf(cmd, "%.7s", buf);
			{
				int reg = htoi(cmd) % 0x1000000;
				int bit;

				data = htoi(buf + 7);
				bit = (data % 0x10000) / 256;
				val = data % 0x100;
				pr_err("[%s] data = %x, write dac reg = 0x%06x, bit = 0x%02x, val = 0x%02x\n",
				       __func__, data, reg, bit, val);
				fiio_regmap_write_bits(dac_dai->name, dac_dai->regmap_l, reg, bit, val);
				fiio_regmap_write_bits(dac_dai->name, dac_dai->regmap_r, reg, bit, val);
			}
		} else {
			pr_err("[%s] error cmd: %s\n", __func__, buf);
		}
	}
}

static const struct snd_soc_dai_ops snd_dac_dai_ops = {
	.set_sysclk = snd_dac_set_dai_sysclk,
	.set_fmt = snd_dac_set_dai_fmt,
	.mute_stream = snd_dac_set_dai_mute,
	.startup = snd_dac_startup,
	.shutdown = snd_dac_shutdown,
	.hw_params = snd_dac_hw_params,
	.trigger = snd_dac_trigger,
};

static struct snd_soc_dai_driver snd_dac_dai_drv = {
	.name = "fiio HiFi",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = 0x1fffe,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_DSD_U16_LE |
			   SNDRV_PCM_FMTBIT_DSD_U32_LE,
	},
	.ops = &snd_dac_dai_ops,
};

static struct snd_soc_component_driver snd_dac_comp_drv = {
	.name = "fiio_codec",
	.probe = snd_dac_probe,
	.remove = snd_dac_remove,
	.suspend = snd_dac_suspend,
	.resume = snd_dac_resume,
	.set_bias_level = snd_dac_set_bias_level,
	.controls = snd_dac_controls,
	.num_controls = ARRAY_SIZE(snd_dac_controls),
	.dapm_widgets = fiio_snd_dummy_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(fiio_snd_dummy_dapm_widgets),
	.dapm_routes = fiio_snd_dummy_audio_map,
	.num_dapm_routes = ARRAY_SIZE(fiio_snd_dummy_audio_map),
};

static struct snd_dac snd_dac_data_init = {
	.name = "fiio dac",
};

static ssize_t snd_dac_read_reg_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct snd_dac *dac = g_dac;
	struct snd_dac_dai *dac_dai = dac->dac_dai;
	int value = 0;

	fiio_regmap_read(dac_dai->name, dac_dai->regmap_l, dac->dac_read_reg, (unsigned int *)&value);
	pr_err("[%s] read_reg = 0x%02x, val = 0x%02x\n", __func__, dac->dac_read_reg, value);
	return sprintf(buf, "%d\n", value);
}

static ssize_t snd_dac_read_reg_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	struct snd_dac *dac = g_dac;

	pr_err("[%s] read_reg = %s\n", __func__, buf);
	dac->dac_read_reg = atoi(buf);
	return size;
}

static ssize_t snd_dac_dac_ctrl_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	snd_dac_set_ctrl(buf, size);
	return size;
}

static const struct device_attribute snd_dac_attributes[] = {
	__ATTR(dac_ctrl, 0200, NULL, snd_dac_dac_ctrl_store),
	__ATTR(dac_vol, 0444, snd_dac_volume_show, NULL),
	__ATTR(dac_read_reg, 0644, snd_dac_read_reg_show, snd_dac_read_reg_store),
};

static struct dac_public_data dac_public_d = {
	.dac_startup = snd_dac_startup_gpl,
	.dac_shutdown = snd_dac_shutdown_gpl,
	.dac_hw_params = snd_dac_hw_params_gpl,
	.dac_mute = snd_dac_mute,
	.dac_sw_mute = snd_dac_sw_mute,
	.dac_shutdown_now = snd_dac_shutdown_now_gpl,
};

static struct fiio_device_driver dac_device_driver = {
	.name = "dac module",
	.private_data = &dac_public_d,
	.load_work = snd_dac_load_work,
};

static int snd_dac_platform_probe(struct platform_device *pdev)
{
	struct snd_dac *dac = &snd_dac_data_init;
	struct device *dev = &pdev->dev;
	int ret;

#line 1670
	pr_err("%s(%d)\n", __func__, __LINE__);
	g_dac = dac;
	platform_set_drvdata(pdev, dac);

	ret = snd_soc_register_component(dev, &snd_dac_comp_drv, &snd_dac_dai_drv, 1);
	if (ret < 0) {
		pr_err("[dac] register codec failed. \n");
		goto ERROR;
	}

	dac->is_playing_now = false;

	dac->headset_notifier.notifier_call = headset_notifier_cb;
	fiio_register_client(notifier_headset, &dac->headset_notifier);
	dac->player_notifier.notifier_call = player_notifier_cb;
	fiio_register_client(notifier_player, &dac->player_notifier);
	dac->output_mode_event_notifier.notifier_call = notify_output_mode_event_notifier_cb;
	fiio_register_client(notifier_output_mode, &dac->output_mode_event_notifier);
	dac->monitor_notifier.notifier_call = monitor_notifier_cb;
	fiio_register_client(notifier_monitor, &dac->monitor_notifier);

	INIT_DELAYED_WORK(&dac->snd_dac_change_output_mode_work, snd_dac_change_output_mode_work_func);
	INIT_DELAYED_WORK(&dac->snd_dac_poweroff_delay_work, snd_dac_poweroff_work_func);
	INIT_DELAYED_WORK(&dac->snd_dac_save_guide_work, snd_dac_save_guide_work_func);
	INIT_DELAYED_WORK(&dac->snd_dac_dsd_unmute_delay_work, snd_dac_dsd_unmute_work_func);
	INIT_DELAYED_WORK(&dac->snd_dac_pcm_unmute_delay_work, snd_dac_pcm_unmute_work_func);
	INIT_DELAYED_WORK(&dac->snd_dac_volume_fade_in_work, snd_dac_volume_fade_in_func);

	ret = fiio_device_attribute_register(snd_dac_attributes, ARRAY_SIZE(snd_dac_attributes));
	if (ret) {
		printk("attribute register failed!!");
		return -ENOMEM;
	}

	dac->dac_public = &dac_public_d;
	fiio_device_register(&dac_device_driver);
#line 1711
	pr_err("%s(%d) probe finished\n", __func__, __LINE__);
	return 0;

ERROR:
	fiio_device_attribute_unregister(snd_dac_attributes);
	return ret;
}

static void snd_dac_platform_shutdown(struct platform_device *pdev)
{
	struct snd_dac *dac = g_dac;

	pr_err(" %s enter \n", __func__);
	if (dac->anolog_board_module) {
		if (dac->anolog_board_module->mute_all)
			dac->anolog_board_module->mute_all(1);
		if (dac->anolog_board_module && dac->anolog_board_module->control_all_power)
			dac->anolog_board_module->control_all_power(false);
	}
	mdelay(1500);
}

static int snd_dac_platform_remove(struct platform_device *pdev)
{
	fiio_device_attribute_unregister(snd_dac_attributes);
	fiio_device_unregister(&dac_device_driver);
	return 0;
}

static const struct of_device_id snd_dac_dt_match[] = {
	{ .compatible = "fiio,dac-core", },
	{ }
};
MODULE_DEVICE_TABLE(of, snd_dac_dt_match);

static struct platform_driver dac_driver = {
	.driver = {
		.name = "fiio_codec",
		.owner = THIS_MODULE,
		.of_match_table = snd_dac_dt_match,
	},
	.probe = snd_dac_platform_probe,
	.remove = snd_dac_platform_remove,
	.shutdown = snd_dac_platform_shutdown,
};

static int __init snd_dac_init(void)
{
	int ret;

	pr_err("[%s]", __func__);
	ret = platform_driver_register(&dac_driver);
	if (ret)
#line 1764
		pr_err("%s(%d) unable to register platform device!\n", __func__, __LINE__);
	return ret;
}

static void __exit snd_dac_exit(void)
{
	platform_driver_unregister(&dac_driver);
}

module_init(snd_dac_init);
module_exit(snd_dac_exit);

MODULE_AUTHOR("mxc@fiio.net");
MODULE_DESCRIPTION("ALSA DAC Core");
MODULE_LICENSE("GPL v2");
