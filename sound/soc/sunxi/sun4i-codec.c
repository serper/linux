// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2014 Emilio López <emilio@elopez.com.ar>
 * Copyright 2014 Jon Smirl <jonsmirl@gmail.com>
 * Copyright 2015 Maxime Ripard <maxime.ripard@free-electrons.com>
 * Copyright 2015 Adam Sampson <ats@offog.org>
 * Copyright 2016 Chen-Yu Tsai <wens@csie.org>
 * Copyright 2018 Mesih Kilinc <mesihkilinc@gmail.com>
 *
 * Based on the Allwinner SDK driver, released under the GPL.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/gpio/consumer.h>

#include <sound/core.h>
#include <sound/jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>
#include <sound/initval.h>
#include <sound/dmaengine_pcm.h>

static int sun4i_codec_dma_prepare_slave_config(struct snd_pcm_substream *substream,
					     struct snd_pcm_hw_params *params,
					     struct dma_slave_config *slave_config);
static const struct snd_dmaengine_pcm_config sun4i_codec_dmaengine_pcm_config;

/* Codec DAC digital controls and FIFO registers */
#define SUN4I_CODEC_DAC_DPC			(0x00)
#define SUN4I_CODEC_DAC_DPC_EN_DA			(31)
#define SUN4I_CODEC_DAC_DPC_DVOL			(12)
#define SUN4I_CODEC_DAC_FIFOC			(0x04)
#define SUN4I_CODEC_DAC_FIFOC_DAC_FS			(29)
#define SUN4I_CODEC_DAC_FIFOC_FIR_VERSION		(28)
#define SUN4I_CODEC_DAC_FIFOC_SEND_LASAT		(26)
#define SUN4I_CODEC_DAC_FIFOC_TX_FIFO_MODE		(24)
#define SUN4I_CODEC_DAC_FIFOC_DRQ_CLR_CNT		(21)
#define SUN4I_CODEC_DAC_FIFOC_TX_TRIG_LEVEL		(8)
#define SUN4I_CODEC_DAC_FIFOC_MONO_EN			(6)
#define SUN4I_CODEC_DAC_FIFOC_TX_SAMPLE_BITS		(5)
#define SUN4I_CODEC_DAC_FIFOC_DAC_DRQ_EN		(4)
#define SUN4I_CODEC_DAC_FIFOC_FIFO_FLUSH		(0)
#define SUN4I_CODEC_DAC_FIFOS			(0x08)
#define SUN4I_CODEC_DAC_TXDATA			(0x0c)

/* Codec DAC side analog signal controls */
#define SUN4I_CODEC_DAC_ACTL			(0x10)
#define SUN4I_CODEC_DAC_ACTL_DACAENR			(31)
#define SUN4I_CODEC_DAC_ACTL_DACAENL			(30)
#define SUN4I_CODEC_DAC_ACTL_MIXEN			(29)
#define SUN4I_CODEC_DAC_ACTL_LNG			(26)
#define SUN4I_CODEC_DAC_ACTL_FMG			(23)
#define SUN4I_CODEC_DAC_ACTL_MICG			(20)
#define SUN4I_CODEC_DAC_ACTL_LLNS			(19)
#define SUN4I_CODEC_DAC_ACTL_RLNS			(18)
#define SUN4I_CODEC_DAC_ACTL_LFMS			(17)
#define SUN4I_CODEC_DAC_ACTL_RFMS			(16)
#define SUN4I_CODEC_DAC_ACTL_LDACLMIXS			(15)
#define SUN4I_CODEC_DAC_ACTL_RDACRMIXS			(14)
#define SUN4I_CODEC_DAC_ACTL_LDACRMIXS			(13)
#define SUN4I_CODEC_DAC_ACTL_MIC1LS			(12)
#define SUN4I_CODEC_DAC_ACTL_MIC1RS			(11)
#define SUN4I_CODEC_DAC_ACTL_MIC2LS			(10)
#define SUN4I_CODEC_DAC_ACTL_MIC2RS			(9)
#define SUN4I_CODEC_DAC_ACTL_DACPAS			(8)
#define SUN4I_CODEC_DAC_ACTL_MIXPAS			(7)
#define SUN4I_CODEC_DAC_ACTL_PA_MUTE			(6)
#define SUN4I_CODEC_DAC_ACTL_PA_VOL			(0)
#define SUN4I_CODEC_DAC_TUNE			(0x14)
#define SUN4I_CODEC_DAC_DEBUG			(0x18)

/* Codec ADC digital controls and FIFO registers */
#define SUN4I_CODEC_ADC_FIFOC			(0x1c)
#define SUN4I_CODEC_ADC_FIFOC_ADC_FS			(29)
#define SUN4I_CODEC_ADC_FIFOC_EN_AD			(28)
#define SUN4I_CODEC_ADC_FIFOC_RX_FIFO_MODE		(24)
#define SUN4I_CODEC_ADC_FIFOC_RX_TRIG_LEVEL		(8)
#define SUN4I_CODEC_ADC_FIFOC_MONO_EN			(7)
#define SUN4I_CODEC_ADC_FIFOC_RX_SAMPLE_BITS		(6)
#define SUN4I_CODEC_ADC_FIFOC_ADC_DRQ_EN		(4)
#define SUN4I_CODEC_ADC_FIFOC_FIFO_FLUSH		(0)
#define SUN4I_CODEC_ADC_FIFOS			(0x20)
#define SUN4I_CODEC_ADC_RXDATA			(0x24)

/* Codec ADC side analog signal controls */
#define SUN4I_CODEC_ADC_ACTL			(0x28)
#define SUN4I_CODEC_ADC_ACTL_ADC_R_EN			(31)
#define SUN4I_CODEC_ADC_ACTL_ADC_L_EN			(30)
#define SUN4I_CODEC_ADC_ACTL_PREG1EN			(29)
#define SUN4I_CODEC_ADC_ACTL_PREG2EN			(28)
#define SUN4I_CODEC_ADC_ACTL_VMICEN			(27)
#define SUN4I_CODEC_ADC_ACTL_PREG1			(25)
#define SUN4I_CODEC_ADC_ACTL_PREG2			(23)
#define SUN4I_CODEC_ADC_ACTL_VADCG			(20)
#define SUN4I_CODEC_ADC_ACTL_ADCIS			(17)
#define SUN4I_CODEC_ADC_ACTL_LNPREG			(13)
#define SUN4I_CODEC_ADC_ACTL_PA_EN			(4)
#define SUN4I_CODEC_ADC_ACTL_DDE			(3)
#define SUN4I_CODEC_ADC_DEBUG			(0x2c)

/* FIFO counters */
#define SUN4I_CODEC_ADC_RXCNT			(0x34)

/* Calibration register (sun7i only) */
#define SUN7I_CODEC_AC_DAC_CAL			(0x38)

/* Microphone controls (sun7i only) */
#define SUN7I_CODEC_AC_MIC_PHONE_CAL		(0x3c)

#define SUN7I_CODEC_AC_MIC_PHONE_CAL_PREG1		(29)
#define SUN7I_CODEC_AC_MIC_PHONE_CAL_PREG2		(26)

/*
 * sun6i specific registers
 *
 * sun6i shares the same digital control and FIFO registers as sun4i,
 * but only the DAC digital controls are at the same offset. The others
 * have been moved around to accommodate extra analog controls.
 */

/* Codec DAC digital controls and FIFO registers */
#define SUN6I_CODEC_ADC_FIFOC			(0x10)
#define SUN6I_CODEC_ADC_FIFOC_EN_AD			(28)
#define SUN6I_CODEC_ADC_FIFOS			(0x14)
#define SUN6I_CODEC_ADC_RXDATA			(0x18)

/* Output mixer and gain controls */
#define SUN6I_CODEC_OM_DACA_CTRL		(0x20)
#define SUN6I_CODEC_OM_DACA_CTRL_DACAREN		(31)
#define SUN6I_CODEC_OM_DACA_CTRL_DACALEN		(30)
#define SUN6I_CODEC_OM_DACA_CTRL_RMIXEN			(29)
#define SUN6I_CODEC_OM_DACA_CTRL_LMIXEN			(28)
#define SUN6I_CODEC_OM_DACA_CTRL_RMIX_MIC1		(23)
#define SUN6I_CODEC_OM_DACA_CTRL_RMIX_MIC2		(22)
#define SUN6I_CODEC_OM_DACA_CTRL_RMIX_PHONE		(21)
#define SUN6I_CODEC_OM_DACA_CTRL_RMIX_PHONEP		(20)
#define SUN6I_CODEC_OM_DACA_CTRL_RMIX_LINEINR		(19)
#define SUN6I_CODEC_OM_DACA_CTRL_RMIX_DACR		(18)
#define SUN6I_CODEC_OM_DACA_CTRL_RMIX_DACL		(17)
#define SUN6I_CODEC_OM_DACA_CTRL_LMIX_MIC1		(16)
#define SUN6I_CODEC_OM_DACA_CTRL_LMIX_MIC2		(15)
#define SUN6I_CODEC_OM_DACA_CTRL_LMIX_PHONE		(14)
#define SUN6I_CODEC_OM_DACA_CTRL_LMIX_PHONEN		(13)
#define SUN6I_CODEC_OM_DACA_CTRL_LMIX_LINEINL		(12)
#define SUN6I_CODEC_OM_DACA_CTRL_LMIX_DACL		(11)
#define SUN6I_CODEC_OM_DACA_CTRL_LMIX_DACR		(10)
#define SUN6I_CODEC_OM_DACA_CTRL_RHPIS			(9)
#define SUN6I_CODEC_OM_DACA_CTRL_LHPIS			(8)
#define SUN6I_CODEC_OM_DACA_CTRL_RHPPAMUTE		(7)
#define SUN6I_CODEC_OM_DACA_CTRL_LHPPAMUTE		(6)
#define SUN6I_CODEC_OM_DACA_CTRL_HPVOL			(0)
#define SUN6I_CODEC_OM_PA_CTRL			(0x24)
#define SUN6I_CODEC_OM_PA_CTRL_HPPAEN			(31)
#define SUN6I_CODEC_OM_PA_CTRL_HPCOM_CTL		(29)
#define SUN6I_CODEC_OM_PA_CTRL_COMPTEN			(28)
#define SUN6I_CODEC_OM_PA_CTRL_MIC1G			(15)
#define SUN6I_CODEC_OM_PA_CTRL_MIC2G			(12)
#define SUN6I_CODEC_OM_PA_CTRL_LINEING			(9)
#define SUN6I_CODEC_OM_PA_CTRL_PHONEG			(6)
#define SUN6I_CODEC_OM_PA_CTRL_PHONEPG			(3)
#define SUN6I_CODEC_OM_PA_CTRL_PHONENG			(0)

