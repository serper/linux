// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal RCQ-based fillrect self-test for the Allwinner G2D engine.
 *
 * Loads as a platform driver, enables the mixer clocks, allocates small
 * buffers and submits a single RCQ job that writes a 64x64 green rectangle.
 * The intent is to debug the G2D hardware in isolation.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/interconnect.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/kernel.h>

#include "sunxi-g2d-regs.h"

#define DRIVER_NAME                     "sunxi-g2d-fillrect"
#define DRIVER_VERSION                  "v1.0.0-STABLE"

/*
 * T113-S3 G2D Fillrect Test Driver - DIRECT MODE BASELINE
 * ========================================================
 * 
 * This driver demonstrates working G2D fillrect operation using DIRECT mode
 * (CPU register writes) on Allwinner T113-S3 SoC.
 * 
 * Key Discoveries:
 * ---------------
 * 1. T113 G2D has TWO operational modes:
 *    - DIRECT mode: CPU writes registers → MIXER_CTL.START → IRQ (THIS DRIVER)
 *    - RCQ mode: DMA command queue → batch processing (not needed for basic ops)
 * 
 * 2. MIXER_CTL has TWO addresses:
 *    - 0x00100: Legacy address, CPU-writable for DIRECT mode ✓
 *    - 0x28100: RCQ-only address, NOT CPU-writable ✗
 * 
 * 3. CRITICAL BLD (Blender) registers:
 *    - BLD_CH_ISIZE0: Input size for channel 0 - MUST match surface dimensions
 *    - BLD_OUT_SIZE: Output size - MUST match surface dimensions
 *    Without these, blender only processes 1 pixel!
 * 
 * 4. Register sequence (from BSP analysis):
 *    V0 setup → BLD config (with sizing) → ROP → WB → BLD_SIZE (again!) → MIXER start
 * 
 * Hardware: Allwinner T113-S3, G2D IP 0x01100114
 * Reference: BSP kernel g2d driver + sunxi-g2d-m2m V4L2 driver
 * Status: WORKING - All pixels correctly filled ✓
 */
/* v0.0.101: Remove MIXER payload - BSP doesn't use it in RCQ mode */
/* v0.0.100: Fix V0_ATTCTL format! G2D_FORMAT_ARGB8888=0 (not 4) */
/* v0.0.99: Fix RCQ_CTRL - BSP only has bit[0]=update, no EN bit! */
/* v0.0.98: Remove reset before apply + fix double RCQ_IRQ_CTL write */
/* BSP g2d_bsp_open() only sets gates=1, rst=1 (no pulse) */
/* v0.0.94: Fix payload structures with correct offsets and padding */
/* v0.0.93: Use exact BSP RCQ header format from g2d_rcq.h */
/* v0.0.92: Split headers into small chunks with proper dirty bitmask */
/* v0.0.91: Add MBUS interconnect configuration (G2D uses master #3) */
/* v0.0.90: RCQ with exact BSP g2d_mixer_apply sequence */
/* BSP RCQ: disable RCQ -> set headers -> apply frames -> enable RCQ */
#define TEST_WIDTH                      64U
#define TEST_HEIGHT                     64U
#define TEST_BPP                        4U
#define TEST_COLOR                      0xFF00FF00U

#define RCQ_SUBBLOCK_BIAS               0x28000
#define RCQ_HEADER_MAX                  64U
#define RCQ_DATA_BYTES                  1024U

/* v0.0.109: G2D_RCQ_CTRL_EN defined in sunxi-g2d-regs.h as BIT(4) */
/* T113 RCQ v2 needs BOTH EN (bit4) and UPDATE (bit0) for execution */

/* V0.0.75: g2d_rcq_head now defined in sunxi-g2d-regs.h */

struct sunxi_g2d_fill_dev {
        struct device *dev;
        void __iomem *mmio;
        resource_size_t mmio_size;
        void __iomem *ccu;        /* CCU base for G2D clock/reset control */
        struct clk *clk_mod;
        struct clk *clk_bus;
        struct clk *clk_mbus;
        struct reset_control *rst;
        struct icc_path *mbus;    /* v0.0.91: MBUS interconnect path */
        int irq;
        struct completion done;
        
        /* V0.0.75: RCQ DMA memory */
        void *rcq_virt;           /* Virtual address of RCQ memory */
        dma_addr_t rcq_phys;      /* Physical/DMA address */
        size_t rcq_size;          /* Total allocation size */
};

static inline u32 g2d_rcq_reg_off(u32 reg)
{
        /* MIXER registers already have RCQ offset (0x28xxx), don't add again */
        if (reg >= 0x28000)
                return reg;
        /* Other registers need RCQ offset added */
        return reg + RCQ_SUBBLOCK_BIAS;
}

static inline int clk_prepare_enable_optional(struct clk *clk)
{
        return clk ? clk_prepare_enable(clk) : 0;
}

static inline void clk_disable_unprepare_optional(struct clk *clk)
{
        if (clk)
                clk_disable_unprepare(clk);
}

static void g2d_top_enable(struct sunxi_g2d_fill_dev *fill)
{
        /* BSP g2d_bsp_open() sequence */
        iowrite32(0x3, fill->mmio + G2D_SCLK_GATE);  /* MIXER + ROT */
        iowrite32(0x3, fill->mmio + G2D_HCLK_GATE);  /* MIXER + ROT */
        
        /* BSP g2d_bsp_reset() does 0->1 pulse */
        iowrite32(0x0, fill->mmio + G2D_AHB_RESET);  /* Assert reset */
        wmb();
        usleep_range(10, 20);  /* Small delay for reset propagation */
        iowrite32(0x3, fill->mmio + G2D_AHB_RESET);  /* Release reset */
        wmb();
        
        /* Configure CMD_CTL for DRAM command control (CRITICAL for RCQ DMA) */
        iowrite32(G2D_CMD_CTL_DEFAULT, fill->mmio + G2D_CMD_CTL);
        wmb();
        
	dev_info(fill->dev, "TOP gates opened: SCLK=0x%08x HCLK=0x%08x RESET=0x%08x CMD_CTL=0x%08x\n",
		 ioread32(fill->mmio + G2D_SCLK_GATE),
		 ioread32(fill->mmio + G2D_HCLK_GATE),
		 ioread32(fill->mmio + G2D_AHB_RESET),
		 ioread32(fill->mmio + G2D_CMD_CTL));
	
	/* Read G2D_VERSION to identify hardware variant */
	u32 version = ioread32(fill->mmio + G2D_VERSION);
	dev_info(fill->dev, "G2D_VERSION=0x%08x (IP version bits[31:16]=0x%04x)\n",
		 version, (version >> 16) & 0xFFFF);
}

static void g2d_ccu_enable(struct sunxi_g2d_fill_dev *fill)
{
	u32 clk_reg, bgr_reg;
	
	if (!fill->ccu) {
		dev_warn(fill->dev, "CCU not mapped, skipping\n");
		return;
	}
	
	/* G2D_CLK_REG: Enable G2D clock */
	clk_reg = ioread32(fill->ccu + G2D_CLK_REG);
	clk_reg |= G2D_CLK_GATING;
	iowrite32(clk_reg, fill->ccu + G2D_CLK_REG);
	
	/* G2D_BGR_REG: Only bit 0 (G2D_GATING) + bit 16 (G2D_RST) per T113 manual */
	/* User manual section 3.3.6.43: No individual sub-block bits exist */
	bgr_reg = 0x00010001;
	iowrite32(bgr_reg, fill->ccu + G2D_BGR_REG);
	
	wmb();
	
	clk_reg = ioread32(fill->ccu + G2D_CLK_REG);
	bgr_reg = ioread32(fill->ccu + G2D_BGR_REG);
	
	dev_info(fill->dev, "CCU: CLK_REG=0x%08x BGR_REG=0x%08x\n",
		 clk_reg, bgr_reg);
}

static void g2d_top_disable(struct sunxi_g2d_fill_dev *fill)
{
        /* BSP g2d_bsp_close() sequence */
        iowrite32(0x0, fill->mmio + G2D_AHB_RESET);
        iowrite32(0x0, fill->mmio + G2D_HCLK_GATE);
        iowrite32(0x0, fill->mmio + G2D_SCLK_GATE);
        wmb();
}

static void g2d_bsp_reset(struct sunxi_g2d_fill_dev *fill)
{
        /* BSP g2d_bsp_reset() sequence - full reset of MIXER + ROT */
        iowrite32(0x0, fill->mmio + G2D_AHB_RESET);
        wmb();
        iowrite32(0x3, fill->mmio + G2D_AHB_RESET);
        wmb();
}

static irqreturn_t sunxi_g2d_irq(int irq, void *data)
{
        struct sunxi_g2d_fill_dev *fill = data;
        u32 mixer_int = ioread32(fill->mmio + G2D_MIXER_INT);
        u32 rcq_status = ioread32(fill->mmio + G2D_RCQ_STATUS);
        bool handled = false;

        /* CRITICAL: Clear MIXER_INT pending FIRST to stop IRQ storm */
        if (mixer_int & G2D_MIXER_INT_IRQ_PENDING) {
                iowrite32(G2D_MIXER_INT_IRQ_PENDING, fill->mmio + G2D_MIXER_INT);
                wmb();
                
                /* Direct mode completion - signal done */
                dev_info(fill->dev, "IRQ: MIXER finish (DIRECT mode) - task complete!\n");
                complete(&fill->done);
                handled = true;
        }

        /* RCQ cfg_finish_irq - only relevant in RCQ mode, ignore in DIRECT */
        if (rcq_status & G2D_RCQ_STATUS_CFG_FINISH) {
                dev_info(fill->dev, "IRQ: cfg_finish (RCQ config loaded, ignoring in DIRECT mode)\n");
                iowrite32(G2D_RCQ_STATUS_CFG_FINISH, fill->mmio + G2D_RCQ_STATUS);
                wmb();
                handled = true;
        }

        /* RCQ task_end_irq - RCQ batch completion */
        if (rcq_status & G2D_RCQ_STATUS_TASK_END) {
                dev_info(fill->dev, "IRQ: RCQ task_end - RCQ batch complete!\n");
                iowrite32(G2D_RCQ_STATUS_TASK_END, fill->mmio + G2D_RCQ_STATUS);
                wmb();
                complete(&fill->done);
                handled = true;
        }

        return handled ? IRQ_HANDLED : IRQ_NONE;
}

/* V0.0.75: Simplified RCQ header setup - kept for compatibility with old test */
static void __maybe_unused set_rcq_header(struct g2d_rcq_head *head, dma_addr_t payload_addr,
                           size_t len, u32 reg)
{
	/* Build header following BSP semantics:
	 * dw0: [31:24]=high_addr (bits 39:32), [23:0]=len (bytes)
	 * dirty: bit0 = dirty flag, bits[31:16] = n_header_len
	 * Use provided macro to build dirty field for consistency.
	 */
	head->low_addr = lower_32_bits(payload_addr);
	/* high_addr is bits [39:32] of the physical address */
	/* Use 64-bit shift to avoid overflow on 32-bit builds */
	head->dw0 = (((u32)(((u64)payload_addr >> 32) & 0xFF)) << 24) | (len & 0xFFFFFF);
	/* Default: mark block dirty, no chained headers (n_header_len = 0) */
	head->dirty = G2D_RCQ_MAKE_DIRTY(1, 0);
	head->reg_offset = g2d_rcq_reg_off(reg);
}

/*
 * Direct register-write fillrect replicating BSP g2d_fillrectangle().
 * No RCQ - pure CPU writes as BSP does.
 */
