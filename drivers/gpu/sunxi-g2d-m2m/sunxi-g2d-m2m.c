// SPDX-License-Identifier: GPL-2.0
//
// Minimal V4L2 mem2mem driver skeleton for Allwinner G2D
// Features MVP: XRGB8888 OUTPUT -> XRGB8888 CAPTURE, scale if WxH differ
// Author: tú+yomismo

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/pm_runtime.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/bitops.h>
#include <linux/string.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#define DRV_NAME "sunxi-g2d-m2m"

// G2D register map (v2 style) extracted from BSP
#include "sunxi-g2d-regs.h"

// ========= DT binding =========

/* Variant-specific configuration */
struct sunxi_g2d_variant {
	bool use_rcq;         /* Requires RCQ for sub-block access */
	u32 bias_subblocks;   /* Memory layout offset for RCQ mode */
};

static const struct sunxi_g2d_variant t113_variant = {
	.use_rcq = true,
	.bias_subblocks = 0x28000,  /* T113 RCQ layout */
};

static const struct sunxi_g2d_variant default_variant = {
	.use_rcq = false,
	.bias_subblocks = 0,  /* Legacy direct access */
};

static const struct of_device_id sunxi_g2d_of_match[] = {
	{ .compatible = "allwinner,t113-g2d", .data = &t113_variant },
	{ .compatible = "allwinner,sun8i-g2d", .data = &default_variant },
	{ .compatible = "allwinner,sun8i-g2d-v1", .data = &default_variant },
	{ .compatible = "allwinner,sun8i-g2d-v2", .data = &default_variant },
	{ .compatible = "allwinner,sunxi-g2d", .data = &default_variant },
	{}
};
MODULE_DEVICE_TABLE(of, sunxi_g2d_of_match);

// ========= HW regs base =========
// Tamaño de la ventana MMIO útil (TOP..VSU); evita solapar crypto (ver DTS)
#define G2D_REG_SIZE   0x1000

/* Forward declarations needed by early inline helpers */
struct sunxi_g2d_ctx;
struct sunxi_g2d_dev;

/* Define struct sunxi_g2d_dev early so inline helpers can access members */
struct sunxi_g2d_dev {
	struct device *dev;
	void __iomem *mmio;
	resource_size_t mmio_size;
	void __iomem *ccu;  /* T113: CCU base for G2D_CLK_REG/G2D_BGR_REG */
	bool use_rcq; /* T113 requires RCQ for sub-blocks */
	u8 reg_shift; /* 0 = layout pequeño (0x0100), 4 = layout grande (<<4) */
	/* Sesgos por bloque para layouts alternativos (T113): off_final = ((reg + bias_subblocks) + bias_block) << reg_shift */
	u32 bias_subblocks; /* sesgo global para MIXER/BLD/V0/WB si todos se desplazan juntos (p.ej., +0x28000) */
	u32 bias_mixer; /* aplicado cuando reg in [G2D_MIXER, G2D_MIXER+0x3FF] */
	u32 bias_bld;   /* aplicado cuando reg in [G2D_BLD  , G2D_BLD  +0x3FF] */
	u32 bias_v0;    /* aplicado cuando reg in [G2D_V0   , G2D_V0   +0x3FF] */
	u32 bias_wb;    /* aplicado cuando reg in [G2D_WB   , G2D_WB   +0x3FF] */
	struct clk *clk;
	struct clk *clk_mod;
	struct clk *clk_mbus;
	struct reset_control *rst;
	int irq;

	struct v4l2_device v4l2_dev;
	struct video_device vfd;
	struct v4l2_m2m_dev *m2m_dev;

	struct mutex dev_mutex; // serializa open/release
	spinlock_t irqlock;

	// job state
	struct sunxi_g2d_ctx *curr_ctx; // contexto en ejecución

// detección de HW no operativo (mapa de registros incorrecto/clock)
	bool hw_broken;
};

static inline bool g2d_is_stub_value(u32 val)
{
	return val == 0x31400102;
}

/* MMIO helpers */
static inline void __g2d_writel(void __iomem *mmio, u32 val, u32 reg)
{
	iowrite32(val, mmio + reg);
}

static inline u32 __g2d_readl(void __iomem *mmio, u32 reg)
{
	return ioread32(mmio + reg);
}

/* Calcula el offset final (con bias/shift) para registros del engine.
 * Reglas:
 *  - TOP gates (SCLK_GATE/HCLK_GATE/AHB_RESET/SCLK_DIV) sin bias
 *  - Sub-bloques (MIXER/BLD/V0/WB) con bias_subblocks y bias por bloque
 *  - Registros de core (CLK_REG/BGR_REG) SIN bias (pertenecen a TOP)
 */
static inline u32 g2d_calc_off(struct sunxi_g2d_dev *g2d, u32 reg)
{
	u32 off = reg;
	bool is_top_gate = (reg == G2D_SCLK_GATE) || (reg == G2D_HCLK_GATE) ||
			   (reg == G2D_AHB_RESET) || (reg == G2D_SCLK_DIV);
	bool is_mixer = (reg >= G2D_MIXER && reg < (G2D_MIXER + 0x400));
	bool is_bld   = (reg >= G2D_BLD   && reg < (G2D_BLD   + 0x400));
	bool is_v0    = (reg >= G2D_V0    && reg < (G2D_V0    + 0x400));
	bool is_wb    = (reg >= G2D_WB    && reg < (G2D_WB    + 0x400));
	bool is_core  = (reg == G2D_CLK_REG) || (reg == G2D_BGR_REG);

	/* Aplica sesgo solo a sub-bloques; nunca a TOP ni a registros de core */
	if (!is_top_gate && !is_core && (is_mixer || is_bld || is_v0 || is_wb)) {
		off += g2d->bias_subblocks;
		if (is_mixer)
			off += g2d->bias_mixer;
		else if (is_bld)
			off += g2d->bias_bld;
		else if (is_v0)
			off += g2d->bias_v0;
		else if (is_wb)
			off += g2d->bias_wb;
		/* is_core: sin bias */
	}

	return off << g2d->reg_shift;
}

static inline void g2d_writel_dev(struct sunxi_g2d_dev *g2d, u32 val, u32 reg)
{
	u32 off = g2d_calc_off(g2d, reg);
	if (off >= g2d->mmio_size) {
		dev_warn_ratelimited(g2d->dev,
			"MMIO write OOB: reg=0x%04x final_off=0x%08x size=0x%llx val=0x%08x\n",
			reg, off, (unsigned long long)g2d->mmio_size, val);
		/* Marca HW como roto para evitar más intentos peligrosos */
		g2d->hw_broken = true;
		return;
	}
	/* Debug: log de escrituras críticas */
	if (reg == G2D_MIXER_CLK || reg == V0_ATTCTL || reg == BLD_EN_CTL || reg == WB_ATT) {
		dev_info(g2d->dev, "WRITE: reg=0x%04x off=0x%05x val=0x%08x\n", reg, off, val);
	}
	__g2d_writel(g2d->mmio, val, off);
}

static inline u32 g2d_readl_dev(struct sunxi_g2d_dev *g2d, u32 reg)
{
	u32 off = g2d_calc_off(g2d, reg);
	u32 val;
	if (off >= g2d->mmio_size) {
		dev_warn_ratelimited(g2d->dev,
			"MMIO read OOB: reg=0x%04x final_off=0x%08x size=0x%llx\n",
			reg, off, (unsigned long long)g2d->mmio_size);
		return 0;
	}
	val = __g2d_readl(g2d->mmio, off);
	/* Debug: log de lecturas críticas */
	if (reg == G2D_MIXER_CLK || reg == V0_ATTCTL || reg == BLD_EN_CTL || reg == WB_ATT) {
		dev_info(g2d->dev, "READ: reg=0x%04x off=0x%05x val=0x%08x\n", reg, off, val);
	}
	return val;
}

/* Program the RCQ header pointer/length pair.
 * Mirrors the legacy BSP helper g2d_top_set_rcq_head().
 */
static inline void g2d_rcq_load_head(struct sunxi_g2d_dev *g2d,
	dma_addr_t dma, u32 len_bytes)
{
	u32 len = len_bytes & G2D_RCQ_HEAD_LEN_MASK;

	g2d_writel_dev(g2d, lower_32_bits(dma), G2D_RCQ_HEAD_LOW);
	g2d_writel_dev(g2d, upper_32_bits(dma), G2D_RCQ_HEAD_HIGH);
	g2d_writel_dev(g2d, len, G2D_RCQ_HEAD_LEN);
}

struct g2d_rcq_hw_head {
	u32 low_addr;
	u32 len_high_addr; /* len[23:0], high_addr[31:24] */
	u32 dirty_next_len; /* dirty bit0, next header len[31:16] */
	u32 reg_offset;
} __packed;

#define G2D_RCQ_DIRTY_BIT	BIT(0)
#define G2D_RCQ_LEN_MASK24	GENMASK(23, 0)

#define G2D_RCQ_ALIGN32(x) ALIGN(x, 32)
#define G2D_RCQ_HEADER_ALIGN(x) ALIGN(x, 2)

/* Prueba fillrect: rellena un buffer de 32x32 con color sólido usando MIXER directo 
 * NOTA: En T113 no funciona - el hardware solo responde a RCQ, ver g2d_hw_fillrect_rcq()
 */
static void __maybe_unused g2d_hw_fillrect(struct platform_device *pdev,
	struct sunxi_g2d_dev *g2d)
{
	const u32 test_w = 32;
	const u32 test_h = 32;
	const u32 bytes_per_pixel = 4; /* ARGB8888 */
	const u32 pitch = test_w * bytes_per_pixel;
	const size_t surf_bytes = pitch * test_h;
	/* Formato correcto según enum g2d_fmt_hw_id en sunxi_g2d_hw.h:
	 * G2D_FORMAT_ARGB8888 = 0x00, G2D_FORMAT_XRGB8888 = 0x04
	 * 0x08 = G2D_FORMAT_RGB888 (24-bit packed) - INCORRECTO para fillcolor 32-bit!
	 */
	const u32 fmt_v0 = 0x04; /* G2D_FORMAT_XRGB8888 - formato 32-bit correcto */
	const u32 fmt_wb = 0x04; /* G2D_FORMAT_XRGB8888 - mismo formato que V0 */
	const u32 fill_color = 0xFF00FF00; /* Verde brillante ARGB */
	void *dst_surface = NULL;
	dma_addr_t dst_dma = 0;
	u32 size_word = (test_w - 1) | ((test_h - 1) << 16);
	u32 v0_att = V0_ATTCTL_EN | (fmt_v0 << V0_ATTCTL_FMT_SHIFT) | V0_ATTCTL_FILLCOLOR_EN;
	u32 wb_att = fmt_wb; /* BSP escribe solo formato, sin enable bit explícito */
	u32 wb_hadd;
	u32 status_before, status_after;
	unsigned long timeout;

	dst_surface = dmam_alloc_coherent(g2d->dev, surf_bytes, &dst_dma, GFP_KERNEL);
	if (!dst_surface) {
		dev_warn(&pdev->dev, "fillrect: sin superficie destino (%zu bytes)\n", surf_bytes);
		return;
	}
	memset(dst_surface, 0x00, surf_bytes);
	wb_hadd = (u32)((dst_dma >> 24) & 0xFF);

	dev_info(&pdev->dev, "fillrect: bias_subblocks=0x%05x reg_shift=%u\n",
		 g2d->bias_subblocks, g2d->reg_shift);

	/* Reset completo como BSP g2d_bsp_reset() - ambos MIXER y ROT */
	__g2d_writel(g2d->mmio, 0x0, G2D_AHB_RESET);  /* Assert reset MIXER+ROT */
	udelay(10);  /* Delay más largo durante assert */
	__g2d_writel(g2d->mmio, 0x3, G2D_AHB_RESET);  /* De-assert reset */
	udelay(100); /* Delay más largo tras de-assert para estabilización */
	dev_info(&pdev->dev, "fillrect: G2D reset completo (AHB_RST 0→3, delays 10us+100us)\n");

	/* CRÍTICO: Configurar MIXER_CLK después del reset */
	g2d_writel_dev(g2d, 0x1, G2D_MIXER_CLK);
	dev_info(&pdev->dev, "fillrect: MIXER_CLK=0x%08x\n", 
		 g2d_readl_dev(g2d, G2D_MIXER_CLK));

	/* CRÍTICO: Deshabilitar RCQ para forzar modo directo (register writes)
	 * Si RCQ está activo, hardware ignora escrituras de registros y espera command queue.
	 * BSP no usa RCQ para fillrectangle, escribe registros directamente.
	 */
	__g2d_writel(g2d->mmio, 0x0, G2D_RCQ_CTRL);      /* Disable RCQ */
	__g2d_writel(g2d->mmio, 0x0, G2D_RCQ_HEAD_LOW);  /* Clear head pointer */
	__g2d_writel(g2d->mmio, 0x0, G2D_RCQ_HEAD_HIGH); /* Clear head pointer */
	__g2d_writel(g2d->mmio, 0x0, G2D_RCQ_HEAD_LEN);  /* Clear command length */
	__g2d_writel(g2d->mmio, 0x0, G2D_RCQ_IRQ_CTL);   /* Disable RCQ IRQ routing */
	dev_info(&pdev->dev, "fillrect: RCQ disabled (modo directo - register writes)\n");

	/* BSP NO toca MIXER_CLK - tal vez no existe en T113, los gates TOP son suficientes */

	/* Configura Video Layer 0 con fillcolor */
	g2d_writel_dev(g2d, fill_color, V0_FILLC);
	dev_info(&pdev->dev, "fillrect: V0_FILLC=0x%08x @ off=0x%05x\n",
		 g2d_readl_dev(g2d, V0_FILLC), g2d_calc_off(g2d, V0_FILLC));
	g2d_writel_dev(g2d, v0_att, V0_ATTCTL);
	dev_info(&pdev->dev, "fillrect: V0_ATTCTL=0x%08x @ off=0x%05x\n",
		 g2d_readl_dev(g2d, V0_ATTCTL), g2d_calc_off(g2d, V0_ATTCTL));
	g2d_writel_dev(g2d, size_word, V0_MBSIZE);
	dev_info(&pdev->dev, "fillrect: V0_MBSIZE=0x%08x @ off=0x%05x\n",
		 g2d_readl_dev(g2d, V0_MBSIZE), g2d_calc_off(g2d, V0_MBSIZE));
	g2d_writel_dev(g2d, 0, V0_COOR);
	dev_info(&pdev->dev, "fillrect: V0_COOR=0x%08x @ off=0x%05x\n",
		 g2d_readl_dev(g2d, V0_COOR), g2d_calc_off(g2d, V0_COOR));