/* Microphone, line out and phone out controls */
#define SUN6I_CODEC_MIC_CTRL			(0x28)
#define SUN6I_CODEC_MIC_CTRL_HBIASEN			(31)
#define SUN6I_CODEC_MIC_CTRL_MBIASEN			(30)
#define SUN6I_CODEC_MIC_CTRL_MIC1AMPEN			(28)
#define SUN6I_CODEC_MIC_CTRL_MIC1BOOST			(25)
#define SUN6I_CODEC_MIC_CTRL_MIC2AMPEN			(24)
#define SUN6I_CODEC_MIC_CTRL_MIC2BOOST			(21)
#define SUN6I_CODEC_MIC_CTRL_MIC2SLT			(20)
#define SUN6I_CODEC_MIC_CTRL_LINEOUTLEN			(19)
#define SUN6I_CODEC_MIC_CTRL_LINEOUTREN			(18)
#define SUN6I_CODEC_MIC_CTRL_LINEOUTLSRC		(17)
#define SUN6I_CODEC_MIC_CTRL_LINEOUTRSRC		(16)
#define SUN6I_CODEC_MIC_CTRL_LINEOUTVC			(11)
#define SUN6I_CODEC_MIC_CTRL_PHONEPREG			(8)

/* ADC mixer controls */
#define SUN6I_CODEC_ADC_ACTL			(0x2c)
#define SUN6I_CODEC_ADC_ACTL_ADCREN			(31)
#define SUN6I_CODEC_ADC_ACTL_ADCLEN			(30)
#define SUN6I_CODEC_ADC_ACTL_ADCRG			(27)
#define SUN6I_CODEC_ADC_ACTL_ADCLG			(24)
#define SUN6I_CODEC_ADC_ACTL_RADCMIX_MIC1		(13)
#define SUN6I_CODEC_ADC_ACTL_RADCMIX_MIC2		(12)
#define SUN6I_CODEC_ADC_ACTL_RADCMIX_PHONE		(11)
#define SUN6I_CODEC_ADC_ACTL_RADCMIX_PHONEP		(10)
#define SUN6I_CODEC_ADC_ACTL_RADCMIX_LINEINR		(9)
#define SUN6I_CODEC_ADC_ACTL_RADCMIX_OMIXR		(8)
#define SUN6I_CODEC_ADC_ACTL_RADCMIX_OMIXL		(7)
#define SUN6I_CODEC_ADC_ACTL_LADCMIX_MIC1		(6)
#define SUN6I_CODEC_ADC_ACTL_LADCMIX_MIC2		(5)
#define SUN6I_CODEC_ADC_ACTL_LADCMIX_PHONE		(4)
#define SUN6I_CODEC_ADC_ACTL_LADCMIX_PHONEN		(3)
#define SUN6I_CODEC_ADC_ACTL_LADCMIX_LINEINL		(2)
#define SUN6I_CODEC_ADC_ACTL_LADCMIX_OMIXL		(1)
#define SUN6I_CODEC_ADC_ACTL_LADCMIX_OMIXR		(0)

/* Analog performance tuning controls */
#define SUN6I_CODEC_ADDA_TUNE			(0x30)

/* Calibration controls */
#define SUN6I_CODEC_CALIBRATION			(0x34)

/* FIFO counters (sun6i) */
#define SUN6I_CODEC_ADC_RXCNT			(0x44)

/* headset jack detection and button support registers */
#define SUN6I_CODEC_HMIC_CTL			(0x50)
#define SUN6I_CODEC_HMIC_DATA			(0x54)

/* TODO sun6i DAP (Digital Audio Processing) bits */

/* FIFO counters moved on A23 (solo RX se emplearía en este driver) */
#define SUN8I_A23_CODEC_ADC_RXCNT		(0x20)

/* TX FIFO moved on H3 */
#define SUN8I_H3_CODEC_DAC_TXDATA		(0x20)
#define SUN8I_H3_CODEC_DAC_DBG			(0x48)
#define SUN8I_H3_CODEC_ADC_DBG			(0x4c)

/* H616 specific registers */
#define SUN50I_H616_CODEC_DAC_FIFOC		(0x10)

#define SUN50I_DAC_FIFO_STA			(0x14)
#define SUN50I_DAC_TXE_INT			(3)
#define SUN50I_DAC_TXU_INT			(2)
#define SUN50I_DAC_TXO_INT			(1)

#define SUN50I_DAC_CNT				(0x24)
#define SUN50I_DAC_DG_REG			(0x28)
#define SUN50I_DAC_DAP_CTL			(0xf0)

#define SUN50I_H616_DAC_AC_DAC_REG		(0x310)
#define SUN50I_H616_DAC_LEN			(15)
#define SUN50I_H616_DAC_REN			(14)
#define SUN50I_H616_LINEOUTL_EN			(13)
#define SUN50I_H616_LMUTE			(12)
#define SUN50I_H616_LINEOUTR_EN			(11)
#define SUN50I_H616_RMUTE			(10)
#define SUN50I_H616_RSWITCH			(9)
#define SUN50I_H616_RAMPEN			(8)
#define SUN50I_H616_LINEOUTL_SEL		(6)
#define SUN50I_H616_LINEOUTR_SEL		(5)
#define SUN50I_H616_LINEOUT_VOL			(0)

#define SUN50I_H616_DAC_AC_MIXER_REG		(0x314)
#define SUN50I_H616_LMIX_LDAC			(21)
#define SUN50I_H616_LMIX_RDAC			(20)
#define SUN50I_H616_RMIX_RDAC			(17)
#define SUN50I_H616_RMIX_LDAC			(16)
#define SUN50I_H616_LMIXEN			(11)
#define SUN50I_H616_RMIXEN			(10)

#define SUN50I_H616_DAC_AC_RAMP_REG		(0x31c)
#define SUN50I_H616_RAMP_STEP			(4)
#define SUN50I_H616_RDEN			(0)

/* TODO H3 DAP (Digital Audio Processing) bits */

/*
 * sun20i D1 and similar codecs specific registers
 *
 * Almost all registers moved on D1, including ADC digital controls,
 * FIFO and RX data registers. Only DAC control are at the same offset.
 */

#define SUN20I_D1_CODEC_DAC_VOL_CTRL		(0x04)
#define SUN20I_D1_CODEC_DAC_VOL_SEL			(16)
#define SUN20I_D1_CODEC_DAC_VOL_L			(8)
#define SUN20I_D1_CODEC_DAC_VOL_R			(0)
#define SUN20I_D1_CODEC_DAC_FIFOC		(0x10)
#define SUN20I_D1_CODEC_ADC_FIFOC		(0x30)
#define SUN20I_D1_CODEC_ADC_FIFOC_EN_AD			(28)
#define SUN20I_D1_CODEC_ADC_FIFOC_RX_SAMPLE_BITS	(16)
#define SUN20I_D1_CODEC_ADC_FIFOC_RX_TRIG_LEVEL		(4)
#define SUN20I_D1_CODEC_ADC_FIFOC_ADC_DRQ_EN		(3)
#define SUN20I_D1_CODEC_ADC_VOL_CTRL1		(0x34)
#define SUN20I_D1_CODEC_ADC_VOL_CTRL1_ADC3_VOL		(16)
#define SUN20I_D1_CODEC_ADC_VOL_CTRL1_ADC2_VOL		(8)
#define SUN20I_D1_CODEC_ADC_VOL_CTRL1_ADC1_VOL		(0)
#define SUN20I_D1_CODEC_ADC_RXDATA		(0x40)
#define SUN20I_D1_CODEC_ADC_DIG_CTRL		(0x50)
#define SUN20I_D1_CODEC_ADC_DIG_CTRL_ADC3_CH_EN		(2)
#define SUN20I_D1_CODEC_ADC_DIG_CTRL_ADC2_CH_EN		(1)
#define SUN20I_D1_CODEC_ADC_DIG_CTRL_ADC1_CH_EN		(0)
#define SUN20I_D1_CODEC_VRA1SPEEDUP_DOWN_CTRL	(0x54)

/* TODO D1 DAP (Digital Audio Processing) bits */

#define SUN4I_DMA_MAX_BURST			(8)

/* suniv specific registers */

#define SUNIV_DMA_MAX_BURST			(4)

