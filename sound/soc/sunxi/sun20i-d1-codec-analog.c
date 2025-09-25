// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Allwinner sun20i-d1 SoC Analog Codec Driver
 *
 * Copyright (C) 2023 Maksim Kiselev <bigunclemax@gmail.com>
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>

#define SUN20I_D1_CODEC_ANALOG_EN_DAC		0x00
#define SUN20I_D1_CODEC_ANALOG_EN_ADC		0x04
#define SUN20I_D1_CODEC_ANALOG_DAC_VOL		0x08
#define SUN20I_D1_CODEC_ANALOG_ADC_VOL		0x0c
#define SUN20I_D1_CODEC_ANALOG_MIC1_VOL		0x10
#define SUN20I_D1_CODEC_ANALOG_MIC2_VOL		0x14
#define SUN20I_D1_CODEC_ANALOG_LINEIN_VOL	0x18
#define SUN20I_D1_CODEC_ANALOG_MIXER_VOL	0x1c
#define SUN20I_D1_CODEC_ANALOG_OUT_MIXER	0x20
#define SUN20I_D1_CODEC_ANALOG_IN_MIXER		0x24
#define SUN20I_D1_CODEC_ANALOG_MIC1_MIXER	0x28
#define SUN20I_D1_CODEC_ANALOG_MIC2_MIXER	0x2c
#define SUN20I_D1_CODEC_ANALOG_LINEIN_MIXER	0x30
#define SUN20I_D1_CODEC_ANALOG_REC_MIXER	0x34
#define SUN20I_D1_CODEC_ANALOG_HEADPHONE_VOL	0x38
#define SUN20I_D1_CODEC_ANALOG_SPK_VOL		0x3c
#define SUN20I_D1_CODEC_ANALOG_MIC1_PA		0x40
#define SUN20I_D1_CODEC_ANALOG_MIC2_PA		0x44
#define SUN20I_D1_CODEC_ANALOG_LINEIN_PA	0x48
#define SUN20I_D1_CODEC_ANALOG_HP_PA		0x4c
#define SUN20I_D1_CODEC_ANALOG_SPK_PA		0x50
#define SUN20I_D1_CODEC_ANALOG_PA_EN		0x54

struct sun20i_d1_codec_analog {
	struct device *dev;
	struct regmap *regmap;
	struct clk *clk;
};

static const DECLARE_TLV_DB_SCALE(dac_vol_tlv, -11925, 75, 0);
static const DECLARE_TLV_DB_SCALE(adc_vol_tlv, -11925, 75, 0);
static const DECLARE_TLV_DB_SCALE(mic_vol_tlv, -450, 150, 0);
static const DECLARE_TLV_DB_SCALE(linein_vol_tlv, -450, 150, 0);
static const DECLARE_TLV_DB_SCALE(mixer_vol_tlv, -450, 150, 0);
static const DECLARE_TLV_DB_SCALE(hp_vol_tlv, -6300, 100, 1);
static const DECLARE_TLV_DB_SCALE(spk_vol_tlv, -4800, 150, 0);

static const struct snd_kcontrol_new sun20i_d1_codec_analog_controls[] = {
	SOC_SINGLE_TLV("DAC Playback Volume", SUN20I_D1_CODEC_ANALOG_DAC_VOL,
		       0, 255, 0, dac_vol_tlv),
	SOC_SINGLE_TLV("ADC Capture Volume", SUN20I_D1_CODEC_ANALOG_ADC_VOL,
		       0, 255, 0, adc_vol_tlv),
	SOC_SINGLE_TLV("MIC1 Capture Volume", SUN20I_D1_CODEC_ANALOG_MIC1_VOL,
		       0, 31, 0, mic_vol_tlv),
	SOC_SINGLE_TLV("MIC2 Capture Volume", SUN20I_D1_CODEC_ANALOG_MIC2_VOL,
		       0, 31, 0, mic_vol_tlv),
	SOC_SINGLE_TLV("LINEIN Capture Volume", SUN20I_D1_CODEC_ANALOG_LINEIN_VOL,
		       0, 31, 0, linein_vol_tlv),
	SOC_SINGLE_TLV("MIXER Capture Volume", SUN20I_D1_CODEC_ANALOG_MIXER_VOL,
		       0, 31, 0, mixer_vol_tlv),
	SOC_SINGLE_TLV("Headphone Playback Volume", SUN20I_D1_CODEC_ANALOG_HEADPHONE_VOL,
		       0, 63, 0, hp_vol_tlv),
	SOC_SINGLE_TLV("Speaker Playback Volume", SUN20I_D1_CODEC_ANALOG_SPK_VOL,
		       0, 31, 0, spk_vol_tlv),
};

