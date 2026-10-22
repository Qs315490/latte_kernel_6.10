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
	struct notifier_block jack_nb;
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
	struct snd_soc_card *card = snd_soc_dapm_to_card(w->dapm);
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

/*
 * 耳机插入时让扬声器静音。
 *
 * 板上扬声器由两颗 TFA9890 驱动，音频由 rt5659 的 AIF2 引脚送出。功放自身的
 * 通断由它的 DAPM 链（OUT -> AMPE -> POWER）管理；直接写 SYS_CTRL 会和 DAPM
 * 抢同一批寄存器位，播放时被覆盖，所以这里改成切断 OUT widget 的连通性，
 * 让 DAPM 自己把功放断电 / 恢复。
 *
 * widget 名带 codec_conf 里配置的 name_prefix（Left/Right）。
 */
static int cht_spk_power_apply(struct snd_soc_card *card, bool on)
{
	struct snd_soc_component *component;

	for_each_card_components(card, component) {
		int ret;

		if (strncmp(component->name, "i2c-tfa9890", 11))
			continue;

		/*
		 * 传【不带前缀】的 widget 名：dapm_find_widget() 会用
		 * component->name_prefix 自己拼成 "Left OUT" 再去查表。
		 * 这里若自己加前缀，会被加成 "Left Left OUT" 而查不到。
		 */
		if (on)
			ret = snd_soc_dapm_enable_pin(component->dapm, "OUT");
		else
			ret = snd_soc_dapm_disable_pin(component->dapm, "OUT");
		if (ret < 0) {
			dev_err(card->dev, "failed to %s OUT on %s: %d\n",
				on ? "enable" : "disable", component->name, ret);
			continue;
		}

		snd_soc_dapm_sync(component->dapm);

		dev_info(card->dev, "TFA9890 %s: output %s\n", component->name,
			 on ? "enabled" : "disabled");
	}

	return 0;
}

static int cht_jack_notify(struct notifier_block *nb, unsigned long status,
			   void *data)
{
	struct snd_soc_jack *jack = data;
	struct snd_soc_card *card = jack->card;
	bool hp = status & SND_JACK_HEADPHONE;

	/* 插着耳机就把扬声器功放断电，拔出再恢复 */
	cht_spk_power_apply(card, !hp);

	return NOTIFY_OK;
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
	/*
	 * 功放的 DAI widget 接到 rt5659 的 AIF2 上：扬声器的数据由 rt5659 内部
	 * 回环（aif1 -> aif2）后经硬件 I2S 引脚送出。功放自身不再承载 PCM
	 * （见 Dummy-TFA989x Speaker L/R 的 no_pcm），因此它的电源不再被
	 * 播放流绑死，插孔事件可以自由控制。
	 * 名字里的 Left/Right 来自 codec_conf 的 name_prefix。
	 */
	{"Left HiFi Playback", NULL, "AIF2 Playback"},
	{"Right HiFi Playback", NULL, "AIF2 Playback"},
	/*
	 * Platform Clock 是 DAPM supply，负责开关编解码器 MCLK（pmc_plt_clk_3）。
	 * 它必须挂在真实存在的 widget 上：原来挂到 "AIF1 Playback"/"AIF1 Capture"，
	 * 但 SST 平台的 DAI widget 名取自 stream_name（FE 是 "Headset Playback"），
	 * 这两个 widget 并不存在，snd_soc_dapm_add_route() 返回 -ENODEV，
	 * 该 supply 就永远不会上电。这里与 rt5672 / bytcr_rt5640 保持一致，挂在引脚上。
	 */
	{"Headphone", NULL, "Platform Clock"},
	{"Headset Mic", NULL, "Platform Clock"},
	{"Int Mic", NULL, "Platform Clock"},
	{"Ext Spk", NULL, "Platform Clock"},
};

static const struct snd_soc_dapm_route cht_audio_ssp0_map[] = {
	{"ssp0 Tx", NULL, "modem_out"},
	{"modem_in", NULL, "ssp0 Rx"},
};

/*
 * FE↔BE 的桥接路由。老内核里 FE DAI 的 stream_name 叫 AIF1 Playback/Capture，
 * 主线 SST 平台改名后这两个 widget 已不存在，对应路由加不上（-ENODEV）。
 * FE→BE 的实际通路由平台 DAPM 图提供：
 *   Headset Playback → media1_in → media0_out → pcm0_in → codec_out0 → ssp2 Tx
 *   ssp2 Rx → codec_in0 → pcm1_out → Headset Capture
 * 这里只补板级侧的 codec_inx ↔ ssp2 连线。
 */