/* Codec DAC digital controls and FIFO registers */
#define SUNIV_CODEC_ADC_FIFOC			(0x10)
#define SUNIV_CODEC_ADC_FIFOC_EN_AD		(28)
#define SUNIV_CODEC_ADC_FIFOS			(0x14)
#define SUNIV_CODEC_ADC_RXDATA			(0x18)

/* Output mixer and gain controls */
#define SUNIV_CODEC_OM_DACA_CTRL			(0x20)
#define SUNIV_CODEC_OM_DACA_CTRL_DACAREN		(31)
#define SUNIV_CODEC_OM_DACA_CTRL_DACALEN		(30)
#define SUNIV_CODEC_OM_DACA_CTRL_RMIXEN			(29)
#define SUNIV_CODEC_OM_DACA_CTRL_LMIXEN			(28)
#define SUNIV_CODEC_OM_DACA_CTRL_RHPPAMUTE		(27)
#define SUNIV_CODEC_OM_DACA_CTRL_LHPPAMUTE		(26)
#define SUNIV_CODEC_OM_DACA_CTRL_RHPIS			(25)
#define SUNIV_CODEC_OM_DACA_CTRL_LHPIS			(24)
#define SUNIV_CODEC_OM_DACA_CTRL_HPCOM_CTL		(22)
#define SUNIV_CODEC_OM_DACA_CTRL_COMPTEN		(21)
#define SUNIV_CODEC_OM_DACA_CTRL_RMIXMUTE_MICIN		(20)
#define SUNIV_CODEC_OM_DACA_CTRL_RMIXMUTE_LINEIN	(19)
#define SUNIV_CODEC_OM_DACA_CTRL_RMIXMUTE_FMIN		(18)
#define SUNIV_CODEC_OM_DACA_CTRL_RMIXMUTE_RDAC		(17)
#define SUNIV_CODEC_OM_DACA_CTRL_RMIXMUTE_LDAC		(16)
#define SUNIV_CODEC_OM_DACA_CTRL_HPPAEN			(15)
#define SUNIV_CODEC_OM_DACA_CTRL_LMIXMUTE_MICIN		(12)
#define SUNIV_CODEC_OM_DACA_CTRL_LMIXMUTE_LINEIN	(11)
#define SUNIV_CODEC_OM_DACA_CTRL_LMIXMUTE_FMIN		(10)
#define SUNIV_CODEC_OM_DACA_CTRL_LMIXMUTE_LDAC		(9)
#define SUNIV_CODEC_OM_DACA_CTRL_LMIXMUTE_RDAC		(8)
#define SUNIV_CODEC_OM_DACA_CTRL_LTLNMUTE		(7)
#define SUNIV_CODEC_OM_DACA_CTRL_RTLNMUTE		(6)
#define SUNIV_CODEC_OM_DACA_CTRL_HPVOL			(0)

/* Analog Input Mixer controls */
#define SUNIV_CODEC_ADC_ACTL		(0x24)
#define SUNIV_CODEC_ADC_ADCEN		(31)
#define SUNIV_CODEC_ADC_MICG		(24)
#define SUNIV_CODEC_ADC_LINEINVOL	(21)
#define SUNIV_CODEC_ADC_ADCG		(16)
#define SUNIV_CODEC_ADC_ADCMIX_MIC	(13)
#define SUNIV_CODEC_ADC_ADCMIX_FMINL	(12)
#define SUNIV_CODEC_ADC_ADCMIX_FMINR	(11)
#define SUNIV_CODEC_ADC_ADCMIX_LINEIN	(10)
#define SUNIV_CODEC_ADC_ADCMIX_LOUT	(9)
#define SUNIV_CODEC_ADC_ADCMIX_ROUT	(8)
#define SUNIV_CODEC_ADC_PASPEEDSELECT	(7)
#define SUNIV_CODEC_ADC_FMINVOL		(4)
#define SUNIV_CODEC_ADC_MICAMPEN	(3)
#define SUNIV_CODEC_ADC_MICBOOST	(0)

#define SUNIV_CODEC_ADC_DBG		(0x4c)

struct sun4i_codec_quirks {
	const struct regmap_config *regmap_config;
	const struct snd_soc_component_driver *codec;
	struct snd_soc_card *(*create_card)(struct device *dev);
	struct reg_field reg_dac_fifoc;
	struct reg_field reg_adc_fifoc;
	unsigned int adc_drq_en;
	unsigned int rx_sample_bits;
	unsigned int rx_trig_level;
	unsigned int reg_dac_txdata;
	unsigned int reg_adc_rxdata;
	unsigned int dma_max_burst;
	bool has_reset;
	bool has_dual_clock;
};

static struct snd_soc_card *sun20i_d1_codec_create_card(struct device *dev);
static struct snd_soc_dai_link *sun4i_codec_create_link(struct device *dev, int *num_links);

struct sun4i_codec {
	struct device	*dev;
	struct regmap	*regmap;
	struct clk	*clk_apb;
	struct clk	*clk_module;
	struct clk	*clk_module_dac;
	struct reset_control *rst;
	struct gpio_desc *gpio_pa;
	struct gpio_desc *gpio_hp;

	const struct sun4i_codec_quirks *quirks;

	/* ADC_FIFOC register is at different offset on different SoCs */
	struct regmap_field *reg_adc_fifoc;
	/* DAC_FIFOC register is at different offset on different SoCs */
	struct regmap_field *reg_dac_fifoc;

	struct snd_dmaengine_dai_dma_data	capture_dma_data;
	struct snd_dmaengine_dai_dma_data	playback_dma_data;
	unsigned int	ofs_txdata; /* Offset dinámico TXDATA (D1=0x20) */
};

static void sun4i_codec_start_playback(struct sun4i_codec *scodec)
{
	/* Flush TX FIFO */
	regmap_field_set_bits(scodec->reg_dac_fifoc,
			      BIT(SUN4I_CODEC_DAC_FIFOC_FIFO_FLUSH));

	/* Enable DAC DRQ */
	regmap_field_set_bits(scodec->reg_dac_fifoc,
			      BIT(SUN4I_CODEC_DAC_FIFOC_DAC_DRQ_EN));
}

static void sun4i_codec_stop_playback(struct sun4i_codec *scodec)
{
	/* Disable DAC DRQ */
	regmap_field_clear_bits(scodec->reg_dac_fifoc,
				BIT(SUN4I_CODEC_DAC_FIFOC_DAC_DRQ_EN));
}

static void sun4i_codec_start_capture(struct sun4i_codec *scodec)
{
	/* Enable ADC DRQ */
	regmap_field_set_bits(scodec->reg_adc_fifoc,
			      BIT(scodec->quirks->adc_drq_en));
}

static void sun4i_codec_stop_capture(struct sun4i_codec *scodec)
{
	/* Disable ADC DRQ */
	regmap_field_clear_bits(scodec->reg_adc_fifoc,
				 BIT(scodec->quirks->adc_drq_en));
}

static int sun4i_codec_trigger(struct snd_pcm_substream *substream, int cmd,
		       struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(rtd->card);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			sun4i_codec_start_playback(scodec);
		} else {
			/* D1: Configure ADC channel and DRQ for capture */
			if (of_device_is_compatible(scodec->dev->of_node, "allwinner,sun20i-d1-codec")) {
				bool mono = substream->runtime && substream->runtime->channels == 1;
				
				/* Enable ADC */
				regmap_update_bits(scodec->regmap, SUN20I_D1_CODEC_ADC_FIFOC,
					BIT(SUN20I_D1_CODEC_ADC_FIFOC_EN_AD),
					BIT(SUN20I_D1_CODEC_ADC_FIFOC_EN_AD));
				
				/* Configure ADC channels: ADC3 for mono, ADC1+ADC3 for stereo */
				regmap_update_bits(scodec->regmap, SUN20I_D1_CODEC_ADC_DIG_CTRL,
					BIT(SUN20I_D1_CODEC_ADC_DIG_CTRL_ADC1_CH_EN) |
					BIT(SUN20I_D1_CODEC_ADC_DIG_CTRL_ADC3_CH_EN),
					mono ? BIT(SUN20I_D1_CODEC_ADC_DIG_CTRL_ADC3_CH_EN) :
					(BIT(SUN20I_D1_CODEC_ADC_DIG_CTRL_ADC1_CH_EN) |
					 BIT(SUN20I_D1_CODEC_ADC_DIG_CTRL_ADC3_CH_EN)));
				
				/* Enable DRQ */
				regmap_update_bits(scodec->regmap, SUN20I_D1_CODEC_ADC_FIFOC,
					BIT(SUN20I_D1_CODEC_ADC_FIFOC_ADC_DRQ_EN),
					BIT(SUN20I_D1_CODEC_ADC_FIFOC_ADC_DRQ_EN));
			}
			sun4i_codec_start_capture(scodec);
		}
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			sun4i_codec_stop_playback(scodec);
		else
			sun4i_codec_stop_capture(scodec);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int sun4i_codec_prepare_capture(struct snd_pcm_substream *substream,
				       struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(rtd->card);

	/* Flush RX FIFO */
	regmap_field_set_bits(scodec->reg_adc_fifoc,
				 BIT(SUN4I_CODEC_ADC_FIFOC_FIFO_FLUSH));

	/* Set RX FIFO trigger level */
	regmap_field_update_bits(scodec->reg_adc_fifoc,
				 0xf << scodec->quirks->rx_trig_level,
				 0x7 << scodec->quirks->rx_trig_level);

	/*
	 * FIXME: Undocumented in the datasheet, but
	 *        Allwinner's code mentions that it is
	 *        related to microphone gain
	 */
	if (of_device_is_compatible(scodec->dev->of_node,
				    "allwinner,sun4i-a10-codec") ||
	    of_device_is_compatible(scodec->dev->of_node,
				    "allwinner,sun7i-a20-codec")) {
		regmap_update_bits(scodec->regmap, SUN4I_CODEC_ADC_ACTL,
				   0x3 << 25,
				   0x1 << 25);
	}

	if (of_device_is_compatible(scodec->dev->of_node,
				    "allwinner,sun7i-a20-codec"))
		/* FIXME: Undocumented bits */
		regmap_update_bits(scodec->regmap, SUN4I_CODEC_DAC_TUNE,
				   0x3 << 8,
				   0x1 << 8);

	return 0;
}