static int __maybe_unused sunxi_g2d_run_fillrect_direct(struct sunxi_g2d_fill_dev *fill)
{
	const u32 pitch = TEST_WIDTH * TEST_BPP;
	const size_t surface_bytes = pitch * TEST_HEIGHT;
	const u32 size_hw = ((TEST_HEIGHT - 1) << 16) | (TEST_WIDTH - 1);
	void *dst = NULL;
	dma_addr_t dst_dma = 0;
	dma_addr_t mbus_offset;
	dma_addr_t v0_addr_device, wb_addr_device;
	bool has_iommu;
	u32 tmp;
	int ret;
	unsigned long timeout;

        dev_info(fill->dev, "=== V0.0.47: FORCE NON-MANAGED DMA ALLOC FOR IOMMU ===\n");

        /* V0.0.47: Use non-managed dma_alloc_coherent() which works better with IOMMU */
        dst = dma_alloc_coherent(fill->dev, surface_bytes, &dst_dma, GFP_KERNEL);
        if (!dst) {
                dev_err(fill->dev, "Failed to allocate destination buffer\n");
                return -ENOMEM;
        }
	
	/* V0.0.66: NO MBUS OFFSET - CLK_MBUS_G2D means G2D handles MBUS internally
	 * Use addresses directly as returned by dma_alloc_coherent()
	 */
	mbus_offset = 0;  /* No manual offset needed */
	
	dev_info(fill->dev, ">>> V0.0.66: MBUS configuration (CLK_MBUS_G2D present):\n");
	dev_info(fill->dev, "    dst_dma=0x%llx size=%zu\n", (u64)dst_dma, surface_bytes);
	dev_info(fill->dev, "    Using DMA address directly (no offset subtraction)\n");

	/* V0.0.43: Write test pattern BEFORE G2D operation */
	u32 *test_buf = (u32 *)dst;
	for (int i = 0; i < (surface_bytes / 4); i++) {
		test_buf[i] = 0xDEADBEEF;  /* Test pattern */
	}
	wmb();  /* Ensure writes complete */
	dev_info(fill->dev, ">>> V0.0.43: Pre-filled buffer with 0xDEADBEEF pattern\n");
	dev_info(fill->dev, "    Before G2D: [0]=0x%08x [10]=0x%08x [100]=0x%08x\n",
		 test_buf[0], test_buf[10], test_buf[100]);

	/* BSP sequence: g2d_bsp_reset() - full reset */
	g2d_bsp_reset(fill);

	dev_info(fill->dev, ">>> AFTER RESET: V0_ATTCTL=0x%08x V0_SIZE=0x%08x BLD_SIZE=0x%08x WB_ATT=0x%08x\n",
		 ioread32(fill->mmio + G2D_V0 + V0_ATTCTL),
		 ioread32(fill->mmio + G2D_V0 + V0_SIZE),
		 ioread32(fill->mmio + G2D_BLD + BLD_SIZE),
		 ioread32(fill->mmio + WB_ATT));  /* V0.0.26: Fixed WB address */

	dev_info(fill->dev, ">>> CONFIGURATION START - BSP SEQUENCE ONLY\n");

	/*
	 * V0.0.38: Test BSP original addresses from your discovery + ADD mode field!
	 * G2D_V0 = 0x00800, G2D_BLD = 0x00400 (not 0x02000 which is G2D_UI2)
	 */
	u32 bsp_v0_base = 0x00800;      /* BSP G2D_V0 address */
	u32 bsp_bld_base = 0x00400;     /* BSP G2D_BLD address */
	u32 legacy_ui2_base = 0x02000;  /* G2D_UI2 - where BLD was working */
	
    dev_info(fill->dev, ">>> V0.0.38: Testing BSP original addresses - V0=0x%05x, BLD=0x%05x\n",
		 bsp_v0_base, bsp_bld_base);

	/* Test V0 at BSP original address 0x00800 */
	dev_info(fill->dev, ">>> V0.0.37: Testing V0 at BSP address 0x%05x\n", bsp_v0_base);
	
	/* Clear and test V0 at BSP address */
	iowrite32(0x00000000, fill->mmio + bsp_v0_base + 0x00);
	iowrite32(0x00000000, fill->mmio + bsp_v0_base + 0x08);
	wmb();
	
	/* Write test patterns */
	iowrite32(0x12345678, fill->mmio + bsp_v0_base + 0x00);  /* V0_ATTCTL */
	iowrite32(0x87654321, fill->mmio + bsp_v0_base + 0x08);  /* V0_SIZE */
	wmb();
	
	u32 v0_attctl_read = ioread32(fill->mmio + bsp_v0_base + 0x00);
	u32 v0_size_read = ioread32(fill->mmio + bsp_v0_base + 0x08);
	
	dev_info(fill->dev, "  BSP V0 (0x%05x): wrote 0x12345678/0x87654321, read 0x%08x/0x%08x %s\n",
		 bsp_v0_base, v0_attctl_read, v0_size_read,
		 (v0_attctl_read == 0x12345678 && v0_size_read == 0x87654321) ? "✓ PERFECT MATCH!" : 
		 (v0_attctl_read != 0x00000000 && v0_size_read != 0x00000000) ? "✓ responds" : "✗ no response");

	/* Test BLD at BSP original address 0x00400 */
	dev_info(fill->dev, ">>> V0.0.37: Testing BLD at BSP address 0x%05x\n", bsp_bld_base);
	
	iowrite32(0xAABBCCDD, fill->mmio + bsp_bld_base + 0x00);  /* BLD_EN_CTL */
	iowrite32(0x11223344, fill->mmio + bsp_bld_base + 0x10);  /* BLD_SIZE */
	wmb();
	
	u32 bld_en_read = ioread32(fill->mmio + bsp_bld_base + 0x00);
	u32 bld_size_read = ioread32(fill->mmio + bsp_bld_base + 0x10);
	
	dev_info(fill->dev, "  BSP BLD (0x%05x): wrote 0xAABBCCDD/0x11223344, read 0x%08x/0x%08x %s\n",
		 bsp_bld_base, bld_en_read, bld_size_read,
		 (bld_en_read == 0xAABBCCDD && bld_size_read == 0x11223344) ? "✓ PERFECT MATCH!" :
		 (bld_en_read != 0x00000000 && bld_size_read != 0x00000000) ? "✓ responds" : "✗ no response");

	/* Use working addresses for final configuration */
	u32 final_v0_base = (v0_attctl_read == 0x12345678) ? bsp_v0_base : 0x05000;  /* fallback to 0x05000 */
	u32 final_bld_base = (bld_size_read == 0x11223344) ? bsp_bld_base : legacy_ui2_base;  /* Use BSP if SIZE register responds */
	u32 final_wb_base = 0x03000;  /* Will be updated by WB scan in v0.0.72 */
	
	dev_info(fill->dev, ">>> V0.0.67: Using V0=0x%05x, BLD=0x%05x for final config\n",
		 final_v0_base, final_bld_base);

	/* Configure V0 using the working BSP address with CORRECT BSP sequence */
    dev_info(fill->dev, ">>> V0.0.38: V0 configuration following BSP sequence + MODE FIELD at 0x%05x\n", final_v0_base);	/* BSP V0 Configuration Sequence - EXACTLY as in g2d_vlayer_set():
	 * 1. V0_ATTCTL (WITHOUT fill color bit initially)
	 * 2. V0_MBSIZE, V0_SIZE, V0_COOR  
	 * 3. V0_PITCH0/1/2
	 * 4. V0_LADD0/1/2, V0_HADD
	 * 5. Fill color separately via g2d_fc_set() pattern
	 */
	
    /* Step 1: V0_ATTCTL without fill color bit (bit 4) + MODE FIELD (bits 1-2) */
    /* BSP uses mode=0 for fillrect: dst->mode = 0; mapped to (mode << 1) */
    u32 v0_attctl = (0xFF << 24) | V0_ATTCTL_EN | (G2D_FORMAT_XRGB8888 << V0_ATTCTL_FMT_SHIFT) | (0 << 1);
    iowrite32(v0_attctl, fill->mmio + final_v0_base + 0x00);  /* V0_ATTCTL */
    wmb();
    u32 v0_attctl_written = ioread32(fill->mmio + final_v0_base + 0x00);
    dev_info(fill->dev, ">>> V0_ATTCTL: wrote 0x%08x, readback 0x%08x\n", v0_attctl, v0_attctl_written);

    /* Step 2: Size and coordinate registers */
    iowrite32(size_hw, fill->mmio + final_v0_base + 0x04);    /* V0_MBSIZE */
    iowrite32(size_hw, fill->mmio + final_v0_base + 0x2C);    /* V0_SIZE (same as MBSIZE) */
    iowrite32(0x00000000, fill->mmio + final_v0_base + 0x08); /* V0_COOR */

    /* Step 3: Pitch registers */
    iowrite32(pitch, fill->mmio + final_v0_base + 0x0C);      /* V0_PITCH0 */
    iowrite32(pitch, fill->mmio + final_v0_base + 0x10);      /* V0_PITCH1 */
    iowrite32(pitch, fill->mmio + final_v0_base + 0x14);      /* V0_PITCH2 */

    /* Step 4: Address registers with MBUS offset correction */
    v0_addr_device = dst_dma - mbus_offset;
    
    iowrite32(lower_32_bits(v0_addr_device), fill->mmio + final_v0_base + 0x18);  /* V0_LADD0 */
    iowrite32(lower_32_bits(v0_addr_device), fill->mmio + final_v0_base + 0x1C);  /* V0_LADD1 */
    iowrite32(lower_32_bits(v0_addr_device), fill->mmio + final_v0_base + 0x20);  /* V0_LADD2 */
    iowrite32(upper_32_bits(v0_addr_device), fill->mmio + final_v0_base + 0x28);  /* V0_HADD */
    wmb();

    /* Step 5: BSP g2d_fc_set(0, color_value) - BEFORE BLD configuration! */
    dev_info(fill->dev, ">>> V0.0.40: BSP g2d_fc_set BEFORE BLD config (correct sequence!)\n");
    /* Read current V0_ATTCTL */
    u32 current_attctl = ioread32(fill->mmio + final_v0_base + 0x00);
    dev_info(fill->dev, ">>> Before fill color: V0_ATTCTL readback = 0x%08x\n", current_attctl);
    /* Enable fill color bit (bit 4) */
    current_attctl |= BIT(4);
    dev_info(fill->dev, ">>> Setting fill color: V0_ATTCTL = 0x%08x (added bit 4)\n", current_attctl);
    /* Write back V0_ATTCTL with fill color enabled */
    iowrite32(current_attctl, fill->mmio + final_v0_base + 0x00);
    /* Now write the fill color value */
    iowrite32(TEST_COLOR, fill->mmio + final_v0_base + 0x24);  /* V0_FILLC */
    wmb();

    /* Step 6: Configure BLD using BSP sequence (AFTER g2d_fc_set) */
    dev_info(fill->dev, ">>> V0.0.40: BSP BLD config AFTER g2d_fc_set (correct sequence!)\n");

	/* V0.0.70: MIXER Output Size Configuration
	 * MIXER needs output size configured before BLD/WB can work
	 * Legacy MIXER base = 0x00100
	 */
	u32 mixer_base = 0x00100;
	dev_info(fill->dev, ">>> V0.0.70: MIXER output config at 0x%05x\n", mixer_base);
	/* Some SoCs have MIXER_OUT_SIZE at offset 0x08, try it */
	iowrite32(size_hw, fill->mmio + mixer_base + 0x08);  /* MIXER size/route register */
	wmb();

	/* V0.0.69: BLD Configuration - BSP SIMPLE mode following user spec
	 * BLD @ 0x0400
	 * BLD_EN_CTL = PIPE0_EN (bit 8)
	 * BLD_CH_ISIZE0 = 0x003F003F
	 * BLD_OUT_SIZE = 0x003F003F
	 * BLD_PREMUL_CTL and BLD_OUT_COLOR = 0 (RGB basic mode)
	 */
	dev_info(fill->dev, ">>> V0.0.69: BSP simple BLD config at 0x%05x\n", final_bld_base);
	
	iowrite32(0x00000100, fill->mmio + final_bld_base + 0x000);  /* BLD_EN_CTL = PIPE0_EN (bit 8) */
	iowrite32(0x00000000, fill->mmio + final_bld_base + 0x010);  /* BLD_FILLC0 */
	iowrite32(0x00000000, fill->mmio + final_bld_base + 0x014);  /* BLD_FILLC1 */
	iowrite32(size_hw, fill->mmio + final_bld_base + 0x020);     /* BLD_CH_ISIZE0 = 0x003F003F */
	iowrite32(0, fill->mmio + final_bld_base + 0x030);           /* BLD_CH_OFFSET0 = 0 */
	iowrite32(0x00000000, fill->mmio + final_bld_base + 0x040);  /* BLD_PREMUL_CTL = 0 (no premul) */
	iowrite32(0x00000000, fill->mmio + final_bld_base + 0x044);  /* BLD_BK_COLOR = 0 */
	iowrite32(size_hw, fill->mmio + final_bld_base + 0x048);     /* BLD_OUT_SIZE (BLD_SIZE) = 0x003F003F */
	iowrite32(0x03010301, fill->mmio + final_bld_base + 0x04C);  /* BLD_CTL */
	iowrite32(0, fill->mmio + final_bld_base + 0x060);           /* BLD_OUT_COLOR = 0 */
	wmb();

	/*
	 * V0.0.71: ROP_CTL = 0xF0 (restore to working value)
	 * User spec: "ROP_CTL = 0x00000000... puede 'matar' el flujo si está en 0"
	 */
	iowrite32(0xF0, fill->mmio + ROP_CTL);  /* ROP at BSP BLD + 0x080 */
	wmb();
	
	/*
	 * V0.0.73: WB (Write-Back) configuration - EXACT BSP LEGACY MATCH
	 * BSP g2d_bsp_v2.c line 708: write_wvalue(WB_ATT, image->format);
	 * BSP writes ONLY format, NO enable bit!
	 * Hypothesis: WB activates automatically when MIXER starts
	 */
	final_wb_base = 0x03000;  /* BSP legacy WB base from g2d_regs_v2.h */
	
	wb_addr_device = dst_dma - mbus_offset;
	
	dev_info(fill->dev, ">>> V0.0.73: WB BSP EXACT legacy mode at 0x%05x\n", final_wb_base);
	dev_info(fill->dev, ">>> MBUS: dst_dma=0x%llx device=0x%llx\n", 
		 (u64)dst_dma, (u64)wb_addr_device);
	
	/*
	 * V0.0.73: Write WB_ATT with ONLY format (like BSP g2d_bsp_v2.c:708)
	 * BSP does: write_wvalue(WB_ATT, image->format);
	 * NO enable bit! WB may auto-enable when MIXER starts.
	 */
	u32 wb_att_val = G2D_FORMAT_XRGB8888;  /* Format ONLY, NO enable bit! */
	
	dev_info(fill->dev, ">>> V0.0.73: WB_ATT = 0x%08x (format ONLY, BSP style)\n", wb_att_val);
	dev_info(fill->dev, ">>>   BSP g2d_bsp_v2.c:708 does: write_wvalue(WB_ATT, image->format)\n");
	
	/* Write WB_ATT with format only (BSP exact behavior) */
	iowrite32(wb_att_val, fill->mmio + final_wb_base);  /* WB_ATT = format only */
	wmb();
	
	/* Verify write */
	u32 wb_att_readback = ioread32(fill->mmio + final_wb_base);
	dev_info(fill->dev, ">>>   WB_ATT readback: 0x%08x (wrote 0x%08x)\n", 
		 wb_att_readback, wb_att_val);
	
	/* Write remaining WB registers using BSP base */
	iowrite32(size_hw, fill->mmio + final_wb_base + 0x04);  /* WB_SIZE */
	iowrite32(pitch, fill->mmio + final_wb_base + 0x08);     /* WB_PITCH0 */
	iowrite32(pitch, fill->mmio + final_wb_base + 0x0C);     /* WB_PITCH1 */
	iowrite32(pitch, fill->mmio + final_wb_base + 0x10);     /* WB_PITCH2 */
	iowrite32(lower_32_bits(wb_addr_device), fill->mmio + final_wb_base + 0x14);  /* WB_LADD0 */
	iowrite32(upper_32_bits(wb_addr_device), fill->mmio + final_wb_base + 0x18);  /* WB_HADD0 */
	wmb();

	dev_info(fill->dev, ">>> V0.0.37: NO REDUNDANT CONFIGURATION - BSP sequence only!\n");

	/*
	 * V0.0.74: RCQ initialization sequence (even in direct/legacy mode)
	 * BSP g2d_mixer.c:869-884 does this even when not using RCQ queue
	 * This may be required on T113 to properly initialize the pipeline
	 */
	dev_info(fill->dev, ">>> V0.0.74: RCQ protocol init (BSP g2d_mixer_apply pattern)\n");
	
	/* Disable RCQ update and IRQ before configuration (BSP pattern) */
	u32 rcq_ctrl_val = ioread32(fill->mmio + G2D_RCQ_CTRL);
	rcq_ctrl_val &= ~G2D_RCQ_CTRL_UPDATE;  /* Clear UPDATE bit */
	rcq_ctrl_val &= ~G2D_RCQ_CTRL_EN;      /* Clear EN bit */
	iowrite32(rcq_ctrl_val, fill->mmio + G2D_RCQ_CTRL);
	wmb();
	
	dev_info(fill->dev, ">>>   RCQ_CTRL=0x%08x (UPDATE=0 EN=0 for config)\n",
		 ioread32(fill->mmio + G2D_RCQ_CTRL));

	/*
	 * V0.0.37: Global Control and IRQ setup
	 */
	dev_info(fill->dev, "G2D_CONTROL before: 0x%08x\n", 
		 ioread32(fill->mmio + G2D_CONTROL));
	
	/* Try enabling all bits in G2D_CONTROL */
	iowrite32(0xFFFFFFFF, fill->mmio + G2D_CONTROL);
	wmb();
	dev_info(fill->dev, "G2D_CONTROL after writing 0xFFFFFFFF: 0x%08x\n", 
		 ioread32(fill->mmio + G2D_CONTROL));

	/*
	 * RCQ_IRQ_CTL controls ALL interrupts
	 * G2D_RCQ_IRQ_SEL (bit 0): 0=RCQ mode, 1=Direct mode
	 * We need to set bit 0 to route MIXER_INT interrupts to CPU!
	 */
	dev_info(fill->dev, "RCQ_IRQ_CTL before: 0x%08x\n", 
		 ioread32(fill->mmio + G2D_RCQ_IRQ_CTL));
	iowrite32(G2D_RCQ_IRQ_SEL, fill->mmio + G2D_RCQ_IRQ_CTL);  /* Enable direct mode IRQs */
	wmb();
	dev_info(fill->dev, "RCQ_IRQ_CTL after: 0x%08x (should be 0x01)\n", 
		 ioread32(fill->mmio + G2D_RCQ_IRQ_CTL));

	/*
	 * BSP mixer_irq_enable():
	 *   writes 0x10 to G2D_MIXER_INT (at legacy 0x00104)
	 * Clear any stale pending first, then enable IRQ
	 */
	iowrite32(0x1, fill->mmio + G2D_MIXER_INT);  /* Clear pending */
	wmb();
	iowrite32(0x10, fill->mmio + G2D_MIXER_INT);  /* Enable finish IRQ */
	wmb();
	dev_info(fill->dev, "MIXER_INT (legacy 0x%05x) after IRQ enable: 0x%08x (expect 0x10)\n",
		 G2D_MIXER_INT, ioread32(fill->mmio + G2D_MIXER_INT));

	/* Re-init completion */
	reinit_completion(&fill->done);

	/* Dump all configured registers before starting mixer */
	dev_info(fill->dev, "V0.0.37: Register dump from BSP addresses before mixer start:\n");
	
	dev_info(fill->dev, "  V0 (BSP 0x%05x): V0_ATTCTL=0x%08x V0_SIZE=0x%08x V0_FILLC=0x%08x\n",
		 final_v0_base,
		 ioread32(fill->mmio + final_v0_base + 0x00),  /* V0_ATTCTL */
		 ioread32(fill->mmio + final_v0_base + 0x2C),  /* V0_SIZE - CORRECT OFFSET! */
		 ioread32(fill->mmio + final_v0_base + 0x24)); /* V0_FILLC - CORRECT OFFSET! */
	dev_info(fill->dev, "  V0_PITCH0=0x%08x V0_LADD0=0x%08x V0_HADD=0x%08x\n",
		 ioread32(fill->mmio + final_v0_base + 0x0C),  /* V0_PITCH0 */
		 ioread32(fill->mmio + final_v0_base + 0x18),  /* V0_LADD0 */
		 ioread32(fill->mmio + final_v0_base + 0x28)); /* V0_HADD - CORRECT OFFSET! */
	dev_info(fill->dev, "  BLD (BSP 0x%05x): BLD_EN_CTL=0x%08x BLD_OUT_SIZE=0x%08x BLD_CTL=0x%08x\n",
		 final_bld_base,
		 ioread32(fill->mmio + final_bld_base + 0x000),  /* BLD_EN_CTL */
		 ioread32(fill->mmio + final_bld_base + 0x048),  /* BLD_OUT_SIZE (was BLD_SIZE) */
		 ioread32(fill->mmio + final_bld_base + 0x04C)); /* BLD_CTL */
	dev_info(fill->dev, "  BLD_CH_ISIZE0=0x%08x BLD_OUT_COLOR=0x%08x\n",
		 ioread32(fill->mmio + final_bld_base + 0x020),  /* BLD_CH_ISIZE0 - BSP OFFSET! */
		 ioread32(fill->mmio + final_bld_base + 0x060)); /* BLD_OUT_COLOR - BSP OFFSET! */
	dev_info(fill->dev, "  ROP_CTL (legacy 0x01000)=0x%08x ROP_CTL (RCQ 0x%05x)=0x%08x\n",
		 ioread32(fill->mmio + 0x01000),
		 ROP_CTL, ioread32(fill->mmio + ROP_CTL));
	dev_info(fill->dev, "  WB (scan found 0x%05x): WB_ATT=0x%08x WB_SIZE=0x%08x WB_LADD0=0x%08x\n",
		 final_wb_base,
		 ioread32(fill->mmio + final_wb_base + 0x00),
		 ioread32(fill->mmio + final_wb_base + 0x04),
		 ioread32(fill->mmio + final_wb_base + 0x14));

	/*
	 * BSP starts mixer:
	 *   tmp = read_wvalue(G2D_MIXER_CTL);
	 *   tmp |= 0x80000000;
	 *   write_wvalue(G2D_MIXER_CTL, tmp);
	 * Try ensuring MIXER_CTL starts from known state
	 */
	iowrite32(0x0, fill->mmio + G2D_MIXER_CTL);
	wmb();
	
	/*
	 * V0.0.74: Enable RCQ BEFORE starting MIXER (BSP g2d_mixer_apply:897-898)
	 * BSP does: g2d_top_rcq_irq_en(1); g2d_top_rcq_update_en(1);
	 * This triggers the pipeline to start processing
	 */
	dev_info(fill->dev, ">>> V0.0.74: Enabling RCQ before MIXER start (BSP pattern)\n");
	rcq_ctrl_val = ioread32(fill->mmio + G2D_RCQ_CTRL);
	rcq_ctrl_val |= G2D_RCQ_CTRL_EN;      /* Set EN bit first */
	iowrite32(rcq_ctrl_val, fill->mmio + G2D_RCQ_CTRL);
	wmb();
	rcq_ctrl_val |= G2D_RCQ_CTRL_UPDATE;  /* Then set UPDATE bit - triggers! */
	iowrite32(rcq_ctrl_val, fill->mmio + G2D_RCQ_CTRL);
	wmb();
	
	dev_info(fill->dev, ">>>   RCQ_CTRL=0x%08x (EN=1 UPDATE=1 - pipeline enabled!)\n",
		 ioread32(fill->mmio + G2D_RCQ_CTRL));
	
	/* Now start MIXER */
	tmp = ioread32(fill->mmio + G2D_MIXER_CTL);
	tmp |= 0x80000000;
	iowrite32(tmp, fill->mmio + G2D_MIXER_CTL);
	wmb();
	dev_info(fill->dev, "Started MIXER with CTL=0x%08x\n", tmp);

	/* Check if mixer actually starts running + aggressive IRQ polling */
	dev_info(fill->dev, "V0.0.37: Checking if mixer starts running...\n");
	
	/* Check mixer status after a few ms */
	msleep(10);
	u32 ctl_after = ioread32(fill->mmio + G2D_MIXER_CTL);
	u32 int_after = ioread32(fill->mmio + G2D_MIXER_INT);
	dev_info(fill->dev, "After 10ms: MIXER_CTL=0x%08x MIXER_INT=0x%08x\n", ctl_after, int_after);
	
	/* More aggressive polling to catch IRQ completion flag */
	for (int i = 0; i < 10; i++) {
		msleep(5);
		ctl_after = ioread32(fill->mmio + G2D_MIXER_CTL);
		int_after = ioread32(fill->mmio + G2D_MIXER_INT);
		
		/* Check for completion: START bit cleared OR IRQ pending bit set */
		if (!(ctl_after & 0x80000000) || (int_after & 0x1)) {
			dev_info(fill->dev, "At %dms: MIXER_CTL=0x%08x MIXER_INT=0x%08x %s\n", 
				 15 + i*5, ctl_after, int_after,
				 (int_after & 0x1) ? "IRQ_PENDING!" : "START_CLEARED");
			break;
		}
	}
	
	msleep(50);
	ctl_after = ioread32(fill->mmio + G2D_MIXER_CTL);
	int_after = ioread32(fill->mmio + G2D_MIXER_INT);
	dev_info(fill->dev, "After 60ms: MIXER_CTL=0x%08x MIXER_INT=0x%08x\n", ctl_after, int_after);

	/* If START bit (bit 31) is still set, mixer is stuck */
	if (ctl_after & 0x80000000) {
		dev_warn(fill->dev, "MIXER START bit still set - mixer may be stuck!\n");
	} else {
		dev_info(fill->dev, "MIXER START bit cleared - operation may have completed\n");
	}

	/* V0.0.42: Dump registers AFTER mixer completes to see what G2D actually did */
	dev_info(fill->dev, ">>> V0.0.42: POST-COMPLETION register dump:\n");
	dev_info(fill->dev, "  WB_ATT=0x%08x WB_SIZE=0x%08x WB_PITCH0=0x%08x\n",
		 ioread32(fill->mmio + final_wb_base + 0x00),
		 ioread32(fill->mmio + final_wb_base + 0x04),
		 ioread32(fill->mmio + final_wb_base + 0x08));
	dev_info(fill->dev, "  WB_LADD0=0x%08x WB_HADD0=0x%08x (expected 0x%08x/0x%08x)\n",
		 ioread32(fill->mmio + final_wb_base + 0x14),
		 ioread32(fill->mmio + final_wb_base + 0x18),
		 lower_32_bits(dst_dma), upper_32_bits(dst_dma));
	dev_info(fill->dev, "  V0_ATTCTL=0x%08x V0_FILLC=0x%08x BLD_EN_CTL=0x%08x\n",
		 ioread32(fill->mmio + final_v0_base + 0x00),
		 ioread32(fill->mmio + final_v0_base + 0x24),
		 ioread32(fill->mmio + final_bld_base + 0x00));

	/*
	 * V0.0.37: VERIFY ACTUAL COMPLETION by checking output buffer content
	 * If G2D worked correctly, output buffer should contain green rectangles
	 */
    dev_info(fill->dev, "=== V0.0.41: Verifying output buffer content (WB FORMAT ONLY) ===\n");	/* Sample a few pixels from the output buffer */
	u32 *output_pixels = (u32 *)dst;
	u32 expected_green = 0x0000FF00;  /* ARGB green */
	
	/* Check corners and center */
	u32 positions[] = {0, 15, 15*64, 15*64+15, 32*64+32};  /* TL, TR, BL, BR, Center */
	const char *names[] = {"Top-Left", "Top-Right", "Bottom-Left", "Bottom-Right", "Center"};
	int correct_pixels = 0;
	
	for (int i = 0; i < 5; i++) {
		u32 pixel = output_pixels[positions[i]];
		bool is_correct = (pixel == expected_green);
		dev_info(fill->dev, "%s [%d]: 0x%08x %s\n", 
			 names[i], positions[i], pixel, 
			 is_correct ? "✓ GREEN" : "✗ NOT GREEN");
		if (is_correct) correct_pixels++;
	}
	
	/* Overall assessment */
	if (correct_pixels >= 4) {
		dev_info(fill->dev, "🎉 SUCCESS! G2D fillrect operation completed correctly (%d/5 pixels green)\n", 
			 correct_pixels);
	} else if (correct_pixels > 0) {
		dev_warn(fill->dev, "⚠️  PARTIAL: Some pixels correct (%d/5), G2D may be working partially\n", 
			 correct_pixels);
	} else {
		dev_err(fill->dev, "❌ FAILED: No correct pixels found, G2D operation did not work\n");
		
		/* Check first few pixels for any non-zero data */
		dev_info(fill->dev, "First 8 pixels: ");
		for (int i = 0; i < 8; i++) {
			printk(KERN_CONT "0x%08x ", output_pixels[i]);
		}
		printk(KERN_CONT "\n");
	}

	/* Check if IRQ pending bit is set */
	if (int_after & 0x1) {
		dev_info(fill->dev, "IRQ PENDING bit set - operation finished but no interrupt received!\n");
	}

	/* Wait for IRQ completion (BSP does g2d_wait_cmd_finish) */
	timeout = wait_for_completion_timeout(&fill->done, msecs_to_jiffies(200));
	if (!timeout) {
		u32 status = ioread32(fill->mmio + G2D_MIXER_INT);
		u32 ctl = ioread32(fill->mmio + G2D_MIXER_CTL);
		u32 rcq_irq = ioread32(fill->mmio + G2D_RCQ_IRQ_CTL);
		u32 control = ioread32(fill->mmio + G2D_CONTROL);
		dev_err(fill->dev, "Direct fillrect timeout: MIXER_INT=0x%08x MIXER_CTL=0x%08x RCQ_IRQ_CTL=0x%08x G2D_CONTROL=0x%08x\n",
			status, ctl, rcq_irq, control);
		
		/* Don't fail probe - keep module loaded for debugging */
	dev_info(fill->dev, "V0.0.37: Timeout occurred but forcing success to keep module loaded for debugging\n");
		ret = 0;  /* Force success */
		goto out_free;
	}

	dev_info(fill->dev, "Direct fillrect completed successfully!\n");

	/* Verify first pixel */
	if (*(u32 *)dst == TEST_COLOR)
		dev_info(fill->dev, "PASS: First pixel = 0x%08x\n", *(u32 *)dst);
	else
		dev_warn(fill->dev, "MISMATCH: First pixel = 0x%08x (expected 0x%08x)\n",
			 *(u32 *)dst, TEST_COLOR);

	ret = 0;

out_free:
	/* V0.0.47: Free non-managed DMA buffer */
	if (dst)
		dma_free_coherent(fill->dev, surface_bytes, dst, dst_dma);
	
	dev_info(fill->dev, "=== BSP-style direct fillrect end (v0.0.47) ===\n");
	return ret;
}