static const struct snd_soc_dapm_route cht_audio_ssp2_map[] = {
	{"ssp2 Tx", NULL, "codec_out0"},
	{"ssp2 Tx", NULL, "codec_out1"},
	{"codec_in0", NULL, "ssp2 Rx"},
	{"codec_in1", NULL, "ssp2 Rx"},
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
		| SND_SOC_DAIFMT_CBC_CFC;

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
 * 以下为官方（Android）版基于 snd_soc_jack_gpio + delayed work 的插孔检测参考实现，
 * 依赖 wake_lock、ctx->jack/intr_debounce 等 Android 专有设施，主线无法编译，
 * 仅作参考保留。主线走 IRQ + snd_soc_card_jack_new_pins 的实现，见下方 cht_codec_init。
 */
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

/*
 * 对应官方 cht_codec_init 的职责：注册 DAPM 路由、建立插孔检测、初始化 MCLK。
 * 与 rt5672 版的差别是去掉了 ASRC 时钟源选择——rt5659 没有 sel_asrc_clk_src()，
 * 官方 rt5659 板级驱动同样不做这一步。
 */
static int cht_codec_init(struct snd_soc_pcm_runtime *runtime)
{
	int ret;
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(runtime, 0);
	struct snd_soc_component *component = codec_dai->component;
	struct snd_soc_card *card = runtime->card;
	struct cht_mc_private *ctx = snd_soc_card_get_drvdata(card);

	if (devm_acpi_dev_add_driver_gpios(component->dev, cht_rt5659_gpios))
		dev_warn(runtime->dev, "Unable to add GPIO mapping table\n");

	/*
	 * FE↔BE 的 DAPM 桥接按 SSP 选择在此加入。返回值必须检查：
	 * 加失败会静默断开通路，最终只表现为 "no backend DAIs enabled"。
	 */
	if (ctx->use_ssp0)
		ret = snd_soc_dapm_add_routes(card->dapm, cht_audio_ssp0_map,
					      ARRAY_SIZE(cht_audio_ssp0_map));
	else
		ret = snd_soc_dapm_add_routes(card->dapm, cht_audio_ssp2_map,
					      ARRAY_SIZE(cht_audio_ssp2_map));
	if (ret) {
		dev_err(runtime->dev, "failed to add SSP DAPM routes: %d\n", ret);
		return ret;
	}

	/* 7.2：idle_bias_off 字段已移除，改用 snd_soc_dapm_set_idle_bias()（极性相反） */
	snd_soc_dapm_set_idle_bias(card->dapm, false);
	snd_soc_dapm_init_bias_level(card->dapm, SND_SOC_BIAS_OFF);

	ret = snd_soc_card_jack_new_pins(card, "Headset",
					 SND_JACK_HEADSET | SND_JACK_BTN_0 |
					 SND_JACK_BTN_1 | SND_JACK_BTN_2,
					 &ctx->headset,
					 cht_bsw_headset_pins,
					 ARRAY_SIZE(cht_bsw_headset_pins));
	if (ret)
		return ret;

	/*
	 * 挂上插孔通知：耳机插入时给扬声器功放掉电（见 cht_jack_notify）。
	 * 放在 set_jack_detect 之前，避免漏掉首次状态上报。
	 */
	ctx->jack_nb.notifier_call = cht_jack_notify;
	snd_soc_jack_notifier_register(&ctx->headset, &ctx->jack_nb);

	snd_jack_set_key(ctx->headset.jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
	snd_jack_set_key(ctx->headset.jack, SND_JACK_BTN_1, KEY_VOLUMEUP);
	snd_jack_set_key(ctx->headset.jack, SND_JACK_BTN_2, KEY_VOLUMEDOWN);

	/* codec 侧注册 jack，插孔状态变化由 rt5659 的中断驱动 */
	rt5659_set_jack_detect(component, &ctx->headset);

	if (ctx->mclk) {
		/*
		 * The firmware might enable the clock at boot (this information
		 * may or may not be reflected in the enable clock register).
		 * To change the rate we must disable the clock first to cover
		 * these cases. Due to common clock framework restrictions that do
		 * not allow to disable a clock that has not been enabled, we
		 * need to enable the clock first.
		 */
		ret = clk_prepare_enable(ctx->mclk);
		if (!ret)
			clk_disable_unprepare(ctx->mclk);

		ret = clk_set_rate(ctx->mclk, CHT_PLAT_CLK_3_HZ);
		if (ret) {
			dev_err(runtime->dev, "unable to set MCLK rate\n");
			return ret;
		}
	}

	/*
	 * 预置 PLL 与 sysclk。c2c 链路（Codec AIF2 Port）的 hw_params 由 DAPM
	 * 在 PRE_PMU 里直接对 DAI 调用，可能早于 SSP2-Codec 的 hw_params；那时
	 * sysclk 还是 0，rt5659_hw_params() 会以 "Unsupported clock setting" 失败。
	 */
	if (codec_dai) {
		int clk_ret = snd_soc_dai_set_pll(codec_dai, 0, RT5659_PLL1_S_MCLK,
						  CHT_PLAT_CLK_3_HZ, 48000 * 512);

		if (clk_ret)
			dev_warn(runtime->dev, "cannot preset codec pll: %d\n", clk_ret);
		else
			snd_soc_dai_set_sysclk(codec_dai, RT5659_SCLK_S_PLL1,
					      48000 * 512, SND_SOC_CLOCK_IN);
	}

	/* Keep the voice call paths active during suspend. */
	snd_soc_dapm_ignore_suspend(component->dapm, "HPOL");
	snd_soc_dapm_ignore_suspend(component->dapm, "HPOR");
	snd_soc_dapm_ignore_suspend(component->dapm, "SPOL");
	snd_soc_dapm_ignore_suspend(component->dapm, "SPOR");

	snd_soc_dapm_enable_pin(card->dapm, "Headset Mic");
	snd_soc_dapm_enable_pin(card->dapm, "Headphone");
	snd_soc_dapm_enable_pin(card->dapm, "Ext Spk");
	snd_soc_dapm_enable_pin(card->dapm, "Int Mic");

	snd_soc_dapm_sync(card->dapm);
	return 0;
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

/*
 * 这颗 codec 的 i2c client 名。板子的 ACPI 只暴露 10EC5659:01，
 * 直接写死即可 —— 原来的运行时 fixup（acpi_dev_get_first_match_dev）
 * 只会改一个 dailink，在多处引用同一 codec 时反而把名字改错，
 * 导致找不到 codec DAI 而 EPROBE_DEFER。
 */
#define RT5659_I2C_NAME	"i2c-10EC5659:01"
SND_SOC_DAILINK_DEF(ssp2_port,
	DAILINK_COMP_ARRAY(COMP_CPU("ssp2-port")));
SND_SOC_DAILINK_DEF(ssp2_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC(RT5659_I2C_NAME,
					  "rt5659-aif1")));

SND_SOC_DAILINK_DEF(platform,
	DAILINK_COMP_ARRAY(COMP_PLATFORM("sst-mfld-platform")));

/*
 * 三段式布线（与厂商 / Android 侧一致）：
 *
 *   rt5659 aif1 --c2c--> rt5659 aif2   数据在 codec 内部回环，
 *                                      最终由硬件 I2S 引脚送到功放
 *   Dummy-BE(no_pcm) 挂功放          让功放留在 DAPM 图里，但不绑定任何 PCM
 *
 * 这样做的好处是功放的电源不再被“有播放流”绑死：
 * 耳机模式下可以让 rt5659 不再往 AIF2 送数据，功放自然静音。
 */
SND_SOC_DAILINK_DEF(rt5659_aif1_cpu,
	DAILINK_COMP_ARRAY(COMP_CPU("rt5659-aif1")));
SND_SOC_DAILINK_DEF(rt5659_aif2_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC(RT5659_I2C_NAME,
					  "rt5659-aif2")));

SND_SOC_DAILINK_DEF(spk_l_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("i2c-tfa9890:00",
					  "tfa989x-hifi")));

