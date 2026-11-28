// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2022-2023 Aidan MacDonald
 */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#include "es9219c.h"

/* ES9219C serial mode, indexed by SND_SOC_DAIFMT_FORMAT_* value. */
static const u8 es9219c_ser_modes[] = {
	[SND_SOC_DAI_FORMAT_I2S] = 0,
	[SND_SOC_DAI_FORMAT_LEFT_J] = 1,
};

static const char * const es9219c_supply_names[] = {
	"avdd",
	"vcca",
	"avcc3v3",
	"avcc1v8",
};

/* Private state shared by DAPM events and kcontrol handlers. */
struct es9219c_priv {
	struct regmap *regmap;
	struct regulator_bulk_data supplies[ARRAY_SIZE(es9219c_supply_names)];
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	unsigned int max_clk_div;
	unsigned int osc_drv_bias;
	unsigned int soft_start_time;
	unsigned int clk_gear;
	unsigned int ss_time;
	unsigned int nco;
	bool output_2v;
	bool analog_bypass;
	unsigned int fsr;
	bool master_mode;
	bool powered;
	bool out_mux_powered;
	bool dac_powered;
};

static int es9219c_wide_write(struct regmap *regmap, unsigned int reg,
			      int count, unsigned int value)
{
	u8 data[4];
	int i;

	for (i = 0; i < count; i++) {
		data[i] = value & 0xff;
		value >>= 8;
	}
	return regmap_raw_write(regmap, reg, data, count);
}

static int es9219c_apply_clk_params(struct snd_soc_component *component)
{
	struct es9219c_priv *priv = snd_soc_component_get_drvdata(component);
	unsigned int clk_div, clk_gear, nco, ss_time, max_ss_time;
	unsigned long mclk;
	int ret;

	if (!priv->powered)
		return 0;
	mclk = clk_get_rate(priv->mclk);
	if (!mclk) {
		dev_err(component->dev, "MCLK rate is zero\n");
		return -EINVAL;
	}
	/* CLK >= 128*FSR, limited by the platform-specific maximum divider. */
	clk_div = mclk / (128 * priv->fsr);
	if (clk_div > priv->max_clk_div)
		clk_div = priv->max_clk_div;
	if (clk_div >= 8)
		clk_gear = 3;
	else if (clk_div >= 4)
		clk_gear = 2;
	else if (clk_div >= 2)
		clk_gear = 1;
	else
		clk_gear = 0;
	ret = snd_soc_component_write_field(component, ES9219C_SYSTEM,
					    ES9219C_SYSTEM_CLK_GEAR, clk_gear);
	if (ret)
		return ret;
	priv->clk_gear = clk_gear;

	/* Soft-start time, clamped so the settling delay stays below 250 ms. */
	if (priv->soft_start_time > clk_gear)
		ss_time = priv->soft_start_time - clk_gear;
	else
		ss_time = 0;
	if ((mclk >> clk_gear) < 32768)
		max_ss_time = 0;
	else
		max_ss_time = ilog2((mclk >> clk_gear) / 4096 / 2 / 4);
	if (ss_time > max_ss_time)
		ss_time = max_ss_time;
	ret = snd_soc_component_write_field(component, ES9219C_SS_CONFIG,
					    ES9219C_SS_CONFIG_TIME, ss_time);
	if (ret)
		return ret;
	priv->ss_time = ss_time;

	/* NCO drives the frame clock only in master mode. */
	if (priv->master_mode)
		nco = div_u64((u64)priv->fsr << 32, mclk >> clk_gear);
	else
		nco = 0;
	ret = snd_soc_component_write_field(component, ES9219C_MODE_SYNC,
					    ES9219C_MODE_SYNC_MASTER,
					    priv->master_mode);
	if (ret)
		return ret;
	ret = snd_soc_component_write_field(component, ES9219C_GEN_CONFIG,
					    ES9219C_GEN_CONFIG_ASRC_EN,
					    !priv->master_mode);
	if (ret)
		return ret;
	if (nco != priv->nco) {
		ret = es9219c_wide_write(priv->regmap, ES9219C_NCO, 4, nco);
		if (ret)
			return ret;
		priv->nco = nco;
	}
	return 0;
}