	/* Configura Blender: pipe0 habilitado siguiendo g2d_bldin_set() del BSP (línea 1330-1348)
	 * BSP hace READ-MODIFY-WRITE en BLD_EN_CTL, no sobrescribe
	 */
	u32 bld_en = g2d_readl_dev(g2d, BLD_EN_CTL);
	bld_en |= BLD_PIPE0_EN;  /* bit 8 para pipe0 */
	g2d_writel_dev(g2d, bld_en, BLD_EN_CTL);
	dev_info(&pdev->dev, "fillrect: BLD_EN_CTL=0x%08x (RMW) @ off=0x%05x\n",
		 g2d_readl_dev(g2d, BLD_EN_CTL), g2d_calc_off(g2d, BLD_EN_CTL));
	
	/* BLD_PREMUL_CTL: sin premultiplicación alpha (fillcolor usa alpha=0xFF opaco) */
	g2d_writel_dev(g2d, 0x0, BLD_PREMUL_CTL);
	dev_info(&pdev->dev, "fillrect: BLD_PREMUL_CTL=0x%08x\n", g2d_readl_dev(g2d, BLD_PREMUL_CTL));
	
	g2d_writel_dev(g2d, size_word, BLD_CH_ISIZE0);
	dev_info(&pdev->dev, "fillrect: BLD_CH_ISIZE0=0x%08x\n", g2d_readl_dev(g2d, BLD_CH_ISIZE0));
	g2d_writel_dev(g2d, 0, BLD_CH_OFFSET0);
	g2d_writel_dev(g2d, size_word, BLD_OUT_SIZE);
	dev_info(&pdev->dev, "fillrect: BLD_OUT_SIZE=0x%08x\n", g2d_readl_dev(g2d, BLD_OUT_SIZE));
	
	/* BLD_OUT_COLOR: Según g2d_bld_cs_set() línea 1383, para formatos RGB (<=G2D_FORMAT_BGRA1010102)
	 * debe tener bit 1 = 0 (RGB color space). Nuestro formato 0x04 (XRGB8888) < 0x18 (última BGRA)
	 */
	u32 bld_out_color = g2d_readl_dev(g2d, BLD_OUT_COLOR);
	bld_out_color &= ~BIT(1);  /* Clear bit 1 para RGB, no YUV */
	g2d_writel_dev(g2d, bld_out_color, BLD_OUT_COLOR);
	dev_info(&pdev->dev, "fillrect: BLD_OUT_COLOR=0x%08x (RGB color space)\n",
		 g2d_readl_dev(g2d, BLD_OUT_COLOR));
	
	/* Configurar BLD_CTL (control de blending) - existe en BSP g2d_regs_v2.h */
	g2d_writel_dev(g2d, 0x0, BLD_CTL); /* Modo directo, sin alpha blending especial */
	dev_info(&pdev->dev, "fillrect: BLD_CTL=0x%08x\n", g2d_readl_dev(g2d, BLD_CTL));

	/* Configura ROP (BSP usa ROP_CTL=0xF0 y ROP_INDEX0=0x61080 para COPYPEN) */
	g2d_writel_dev(g2d, 0xF0, ROP_CTL);
	g2d_writel_dev(g2d, 0x61080, ROP_INDEX0);
	dev_info(&pdev->dev, "fillrect: ROP_CTL=0x%08x ROP_INDEX0=0x%08x @ off=0x%05x\n",
		 g2d_readl_dev(g2d, ROP_CTL), g2d_readl_dev(g2d, ROP_INDEX0),
		 g2d_calc_off(g2d, ROP_INDEX0));

	/* Configura Write-Back - BSP g2d_wb_set() línea 708-789
	 * CRÍTICO: BSP escribe BLD_SIZE (0x448) desde aquí, no desde BLD config (línea 718)
	 */
	g2d_writel_dev(g2d, lower_32_bits(dst_dma), WB_LADD0);
	dev_info(&pdev->dev, "fillrect: WB_LADD0=0x%08x @ off=0x%05x\n",
		 g2d_readl_dev(g2d, WB_LADD0), g2d_calc_off(g2d, WB_LADD0));
	g2d_writel_dev(g2d, wb_hadd, WB_HADD0);
	g2d_writel_dev(g2d, pitch, WB_PITCH0);
	dev_info(&pdev->dev, "fillrect: WB_PITCH0=0x%08x\n", g2d_readl_dev(g2d, WB_PITCH0));
	g2d_writel_dev(g2d, size_word, WB_SIZE);
	dev_info(&pdev->dev, "fillrect: WB_SIZE=0x%08x\n", g2d_readl_dev(g2d, WB_SIZE));
	
	/* BSP línea 718: write_wvalue(BLD_SIZE, tmp) - mismo valor que WB_SIZE
	 * Esto parece ser una especie de "enable" o sincronización para WB
	 */
	g2d_writel_dev(g2d, size_word, BLD_SIZE);
	dev_info(&pdev->dev, "fillrect: BLD_SIZE=0x%08x (desde WB config, post-ROP)\n",
		 g2d_readl_dev(g2d, BLD_SIZE));
	
	g2d_writel_dev(g2d, wb_att, WB_ATT);
	dev_info(&pdev->dev, "fillrect: WB_ATT=0x%08x\n", g2d_readl_dev(g2d, WB_ATT));

	/* Habilita IRQ finish y limpia pending */
	g2d_writel_dev(g2d, G2D_MIXER_INT_FINISH_IRQ_EN | G2D_MIXER_INT_IRQ_PENDING,
		       G2D_MIXER_INT);

	/* Asegura que todas las escrituras lleguen al hardware */
	wmb();

	dev_info(&pdev->dev, "fillrect: configuración completa, registros pre-start:\n");
	dev_info(&pdev->dev, "  V0_ATTCTL=0x%08x V0_FILLC=0x%08x\n",
		 g2d_readl_dev(g2d, V0_ATTCTL), g2d_readl_dev(g2d, V0_FILLC));
	dev_info(&pdev->dev, "  BLD_EN_CTL=0x%08x BLD_OUT_SIZE=0x%08x\n",
		 g2d_readl_dev(g2d, BLD_EN_CTL), g2d_readl_dev(g2d, BLD_OUT_SIZE));
	dev_info(&pdev->dev, "  ROP_INDEX0=0x%08x\n", g2d_readl_dev(g2d, ROP_INDEX0));
	dev_info(&pdev->dev, "  WB_ATT=0x%08x WB_SIZE=0x%08x WB_LADD0=0x%08x\n",
		 g2d_readl_dev(g2d, WB_ATT), g2d_readl_dev(g2d, WB_SIZE),
		 g2d_readl_dev(g2d, WB_LADD0));
	dev_info(&pdev->dev, "  MIXER_INT=0x%08x\n", g2d_readl_dev(g2d, G2D_MIXER_INT));

	dev_info(&pdev->dev, "fillrect: iniciando MIXER (32x32 @ color=0x%08x)\n", fill_color);
	status_before = g2d_readl_dev(g2d, G2D_MIXER_CTL);

	/* START - usar READ-MODIFY-WRITE como el BSP (línea 2351-2353) */
	status_before = g2d_readl_dev(g2d, G2D_MIXER_CTL);
	g2d_writel_dev(g2d, status_before | G2D_MIXER_CTL_START, G2D_MIXER_CTL);
	dev_info(&pdev->dev, "fillrect: MIXER_CTL RMW: before=0x%08x after=0x%08x\n",
		 status_before, g2d_readl_dev(g2d, G2D_MIXER_CTL));
	
	/* Debug ampliado: leer todos los registros críticos post-START */
	dev_info(&pdev->dev, "fillrect: POST-START registers:\n");
	dev_info(&pdev->dev, "  MIXER_CTL=0x%08x MIXER_INT=0x%08x\n",
		 g2d_readl_dev(g2d, G2D_MIXER_CTL),
		 g2d_readl_dev(g2d, G2D_MIXER_INT));
	dev_info(&pdev->dev, "  V0_ATTCTL=0x%08x BLD_EN_CTL=0x%08x BLD_CTL=0x%08x\n",
		 g2d_readl_dev(g2d, V0_ATTCTL),
		 g2d_readl_dev(g2d, BLD_EN_CTL),
		 g2d_readl_dev(g2d, BLD_CTL));
	dev_info(&pdev->dev, "  WB_ATT=0x%08x ROP_CTL=0x%08x\n",
		 g2d_readl_dev(g2d, WB_ATT),
		 g2d_readl_dev(g2d, ROP_CTL));
	dev_info(&pdev->dev, "  RCQ_CTRL=0x%08x RCQ_IRQ_CTL=0x%08x (must be 0x0 for direct mode)\n",
		 __g2d_readl(g2d->mmio, G2D_RCQ_CTRL),
		 __g2d_readl(g2d->mmio, G2D_RCQ_IRQ_CTL));

	/* Espera hasta 100ms por IRQ pending con yield para no bloquear totalmente */
	timeout = jiffies + msecs_to_jiffies(100);
	while (time_before(jiffies, timeout)) {
		u32 int_reg = g2d_readl_dev(g2d, G2D_MIXER_INT);
		if (int_reg & G2D_MIXER_INT_IRQ_PENDING) {
			dev_info(&pdev->dev, "fillrect: IRQ triggered! INT=0x%08x\n", int_reg);
			g2d_writel_dev(g2d, G2D_MIXER_INT_IRQ_PENDING, G2D_MIXER_INT);
			break;
		}
		/* Usa usleep_range en lugar de cpu_relax para no saturar CPU */
		usleep_range(100, 200);
	}
	
	if (!time_before(jiffies, timeout)) {
		u32 int_status = g2d_readl_dev(g2d, G2D_MIXER_INT);
		dev_warn(&pdev->dev, "fillrect: timeout esperando IRQ, MIXER_INT=0x%08x\n", int_status);
	}

	status_after = g2d_readl_dev(g2d, G2D_MIXER_CTL);
	dev_info(&pdev->dev, "fillrect: CTL before=0x%08x after=0x%08x\n",
		 status_before, status_after);

	/* Verifica si el buffer tiene el color esperado */
	{
		u32 *pixels = (u32 *)dst_surface;
		bool all_green = true;
		unsigned int i;
		for (i = 0; i < (test_w * test_h); i++) {
			if (pixels[i] != fill_color) {
				all_green = false;
				break;
			}
		}
		dev_info(&pdev->dev, "fillrect: buffer check: %s (first pixel=0x%08x)\n",
			 all_green ? "PASS" : "FAIL", pixels[0]);
	}

	dmam_free_coherent(g2d->dev, surf_bytes, dst_surface, dst_dma);
}

/* ========== RCQ Structures (BSP format) ========== */

/* RCQ header - 16 bytes, must be 32-byte aligned in memory */
union rcq_hd_dw0 {
	u32 dwval;
	struct {
		u32 len:24;        /* Length of data block in bytes */
		u32 high_addr:8;   /* Upper 8 bits of DMA address */
	} bits;
};

union rcq_hd_dirty {
	u32 dwval;
	struct {
		u32 dirty:1;           /* Update flag - set to 1 */
		u32 res0:15;
		u32 n_header_len:16;   /* Next frame header length (0 for single frame) */
	} bits;
};

struct g2d_rcq_head {
	u32 low_addr;              /* Physical address of data block (32-byte aligned) */
	union rcq_hd_dw0 dw0;      /* Length + high address bits */
	union rcq_hd_dirty dirty;  /* Dirty bit (must be 1 to trigger update) */
	u32 reg_offset;            /* Base register offset for this block */
} __packed;

#define RCQ_ALIGN_BYTES 32
#define ALIGN_32(x) ALIGN(x, RCQ_ALIGN_BYTES)

/* Test fillrect via RCQ - T113 requires BSP-style RCQ with headers+data blocks */
static void g2d_hw_fillrect_rcq(struct platform_device *pdev,
	struct sunxi_g2d_dev *g2d)
{
	const u32 test_w = 32;
	const u32 test_h = 32;
	const u32 bytes_per_pixel = 4;
	const u32 pitch = test_w * bytes_per_pixel;
	const size_t surf_bytes = pitch * test_h;
	const u32 fmt_v0 = 0x04; /* XRGB8888 */
	const u32 fmt_wb = 0x04;
	const u32 fill_color = 0xFF00FF00; /* Verde */
	
	/* BSP-style RCQ: header array + register data block */
	struct g2d_rcq_head *rcq_headers = NULL;
	dma_addr_t rcq_headers_dma = 0;
	u32 *reg_data = NULL;
	dma_addr_t reg_data_dma = 0;
	size_t reg_data_size;
	u32 header_count = 5;  /* 5 headers: V0, BLD, ROP, WB, MIXER */
	u32 reg_idx = 0;
	
	void *dst_surface = NULL;
	dma_addr_t dst_dma = 0;
	u32 wb_hadd __maybe_unused;
	unsigned long timeout;
	int i;
	
	(void)fmt_v0;  /* Will be used when we add full register setup */
	(void)fmt_wb;
	(void)fill_color;
	
	dev_info(&pdev->dev, "=== fillrect_rcq: Testing G2D via BSP-style RCQ ===\n");
	
	/* Allocate dst surface */
	dst_surface = dmam_alloc_coherent(g2d->dev, surf_bytes, &dst_dma, GFP_KERNEL);
	if (!dst_surface) {
		dev_err(&pdev->dev, "fillrect_rcq: failed to alloc dst surface\n");
		return;
	}
	memset(dst_surface, 0x00, surf_bytes);
	wb_hadd = (u32)((dst_dma >> 24) & 0xFF);
	