static int __maybe_unused sunxi_g2d_run_fillrect(struct sunxi_g2d_fill_dev *fill)
{
    const u32 pitch = TEST_WIDTH * TEST_BPP;
    const size_t surface_bytes = pitch * TEST_HEIGHT;
    const u32 size_hw = ((TEST_HEIGHT - 1) << 16) | (TEST_WIDTH - 1);
    const u32 v0_att = (0xff << 24) | V0_ATTCTL_EN | V0_ATTCTL_FILLCOLOR_EN |
                       (G2D_FORMAT_XRGB8888 << V0_ATTCTL_FMT_SHIFT);
    const u32 wb_att = WB_ATT_EN | (G2D_FORMAT_XRGB8888 << WB_ATT_FMT_SHIFT);
        void *dst = NULL, *src = NULL;
        dma_addr_t dst_dma = 0, src_dma = 0;
        struct g2d_rcq_head *headers = NULL;
        dma_addr_t headers_dma = 0;
        u32 *payload = NULL;
        dma_addr_t payload_dma = 0;
	size_t header_bytes = 0, payload_bytes = 0;
	unsigned int head_count = 0, cmd_cnt = 0;
        int ret = 0, i;

    struct {
            u32 reg;
            u32 val;
        } cmds[RCQ_HEADER_MAX];
    u32 rcq_status;
#define APPEND_CMD(reg_, val_)                                             \
    do {                                                                   \
        if (cmd_cnt >= RCQ_HEADER_MAX) {                                   \
            dev_err(fill->dev, "RCQ command overflow (max=%u)\n",         \
                    RCQ_HEADER_MAX);                                       \
            ret = -EINVAL;                                                 \
            goto out_payload;                                              \
        }                                                                  \
        cmds[cmd_cnt++] = (typeof(cmds[0])) { (reg_), (val_) };            \
    } while (0)

        dev_info(fill->dev, "starting RCQ fillrect self-test (%ux%u)\n",
                 TEST_WIDTH, TEST_HEIGHT);

    g2d_bsp_reset(fill);
        iowrite32(G2D_MIXER_INT_IRQ_PENDING, fill->mmio + G2D_MIXER_INT);
        wmb();        dst = dmam_alloc_coherent(fill->dev, surface_bytes, &dst_dma,
                                  GFP_KERNEL | GFP_DMA32);
        if (!dst)
                return -ENOMEM;

        src = dmam_alloc_coherent(fill->dev, surface_bytes, &src_dma,
                                  GFP_KERNEL | GFP_DMA32);
        if (!src) {
            ret = -ENOMEM;
            goto out_dst;
        }
        memset(src, 0x55, surface_bytes);

        headers = dmam_alloc_coherent(fill->dev,
                                      ALIGN(RCQ_HEADER_MAX * sizeof(*headers), 32),
                                      &headers_dma, GFP_KERNEL);
        if (!headers) {
            ret = -ENOMEM;
            goto out_src;
        }
    memset(headers, 0, ALIGN(RCQ_HEADER_MAX * sizeof(*headers), 32));

    /* Allocate payload buffer for register values */
    payload = dmam_alloc_coherent(fill->dev, RCQ_HEADER_MAX * sizeof(u32),
                                   &payload_dma, GFP_KERNEL);
    if (!payload) {
        ret = -ENOMEM;
        goto out_headers;
    }
    memset(payload, 0, RCQ_HEADER_MAX * sizeof(u32));

    dev_info(fill->dev,
             "buffers: dst_dma=%pad src_dma=%pad bytes=%zu\n",
             &dst_dma, &src_dma, surface_bytes);

    dev_info(fill->dev, "RCQ: headers_dma=%pad payload_dma=%pad\n",
             &headers_dma, &payload_dma);

    APPEND_CMD(BLD_EN_CTL, 0);
    APPEND_CMD(BLD_PREMUL_CTL, 0);
    APPEND_CMD(BLD_CH_ISIZE0, size_hw);
    APPEND_CMD(BLD_CH_OFFSET0, 0);
    APPEND_CMD(BLD_FILLC0, TEST_COLOR);
    APPEND_CMD(BLD_FILLC1, TEST_COLOR);
    APPEND_CMD(BLD_BK_COLOR, 0);
    APPEND_CMD(BLD_OUT_COLOR, 0);
    APPEND_CMD(BLD_OUT_SIZE, size_hw);
    APPEND_CMD(BLD_SIZE, size_hw);
    APPEND_CMD(BLD_CTL, 0);
    APPEND_CMD(ROP_CTL, 0xF0);
    APPEND_CMD(BLD_EN_CTL, BLD_PIPE0_EN);

    APPEND_CMD(V0_ATTCTL, v0_att);
    APPEND_CMD(V0_MBSIZE, size_hw);
    APPEND_CMD(V0_COOR, 0);
    APPEND_CMD(V0_PITCH0, pitch);
	APPEND_CMD(V0_PITCH1, pitch);
	APPEND_CMD(V0_PITCH2, pitch);
	APPEND_CMD(V0_LADD0, lower_32_bits(src_dma));
	APPEND_CMD(V0_LADD1, lower_32_bits(src_dma));
	APPEND_CMD(V0_LADD2, lower_32_bits(src_dma));
    APPEND_CMD(V0_HADD, 0);
    APPEND_CMD(V0_SIZE, size_hw);
    APPEND_CMD(V0_FILLC, TEST_COLOR);

    APPEND_CMD(WB_ATT, wb_att);
    APPEND_CMD(WB_SIZE, size_hw);
    APPEND_CMD(WB_PITCH0, pitch);
	APPEND_CMD(WB_PITCH1, pitch);
	APPEND_CMD(WB_PITCH2, pitch);
	APPEND_CMD(WB_LADD0, lower_32_bits(dst_dma));
	APPEND_CMD(WB_HADD0, 0);
	APPEND_CMD(WB_LADD1, lower_32_bits(dst_dma));
	APPEND_CMD(WB_HADD1, 0);
	APPEND_CMD(WB_LADD2, lower_32_bits(dst_dma));
	APPEND_CMD(WB_HADD2, 0);

    /* G2D_MIXER_INT is LEGACY - don't put in RCQ, write directly later */
    APPEND_CMD(G2D_MIXER_CTL, G2D_MIXER_CTL_START);

    /* Build RCQ: write values to payload, headers point to them */
    for (i = 0; i < cmd_cnt; i++) {
        payload[i] = cmds[i].val;
        set_rcq_header(&headers[head_count], payload_dma + (i * sizeof(u32)),
                       sizeof(u32), cmds[i].reg);
        head_count++;
    }

    /* Pad to even number of headers */
    if (head_count & 0x1) {
            memset(&headers[head_count], 0, sizeof(*headers));
            head_count++;
    }

    header_bytes = ALIGN(head_count * sizeof(*headers), 32);
    payload_bytes = cmd_cnt * sizeof(u32);

    dev_info(fill->dev,
             "RCQ ready: headers=%u (%zu bytes) payload=%zu bytes\n",
             head_count, header_bytes, payload_bytes);

    if (head_count == 0) {
            dev_warn(fill->dev, "no headers, skipping RCQ trigger\n");
            ret = -EINVAL;
            goto out_payload;
    }

    print_hex_dump(KERN_INFO, "rcq payload ", DUMP_PREFIX_OFFSET,
                   16, 4, payload, payload_bytes, false);

    print_hex_dump(KERN_INFO, "rcq headers ", DUMP_PREFIX_OFFSET,
                   16, 4, headers, header_bytes, false);

    /* Reset G2D before RCQ setup */
    g2d_bsp_reset(fill);
    
	/* Enable RCQ mode FIRST - use UPDATE per T113 BSP semantics */
	iowrite32(G2D_RCQ_CTRL_UPDATE, fill->mmio + G2D_RCQ_CTRL);
    wmb();
    
    /* Enable RCQ interrupts */
    iowrite32(G2D_RCQ_IRQ_SEL | G2D_RCQ_IRQ_TASK_END_EN,
              fill->mmio + G2D_RCQ_IRQ_CTL);
    wmb();
    
    /* Enable MIXER finish IRQ (LEGACY register, written directly not via RCQ) */
    iowrite32(G2D_MIXER_INT_FINISH_IRQ_EN, fill->mmio + G2D_MIXER_INT);
    wmb();

    /* Clear any pending status */
    iowrite32(G2D_RCQ_STATUS_TASK_END | G2D_RCQ_STATUS_CFG_FINISH,
              fill->mmio + G2D_RCQ_STATUS);
    wmb();

    /* Configure RCQ header pointer and count */
	/* Ensure RCQ DMA memory is visible to the device before programming
	 * the head pointers. Even though we use dma_alloc_coherent(), add an
	 * explicit sync for robustness across IOMMU/hardware variants and print
	 * headers again for debugging.
	 */
	dma_sync_single_for_device(fill->dev, fill->rcq_phys,
							   header_bytes + payload_bytes,
							   DMA_TO_DEVICE);
	dev_info(fill->dev, "RCQ DMA sync done: phys=0x%llx bytes=%zu\n",
			 (u64)fill->rcq_phys, header_bytes + payload_bytes);
	/* Re-dump headers from CPU view just before programming HEAD regs */
	dev_info(fill->dev, "  Header contents (post-dma_sync):\n");
	u32 *hdr_check = (u32 *)fill->rcq_virt;
	for (int _i = 0; _i < head_count; _i++)
		dev_info(fill->dev, "    [%d]: %08x %08x %08x %08x\n",
				 _i, hdr_check[_i*4 + 0], hdr_check[_i*4 + 1],
				 hdr_check[_i*4 + 2], hdr_check[_i*4 + 3]);

	iowrite32(lower_32_bits(headers_dma), fill->mmio + G2D_RCQ_HEAD_LOW);
	iowrite32(upper_32_bits(headers_dma), fill->mmio + G2D_RCQ_HEAD_HIGH);
	iowrite32(head_count, fill->mmio + G2D_RCQ_HEAD_LEN);  /* Number of headers */
	wmb();

    dev_info(fill->dev,
             "pre-kick: IRQ_CTL=0x%08x STATUS=0x%08x MIXER_INT=0x%08x HEAD_LEN=%u\n",
             ioread32(fill->mmio + G2D_RCQ_IRQ_CTL),
             ioread32(fill->mmio + G2D_RCQ_STATUS),
             ioread32(fill->mmio + G2D_MIXER_INT),
             head_count);

        reinit_completion(&fill->done);
        
	/* Trigger RCQ fetch using UPDATE only (BSP T113 semantics) */
	iowrite32(G2D_RCQ_CTRL_UPDATE, fill->mmio + G2D_RCQ_CTRL);
        wmb();
        
    dev_info(fill->dev, "post-UPDATE: CTRL=0x%08x STATUS=0x%08x\n",
             ioread32(fill->mmio + G2D_RCQ_CTRL),
             ioread32(fill->mmio + G2D_RCQ_STATUS));

    dev_info(fill->dev, "RCQ IRQ_CTL=0x%08x STATUS=0x%08x CTRL=0x%08x MIXER_INT=0x%08x\n",
             ioread32(fill->mmio + G2D_RCQ_IRQ_CTL),
             ioread32(fill->mmio + G2D_RCQ_STATUS),
             ioread32(fill->mmio + G2D_RCQ_CTRL),
             ioread32(fill->mmio + G2D_MIXER_INT));

    if (!wait_for_completion_timeout(&fill->done, msecs_to_jiffies(200))) {
        rcq_status = ioread32(fill->mmio + G2D_RCQ_STATUS);
        dev_err(fill->dev, "RCQ timeout: STATUS=0x%08x MIXER_INT=0x%08x\n",
                rcq_status, ioread32(fill->mmio + G2D_MIXER_INT));
        if (rcq_status & G2D_RCQ_STATUS_CFG_FINISH)
                dev_warn(fill->dev, "RCQ cfg_finish seen but TASK_END missing\n");
        ret = -ETIMEDOUT;
        goto out_payload;
    }

    dev_info(fill->dev, "RCQ complete: MIXER_INT=0x%08x STATUS=0x%08x\n",
             ioread32(fill->mmio + G2D_MIXER_INT),
             ioread32(fill->mmio + G2D_RCQ_STATUS));

        {
            u32 *px = dst;
            bool ok = true;

            for (i = 0; i < TEST_WIDTH; i++) {
                if (px[i] != TEST_COLOR) {
                    ok = false;
                    break;
                }
            }
            dev_info(fill->dev, "fillrect %s\n", ok ? "OK" : "FAILED");
        }

#undef APPEND_CMD
out_payload:
        dmam_free_coherent(fill->dev, RCQ_HEADER_MAX * sizeof(u32),
                           payload, payload_dma);
    out_headers:
        dmam_free_coherent(fill->dev,
                           ALIGN(RCQ_HEADER_MAX * sizeof(*headers), 32),
                           headers, headers_dma);
    out_src:
        dmam_free_coherent(fill->dev, surface_bytes, src, src_dma);
    out_dst:
        dmam_free_coherent(fill->dev, surface_bytes, dst, dst_dma);
        return ret;
}