static int es9219c_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	struct es9219c_priv *priv = snd_soc_component_get_drvdata(component);
	unsigned int mode = fmt & SND_SOC_DAIFMT_FORMAT_MASK;
	unsigned int ser_mode, val;
	bool master;
	int ret;

	if (mode != SND_SOC_DAI_FORMAT_I2S && mode != SND_SOC_DAI_FORMAT_LEFT_J)
		return -EINVAL;
	ser_mode = es9219c_ser_modes[mode];
	if ((fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF)
		return -EINVAL;
	switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_BP_FP:
		master = true;
		break;
	case SND_SOC_DAIFMT_BC_FC:
		master = false;
		break;
	default:
		return -EINVAL;
	}

	/* keep SER_LEN, clear autoselect/decoder, set the mode. */
	val = snd_soc_component_read(component, ES9219C_INPUT_SEL);
	if (val < 0)
		return val;
	val &= ES9219C_INPUT_SEL_SER_LEN;
	val |= FIELD_PREP(ES9219C_INPUT_SEL_SER_MODE, ser_mode);
	ret = snd_soc_component_write(component, ES9219C_INPUT_SEL, val);
	if (ret)
		return ret;

	priv->master_mode = master;
	return es9219c_apply_clk_params(component);
}

static int es9219c_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct es9219c_priv *priv = snd_soc_component_get_drvdata(component);
	unsigned int width = params_width(params);
	unsigned int ser_len;
	int ret;

	if (width != 16 && width != 24 && width != 32)
		return -EINVAL;
	ser_len = width / 8 - 2;
	priv->fsr = params_rate(params);
	if (clk_get_rate(priv->mclk) < 128 * priv->fsr) {
		dev_err(component->dev, "MCLK too low for %u Hz\n", priv->fsr);
		return -EINVAL;
	}
	if (snd_soc_component_read_field(component, ES9219C_DOP_VOL_RAMP,
					 ES9219C_DOP_ENABLE) && priv->fsr % 44100) {
		dev_err(component->dev, "DoP requires a 44.1 kHz-family rate\n");
		return -EINVAL;
	}
	ret = snd_soc_component_write_field(component, ES9219C_INPUT_SEL,
					    ES9219C_INPUT_SEL_SER_LEN, ser_len);
	if (ret)
		return ret;
	return es9219c_apply_clk_params(component);
}

static int es9219c_mute_stream(struct snd_soc_dai *dai, int mute, int direction)
{
	struct snd_soc_component *component = dai->component;

	return snd_soc_component_update_bits(component, ES9219C_FBW_MUTE,
					     ES9219C_FBW_MUTE_MUTE,
					     mute ? ES9219C_FBW_MUTE_MUTE : 0);
}

static const struct snd_soc_dai_ops es9219c_dai_ops = {
	.set_fmt = es9219c_set_fmt,
	.hw_params = es9219c_hw_params,
	.mute_stream = es9219c_mute_stream,
};

static struct snd_soc_dai_driver es9219c_dai = {
	.name = "es9219c",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_CONTINUOUS,
		.rate_min = 8000,
		.rate_max = 384000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE |
			   SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &es9219c_dai_ops,
};