static int sun4i_codec_prepare_playback(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(rtd->card);
	u32 val;

	/* Flush the TX FIFO */
	regmap_field_set_bits(scodec->reg_dac_fifoc,
		      BIT(SUN4I_CODEC_DAC_FIFOC_FIFO_FLUSH));

	/* Set TX FIFO Empty Trigger Level */
	regmap_field_update_bits(scodec->reg_dac_fifoc,
			 0x3f << SUN4I_CODEC_DAC_FIFOC_TX_TRIG_LEVEL,
			 0x18 << SUN4I_CODEC_DAC_FIFOC_TX_TRIG_LEVEL);

	if (substream->runtime->rate > 32000)
		/* Use 64 bits FIR filter */
		val = 0;
	else
		/* Use 32 bits FIR filter */
		val = BIT(SUN4I_CODEC_DAC_FIFOC_FIR_VERSION);

	regmap_field_update_bits(scodec->reg_dac_fifoc,
			 BIT(SUN4I_CODEC_DAC_FIFOC_FIR_VERSION),
			 val);

	/* Send zeros when we have an underrun */
	regmap_field_clear_bits(scodec->reg_dac_fifoc,
			BIT(SUN4I_CODEC_DAC_FIFOC_SEND_LASAT));

	return 0;
};

static int sun4i_codec_prepare(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return sun4i_codec_prepare_playback(substream, dai);

	return sun4i_codec_prepare_capture(substream, dai);
}

static unsigned long sun4i_codec_get_mod_freq(struct snd_pcm_hw_params *params)
{
	unsigned int rate = params_rate(params);

	switch (rate) {
	case 176400:
	case 88200:
	case 44100:
	case 33075:
	case 22050:
	case 14700:
	case 11025:
	case 7350:
		return 22579200;

	case 192000:
	case 96000:
	case 48000:
	case 32000:
	case 24000:
	case 16000:
	case 12000:
	case 8000:
		return 24576000;

	default:
		return 0;
	}
}

static int sun4i_codec_get_hw_rate(struct snd_pcm_hw_params *params)
{
	unsigned int rate = params_rate(params);

	switch (rate) {
	case 192000:
	case 176400:
		return 6;

	case 96000:
	case 88200:
		return 7;

	case 48000:
	case 44100:
		return 0;

	case 32000:
	case 33075:
		return 1;

	case 24000:
	case 22050:
		return 2;

	case 16000:
	case 14700:
		return 3;

	case 12000:
	case 11025:
		return 4;

	case 8000:
	case 7350:
		return 5;

	default:
		return -EINVAL;
	}
}

static int sun4i_codec_hw_params_capture(struct sun4i_codec *scodec,
			 struct snd_pcm_hw_params *params,
			 unsigned int hwrate)
{
	u32 before = 0;

	regmap_field_read(scodec->reg_adc_fifoc, &before);

	if (of_device_is_compatible(scodec->dev->of_node, "allwinner,sun20i-d1-codec")) {
		/* D1: Configure ADC FIFO for capture */
		regmap_field_update_bits(scodec->reg_adc_fifoc,
				 7 << SUN4I_CODEC_ADC_FIFOC_ADC_FS,
				 hwrate << SUN4I_CODEC_ADC_FIFOC_ADC_FS);
		
		/* Configure for 16-bit samples */
		regmap_field_clear_bits(scodec->reg_adc_fifoc,
				BIT(scodec->quirks->rx_sample_bits));
		
		/* Configure RX_FIFO_MODE for proper DMA transfer */
		regmap_field_set_bits(scodec->reg_adc_fifoc,
				BIT(SUN4I_CODEC_ADC_FIFOC_RX_FIFO_MODE));
		
		/* Configure MONO_EN for single channel */
		if (params_channels(params) == 1)
			regmap_field_set_bits(scodec->reg_adc_fifoc,
				      BIT(SUN4I_CODEC_ADC_FIFOC_MONO_EN));
		else
			regmap_field_clear_bits(scodec->reg_adc_fifoc,
					BIT(SUN4I_CODEC_ADC_FIFOC_MONO_EN));
	} else {
		/* Ruta legacy original */
		regmap_field_update_bits(scodec->reg_adc_fifoc,
				 7 << SUN4I_CODEC_ADC_FIFOC_ADC_FS,
				 hwrate << SUN4I_CODEC_ADC_FIFOC_ADC_FS);
		if (params_channels(params) == 1)
			regmap_field_set_bits(scodec->reg_adc_fifoc,
				      BIT(SUN4I_CODEC_ADC_FIFOC_MONO_EN));
		else
			regmap_field_clear_bits(scodec->reg_adc_fifoc,
					BIT(SUN4I_CODEC_ADC_FIFOC_MONO_EN));
		regmap_field_clear_bits(scodec->reg_adc_fifoc,
				BIT(SUN4I_CODEC_ADC_FIFOC_RX_SAMPLE_BITS));
		regmap_field_set_bits(scodec->reg_adc_fifoc,
				BIT(SUN4I_CODEC_ADC_FIFOC_RX_FIFO_MODE));
	}

	return 0;
}

static int sun4i_codec_hw_params_playback(struct sun4i_codec *scodec,
				  struct snd_pcm_hw_params *params,
				  unsigned int hwrate)
{
	u32 val;

	/* Set DAC sample rate */
	regmap_field_update_bits(scodec->reg_dac_fifoc,
				 7 << SUN4I_CODEC_DAC_FIFOC_DAC_FS,
				 hwrate << SUN4I_CODEC_DAC_FIFOC_DAC_FS);

	/* Set the number of channels we want to use */
	if (params_channels(params) == 1)
		val = BIT(SUN4I_CODEC_DAC_FIFOC_MONO_EN);
	else
		val = 0;

	regmap_field_update_bits(scodec->reg_dac_fifoc,
				 BIT(SUN4I_CODEC_DAC_FIFOC_MONO_EN),
				 val);

	/* Sólo 16 bits */
	regmap_field_clear_bits(scodec->reg_dac_fifoc,
				BIT(SUN4I_CODEC_DAC_FIFOC_TX_SAMPLE_BITS));
	regmap_field_set_bits(scodec->reg_dac_fifoc,
				BIT(SUN4I_CODEC_DAC_FIFOC_TX_FIFO_MODE));

	return 0;
}

static int sun4i_codec_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(rtd->card);
	unsigned long clk_freq;
	int ret, hwrate;

	clk_freq = sun4i_codec_get_mod_freq(params);
	if (!clk_freq)
		return -EINVAL;

	ret = clk_set_rate(scodec->clk_module, clk_freq);
	if (ret)
		return ret;

	hwrate = sun4i_codec_get_hw_rate(params);
	if (hwrate < 0)
		return hwrate;

	if (scodec->quirks->has_dual_clock) {
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			ret = clk_set_rate(scodec->clk_module_dac, clk_freq);
		else
			ret = clk_set_rate(scodec->clk_module, clk_freq);
		if (ret)
			return ret;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return sun4i_codec_hw_params_playback(scodec, params,
						      hwrate);

	return sun4i_codec_hw_params_capture(scodec, params,
					     hwrate);
}

static int sun4i_codec_startup(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(rtd->card);

	/*
	 * Stop issuing DRQ when we have room for less than 16 samples
	 * in our TX FIFO
	 */
	regmap_field_set_bits(scodec->reg_dac_fifoc,
			      3 << SUN4I_CODEC_DAC_FIFOC_DRQ_CLR_CNT);

	if (scodec->quirks->has_dual_clock &&
	    substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		return clk_prepare_enable(scodec->clk_module_dac);
	else
		return clk_prepare_enable(scodec->clk_module);
}

static void sun4i_codec_shutdown(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(rtd->card);

	if (scodec->quirks->has_dual_clock &&
	    substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		clk_disable_unprepare(scodec->clk_module_dac);
	else
		clk_disable_unprepare(scodec->clk_module);
}

static int sun4i_codec_dai_probe(struct snd_soc_dai *dai)
{
	struct snd_soc_card *card = dai->component && dai->component->card ? dai->component->card : NULL;
	struct sun4i_codec *scodec = card ? snd_soc_card_get_drvdata(card) : NULL;

	if (!scodec) {
		dev_err(dai->dev, "Missing codec drvdata\n");
		return -ENODEV;
	}

	snd_soc_dai_init_dma_data(dai,
							  &scodec->playback_dma_data,
							  &scodec->capture_dma_data);
	return 0;
}

static const struct snd_soc_dai_ops sun4i_codec_dai_ops = {
    /* .probe inicializa dma_data ahora que el DAI existe; scodec se obtiene vía card->drvdata */
    .probe          = sun4i_codec_dai_probe,
    .startup        = sun4i_codec_startup,
    .shutdown       = sun4i_codec_shutdown,
    .trigger        = sun4i_codec_trigger,
    .hw_params      = sun4i_codec_hw_params,
    .prepare        = sun4i_codec_prepare,
};

/* Máscaras separadas: playback hasta 192 kHz; capture limitado a 48 kHz */
#define SUN4I_CODEC_PLAYBACK_RATES (\
		SNDRV_PCM_RATE_8000_48000 |\
		SNDRV_PCM_RATE_12000 |\
		SNDRV_PCM_RATE_24000 |\
		SNDRV_PCM_RATE_96000 |\
		SNDRV_PCM_RATE_192000)

/* 24 kHz no está incluido dentro del rango 8000_48000 */
#define SUN4I_CODEC_CAPTURE_RATES (\
		SNDRV_PCM_RATE_8000_48000 |\
		SNDRV_PCM_RATE_24000)

