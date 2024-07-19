// SPDX-License-Identifier: GPL-2.0-only
/*
 *  cht_bsw_rt5659.c - ASoc Machine driver for Intel Cherryview-based platforms
 *                     Cherrytrail and Braswell, with RT5659 codec.
 *
 *  Copyright (C) 2014 Intel Corp
 *  Author: Subhransu S. Prusty <subhransu.s.prusty@intel.com>
 *          Mengdong Lin <mengdong.lin@intel.com>
 */

#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/soc-acpi.h>
#include <asm/platform_sst_audio.h>
#include "../../codecs/rt5659.h"
#include "../atom/sst-atom-controls.h"
#include "../common/soc-intel-quirks.h"


/* The platform clock #3 outputs 19.2Mhz clock to codec as I2S MCLK */
#define CHT_PLAT_CLK_3_HZ	19200000
#define CHT_CODEC_DAI	"rt5659-aif1"
#define CHT_CODEC_DAI2	"rt5659-aif2"
#define CHT_CODEC_DAI3	"rt5659-aif3"

struct cht_mc_private {
	struct snd_soc_jack headset;
	char codec_name[SND_ACPI_I2C_ID_LEN];
	struct clk *mclk;
	bool use_ssp0;
};

/* Headset jack detection DAPM pins */
static struct snd_soc_jack_pin cht_bsw_headset_pins[] = {
	{
		.pin = "Headset Mic",
		.mask = SND_JACK_MICROPHONE,
	},
	{
		.pin = "Headphone",
		.mask = SND_JACK_HEADPHONE,
	},
};

static int platform_clock_control(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *k, int  event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct snd_soc_dai *codec_dai;
	struct cht_mc_private *ctx = snd_soc_card_get_drvdata(card);
	int ret;

	codec_dai = snd_soc_card_get_codec_dai(card, CHT_CODEC_DAI);
	if (!codec_dai) {
		dev_err(card->dev, "Codec dai not found; Unable to set platform clock\n");
		return -EIO;
	}

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		if (ctx->mclk) {
			ret = clk_prepare_enable(ctx->mclk);
			if (ret < 0) {
				dev_err(card->dev,
					"could not configure MCLK state");
				return ret;
			}
		}

		/* set codec PLL source to the 19.2MHz platform clock (MCLK) */
		ret = snd_soc_dai_set_pll(codec_dai, 0, RT5659_PLL1_S_MCLK,
				CHT_PLAT_CLK_3_HZ, 48000 * 512);
		if (ret < 0) {
			dev_err(card->dev, "can't set codec pll: %d\n", ret);
			return ret;
		}

		/* set codec sysclk source to PLL */
		ret = snd_soc_dai_set_sysclk(codec_dai, RT5659_SCLK_S_PLL1,
			48000 * 512, SND_SOC_CLOCK_IN);
		if (ret < 0) {
			dev_err(card->dev, "can't set codec sysclk: %d\n", ret);
			return ret;
		}
	} else {
		/* Set codec sysclk source to its internal clock because codec
		 * PLL will be off when idle and MCLK will also be off by ACPI
		 * when codec is runtime suspended. Codec needs clock for jack
		 * detection and button press.
		 */
		ret = snd_soc_dai_set_sysclk(codec_dai, RT5659_SCLK_S_RCCLK,
					     48000 * 512, SND_SOC_CLOCK_IN);
		if (ret < 0) {
			dev_err(card->dev, "failed to set codec sysclk: %d\n", ret);
			return ret;
		}

		if (ctx->mclk)
			clk_disable_unprepare(ctx->mclk);
	}
	return 0;
}

static const struct snd_soc_dapm_widget cht_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Int Mic", NULL),
	SND_SOC_DAPM_SPK("Ext Spk", NULL),
	SND_SOC_DAPM_SUPPLY("Platform Clock", SND_SOC_NOPM, 0, 0,
			platform_clock_control, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMD),
};