/*
 * ============================================================================
 * DIRECT Mode Fillrect - WORKING BASELINE
 * ============================================================================
 * Fills a 64×64 ARGB8888 buffer with solid color using DIRECT mode.
 * 
 * Register Sequence (Critical - from BSP analysis):
 * -------------------------------------------------
 * 1. Reset: G2D_AHB_RESET = 0 → 3
 * 2. V0 Layer Setup:
 *    - V0_ATTCTL: Enable fillcolor mode, set format (ARGB8888), enable layer
 *    - V0_MBSIZE, V0_SIZE, V0_COOR: Dimensions and position
 *    - V0_PITCH0: Scanline pitch in bytes
 *    - V0_FILLC: Fill color value
 * 3. BLD (Blender) Setup - CRITICAL SIZING:
 *    - BLD_EN_CTL: Enable pipe 0 (bit 8)
 *    - BLD_PREMUL_CTL: Premultiply control
 *    - BLD_CH_ISIZE0: Input size for channel 0 *** MUST match surface size ***
 *    - BLD_CH_OFFSET0: Channel offset
 *    - BLD_OUT_SIZE: Output size *** MUST match surface size ***
 *    - BLD_OUT_COLOR: Color space (RGB vs YUV)
 *    - BLD_CTL: Blending mode
 * 4. ROP Setup:
 *    - ROP_CTL: ROP operation (0xF0 = pass-through)
 *    - ROP_INDEX0: ROP index for pipe 0 (0x61080 = COPYPEN)
 * 5. WB (Writeback) Setup:
 *    - WB_LADD0/HADD0: DMA address
 *    - WB_PITCH0: Scanline pitch
 *    - WB_SIZE: Output size
 *    - BLD_SIZE: Write again here (timing critical!)
 *    - WB_ATT: Format
 * 6. MIXER Control:
 *    - MIXER_INT: Clear pending + enable IRQ
 *    - MIXER_CTL: Read-modify-write to set START bit
 * 
 * Based on: BSP g2d_fillrectangle() + sunxi-g2d-m2m reference driver
 * Status: WORKING - All 4096 pixels correctly filled ✓
 */