static struct snd_soc_dai_driver sun4i_codec_dai = {
	.name	= "Codec",
	.ops	= &sun4i_codec_dai_ops,
	.playback = {
		.stream_name	= "Codec Playback",
		.channels_min	= 1,
		.channels_max	= 2,
		.rate_min	= 8000,
		.rate_max	= 192000,
		.rates		= SUN4I_CODEC_PLAYBACK_RATES,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE,
		.sig_bits	= 16,
	},
	.capture = {
		.stream_name	= "Codec Capture",
		.channels_min	= 1,
		.channels_max	= 2,
		.rate_min	= 8000,
		.rate_max	= 48000,
		.rates		= SUN4I_CODEC_CAPTURE_RATES,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE,
		.sig_bits	= 16,
	},
};

/*** sun4i Codec ***/
static const struct snd_kcontrol_new sun4i_codec_pa_mute =
	SOC_DAPM_SINGLE("Switch", SUN4I_CODEC_DAC_ACTL,
			SUN4I_CODEC_DAC_ACTL_PA_MUTE, 1, 0);

static DECLARE_TLV_DB_SCALE(sun4i_codec_pa_volume_scale, -6300, 100, 1);
static DECLARE_TLV_DB_SCALE(sun4i_codec_linein_loopback_gain_scale, -150, 150,
			    0);
static DECLARE_TLV_DB_SCALE(sun4i_codec_linein_preamp_gain_scale, -1200, 300,
			    0);
static DECLARE_TLV_DB_SCALE(sun4i_codec_fmin_loopback_gain_scale, -450, 150,
			    0);
static DECLARE_TLV_DB_SCALE(sun4i_codec_micin_loopback_gain_scale, -450, 150,
			    0);
static DECLARE_TLV_DB_RANGE(sun4i_codec_micin_preamp_gain_scale,
			    0, 0, TLV_DB_SCALE_ITEM(0, 0, 0),
			    1, 7, TLV_DB_SCALE_ITEM(3500, 300, 0));
static DECLARE_TLV_DB_RANGE(sun7i_codec_micin_preamp_gain_scale,
			    0, 0, TLV_DB_SCALE_ITEM(0, 0, 0),
			    1, 7, TLV_DB_SCALE_ITEM(2400, 300, 0));

static const struct snd_kcontrol_new sun4i_codec_controls[] = {
	SOC_SINGLE_TLV("Power Amplifier Volume", SUN4I_CODEC_DAC_ACTL,
		       SUN4I_CODEC_DAC_ACTL_PA_VOL, 0x3F, 0,
		       sun4i_codec_pa_volume_scale),
	SOC_SINGLE_TLV("Line Playback Volume", SUN4I_CODEC_DAC_ACTL,
		       SUN4I_CODEC_DAC_ACTL_LNG, 1, 0,
		       sun4i_codec_linein_loopback_gain_scale),
	SOC_SINGLE_TLV("Line Boost Volume", SUN4I_CODEC_ADC_ACTL,
		       SUN4I_CODEC_ADC_ACTL_LNPREG, 7, 0,
		       sun4i_codec_linein_preamp_gain_scale),
	SOC_SINGLE_TLV("FM Playback Volume", SUN4I_CODEC_DAC_ACTL,
		       SUN4I_CODEC_DAC_ACTL_FMG, 3, 0,
		       sun4i_codec_fmin_loopback_gain_scale),
	SOC_SINGLE_TLV("Mic Playback Volume", SUN4I_CODEC_DAC_ACTL,
		       SUN4I_CODEC_DAC_ACTL_MICG, 7, 0,
		       sun4i_codec_micin_loopback_gain_scale),
	SOC_SINGLE_TLV("Mic1 Boost Volume", SUN4I_CODEC_ADC_ACTL,
		       SUN4I_CODEC_ADC_ACTL_PREG1, 3, 0,
		       sun4i_codec_micin_preamp_gain_scale),
	SOC_SINGLE_TLV("Mic2 Boost Volume", SUN4I_CODEC_ADC_ACTL,
		       SUN4I_CODEC_ADC_ACTL_PREG2, 3, 0,
		       sun4i_codec_micin_preamp_gain_scale),
};

static const struct snd_kcontrol_new sun7i_codec_controls[] = {
	SOC_SINGLE_TLV("Power Amplifier Volume", SUN4I_CODEC_DAC_ACTL,
		       SUN4I_CODEC_DAC_ACTL_PA_VOL, 0x3F, 0,
		       sun4i_codec_pa_volume_scale),
	SOC_SINGLE_TLV("Line Playback Volume", SUN4I_CODEC_DAC_ACTL,
		       SUN4I_CODEC_DAC_ACTL_LNG, 1, 0,
		       sun4i_codec_linein_loopback_gain_scale),
	SOC_SINGLE_TLV("Line Boost Volume", SUN4I_CODEC_ADC_ACTL,
		       SUN4I_CODEC_ADC_ACTL_LNPREG, 7, 0,
		       sun4i_codec_linein_preamp_gain_scale),
	SOC_SINGLE_TLV("FM Playback Volume", SUN4I_CODEC_DAC_ACTL,
		       SUN4I_CODEC_DAC_ACTL_FMG, 3, 0,
		       sun4i_codec_fmin_loopback_gain_scale),
	SOC_SINGLE_TLV("Mic Playback Volume", SUN4I_CODEC_DAC_ACTL,
		       SUN4I_CODEC_DAC_ACTL_MICG, 7, 0,
		       sun4i_codec_micin_loopback_gain_scale),
	SOC_SINGLE_TLV("Mic1 Boost Volume", SUN7I_CODEC_AC_MIC_PHONE_CAL,
		       SUN7I_CODEC_AC_MIC_PHONE_CAL_PREG1, 7, 0,
		       sun7i_codec_micin_preamp_gain_scale),
	SOC_SINGLE_TLV("Mic2 Boost Volume", SUN7I_CODEC_AC_MIC_PHONE_CAL,
		       SUN7I_CODEC_AC_MIC_PHONE_CAL_PREG2, 7, 0,
		       sun7i_codec_micin_preamp_gain_scale),
};

static const struct snd_kcontrol_new sun4i_codec_mixer_controls[] = {
	SOC_DAPM_SINGLE("Left Mixer Left DAC Playback Switch",
			SUN4I_CODEC_DAC_ACTL, SUN4I_CODEC_DAC_ACTL_LDACLMIXS,
			1, 0),
	SOC_DAPM_SINGLE("Right Mixer Right DAC Playback Switch",
			SUN4I_CODEC_DAC_ACTL, SUN4I_CODEC_DAC_ACTL_RDACRMIXS,
			1, 0),
	SOC_DAPM_SINGLE("Right Mixer Left DAC Playback Switch",
			SUN4I_CODEC_DAC_ACTL,
			SUN4I_CODEC_DAC_ACTL_LDACRMIXS, 1, 0),
	SOC_DAPM_DOUBLE("Line Playback Switch", SUN4I_CODEC_DAC_ACTL,
			SUN4I_CODEC_DAC_ACTL_LLNS,
			SUN4I_CODEC_DAC_ACTL_RLNS, 1, 0),
	SOC_DAPM_DOUBLE("FM Playback Switch", SUN4I_CODEC_DAC_ACTL,
			SUN4I_CODEC_DAC_ACTL_LFMS,
			SUN4I_CODEC_DAC_ACTL_RFMS, 1, 0),
	SOC_DAPM_DOUBLE("Mic1 Playback Switch", SUN4I_CODEC_DAC_ACTL,
			SUN4I_CODEC_DAC_ACTL_MIC1LS,
			SUN4I_CODEC_DAC_ACTL_MIC1RS, 1, 0),
	SOC_DAPM_DOUBLE("Mic2 Playback Switch", SUN4I_CODEC_DAC_ACTL,
			SUN4I_CODEC_DAC_ACTL_MIC2LS,
			SUN4I_CODEC_DAC_ACTL_MIC2RS, 1, 0),
};

static const struct snd_kcontrol_new sun4i_codec_pa_mixer_controls[] = {
	SOC_DAPM_SINGLE("DAC Playback Switch", SUN4I_CODEC_DAC_ACTL,
			SUN4I_CODEC_DAC_ACTL_DACPAS, 1, 0),
	SOC_DAPM_SINGLE("Mixer Playback Switch", SUN4I_CODEC_DAC_ACTL,
			SUN4I_CODEC_DAC_ACTL_MIXPAS, 1, 0),
};