static const struct snd_soc_dapm_route cht_audio_map[] = {
	{"IN1P", NULL, "Headset Mic"},
	{"IN1N", NULL, "Headset Mic"},
	{"IN3P", NULL, "Int Mic"},
	{"IN3N", NULL, "Int Mic"},
	{"IN4P", NULL, "Int Mic"},
	{"IN4N", NULL, "Int Mic"},
	{"Int Mic", NULL, "MICBIAS2"},
	{"Int Mic", NULL, "MICBIAS3"},
	{"Headphone", NULL, "HPOL"},
	{"Headphone", NULL, "HPOR"},
	{"Ext Spk", NULL, "SPOL"},
	{"Ext Spk", NULL, "SPOR"},
};

static const struct snd_soc_dapm_route cht_audio_ssp0_map[] = {
	{"AIF1 Playback", NULL, "ssp0 Tx"},
	{"ssp0 Tx", NULL, "modem_out"},
	{"modem_in", NULL, "ssp0 Rx"},
	{"ssp0 Rx", NULL, "AIF1 Capture"},
};

static const struct snd_soc_dapm_route cht_audio_ssp2_map[] = {
	{"AIF1 Playback", NULL, "ssp2 Tx"},
	{"ssp2 Tx", NULL, "codec_out0"},
	{"ssp2 Tx", NULL, "codec_out1"},
	{"codec_in0", NULL, "ssp2 Rx"},
	{"codec_in1", NULL, "ssp2 Rx"},
	{"ssp2 Rx", NULL, "AIF1 Capture"},
	{"AIF1 Playback", NULL, "Platform Clock"},
	{"AIF1 Capture", NULL, "Platform Clock"},
};

static const struct snd_kcontrol_new cht_mc_controls[] = {
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
	SOC_DAPM_PIN_SWITCH("Int Mic"),
	SOC_DAPM_PIN_SWITCH("Ext Spk"),
};

static int cht_aif1_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	unsigned int fmt;
	int ret;
//////////////////////////////////////////////////////////////////
	pr_debug("Enter:%s, codec_dai name: %s\n", __func__,codec_dai->name);

	/* proceed only if dai is valid */
	if (strncmp(codec_dai->name, "rt5659-aif1", 11))
		return 0;

	/* TDM 4 slot 24 bit set the Rx and Tx bitmask to
	 * 4 active slots as 0xF
	 */
	ret = snd_soc_dai_set_tdm_slot(codec_dai, 0xF, 0xF, 4,
			SNDRV_PCM_FORMAT_GSM);
	if (ret < 0) {
		pr_err("can't set codec TDM slot %d\n", ret);
		return ret;
	}

	/* TDM slave Mode */
	fmt =   SND_SOC_DAIFMT_DSP_B | SND_SOC_DAIFMT_IB_NF
		| SND_SOC_DAIFMT_CBS_CFS;

	/* Set codec DAI configuration */
	ret = snd_soc_dai_set_fmt(codec_dai, fmt);
	if (ret < 0) {
		pr_err("can't set codec DAI configuration %d\n", ret);
		return ret;
	}
////////////////////////////////////////////////////////////////////

	/* set codec PLL source to the 19.2MHz platform clock (MCLK) */
	ret = snd_soc_dai_set_pll(codec_dai, 0, RT5659_PLL1_S_MCLK,
				  CHT_PLAT_CLK_3_HZ, params_rate(params) * 512);
	if (ret < 0) {
		dev_err(rtd->dev, "can't set codec pll: %d\n", ret);
		return ret;
	}

	/* set codec sysclk source to PLL */
	ret = snd_soc_dai_set_sysclk(codec_dai, RT5659_SCLK_S_PLL1,
				     params_rate(params) * 512,
				     SND_SOC_CLOCK_IN);
	if (ret < 0) {
		dev_err(rtd->dev, "can't set codec sysclk: %d\n", ret);
		return ret;
	}
	return 0;
}

static const struct acpi_gpio_params headset_gpios = { 0, 0, false };

