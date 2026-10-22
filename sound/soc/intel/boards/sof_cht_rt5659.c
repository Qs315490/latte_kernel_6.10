// SPDX-License-Identifier: GPL-2.0-only
//
// SOF machine driver for the Xiaomi Mi Pad 2 (Cherry Trail, "latte").
//
// SSP2 carries the RT5659 codec (headphones/speaker), SSP1 carries the
// BCM4356 BT/FM PCM bus.  The FM capture stream is exposed as a separate
// host PCM by the topology (sof-cht-rt5659.tplg).
//
// Copyright (c) 2026 Xcfcdl <746953609@qq.com>

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-acpi.h>
#include "../../codecs/rt5659.h"
#include "../atom/sst-atom-controls.h"

#define CODEC_DAI_NAME	"rt5659-aif1"
#define CODEC_ACPI_HID	"10EC5659"

#define SOF_CARD_NAME	"sof-cht rt5659"
#define SOF_DRIVER_NAME	"SOF"
#define CARD_NAME	"sof-cht-rt5659"

#define CHT_MCLK_19M2	19200000

static const struct snd_soc_dapm_widget sof_cht_rt5659_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Int Mic", NULL),
	SND_SOC_DAPM_SPK("Ext Spk", NULL),
};

static const struct snd_soc_dapm_route sof_cht_rt5659_map[] = {
	{"IN1P", NULL, "Headset Mic"},
	{"IN1N", NULL, "Headset Mic"},
	{"IN3P", NULL, "Int Mic"},
	{"IN3N", NULL, "Int Mic"},
	{"Headphone", NULL, "HPOL"},
	{"Headphone", NULL, "HPOR"},
	{"Ext Spk", NULL, "SPOL"},
	{"Ext Spk", NULL, "SPOR"},

	{"AIF1 Playback", NULL, "ssp2 Tx"},
	{"ssp2 Tx", NULL, "codec_out0"},
	{"ssp2 Tx", NULL, "codec_out1"},
	{"codec_in0", NULL, "ssp2 Rx"},
	{"codec_in1", NULL, "ssp2 Rx"},
	{"ssp2 Rx", NULL, "AIF1 Capture"},
};

static const struct snd_kcontrol_new sof_cht_rt5659_controls[] = {
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
	SOC_DAPM_PIN_SWITCH("Int Mic"),
	SOC_DAPM_PIN_SWITCH("Ext Spk"),
};

/*
 * Fixed 48 kHz setup: PLL1 from the 19.2 MHz MCLK, 512*fs output.
 *
 * rt5659 caches the last PLL/sysclk parameters and skips the register
 * writes when they did not change, so after an idle bias-off (or a codec
 * reset) the cached values can no longer match the hardware and the codec
 * stays silent.  Walk each setting through a different value first so the
 * final write is unconditional.
 */
static int sof_cht_rt5659_set_clock(struct snd_soc_dai *codec_dai)
{
	unsigned int fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBC_CFC;
	int ret;

	snd_soc_dai_set_pll(codec_dai, 0, RT5659_PLL1_S_MCLK,
			    CHT_MCLK_19M2, 44100 * 512);
	ret = snd_soc_dai_set_pll(codec_dai, 0, RT5659_PLL1_S_MCLK,
				  CHT_MCLK_19M2, 48000 * 512);
	if (ret) {
		dev_err(codec_dai->dev, "failed to set codec pll: %d\n", ret);
		return ret;
	}

	snd_soc_dai_set_sysclk(codec_dai, RT5659_SCLK_S_PLL1,
			       44100 * 512, SND_SOC_CLOCK_IN);
	ret = snd_soc_dai_set_sysclk(codec_dai, RT5659_SCLK_S_PLL1,
				     48000 * 512, SND_SOC_CLOCK_IN);
	if (ret) {
		dev_err(codec_dai->dev, "failed to set codec sysclk: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_fmt(codec_dai, fmt);
	if (ret)
		dev_err(codec_dai->dev, "failed to set codec fmt: %d\n", ret);

	return ret;
}

/* Headset jack detection pins (JD3 on the Mi Pad 2). */
static struct snd_soc_jack sof_cht_headset;
static struct snd_soc_jack_pin sof_cht_headset_pins[] = {
	{
		.pin = "Headset Mic",
		.mask = SND_JACK_MICROPHONE,
	},
	{
		.pin = "Headphone",
		.mask = SND_JACK_HEADPHONE,
	},
};

static int sof_cht_rt5659_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_component *component = codec_dai->component;
	int ret;

	ret = sof_cht_rt5659_set_clock(codec_dai);
	if (ret)
		return ret;

	ret = snd_soc_card_jack_new_pins(rtd->card, "Headset",
					 SND_JACK_HEADSET | SND_JACK_BTN_0 |
					 SND_JACK_BTN_1 | SND_JACK_BTN_2,
					 &sof_cht_headset,
					 sof_cht_headset_pins,
					 ARRAY_SIZE(sof_cht_headset_pins));
	if (ret)
		return ret;

	snd_jack_set_key(sof_cht_headset.jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
	snd_jack_set_key(sof_cht_headset.jack, SND_JACK_BTN_1, KEY_VOLUMEUP);
	snd_jack_set_key(sof_cht_headset.jack, SND_JACK_BTN_2, KEY_VOLUMEDOWN);

	return rt5659_set_jack_detect(component, &sof_cht_headset);
}

/*
 * Runs on every BE hw_params, before the codec DAI's own hw_params() gets
 * to compute its dividers from the cached sysclk.
 */
static int sof_cht_rt5659_be_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);

	return sof_cht_rt5659_set_clock(snd_soc_rtd_to_codec(rtd, 0));
}