static const struct snd_soc_dapm_widget sun4i_codec_codec_dapm_widgets[] = {
	/* Digital parts of the ADCs */
	SND_SOC_DAPM_SUPPLY("ADC", SUN4I_CODEC_ADC_FIFOC,
			    SUN4I_CODEC_ADC_FIFOC_EN_AD, 0,
			    NULL, 0),

	/* Digital parts of the DACs */
	SND_SOC_DAPM_SUPPLY("DAC", SUN4I_CODEC_DAC_DPC,
			    SUN4I_CODEC_DAC_DPC_EN_DA, 0,
			    NULL, 0),

	/* Analog parts of the ADCs */
	SND_SOC_DAPM_ADC("Left ADC", "Codec Capture", SUN4I_CODEC_ADC_ACTL,
			 SUN4I_CODEC_ADC_ACTL_ADC_L_EN, 0),
	SND_SOC_DAPM_ADC("Right ADC", "Codec Capture", SUN4I_CODEC_ADC_ACTL,
			 SUN4I_CODEC_ADC_ACTL_ADC_R_EN, 0),

	/* Analog parts of the DACs */
	SND_SOC_DAPM_DAC("Left DAC", "Codec Playback", SUN4I_CODEC_DAC_ACTL,
			 SUN4I_CODEC_DAC_ACTL_DACAENL, 0),
	SND_SOC_DAPM_DAC("Right DAC", "Codec Playback", SUN4I_CODEC_DAC_ACTL,
			 SUN4I_CODEC_DAC_ACTL_DACAENR, 0),

	/* Mixers */
	SND_SOC_DAPM_MIXER("Left Mixer", SND_SOC_NOPM, 0, 0,
			   sun4i_codec_mixer_controls,
			   ARRAY_SIZE(sun4i_codec_mixer_controls)),
	SND_SOC_DAPM_MIXER("Right Mixer", SND_SOC_NOPM, 0, 0,
			   sun4i_codec_mixer_controls,
			   ARRAY_SIZE(sun4i_codec_mixer_controls)),

	/* Global Mixer Enable */
	SND_SOC_DAPM_SUPPLY("Mixer Enable", SUN4I_CODEC_DAC_ACTL,
			    SUN4I_CODEC_DAC_ACTL_MIXEN, 0, NULL, 0),

	/* VMIC */
	SND_SOC_DAPM_SUPPLY("VMIC", SUN4I_CODEC_ADC_ACTL,
			    SUN4I_CODEC_ADC_ACTL_VMICEN, 0, NULL, 0),

	/* Mic Pre-Amplifiers */
	SND_SOC_DAPM_PGA("MIC1 Pre-Amplifier", SUN4I_CODEC_ADC_ACTL,
			 SUN4I_CODEC_ADC_ACTL_PREG1EN, 0, NULL, 0),
	SND_SOC_DAPM_PGA("MIC2 Pre-Amplifier", SUN4I_CODEC_ADC_ACTL,
			 SUN4I_CODEC_ADC_ACTL_PREG2EN, 0, NULL, 0),

	/* Power Amplifier */
	SND_SOC_DAPM_MIXER("Power Amplifier", SUN4I_CODEC_ADC_ACTL,
			   SUN4I_CODEC_ADC_ACTL_PA_EN, 0,
			   sun4i_codec_pa_mixer_controls,
			   ARRAY_SIZE(sun4i_codec_pa_mixer_controls)),
	SND_SOC_DAPM_SWITCH("Power Amplifier Mute", SND_SOC_NOPM, 0, 0,
			    &sun4i_codec_pa_mute),

	SND_SOC_DAPM_INPUT("Line Right"),
	SND_SOC_DAPM_INPUT("Line Left"),
	SND_SOC_DAPM_INPUT("FM Right"),
	SND_SOC_DAPM_INPUT("FM Left"),
	SND_SOC_DAPM_INPUT("Mic1"),
	SND_SOC_DAPM_INPUT("Mic2"),

	SND_SOC_DAPM_OUTPUT("HP Right"),
	SND_SOC_DAPM_OUTPUT("HP Left"),
};

static const struct snd_soc_dapm_route sun4i_codec_codec_dapm_routes[] = {
	/* Left ADC / DAC Routes */
	{ "Left ADC", NULL, "ADC" },
	{ "Left DAC", NULL, "DAC" },

	/* Right ADC / DAC Routes */
	{ "Right ADC", NULL, "ADC" },
	{ "Right DAC", NULL, "DAC" },

	/* Right Mixer Routes */
	{ "Right Mixer", NULL, "Mixer Enable" },
	{ "Right Mixer", "Right Mixer Left DAC Playback Switch", "Left DAC" },
	{ "Right Mixer", "Right Mixer Right DAC Playback Switch", "Right DAC" },
	{ "Right Mixer", "Line Playback Switch", "Line Right" },
	{ "Right Mixer", "FM Playback Switch", "FM Right" },
	{ "Right Mixer", "Mic1 Playback Switch", "MIC1 Pre-Amplifier" },
	{ "Right Mixer", "Mic2 Playback Switch", "MIC2 Pre-Amplifier" },

	/* Left Mixer Routes */
	{ "Left Mixer", NULL, "Mixer Enable" },
	{ "Left Mixer", "Left Mixer Left DAC Playback Switch", "Left DAC" },
	{ "Left Mixer", "Line Playback Switch", "Line Left" },
	{ "Left Mixer", "FM Playback Switch", "FM Left" },
	{ "Left Mixer", "Mic1 Playback Switch", "MIC1 Pre-Amplifier" },
	{ "Left Mixer", "Mic2 Playback Switch", "MIC2 Pre-Amplifier" },

	/* Power Amplifier Routes */
	{ "Power Amplifier", "Mixer Playback Switch", "Left Mixer" },
	{ "Power Amplifier", "Mixer Playback Switch", "Right Mixer" },
	{ "Power Amplifier", "DAC Playback Switch", "Left DAC" },
	{ "Power Amplifier", "DAC Playback Switch", "Right DAC" },

	/* Headphone Output Routes */
	{ "Power Amplifier Mute", "Switch", "Power Amplifier" },
	{ "HP Right", NULL, "Power Amplifier Mute" },
	{ "HP Left", NULL, "Power Amplifier Mute" },

	/* Mic1 Routes */
	{ "Left ADC", NULL, "MIC1 Pre-Amplifier" },
	{ "Right ADC", NULL, "MIC1 Pre-Amplifier" },
	{ "MIC1 Pre-Amplifier", NULL, "Mic1"},
	{ "Mic1", NULL, "VMIC" },

	/* Mic2 Routes */
	{ "Left ADC", NULL, "MIC2 Pre-Amplifier" },
	{ "Right ADC", NULL, "MIC2 Pre-Amplifier" },
	{ "MIC2 Pre-Amplifier", NULL, "Mic2"},
	{ "Mic2", NULL, "VMIC" },
};

static const DECLARE_TLV_DB_SCALE(sun20i_d1_codec_dvol_scale, -12000, 75, 1);

static const struct snd_kcontrol_new sun20i_d1_codec_codec_controls[] = {
	SOC_SINGLE_TLV("DAC Playback Volume", SUN4I_CODEC_DAC_DPC,
		       SUN4I_CODEC_DAC_DPC_DVOL, 0x3f, 1,
		       sun20i_d1_codec_dvol_scale),
	SOC_DOUBLE_TLV("DAC Front Playback Volume", SUN20I_D1_CODEC_DAC_VOL_CTRL,
		       SUN20I_D1_CODEC_DAC_VOL_L, SUN20I_D1_CODEC_DAC_VOL_R,
		       0xFF, 0, sun20i_d1_codec_dvol_scale),

	SOC_SINGLE_TLV("ADC1 Capture Volume", SUN20I_D1_CODEC_ADC_VOL_CTRL1,
		       SUN20I_D1_CODEC_ADC_VOL_CTRL1_ADC1_VOL, 0xff, 0,
		       sun20i_d1_codec_dvol_scale),
	SOC_SINGLE_TLV("ADC2 Capture Volume", SUN20I_D1_CODEC_ADC_VOL_CTRL1,
		       SUN20I_D1_CODEC_ADC_VOL_CTRL1_ADC2_VOL, 0xff, 0,
		       sun20i_d1_codec_dvol_scale),
	SOC_SINGLE_TLV("ADC3 Capture Volume", SUN20I_D1_CODEC_ADC_VOL_CTRL1,
		       SUN20I_D1_CODEC_ADC_VOL_CTRL1_ADC3_VOL, 0xff, 0,
		       sun20i_d1_codec_dvol_scale),
};