static const struct acpi_gpio_mapping cht_rt5659_gpios[] = {
	{ "headset-gpios", &headset_gpios, 1 },
	{},
};
/*
static int cht_codec_init(struct snd_soc_pcm_runtime *runtime)
{
	int ret;
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(runtime, 0);
	struct snd_soc_component *component = codec_dai->component;
	struct cht_mc_private *ctx = snd_soc_card_get_drvdata(runtime->card);

	if (devm_acpi_dev_add_driver_gpios(component->dev, cht_rt5659_gpios))
		dev_warn(runtime->dev, "Unable to add GPIO mapping table\n");

	// Select codec ASRC clock source to track I2S1 clock, because codec
	// is in slave mode and 100fs I2S format (BCLK = 100 * LRCLK) cannot
	// be supported by RT5672. Otherwise, ASRC will be disabled and cause
	// noise.

	rt5659_sel_asrc_clk_src(component,
				RT5659_DA_STEREO_FILTER
				| RT5659_DA_MONO_L_FILTER
				| RT5659_DA_MONO_R_FILTER
				| RT5659_AD_STEREO_FILTER
				| RT5659_AD_MONO_L_FILTER
				| RT5659_AD_MONO_R_FILTER,
				RT5659_CLK_SEL_I2S1_ASRC);

	if (ctx->use_ssp0) {
		ret = snd_soc_dapm_add_routes(&runtime->card->dapm,
					      cht_audio_ssp0_map,
					      ARRAY_SIZE(cht_audio_ssp0_map));
	} else {
		ret = snd_soc_dapm_add_routes(&runtime->card->dapm,
					      cht_audio_ssp2_map,
					      ARRAY_SIZE(cht_audio_ssp2_map));
	}
	if (ret)
		return ret;

	ret = snd_soc_card_jack_new_pins(runtime->card, "Headset",
					 SND_JACK_HEADSET | SND_JACK_BTN_0 |
					 SND_JACK_BTN_1 | SND_JACK_BTN_2,
					 &ctx->headset,
					 cht_bsw_headset_pins,
					 ARRAY_SIZE(cht_bsw_headset_pins));
        if (ret)
                return ret;

	snd_jack_set_key(ctx->headset.jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
	snd_jack_set_key(ctx->headset.jack, SND_JACK_BTN_1, KEY_VOLUMEUP);
	snd_jack_set_key(ctx->headset.jack, SND_JACK_BTN_2, KEY_VOLUMEDOWN);

	rt5659_set_jack_detect(component, &ctx->headset);
	if (ctx->mclk) {
		// The firmware might enable the clock at
		// boot (this information may or may not
		// be reflected in the enable clock register).
		// To change the rate we must disable the clock
		// first to cover these cases. Due to common
		// clock framework restrictions that do not allow
		// to disable a clock that has not been enabled,
		// we need to enable the clock first.
		ret = clk_prepare_enable(ctx->mclk);
		if (!ret)
			clk_disable_unprepare(ctx->mclk);

		ret = clk_set_rate(ctx->mclk, CHT_PLAT_CLK_3_HZ);

		if (ret) {
			dev_err(runtime->dev, "unable to set MCLK rate\n");
			return ret;
		}
	}
	return 0;
}
*/
static int cht_audio_init(struct snd_soc_pcm_runtime *runtime)
{
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(runtime, 0);
	struct snd_soc_component *component = codec_dai->component;
	struct cht_mc_private *ctx = snd_soc_card_get_drvdata(runtime->card);
    int ret;
	struct snd_soc_card *card = runtime->card;

	if (ctx->use_ssp0) {
		ret = snd_soc_dapm_add_routes(&runtime->card->dapm,
					      cht_audio_ssp0_map,
					      ARRAY_SIZE(cht_audio_ssp0_map));
        pr_debug("%s add cht_audio_ssp0_map got ret %d \n", __func__,ret);
	} else {
		ret = snd_soc_dapm_add_routes(&runtime->card->dapm,
					      cht_audio_ssp2_map,
					      ARRAY_SIZE(cht_audio_ssp2_map));
        pr_debug("%s add cht_audio_ssp2_map got ret %d \n", __func__,ret);
	}

	int codec_gpio;
	int pol = 0, val = 0;
	struct gpio_desc *desc;

	pr_debug("Enter:%s", __func__);

	/* Set codec bias level */
    card->dapm.bias_level = SND_SOC_BIAS_OFF;
	card->dapm.idle_bias_off = true;

	desc = devm_gpiod_get_index(component->dev, NULL, 0, 0);
	if (!IS_ERR(desc)) {
		codec_gpio = desc_to_gpio(desc);
		devm_gpiod_put(component->dev, desc);
		pr_debug("%s: GPIOs - JD/BP-int: %d (pol = %d, val = %d)\n",
				__func__, codec_gpio, pol, val);

	} else {
		codec_gpio = -1;
		pr_err("%s: GPIOs - JD/BP-int: Not present!\n", __func__);
	}

/*
	hs_gpio.gpio = codec_gpio;

	ctx->intr_debounce = CHT_INTR_DEBOUNCE;
	ctx->hs_insert_det_delay = CHT_HS_INSERT_DET_DELAY;
	ctx->hs_remove_det_delay = CHT_HS_REMOVE_DET_DELAY;
	ctx->button_det_delay = CHT_BUTTON_DET_DELAY;
	ctx->hs_det_poll_intrvl = CHT_HS_DET_POLL_INTRVL;
	ctx->hs_det_retry = CHT_HS_DET_RETRY_COUNT;
	ctx->button_en_delay = CHT_BUTTON_EN_DELAY;
	ctx->process_button_events = false;
	//wake_lock_init(&ctx->jack_wake_lock, WAKE_LOCK_SUSPEND, "jack_wakelock");

	INIT_DELAYED_WORK(&ctx->hs_insert_work, cht_check_hs_insert_status);
	INIT_DELAYED_WORK(&ctx->hs_remove_work, cht_check_hs_remove_status);
	INIT_DELAYED_WORK(&ctx->hs_button_work, cht_check_hs_button_status);
	INIT_DELAYED_WORK(&ctx->hs_button_en_work, cht_enable_hs_button_events);

	mutex_init(&ctx->jack_mlock);

	ret = snd_soc_jack_new(codec, "Intel MID Audio Jack",
			       SND_JACK_HEADSET | SND_JACK_HEADPHONE | SND_JACK_BTN_0 |
			       SND_JACK_BTN_1 | SND_JACK_BTN_2, &ctx->jack);
	if (ret) {
		pr_err("jack creation failed\n");
		return ret;
	}

	snd_jack_set_key(ctx->jack.jack, SND_JACK_BTN_0, KEY_MEDIA);
	snd_jack_set_key(ctx->jack.jack, SND_JACK_BTN_1, BTN_1);
	snd_jack_set_key(ctx->jack.jack, SND_JACK_BTN_2, BTN_2);

	ret = snd_soc_jack_add_gpios(&ctx->jack, 1, &hs_gpio);
	if (ret) {
		pr_err("adding jack GPIO failed\n");
		return ret;
	}
*/
	/* Keep the voice call paths active during
	 * suspend. Mark the end points ignore_suspend
	 */
	/*TODO: CHECK this */
	snd_soc_dapm_ignore_suspend(&component->dapm, "HPOL");
	snd_soc_dapm_ignore_suspend(&component->dapm, "HPOR");

	snd_soc_dapm_ignore_suspend(&component->dapm, "SPOL");
	snd_soc_dapm_ignore_suspend(&component->dapm, "SPOR");

	snd_soc_dapm_enable_pin(&card->dapm, "Headset Mic");
	snd_soc_dapm_enable_pin(&card->dapm, "Headphone");
	snd_soc_dapm_enable_pin(&card->dapm, "Ext Spk");
	snd_soc_dapm_enable_pin(&card->dapm, "Int Mic");

	snd_soc_dapm_sync(&card->dapm);
	return ret;
}