	/* Allocate RCQ headers (32-byte aligned) - we need 5 headers (V0, BLD, ROP, WB, MIXER) */
	header_count = 5;
	rcq_headers = dmam_alloc_coherent(g2d->dev, 
					   ALIGN_32(header_count * sizeof(*rcq_headers)),
					   &rcq_headers_dma, GFP_KERNEL);
	if (!rcq_headers) {
		dev_err(&pdev->dev, "fillrect_rcq: failed to alloc rcq_headers\n");
		goto free_dst;
	}
	memset(rcq_headers, 0, ALIGN_32(header_count * sizeof(*rcq_headers)));
	
	/* Allocate register data block (32-byte aligned) - ~25 registers for complete fillrect */
	reg_data_size = ALIGN_32(30 * sizeof(u32));
	reg_data = dmam_alloc_coherent(g2d->dev, reg_data_size,
					&reg_data_dma, GFP_KERNEL);
	if (!reg_data) {
		dev_err(&pdev->dev, "fillrect_rcq: failed to alloc reg_data\n");
		goto free_headers;
	}
	memset(reg_data, 0, reg_data_size);
	
	dev_info(&pdev->dev, "fillrect_rcq: dst=0x%08x hdr=0x%08x data=0x%08x\n",
		 (u32)dst_dma, (u32)rcq_headers_dma, (u32)reg_data_dma);
	
	/* ========== Fill register data block - COMPLETE fillrect setup ========== */
	/*
	 * BSP RCQ format: The data block contains consecutive u32 values that will
	 * be written to consecutive registers starting from reg_offset in the header.
	 * 
	 * For a complete fillrect, we need to configure multiple blocks.
	 * We'll create multiple headers, one per register block.
	 * 
	 * NOTE: For simplicity, we'll pack everything into one data block with
	 * multiple headers pointing to different sections.
	 */
	
	reg_idx = 0;
	
	/* === V0 Layer Configuration (fill color source) === */
	/* V0 registers start at 0x800, we need: FILLC, ATTCTL, MBSIZE, COOR */
	u32 v0_start_idx = reg_idx;
	reg_data[reg_idx++] = fill_color;        /* V0_FILLC @ 0x824 */
	reg_data[reg_idx++] = 0x00000411;        /* V0_ATTCTL @ 0x800: EN|FILLCOLOR_EN|fmt=XRGB8888 */
	reg_data[reg_idx++] = 0x001F001F;        /* V0_MBSIZE @ 0x804: 32x32 */
	reg_data[reg_idx++] = 0x00000000;        /* V0_COOR @ 0x808: (0,0) */
	
	/* === BLD Configuration (blender/router) === */
	u32 bld_start_idx = reg_idx;
	reg_data[reg_idx++] = 0x00000101;        /* BLD_EN_CTL @ 0x400: Pipe0 enabled */
	reg_data[reg_idx++] = 0x00000000;        /* BLD_PREMUL_CTL @ 0x408 */
	reg_data[reg_idx++] = 0x001F001F;        /* BLD_CH_ISIZE0 @ 0x410: 32x32 */
	reg_data[reg_idx++] = 0x00000000;        /* BLD_CH_OFFSET0 @ 0x414: (0,0) */
	reg_data[reg_idx++] = 0x001F001F;        /* BLD_OUT_SIZE @ 0x484: 32x32 */
	reg_data[reg_idx++] = 0x00000000;        /* BLD_OUT_COLOR @ 0x488: RGB mode */
	reg_data[reg_idx++] = 0x00000000;        /* BLD_CTL @ 0x480 */
	reg_data[reg_idx++] = 0x001F001F;        /* BLD_SIZE @ 0x40C: 32x32 (BSP adds this) */
	
	/* === ROP Configuration (FIX #6: Try ROP_INDEX0=0 instead of 0x00061080) === */
	u32 rop_start_idx = reg_idx;
	reg_data[reg_idx++] = 0x00000F00;        /* ROP_CTL @ 0x480: ROP3 mode */
	reg_data[reg_idx++] = 0x00000000;        /* ROP_INDEX0 @ 0x484: Try default 0 */
	
	/* === WB Configuration (writeback/destination) === */
	u32 wb_start_idx = reg_idx;
	reg_data[reg_idx++] = (u32)dst_dma;      /* WB_LADD0 @ 0x3014 */
	reg_data[reg_idx++] = wb_hadd;           /* WB_HADD0 @ 0x3018 */
	reg_data[reg_idx++] = pitch;             /* WB_PITCH0 @ 0x301C: 128 bytes */
	reg_data[reg_idx++] = 0x001F001F;        /* WB_SIZE @ 0x3004: 32x32 */
	reg_data[reg_idx++] = fmt_wb;            /* WB_ATT @ 0x3000: XRGB8888 */
	
	/* === MIXER Control (FIX #5: MIXER_CLK as first RCQ entry) === */
	u32 mixer_start_idx = reg_idx;
	reg_data[reg_idx++] = 0x00000001;        /* MIXER_CLK @ 0x108: Enable internal sub-block clocks */
	reg_data[reg_idx++] = G2D_MIXER_INT_FINISH_IRQ_EN;  /* MIXER_INT @ 0x10C */
	reg_data[reg_idx++] = G2D_MIXER_CTL_START;          /* MIXER_CTL @ 0x110 */
	
	dev_info(&pdev->dev, "fillrect_rcq: filled %u register values (V0:%u BLD:%u ROP:%u WB:%u MIXER:%u)\n",
		 reg_idx, v0_start_idx, bld_start_idx, rop_start_idx, wb_start_idx, mixer_start_idx);
	
	/* ========== Setup RCQ headers (BSP format) - one per register block ========== */
	
	/* Header 0: V0 Layer (4 registers starting at V0_FILLC) */
	rcq_headers[0].low_addr = (u32)(reg_data_dma + v0_start_idx * sizeof(u32));
	rcq_headers[0].dw0.bits.len = 4 * sizeof(u32);
	rcq_headers[0].dw0.bits.high_addr = (u8)upper_32_bits(reg_data_dma);
	rcq_headers[0].dirty.bits.dirty = 1;
	rcq_headers[0].dirty.bits.n_header_len = 0;
	rcq_headers[0].reg_offset = g2d_calc_off(g2d, V0_FILLC);  /* Start at V0_FILLC */
	
	/* Header 1: BLD (8 registers starting at BLD_EN_CTL) */
	rcq_headers[1].low_addr = (u32)(reg_data_dma + bld_start_idx * sizeof(u32));
	rcq_headers[1].dw0.bits.len = 8 * sizeof(u32);
	rcq_headers[1].dw0.bits.high_addr = (u8)upper_32_bits(reg_data_dma);
	rcq_headers[1].dirty.bits.dirty = 1;
	rcq_headers[1].dirty.bits.n_header_len = 0;
	rcq_headers[1].reg_offset = g2d_calc_off(g2d, BLD_EN_CTL);
	
	/* Header 2: ROP (2 registers starting at ROP_CTL) */
	rcq_headers[2].low_addr = (u32)(reg_data_dma + rop_start_idx * sizeof(u32));
	rcq_headers[2].dw0.bits.len = 2 * sizeof(u32);
	rcq_headers[2].dw0.bits.high_addr = (u8)upper_32_bits(reg_data_dma);
	rcq_headers[2].dirty.bits.dirty = 1;
	rcq_headers[2].dirty.bits.n_header_len = 0;
	rcq_headers[2].reg_offset = g2d_calc_off(g2d, ROP_CTL);
	
	/* Header 3: WB (5 registers starting at WB_LADD0) */
	rcq_headers[3].low_addr = (u32)(reg_data_dma + wb_start_idx * sizeof(u32));
	rcq_headers[3].dw0.bits.len = 5 * sizeof(u32);
	rcq_headers[3].dw0.bits.high_addr = (u8)upper_32_bits(reg_data_dma);
	rcq_headers[3].dirty.bits.dirty = 1;
	rcq_headers[3].dirty.bits.n_header_len = 0;
	rcq_headers[3].reg_offset = g2d_calc_off(g2d, WB_LADD0);
	
	/* Header 4: MIXER (3 registers starting at MIXER_CLK) */
	rcq_headers[4].low_addr = (u32)(reg_data_dma + mixer_start_idx * sizeof(u32));
	rcq_headers[4].dw0.bits.len = 3 * sizeof(u32);
	rcq_headers[4].dw0.bits.high_addr = (u8)upper_32_bits(reg_data_dma);
	rcq_headers[4].dirty.bits.dirty = 1;
	rcq_headers[4].dirty.bits.n_header_len = 0;
	rcq_headers[4].reg_offset = g2d_calc_off(g2d, G2D_MIXER_CLK);
	
	dev_info(&pdev->dev, "fillrect_rcq: Setup %u RCQ headers:\n", header_count);
	for (i = 0; i < header_count; i++) {
		dev_info(&pdev->dev, "  [%d] low=0x%08x dw0=0x%08x dirty=0x%08x off=0x%04x (len=%u)\n",
			 i, rcq_headers[i].low_addr, rcq_headers[i].dw0.dwval,
			 rcq_headers[i].dirty.dwval, rcq_headers[i].reg_offset,
			 rcq_headers[i].dw0.bits.len);
	}
	
	/* ========== Program and activate RCQ ========== */
	
	/* NOTE: NOT resetting via G2D_AHB_RESET to preserve RCQ state from probe init */
	dev_info(&pdev->dev, "fillrect_rcq: Skipping AHB reset to preserve RCQ state\n");
	
	/* Program RCQ head pointer - points to HEADER array, not data */
	__g2d_writel(g2d->mmio, (u32)rcq_headers_dma, G2D_RCQ_HEAD_LOW);
	__g2d_writel(g2d->mmio, upper_32_bits(rcq_headers_dma), G2D_RCQ_HEAD_HIGH);
	
	/* RCQ_HEAD_LEN = number of HEADERS (not bytes!) according to BSP */
	__g2d_writel(g2d->mmio, header_count, G2D_RCQ_HEAD_LEN);
	
	dev_info(&pdev->dev, "fillrect_rcq: RCQ HEAD_LOW=0x%08x HEAD_HIGH=0x%08x LEN=%u (headers)\n",
		 __g2d_readl(g2d->mmio, G2D_RCQ_HEAD_LOW),
		 __g2d_readl(g2d->mmio, G2D_RCQ_HEAD_HIGH),
		 __g2d_readl(g2d->mmio, G2D_RCQ_HEAD_LEN));
	
	/* FIX #4: Properly enable and clear RCQ IRQ */
	__g2d_writel(g2d->mmio, 0, G2D_RCQ_IRQ_CTL);  // Disable first
	__g2d_writel(g2d->mmio, G2D_RCQ_STATUS_TASK_END | G2D_RCQ_STATUS_CFG_FINISH, 
		     G2D_RCQ_STATUS);  // Clear status bits
	__g2d_writel(g2d->mmio, G2D_RCQ_IRQ_TASK_END_EN | G2D_RCQ_IRQ_CFG_FINISH_EN, 
		     G2D_RCQ_IRQ_CTL);  // Enable IRQs without SEL
	
	dev_info(&pdev->dev, "fillrect_rcq: RCQ_IRQ_CTL=0x%08x RCQ_STATUS before: 0x%08x\n",
		 __g2d_readl(g2d->mmio, G2D_RCQ_IRQ_CTL),
		 __g2d_readl(g2d->mmio, G2D_RCQ_STATUS));
	
	/* Activate RCQ with UPDATE bit */
	__g2d_writel(g2d->mmio, G2D_RCQ_CTRL_UPDATE, G2D_RCQ_CTRL);
	udelay(10);
	
	dev_info(&pdev->dev, "fillrect_rcq: RCQ activated! CTRL=0x%08x STATUS=0x%08x\n",
		 __g2d_readl(g2d->mmio, G2D_RCQ_CTRL),
		 __g2d_readl(g2d->mmio, G2D_RCQ_STATUS));
	
	/* FIX #7: Enhanced polling with detailed STATUS logging */
	timeout = jiffies + msecs_to_jiffies(100);
	while (time_before(jiffies, timeout)) {
		u32 int_reg = g2d_readl_dev(g2d, G2D_MIXER_INT);
		u32 rcq_status = __g2d_readl(g2d->mmio, G2D_RCQ_STATUS);
		
		/* Log any changes in RCQ_STATUS */
		static u32 last_status = 0;
		if (rcq_status != last_status) {
			dev_info(&pdev->dev, "fillrect_rcq: STATUS changed: 0x%08x -> 0x%08x\n",
				 last_status, rcq_status);
			last_status = rcq_status;
		}
		
		/* Check for TASK_END bit (bit 0) */
		if (rcq_status & BIT(0)) {
			dev_info(&pdev->dev, "fillrect_rcq: TASK_END! STATUS=0x%08x\n", rcq_status);
			break;
		}
		
		if (int_reg & G2D_MIXER_INT_IRQ_PENDING) {
			dev_info(&pdev->dev, "fillrect_rcq: IRQ! MIXER_INT=0x%08x RCQ_STATUS=0x%08x\n",
				 int_reg, rcq_status);
			g2d_writel_dev(g2d, G2D_MIXER_INT_IRQ_PENDING, G2D_MIXER_INT);
			break;
		}
		usleep_range(1000, 2000);  // Poll every 1-2ms
	}
	
	if (!time_before(jiffies, timeout)) {
		dev_warn(&pdev->dev, "fillrect_rcq: timeout! MIXER_INT=0x%08x RCQ_STATUS=0x%08x\n",
			 g2d_readl_dev(g2d, G2D_MIXER_INT),
			 __g2d_readl(g2d->mmio, G2D_RCQ_STATUS));
	}
	
	/* Verificar resultado */
	{
		u32 *pixels = (u32 *)dst_surface;
		bool all_green = true;
		int check_count = min(10, (int)(surf_bytes / 4));
		
		for (i = 0; i < check_count; i++) {
			if (pixels[i] != fill_color) {
				all_green = false;
				break;
			}
		}
		dev_info(&pdev->dev, "fillrect_rcq: buffer check: %s (first pixel=0x%08x)\n",
			 all_green ? "PASS ✓" : "FAIL ✗", pixels[0]);
	}
	