static const struct snd_soc_dapm_widget sun20i_d1_codec_codec_widgets[] = {
	/* Digital parts of the ADCs */
	SND_SOC_DAPM_SUPPLY("ADC Enable", SUN20I_D1_CODEC_ADC_FIFOC,
			    SUN20I_D1_CODEC_ADC_FIFOC_EN_AD, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("ADC1 CH Enable", SUN20I_D1_CODEC_ADC_DIG_CTRL,
			    SUN20I_D1_CODEC_ADC_DIG_CTRL_ADC1_CH_EN, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("ADC2 CH Enable", SUN20I_D1_CODEC_ADC_DIG_CTRL,
			    SUN20I_D1_CODEC_ADC_DIG_CTRL_ADC2_CH_EN, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("ADC3 CH Enable", SUN20I_D1_CODEC_ADC_DIG_CTRL,
			    SUN20I_D1_CODEC_ADC_DIG_CTRL_ADC3_CH_EN, 0, NULL, 0),
	/* Widget ADC digital para activar supplies pero sin stream_name */
	SND_SOC_DAPM_ADC("Digital ADC", NULL, SND_SOC_NOPM, 0, 0),
	/* Endpoint de entrada desde el dominio analógico */
	SND_SOC_DAPM_INPUT("Codec In"),
	/* AIF de captura digital - endpoint del stream PCM */
	SND_SOC_DAPM_AIF_OUT("Capture AIF", "Codec Capture", 0, SND_SOC_NOPM, 0, 0),
	/* Suministro de mic bias y preamp (equivalentes a variante legacy) */
	SND_SOC_DAPM_SUPPLY("VMIC", SUN4I_CODEC_ADC_ACTL,
			    SUN4I_CODEC_ADC_ACTL_VMICEN, 0, NULL, 0),
	SND_SOC_DAPM_PGA("MIC1 Pre-Amplifier", SUN4I_CODEC_ADC_ACTL,
			 SUN4I_CODEC_ADC_ACTL_PREG1EN, 0, NULL, 0),
	SND_SOC_DAPM_INPUT("Mic1"),
	/* (Placeholder) Mic3 analógico vive en driver analog; aquí sólo enlazamos digitalmente */
	/* Digital parts of the DACs */
	SND_SOC_DAPM_SUPPLY("DAC Enable", SUN4I_CODEC_DAC_DPC,
			    SUN4I_CODEC_DAC_DPC_EN_DA, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("DAC VOL_SEL Enable", SUN20I_D1_CODEC_DAC_VOL_CTRL,
			    SUN20I_D1_CODEC_DAC_VOL_SEL, 0, NULL, 0),
	/* Nuevo nodo intermedio para que DAPM tenga un sink digital real antes de cruzar
	 * al componente analógico. El stream PCM engancha aquí; las supplies deben
	 * encenderse cuando este DAC digital se active.
	 */
	SND_SOC_DAPM_DAC("Digital DAC", "Codec Playback", SND_SOC_NOPM, 0, 0),
	/* Endpoint de salida digital interno */
	SND_SOC_DAPM_OUTPUT("Codec Out"),
	/* AIF explícito para playback: ayuda a DAPM a establecer dependencia */
	SND_SOC_DAPM_AIF_OUT("Playback AIF", "Codec Playback", 0, SND_SOC_NOPM, 0, 0),
};

static const struct snd_soc_dapm_route sun20i_d1_codec_codec_routes[] = {
	/* Rutas de playback */
	{ "Codec Out", NULL, "Digital DAC" },
	{ "Digital DAC", NULL, "Playback AIF" },
	
	/* Rutas internas de captura: entrada -> ADC -> supplies -> AIF */
	{ "Digital ADC", NULL, "Codec In" },
	{ "Digital ADC", NULL, "ADC Enable" },
	{ "Digital ADC", NULL, "ADC3 CH Enable" },
	{ "Capture AIF", NULL, "Digital ADC" },
};

static const struct snd_soc_component_driver sun4i_codec_codec = {
	.controls		= sun4i_codec_controls,
	.num_controls		= ARRAY_SIZE(sun4i_codec_controls),
	.dapm_widgets		= sun4i_codec_codec_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(sun4i_codec_codec_dapm_widgets),
	.dapm_routes		= sun4i_codec_codec_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(sun4i_codec_codec_dapm_routes),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 0,
};

static const struct snd_soc_component_driver sun20i_d1_codec_codec = {
	.controls		= sun20i_d1_codec_codec_controls,
	.num_controls		= ARRAY_SIZE(sun20i_d1_codec_codec_controls),
	.dapm_widgets		= sun20i_d1_codec_codec_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(sun20i_d1_codec_codec_widgets),
	.dapm_routes		= sun20i_d1_codec_codec_routes,
	.num_dapm_routes	= ARRAY_SIZE(sun20i_d1_codec_codec_routes),
	.idle_bias_on		= 1,
	.use_pmdown_time	= 1,
	.endianness		= 0,
};

static const struct regmap_config sun20i_d1_codec_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= SUN20I_D1_CODEC_VRA1SPEEDUP_DOWN_CTRL,
};

/* Legacy quirks removed - D1 focused implementation */

static const struct sun4i_codec_quirks sun20i_d1_codec_quirks = {
	.regmap_config	= &sun20i_d1_codec_regmap_config,
	.codec		= &sun20i_d1_codec_codec,
	.create_card	= sun20i_d1_codec_create_card,
	.reg_dac_fifoc	= REG_FIELD(SUN20I_D1_CODEC_DAC_FIFOC, 0, 31),
	.reg_adc_fifoc	= REG_FIELD(SUN20I_D1_CODEC_ADC_FIFOC, 0, 31),
	.adc_drq_en	= SUN20I_D1_CODEC_ADC_FIFOC_ADC_DRQ_EN,
	.rx_sample_bits	= SUN20I_D1_CODEC_ADC_FIFOC_RX_SAMPLE_BITS,
	.rx_trig_level	= SUN20I_D1_CODEC_ADC_FIFOC_RX_TRIG_LEVEL,
	/* D1: TXDATA se encuentra en 0x20 (verificado en reproducción estable). */
	.reg_dac_txdata	= 0x20,
	.reg_adc_rxdata	= SUN20I_D1_CODEC_ADC_RXDATA,
	.has_reset	= true,
	.has_dual_clock = true,
	.dma_max_burst	= SUN4I_DMA_MAX_BURST,
};

static const struct snd_soc_dapm_route sun20i_d1_codec_card_routes[] = {
	/* Playback path: Digital DAC -> analog DACs */
	{ "Digital DAC", NULL, "DAC Enable" },
	{ "Digital DAC", NULL, "DAC VOL_SEL Enable" },
	{ "Digital DAC", NULL, "Codec Playback" },
	{ "Left DAC", NULL, "Codec Out" },
	{ "Right DAC", NULL, "Codec Out" },
	{ "Speaker", NULL, "Left DAC" },
	{ "Speaker", NULL, "Right DAC" },
	{ "Speaker", NULL, "RAMP Enable" },

	/* Capture path: ADC3 -> Digital ADC */
	{ "MIC1 Pre-Amplifier", NULL, "Mic1" },
	{ "Mic1", NULL, "VMIC" },
	{ "ADC1", NULL, "MIC1 Pre-Amplifier" },
	{ "Codec In", NULL, "ADC3" },
	{ "ADC3", NULL, "Mic3 Amplifier" },
	{ "Mic3 Amplifier", NULL, "MBIAS" },
};

/* Card-level DAPM routes connecting digital and analog components */

static struct snd_soc_dai_link *sun4i_codec_create_link(struct device *dev, int *num_links)
{
	struct snd_soc_dai_link *link;
	struct snd_soc_dai_link_component *cpus;
	struct snd_soc_dai_link_component *codecs;
	struct snd_soc_dai_link_component *platforms;

	link = devm_kzalloc(dev, sizeof(*link), GFP_KERNEL);
	if (!link)
		return NULL;

	link->name = "Codec";
	link->stream_name = "Codec";
	link->dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBP_CFC;

	/*
	 * El driver original minimalista solo rellenaba name/stream_name/dai_fmt.
	 * Las versiones recientes del core ASoC esperan que se configuren los
	 * arrays cpus/codecs/platforms (multi-component). La ausencia de estos
	 * punteros provoca un NULL deref en snd_soc_runtime_set_dai_fmt.
	 *
	 * El codec interno expone un único DAI llamado "Codec"; usamos el mismo
	 * of_node (este dispositivo) tanto para CPU DAI como para CODEC DAI.
	 * Esto es aceptable para un codec interno integrado (pattern utilizado
	 * en otros drivers legacy migrados). Si más adelante se desea separar
	 * CPU y CODEC, se podrá ajustar aquí.
	 */
	cpus = devm_kzalloc(dev, sizeof(*cpus), GFP_KERNEL);
	codecs = devm_kzalloc(dev, sizeof(*codecs), GFP_KERNEL);
	platforms = devm_kzalloc(dev, sizeof(*platforms), GFP_KERNEL);
	if (!cpus || !codecs || !platforms)
		return NULL;

	/* CPU side */
	cpus[0].of_node = dev->of_node;
	cpus[0].dai_name = "Codec"; /* nombre del DAI expuesto por sun4i_codec_dai */
	link->cpus = cpus;
	link->num_cpus = 1;

	/* CODEC side */
	codecs[0].of_node = dev->of_node;
	codecs[0].dai_name = "Codec";
	link->codecs = codecs;
	link->num_codecs = 1;

	/* Platform (DMA / PCM) provider: mismo nodo */
	platforms[0].of_node = dev->of_node;
	link->platforms = platforms;
	link->num_platforms = 1;

	/* Link totalmente enrutado dentro del codec interno */

	*num_links = 1;
	return link;
}

static const struct of_device_id sun4i_codec_of_match[] = {
	{
		.compatible = "allwinner,sun20i-d1-codec",
		.data = &sun20i_d1_codec_quirks,
	},
	{}
};
MODULE_DEVICE_TABLE(of, sun4i_codec_of_match);

static int sun4i_codec_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct sun4i_codec *scodec;
	const struct sun4i_codec_quirks *quirks;
	struct resource *res;
	void __iomem *base;
	int ret;

	scodec = devm_kzalloc(&pdev->dev, sizeof(*scodec), GFP_KERNEL);
	if (!scodec)
		return -ENOMEM;

	scodec->dev = &pdev->dev;

	base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	quirks = of_device_get_match_data(&pdev->dev);
	if (quirks == NULL) {
		dev_err(&pdev->dev, "Failed to determine the quirks to use\n");
		return -ENODEV;
	}

	scodec->quirks = quirks;

	scodec->regmap = devm_regmap_init_mmio(&pdev->dev, base,
					       quirks->regmap_config);
	if (IS_ERR(scodec->regmap)) {
		dev_err(&pdev->dev, "Failed to create our regmap\n");
		return PTR_ERR(scodec->regmap);
	}

	/* Get the clocks from the DT */
	scodec->clk_apb = devm_clk_get_enabled(&pdev->dev, "apb");
	if (IS_ERR(scodec->clk_apb)) {
		dev_err(&pdev->dev, "Failed to get the APB clock\n");
		return PTR_ERR(scodec->clk_apb);
	}

	if (quirks->has_dual_clock) {
		scodec->clk_module = devm_clk_get(&pdev->dev, "adc");
		if (IS_ERR(scodec->clk_module)) {
			dev_err(&pdev->dev, "Failed to get the ADC module clock\n");
			return PTR_ERR(scodec->clk_module);
		}

		scodec->clk_module_dac = devm_clk_get(&pdev->dev, "dac");
		if (IS_ERR(scodec->clk_module_dac)) {
			dev_err(&pdev->dev, "Failed to get the DAC module clock\n");
			return PTR_ERR(scodec->clk_module_dac);
		}
	} else {
		scodec->clk_module = devm_clk_get(&pdev->dev, "codec");
		if (IS_ERR(scodec->clk_module)) {
			dev_err(&pdev->dev, "Failed to get the module clock\n");
			return PTR_ERR(scodec->clk_module);
		}
	}

	if (quirks->has_reset) {
		scodec->rst = devm_reset_control_get_exclusive_deasserted(&pdev->dev, NULL);
		if (IS_ERR(scodec->rst)) {
			dev_err(&pdev->dev, "Failed to get reset control\n");
			return PTR_ERR(scodec->rst);
		}
	}

	scodec->gpio_pa = devm_gpiod_get_optional(&pdev->dev, "allwinner,pa",
						  GPIOD_OUT_LOW);
	if (IS_ERR(scodec->gpio_pa)) {
		ret = PTR_ERR(scodec->gpio_pa);
		dev_err_probe(&pdev->dev, ret, "Failed to get pa gpio\n");
		return ret;
	}

	scodec->gpio_hp = devm_gpiod_get_optional(&pdev->dev, "hp-det", GPIOD_IN);
	if (IS_ERR(scodec->gpio_hp)) {
		ret = PTR_ERR(scodec->gpio_hp);
		dev_err_probe(&pdev->dev, ret, "Failed to get hp-det gpio\n");
		return ret;
	}

	/* reg_field setup */
	scodec->reg_adc_fifoc = devm_regmap_field_alloc(&pdev->dev,
							scodec->regmap,
							quirks->reg_adc_fifoc);
	if (IS_ERR(scodec->reg_adc_fifoc)) {
		ret = PTR_ERR(scodec->reg_adc_fifoc);
		dev_err(&pdev->dev, "Failed to create regmap fields: %d\n",
			ret);
		return ret;
	}

	scodec->reg_dac_fifoc = devm_regmap_field_alloc(&pdev->dev,
							scodec->regmap,
							quirks->reg_dac_fifoc);
	if (IS_ERR(scodec->reg_dac_fifoc)) {
		ret = PTR_ERR(scodec->reg_dac_fifoc);
		dev_err(&pdev->dev, "Failed to create regmap fields: %d\n",
			ret);
		return ret;
	}

	/* DMA configuration for TX FIFO */
	scodec->ofs_txdata = quirks->reg_dac_txdata;
	scodec->playback_dma_data.addr = res->start + scodec->ofs_txdata;
	scodec->playback_dma_data.maxburst = quirks->dma_max_burst;
	scodec->playback_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;

	/* DMA configuration for RX FIFO */
	scodec->capture_dma_data.addr = res->start + quirks->reg_adc_rxdata;
	scodec->capture_dma_data.maxburst = quirks->dma_max_burst;
	scodec->capture_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;

	ret = devm_snd_soc_register_component(&pdev->dev, quirks->codec,
				     &sun4i_codec_dai, 1);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register our codec\n");
		return ret;
	}

	ret = devm_snd_dmaengine_pcm_register(&pdev->dev, &sun4i_codec_dmaengine_pcm_config, 0);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register against DMAEngine\n");
		return ret;
	}

	card = quirks->create_card(&pdev->dev);
	if (IS_ERR(card)) {
		ret = PTR_ERR(card);
		dev_err(&pdev->dev, "Failed to create our card\n");
		return ret;
	}

	snd_soc_card_set_drvdata(card, scodec);

	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "Failed to register our card\n");
		return ret;
	}

	return 0;
}