static int cht_codec_fixup(struct snd_soc_pcm_runtime *rtd,
			    struct snd_pcm_hw_params *params)
{
	struct cht_mc_private *ctx = snd_soc_card_get_drvdata(rtd->card);
	struct snd_interval *rate = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
						SNDRV_PCM_HW_PARAM_CHANNELS);
	// int ret, bits;

	pr_debug("Invoked %s for dailink %s\n", __func__, rtd->dai_link->name);

	/* The DSP will covert the FE rate to 48k, stereo, 24bits */
	rate->min = rate->max = 48000;
	channels->min = channels->max = 4;

	if (ctx->use_ssp0) {
		// set SSP0 to 16-bit
		params_set_format(params, SNDRV_PCM_FORMAT_S16_LE);
		// bits = 16;
	} else {
		// set SSP2 to 24-bit
		params_set_format(params, SNDRV_PCM_FORMAT_S24_LE);
		// bits = 24;
	}

	/*
	 * The default mode for the cpu-dai is TDM 4 slot. The default mode
	 * for the codec-dai is I2S. So we need to either set the cpu-dai to
	 * I2S mode to match the codec-dai, or set the codec-dai to TDM 4 slot
	 * (or program both to yet another mode).
	 * One board, the Lenovo Miix 2 10, uses not 1 but 2 codecs connected
	 * to SSP2. The second piggy-backed, output-only codec is inside the
	 * keyboard-dock (which has extra speakers). Unlike the main rt5672
	 * codec, we cannot configure this codec, it is hard coded to use
	 * 2 channel 24 bit I2S. For this to work we must use I2S mode on this
	 * board. Since we only support 2 channels anyways, there is no need
	 * for TDM on any cht-bsw-rt5659 designs. So we use I2S 2ch everywhere.
	 */
	 /*
	ret = snd_soc_dai_set_fmt(snd_soc_rtd_to_cpu(rtd, 0),
				  SND_SOC_DAIFMT_I2S     |
				  SND_SOC_DAIFMT_NB_NF   |
				  SND_SOC_DAIFMT_BP_FP);
	if (ret < 0) {
		dev_err(rtd->dev, "can't set format to I2S, err %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_tdm_slot(snd_soc_rtd_to_cpu(rtd, 0), 0x3, 0x3, 2, bits);
	if (ret < 0) {
		dev_err(rtd->dev, "can't set I2S config, err %d\n", ret);
		return ret;
	}
	*/
	return 0;
}