static int es9219c_power_up(struct snd_soc_component *component)
{
	struct es9219c_priv *priv = snd_soc_component_get_drvdata(component);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(priv->supplies), priv->supplies);
	if (ret) {
		dev_err(component->dev, "Cannot enable supplies: %d\n", ret);
		return ret;
	}
	ret = clk_prepare_enable(priv->mclk);
	if (ret) {
		dev_err(component->dev, "Cannot enable mclk: %d\n", ret);
		goto err_regulator;
	}
	gpiod_set_value_cansleep(priv->reset_gpio, 0); /* release RESETB */
	usleep_range(1500, 2000);
	priv->powered = true;
	ret = snd_soc_component_write_field(component, ES9219C_AMP_CONFIG,
					    ES9219C_AMP_CONFIG_MODE,
					    ES9219C_AMP_MODE_CORE_ON);
	if (ret < 0)
		goto err_clk_reset;
	ret = snd_soc_component_write_field(component, ES9219C_SYSTEM,
					    ES9219C_SYSTEM_OSC_DRV,
					    priv->osc_drv_bias);
	if (ret < 0)
		goto err_clk_reset;
	ret = es9219c_apply_clk_params(component);
	if (ret)
		goto err_clk_reset;
	regcache_cache_only(priv->regmap, false);
	ret = regcache_sync(priv->regmap);
	if (ret) {
		regcache_cache_only(priv->regmap, true);
		regcache_mark_dirty(priv->regmap);
		goto err_clk_reset;
	}
	return 0;
err_clk_reset:
	priv->powered = false;
	gpiod_set_value_cansleep(priv->reset_gpio, 1); /* assert RESETB */
	clk_disable_unprepare(priv->mclk);
err_regulator:
	regulator_bulk_disable(ARRAY_SIZE(priv->supplies), priv->supplies);
	return ret;
}

static void es9219c_power_down(struct snd_soc_component *component)
{
	struct es9219c_priv *priv = snd_soc_component_get_drvdata(component);

	regcache_cache_only(priv->regmap, true);
	regcache_mark_dirty(priv->regmap);
	priv->powered = false;
	gpiod_set_value_cansleep(priv->reset_gpio, 0);
	clk_disable_unprepare(priv->mclk);
	regulator_bulk_disable(ARRAY_SIZE(priv->supplies), priv->supplies);
}

static int es9219c_set_bias_level(struct snd_soc_component *component,
				  enum snd_soc_bias_level level)
{
	int ret = 0;

	switch (level) {
	case SND_SOC_BIAS_STANDBY:
		if (snd_soc_component_get_bias_level(component) == SND_SOC_BIAS_OFF)
			ret = es9219c_power_up(component);
		break;
	case SND_SOC_BIAS_OFF:
		es9219c_power_down(component);
		break;
	default:
		break;
	}
	return ret;
}
/* Returns soft start delay in microseconds */

static unsigned long es9219c_get_soft_start_delay(struct snd_soc_component *component)
{
	struct es9219c_priv *priv = snd_soc_component_get_drvdata(component);
	unsigned long mclk;

	if (!priv->powered)
		return 0;
	mclk = clk_get_rate(priv->mclk);
	if (!mclk)
		return 0;
	return div_u64((1000000ull * 4096ull) << (1 + priv->ss_time),
		       mclk >> priv->clk_gear);
}

static void es9219c_set_amp_mode(struct snd_soc_component *component)
{
	struct es9219c_priv *priv = snd_soc_component_get_drvdata(component);
	unsigned long ss_delay;
	int cur_mode, ret, new_mode;

	if (!priv->out_mux_powered)
		new_mode = ES9219C_AMP_MODE_CORE_ON;
	else if (priv->analog_bypass)
		new_mode = ES9219C_AMP_MODE_LOWFI;
	else if (!priv->dac_powered)
		new_mode = ES9219C_AMP_MODE_CORE_ON;
	else
		new_mode = priv->output_2v ? ES9219C_AMP_MODE_HIFI_2V :
					      ES9219C_AMP_MODE_HIFI_1V;

	cur_mode = snd_soc_component_read_field(component, ES9219C_AMP_CONFIG,
						ES9219C_AMP_CONFIG_MODE);
	if (cur_mode == new_mode)
		return;
	ss_delay = es9219c_get_soft_start_delay(component);
	ret = snd_soc_component_write_field(component, ES9219C_AMP_CONFIG,
					    ES9219C_AMP_CONFIG_MODE,
					    ES9219C_AMP_MODE_CORE_ON);
	if (ret < 0)
		return;
	if (ret) {
		/* Extra settling time is required for the mode change. */
		usleep_range(ss_delay + 25000, ss_delay + 26000);
	}
	ret = snd_soc_component_write_field(component, ES9219C_AMP_CONFIG,
					    ES9219C_AMP_CONFIG_MODE, new_mode);
	if (ret < 0)
		return;
	if (ret)
		usleep_range(ss_delay, ss_delay + 1000);
}