static const char * const out_mixer_texts[] = {
	"Stereo DAC", "MIC1", "MIC2", "LINEIN", "MIXER"
};

static const struct soc_enum out_mixer_enum =
	SOC_ENUM_SINGLE(SUN20I_D1_CODEC_ANALOG_OUT_MIXER, 0, 5, out_mixer_texts);

static const struct snd_kcontrol_new sun20i_d1_out_mixer_controls =
	SOC_DAPM_ENUM("Route", out_mixer_enum);

static const char * const in_mixer_texts[] = {
	"Stereo DAC", "MIC1", "MIC2", "LINEIN"
};

static const struct soc_enum in_mixer_enum =
	SOC_ENUM_SINGLE(SUN20I_D1_CODEC_ANALOG_IN_MIXER, 0, 4, in_mixer_texts);

static const struct snd_kcontrol_new sun20i_d1_in_mixer_controls =
	SOC_DAPM_ENUM("Route", in_mixer_enum);

static const char * const mic1_mixer_texts[] = {
	"MIC1", "MIXER"
};

static const struct soc_enum mic1_mixer_enum =
	SOC_ENUM_SINGLE(SUN20I_D1_CODEC_ANALOG_MIC1_MIXER, 0, 2, mic1_mixer_texts);

static const struct snd_kcontrol_new sun20i_d1_mic1_mixer_controls =
	SOC_DAPM_ENUM("Route", mic1_mixer_enum);

static const char * const mic2_mixer_texts[] = {
	"MIC2", "MIXER"
};

static const struct soc_enum mic2_mixer_enum =
	SOC_ENUM_SINGLE(SUN20I_D1_CODEC_ANALOG_MIC2_MIXER, 0, 2, mic2_mixer_texts);

static const struct snd_kcontrol_new sun20i_d1_mic2_mixer_controls =
	SOC_DAPM_ENUM("Route", mic2_mixer_enum);

static const char * const linein_mixer_texts[] = {
	"LINEIN", "MIXER"
};

static const struct soc_enum linein_mixer_enum =
	SOC_ENUM_SINGLE(SUN20I_D1_CODEC_ANALOG_LINEIN_MIXER, 0, 2, linein_mixer_texts);

static const struct snd_kcontrol_new sun20i_d1_linein_mixer_controls =
	SOC_DAPM_ENUM("Route", linein_mixer_enum);

static const char * const rec_mixer_texts[] = {
	"MIC1", "MIC2", "LINEIN", "MIXER"
};

static const struct soc_enum rec_mixer_enum =
	SOC_ENUM_SINGLE(SUN20I_D1_CODEC_ANALOG_REC_MIXER, 0, 4, rec_mixer_texts);

static const struct snd_kcontrol_new sun20i_d1_rec_mixer_controls =
	SOC_DAPM_ENUM("Route", rec_mixer_enum);

static const struct snd_soc_dapm_widget sun20i_d1_codec_analog_widgets[] = {
	SND_SOC_DAPM_DAC("DAC", "Playback", SUN20I_D1_CODEC_ANALOG_EN_DAC, 0, 0),
	SND_SOC_DAPM_ADC("ADC", "Capture", SUN20I_D1_CODEC_ANALOG_EN_ADC, 0, 0),

	SND_SOC_DAPM_MIXER("Output Mixer", SUN20I_D1_CODEC_ANALOG_PA_EN, 0, 0,
			   &sun20i_d1_out_mixer_controls, 1),
	SND_SOC_DAPM_MIXER("Input Mixer", SND_SOC_NOPM, 0, 0,
			   &sun20i_d1_in_mixer_controls, 1),
	SND_SOC_DAPM_MIXER("MIC1 Mixer", SND_SOC_NOPM, 0, 0,
			   &sun20i_d1_mic1_mixer_controls, 1),
	SND_SOC_DAPM_MIXER("MIC2 Mixer", SND_SOC_NOPM, 0, 0,
			   &sun20i_d1_mic2_mixer_controls, 1),
	SND_SOC_DAPM_MIXER("LINEIN Mixer", SND_SOC_NOPM, 0, 0,
			   &sun20i_d1_linein_mixer_controls, 1),
	SND_SOC_DAPM_MIXER("Record Mixer", SND_SOC_NOPM, 0, 0,
			   &sun20i_d1_rec_mixer_controls, 1),

	SND_SOC_DAPM_INPUT("MIC1"),
	SND_SOC_DAPM_INPUT("MIC2"),
	SND_SOC_DAPM_INPUT("LINEIN"),

	SND_SOC_DAPM_OUTPUT("HP"),
	SND_SOC_DAPM_OUTPUT("SPK"),
};