static int sof_cht_rt5659_fixup(struct snd_soc_pcm_runtime *rtd,
				struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
						      SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
							  SNDRV_PCM_HW_PARAM_CHANNELS);

	/* The DSP converts to 48 kHz stereo 16 bit at the DAI */
	rate->min = 48000;
	rate->max = 48000;
	channels->min = 2;
	channels->max = 2;
	params_set_format(params, SNDRV_PCM_FORMAT_S16_LE);

	return 0;
}

static int sof_cht_rt5659_aif1_startup(struct snd_pcm_substream *substream)
{
	return snd_pcm_hw_constraint_single(substream->runtime,
					    SNDRV_PCM_HW_PARAM_RATE, 48000);
}

static const struct snd_soc_ops sof_cht_rt5659_aif1_ops = {
	.startup = sof_cht_rt5659_aif1_startup,
};

static const struct snd_soc_ops sof_cht_rt5659_be_ops = {
	.hw_params = sof_cht_rt5659_be_hw_params,
};

SND_SOC_DAILINK_DEF(dummy,
		    DAILINK_COMP_ARRAY(COMP_DUMMY()));

SND_SOC_DAILINK_DEF(media,
		    DAILINK_COMP_ARRAY(COMP_CPU("media-cpu-dai")));

SND_SOC_DAILINK_DEF(deepbuffer,
		    DAILINK_COMP_ARRAY(COMP_CPU("deepbuffer-cpu-dai")));

SND_SOC_DAILINK_DEF(ssp2,
		    DAILINK_COMP_ARRAY(COMP_CPU("ssp2-port")));

SND_SOC_DAILINK_DEF(ssp1,
		    DAILINK_COMP_ARRAY(COMP_CPU("ssp1-port")));

SND_SOC_DAILINK_DEF(rt5659,
		    DAILINK_COMP_ARRAY(COMP_CODEC("i2c-10EC5659:00", CODEC_DAI_NAME)));

SND_SOC_DAILINK_DEF(platform,
		    DAILINK_COMP_ARRAY(COMP_PLATFORM("sst-mfld-platform")));

SND_SOC_DAILINK_DEF(rt5659_aif2_cpu,
		    DAILINK_COMP_ARRAY(COMP_CPU("rt5659-aif2")));

SND_SOC_DAILINK_DEF(spk_l_codec,
		    DAILINK_COMP_ARRAY(COMP_CODEC("i2c-tfa9890:00", "tfa989x-hifi")));

SND_SOC_DAILINK_DEF(spk_r_codec,
		    DAILINK_COMP_ARRAY(COMP_CODEC("i2c-tfa9890:01", "tfa989x-hifi")));

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