static int es9219c_output_dapm_event(struct snd_soc_dapm_widget *widget,
				     struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(widget->dapm);
	struct es9219c_priv *priv = snd_soc_component_get_drvdata(component);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		if (widget->id == snd_soc_dapm_dac)
			priv->dac_powered = true;
		else
			priv->out_mux_powered = true;
		break;
	case SND_SOC_DAPM_POST_PMD:
		if (widget->id == snd_soc_dapm_dac)
			priv->dac_powered = false;
		else
			priv->out_mux_powered = false;
		break;
	default:
		return 0;
	}
	es9219c_set_amp_mode(component);
	return 0;
}

static int es9219c_outlevel_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct es9219c_priv *priv = snd_soc_component_get_drvdata(component);

	ucontrol->value.enumerated.item[0] = priv->output_2v;
	return 0;
}

static int es9219c_outlevel_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct es9219c_priv *priv = snd_soc_component_get_drvdata(component);

	priv->output_2v = ucontrol->value.enumerated.item[0];
	es9219c_set_amp_mode(component);
	return 1;
}

static int es9219c_softstart_get(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct es9219c_priv *priv = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = priv->soft_start_time;
	return 0;
}

static int es9219c_softstart_put(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct es9219c_priv *priv = snd_soc_component_get_drvdata(component);

	priv->soft_start_time = ucontrol->value.integer.value[0];
	if (es9219c_apply_clk_params(component))
		return 0;
	return 1;
}

static int es9219c_output_mux_get(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_dapm_kcontrol_component(kcontrol);
	struct es9219c_priv *priv = snd_soc_component_get_drvdata(component);

	ucontrol->value.enumerated.item[0] = priv->analog_bypass;
	return 0;
}

static int es9219c_output_mux_put(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_dapm_kcontrol_component(kcontrol);
	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_dapm(kcontrol);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	struct es9219c_priv *priv = snd_soc_component_get_drvdata(component);
	unsigned int value = ucontrol->value.enumerated.item[0];

	priv->analog_bypass = value;
	es9219c_set_amp_mode(component);
	snd_soc_dapm_mux_update_power(dapm, kcontrol, value, e, NULL);
	return 1;
}

static DECLARE_TLV_DB_SCALE(analog_vol_scale, -2400, 100, 0);

static DECLARE_TLV_DB_SCALE(digital_vol_scale, -12750, 50, 0);

/* ES9219C defines only two valid filter shapes; all others are reserved. */
static const char * const es9219c_digfilter_texts[] = {
	"Linear Phase Fast",
	"Hybrid Fast",
};

static int es9219c_digfilter_get(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	unsigned int reg = snd_soc_component_read(component, ES9219C_FBW_MUTE);

	ucontrol->value.enumerated.item[0] =
		FIELD_GET(ES9219C_FILTER_SHAPE, reg) == 6;
	return 0;
}

static int es9219c_digfilter_put(struct snd_kcontrol *kcontrol,
			 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	unsigned int val = ucontrol->value.enumerated.item[0] ? 6 : 0;

	return snd_soc_component_update_bits(component, ES9219C_FBW_MUTE,
					    ES9219C_FILTER_SHAPE,
					    FIELD_PREP(ES9219C_FILTER_SHAPE, val));
}

static SOC_ENUM_SINGLE_EXT_DECL(es9219c_digfilter_enum, es9219c_digfilter_texts);

static const char * const es9219c_outlevel_texts[] = { "1V", "2V" };