	/* Cleanup */
	dmam_free_coherent(g2d->dev, reg_data_size, reg_data, reg_data_dma);
free_headers:
	dmam_free_coherent(g2d->dev, ALIGN_32(header_count * sizeof(*rcq_headers)),
			   rcq_headers, rcq_headers_dma);
free_dst:
	dmam_free_coherent(g2d->dev, surf_bytes, dst_surface, dst_dma);
	
	dev_info(&pdev->dev, "=== fillrect_rcq: End ===\n");
}

/* Pequeña prueba de estímulo RCQ: carga un header ficticio, habilita IRQ y
 * pulsa el bit UPDATE para ver si el bloque responde (status/frame counter).
 */
static void g2d_rcq_kick_hw(struct platform_device *pdev,
	struct sunxi_g2d_dev *g2d)
{
	/* Reserva holgada para headers + payloads alineados */
	const size_t rcq_buf_bytes = 1024;
	const u32 test_w = 32;
	const u32 test_h = 32;
	const u32 bytes_per_pixel = 4; /* XRGB8888 */
	const u32 pitch = test_w * bytes_per_pixel;
	const size_t surf_bytes = pitch * test_h;
	const u32 fmt_hw = 0x08; /* según tabla g_formats (XRGB8888) */
	struct g2d_rcq_hw_head *head;
	void *virt;
	void *src_surface = NULL, *dst_surface = NULL;
	dma_addr_t dma;
	dma_addr_t src_dma = 0, dst_dma = 0;
	u32 irq_ctl_before, irq_ctl_after;
	u32 status_before, status_after;
	u32 pending;
	u32 header_len_bytes;
	/* Start bit lives at bit 31 per HW docs */
	u32 ctl_word = g2d_readl_dev(g2d, G2D_MIXER_CTL) | G2D_MIXER_CTL_START;
	/* ACK pending IRQ (bit0) and enable finish interrupt (bit4) */
	u32 int_word = g2d_readl_dev(g2d, G2D_MIXER_INT) |
		(G2D_MIXER_INT_FINISH_IRQ_EN | G2D_MIXER_INT_IRQ_PENDING);
	const unsigned int payload_len = sizeof(u32);
	/* Limita construcción estática para depuración */
	struct {
		u32 reg;
		u32 val;
	} cmds[24];
	unsigned int cmd_cnt = 0;
	u32 size_word = (test_w - 1) | ((test_h - 1) << 16);
	u32 v0_att = V0_ATTCTL_EN | (fmt_hw << V0_ATTCTL_FMT_SHIFT);
	u32 wb_att = WB_ATT_EN | (fmt_hw << WB_ATT_FMT_SHIFT);
	u32 v0_hadd;
	u32 wb_hadd;
	size_t header_bytes;
	size_t payload_offsets[ARRAY_SIZE(cmds)];
	u32 payload_vals[ARRAY_SIZE(cmds)];
	unsigned int header_slots;
	unsigned int i;

	if (g2d->mmio_size <= G2D_RCQ_HEAD_LEN) {
		dev_warn(&pdev->dev, "RCQ kick: mmio_size 0x%llx demasiado pequeña\n",
			 (unsigned long long)g2d->mmio_size);
		return;
	}

	virt = dmam_alloc_coherent(g2d->dev, rcq_buf_bytes, &dma, GFP_KERNEL);
	if (!virt) {
		dev_warn(&pdev->dev, "RCQ kick: sin memoria coherente (%zu bytes)\n",
			rcq_buf_bytes);
		return;
	}

	src_surface = dmam_alloc_coherent(g2d->dev, surf_bytes, &src_dma,
					   GFP_KERNEL);
	if (!src_surface) {
		dev_warn(&pdev->dev, "RCQ kick: sin superficie fuente (%zu bytes)\n",
			surf_bytes);
		goto free_head;
	}

	dst_surface = dmam_alloc_coherent(g2d->dev, surf_bytes, &dst_dma,
					   GFP_KERNEL);
	if (!dst_surface) {
		dev_warn(&pdev->dev, "RCQ kick: sin superficie destino (%zu bytes)\n",
			surf_bytes);
		goto free_src;
	}

	memset(virt, 0, rcq_buf_bytes);
	memset(src_surface, 0x55, surf_bytes);
	memset(dst_surface, 0x00, surf_bytes);

	v0_hadd = (u32)((src_dma >> 24) & 0xFF);
	wb_hadd = (u32)((dst_dma >> 24) & 0xFF);

	cmds[cmd_cnt++] = (typeof(cmds[0])) {
		.reg = g2d_calc_off(g2d, G2D_MIXER_CLK),
		.val = g2d_readl_dev(g2d, G2D_MIXER_CLK),
	};
	cmds[cmd_cnt++] = (typeof(cmds[0])) {
		.reg = g2d_calc_off(g2d, BLD_OUT_SIZE),
		.val = size_word,
	};
	cmds[cmd_cnt++] = (typeof(cmds[0])) {
		.reg = g2d_calc_off(g2d, BLD_CH_ISIZE0),
		.val = size_word,
	};
	cmds[cmd_cnt++] = (typeof(cmds[0])) {
		.reg = g2d_calc_off(g2d, BLD_EN_CTL),
		.val = BLD_PIPE0_EN,
	};
	cmds[cmd_cnt++] = (typeof(cmds[0])) {
		.reg = g2d_calc_off(g2d, V0_LADD0),
		.val = lower_32_bits(src_dma),
	};
	cmds[cmd_cnt++] = (typeof(cmds[0])) {
		.reg = g2d_calc_off(g2d, V0_HADD),
		.val = v0_hadd,
	};
	cmds[cmd_cnt++] = (typeof(cmds[0])) {
		.reg = g2d_calc_off(g2d, V0_PITCH0),
		.val = pitch,
	};
	cmds[cmd_cnt++] = (typeof(cmds[0])) {
		.reg = g2d_calc_off(g2d, V0_MBSIZE),
		.val = size_word,
	};
	cmds[cmd_cnt++] = (typeof(cmds[0])) {
		.reg = g2d_calc_off(g2d, V0_COOR),
		.val = 0,
	};
	cmds[cmd_cnt++] = (typeof(cmds[0])) {
		.reg = g2d_calc_off(g2d, V0_ATTCTL),
		.val = v0_att,
	};
	cmds[cmd_cnt++] = (typeof(cmds[0])) {
		.reg = g2d_calc_off(g2d, WB_LADD0),
		.val = lower_32_bits(dst_dma),
	};
	cmds[cmd_cnt++] = (typeof(cmds[0])) {
		.reg = g2d_calc_off(g2d, WB_HADD0),
		.val = wb_hadd,
	};
	cmds[cmd_cnt++] = (typeof(cmds[0])) {
		.reg = g2d_calc_off(g2d, WB_PITCH0),
		.val = pitch,
	};
	cmds[cmd_cnt++] = (typeof(cmds[0])) {
		.reg = g2d_calc_off(g2d, WB_SIZE),
		.val = size_word,
	};
	cmds[cmd_cnt++] = (typeof(cmds[0])) {
		.reg = g2d_calc_off(g2d, WB_ATT),
		.val = wb_att,
	};
	cmds[cmd_cnt++] = (typeof(cmds[0])) {
		.reg = g2d_calc_off(g2d, G2D_MIXER_INT),
		.val = int_word,
	};
	cmds[cmd_cnt++] = (typeof(cmds[0])) {
		.reg = g2d_calc_off(g2d, G2D_MIXER_CTL),
		.val = ctl_word,
	};

	header_slots = G2D_RCQ_HEADER_ALIGN(cmd_cnt);
	header_bytes = header_slots * sizeof(*head);
	head = virt;
	memset(head, 0, header_bytes);

	{
		size_t payload_cursor = G2D_RCQ_ALIGN32(header_bytes);
		for (i = 0; i < cmd_cnt; i++) {
			if (payload_cursor + payload_len > rcq_buf_bytes) {
				dev_warn(&pdev->dev,
					 "RCQ kick: buffer insuficiente para payload cmd %u\n",
					 i);
				goto free_dst;
			}
			payload_offsets[i] = payload_cursor;
			payload_cursor = G2D_RCQ_ALIGN32(payload_cursor + payload_len);
		}
	}

	for (i = 0; i < cmd_cnt; i++) {
		u8 *payload_ptr = (u8 *)virt + payload_offsets[i];
		dma_addr_t payload_dma = dma + payload_offsets[i];

		payload_vals[i] = cmds[i].val;
		*(u32 *)payload_ptr = cmds[i].val;

		head[i].low_addr = lower_32_bits(payload_dma);
		head[i].len_high_addr = (payload_len & G2D_RCQ_LEN_MASK24) |
				       ((upper_32_bits(payload_dma) & 0xFF) << 24);
		head[i].dirty_next_len = G2D_RCQ_DIRTY_BIT;
		head[i].reg_offset = cmds[i].reg;
	}

	header_len_bytes = header_slots * sizeof(*head);
	g2d_rcq_load_head(g2d, dma, header_len_bytes);

	irq_ctl_before = g2d_readl_dev(g2d, G2D_RCQ_IRQ_CTL);
	status_before = g2d_readl_dev(g2d, G2D_RCQ_STATUS);

	g2d_writel_dev(g2d,
		irq_ctl_before | G2D_RCQ_IRQ_SEL |
		G2D_RCQ_IRQ_TASK_END_EN | G2D_RCQ_IRQ_CFG_FINISH_EN,
		G2D_RCQ_IRQ_CTL);
	g2d_writel_dev(g2d, G2D_RCQ_STATUS_TASK_END | G2D_RCQ_STATUS_CFG_FINISH,
		G2D_RCQ_STATUS);

	g2d_writel_dev(g2d, G2D_RCQ_CTRL_UPDATE, G2D_RCQ_CTRL);
	usleep_range(50, 100);

	status_after = g2d_readl_dev(g2d, G2D_RCQ_STATUS);
	irq_ctl_after = g2d_readl_dev(g2d, G2D_RCQ_IRQ_CTL);

	dev_info(&pdev->dev,
		 "RCQ kick: head=0x%08x%08x len=%u bytes status_before=0x%08x status_after=0x%08x frame_cnt=%u irq_ctl=0x%08x->0x%08x cmds=%u reg0=0x%08x val0=0x%08x reg1=0x%08x val1=0x%08x\n",
		 g2d_readl_dev(g2d, G2D_RCQ_HEAD_HIGH),
		 g2d_readl_dev(g2d, G2D_RCQ_HEAD_LOW),
		 g2d_readl_dev(g2d, G2D_RCQ_HEAD_LEN) & G2D_RCQ_HEAD_LEN_MASK,
		 status_before, status_after,
		 (status_after & G2D_RCQ_STATUS_FRAME_CNT_MASK) >>
		 G2D_RCQ_STATUS_FRAME_CNT_SHIFT,
		 irq_ctl_before, irq_ctl_after, cmd_cnt,
		 cmds[0].reg, payload_vals[0],
		 cmds[1].reg, payload_vals[1]);

	/* Limpia pending si algo se activó */
	pending = status_after &
		(G2D_RCQ_STATUS_TASK_END | G2D_RCQ_STATUS_CFG_FINISH);
	if (pending)
		g2d_writel_dev(g2d, pending, G2D_RCQ_STATUS);

	/* Limpia UPDATE y deja IRQ como estaban originalmente */
	g2d_writel_dev(g2d, 0, G2D_RCQ_CTRL);
	g2d_writel_dev(g2d, irq_ctl_before, G2D_RCQ_IRQ_CTL);

free_dst:
	if (dst_surface)
		dmam_free_coherent(g2d->dev, surf_bytes, dst_surface, dst_dma);
free_src:
	if (src_surface)
		dmam_free_coherent(g2d->dev, surf_bytes, src_surface, src_dma);
free_head:
	dmam_free_coherent(g2d->dev, rcq_buf_bytes, virt, dma);
}

struct sunxi_g2d_fmt {
	u32 fourcc;
	u8  depth;      // bits per pixel
	u8  num_planes; // 1 por ahora
	u8  hw_fmt_id;  // ID FBFMT del HW (bits [13:8]) según PoC/Tina
};

/* Mapas FBFMT (aprox.) basados en PoC/Tina: 
 * 0x08: XRGB8888, 0x09: XBGR8888, 0x0A: RGBX8888, 0x0B: BGRX8888
 */
static const struct sunxi_g2d_fmt g_formats[] = {
	{ .fourcc = V4L2_PIX_FMT_XRGB32, .depth = 32, .num_planes = 1, .hw_fmt_id = 0x08 }, // XR24
#ifdef V4L2_PIX_FMT_BGRX32
	{ .fourcc = V4L2_PIX_FMT_BGRX32, .depth = 32, .num_planes = 1, .hw_fmt_id = 0x0B }, // BX24
#endif
#ifdef V4L2_PIX_FMT_XBGR32
	{ .fourcc = V4L2_PIX_FMT_XBGR32, .depth = 32, .num_planes = 1, .hw_fmt_id = 0x09 }, // XB24
#endif
#ifdef V4L2_PIX_FMT_RGBX32
	{ .fourcc = V4L2_PIX_FMT_RGBX32, .depth = 32, .num_planes = 1, .hw_fmt_id = 0x0A }, // RX24
#endif
};

static const struct sunxi_g2d_fmt *find_fmt(u32 fourcc)
{
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(g_formats); i++)
		if (g_formats[i].fourcc == fourcc)
			return &g_formats[i];
	return NULL;
}

struct sunxi_g2d_ctx;

struct sunxi_g2d_ctx {
	struct sunxi_g2d_dev *g2d;
	struct v4l2_fh fh;

	// formats
	struct v4l2_pix_format out_fmt; // OUTPUT
	struct v4l2_pix_format cap_fmt; // CAPTURE

	// vb2 queues
	struct vb2_queue out_q;
	struct vb2_queue cap_q;

	// param job actual (calculado al submit)
	bool needs_scale;

	// estado del job en curso
	struct vb2_v4l2_buffer *cur_src;
	struct vb2_v4l2_buffer *cur_dst;
	bool job_done;
	struct delayed_work timeout_work;
};