static struct snd_soc_dai_link sof_cht_rt5659_dais[] = {
	[MERR_DPCM_AUDIO] = {
		.name = "Audio Port",
		.stream_name = "Audio",
		.nonatomic = true,
		.dynamic = 1,
		.ops = &sof_cht_rt5659_aif1_ops,
		SND_SOC_DAILINK_REG(media, dummy, platform),
	},
	[MERR_DPCM_DEEP_BUFFER] = {
		.name = "Deep-Buffer Audio Port",
		.stream_name = "Deep-Buffer Audio",
		.nonatomic = true,
		.dynamic = 1,
		.playback_only = 1,
		.ops = &sof_cht_rt5659_aif1_ops,
		SND_SOC_DAILINK_REG(deepbuffer, dummy, platform),
	},
	/* back ends */
	{
		.name = "SSP2-Codec",
		.id = 0,
		.no_pcm = 1,
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBC_CFC,
		.ops = &sof_cht_rt5659_be_ops,
		.init = sof_cht_rt5659_init,
		.be_hw_params_fixup = sof_cht_rt5659_fixup,
		SND_SOC_DAILINK_REG(ssp2, rt5659, platform),
	},
	{
		.name = "rt5659_AIF2-TFA989x_Speaker_L",
		.stream_name = "aif2-spk_l",
		SND_SOC_DAILINK_REG(rt5659_aif2_cpu, spk_l_codec),
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBS_CFS,
		.c2c_params = nxp_tfa989x_params,
		.num_c2c_params = 1,
	},
	{
		.name = "rt5659_AIF2-TFA989x_Speaker_R",
		.stream_name = "aif2-spk_r",
		SND_SOC_DAILINK_REG(rt5659_aif2_cpu, spk_r_codec),
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBS_CFS,
		.c2c_params = nxp_tfa989x_params,
		.num_c2c_params = 1,
	},
	{
		/* BCM4356 FM/BT PCM capture, routed by the topology */
		.name = "SSP1-BTFM",
		.id = 1,
		.no_pcm = 1,
		.capture_only = 1,
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			   SND_SOC_DAIFMT_CBC_CFC,
		SND_SOC_DAILINK_REG(ssp1, dummy, platform),
	},
};

static struct snd_soc_card sof_cht_rt5659_card = {
	.name = CARD_NAME,
	.owner = THIS_MODULE,
	.dai_link = sof_cht_rt5659_dais,
	.num_links = ARRAY_SIZE(sof_cht_rt5659_dais),
	.dapm_widgets = sof_cht_rt5659_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sof_cht_rt5659_widgets),
	.dapm_routes = sof_cht_rt5659_map,
	.num_dapm_routes = ARRAY_SIZE(sof_cht_rt5659_map),
	.controls = sof_cht_rt5659_controls,
	.num_controls = ARRAY_SIZE(sof_cht_rt5659_controls),
	.dapm.idle_bias_off = true,
};

static char codec_name[SND_ACPI_I2C_ID_LEN];

static int sof_cht_rt5659_probe(struct platform_device *pdev)
{
	struct snd_soc_acpi_mach *mach;
	struct acpi_device *adev;
	bool sof_parent;
	int dai_index = -1;
	int i;

	sof_cht_rt5659_card.dev = &pdev->dev;
	mach = dev_get_platdata(&pdev->dev);

	for (i = 0; i < ARRAY_SIZE(sof_cht_rt5659_dais); i++) {
		if (sof_cht_rt5659_dais[i].num_codecs &&
		    !strcmp(sof_cht_rt5659_dais[i].codecs->dai_name,
			    CODEC_DAI_NAME)) {
			dai_index = i;
			break;
		}
	}
	if (dai_index < 0) {
		dev_err(&pdev->dev, "codec DAI link not found\n");
		return -EINVAL;
	}

	adev = acpi_dev_get_first_match_dev(mach->id, NULL, -1);
	if (!adev) {
		dev_err(&pdev->dev, "cannot find '%s' ACPI device\n", mach->id);
		return -ENOENT;
	}
	snprintf(codec_name, sizeof(codec_name), "i2c-%s", acpi_dev_name(adev));
	sof_cht_rt5659_dais[dai_index].codecs->name = codec_name;
	acpi_dev_put(adev);

	if (mach->mach_params.platform) {
		for (i = 0; i < ARRAY_SIZE(sof_cht_rt5659_dais); i++) {
			struct snd_soc_dai_link *link = &sof_cht_rt5659_dais[i];
			const char *name;

			if (link->num_platforms != 1)
				continue;
			name = devm_kstrdup(&pdev->dev,
					    mach->mach_params.platform,
					    GFP_KERNEL);
			if (!name)
				return -ENOMEM;
			link->platforms->name = name;
		}
	}

	sof_parent = snd_soc_acpi_sof_parent(&pdev->dev);
	if (sof_parent) {
		sof_cht_rt5659_card.name = SOF_CARD_NAME;
		sof_cht_rt5659_card.driver_name = SOF_DRIVER_NAME;
		pdev->dev.driver->pm = &snd_soc_pm_ops;
	}

	return devm_snd_soc_register_card(&pdev->dev, &sof_cht_rt5659_card);
}

static struct platform_driver sof_cht_rt5659_driver = {
	.driver = {
		.name = "sof_cht_rt5659",
	},
	.probe = sof_cht_rt5659_probe,
};
module_platform_driver(sof_cht_rt5659_driver);

MODULE_AUTHOR("Xcfcdl <746953609@qq.com>");
MODULE_DESCRIPTION("ASoC SOF machine driver for Mi Pad 2 (Cherry Trail + RT5659)");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:sof_cht_rt5659");