static const struct snd_soc_dapm_route sun20i_d1_codec_analog_routes[] = {
	{ "DAC", NULL, "Output Mixer" },
	{ "Output Mixer", "Stereo DAC", "DAC" },
	{ "Output Mixer", "MIC1", "MIC1 Mixer" },
	{ "Output Mixer", "MIC2", "MIC2 Mixer" },
	{ "Output Mixer", "LINEIN", "LINEIN Mixer" },
	{ "Output Mixer", "MIXER", "Input Mixer" },

	{ "Input Mixer", "Stereo DAC", "DAC" },
	{ "Input Mixer", "MIC1", "MIC1 Mixer" },
	{ "Input Mixer", "MIC2", "MIC2 Mixer" },
	{ "Input Mixer", "LINEIN", "LINEIN Mixer" },

	{ "MIC1 Mixer", "MIC1", "MIC1" },
	{ "MIC1 Mixer", "MIXER", "Input Mixer" },

	{ "MIC2 Mixer", "MIC2", "MIC2" },
	{ "MIC2 Mixer", "MIXER", "Input Mixer" },

	{ "LINEIN Mixer", "LINEIN", "LINEIN" },
	{ "LINEIN Mixer", "MIXER", "Input Mixer" },

	{ "Record Mixer", "MIC1", "MIC1 Mixer" },
	{ "Record Mixer", "MIC2", "MIC2 Mixer" },
	{ "Record Mixer", "LINEIN", "LINEIN Mixer" },
	{ "Record Mixer", "MIXER", "Input Mixer" },

	{ "ADC", NULL, "Record Mixer" },

	{ "HP", NULL, "Output Mixer" },
	{ "SPK", NULL, "Output Mixer" },
};

static const struct regmap_config sun20i_d1_codec_analog_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = SUN20I_D1_CODEC_ANALOG_PA_EN,
};

static int sun20i_d1_codec_analog_probe(struct platform_device *pdev)
{
	struct sun20i_d1_codec_analog *scodec;
	struct device *dev = &pdev->dev;
	void __iomem *base;
	int ret;

	scodec = devm_kzalloc(dev, sizeof(*scodec), GFP_KERNEL);
	if (!scodec)
		return -ENOMEM;

	scodec->dev = dev;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	scodec->regmap = devm_regmap_init_mmio(dev, base,
					       &sun20i_d1_codec_analog_regmap_config);
	if (IS_ERR(scodec->regmap))
		return PTR_ERR(scodec->regmap);

	scodec->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(scodec->clk))
		return PTR_ERR(scodec->clk);

	ret = clk_prepare_enable(scodec->clk);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, scodec);

	return 0;
}

static void sun20i_d1_codec_analog_remove(struct platform_device *pdev)
{
	struct sun20i_d1_codec_analog *scodec = platform_get_drvdata(pdev);

	clk_disable_unprepare(scodec->clk);
}

static const struct of_device_id sun20i_d1_codec_analog_of_match[] = {
	{ .compatible = "allwinner,sun20i-d1-codec-analog" },
	{}
};
MODULE_DEVICE_TABLE(of, sun20i_d1_codec_analog_of_match);

static struct platform_driver sun20i_d1_codec_analog_driver = {
	.probe = sun20i_d1_codec_analog_probe,
	.remove = sun20i_d1_codec_analog_remove,
	.driver = {
		.name = "sun20i-d1-codec-analog",
		.of_match_table = sun20i_d1_codec_analog_of_match,
	},
};
module_platform_driver(sun20i_d1_codec_analog_driver);

MODULE_DESCRIPTION("Allwinner D1 Analog Codec Driver");
MODULE_AUTHOR("Maksim Kiselev <bigunclemax@gmail.com>");
MODULE_LICENSE("GPL");