// Parámetro de módulo para activar programación HW (experimental)
static bool enable_hw = false;
module_param(enable_hw, bool, 0644);
MODULE_PARM_DESC(enable_hw, "Habilita programación HW del G2D (experimental); si false usa memcpy CPU");

// Parámetro de módulo para hacer un autodiagnóstico de MMIO al inicio
static bool self_test;
module_param(self_test, bool, 0644);
MODULE_PARM_DESC(self_test, "Si es 1, vuelca lecturas de MMIO en probe para verificar mapeo/clocking");

// Parámetro opcional para intentar detectar registros RCQ escribiendo patrones
static bool rcq_probe;
module_param(rcq_probe, bool, 0644);
MODULE_PARM_DESC(rcq_probe, "Si es 1 (y self_test también), prueba escrituras sobre la ventana RCQ para detectar offsets con respuesta");

/* Parámetro opcional para disparar la lógica RCQ y observar banderas/contadores */
static bool rcq_kick;
module_param(rcq_kick, bool, 0644);
MODULE_PARM_DESC(rcq_kick, "Si es 1 (y self_test también), programa un header RCQ ficticio y pulsa update/IRQ para ver si el hardware responde");

/* Parámetro opcional para ejecutar fillrect test (puede colgar si HW no responde) */
static bool fillrect_test;
module_param(fillrect_test, bool, 0644);
MODULE_PARM_DESC(fillrect_test, "Si es 1 (y self_test también), ejecuta test de fillrect con MIXER directo");

static void g2d_rcq_probe_window(struct platform_device *pdev,
				 struct sunxi_g2d_dev *g2d)
{
	u32 base_start = 0x28000;
	u32 base_end = 0x28c00;
	u32 hits32 = 0;
	u32 hits64 = 0;
	u32 off;

	if (g2d->bias_subblocks != 0x28000)
		return;

	if (g2d->mmio_size <= base_end) {
		dev_warn(&pdev->dev, "RCQ probe skipped: mmio_size=0x%llx < 0x%05x\n",
			 (unsigned long long)g2d->mmio_size, base_end);
		return;
	}

	dev_info(&pdev->dev, "RCQ probe: writing patterns to 0x%05x-0x%05x (step 4)\n",
		 base_start, base_end - 4);

	for (off = base_start; off < base_end; off += 4) {
		u32 orig = __g2d_readl(g2d->mmio, off);
		u32 pattern = 0xA5000000 | (off & 0xFFFF);
		u32 val;

		__g2d_writel(g2d->mmio, pattern, off);
		udelay(1);
		val = __g2d_readl(g2d->mmio, off);

		if (val != orig) {
			hits32++;
			dev_info(&pdev->dev,
				 "RCQ probe hit at 0x%05x: orig=0x%08x val=0x%08x\n",
				 off, orig, val);
		}

		/* Restaura valor original para no dejar el IP en estado extraño */
		__g2d_writel(g2d->mmio, orig, off);
		udelay(1);
	}

	for (off = base_start; off + 4 < base_end; off += 8) {
		u32 orig_lo = __g2d_readl(g2d->mmio, off);
		u32 orig_hi = __g2d_readl(g2d->mmio, off + 4);
		u64 pattern = 0xAD00000000000000ULL | (((u64)off & 0xFFFF) << 16) | (off & 0xFFFF);
		u32 new_lo = (u32)(pattern & 0xFFFFFFFFULL);
		u32 new_hi = (u32)(pattern >> 32);
		u32 val_lo, val_hi;

		__g2d_writel(g2d->mmio, new_lo, off);
		__g2d_writel(g2d->mmio, new_hi, off + 4);
		udelay(1);
		val_lo = __g2d_readl(g2d->mmio, off);
		val_hi = __g2d_readl(g2d->mmio, off + 4);

		if (val_lo != orig_lo || val_hi != orig_hi) {
			hits64++;
			dev_info(&pdev->dev,
				 "RCQ probe 64-bit hit at 0x%05x: orig=0x%08x%08x val=0x%08x%08x\n",
				 off, orig_hi, orig_lo, val_hi, val_lo);
		}

		__g2d_writel(g2d->mmio, orig_lo, off);
		__g2d_writel(g2d->mmio, orig_hi, off + 4);
		udelay(1);
	}

	dev_info(&pdev->dev, "RCQ probe summary: %u offsets changed (32-bit), %u offsets changed (64-bit stride)\n",
		 hits32, hits64);
}

// ========== VB2 ops ==========
static int qbuf_queue_setup(struct vb2_queue *q,
			    unsigned int *nbufs, unsigned int *nplanes,
			    unsigned int sizes[], struct device *alloc_devs[])
{
	struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(q);
	struct v4l2_pix_format *pf =
		(q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) ? &ctx->out_fmt : &ctx->cap_fmt;

	unsigned int size = pf->sizeimage;
	if (!size)
		return -EINVAL;

	if (*nplanes) {
		if (sizes[0] < size)
			return -EINVAL;
	} else {
		*nplanes = 1;
		sizes[0] = size;
	}
	return 0;
}

static int qbuf_buf_prepare(struct vb2_buffer *vb)
{
	struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct v4l2_pix_format *pf =
		(vb->vb2_queue->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) ? &ctx->out_fmt : &ctx->cap_fmt;

	unsigned long size = pf->sizeimage;
	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, size);
	return 0;
}

static void qbuf_buf_queue(struct vb2_buffer *vb)
{
	struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct sunxi_g2d_dev *g2d = ctx->g2d;

	// Cola M2M estándar: pasa el buffer a v4l2_m2m
	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));

	// Intenta arrancar job si hay OUT+CAP encolados
	dev_info(g2d->dev, "buf_queue: type=%u queued (src_ready=%d dst_ready=%d)\n",
			 vb->vb2_queue->type,
			 v4l2_m2m_num_src_bufs_ready(ctx->fh.m2m_ctx),
			 v4l2_m2m_num_dst_bufs_ready(ctx->fh.m2m_ctx));
	v4l2_m2m_try_schedule(ctx->fh.m2m_ctx);
}

static int qbuf_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(q);
	v4l2_m2m_try_schedule(ctx->fh.m2m_ctx);
	return 0;
}

static void qbuf_stop_streaming(struct vb2_queue *q)
{
	struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *vb;

	if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
		while (v4l2_m2m_num_src_bufs_ready(ctx->fh.m2m_ctx) > 0) {
			vb = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
			v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
		}
	} else {
		while (v4l2_m2m_num_dst_bufs_ready(ctx->fh.m2m_ctx) > 0) {
			vb = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
			v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
		}
	}
}

static const struct vb2_ops qbuf_qops = {
	.queue_setup    = qbuf_queue_setup,
	.buf_prepare    = qbuf_buf_prepare,
	.buf_queue      = qbuf_buf_queue,
	.start_streaming = qbuf_start_streaming,
	.stop_streaming  = qbuf_stop_streaming,
	.wait_prepare   = vb2_ops_wait_prepare,
	.wait_finish    = vb2_ops_wait_finish,
};

// ========== M2M device_run / job_complete ==========
static void g2d_device_run(void *priv)
{
	struct sunxi_g2d_ctx *ctx = priv;
	struct sunxi_g2d_dev *g2d = ctx->g2d;
	unsigned long flags;

	// Saca un par OUT+CAP
	struct vb2_v4l2_buffer *src, *dst;
	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (!src || !dst) {
		dev_dbg(g2d->dev, "device_run: no buffers ready (src=%p dst=%p)\n", src, dst);
		return;
	}

	// Calcula si hay que escalar
	ctx->needs_scale = (ctx->out_fmt.width  != ctx->cap_fmt.width) ||
			   (ctx->out_fmt.height != ctx->cap_fmt.height);

	// Direcciones físicas (CMA)
	dma_addr_t src_dma = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
	dma_addr_t dst_dma = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);

	u32 src_w = ctx->out_fmt.width;
	u32 src_h = ctx->out_fmt.height;
	u32 dst_w = ctx->cap_fmt.width;
	u32 dst_h = ctx->cap_fmt.height;
	u32 src_pitch = ctx->out_fmt.bytesperline;
	u32 dst_pitch = ctx->cap_fmt.bytesperline;

	// Guarda estado del job y marca contexto en ejecución para IRQ
	ctx->cur_src = src;
	ctx->cur_dst = dst;
	ctx->job_done = false;
	spin_lock_irqsave(&g2d->irqlock, flags);
	g2d->curr_ctx = ctx;
	spin_unlock_irqrestore(&g2d->irqlock, flags);

	if (enable_hw && g2d->irq > 0 && !ctx->needs_scale) {
		/* Dump previo rápido para ver si el bloque responde (solo informativo) */
		u32 pre_int = g2d_readl_dev(g2d, G2D_MIXER_INT);
		u32 pre_ctl = g2d_readl_dev(g2d, G2D_MIXER_CTL);
		u32 pre_clk = g2d_readl_dev(g2d, G2D_MIXER_CLK);
		dev_dbg(g2d->dev, "pre: MIXER INT=0x%08x CTL=0x%08x CLK=0x%08x\n", pre_int, pre_ctl, pre_clk);
		/* Evita tocar registros fuera de la ventana mapeada (WB está a 0x3000 en nuestro header) */
		if (g2d->hw_broken || g2d->mmio_size <= WB_LADD0) {
			if (g2d->mmio_size <= WB_LADD0)
				dev_dbg(g2d->dev, "device_run: HW path skipped (WB regs out of mapped range: size=0x%llx)\n",
					(unsigned long long)g2d->mmio_size);
		} else {
			/* ==== PROGRAMAR G2D (conservador, v2) ==== */
			/* Selección de formato HW (FBFMT) en bits [13:8] según formato V4L2 */
			const struct sunxi_g2d_fmt *fmt_out = find_fmt(ctx->out_fmt.pixelformat);
			const struct sunxi_g2d_fmt *fmt_cap = find_fmt(ctx->cap_fmt.pixelformat);
			u32 v0_att = V0_ATTCTL_EN;
			u32 wb_att = WB_ATT_EN;
			if (fmt_out)
				v0_att |= ((u32)fmt_out->hw_fmt_id << V0_ATTCTL_FMT_SHIFT);
			if (fmt_cap)
				wb_att |= ((u32)fmt_cap->hw_fmt_id << WB_ATT_FMT_SHIFT);
			/* Fuente V0 */
			g2d_writel_dev(g2d, lower_32_bits(src_dma), V0_LADD0);
			/* Bits altos (8b) de la dirección para cada plano; usamos sólo plano 0 */
			g2d_writel_dev(g2d, (u32)((src_dma >> 24) & 0xFF), V0_HADD);
			g2d_writel_dev(g2d, src_pitch, V0_PITCH0);
			g2d_writel_dev(g2d, (src_w - 1) | ((src_h - 1) << 16), V0_MBSIZE);
			g2d_writel_dev(g2d, 0, V0_COOR);
			g2d_writel_dev(g2d, v0_att, V0_ATTCTL);
		dev_dbg(g2d->dev, "V0: ATT=0x%08x LADD0=0x%08x HADD=0x%08x PITCH0=0x%08x MBSIZE=0x%08x\n",
			g2d_readl_dev(g2d, V0_ATTCTL),
			g2d_readl_dev(g2d, V0_LADD0),
			g2d_readl_dev(g2d, V0_HADD),
			g2d_readl_dev(g2d, V0_PITCH0),
			g2d_readl_dev(g2d, V0_MBSIZE));

			/* Destino WB */
			g2d_writel_dev(g2d, lower_32_bits(dst_dma), WB_LADD0);
			g2d_writel_dev(g2d, (u32)((dst_dma >> 24) & 0xFF), WB_HADD0);
			g2d_writel_dev(g2d, dst_pitch, WB_PITCH0);
			g2d_writel_dev(g2d, (dst_w - 1) | ((dst_h - 1) << 16), WB_SIZE);
			g2d_writel_dev(g2d, wb_att, WB_ATT);
		dev_dbg(g2d->dev, "WB: ATT=0x%08x LADD0=0x%08x HADD0=0x%08x PITCH0=0x%08x SIZE=0x%08x\n",
			g2d_readl_dev(g2d, WB_ATT),
			g2d_readl_dev(g2d, WB_LADD0),
			g2d_readl_dev(g2d, WB_HADD0),
			g2d_readl_dev(g2d, WB_PITCH0),
			g2d_readl_dev(g2d, WB_SIZE));

			/* BLD: ruta mínima V0 -> salida (WB) */
			g2d_writel_dev(g2d, (dst_w - 1) | ((dst_h - 1) << 16), BLD_OUT_SIZE);
			g2d_writel_dev(g2d, (src_w - 1) | ((src_h - 1) << 16), BLD_CH_ISIZE0);
			/* habilitar PIPE0 */
			g2d_writel_dev(g2d, BLD_PIPE0_EN, BLD_EN_CTL);
		dev_dbg(g2d->dev, "BLD: EN_CTL=0x%08x OUT_SIZE=0x%08x CH0_ISIZE=0x%08x\n",
			g2d_readl_dev(g2d, BLD_EN_CTL),
			g2d_readl_dev(g2d, BLD_OUT_SIZE),
			g2d_readl_dev(g2d, BLD_CH_ISIZE0));

			/* Nota: ya no hacemos fallback inmediato por lecturas iguales; esperamos al timeout */

			/* IRQ de MIXER: limpia pending y habilita */
			g2d_writel_dev(g2d, G2D_MIXER_INT_IRQ_PENDING, G2D_MIXER_INT);
			g2d_writel_dev(g2d, G2D_MIXER_INT_FINISH_IRQ_EN, G2D_MIXER_INT);

			/* START (bit 31 en T113) */
			g2d_writel_dev(g2d, G2D_MIXER_CTL_START, G2D_MIXER_CTL);

			/* Timeout de seguridad */
			schedule_delayed_work(&ctx->timeout_work, msecs_to_jiffies(50));
			dev_dbg(g2d->dev, "device_run: scheduled HW blit %ux%u -> %ux%u (INT=0x%08x CTL=0x%08x)\n",
					src_w, src_h, dst_w, dst_h,
					g2d_readl_dev(g2d, G2D_MIXER_INT),
					g2d_readl_dev(g2d, G2D_MIXER_CTL));
			return;
		}
	}

	// Fallback: copia por CPU (simple y lenta), finaliza sin IRQ
	{
		void *src_v = vb2_plane_vaddr(&src->vb2_buf, 0);
		void *dst_v = vb2_plane_vaddr(&dst->vb2_buf, 0);
		size_t rows = min_t(u32, src_h, dst_h);
		size_t bytes_per_row = min_t(u32, src_pitch, dst_pitch);
		size_t i;
		if (src_v && dst_v) {
			for (i = 0; i < rows; i++)
				memcpy(dst_v + i * dst_pitch, src_v + i * src_pitch, bytes_per_row);
			dev_dbg(g2d->dev, "device_run: memcpy fallback %ux%u bytes/row=%zu\n", src_w, src_h, bytes_per_row);
		} else {
			dev_dbg(g2d->dev, "device_run: memcpy fallback but vaddr NULL (src=%p dst=%p)\n", src_v, dst_v);
		}
	}

	// Completa buffers (ruta CPU)
	dst->sequence = src->sequence;
	dst->field = V4L2_FIELD_NONE;
	dst->vb2_buf.timestamp = ktime_get_ns();
	v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);
	v4l2_m2m_buf_done(dst, VB2_BUF_STATE_DONE);
	v4l2_m2m_job_finish(g2d->m2m_dev, ctx->fh.m2m_ctx);
}