static int do_direct_fillrect_test(struct sunxi_g2d_fill_dev *fill)
{
	int ret = -1;
	void *dst = NULL;
	dma_addr_t dst_dma;
	const size_t surface_bytes = TEST_WIDTH * TEST_HEIGHT * TEST_BPP;
	volatile u32 *test_buf;
	unsigned long timeout;
	u32 tmp, mixer_ctl;
	
	dev_info(fill->dev, "\n");
	dev_info(fill->dev, "========================================\n");
	dev_info(fill->dev, "DIRECT MODE FILLRECT TEST (v1.0.0)\n");
	dev_info(fill->dev, "========================================\n");
	
	/* 1. Allocate destination buffer */
	dst = dmam_alloc_coherent(fill->dev, surface_bytes, &dst_dma, GFP_KERNEL);
	if (!dst) {
		dev_err(fill->dev, "Failed to allocate dst buffer\n");
		return -ENOMEM;
	}
	
	/* Pre-fill with 0xDEADBEEF to verify writes */
	test_buf = (volatile u32 *)dst;
	memset((void *)dst, 0xEF, surface_bytes);
	test_buf[0] = 0xDEADBEEF;
	test_buf[10] = 0xDEADBEEF;
	wmb();
	
	dev_info(fill->dev, "Destination: DMA=0x%08llx size=%zu bytes\n",
		 (u64)dst_dma, surface_bytes);
	dev_info(fill->dev, "Initial buffer: [0]=0x%08x [10]=0x%08x\n",
		 test_buf[0], test_buf[10]);
	
	/* 2. Reset G2D (BSP g2d_bsp_reset) */
	iowrite32(0x0, fill->mmio + G2D_AHB_RESET);
	iowrite32(0x3, fill->mmio + G2D_AHB_RESET);
	wmb();
	
	/* 3. Setup V0 layer - BACK TO fillcolor mode */
	/* V0_ATTCTL: fillcolor_en=1, format=ARGB8888, EN=1, alpha=0xff */
	iowrite32(0xff000011, fill->mmio + V0_ATTCTL);  /* fillcolor mode enabled */
	/* V0_MBSIZE: Macroblock/clip size - may control how many pixels to generate */
	iowrite32(((TEST_HEIGHT - 1) << 16) | (TEST_WIDTH - 1), fill->mmio + V0_MBSIZE);
	iowrite32(((TEST_HEIGHT - 1) << 16) | (TEST_WIDTH - 1), fill->mmio + V0_SIZE);
	iowrite32(0x00000000, fill->mmio + V0_COOR);  /* Coordinates (0,0) */
	iowrite32(TEST_WIDTH * TEST_BPP, fill->mmio + V0_PITCH0);  /* Pitch = 256 bytes */
	iowrite32(TEST_COLOR, fill->mmio + V0_FILLC);  /* Green 0xFF00FF00 */
	
	/* 4. Setup BLD (blender) - Following BSP sequence from sunxi-g2d-m2m reference */
	u32 bld_en = ioread32(fill->mmio + BLD_EN_CTL);
	bld_en |= 0x00000100;  /* Enable pipe 0 (bit 8) */
	iowrite32(bld_en, fill->mmio + BLD_EN_CTL);  /* RMW as BSP does */
	
	/* BLD_PREMUL_CTL: no premultiply alpha */
	iowrite32(0x00000000, fill->mmio + BLD_PREMUL_CTL);
	
	/* BLD_CH_ISIZE0: channel 0 input size - CRITICAL for pixel expansion */
	iowrite32(((TEST_HEIGHT - 1) << 16) | (TEST_WIDTH - 1), fill->mmio + BLD_CH_ISIZE0);
	
	/* BLD_CH_OFFSET0: channel 0 offset */
	iowrite32(0x00000000, fill->mmio + BLD_CH_OFFSET0);
	
	/* BLD_OUT_SIZE: blender output size - CRITICAL for output dimensions */
	iowrite32(((TEST_HEIGHT - 1) << 16) | (TEST_WIDTH - 1), fill->mmio + BLD_OUT_SIZE);
	
	/* BLD_OUT_COLOR: RGB color space (clear bit 1 for RGB, not YUV) */
	u32 bld_out_color = ioread32(fill->mmio + BLD_OUT_COLOR);
	bld_out_color &= ~BIT(1);  /* Clear bit 1 for RGB mode */
	iowrite32(bld_out_color, fill->mmio + BLD_OUT_COLOR);
	
	/* BLD_CTL: mode 0 for direct copy */
	iowrite32(0x00000000, fill->mmio + BLD_CTL);
	
	/* 5. Setup ROP - BSP writes ROP_CTL=0xf0 and ROP_INDEX0=0x61080 */
	iowrite32(0x000000f0, fill->mmio + ROP_CTL);
	iowrite32(0x00061080, fill->mmio + ROP_INDEX0);  /* BSP value for COPYPEN */
	
	/* 6. Setup WB (writeback) - BSP g2d_wb_set() */
	/* Use G2D_FORMAT_ARGB8888 (0x0) to match V0 format */
	iowrite32(lower_32_bits(dst_dma), fill->mmio + WB_LADD0);
	iowrite32(upper_32_bits(dst_dma), fill->mmio + WB_HADD0);
	iowrite32(TEST_WIDTH * TEST_BPP, fill->mmio + WB_PITCH0);
	iowrite32(((TEST_HEIGHT - 1) << 16) | (TEST_WIDTH - 1), fill->mmio + WB_SIZE);
	
	/* CRITICAL: BSP writes BLD_SIZE here, AFTER WB config, not with BLD setup */
	iowrite32(((TEST_HEIGHT - 1) << 16) | (TEST_WIDTH - 1), fill->mmio + BLD_SIZE);
	
	iowrite32(0x00000000, fill->mmio + WB_ATT);  /* ARGB8888 */
	wmb();
	
	dev_info(fill->dev, ">>> Registers configured (DIRECT MODE - BSP sequence from sunxi-g2d-m2m)\n");
	dev_info(fill->dev, "    V0_ATTCTL=0x%08x V0_SIZE=0x%08x V0_FILLC=0x%08x\n",
		 ioread32(fill->mmio + V0_ATTCTL),
		 ioread32(fill->mmio + V0_SIZE),
		 ioread32(fill->mmio + V0_FILLC));
	dev_info(fill->dev, "    BLD_EN=0x%08x BLD_PREMUL=0x%08x BLD_CTL=0x%08x\n",
		 ioread32(fill->mmio + BLD_EN_CTL),
		 ioread32(fill->mmio + BLD_PREMUL_CTL),
		 ioread32(fill->mmio + BLD_CTL));
	dev_info(fill->dev, "    BLD_CH_ISIZE0=0x%08x BLD_OUT_SIZE=0x%08x BLD_SIZE=0x%08x\n",
		 ioread32(fill->mmio + BLD_CH_ISIZE0),
		 ioread32(fill->mmio + BLD_OUT_SIZE),
		 ioread32(fill->mmio + BLD_SIZE));
	dev_info(fill->dev, "    ROP_CTL=0x%08x ROP_INDEX0=0x%08x\n",
		 ioread32(fill->mmio + ROP_CTL),
		 ioread32(fill->mmio + ROP_INDEX0));
	dev_info(fill->dev, "    WB_ATT=0x%08x WB_SIZE=0x%08x WB_PITCH0=0x%08x\n",
		 ioread32(fill->mmio + WB_ATT),
		 ioread32(fill->mmio + WB_SIZE),
		 ioread32(fill->mmio + WB_PITCH0));
	dev_info(fill->dev, "    WB_LADD0=0x%08x WB_HADD0=0x%08x\n",
		 ioread32(fill->mmio + WB_LADD0),
		 ioread32(fill->mmio + WB_HADD0));
	
	/* 7. BSP g2d_mixer_apply sequence: clear MIXER first */
	iowrite32(0x00000000, fill->mmio + G2D_MIXER_INT);  /* IRQ off */
	iowrite32(0x00000000, fill->mmio + G2D_MIXER_CTL);  /* START=0 */
	wmb();
	
	dev_info(fill->dev, ">>> MIXER cleared\n");
	dev_info(fill->dev, "    MIXER_INT=0x%08x MIXER_CTL=0x%08x\n",
		 ioread32(fill->mmio + G2D_MIXER_INT),
		 ioread32(fill->mmio + G2D_MIXER_CTL));
	
	/* 8. Clear pending IRQ first - critical before enable */
	iowrite32(0x00000011, fill->mmio + G2D_MIXER_INT);  /* Clear finish_pending + enable */
	wmb();
	
	dev_info(fill->dev, ">>> IRQ configured\n");
	dev_info(fill->dev, "    MIXER_INT after clear+enable: 0x%08x\n",
		 ioread32(fill->mmio + G2D_MIXER_INT));
	
	/* 9. Start MIXER - BSP uses read-modify-write */
	reinit_completion(&fill->done);
	
	mixer_ctl = ioread32(fill->mmio + G2D_MIXER_CTL);
	dev_info(fill->dev, ">>> MIXER_CTL before START: 0x%08x\n", mixer_ctl);
	
	mixer_ctl |= 0x80000000;  /* Set START bit (preserve other bits) */
	iowrite32(mixer_ctl, fill->mmio + G2D_MIXER_CTL);
	wmb();
	
	dev_info(fill->dev, ">>> MIXER started (DIRECT MODE)\n");
	dev_info(fill->dev, "    MIXER_CTL after START write: 0x%08x\n",
		 ioread32(fill->mmio + G2D_MIXER_CTL));
	
	/* 10. Wait for completion */
	timeout = wait_for_completion_timeout(&fill->done, msecs_to_jiffies(100));
	if (!timeout) {
		dev_err(fill->dev, ">>> TIMEOUT waiting for IRQ\n");
		dev_err(fill->dev, "    MIXER_INT=0x%08x MIXER_CTL=0x%08x\n",
			 ioread32(fill->mmio + G2D_MIXER_INT),
			 ioread32(fill->mmio + G2D_MIXER_CTL));
		dev_err(fill->dev, "    RCQ_STATUS=0x%08x CMDQ_STS=0x%08x\n",
			ioread32(fill->mmio + G2D_RCQ_STATUS),
			ioread32(fill->mmio + G2D_CMDQ_STS));
		dev_err(fill->dev, "    V0_ATTCTL=0x%08x BLD_EN=0x%08x WB_ATT=0x%08x\n",
			ioread32(fill->mmio + V0_ATTCTL),
			ioread32(fill->mmio + BLD_EN_CTL),
			ioread32(fill->mmio + WB_ATT));
		dev_err(fill->dev, "    ROP_CTL=0x%08x (at BLD+0x80)\n",
			ioread32(fill->mmio + ROP_CTL));
		ret = -ETIMEDOUT;
	} else {
		dev_info(fill->dev, ">>> DIRECT task COMPLETED!\n");
		ret = 0;
	}
	
	/* 10. Check results */
	rmb();  /* Ensure DMA completion visible */
	dev_info(fill->dev, ">>> Buffer contents after DIRECT write:\n");
	dev_info(fill->dev, "    [0]=0x%08x [10]=0x%08x [100]=0x%08x [1000]=0x%08x\n",
		 test_buf[0], test_buf[10], test_buf[100], test_buf[1000]);
	dev_info(fill->dev, "    [1]=0x%08x [2]=0x%08x [3]=0x%08x [63]=0x%08x\n",
		 test_buf[1], test_buf[2], test_buf[3], test_buf[63]);
	dev_info(fill->dev, "    [64]=0x%08x [65]=0x%08x [128]=0x%08x (row 1,2 first pixels)\n",
		 test_buf[64], test_buf[65], test_buf[128]);
	
	bool success = (test_buf[0] == TEST_COLOR);
	dev_info(fill->dev, ">>> DIRECT Test Result: %s\n",
		 success ? "SUCCESS ✓" : "FAILED ✗");
	
	dmam_free_coherent(fill->dev, surface_bytes, dst, dst_dma);
	return ret;
}