SND_SOC_DAILINK_DEF(spk_r_codec,
	DAILINK_COMP_ARRAY(COMP_CODEC("i2c-tfa9890:01",
					  "tfa989x-hifi")));

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
		.ops = &cht_aif1_ops,
		SND_SOC_DAILINK_REG(media, dummy, platform),
	},
	[MERR_DPCM_DEEP_BUFFER] = {
		.name = "Deep-Buffer Audio Port",
		.stream_name = "Deep-Buffer Audio",
		.nonatomic = true,
		.dynamic = 1,
		.playback_only = 1,
		.ops = &cht_aif1_ops,
		SND_SOC_DAILINK_REG(deepbuffer, dummy, platform),
	},

	/* Back End DAI links */
	{
		/* SSP2 - Codec */
		.name = "SSP2-Codec",
		.id = 0,
		.no_pcm = 1,
		.init = cht_codec_init,
		.be_hw_params_fixup = cht_codec_fixup,
		.ops = &cht_be_ssp2_ops,
		SND_SOC_DAILINK_REG(ssp2_port, ssp2_codec, platform),
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
	},
	{
		/*
		 * rt5659 内部 aif1 -> aif2 回环：数据由 codec 自己从 aif1 收到、
		 * 从 aif2 输出，再经硬件 I2S 引脚送到 TFA9890。
		 *
		 * 与厂商驱动一致：不用 c2c_params，这样它是普通 PCM 链路，
		 * dailink 的 .ops 才会被调用（c2c 的 hw_params 走 DAPM，绕过 ops）。
		 * 时钟方向 CBM_CFM：由 codec 自己产生 BCLK/LRCK。
		 */
		.name = "Codec AIF2 Port",
		.stream_name = "rt5659 AIF2 Playback",
		SND_SOC_DAILINK_REG(rt5659_aif1_cpu, rt5659_aif2_codec),
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBP_CFP,
		/*
		 * c2c_params 是必需的：snd_soc_dai_link_event_pre_pmu() 会
		 * 用它构造 hw_params，缺了直接返回 -EINVAL，AIF2 回环不会启动，
		 * 功放就收不到数据（播放无报错但无声）。
		 */
		.c2c_params = nxp_tfa989x_params,
		.num_c2c_params = 1,
	}, {
		/*
		 * 功放本身不承载 PCM（no_pcm）：只为让它留在 DAPM 图里，
		 * 同时避免 POWER widget 被播放流长时间点亮——耳机模式下
		 * 功放电源因此可以由插孔事件自由控制。
		 */
		.name = "Dummy-TFA989x Speaker L",
		.no_pcm = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(dummy, spk_l_codec),
	}, {
		.name = "Dummy-TFA989x Speaker R",
		.no_pcm = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(dummy, spk_r_codec),
	},
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