static irqreturn_t g2d_irq(int irq, void *data)
{
	struct sunxi_g2d_dev *g2d = data;
	unsigned long flags;
	struct sunxi_g2d_ctx *ctx;
	u32 st;

	// Lee y limpia IRQ del MIXER (v2: write-back para clear)
	st = g2d_readl_dev(g2d, G2D_MIXER_INT);
	if (!(st & G2D_MIXER_INT_IRQ_PENDING))
		return IRQ_NONE;
	g2d_writel_dev(g2d, st, G2D_MIXER_INT);
	dev_dbg(g2d->dev, "irq: MIXER_INT=0x%08x cleared, finishing job\n", st);

	// Finaliza el job en curso si hay
	spin_lock_irqsave(&g2d->irqlock, flags);
	ctx = g2d->curr_ctx;
	g2d->curr_ctx = NULL;
	spin_unlock_irqrestore(&g2d->irqlock, flags);

	if (ctx && !ctx->job_done) {
		struct vb2_v4l2_buffer *src = ctx->cur_src;
		struct vb2_v4l2_buffer *dst = ctx->cur_dst;
		cancel_delayed_work(&ctx->timeout_work);
		if (src && dst) {
			dst->sequence = src->sequence;
			dst->field = V4L2_FIELD_NONE;
			dst->vb2_buf.timestamp = ktime_get_ns();
			v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
			v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
			v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);
			v4l2_m2m_buf_done(dst, VB2_BUF_STATE_DONE);
		}
		ctx->job_done = true;
		v4l2_m2m_job_finish(g2d->m2m_dev, ctx->fh.m2m_ctx);
	}

	return IRQ_HANDLED;
}

// Timeout: finaliza el trabajo si no llegó IRQ a tiempo
static void g2d_timeout_workfn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct sunxi_g2d_ctx *ctx = container_of(dwork, struct sunxi_g2d_ctx, timeout_work);
	struct sunxi_g2d_dev *g2d = ctx->g2d;
	unsigned long flags;

	// Si ya está marcado done por IRQ, nada que hacer
	if (ctx->job_done)
		return;

	// Desengancha el contexto si sigue como actual
	spin_lock_irqsave(&g2d->irqlock, flags);
	if (g2d->curr_ctx == ctx)
		g2d->curr_ctx = NULL;
	spin_unlock_irqrestore(&g2d->irqlock, flags);

	// Completa buffers de forma conservadora; intenta copiar para no dejar salida a cero
	dev_warn(g2d->dev, "timeout: HW completion not observed, finalizing conservatively (will fallback next ops)\n");
	/* Marca HW como roto para siguientes trabajos */
	g2d->hw_broken = true;
	if (ctx->cur_src && ctx->cur_dst) {
		struct vb2_v4l2_buffer *src = ctx->cur_src;
		struct vb2_v4l2_buffer *dst = ctx->cur_dst;
		void *src_v = vb2_plane_vaddr(&src->vb2_buf, 0);
		void *dst_v = vb2_plane_vaddr(&dst->vb2_buf, 0);
		u32 src_h = ctx->out_fmt.height;
		u32 dst_h = ctx->cap_fmt.height;
		u32 src_pitch = ctx->out_fmt.bytesperline;
		u32 dst_pitch = ctx->cap_fmt.bytesperline;
		size_t rows = min_t(u32, src_h, dst_h);
		size_t bytes_per_row = min_t(u32, src_pitch, dst_pitch);

		if (src_v && dst_v) {
			size_t i;
			for (i = 0; i < rows; i++)
				memcpy(dst_v + i * dst_pitch, src_v + i * src_pitch, bytes_per_row);
		}

		dst->sequence = src->sequence;
		dst->field = V4L2_FIELD_NONE;
		dst->vb2_buf.timestamp = ktime_get_ns();
		v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);
		v4l2_m2m_buf_done(dst, VB2_BUF_STATE_DONE);
	}
	ctx->job_done = true;
	v4l2_m2m_job_finish(g2d->m2m_dev, ctx->fh.m2m_ctx);
}

static const struct v4l2_m2m_ops g2d_m2m_ops = {
	.device_run = g2d_device_run,
};

static int g2d_job_ready(void *priv)
{
	struct sunxi_g2d_ctx *ctx = priv;
	return v4l2_m2m_num_src_bufs_ready(ctx->fh.m2m_ctx) > 0 &&
		   v4l2_m2m_num_dst_bufs_ready(ctx->fh.m2m_ctx) > 0;
}

static void g2d_job_abort(void *priv)
{
	struct sunxi_g2d_ctx *ctx = priv;
	struct sunxi_g2d_dev *g2d = ctx->g2d;
	unsigned long flags;
	cancel_delayed_work_sync(&ctx->timeout_work);
	spin_lock_irqsave(&g2d->irqlock, flags);
	if (g2d->curr_ctx == ctx)
		g2d->curr_ctx = NULL;
	spin_unlock_irqrestore(&g2d->irqlock, flags);
}

static const struct v4l2_m2m_ops g2d_m2m_ops_full = {
	.device_run = g2d_device_run,
	.job_ready  = g2d_job_ready,
	.job_abort  = g2d_job_abort,
};
// v4l2-mem2mem (antiguas) requieren queue_init callback
static int g2d_queue_init(void *priv, struct vb2_queue *out, struct vb2_queue *cap)
{
	struct sunxi_g2d_ctx *ctx = priv;

	memset(out, 0, sizeof(*out));
	out->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	out->io_modes = VB2_MMAP;
	out->drv_priv = ctx;
	out->buf_struct_size = sizeof(struct vb2_v4l2_buffer);
	out->ops = &qbuf_qops;
	out->mem_ops = &vb2_dma_contig_memops;
	out->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	out->lock = &ctx->g2d->dev_mutex;
	out->dev = ctx->g2d->dev;
	if (vb2_queue_init(out))
		return -EINVAL;

	memset(cap, 0, sizeof(*cap));
	*cap = *out;
	cap->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	return vb2_queue_init(cap);
}

// ========== IOCTLs ==========
static inline struct sunxi_g2d_ctx *file2ctx(struct file *filp)
{
	struct v4l2_fh *fh = filp->private_data;
	return container_of(fh, struct sunxi_g2d_ctx, fh);
}

static int g2d_querycap(struct file *filp, void *priv, struct v4l2_capability *cap)
{
	struct sunxi_g2d_dev *g2d = video_drvdata(filp);
	strscpy(cap->driver, DRV_NAME, sizeof(cap->driver));
	strscpy(cap->card, "sunxi-g2d-m2m", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s", DRV_NAME);
	cap->device_caps = g2d->vfd.device_caps;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}
static int g2d_enum_fmt_out(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(g_formats))
		return -EINVAL;
	f->pixelformat = g_formats[f->index].fourcc;
	strscpy(f->description, "32-bit RGBX", sizeof(f->description));
	return 0;
}

static int g2d_enum_fmt_cap(struct file *file, void *priv, struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(g_formats))
		return -EINVAL;
	f->pixelformat = g_formats[f->index].fourcc;
	strscpy(f->description, "32-bit RGBX", sizeof(f->description));
	return 0;
}
static int g2d_try_fmt(struct file *filp, void *priv, struct v4l2_format *f)
{
	const struct sunxi_g2d_fmt *fmt = find_fmt(f->fmt.pix.pixelformat);
	if (!fmt)
		fmt = &g_formats[0];
	f->fmt.pix.pixelformat = fmt->fourcc;

	// clamp tamaños
	if (!f->fmt.pix.width)  f->fmt.pix.width  = 16;
	if (!f->fmt.pix.height) f->fmt.pix.height = 16;

	// bytes por línea e imagen
	f->fmt.pix.bytesperline = (f->fmt.pix.width * (fmt->depth / 8));
	f->fmt.pix.sizeimage = f->fmt.pix.bytesperline * f->fmt.pix.height;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	return 0;
}

static int g2d_g_fmt_out(struct file *filp, void *priv, struct v4l2_format *f)
{
	struct sunxi_g2d_ctx *ctx = file2ctx(filp);
	f->fmt.pix = ctx->out_fmt;
	return 0;
}

static int g2d_g_fmt_cap(struct file *filp, void *priv, struct v4l2_format *f)
{
	struct sunxi_g2d_ctx *ctx = file2ctx(filp);
	f->fmt.pix = ctx->cap_fmt;
	return 0;
}

static int g2d_s_fmt_out(struct file *filp, void *priv, struct v4l2_format *f)
{
	struct sunxi_g2d_ctx *ctx = file2ctx(filp);
	int ret = g2d_try_fmt(filp, priv, f);
	if (ret) return ret;
	ctx->out_fmt = f->fmt.pix;
	return 0;
}

static int g2d_s_fmt_cap(struct file *filp, void *priv, struct v4l2_format *f)
{
	struct sunxi_g2d_ctx *ctx = file2ctx(filp);
	int ret = g2d_try_fmt(filp, priv, f);
	if (ret) return ret;
	ctx->cap_fmt = f->fmt.pix;
	return 0;
}

/* no custom reqbufs; vb2 helpers se encargan */

static const struct v4l2_ioctl_ops g2d_ioctl_ops = {
	.vidioc_querycap                = g2d_querycap,
	.vidioc_enum_fmt_vid_cap        = g2d_enum_fmt_cap,
	.vidioc_enum_fmt_vid_out        = g2d_enum_fmt_out,
	.vidioc_g_fmt_vid_cap           = g2d_g_fmt_cap,
	.vidioc_s_fmt_vid_cap           = g2d_s_fmt_cap,
	.vidioc_try_fmt_vid_cap         = g2d_try_fmt,
	.vidioc_g_fmt_vid_out           = g2d_g_fmt_out,
	.vidioc_s_fmt_vid_out           = g2d_s_fmt_out,
	.vidioc_try_fmt_vid_out         = g2d_try_fmt,
	/* no custom reqbufs needed; use v4l2_m2m ioctl helpers */
	.vidioc_reqbufs                 = v4l2_m2m_ioctl_reqbufs,
	.vidioc_create_bufs             = v4l2_m2m_ioctl_create_bufs,
	.vidioc_querybuf                = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf                    = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf                   = v4l2_m2m_ioctl_dqbuf,
	.vidioc_expbuf                  = v4l2_m2m_ioctl_expbuf,

	.vidioc_streamon                = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff               = v4l2_m2m_ioctl_streamoff,
};

// ========== File ops / vdev ==========
static int g2d_open(struct file *filp)
{
	struct sunxi_g2d_dev *g2d = video_drvdata(filp);
	struct sunxi_g2d_ctx *ctx;
	int ret;

	mutex_lock(&g2d->dev_mutex);

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) { ret = -ENOMEM; goto err; }

	ctx->g2d = g2d;
	v4l2_fh_init(&ctx->fh, &g2d->vfd);
	filp->private_data = &ctx->fh;

	// formatos por defecto
	ctx->out_fmt.pixelformat = g_formats[0].fourcc;
	ctx->out_fmt.width = 640; ctx->out_fmt.height = 360;
	ctx->out_fmt.bytesperline = 640*4; ctx->out_fmt.sizeimage = 640*360*4;

	ctx->cap_fmt.pixelformat = g_formats[0].fourcc;
	ctx->cap_fmt.width = 320; ctx->cap_fmt.height = 180;
	ctx->cap_fmt.bytesperline = 320*4; ctx->cap_fmt.sizeimage = 320*180*4;

	// init timeout work
	INIT_DELAYED_WORK(&ctx->timeout_work, g2d_timeout_workfn);

	// m2m context (las colas se inicializan vía queue_init)
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(g2d->m2m_dev, ctx, g2d_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) { ret = PTR_ERR(ctx->fh.m2m_ctx); goto err_fh; }

	v4l2_fh_add(&ctx->fh, filp);
	mutex_unlock(&g2d->dev_mutex);
	return 0;

err_fh:
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
err:
	mutex_unlock(&g2d->dev_mutex);
	return ret;
}

static int g2d_release(struct file *filp)
{
	struct sunxi_g2d_dev *g2d = video_drvdata(filp);
	struct v4l2_fh *fh = filp->private_data;
	struct sunxi_g2d_ctx *ctx = container_of(fh, struct sunxi_g2d_ctx, fh);

	mutex_lock(&g2d->dev_mutex);
	cancel_delayed_work_sync(&ctx->timeout_work);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_del(&ctx->fh, filp);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	mutex_unlock(&g2d->dev_mutex);
	return 0;
}

static const struct v4l2_file_operations g2d_fops = {
	.owner          = THIS_MODULE,
	.open           = g2d_open,
	.release        = g2d_release,
	.unlocked_ioctl = video_ioctl2,
	.poll           = v4l2_m2m_fop_poll,
	.mmap           = v4l2_m2m_fop_mmap,
};