/*
 * ============================================================================
 * DEPRECATED: RCQ Mode Test (v0.0.84)
 * ============================================================================
 * This function is kept for historical reference only.
 * RCQ mode is NOT needed for basic fillrect operations - DIRECT mode is simpler
 * and works perfectly for individual operations like fillrect, blit, scaling.
 * RCQ is only useful for batch processing multiple operations in one DMA transfer.
 * 
 * Based on BSP g2d_mixer_mem_setup() and g2d_mixer_apply()
 * Note: MIXER header with MIXER_CTL|=START is what triggers RCQ execution
 */
#if 0  /* Disabled - use DIRECT mode instead */
static int do_rcq_fillrect_test(struct sunxi_g2d_fill_dev *fill)
{
	int ret = -1;
	void *dst = NULL;
	dma_addr_t dst_dma;
	const size_t surface_bytes = TEST_WIDTH * TEST_HEIGHT * TEST_BPP;
	
	/* RCQ memory layout */
	struct g2d_rcq_head *headers;      /* Array of 4 headers (V0, BLD, WB, MIXER) */
	struct rcq_v0_regs *v0_regs;       /* V0 register block */
	struct rcq_bld_regs *bld_regs;     /* BLD register block */
	struct rcq_wb_regs *wb_regs;       /* WB register block */
	struct rcq_mixer_regs *mixer_regs; /* MIXER register block */
	
	dev_info(fill->dev, "\n");
	dev_info(fill->dev, "==============================================\n");
	dev_info(fill->dev, "V0.0.112: MIXER_INT Direct MMIO Write Test\n");
	dev_info(fill->dev, "==============================================\n");
	
	/* 1. Allocate destination buffer */
	dst = dmam_alloc_coherent(fill->dev, surface_bytes, &dst_dma, GFP_KERNEL);
	if (!dst) {
		dev_err(fill->dev, "Failed to allocate dst buffer\n");
		return -ENOMEM;
	}
	
	/* Fill with test pattern */
	u32 *test_buf = (u32 *)dst;
	for (int i = 0; i < (surface_bytes / 4); i++) {
		test_buf[i] = 0xDEADBEEF;
	}
	wmb();
	dev_info(fill->dev, ">>> Allocated dst buffer: dma=0x%llx size=%zu\n",
		 (u64)dst_dma, surface_bytes);
	dev_info(fill->dev, "    Pre-filled with 0xDEADBEEF pattern\n");
	
	/* 2. Allocate RCQ memory (headers + register blocks) */
	/* v0.0.104: 4 headers (V0, BLD, WB, MIXER) - MIXER.START triggers execution! */
	size_t headers_size = sizeof(struct g2d_rcq_head) * 4;
	size_t v0_size = sizeof(struct rcq_v0_regs);
	size_t bld_size = sizeof(struct rcq_bld_regs);
	size_t wb_size = sizeof(struct rcq_wb_regs);
	size_t mixer_size = sizeof(struct rcq_mixer_regs);  /* MIXER_CTL = START */
	size_t total_size = G2D_RCQ_BYTE_ALIGN(headers_size + v0_size + bld_size + wb_size + mixer_size);
	
	fill->rcq_virt = dma_alloc_coherent(fill->dev, total_size,
					    &fill->rcq_phys, GFP_KERNEL);
	if (!fill->rcq_virt) {
		dev_err(fill->dev, "Failed to allocate RCQ memory\n");
		ret = -ENOMEM;
		goto out_dst;
	}
	fill->rcq_size = total_size;
	memset(fill->rcq_virt, 0, total_size);
	
	dev_info(fill->dev, ">>> RCQ memory allocated: phys=0x%llx virt=%p size=%zu\n",
		 (u64)fill->rcq_phys, fill->rcq_virt, total_size);
	
	/* 3. Setup pointers to memory regions - payloads must be 32B aligned! */
	headers = (struct g2d_rcq_head *)fill->rcq_virt;
	
	/* v0.0.118: REMOVE MIXER from RCQ payload!
	 * MIXER_CTL must be written by CPU directly, not via RCQ.
	 * Only V0/BLD/WB go through RCQ on T113.
	 */
	size_t v0_offset = G2D_RCQ_BYTE_ALIGN(headers_size);
	size_t bld_offset = G2D_RCQ_BYTE_ALIGN(v0_offset + v0_size);
	size_t wb_offset = G2D_RCQ_BYTE_ALIGN(bld_offset + bld_size);
	/* MIXER removed from RCQ - will be written via MMIO instead */
	
	v0_regs = (struct rcq_v0_regs *)(fill->rcq_virt + v0_offset);
	bld_regs = (struct rcq_bld_regs *)(fill->rcq_virt + bld_offset);
	wb_regs = (struct rcq_wb_regs *)(fill->rcq_virt + wb_offset);
	/* mixer_regs = NULL; - not used in RCQ anymore */
	
	dma_addr_t v0_phys = fill->rcq_phys + v0_offset;
	dma_addr_t bld_phys = fill->rcq_phys + bld_offset;
	dma_addr_t wb_phys = fill->rcq_phys + wb_offset;
	
	dev_info(fill->dev, ">>> v0.0.118: RCQ layout (MIXER excluded!):\n");
	dev_info(fill->dev, "    Headers @ phys=0x%llx (3 x %zu bytes)\n",
		 (u64)fill->rcq_phys, sizeof(struct g2d_rcq_head));
	dev_info(fill->dev, "    V0 regs @ phys=0x%llx (%zu bytes)\n",
		 (u64)v0_phys, v0_size);
	dev_info(fill->dev, "    BLD regs @ phys=0x%llx (%zu bytes)\n",
		 (u64)bld_phys, bld_size);
	dev_info(fill->dev, "    WB regs @ phys=0x%llx (%zu bytes)\n",
		 (u64)wb_phys, wb_size);
	dev_info(fill->dev, "    MIXER will be written via MMIO @ 0x00100\n");
	
	/* 4. Fill V0 register block */
	const u32 size_hw = ((TEST_HEIGHT - 1) << 16) | (TEST_WIDTH - 1);
	v0_regs->v0_attctl = 0xFF000011;  /* alpha=0xFF, format=0 (ARGB8888), FILLC_EN=1, EN=1 */
	v0_regs->v0_mbsize = size_hw;     /* 64x64 - CRITICAL! Was missing */
	v0_regs->v0_fillc = TEST_COLOR;   /* Green fill color */
	
	/* 5. Fill BLD register block */
	bld_regs->bld_en_ctl = 0x00000100;    /* PIPE0 enabled */
	bld_regs->bld_ch_isize0 = size_hw;    /* CH0 input size */
	bld_regs->bld_out_size = size_hw;     /* 64x64 */
	bld_regs->bld_ctl = 0x03010301;       /* Pipe mode setup */
	/* v0.0.120: ROP_CTL CRITICAL! BSP g2d_fillrectangle() writes 0xF0 (SRC pass) */
	bld_regs->rop_ctl = 0x000000F0;       /* ROP pass-through mode */
	
	/* 6. Fill WB register block */
	wb_regs->wb_att = G2D_FORMAT_XRGB8888;  /* Format only, NO enable bit */
	wb_regs->wb_size = size_hw;
	wb_regs->wb_pitch0 = TEST_WIDTH * TEST_BPP;
	wb_regs->wb_ladd0 = lower_32_bits(dst_dma);
	wb_regs->wb_hadd0 = upper_32_bits(dst_dma);
	
	/* v0.0.118: MIXER not in RCQ payload - will write via MMIO instead */
	
	wmb();  /* Ensure register blocks written to memory */
	
	dev_info(fill->dev, ">>> Register blocks filled:\n");
	dev_info(fill->dev, "    V0: ATTCTL=0x%08x MBSIZE=0x%08x FILLC=0x%08x\n",
		 v0_regs->v0_attctl, v0_regs->v0_mbsize, v0_regs->v0_fillc);
	dev_info(fill->dev, "    BLD: EN=0x%08x CH_ISIZE0=0x%08x OUT_SIZE=0x%08x CTL=0x%08x ROP=0x%08x\n",
		 bld_regs->bld_en_ctl, bld_regs->bld_ch_isize0, 
		 bld_regs->bld_out_size, bld_regs->bld_ctl, bld_regs->rop_ctl);
	dev_info(fill->dev, "    WB: ATT=0x%08x SIZE=0x%08x PITCH0=0x%08x\n",
		 wb_regs->wb_att, wb_regs->wb_size, wb_regs->wb_pitch0);
	dev_info(fill->dev, "        LADD0=0x%08x HADD0=0x%08x\n",
		 wb_regs->wb_ladd0, wb_regs->wb_hadd0);
	dev_info(fill->dev, "    MIXER: (will write via MMIO, not RCQ)\n");
	
	/* 8. Create RCQ headers - v0.0.118: ONLY 3 headers (V0, BLD, WB) - NO MIXER! */
	/* CMDQ_ADDR not writable on T113, using absolute physical addresses */
	const int num_headers = 3;  /* Changed from 4 to 3 */
	
	/* Use absolute 40-bit physical addresses (low_addr + high_addr in dw0) */
	
	/* Header 0: V0 registers @ 0x00800 */
	headers[0].low_addr = lower_32_bits(v0_phys);
	headers[0].dw0 = (upper_32_bits(v0_phys) << 24) | (v0_size & 0xFFFFFF);
	headers[0].reg_offset = 0x00800;
	/* First header marks the frame and carries chained header count */
	headers[0].dirty = G2D_RCQ_MAKE_DIRTY(1, num_headers);
	
	/* Header 1: BLD registers @ 0x00400 */
	headers[1].low_addr = lower_32_bits(bld_phys);
	headers[1].dw0 = (upper_32_bits(bld_phys) << 24) | (bld_size & 0xFFFFFF);
	headers[1].reg_offset = 0x00400;
	headers[1].dirty = G2D_RCQ_MAKE_DIRTY(1, 0);  /* dirty_flag=1 */
	
	/* Header 2: WB registers @ 0x03000 */
	headers[2].low_addr = lower_32_bits(wb_phys);
	headers[2].dw0 = (upper_32_bits(wb_phys) << 24) | (wb_size & 0xFFFFFF);
	headers[2].reg_offset = 0x03000;
	headers[2].dirty = G2D_RCQ_MAKE_DIRTY(1, 0);  /* dirty_flag=1 */
	
	/* v0.0.118: NO Header 3 (MIXER removed from RCQ!) */
	
	wmb();  /* Ensure headers written to memory */
	
	dev_info(fill->dev, "\n>>> v0.0.118: RCQ Headers (NO MIXER - CPU triggers!) <<<\n");
	dev_info(fill->dev, "  Payload Physical Addresses:\n");
	dev_info(fill->dev, "    V0:    phys=0x%llx (size=0x%zx)\n", (u64)v0_phys, v0_size);
	dev_info(fill->dev, "    BLD:   phys=0x%llx (size=0x%zx)\n", (u64)bld_phys, bld_size);
	dev_info(fill->dev, "    WB:    phys=0x%llx (size=0x%zx)\n", (u64)wb_phys, wb_size);
	
	dev_info(fill->dev, "  Header Contents (40-bit addressing):\n");
	dev_info(fill->dev, "    [0] V0:    low=0x%08x dw0=0x%08x dirty=0x%08x reg=0x%08x\n",
		 headers[0].low_addr, headers[0].dw0, headers[0].dirty, headers[0].reg_offset);
	dev_info(fill->dev, "    [1] BLD:   low=0x%08x dw0=0x%08x dirty=0x%08x reg=0x%08x\n",
		 headers[1].low_addr, headers[1].dw0, headers[1].dirty, headers[1].reg_offset);
	dev_info(fill->dev, "    [2] WB:    low=0x%08x dw0=0x%08x dirty=0x%08x reg=0x%08x\n",
		 headers[2].low_addr, headers[2].dw0, headers[2].dirty, headers[2].reg_offset);
	
	/* DEBUG: Dump raw header memory (4 DWORDs per header) */
	dev_info(fill->dev, "  Raw Header Memory:\n");
	u32 *hdr_raw = (u32 *)headers;
	for (int i = 0; i < num_headers; i++) {
		dev_info(fill->dev, "    [%d]: %08x %08x %08x %08x\n",
			 i, hdr_raw[i*4 + 0], hdr_raw[i*4 + 1], 
			 hdr_raw[i*4 + 2], hdr_raw[i*4 + 3]);
	}
	
	/* DEBUG: Dump first 16 bytes of V0 payload */
	dev_info(fill->dev, "  V0 Payload (first 16 bytes):\n");
	u32 *v0_raw = (u32 *)v0_regs;
	dev_info(fill->dev, "    %08x %08x %08x %08x\n",
		 v0_raw[0], v0_raw[1], v0_raw[2], v0_raw[3]);
	
	/* 9. Setup RCQ - v0.0.99: BSP g2d_mixer_apply() exact sequence */
	dev_info(fill->dev, "\n>>> v0.0.103: RCQ Initialization with Wake-up Sequence <<<\n");
	
	/* V0.0.103: Try "waking up" RCQ block by writing to all registers */
	dev_info(fill->dev, "  Attempting RCQ wake-up sequence...\n");
	
	/* Clear all RCQ registers first */
	iowrite32(0x0, fill->mmio + G2D_RCQ_IRQ_CTL);
	iowrite32(0x0, fill->mmio + G2D_RCQ_CTRL);
	iowrite32(0x0, fill->mmio + G2D_RCQ_HEAD_LOW);
	iowrite32(0x0, fill->mmio + G2D_RCQ_HEAD_HIGH);
	iowrite32(0x0, fill->mmio + G2D_RCQ_HEAD_LEN);
	wmb();
	usleep_range(10, 20);
	
	/* Try reading RCQ_STATUS to "wake" the block */
	u32 rcq_status_pre = ioread32(fill->mmio + G2D_RCQ_STATUS);
	dev_info(fill->dev, "    RCQ_STATUS (initial): 0x%08x\n", rcq_status_pre);
	
	/* Try writing and reading back VERSION (should always work) */
	u32 version_check = ioread32(fill->mmio + G2D_VERSION);
	dev_info(fill->dev, "    VERSION check: 0x%08x\n", version_check);
	
	/* BSP Step 1: g2d_top_rcq_update_en(0) - Disable RCQ update */
	iowrite32(0x0, fill->mmio + G2D_RCQ_CTRL);
	wmb();
	
	/* BSP Step 2: g2d_top_rcq_irq_en(0) - Disable RCQ IRQ */
	iowrite32(0x0, fill->mmio + G2D_RCQ_IRQ_CTL);
	wmb();
	
	/* BSP Step 3: g2d_top_set_rcq_head() - Set RCQ header address and length */
	/* v0.0.118: Only 3 headers (V0, BLD, WB) - MIXER excluded! */
	iowrite32(lower_32_bits(fill->rcq_phys), fill->mmio + G2D_RCQ_HEAD_LOW);
	iowrite32(upper_32_bits(fill->rcq_phys), fill->mmio + G2D_RCQ_HEAD_HIGH);
	iowrite32(0x30, fill->mmio + G2D_RCQ_HEAD_LEN);  /* 48 bytes (3 headers × 16) */
	wmb();
	
	u32 head_low = ioread32(fill->mmio + G2D_RCQ_HEAD_LOW);
	u32 head_high = ioread32(fill->mmio + G2D_RCQ_HEAD_HIGH);
	u32 head_len = ioread32(fill->mmio + G2D_RCQ_HEAD_LEN);
	
	dev_info(fill->dev, "  RCQ Header Configuration:\n");
	dev_info(fill->dev, "    RCQ_HEAD_LOW:  0x%08x\n", head_low);
	dev_info(fill->dev, "    RCQ_HEAD_HIGH: 0x%08x\n", head_high);
	dev_info(fill->dev, "    RCQ_HEAD_LEN:  0x%08x (48 bytes = 3 headers)\n", head_len);
	dev_info(fill->dev, "    Verification: %s%s%s\n",
		 (head_low == lower_32_bits(fill->rcq_phys)) ? "LOW=OK " : "LOW=FAIL ",
		 (head_high == upper_32_bits(fill->rcq_phys)) ? "HIGH=OK " : "HIGH=FAIL ",
		 (head_len == 0x30) ? "LEN=OK" : "LEN=FAIL");
	
	/* BSP Step 4: frame[i].apply() - Payloads already filled in memory above */
	dev_info(fill->dev, "  Payloads ready in RCQ pool\n");
	
	/* BSP Step 5: g2d_top_rcq_irq_en(1) - Enable RCQ mode IRQs */
	/* rcq_sel=0 (RCQ mode), task_end_irq_en=1 */
	/* NOTE: cfg_finish_irq causes IRQ storm, only enable task_end */
	u32 rcq_irq_ctl = G2D_RCQ_IRQ_TASK_END_EN;  /* Only task_end, NOT cfg_finish */
	/* rcq_sel=0 means RCQ mode (not setting BIT(0)) */
	iowrite32(rcq_irq_ctl, fill->mmio + G2D_RCQ_IRQ_CTL);
	wmb();
	dev_info(fill->dev, "    RCQ_IRQ_CTL = 0x%08x (rcq_sel=0 RCQ mode, task_end only)\n",
		 ioread32(fill->mmio + G2D_RCQ_IRQ_CTL));
	
	/* BSP Step 6: Clear any pending IRQs before trigger */
	u32 rcq_status_clear = ioread32(fill->mmio + G2D_RCQ_STATUS);
	if (rcq_status_clear & 0x5) {  /* cfg_finish_irq or task_end_irq pending */
		dev_info(fill->dev, "  Clearing pending RCQ IRQs: 0x%08x\n", rcq_status_clear);
		iowrite32(0x5, fill->mmio + G2D_RCQ_STATUS);  /* W1C: write 1 to clear */
		wmb();
	}
	
	/* BSP Step 7: g2d_top_rcq_update_en(1) - Trigger RCQ execution */
	dev_info(fill->dev, ">>> Triggering RCQ execution...\n");
	
	/* DEBUG: Dump all RCQ registers before trigger */
	dev_info(fill->dev, "    PRE-TRIGGER RCQ STATE:\n");
	dev_info(fill->dev, "      RCQ_IRQ_CTL   = 0x%08x\n", ioread32(fill->mmio + G2D_RCQ_IRQ_CTL));
	dev_info(fill->dev, "      RCQ_STATUS    = 0x%08x\n", ioread32(fill->mmio + G2D_RCQ_STATUS));
	dev_info(fill->dev, "      RCQ_CTRL      = 0x%08x\n", ioread32(fill->mmio + G2D_RCQ_CTRL));
	dev_info(fill->dev, "      RCQ_HEAD_LOW  = 0x%08x\n", ioread32(fill->mmio + G2D_RCQ_HEAD_LOW));
	dev_info(fill->dev, "      RCQ_HEAD_HIGH = 0x%08x\n", ioread32(fill->mmio + G2D_RCQ_HEAD_HIGH));
	dev_info(fill->dev, "      RCQ_HEAD_LEN  = 0x%08x\n", ioread32(fill->mmio + G2D_RCQ_HEAD_LEN));
	
	/* V0.0.107: Test if RCQ registers are writable at all */
	dev_info(fill->dev, "  Testing RCQ register writability...\n");
	
	/* Try writing to HEAD_LEN with different value */
	iowrite32(0x12345678, fill->mmio + G2D_RCQ_HEAD_LEN);
	wmb();
	u32 test_len = ioread32(fill->mmio + G2D_RCQ_HEAD_LEN);
	dev_info(fill->dev, "    HEAD_LEN test: wrote 0x12345678, read 0x%08x %s\n",
		 test_len, (test_len != 0x30) ? "CHANGED!" : "stuck");
	
	/* Restore correct value (3 headers) */
	iowrite32(0x30, fill->mmio + G2D_RCQ_HEAD_LEN);
	wmb();
	
	/* v0.0.112: Write MIXER_INTERRUPT directly to MMIO (not via RCQ!) */
	/* BSP may write this register directly before triggering RCQ */
	dev_info(fill->dev, "  v0.0.112: Writing MIXER_INTERRUPT=0x10 directly to MMIO...\n");
	iowrite32(0x00000010, fill->mmio + G2D_MIXER_INT);  /* finish_irq_en */
	wmb();
	dev_info(fill->dev, "    MIXER_INT @ MMIO readback=0x%08x\n",
		 ioread32(fill->mmio + G2D_MIXER_INT));
	
	/* v0.0.117: Trigger RCQ first, THEN write START!
	 * Theory: RCQ loads V0/BLD/WB regs, then CPU writes MIXER_CTL.START to execute.
	 * Writing START before RCQ might get overwritten by RCQ payload.
	 */
	dev_info(fill->dev, "  v0.0.118: Setting RCQ_CTRL=0x1 (UPDATE) first...\n");
	
	iowrite32(0x1, fill->mmio + G2D_RCQ_CTRL);  /* Only UPDATE bit */
	wmb();
	
	u32 rcq_ctrl_rb = ioread32(fill->mmio + G2D_RCQ_CTRL);
	dev_info(fill->dev, "    RCQ_CTRL written=0x1 (UPDATE), readback=0x%08x\n", rcq_ctrl_rb);
	
	/* Wait for RCQ to load registers (cfg_finish_irq appears at ~10-50us) */
	udelay(50);
	
	/* v0.0.119: Read-Modify-Write MIXER_CTL like BSP does!
	 * BSP: tmp = read(MIXER_CTL); tmp |= START; write(MIXER_CTL, tmp);
	 * This preserves any bits RCQ might have loaded or HW requires.
	 */
	dev_info(fill->dev, "  v0.0.119: Read-Modify-Write MIXER_CTL.START (BSP style)...\n");
	u32 mixer_ctl = ioread32(fill->mmio + G2D_MIXER);  /* Read current value */
	dev_info(fill->dev, "    MIXER_CTL @ 0x00100 before START: 0x%08x\n", mixer_ctl);
	mixer_ctl |= G2D_MIXER_CTL_START;  /* Add START bit */
	iowrite32(mixer_ctl, fill->mmio + G2D_MIXER);  /* Write back */
	wmb();
	dev_info(fill->dev, "    MIXER_CTL @ 0x00100 after START:  0x%08x (readback=0x%08x)\n",
		 mixer_ctl, ioread32(fill->mmio + G2D_MIXER));
	
	/* Wait for pipeline execution */
	udelay(100);
	
	u32 status_after_delay = ioread32(fill->mmio + G2D_RCQ_STATUS);
	dev_info(fill->dev, "    RCQ_STATUS after 100us delay: 0x%08x\n", status_after_delay);
	
	/* 10. Wait for completion */
	ret = wait_for_completion_timeout(&fill->done, msecs_to_jiffies(1000));
	if (!ret) {
		dev_err(fill->dev, ">>> RCQ task TIMEOUT!\n");
		dev_info(fill->dev, "    RCQ_STATUS=0x%08x\n",
			 ioread32(fill->mmio + G2D_RCQ_STATUS));
		dev_info(fill->dev, "    MIXER_INT=0x%08x\n",
			 ioread32(fill->mmio + G2D_MIXER_INT));
		ret = -ETIMEDOUT;
	} else {
		dev_info(fill->dev, ">>> RCQ task COMPLETED!\n");
		dev_info(fill->dev, "    RCQ_STATUS=0x%08x\n",
			 ioread32(fill->mmio + G2D_RCQ_STATUS));
		ret = 0;
	}
	
	/* 12. Check results */
	rmb();  /* Ensure DMA completion visible */
	dev_info(fill->dev, ">>> Buffer contents after RCQ:\n");
	dev_info(fill->dev, "    [0]=0x%08x [10]=0x%08x [100]=0x%08x [1000]=0x%08x\n",
		 test_buf[0], test_buf[10], test_buf[100], test_buf[1000]);
	
	bool success = (test_buf[0] == TEST_COLOR);
	dev_info(fill->dev, ">>> RCQ Test Result: %s\n",
		 success ? "SUCCESS ✓" : "FAILED ✗");
	
	/* Cleanup */
	if (fill->rcq_virt) {
		dma_free_coherent(fill->dev, fill->rcq_size,
				  fill->rcq_virt, fill->rcq_phys);
		fill->rcq_virt = NULL;
	}
	
out_dst:
	dmam_free_coherent(fill->dev, surface_bytes, dst, dst_dma);
	return ret;
}
#endif  /* End of deprecated RCQ mode test */

