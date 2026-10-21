# 驱动加载顺序调整
/etc/modules-load.d/modules.conf 新起一行填写
```
snd_soc_rt5659
```
# 声音修复
注意。如果开机后扬声器某个声道没声音，请执行以下命令。无需root，普通用户执行。
```bash
amixer -c0 cset "name='Left Amp Input'" Left
amixer -c0 cset "name='Right Amp Input'" Right
```
/usr/share/alsa/ucm2/conf.d/cht-bsw-rt5659/ 文件夹下
cht-bsw-rt5659.conf
```
Syntax 3

SectionUseCase."HiFi" {
	File "HiFi.conf"
	Comment "Default"
}
```
HiFi.conf
```
SectionVerb {
	If.Controls {
		Condition {
			Type ControlExists
			Control "name='media0_in Gain 0 Switch'"
		}
		Before.EnableSequence "0"
		True {
			Include.pe.File "/platforms/bytcr/PlatformEnableSeq.conf"
			Include.pd.File "/platforms/bytcr/PlatformDisableSeq.conf"
		}
	}

    EnableSequence [
		cset "name='codec_out0 Gain 0 Volume' 72%"
		cset "name='IF2 ADC Mux' DAC_REF"
		cset "name='IF3 ADC Mux' IF_ADC1"
		# 音量50%
		# cset "name='DAC1 Playback Volume' 50%"
		# 功放左右声道归属属于扬声器路径，见 Speaker 段的 EnableSequence
    ]

	DisableSequence [

	]

	Value {
		PlaybackPCM "hw:${CardId}"
		# The speaker ampl. path on the 5659 has no speaker vol control
		# Use the digital DAC1 master control as MixerElem
		# PlaybackMixerElem "DAC1"
		# PlaybackVolume "DAC1 Playback Volume"
		# PlaybackSwitch "DAC1 Playback Switch"
		# PlaybackVolumeScale "0-155:0-100%log"
	}
}

SectionDevice."Speaker" {
	Comment "Stereo Speakers"

	ConflictingDevice []

	Value {
		# The speaker ampl. path on the 5659 has no speaker vol control
		# Use the digital DAC1 master control as MixerElem
		PlaybackMixerElem "DAC1"
		PlaybackVolume "DAC1 Playback Volume"
		PlaybackSwitch "DAC1 Playback Switch"
	}

	EnableSequence [
		cset "name='DAC1 Playback Switch' on"
		# 两颗 TFA9890 共用同一 I2S，各自放大一个声道。
		# 控件名的 Left/Right 前缀由板级 codec_conf 提供。
		cset "name='Left Amp Input' Left"
		cset "name='Right Amp Input' Right"
	]
	DisableSequence [
		cset "name='DAC1 Playback Switch' off"
	]
}

SectionDevice."IntMic" {
	Comment "Microphone"

	ConflictingDevice []

	EnableSequence [
		# 开启 Int Mic
		cset "name='Int Mic Switch' on"
		cset "name='RECMIX1L BST3 Switch' on"
		cset "name='RECMIX1R BST4 Switch' on"
		cset "name='STO1 ADC Capture Switch' on"

		# 开启 Boost（如果需要）
		cset "name='IN3 Boost Volume' 45"
		cset "name='IN4 Boost Volume' 45"

		# 设置 ADC 源 (选择录音混音器的输出作为 ADC 输入)
		cset "name='Stereo1 ADC Source' 'ADC1'"
		cset "name='Stereo1 ADC1 Source' 'ADC'"

		# 设置录音混音器输出到 ADC
		cset "name='Stereo1 ADC MIXL ADC1 Switch' on"
		cset "name='Stereo1 ADC MIXR ADC1 Switch' on"
		cset "name='DAC1 MIXL Stereo ADC Switch' off"
		cset "name='DAC1 MIXR Stereo ADC Switch' off"
		cset "name='media_loop2_out mix 0 codec_in0 Switch' on"
		cset "name='pcm1_out mix 0 media_loop2_in Switch' on"

		# 设置 ADC 音量
		cset "name='IN Capture Volume' 23"
		cset "name='STO1 ADC Capture Volume' 70"
	]

	DisableSequence [
		cset "name='Int Mic Switch' off"
	]

	Value {
		CapturePCM "hw:${CardId}"
		# CaptureMixerElem "Int Mic"
		CaptureVolume "IN Capture Volume"
		CaptureSwitch "Int Mic Switch"
	}
}
```
# 实验性设置方法
复制audio下所有文件，保持目录结构复制到/usr/share/alsa/ucm2  
修改文件所有者为root，组root。权限755

# ~~设置声卡模式~~ 以下内容不需要了，已经修复
## 安装声音调整面板
新发行版使用 `PipeWire` 代替 `PulseAudio`。无需安装
```bash
apt install pavucontrol
```
## 将声卡模式设置为专业音频
配置 -> Built-in Audio -> 专业音频(Pro Audio)  
pipewire版本高于 `1.2.7(大概)` 就有 `专业音频(Pro Audio)`

至此声音设备正常识别