// ========== Clock Diagnostics ==========
static void g2d_clock_debug(struct platform_device *pdev)
{
	struct clk *clk;
	const char *clk_names[] = {
		"bus", "ahb", "hclk", "mod", "g2d", 
		"mbus", "mbus_g2d", "ram", NULL
	};
	int i;
	
	dev_info(&pdev->dev, "=== G2D Clock Debug ===\n");
	
	for (i = 0; clk_names[i]; i++) {
		clk = devm_clk_get_optional(&pdev->dev, clk_names[i]);
		if (IS_ERR(clk)) {
			dev_info(&pdev->dev, "Clock '%s': error %ld\n", 
				 clk_names[i], PTR_ERR(clk));
		} else if (clk) {
			unsigned long rate = clk_get_rate(clk);
			
			dev_info(&pdev->dev, "Clock '%s': FOUND rate=%lu Hz\n",
				 clk_names[i], rate);
			
			/* Intenta preparar y habilitar temporalmente para diagnóstico */
			int ret = clk_prepare_enable(clk);
			if (ret) {
				dev_info(&pdev->dev, "  -> enable FAILED: %d\n", ret);
			} else {
				unsigned long new_rate = clk_get_rate(clk);
				dev_info(&pdev->dev, "  -> enabled OK, rate=%lu Hz\n", new_rate);
				clk_disable_unprepare(clk);
			}
		} else {
			dev_info(&pdev->dev, "Clock '%s': not found (NULL)\n", clk_names[i]);
		}
	}
	
	dev_info(&pdev->dev, "=== End Clock Debug ===\n");
}

/* Diagnóstico directo de registros CCU - solo para debugging */
static void g2d_ccu_registers_debug(struct platform_device *pdev)
{
	void __iomem *ccu_base;
	struct device_node *ccu_np;
	
	ccu_np = of_find_compatible_node(NULL, NULL, "allwinner,sun8i-t113-ccu");
	if (!ccu_np) {
		dev_warn(&pdev->dev, "CCU node not found\n");
		return;
	}
	
	ccu_base = of_iomap(ccu_np, 0);
	of_node_put(ccu_np);
	if (!ccu_base) {
		dev_warn(&pdev->dev, "Failed to map CCU\n");
		return;
	}
	
	dev_info(&pdev->dev, "=== CCU G2D Register Dump ===\n");
	dev_info(&pdev->dev, "  0x630 (G2D_CLK):    0x%08x\n", readl(ccu_base + 0x630));
	dev_info(&pdev->dev, "  0x63c (BUS_GATE):   0x%08x\n", readl(ccu_base + 0x63c));
	dev_info(&pdev->dev, "  0x804 (MBUS_GATE):  0x%08x\n", readl(ccu_base + 0x804));
	dev_info(&pdev->dev, "=== End CCU Debug ===\n");
	
	iounmap(ccu_base);
}

/* Intentar configurar MIXER clock via RCQ (para T113) */
static void g2d_try_mixer_clock_rcq(struct sunxi_g2d_dev *g2d, bool enable)
{
	u32 rcq_status;
	
	dev_info(g2d->dev, "=== Trying MIXER clock via RCQ ===\n");
	
	/* Verificar si RCQ está disponible */
	rcq_status = __g2d_readl(g2d->mmio, G2D_RCQ_STATUS);
	dev_info(g2d->dev, "  RCQ_STATUS before: 0x%08x\n", rcq_status);
	
	/* Intentar habilitar RCQ temporalmente para configurar el clock */
	__g2d_writel(g2d->mmio, G2D_RCQ_CTRL_UPDATE, G2D_RCQ_CTRL);
	
	/* El método específico puede requerir documentación del T113 */
	dev_info(g2d->dev, "  RCQ method attempted (implementation-specific)\n");
	
	/* Restaurar RCQ a deshabilitado */
	__g2d_writel(g2d->mmio, 0x0, G2D_RCQ_CTRL);
	
	dev_info(g2d->dev, "=== End RCQ attempt ===\n");
}

/* Buscar el registro MIXER_CLK correcto - el T113 puede tener layout diferente */
static u32 find_mixer_clock_reg(struct sunxi_g2d_dev *g2d)
{
	/* Posibles ubicaciones del MIXER_CLK en diferentes layouts:
	 * 0x108    - G2D v2 estándar (bias=0)
	 * 0x0108   - Explícito con leading zero
	 * 0x28108  - Layout RCQ del T113 (MIXER en espacio RCQ 0x28000)
	 * 0x30108  - Otra posible ubicación RCQ
	 * 0x00028  - Offset alternativo observado en algunos SoCs
	 */
	u32 test_locations[] = {0x108, 0x0108, 0x28108, 0x30108, 0x00028, 0};
	int i;
	
	dev_info(g2d->dev, "=== Scanning for MIXER_CLK register ===\n");
	
	for (i = 0; test_locations[i]; i++) {
		u32 off = test_locations[i];
		u32 orig, readback;
		
		/* Verificar que el offset está dentro del rango mapeado */
		if (off >= g2d->mmio_size) {
			dev_info(g2d->dev, "  0x%05x: SKIP (out of range)\n", off);
			continue;
		}
		
		orig = __g2d_readl(g2d->mmio, off);
		__g2d_writel(g2d->mmio, 0xA5A5A5A5, off);
		readback = __g2d_readl(g2d->mmio, off);
		__g2d_writel(g2d->mmio, orig, off); /* Restaura valor original */
		
		dev_info(g2d->dev, "  0x%05x: orig=0x%08x write=0xA5A5A5A5 read=0x%08x %s\n",
			 off, orig, readback, 
			 (readback == 0xA5A5A5A5) ? "✓ WRITABLE" : "✗ readonly/different");
		
		if (readback == 0xA5A5A5A5) {
			dev_info(g2d->dev, "=== MIXER_CLK candidate found at 0x%05x ===\n", off);
			return off;
		}
	}
	
	dev_warn(g2d->dev, "=== No writable MIXER_CLK register found ===\n");
	return 0;
}

/* Verificación del MIXER clock interno */
static void g2d_check_mixer_clock(struct platform_device *pdev, 
                                  struct sunxi_g2d_dev *g2d)
{
	u32 mixer_clk_offset;
	u32 mixer_clk, mixer_ctl, mixer_int;
	
	/* Buscar el registro MIXER_CLK correcto */
	mixer_clk_offset = find_mixer_clock_reg(g2d);
	
	if (mixer_clk_offset) {
		dev_info(&pdev->dev, "=== Using MIXER_CLK at offset 0x%05x ===\n", mixer_clk_offset);
		
		/* Intentar habilitar el clock */
		__g2d_writel(g2d->mmio, 0x1, mixer_clk_offset);
		mixer_clk = __g2d_readl(g2d->mmio, mixer_clk_offset);
		
		dev_info(&pdev->dev, "  MIXER_CLK after enable: 0x%08x (bit0=%s)\n",
			 mixer_clk, (mixer_clk & 0x1) ? "ENABLED" : "disabled");
		
		/* Guardar el offset encontrado para uso posterior */
		// Podrías añadir g2d->mixer_clk_offset = mixer_clk_offset;
	} else {
		dev_warn(&pdev->dev, "=== MIXER_CLK not found - trying RCQ method ===\n");
		
		/* Intentar método RCQ */
		g2d_try_mixer_clock_rcq(g2d, true);
		
		/* Leer el registro estándar para ver si RCQ lo activó */
		mixer_clk = g2d_readl_dev(g2d, G2D_MIXER_CLK);
	}
	
	mixer_ctl = g2d_readl_dev(g2d, G2D_MIXER_CTL);
	mixer_int = g2d_readl_dev(g2d, G2D_MIXER_INT);
	
	dev_info(&pdev->dev, "=== MIXER Status ===\n");
	dev_info(&pdev->dev, "  MIXER_CLK=0x%08x\n", mixer_clk);
	dev_info(&pdev->dev, "  MIXER_CTL=0x%08x\n", mixer_ctl);
	dev_info(&pdev->dev, "  MIXER_INT=0x%08x\n", mixer_int);
}