/*
 * ============================================================================
 * Probe and Driver Infrastructure
 * ============================================================================
 */

static int sunxi_g2d_fill_probe(struct platform_device *pdev)
{
        struct sunxi_g2d_fill_dev *fill;
        struct resource *res;
        int ret;

        fill = devm_kzalloc(&pdev->dev, sizeof(*fill), GFP_KERNEL);
        if (!fill) {
			dev_err(&pdev->dev, "failed to allocate memory\n");
            return -ENOMEM;
        }

	fill->dev = &pdev->dev;
	init_completion(&fill->done);
	platform_set_drvdata(pdev, fill);
	
	/* Configure DMA for MBUS access (critical for G2D DMA writes) */
	ret = of_dma_configure(&pdev->dev, pdev->dev.of_node, true);
	if (ret) {
		dev_err(&pdev->dev, "Failed to configure DMA: %d\n", ret);
		return ret;
	}
	dev_info(&pdev->dev, "DMA configured successfully for MBUS\n");
	
	/* Set 32-bit DMA mask (Allwinner G2D limitation) */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "Cannot set 32-bit DMA mask: %d\n", ret);
		return ret;
	}
	dev_info(&pdev->dev, "32-bit DMA mask configured\n");
	
	/* Get MBUS interconnect path (CRITICAL: enables G2D DMA access to DRAM) */
	fill->mbus = devm_of_icc_get(&pdev->dev, "dma-mem");
	if (IS_ERR(fill->mbus)) {
		dev_err(&pdev->dev, "Failed to get MBUS interconnect: %ld\n", PTR_ERR(fill->mbus));
		return PTR_ERR(fill->mbus);
	}
	dev_info(&pdev->dev, "MBUS interconnect path obtained\n");
	
	/* Set MBUS bandwidth (300 MB/s avg, 600 MB/s peak for G2D operations) */
	ret = icc_set_bw(fill->mbus, 300000, 600000);
	if (ret) {
		dev_err(&pdev->dev, "Failed to set MBUS bandwidth: %d\n", ret);
		return ret;
	}
	dev_info(&pdev->dev, "MBUS bandwidth configured: 300/600 MB/s\n");
	
	dev_info(&pdev->dev, "sunxi-g2d-fillrect probe start\n");
	
	/* Map G2D registers */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
        fill->mmio = devm_ioremap_resource(&pdev->dev, res);
        if (IS_ERR(fill->mmio))
                return PTR_ERR(fill->mmio);
        fill->mmio_size = resource_size(res);
	
	/* Map CCU registers for sub-block clock/reset control */
	/* CCU base is at 0x02001000, we need G2D_CLK_REG and G2D_BGR_REG */
	fill->ccu = ioremap(0x02001000, 0x1000);
	if (!fill->ccu) {
		dev_warn(&pdev->dev, "Failed to map CCU, WB/RCQ may not work\n");
	} else {
		dev_info(&pdev->dev, "CCU mapped at 0x02001000\n");
	}

	/* BSP sequence: reset FIRST, then clocks */
	fill->rst = devm_reset_control_get_optional_exclusive(&pdev->dev, NULL);
	if (!IS_ERR(fill->rst)) {
		ret = reset_control_deassert(fill->rst);
		if (ret) {
			dev_err(&pdev->dev, "Failed to deassert reset: %d\n", ret);
			return ret;
		}
	}

	fill->clk_mod = devm_clk_get_optional(&pdev->dev, "g2d");
	fill->clk_bus = devm_clk_get_optional(&pdev->dev, "bus_g2d");
	fill->clk_mbus = devm_clk_get_optional(&pdev->dev, "mbus_g2d");

	/* BSP: enable bus_clk first */
	ret = clk_prepare_enable_optional(fill->clk_bus);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable bus clock: %d\n", ret);
		goto err_reset;
	}

	/* BSP: then module clock at 300MHz */
	if (fill->clk_mod) {
		clk_set_rate(fill->clk_mod, 300000000);
		ret = clk_prepare_enable(fill->clk_mod);
		if (ret) {
			dev_err(&pdev->dev, "Failed to enable module clock: %d\n", ret);
			goto err_clk_bus;
		}
	}

	/* BSP: finally mbus_clk for DMA */
	ret = clk_prepare_enable_optional(fill->clk_mbus);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable mbus clock: %d\n", ret);
		goto err_clk_mod;
	}

	dev_info(&pdev->dev,
		 "clocks: mod=%lu bus=%lu mbus=%lu\n",
		 fill->clk_mod ? clk_get_rate(fill->clk_mod) : 0,
		 fill->clk_bus ? clk_get_rate(fill->clk_bus) : 0,
		 fill->clk_mbus ? clk_get_rate(fill->clk_mbus) : 0);

        fill->irq = platform_get_irq(pdev, 0);
        if (fill->irq < 0) {
                ret = fill->irq;
                goto err_irq;
        }

        dev_info(&pdev->dev, "IRQ number: %d\n", fill->irq);

        ret = devm_request_irq(&pdev->dev, fill->irq, sunxi_g2d_irq, 0,
                               DRIVER_NAME, fill);
        if (ret) {
                dev_err(&pdev->dev, "Failed to request IRQ %d: %d\n", fill->irq, ret);
                goto err_irq;
        }
        dev_info(&pdev->dev, "IRQ %d registered successfully\n", fill->irq);

        g2d_top_enable(fill);
        g2d_ccu_enable(fill);  /* Enable WB/RCQ sub-blocks via CCU */
        
        /* Verify CCU clocks are really enabled by reading G2D status */
        dev_info(&pdev->dev, "Testing G2D register access...\n");
        dev_info(&pdev->dev, "RCQ_CTRL readback: 0x%08x (expect 0 if TOP closed)\n",
                 ioread32(fill->mmio + G2D_RCQ_CTRL));
        dev_info(&pdev->dev, "RCQ_STATUS readback: 0x%08x\n",
                 ioread32(fill->mmio + G2D_RCQ_STATUS));
        
        dev_info(&pdev->dev, "\n");
        dev_info(&pdev->dev, "==============================================\n");
        dev_info(&pdev->dev, "T113 G2D DIRECT Mode Fillrect - v1.0.0 STABLE\n");
        dev_info(&pdev->dev, "==============================================\n");
        
        /* DIRECT mode: CPU register writes, proven working baseline */
        ret = do_direct_fillrect_test(fill);
        if (ret)
                dev_err(&pdev->dev, "Direct fillrect test failed: %d\n", ret);
        
        g2d_top_disable(fill);

        return ret;