static int cht_aif1_startup(struct snd_pcm_substream *substream)
{
    struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
    struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
    struct snd_soc_dapm_widget_list *list_1;
	struct snd_soc_dapm_widget *widget;
	int stream = 0;
	int paths, i;

	pr_info("%s runtime=%p\n", __func__, substream->runtime);

    paths = snd_soc_dapm_dai_get_connected_widgets(cpu_dai, stream, &list_1, NULL);
    pr_info("%d paths got\n", paths);

    for_each_dapm_widgets(list_1, i, widget) {
        pr_info("rt5659 path widget: name=%s, sname=%s\n", widget->name,widget->sname);
    }

	return snd_pcm_hw_constraint_single(substream->runtime,
			SNDRV_PCM_HW_PARAM_RATE, 48000);
}

static const struct snd_soc_ops cht_aif1_ops = {
	.startup = cht_aif1_startup,
};

static const struct snd_soc_ops cht_be_ssp2_ops = {
	.hw_params = cht_aif1_hw_params,
};

SND_SOC_DAILINK_DEF(dummy,
	DAILINK_COMP_ARRAY(COMP_DUMMY()));

SND_SOC_DAILINK_DEF(media,
	DAILINK_COMP_ARRAY(COMP_CPU("media-cpu-dai")));

SND_SOC_DAILINK_DEF(deepbuffer,
	DAILINK_COMP_ARRAY(COMP_CPU("deepbuffer-cpu-dai")));

SND_SOC_DAILINK_DEF(ssp2_port,
	DAILINK_COMP_ARRAY(COMP_CPU("ssp2-port")));
SND_SOC_DAILINK_DEF(ssp2_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("i2c-10EC5659:00",
				      "rt5659-aif1")));

SND_SOC_DAILINK_DEF(platform,
	DAILINK_COMP_ARRAY(COMP_PLATFORM("sst-mfld-platform")));

SND_SOC_DAILINK_DEF(rt5659_aif2_cpu,
	DAILINK_COMP_ARRAY(COMP_CPU("rt5659-aif2")));

SND_SOC_DAILINK_DEF(spk_l_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("i2c-tfa9890:00",
				      "tfa989x-hifi")));

// SND_SOC_DAILINK_DEF(spk_r_codec,
// 	DAILINK_COMP_ARRAY(COMP_CODEC("i2c-tfa9890:01",
// 				      "tfa989x-hifi")));

static const struct snd_soc_pcm_stream nxp_tfa989x_params[] = {
    {
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.rates = SNDRV_PCM_RATE_48000,
	.sig_bits = 16,
    },
};