static SOC_ENUM_SINGLE_EXT_DECL(es9219c_outlevel_enum, es9219c_outlevel_texts);

static const char * const es9219c_channel_mux_texts[] = { "CH1", "CH2" };

static SOC_ENUM_DOUBLE_DECL(es9219c_channel_mux_enum, ES9219C_MIX_AMUTE, 0, 2, es9219c_channel_mux_texts);

static int es9219c_s16_info(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = -32768;
	uinfo->value.integer.max = 32767;
	return 0;
}

#define ES9219C_SIGNED16_EXT(_name, _reg, _get, _put)				\
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (_name),			\
	.info = es9219c_s16_info, .get = (_get), .put = (_put),			\
	.private_value = SOC_SINGLE_VALUE((_reg), 0, 0xffff, 0, 0) }

/* 16-bit coefficients are little-endian: base register holds the low byte. */
static int es9219c_s16_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	unsigned int lo = snd_soc_component_read(component, mc->reg);
	unsigned int hi = snd_soc_component_read(component, mc->reg + 1);

	ucontrol->value.integer.value[0] = (s16)(lo | (hi << 8));
	return 0;
}

static int es9219c_s16_put(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct es9219c_priv *priv = snd_soc_component_get_drvdata(component);
	s16 val = ucontrol->value.integer.value[0];

	return es9219c_wide_write(priv->regmap, mc->reg, 2, val);
}

static const struct snd_kcontrol_new es9219c_controls[] = {
	SOC_SINGLE_TLV("Headphone Playback Volume",
		       ES9219C_ANALOGVOL, 0, 24, 1, analog_vol_scale),
	SOC_DOUBLE_R_TLV("Master Playback Volume",
			 ES9219C_VOL_CH1, ES9219C_VOL_CH2, 0, 0xff, 1, digital_vol_scale),
	SOC_ENUM_EXT("Digital Filter Roll-Off", es9219c_digfilter_enum, es9219c_digfilter_get, es9219c_digfilter_put),
	SOC_ENUM_EXT("Output Level", es9219c_outlevel_enum, es9219c_outlevel_get, es9219c_outlevel_put),
	SOC_ENUM("Channel Mux", es9219c_channel_mux_enum),
	SOC_SINGLE_EXT("Soft Start Time", SND_SOC_NOPM, 0, 20, 0, es9219c_softstart_get, es9219c_softstart_put),
	SOC_SINGLE("DRE Switch", ES9219C_DRE_CONFIG, 0, 1, 0),
	SOC_SINGLE("DoP Enable", ES9219C_DOP_VOL_RAMP, 3, 1, 0),
	SOC_SINGLE("Crosstalk Compensation", ES9219C_CROSSTALK_CONFIG, 7, 1, 1),
	ES9219C_SIGNED16_EXT("THD Compensation C2", ES9219C_THD_C2, es9219c_s16_get, es9219c_s16_put),
	ES9219C_SIGNED16_EXT("THD Compensation C3", ES9219C_THD_C3, es9219c_s16_get, es9219c_s16_put),
	ES9219C_SIGNED16_EXT("Crosstalk Scale Ch1", ES9219C_CT_SCALE_CH1, es9219c_s16_get, es9219c_s16_put),
	ES9219C_SIGNED16_EXT("Crosstalk Scale Ch2", ES9219C_CT_SCALE_CH2, es9219c_s16_get, es9219c_s16_put),
};

static const char * const es9219c_output_mux_texts[] = { "Digital Input", "AUX Input" };

static SOC_ENUM_SINGLE_EXT_DECL(es9219c_output_mux_enum, es9219c_output_mux_texts);

static const struct snd_kcontrol_new es9219c_output_mux_ctl =
	SOC_DAPM_ENUM_EXT("Output Source", es9219c_output_mux_enum, es9219c_output_mux_get, es9219c_output_mux_put);