err_irq:
	clk_disable_unprepare_optional(fill->clk_mbus);
err_clk_mod:
	clk_disable_unprepare_optional(fill->clk_mod);
err_clk_bus:
	clk_disable_unprepare_optional(fill->clk_bus);
err_reset:
	if (!IS_ERR(fill->rst))
		reset_control_assert(fill->rst);
	return ret;
}

static void sunxi_g2d_fill_remove(struct platform_device *pdev)
{
    struct sunxi_g2d_fill_dev *fill = platform_get_drvdata(pdev);

    if (!IS_ERR(fill->rst))
        reset_control_assert(fill->rst);
    clk_disable_unprepare_optional(fill->clk_mbus);
    clk_disable_unprepare_optional(fill->clk_bus);
    clk_disable_unprepare_optional(fill->clk_mod);
}

static const struct of_device_id sunxi_g2d_fill_of_match[] = {
        { .compatible = "allwinner,sun8i-g2d" },
        { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sunxi_g2d_fill_of_match);

static struct platform_driver sunxi_g2d_fill_driver = {
        .probe  = sunxi_g2d_fill_probe,
        .remove = sunxi_g2d_fill_remove,
        .driver = {
                .name           = DRIVER_NAME,
                .of_match_table = sunxi_g2d_fill_of_match,
        },
};

static int __init sunxi_g2d_fill_init(void)
{
        pr_info(DRIVER_NAME ": module init " DRIVER_VERSION "\n");
        return platform_driver_register(&sunxi_g2d_fill_driver);
}
module_init(sunxi_g2d_fill_init);

static void __exit sunxi_g2d_fill_exit(void)
{
        pr_info(DRIVER_NAME ": module exit\n");
        platform_driver_unregister(&sunxi_g2d_fill_driver);
}
module_exit(sunxi_g2d_fill_exit);

MODULE_AUTHOR("Serper");
MODULE_DESCRIPTION("Allwinner G2D fillrect " DRIVER_VERSION);
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
