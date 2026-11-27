/* SPDX-License-Identifier: GPL-2.0 */
/* FPGA (Gowin) audio routing types shared by gw-fpga, fpga_interface and the DAC drivers */
#ifndef _FIIO_FPGA_COMMON_H
#define _FIIO_FPGA_COMMON_H

#include <linux/types.h>

enum fpga_work_mode {
	LOCAL_PLAYBACK_MODE,
	USB_DAC_MODE,
	USB_XMOS_MODE,
	BT_QCC_MODE,
};

enum fpga_fmt_t {
	FMT_SPDIF,
	FMT_PCM,
	FMT_FM,
	FMT_DSD,
	FMT_SPDIFDOP,
	FMT_SPDIF_MUTE,
	FMT_SPDIFDOP_MUTE,
	FMT_SPDIF_BYPASS,
	FMT_HDMI_IN,
};

enum fpga_channel {
	fpgachl_soc,
	fpgachl_xmos,
	fpgachl_bt,
	fpgachl_spdif_rx,
	fpgachl_hdmi,
};

typedef enum {
	FPGA_AUDIO_HAL_CH_I2S_CH_SOC,
	FPGA_AUDIO_HAL_CH_I2S_CH_UAC,
	FPGA_AUDIO_HAL_CH_I2S_CH_BT,
	FPGA_AUDIO_HAL_CH_I2S_CH_SPDIF,
	FPGA_AUDIO_HAL_CH_I2S_CH_OPT,
	FPGA_AUDIO_HAL_CH_I2S_CH_HDMI,
	FPGA_AUDIO_HAL_CH_SPDIF_BMC,
	FPGA_AUDIO_HAL_CH_OPT_BMC,
} fpga_hal_channel;

typedef enum {
	MCLK_11_12MHz,
	MCLK_22_24MHz,
	MCLK_45_49MHz,
	MCLK_90_98MHz,
} fpga_hal_mclk_type;

typedef enum {
	MCLK_OUT,
	MCLK_IN,
	MCLK_DISABLE,
} fpga_hal_mclk_direction;

typedef enum {
	P2D_DSD64,
	P2D_DSD128,
	P2D_DSD256,
	P2D_DSD512,
} fpga_p2d_type;

typedef enum {
	FPGA_I2S_FORMAT_P,
	FPGA_I2S_FORMAT_L,
	FPGA_I2S_FORMAT_R,
} fpga_i2s_format;

typedef enum {
	FPGA_I2S_BIT_32,
	FPGA_I2S_BIT_24,
	FPGA_I2S_BIT_16,
} fpga_i2s_bits;

typedef enum {
	FPGA_AUDIO_DSD,
	FPGA_AUDIO_PCM,
	FPGA_AUDIO_DOP,
} fpga_i2s_type;

typedef enum {
	FPGA_CONFIG_SOC_I2S_BYPASS,
	FPGA_CONFIG_UAC_I2S_BYPASS,
	FPGA_CONFIG_BT_I2S_BYPASS,
	FPGA_CONFIG_SPDIF_I2S_BYPASS,
	FPGA_CONFIG_OPT_I2S_BYPASS,
	FPGA_CONFIG_HDMI_I2S_BYPASS,
	FPGA_CONFIG_SPDIF_BYPASS,
	FPGA_CONFIG_OPT_BYPASS,
} fpga_config_bypass;

enum fpga_dac_mclk_t {
	AUDIO_DAC_MCLK_22,
	AUDIO_DAC_MCLK_45,
	AUDIO_DAC_MCLK_90,
};

enum fpga_hdmi_code_fmt {
	hdmi_code_fmt_0,
	hdmi_code_fmt_1,
	hdmi_code_fmt_2,
	hdmi_code_fmt_3,
};

struct fpga_public_data {
	bool on;
	void (*power_by_mode)(bool, int);
	int (*hw_params)(int, int, int);
	int (*set_input_channel)(enum fpga_channel);
	void (*init_xmos)(void);
	void (*fill_clk)(bool);
	void (*set_mclk)(enum fpga_dac_mclk_t);
	void (*enter_usb_xmos_mode)(void);
	void (*enter_local_playback_mode)(void);
	void (*config_xmos_spdif_rx)(bool);
	void (*config_hdmi_code_fmt)(enum fpga_hdmi_code_fmt);
	int (*config_spdif_tx)(bool);
	int (*config_opt_tx)(bool);
};

struct fpga_hwinit_params {
	fpga_hal_channel audio_ch;
	fpga_hal_mclk_direction input_mclk_direction;
	fpga_hal_mclk_type input_mclk_type;
	bool dac1_mclk_out_enable;
	fpga_hal_mclk_type dac1_mclk_type;
	bool dac2_mclk_out_enable;
	fpga_hal_mclk_type dac2_mclk_type;
	bool p2d_enable;
	fpga_p2d_type p2d_type;
	bool i2s_in_one_line_data;
	bool dac_mul_line_data;
};

struct fpga_config_params {
	bool is_dsd;
	int sample_rate;
	fpga_i2s_format i2s_out_format;
	fpga_i2s_bits i2s_out_bits;
	fpga_i2s_format i2s_in_format;
	fpga_i2s_bits i2s_in_bits;
	fpga_i2s_type i2s_in_type;
	bool spdif_tx;
	bool opt_tx;
	bool i2s_clk_out_en;
	bool i2s_data_out_en;
};

struct fpga_component {
	char *name;
	bool is_dsd;
	bool soc_use_one_line_data;
	bool dac_multiple_lines_data;
	int sample_rate;
	fpga_i2s_format i2s_out_format;
	fpga_i2s_bits i2s_out_bits;
	fpga_i2s_format i2s_in_format;
	fpga_i2s_bits i2s_in_bits;
	fpga_i2s_type i2s_in_type;
	bool spdif_tx_enable;
	bool opt_tx_enable;
	bool i2s_clk_enable;
	bool i2s_data_enable;
	fpga_hal_channel audio_channel;
	fpga_hal_mclk_direction input_mclk_direction;
	fpga_hal_mclk_type input_mclk_type;
	bool dac1_mclk_out_enable;
	fpga_hal_mclk_type dac1_mclk_type;
	bool dac2_mclk_out_enable;
	fpga_hal_mclk_type dac2_mclk_type;
	bool p2d_enable;
	fpga_p2d_type p2d_type;
	void (*ctrl_power)(bool);
	int (*fpga_config)(struct fpga_component *, enum fpga_fmt_t, int, int);
	void (*fpga_config_full)(struct fpga_component *, struct fpga_config_params *);
	void (*fpga_hwinit)(struct fpga_component *, struct fpga_hwinit_params *);
	void (*power_by_mode)(struct fpga_component *, bool, int);
	int (*set_input_channel)(enum fpga_channel);
	void (*init_xmos)(void);
	void (*fill_clk)(bool);
	void (*set_mclk)(enum fpga_dac_mclk_t);
	void (*enter_usb_xmos_mode)(void);
	void (*enter_local_playback_mode)(void);
	void (*config_xmos_spdif_rx)(bool);
	void (*config_hdmi_code_fmt)(enum fpga_hdmi_code_fmt);
	int (*config_spdif_rx)(bool);
	int (*config_spdif_tx)(bool);
	int (*config_opt_tx)(bool);
	int (*fpga_iic_test)(void);
};

#endif