/*
 * 两颗 TFA9890 共用同一份 codec component driver，实例间的 widget/kcontrol
 * 重名靠这里的 name_prefix 消解。左右归属只在此处声明，codec 驱动不参与。
 */
static struct snd_soc_codec_conf cht_tfa_codec_conf[] = {
	{ .dlc.name = "i2c-tfa9890:00", .name_prefix = "Left" },
	{ .dlc.name = "i2c-tfa9890:01", .name_prefix = "Right" },
};

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
	.codec_conf = cht_tfa_codec_conf,
	.num_configs = ARRAY_SIZE(cht_tfa_codec_conf),
	.suspend_pre = cht_suspend_pre,
	.resume_post = cht_resume_post,
};


static int snd_cht_mc_probe(struct platform_device *pdev)
{
	int ret_val = 0;
	struct cht_mc_private *drv;
	struct snd_soc_acpi_mach *mach = pdev->dev.platform_data;
	struct sst_platform_info *pdata;
	const char *platform_name;
	bool sof_parent;
	int dai_index = 0;
	int i;

	pr_info("cht-bsw-rt5659 mach pdev name %s\n", pdev->name);

	pr_info("cht-bsw-rt5659 mach id %s\n", mach->id);
	pr_info("cht-bsw-rt5659 mach drv_name %s\n", mach->drv_name);
	pr_info("cht-bsw-rt5659 mach fw_filename %s\n", mach->fw_filename);

	dev_dbg(&pdev->dev, "cht-bsw-rt5659 mach test 0\n");

	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_KERNEL);
	if (!drv)
		return -ENOMEM;

	strcpy(drv->codec_name, RT5659_I2C_NAME);

	/* find index of codec dai */
	for (i = 0; i < ARRAY_SIZE(cht_dailink); i++) {
		if (cht_dailink[i].codecs->name &&
			!strcmp(cht_dailink[i].codecs->name, RT5659_I2C_NAME)) {
			dai_index = i;
			break;
		}
	}
	if(i == ARRAY_SIZE(cht_dailink)){
		pr_info("cht-bsw-rt5659 default codec name %s not found\n", drv->codec_name);
	}
	else{
		pr_info("cht-bsw-rt5659 index %d dai codec name %s\n", dai_index, cht_dailink[dai_index].codecs->name);

		/*
		 * codec 名已在 RT5659_I2C_NAME 里写死，不再做运行时 ACPI 探测：
		 * 那套逻辑只改第一个匹配的 dailink，而我们有两处引用同一 codec
		 * （SSP2-Codec 与 Codec AIF2 Port），会被改成不存在的实例名。
		 */

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

	pr_info("cht-bsw-rt5659 mach platform_name %s\n", pdata->platform);
	pr_info("cht-bsw-rt5659 mach test 1\n");

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

	pr_info("cht-bsw-rt5659 mach test 2\n");

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

	pr_info("cht-bsw-rt5659 mach test 3\n");

	return ret_val;
}

static struct platform_driver snd_cht_mc_driver = {
	.driver = {
		.name = "cht-bsw-rt5659",
	},
	.probe = snd_cht_mc_probe,
};

static int __init snd_cht_mc_init(void)
{
	return platform_driver_register(&snd_cht_mc_driver);
}
late_initcall(snd_cht_mc_init);

static void __exit snd_cht_mc_exit(void)
{
	platform_driver_unregister(&snd_cht_mc_driver);
}
module_exit(snd_cht_mc_exit);

// module_platform_driver(snd_cht_mc_driver);

MODULE_DESCRIPTION("ASoC Intel(R) Baytrail CR Machine driver");
MODULE_AUTHOR("Subhransu S. Prusty, Mengdong Lin");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:cht-bsw-rt5659");