// ========== Probe / Remove ==========
static int sunxi_g2d_probe(struct platform_device *pdev)
{
	/* Asegura dominio de energía encendido (si aplica) */
	pm_runtime_enable(&pdev->dev);
	int ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret < 0)
		dev_warn(&pdev->dev, "pm_runtime get failed: %d (continuing)\n", ret);
	struct sunxi_g2d_dev *g2d;
	struct resource *res;

	g2d = devm_kzalloc(&pdev->dev, sizeof(*g2d), GFP_KERNEL);
	if (!g2d) return -ENOMEM;

	g2d->dev = &pdev->dev;
	mutex_init(&g2d->dev_mutex);
	spin_lock_init(&g2d->irqlock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	g2d->mmio = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(g2d->mmio)) return PTR_ERR(g2d->mmio);
	g2d->mmio_size = resource_size(res);

	dev_info(&pdev->dev, "G2D MMIO base=%pa size=0x%llx\n", &res->start, (unsigned long long)g2d->mmio_size);
	if (g2d->mmio_size < (WB_LADD0 + 0x30))
		dev_warn(&pdev->dev, "g2d reg size too small to reach WB block; HW path will be disabled\n");

	/* Detect hardware variant (T113 vs legacy) */
	const struct sunxi_g2d_variant *variant = of_device_get_match_data(&pdev->dev);
	if (!variant)
		variant = &default_variant;  /* Fallback to legacy */

	g2d->use_rcq = variant->use_rcq;
	g2d->bias_subblocks = variant->bias_subblocks;
	dev_info(&pdev->dev, "Variant: use_rcq=%d bias_subblocks=0x%05x\n",
		 g2d->use_rcq, g2d->bias_subblocks);

	/* T113-specific: Map CCU for G2D_CLK_REG and G2D_BGR_REG access */
	if (g2d->use_rcq) {
		g2d->ccu = ioremap(0x02001000, 0x1000);  /* CCU base address from T113 User Manual */
		if (!g2d->ccu) {
			dev_err(&pdev->dev, "Failed to map CCU registers\n");
			return -ENOMEM;
		}
		dev_info(&pdev->dev, "CCU mapped at 0x02001000 for G2D_CLK_REG/G2D_BGR_REG access\n");
		
		/* T113-specific: Check and configure IOMMU for G2D */
		void __iomem *iommu = ioremap(0x02010000, 0x1000);  /* IOMMU base */
		if (iommu) {
			u32 iommu_enable = readl(iommu + 0x0020);
			u32 iommu_bypass = readl(iommu + 0x0030);
			
			dev_info(&pdev->dev, "IOMMU: ENABLE=0x%08x BYPASS=0x%08x (bit3=G2D)\n",
				 iommu_enable, iommu_bypass);
			
			/* Ensure G2D bypass is enabled (bit 3) */
			if (!(iommu_bypass & BIT(3))) {
				dev_warn(&pdev->dev, "IOMMU: G2D bypass disabled, enabling it\n");
				iommu_bypass |= BIT(3);
				writel(iommu_bypass, iommu + 0x0030);
				iommu_bypass = readl(iommu + 0x0030);
				dev_info(&pdev->dev, "IOMMU: BYPASS now = 0x%08x\n", iommu_bypass);
			}
			
			/* For testing: disable IOMMU completely if enabled */
			if (iommu_enable & BIT(0)) {
				dev_warn(&pdev->dev, "IOMMU: Enabled, disabling for G2D testing\n");
				writel(0, iommu + 0x0020);
				iommu_enable = readl(iommu + 0x0020);
				dev_info(&pdev->dev, "IOMMU: ENABLE now = 0x%08x\n", iommu_enable);
			}
			
			iounmap(iommu);
		} else {
			dev_warn(&pdev->dev, "Failed to map IOMMU registers\n");
		}
	}

	/* Diagnóstico exhaustivo de clocks ANTES de inicializarlos */
	g2d_clock_debug(pdev);
	g2d_ccu_registers_debug(pdev);

	/* Clock configuration corrected for kernel 6.17 */
	/* Bus clock (AHB) - required */
	g2d->clk = devm_clk_get(&pdev->dev, "bus");
	if (IS_ERR(g2d->clk)) {
		dev_warn(&pdev->dev, "bus clock not found, trying fallback\n");
		g2d->clk = devm_clk_get(&pdev->dev, NULL);
	}
	if (!IS_ERR(g2d->clk)) {
		ret = clk_prepare_enable(g2d->clk);
		if (ret) {
			dev_err(&pdev->dev, "Failed to enable bus clock: %d\n", ret);
			goto err_clk;
		}
		dev_info(&pdev->dev, "clk bus enabled (rate=%lu)\n", clk_get_rate(g2d->clk));
	} else {
		dev_warn(&pdev->dev, "no bus clock available\n");
		g2d->clk = NULL;
	}

	/* G2D engine clock - use "g2d" not "mod" */
	g2d->clk_mod = devm_clk_get_optional(&pdev->dev, "g2d");
	if (!IS_ERR_OR_NULL(g2d->clk_mod)) {
		ret = clk_prepare_enable(g2d->clk_mod);
		if (ret) {
			dev_err(&pdev->dev, "Failed to enable g2d clock: %d\n", ret);
			goto err_clk_mod;
		}
		/* Configurar a 300 MHz */
		if (clk_set_rate(g2d->clk_mod, 300000000))
			dev_warn(&pdev->dev, "Failed to set g2d clock rate\n");
		dev_info(&pdev->dev, "clk g2d enabled (rate=%lu)\n", clk_get_rate(g2d->clk_mod));
	} else {
		dev_warn(&pdev->dev, "no g2d engine clock found - HW acceleration disabled\n");
		g2d->clk_mod = NULL;
	}

	/* MBUS clock for DDR access */
	g2d->clk_mbus = devm_clk_get_optional(&pdev->dev, "mbus_g2d");
	if (!IS_ERR_OR_NULL(g2d->clk_mbus)) {
		ret = clk_prepare_enable(g2d->clk_mbus);
		if (ret) {
			dev_err(&pdev->dev, "Failed to enable mbus_g2d clock: %d\n", ret);
			goto err_clk_mbus;
		}
		dev_info(&pdev->dev, "clk mbus_g2d enabled (rate=%lu)\n", clk_get_rate(g2d->clk_mbus));
	} else {
		dev_info(&pdev->dev, "clk mbus: not present\n");
	}

	/* reset may be optional on some boards */
	g2d->rst = devm_reset_control_get_optional_exclusive(&pdev->dev, "bus");
	if (IS_ERR(g2d->rst)) {
			if (PTR_ERR(g2d->rst) == -EPROBE_DEFER)
					return -EPROBE_DEFER;

			/* Prueba alias de vendor */
			g2d->rst = devm_reset_control_get_optional_exclusive(&pdev->dev, "g2d");
	}
	if (IS_ERR(g2d->rst)) {
			/* Sin nombres en el DT: prueba por índice 0 */
			g2d->rst = devm_reset_control_get_optional_exclusive(&pdev->dev, NULL);
	}
	if (IS_ERR(g2d->rst)) {
			dev_warn(&pdev->dev, "no reset handle found, continuing without\n");
			g2d->rst = NULL;
	} else {
			int ret = reset_control_deassert(g2d->rst);
			if (ret)
					return ret;
	}
	if (g2d->rst)
		reset_control_deassert(g2d->rst);
	dev_info(&pdev->dev, "reset deasserted (%s)\n", g2d->rst ? "ok" : "none");

	/* ========== Hardware initialization (variant-specific) ========== */
	
	if (g2d->use_rcq) {
		/* T113-specific: Use internal BGR and CLK registers in CCU */
		dev_info(&pdev->dev, "T113 mode: Using CCU registers for G2D gating/reset/clock\n");
		
		/* FIX #2: Separate reset sequence - assert, de-assert, then gate */
		writel(0, g2d->ccu + G2D_BGR_REG);  // Assert reset
		udelay(10);
		
		writel(G2D_BGR_RST, g2d->ccu + G2D_BGR_REG);  // De-assert reset ONLY
		udelay(100);
		
		writel(G2D_BGR_RST | G2D_BGR_GATING, g2d->ccu + G2D_BGR_REG);  // Now enable gating
		udelay(100);
		
		dev_info(&pdev->dev, "G2D_BGR_REG (CCU+0x%03x) = 0x%08x\n",
			 G2D_BGR_REG, readl(g2d->ccu + G2D_BGR_REG));
		
		/* FIX #1: Try src=1 (PLL_PERI0(2X)) - alternative if PLL_DE doesn't work */
		u32 clk_val = G2D_CLK_GATING | (0x1 << G2D_CLK_SRC_SEL_SHIFT) | (0 & G2D_CLK_FACTOR_M_MASK);
		writel(clk_val, g2d->ccu + G2D_CLK_REG);
		udelay(100);
		
		dev_info(&pdev->dev, "G2D_CLK_REG (CCU+0x%03x) = 0x%08x (src=PLL_PERI0(2X))\n",
			 G2D_CLK_REG, readl(g2d->ccu + G2D_CLK_REG));
		
		/* Verify actual clock rate from framework */
		if (g2d->clk_mod) {
			dev_info(&pdev->dev, "G2D module clock actual rate: %lu Hz\n",
				 clk_get_rate(g2d->clk_mod));
		}
		
		/* Check if RCQ subsystem is alive after BGR/CLK configuration */
		dev_info(&pdev->dev, "RCQ_STATUS after BGR/CLK init: 0x%08x\n",
			 __g2d_readl(g2d->mmio, G2D_RCQ_STATUS));
		
	} else {
		/* Legacy mode: Use TOP gates (SCLK_GATE, HCLK_GATE, AHB_RESET) */
		dev_info(&pdev->dev, "Legacy mode: Using TOP gates\n");
		
		__g2d_writel(g2d->mmio, 0x3, G2D_SCLK_GATE);  /* MIXER + ROT */
		__g2d_writel(g2d->mmio, 0x3, G2D_HCLK_GATE);
		__g2d_writel(g2d->mmio, 0x3, G2D_AHB_RESET);
		udelay(10);
		
		/* Try MIXER_CLK (may not exist on all SoCs) */
		g2d_writel_dev(g2d, 0x1, G2D_MIXER_CLK);
		udelay(100);
		
		dev_info(&pdev->dev, "TOP gates: SCLK=0x%08x HCLK=0x%08x AHB_RST=0x%08x\n",
			__g2d_readl(g2d->mmio, G2D_SCLK_GATE),
			__g2d_readl(g2d->mmio, G2D_HCLK_GATE),
			__g2d_readl(g2d->mmio, G2D_AHB_RESET));
		
		dev_info(&pdev->dev, "MIXER_CLK: 0x%08x\n",
			g2d_readl_dev(g2d, G2D_MIXER_CLK));
		
		/* Verify MIXER clock state */
		g2d_check_mixer_clock(pdev, g2d);
	}
	
	/* ============================================================================ */

	/* Layout estándar sin bias */
	g2d->reg_shift = 0;
	g2d->bias_subblocks = 0;
	g2d->bias_mixer = g2d->bias_bld = g2d->bias_v0 = g2d->bias_wb = 0;
	dev_info(&pdev->dev, "Standard G2D v2 layout: MIXER@0x100 V0@0x800 BLD@0x400 WB@0x3000\n");

	/* Habilita la IRQ de FINISH del MIXER y limpia pending (W1C) */
	g2d_writel_dev(g2d, G2D_MIXER_INT_FINISH_IRQ_EN, G2D_MIXER_INT);
	g2d_writel_dev(g2d, G2D_MIXER_INT_IRQ_PENDING, G2D_MIXER_INT);

	dev_info(&pdev->dev, "MIXER pre: CLK=0x%08x CTL=0x%08x INT=0x%08x\n",
		g2d_readl_dev(g2d, G2D_MIXER_CLK),
		g2d_readl_dev(g2d, G2D_MIXER_CTL),
		g2d_readl_dev(g2d, G2D_MIXER_INT));

	if (self_test) {
		struct device_node *ccu_np;
		void __iomem *ccu_base;
		u32 ccu_bus_gate = 0, ccu_g2d_clk = 0, ccu_mbus_gate = 0;
		bool ccu_reset_released = false;

		ccu_np = of_parse_phandle(pdev->dev.of_node, "clocks", 0);
		if (!ccu_np) {
			dev_info(&pdev->dev, "self-test: no CCU phandle found on clocks[0]\n");
			goto skip_ccu_dump;
		}

		ccu_base = of_iomap(ccu_np, 0);
		of_node_put(ccu_np);
		if (!ccu_base) {
			dev_info(&pdev->dev, "self-test: unable to ioremap CCU base\n");
			goto skip_ccu_dump;
		}

		ccu_bus_gate = readl(ccu_base + 0x63c);
		ccu_g2d_clk = readl(ccu_base + 0x630);
		ccu_mbus_gate = readl(ccu_base + 0x804);
		ccu_reset_released = !!(ccu_bus_gate & BIT(16));
		iounmap(ccu_base);

		dev_info(&pdev->dev,
			 "CCU snapshot: BUS_GATE=0x%08x G2D_CLK=0x%08x MBUS_GATE=0x%08x RST_BUS_G2D=%s\n",
			 ccu_bus_gate, ccu_g2d_clk, ccu_mbus_gate,
			 ccu_reset_released ? "released" : "asserted");

skip_ccu_dump:
		/* Vuelca solo registros TOP seguros (evita offsets que puedan colgar) */
		dev_info(&pdev->dev, "MMIO dump (safe registers only):\n");
		dev_info(&pdev->dev, "  SCLK_GATE=0x%08x HCLK_GATE=0x%08x AHB_RESET=0x%08x\n",
			 __g2d_readl(g2d->mmio, G2D_SCLK_GATE),
			 __g2d_readl(g2d->mmio, G2D_HCLK_GATE),
			 __g2d_readl(g2d->mmio, G2D_AHB_RESET));
		
		/* Solo lee sub-bloques si están dentro del rango mapeado */
		if ((g2d_calc_off(g2d, G2D_MIXER_CTL) + 4) <= g2d->mmio_size) {
			dev_info(&pdev->dev, "  MIXER_CTL=0x%08x MIXER_INT=0x%08x\n",
				 g2d_readl_dev(g2d, G2D_MIXER_CTL),
				 g2d_readl_dev(g2d, G2D_MIXER_INT));
		}
		if ((g2d_calc_off(g2d, BLD_EN_CTL) + 4) <= g2d->mmio_size) {
			dev_info(&pdev->dev, "  BLD_EN_CTL=0x%08x\n",
				 g2d_readl_dev(g2d, BLD_EN_CTL));
		}
		if ((g2d_calc_off(g2d, V0_ATTCTL) + 4) <= g2d->mmio_size) {
			dev_info(&pdev->dev, "  V0_ATTCTL=0x%08x\n",
				 g2d_readl_dev(g2d, V0_ATTCTL));
		}
		if ((g2d_calc_off(g2d, WB_ATT) + 4) <= g2d->mmio_size) {
			dev_info(&pdev->dev, "  WB_ATT=0x%08x\n",
				 g2d_readl_dev(g2d, WB_ATT));
		}
		
		/* Lee registros RCQ (estos son offsets directos sin bias) */
		dev_info(&pdev->dev, "  RCQ_IRQ_CTL=0x%08x RCQ_STATUS=0x%08x\n",
			 __g2d_readl(g2d->mmio, G2D_RCQ_IRQ_CTL),
			 __g2d_readl(g2d->mmio, G2D_RCQ_STATUS));
		dev_info(&pdev->dev, "  RCQ_CTRL=0x%08x RCQ_HEAD_LOW=0x%08x\n",
			 __g2d_readl(g2d->mmio, G2D_RCQ_CTRL),
			 __g2d_readl(g2d->mmio, G2D_RCQ_HEAD_LOW));

		/* Prueba fillrect solo si se solicita explícitamente */
		if (fillrect_test) {
			dev_info(&pdev->dev, "=== Testing fillrect via DIRECT mode first (now that CCU is configured) ===\n");
			g2d_hw_fillrect(pdev, g2d);
			dev_info(&pdev->dev, "=== Testing fillrect via RCQ mode (T113 mode) ===\n");
			g2d_hw_fillrect_rcq(pdev, g2d);
		}

		if (rcq_probe)
			g2d_rcq_probe_window(pdev, g2d);
		if (rcq_kick)
			g2d_rcq_kick_hw(pdev, g2d);
	}

	g2d->irq = platform_get_irq(pdev, 0);
	if (g2d->irq > 0) {
		ret = devm_request_irq(&pdev->dev, g2d->irq, g2d_irq, 0, DRV_NAME, g2d);
		if (ret) goto err_clk;
		dev_info(&pdev->dev, "G2D assigned IRQ %d\n", g2d->irq);
	}

	ret = v4l2_device_register(&pdev->dev, &g2d->v4l2_dev);
	if (ret) goto err_clk;

	g2d->m2m_dev = v4l2_m2m_init(&g2d_m2m_ops_full);
	if (IS_ERR(g2d->m2m_dev)) { ret = PTR_ERR(g2d->m2m_dev); goto err_v4l2; }

	strscpy(g2d->vfd.name, DRV_NAME, sizeof(g2d->vfd.name));
	g2d->vfd.v4l2_dev = &g2d->v4l2_dev;
	g2d->vfd.fops = &g2d_fops;
	g2d->vfd.ioctl_ops = &g2d_ioctl_ops;
	g2d->vfd.lock = &g2d->dev_mutex;
	g2d->vfd.device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
	g2d->vfd.release = video_device_release_empty;
	g2d->vfd.vfl_dir = VFL_DIR_M2M;
	video_set_drvdata(&g2d->vfd, g2d);

	ret = video_register_device(&g2d->vfd, VFL_TYPE_VIDEO, -1);
	if (ret) goto err_m2m;

	dev_info(&pdev->dev, "sunxi G2D mem2mem registered as /dev/video%d\n",
		 g2d->vfd.minor);

    platform_set_drvdata(pdev, g2d);

	return 0;

err_m2m:
	v4l2_m2m_release(g2d->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&g2d->v4l2_dev);
err_clk:
	if (!IS_ERR(g2d->rst))
		reset_control_assert(g2d->rst);
	clk_disable_unprepare(g2d->clk);
 err_clk_mod:
	if (!IS_ERR_OR_NULL(g2d->clk_mod))
		clk_disable_unprepare(g2d->clk_mod);
 err_clk_mbus:
	if (!IS_ERR_OR_NULL(g2d->clk_mbus))
		clk_disable_unprepare(g2d->clk_mbus);
	return ret;
}

static void sunxi_g2d_remove(struct platform_device *pdev)
{
	struct sunxi_g2d_dev *g2d = platform_get_drvdata(pdev);

	video_unregister_device(&g2d->vfd);
	v4l2_m2m_release(g2d->m2m_dev);
	v4l2_device_unregister(&g2d->v4l2_dev);
	
	/* T113: Unmap CCU if it was mapped */
	if (g2d->ccu)
		iounmap(g2d->ccu);
	
	if (!IS_ERR(g2d->rst))
		reset_control_assert(g2d->rst);
	clk_disable_unprepare(g2d->clk);
	if (!IS_ERR_OR_NULL(g2d->clk_mod))
		clk_disable_unprepare(g2d->clk_mod);
	if (!IS_ERR_OR_NULL(g2d->clk_mbus))
		clk_disable_unprepare(g2d->clk_mbus);

	pm_runtime_put_sync(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
}

static struct platform_driver sunxi_g2d_driver = {
	.probe  = sunxi_g2d_probe,
	.remove = sunxi_g2d_remove,
	.driver = {
		.name           = DRV_NAME,
		.of_match_table = sunxi_g2d_of_match,
	},
};

module_platform_driver(sunxi_g2d_driver);

MODULE_DESCRIPTION("Allwinner Sunxi G2D V4L2 mem2mem (skeleton)");
MODULE_AUTHOR("Sergio Perez <sergio@pereznus.es>");
MODULE_LICENSE("GPL");