static struct snd_soc_dai_link cht_dailink[] = {
	/* Front End DAI links */
	[MERR_DPCM_AUDIO] = {
		.name = "Audio Port",
		.stream_name = "Audio",
		.nonatomic = true,
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ops = &cht_aif1_ops,
		SND_SOC_DAILINK_REG(media, dummy, platform),
	},
	[MERR_DPCM_DEEP_BUFFER] = {
		.name = "Deep-Buffer Audio Port",
		.stream_name = "Deep-Buffer Audio",
		.nonatomic = true,
		.dynamic = 1,
		.dpcm_playback = 1,
		.ops = &cht_aif1_ops,
		SND_SOC_DAILINK_REG(deepbuffer, dummy, platform),
	},

	/* Back End DAI links */
	{
		/* SSP2 - Codec */
		.name = "SSP2-Codec",
		.id = 0,
		.no_pcm = 1,
		//.init = cht_codec_init,
		.init = cht_audio_init,
		.be_hw_params_fixup = cht_codec_fixup,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ops = &cht_be_ssp2_ops,
		SND_SOC_DAILINK_REG(ssp2_port, ssp2_codec, platform),
		.ignore_suspend = 1,
        .ignore_pmdown_time = 1,
	},
	{
		.name = "rt5659_AIF2-TFA989x_Speaker_L",
		.stream_name = "aif2-spk_l",
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(rt5659_aif2_cpu, spk_l_codec),
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBS_CFS,
		.c2c_params = nxp_tfa989x_params,
        .num_c2c_params = 1,
	},
	// },{
	// 	.name = "rt5659_AIF2-TFA989x_Speaker_R",
	// 	.stream_name = "aif2-spk_r",
	// 	.dpcm_playback = 1,
	// 	.dpcm_capture = 1,
	// 	SND_SOC_DAILINK_REG(rt5659_aif2_cpu, spk_r_codec),
	// 	.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
	// 		SND_SOC_DAIFMT_CBS_CFS,
	// 	.c2c_params = nxp_tfa989x_params,
    //     .num_c2c_params = 1,
	// },
};

static int cht_suspend_pre(struct snd_soc_card *card)
{
	struct snd_soc_component *component;
	struct cht_mc_private *ctx = snd_soc_card_get_drvdata(card);

	for_each_card_components(card, component) {
		if (!strncmp(component->name,
			     ctx->codec_name, sizeof(ctx->codec_name))) {

			dev_dbg(component->dev, "disabling jack detect before going to suspend.\n");
			//rt5659_jack_suspend(component);
			break;
		}
	}
	return 0;
}

static int cht_resume_post(struct snd_soc_card *card)
{
	struct snd_soc_component *component;
	struct cht_mc_private *ctx = snd_soc_card_get_drvdata(card);

	for_each_card_components(card, component) {
		if (!strncmp(component->name,
			     ctx->codec_name, sizeof(ctx->codec_name))) {

			dev_dbg(component->dev, "enabling jack detect for resume.\n");
			//rt5659_jack_resume(component);
			break;
		}
	}

	return 0;
}

/* use space before codec name to simplify card ID, and simplify driver name */
#define SOF_CARD_NAME "bytcht rt5659" /* card name will be 'sof-bytcht rt5659' */
#define SOF_DRIVER_NAME "SOF"

#define CARD_NAME "cht-bsw-rt5659"
#define DRIVER_NAME NULL /* card name will be used for driver name */

/* SoC card */
static struct snd_soc_card snd_soc_card_cht = {
	.owner = THIS_MODULE,
	.dai_link = cht_dailink,
	.num_links = ARRAY_SIZE(cht_dailink),
	.dapm_widgets = cht_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cht_dapm_widgets),
	.dapm_routes = cht_audio_map,
	.num_dapm_routes = ARRAY_SIZE(cht_audio_map),
	.controls = cht_mc_controls,
	.num_controls = ARRAY_SIZE(cht_mc_controls),
	.suspend_pre = cht_suspend_pre,
	.resume_post = cht_resume_post,
};

#define RT5659_I2C_DEFAULT	"i2c-10EC5659:00"

