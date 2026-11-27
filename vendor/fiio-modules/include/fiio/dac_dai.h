/* SPDX-License-Identifier: GPL-2.0 */
/*
 * State shared by the FiiO DAC drivers; up to four DACs plus an AK4191 are
 * driven together. Layout from the stock DWARF.
 */
#ifndef _FIIO_DAC_DAI_H
#define _FIIO_DAC_DAI_H

#include <linux/types.h>
#include <linux/regmap.h>
#include <linux/i2c.h>
#include <sound/asound.h>
#include <fiio/mode_select_common.h>

enum audio_channel_mode {
	STEREO_MODE,
	MONO_MODE,
	SPECIAL_MODE_1,
	SPECIAL_MODE_2,
	SPECIAL_MODE_3,
	SPECIAL_MODE_4,
};

struct snd_dac_dai;

struct snd_dac_dai_ops {
	void (*set_thd_c2)(struct snd_dac_dai *dac_dai);
	void (*set_dac_sample_rate)(struct snd_dac_dai *dac_dai);
	void (*read_reg_all)(struct snd_dac_dai *dac_dai);
	void (*config_dac)(struct snd_dac_dai *dac_dai);
	void (*set_performance_mode)(struct snd_dac_dai *dac_dai, enum system_mode mode);
	void (*sw_mute)(struct snd_dac_dai *dac_dai, int mute);
	void (*set_volume_db)(struct snd_dac_dai *dac_dai, unsigned int left, unsigned int right);
	void (*volume_fade_in)(struct snd_dac_dai *dac_dai);
	void (*set_lpf)(struct snd_dac_dai *dac_dai);
	void (*switch_dac)(struct snd_dac_dai *dac_dai);
	void (*set_dre)(struct snd_dac_dai *dac_dai);
	int (*dac_iic_test)(void);
};

struct snd_dac_dai {
	const char *name;
	struct device *dev;
	bool on;

	const struct snd_dac_dai_ops *ops;

	struct regmap *regmap_l;
	struct regmap *regmap_r;
	struct regmap *regmap_3;
	struct regmap *regmap_4;
	struct regmap *regmap_ak4191;
	struct i2c_client *client_l;
	struct i2c_client *client_r;
	struct i2c_client *client_3;
	struct i2c_client *client_4;
	struct i2c_client *client_ak4191;

	/* stream */
	snd_pcm_format_t format;
	snd_pcm_format_t user_format;
	bool is_dsd;
	unsigned int sample_rate;
	unsigned int bits;

	/* volume */
	int volume;
	int volumeL_DB;
	int volumeR_DB;
	int lo_vol_adjustable;
	const int *volume_table;

	int lpf_setting;
	int lrbalance;
	bool gain_update;
	bool gain_pwr_update;

	int pogain;
	int pogain_bk;

	int synchronous_mode;
	int performance_mode;
	int clk_gear;
	int dpll_width;
	int bypass_osf;
	int thd_c2;
	bool soft_mute;
	bool nos_status;
	int fs_mode;
	bool dre_setting;

	int gain_spe;
	enum audio_channel_mode channel_mode;

	/* current and previous source */
	enum system_mode system_work_mode;
	enum system_mode system_work_mode_old;
};

int dac_get_gpio_value(int gpio);
void dac_set_gpio_value(int gpio, int value);
void dac_set_gpio_value_cansleep(int gpio, int value);
int dac_dual_client_write(struct snd_dac_dai *dac_dai, u8 reg, u8 data);
int dac_dual_regmap_write(struct snd_dac_dai *dac_dai, unsigned int reg, unsigned int val);
int dac_dual_regmap_write_delay(struct snd_dac_dai *dac_dai, unsigned int reg,
				unsigned int val, unsigned int delay);
int dac_dual_regmap_update_bits(struct snd_dac_dai *dac_dai, unsigned int reg,
				unsigned int mask, unsigned int val);
int dac_dual_regmap_multi_reg_write(struct snd_dac_dai *dac_dai,
				    const struct reg_sequence *regs, int num_regs);

#endif