static void sun4i_codec_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);

	snd_soc_unregister_card(card);
}

/*
 * Callback para completar dma_slave_config antes de que el driver DMA lo valide.
 * Observamos que en reproducción (MEM_TO_DEV) dst_maxburst quedaba en 0 y fallaba
 * la comprobación (bit 0 no soportado). Forzamos que src y dst compartan los
 * parámetros de burst y ancho definidos en playback/capture_dma_data.
 */
static int sun4i_codec_dma_prepare_slave_config(struct snd_pcm_substream *substream,
					     struct snd_pcm_hw_params *params,
					     struct dma_slave_config *slave_config)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sun4i_codec *scodec = snd_soc_card_get_drvdata(rtd->card);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (!slave_config->dst_addr)
			slave_config->dst_addr = scodec->playback_dma_data.addr;
		if (!slave_config->dst_addr_width)
			slave_config->dst_addr_width = scodec->playback_dma_data.addr_width;
		if (!slave_config->dst_maxburst)
			slave_config->dst_maxburst = scodec->playback_dma_data.maxburst;
		/* Asegura también un src_maxburst (>0) si quedara a cero */
		if (!slave_config->src_maxburst)
			slave_config->src_maxburst = scodec->playback_dma_data.maxburst;
		if (!slave_config->src_addr_width)
			slave_config->src_addr_width = scodec->playback_dma_data.addr_width;
	} else {
		if (!slave_config->src_addr)
			slave_config->src_addr = scodec->capture_dma_data.addr;
		if (!slave_config->src_addr_width)
			slave_config->src_addr_width = scodec->capture_dma_data.addr_width;
		if (!slave_config->src_maxburst)
			slave_config->src_maxburst = scodec->capture_dma_data.maxburst;
		if (!slave_config->dst_maxburst)
			slave_config->dst_maxburst = scodec->capture_dma_data.maxburst;
		if (!slave_config->dst_addr_width)
			slave_config->dst_addr_width = scodec->capture_dma_data.addr_width;
	}

	return 0;
}

static const struct snd_dmaengine_pcm_config sun4i_codec_dmaengine_pcm_config = {
	.prepare_slave_config = sun4i_codec_dma_prepare_slave_config,
};

static struct snd_soc_aux_dev aux_dev;

static struct snd_soc_card *sun20i_d1_codec_create_card(struct device *dev)
{
	struct snd_soc_card *card;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return ERR_PTR(-ENOMEM);

	aux_dev.dlc.of_node = of_parse_phandle(dev->of_node,
					       "allwinner,codec-analog-controls",
					       0);
	if (!aux_dev.dlc.of_node) {
		dev_err(dev, "Can't find analog controls for codec.\n");
		return ERR_PTR(-EINVAL);
	}

	card->dai_link = sun4i_codec_create_link(dev, &card->num_links);
	if (!card->dai_link)
		return ERR_PTR(-ENOMEM);

	card->dev		= dev;
	card->owner		= THIS_MODULE;
	card->name		= "D1 Audio Codec";
	card->dapm_widgets	= NULL;
	card->num_dapm_widgets	= 0;
	card->dapm_routes	= sun20i_d1_codec_card_routes;
	/* No añadimos rutas de tarjeta hasta confirmar nombres reales de widgets
	 * publicados por ambos componentes (ver debugfs DAPM). */
	card->num_dapm_routes	= ARRAY_SIZE(sun20i_d1_codec_card_routes);
	card->aux_dev		= &aux_dev;
	card->num_aux_devs	= 1;
	/* Usamos false para permitir que DAPM añada rutas implícitas entre los
	 * widgets del DAI y los endpoints recién añadidos (Playback AIF -> Digital DAC). */
	card->fully_routed	= false;

	/* Parse optional device tree audio routing */
	if (of_find_property(dev->of_node, "allwinner,audio-routing", NULL)) {
		int ret = snd_soc_of_parse_audio_routing(card, "allwinner,audio-routing");
		if (ret)
			dev_warn(dev, "failed to parse audio-routing: %d\n", ret);
	}

	return card;
}

static struct platform_driver sun4i_codec_driver = {
	.driver = {
		.name = "sun4i-codec",
		.of_match_table = sun4i_codec_of_match,
	},
	.probe = sun4i_codec_probe,
	.remove = sun4i_codec_remove,
};
module_platform_driver(sun4i_codec_driver);

MODULE_DESCRIPTION("Allwinner A10 codec driver");
MODULE_AUTHOR("Emilio López <emilio@elopez.com.ar>");
MODULE_AUTHOR("Jon Smirl <jonsmirl@gmail.com>");
MODULE_AUTHOR("Maxime Ripard <maxime.ripard@free-electrons.com>");
MODULE_AUTHOR("Chen-Yu Tsai <wens@csie.org>");
MODULE_AUTHOR("Ryan Walklin <ryan@testtoast.com");
MODULE_AUTHOR("Mesih Kilinc <mesikilinc@gmail.com>");
MODULE_LICENSE("GPL");