static int snd_cht_mc_probe(struct platform_device *pdev)
{
	int ret_val = 0;
	struct cht_mc_private *drv;
	struct snd_soc_acpi_mach *mach = pdev->dev.platform_data;
	struct sst_platform_info *pdata;
	const char *platform_name;
	struct acpi_device *adev;
	bool sof_parent;
	int dai_index = 0;
	int i;

    pr_info("rt5659 mach pdev name %s\n", pdev->name);

    pr_info("rt5659 mach id %s\n", mach->id);
    pr_info("rt5659 mach drv_name %s\n", mach->drv_name);
    pr_info("rt5659 mach fw_filename %s\n", mach->fw_filename);

    dev_dbg(&pdev->dev, "rt5659 mach test 0\n");

	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	strcpy(drv->codec_name, RT5659_I2C_DEFAULT);

	/* find index of codec dai */
	for (i = 0; i < ARRAY_SIZE(cht_dailink); i++) {
		if (cht_dailink[i].codecs->name &&
		    !strcmp(cht_dailink[i].codecs->name, RT5659_I2C_DEFAULT)) {
			dai_index = i;
			break;
		}
	}
    if(i == ARRAY_SIZE(cht_dailink)){
        pr_info("rt5659 default codec name %s not found\n", drv->codec_name);
    }
    else{
        pr_info("rt5659 index %d dai codec name %s\n", dai_index, cht_dailink[dai_index].codecs->name);

		/* fixup codec name based on HID */
		adev = acpi_dev_get_first_match_dev(mach->id, NULL, -1);
		if (adev) {
			snprintf(drv->codec_name, sizeof(drv->codec_name),
				 "i2c-%s", acpi_dev_name(adev));
			cht_dailink[dai_index].codecs->name = drv->codec_name;
		}
		acpi_dev_put(adev);

        pr_info("rt5659 acpi_device name %s\n", acpi_dev_name(adev));
        pr_info("rt5659 fixuped index %d dai codec name %s\n", dai_index, cht_dailink[dai_index].codecs->name);

		/* Use SSP0 on Bay Trail CR devices */
		if (soc_intel_is_byt() && mach->mach_params.acpi_ipc_irq_index == 0) {
			cht_dailink[dai_index].cpus->dai_name = "ssp0-port";
			drv->use_ssp0 = true;
		}
    }

	/* override platform name, if required */
	snd_soc_card_cht.dev = &pdev->dev;
	platform_name = mach->mach_params.platform;
    pdata = mach->pdata;
	//platform_name = padata->platform;

    pr_info("rt5659 mach platform_name %s\n", pdata->platform);
    pr_info("rt5659 mach test 1\n");

	ret_val = snd_soc_fixup_dai_links_platform_name(&snd_soc_card_cht,
							platform_name);
	if (ret_val)
		return ret_val;

	//snd_soc_card_cht.components = rt5659_components();

	drv->mclk = devm_clk_get(&pdev->dev, "pmc_plt_clk_3");
	if (IS_ERR(drv->mclk)) {
		dev_err(&pdev->dev,
			"Failed to get MCLK from pmc_plt_clk_3: %ld\n",
			PTR_ERR(drv->mclk));
		return PTR_ERR(drv->mclk);
	}
	snd_soc_card_set_drvdata(&snd_soc_card_cht, drv);

	sof_parent = snd_soc_acpi_sof_parent(&pdev->dev);

    pr_info("rt5659 mach test 2\n");

	/* set card and driver name */
	if (sof_parent) {
		snd_soc_card_cht.name = SOF_CARD_NAME;
		snd_soc_card_cht.driver_name = SOF_DRIVER_NAME;
	} else {
		snd_soc_card_cht.name = CARD_NAME;
		snd_soc_card_cht.driver_name = DRIVER_NAME;
	}

	/* set pm ops */
	if (sof_parent)
		pdev->dev.driver->pm = &snd_soc_pm_ops;

	/* register the soc card */
	ret_val = devm_snd_soc_register_card(&pdev->dev, &snd_soc_card_cht);
	if (ret_val) {
		dev_err(&pdev->dev,
			"snd_soc_register_card failed %d\n", ret_val);
		return ret_val;
	}
	platform_set_drvdata(pdev, &snd_soc_card_cht);

    pr_info("rt5659 mach test 3\n");

	return ret_val;
}

static struct platform_driver snd_cht_mc_driver = {
	.driver = {
		.name = "cht-bsw-rt5659",
	},
	.probe = snd_cht_mc_probe,
};

module_platform_driver(snd_cht_mc_driver);

MODULE_DESCRIPTION("ASoC Intel(R) Baytrail CR Machine driver");
MODULE_AUTHOR("Subhransu S. Prusty, Mengdong Lin");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:cht-bsw-rt5659");