static const struct snd_soc_dapm_widget es9219c_dapm_widgets[] = {
	SND_SOC_DAPM_INPUT("AUX_L"),
	SND_SOC_DAPM_INPUT("AUX_R"),
	SND_SOC_DAPM_DAC_E("DAC", "Playback", SND_SOC_NOPM, 0, 0,
			   es9219c_output_dapm_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MUX_E("Output Mux", SND_SOC_NOPM, 0, 0,
			   &es9219c_output_mux_ctl, es9219c_output_dapm_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUTPUT("OUTL"),
	SND_SOC_DAPM_OUTPUT("OUTR"),
};

static const struct snd_soc_dapm_route es9219c_dapm_routes[] = {
	{"Output Mux", "Digital Input", "DAC"},
	{"Output Mux", "AUX Input", "AUX_L"},
	{"Output Mux", "AUX Input", "AUX_R"},
	{"OUTL", NULL, "Output Mux"},
	{"OUTR", NULL, "Output Mux"},
};

static const struct snd_soc_component_driver es9219c_component = {
	.set_bias_level		= es9219c_set_bias_level,
	.controls		= es9219c_controls,
	.num_controls		= ARRAY_SIZE(es9219c_controls),
	.dapm_widgets		= es9219c_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(es9219c_dapm_widgets),
	.dapm_routes		= es9219c_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(es9219c_dapm_routes),
};

static const struct regmap_range es9219c_readable_ranges[] = {
	regmap_reg_range(ES9219C_SYSTEM, ES9219C_THD_C3_CH2+1),
	regmap_reg_range(ES9219C_CHIP_STATUS, ES9219C_MAX_REGISTER),
};

static const struct regmap_access_table es9219c_readable_table = {
	.yes_ranges = es9219c_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(es9219c_readable_ranges),
};

static const struct regmap_range es9219c_writeable_ranges[] = {
	regmap_reg_range(ES9219C_SYSTEM, ES9219C_THD_C3_CH2+1),
	regmap_reg_range(ES9219C_CROSSTALK_CONFIG, ES9219C_DRE_CONFIG),
};

static const struct regmap_access_table es9219c_writeable_table = {
	.yes_ranges = es9219c_writeable_ranges,
	.n_yes_ranges = ARRAY_SIZE(es9219c_writeable_ranges),
};

static const struct regmap_range es9219c_volatile_ranges[] = {
	regmap_reg_range(ES9219C_FIR_ADDR, ES9219C_FIR_CONFIG),
	regmap_reg_range(ES9219C_CHIP_STATUS, ES9219C_DPLL_NUMBER+3),
	regmap_reg_range(ES9219C_INPUT_STATUS, ES9219C_RAM_READBACK+2),
};

static const struct regmap_access_table es9219c_volatile_table = {
	.yes_ranges = es9219c_volatile_ranges,
	.n_yes_ranges = ARRAY_SIZE(es9219c_volatile_ranges),
};

static const u8 es9219c_reg_defaults[] = {
	0x00, 0x8c, 0x34, 0x58, 0x00, 0x68, 0x72, 0x80, 0xdd, 0x18, 0x02, 0x00,
	0x5a, 0x40, 0x0a, 0x50, 0x50, 0xff, 0xff, 0xff, 0x7f, 0x0d, 0x00, 0x00,
	0x00, 0x00, 0x62, 0xc4, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x00,
	0x00, 0x00, 0x00, 0xcc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x62, 0xc0, 0x0d, 0x62, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x81, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
};

static const struct regmap_config es9219c_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.rd_table = &es9219c_readable_table,
	.wr_table = &es9219c_writeable_table,
	.volatile_table = &es9219c_volatile_table,
	.max_register = ES9219C_MAX_REGISTER,
	.reg_defaults_raw = es9219c_reg_defaults,
	.num_reg_defaults_raw = ARRAY_SIZE(es9219c_reg_defaults),
	.cache_type = REGCACHE_RBTREE,
};

static void es9219c_read_properties(struct device *dev,
				    struct es9219c_priv *priv)
{
	u32 val;

	if (!device_property_read_u32(dev, "ess,max-clock-div", &val)) {
		if (!val || val > 8) {
			dev_warn(dev, "invalid max-clock-div %u, using 1\n", val);
			val = 1;
		}
		priv->max_clk_div = val;
	}
	if (!device_property_read_u32(dev, "ess,oscillator-pad-bias", &val)) {
		if (val > 15) {
			dev_warn(dev, "invalid oscillator-pad-bias %u, clamping\n", val);
			val = 15;
		}
		priv->osc_drv_bias = val;
	}
}

static int es9219c_i2c_probe(struct i2c_client *i2c,
			     const struct i2c_device_id *id)
{
	struct device *dev = &i2c->dev;
	struct es9219c_priv *priv;
	unsigned int val;
	int ret, i;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->regmap = devm_regmap_init_i2c(i2c, &es9219c_regmap);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap), "Failed to create regmap\n");
	for (i = 0; i < ARRAY_SIZE(priv->supplies); i++)
		priv->supplies[i].supply = es9219c_supply_names[i];
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(priv->supplies),
					priv->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request supplies\n");
	priv->mclk = devm_clk_get(dev, "mclk");
	if (IS_ERR(priv->mclk))
		return dev_err_probe(dev, PTR_ERR(priv->mclk), "Failed to request MCLK\n");
	priv->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(priv->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->reset_gpio),
				     "Failed to request reset GPIO\n");
	es9219c_read_properties(dev, priv);

	/* The chip must be powered and out of reset to answer the ID read. */
	ret = regulator_bulk_enable(ARRAY_SIZE(priv->supplies), priv->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot enable supplies\n");
	ret = clk_prepare_enable(priv->mclk);
	if (ret) {
		regulator_bulk_disable(ARRAY_SIZE(priv->supplies), priv->supplies);
		return dev_err_probe(dev, ret, "Cannot enable MCLK\n");
	}
	gpiod_set_value_cansleep(priv->reset_gpio, 1);
	usleep_range(1500, 2000);
	gpiod_set_value_cansleep(priv->reset_gpio, 0);
	msleep(20);
	for (i = 0; i < 3; i++) {
		ret = regmap_read(priv->regmap, ES9219C_CHIP_STATUS, &val);
		if (!ret)
			break;
		msleep(20);
	}
	clk_disable_unprepare(priv->mclk);
	regulator_bulk_disable(ARRAY_SIZE(priv->supplies), priv->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read chip ID\n");
	if (FIELD_GET(ES9219C_CHIP_ID_MASK, val) != ES9219C_CHIP_ID)
		return dev_err_probe(dev, -ENODEV, "Not an ES9219C (chip ID 0x%02x)\n",
				     FIELD_GET(ES9219C_CHIP_ID_MASK, val));

	/* Codec is only powered when needed after this point. */
	regcache_cache_only(priv->regmap, true);
	/* Arbitrary; must be nonzero */
	priv->fsr = 48000;
	i2c_set_clientdata(i2c, priv);
	return devm_snd_soc_register_component(dev, &es9219c_component, &es9219c_dai, 1);
}

static const struct of_device_id es9219c_of_match[] = {
	{ .compatible = "ess,es9219c" },
	{ }
};
MODULE_DEVICE_TABLE(of, es9219c_of_match);

static const struct i2c_device_id es9219c_i2c_id[] = {
	{ "es9219c", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, es9219c_i2c_id);

static struct i2c_driver es9219c_i2c_driver = {
	.driver = {
		.name = "es9219c",
		.of_match_table = of_match_ptr(es9219c_of_match),
	},
	.probe = es9219c_i2c_probe,
	.id_table = es9219c_i2c_id,
};
module_i2c_driver(es9219c_i2c_driver);
MODULE_DESCRIPTION("ESS ES9219C ALSA SoC Codec Driver");
MODULE_AUTHOR("Aidan MacDonald <aidanmacdonald.0x0@xxxxxxxxx>");
MODULE_LICENSE("GPL");
