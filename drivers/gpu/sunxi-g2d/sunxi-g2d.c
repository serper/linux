// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Allwinner G2D Hardware Accelerator Driver
 * 
 * Copyright (C) 2025 Sergio Perez
 *
 * Architecture:
 * - Char device with custom UAPI ioctls for graphics/UI acceleration
 * - DMA-BUF import/export support
 * - Sync fence integration with DRM
 * - Power management with reference counting
 * - Job queue with workqueue for async operations
 * - Future: V4L2 M2M layer on top
 *
 * Based on fillrect v1.0.0 STABLE proven initialization sequence
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/clk.h>
#include <linux/reset.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/sync_file.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>
#include <linux/interconnect.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/iosys-map.h>
#include <uapi/linux/sunxi_g2d.h>
#include "sunxi-g2d-regs.h"

#define DRIVER_NAME		"sunxi-g2d"
#define DRIVER_VERSION		"2.0.0"
#define DRIVER_MAJOR		2
#define DRIVER_MINOR		0
#define DRIVER_PATCHLEVEL	0

/* VSU scaling filter coefficients - from BSP g2d_scal.c */
static const s32 linearcoefftab32[32] = {
	0x00004000, 0x00023e00, 0x00043c00, 0x00063a00, 0x00083800,
	0x000a3600, 0x000c3400, 0x000e3200, 0x00103000, 0x00122e00,
	0x00142c00, 0x00162a00, 0x00182800, 0x001a2600, 0x001c2400,
	0x001e2200, 0x00202000, 0x00221e00, 0x00241c00, 0x00261a00,
	0x00281800, 0x002a1600, 0x002c1400, 0x002e1200, 0x00301000,
	0x00320e00, 0x00340c00, 0x00360a00, 0x00380800, 0x003a0600,
	0x003c0400, 0x003e0200,
};

static const s32 lan2coefftab32_full[512] = {
	0x00004000, 0x000140ff, 0x00033ffe, 0x00043ffd, 0x00063efc, 0xff083dfc,
	0x000a3bfb, 0xff0d39fb, 0xff0f37fb, 0xff1136fa, 0xfe1433fb,
	0xfe1631fb, 0xfd192ffb, 0xfd1c2cfb, 0xfd1f29fb, 0xfc2127fc,
	0xfc2424fc, 0xfc2721fc, 0xfb291ffd, 0xfb2c1cfd, 0xfb2f19fd,
	0xfb3116fe, 0xfb3314fe, 0xfa3611ff, 0xfb370fff, 0xfb390dff,
	0xfb3b0a00, 0xfc3d08ff, 0xfc3e0600, 0xfd3f0400, 0xfe3f0300,
	0xff400100,
	/* counter = 1 */
	0x00004000, 0x000140ff, 0x00033ffe, 0x00043ffd, 0x00063efc,
	0xff083dfc, 0x000a3bfb, 0xff0d39fb, 0xff0f37fb, 0xff1136fa,
	0xfe1433fb, 0xfe1631fb, 0xfd192ffb, 0xfd1c2cfb, 0xfd1f29fb,
	0xfc2127fc, 0xfc2424fc, 0xfc2721fc, 0xfb291ffd, 0xfb2c1cfd,
	0xfb2f19fd, 0xfb3116fe, 0xfb3314fe, 0xfa3611ff, 0xfb370fff,
	0xfb390dff, 0xfb3b0a00, 0xfc3d08ff, 0xfc3e0600, 0xfd3f0400,
	0xfe3f0300, 0xff400100,
	/* counter = 2 */
	0xff053804, 0xff063803, 0xff083801, 0xff093701, 0xff0a3700,
	0xff0c3500, 0xff0e34ff, 0xff1033fe, 0xff1232fd, 0xfe1431fd,
	0xfe162ffd, 0xfe182dfd, 0xfd1b2cfc, 0xfd1d2afc, 0xfd1f28fc,
	0xfd2126fc, 0xfd2323fd, 0xfc2621fd, 0xfc281ffd, 0xfc2a1dfd,
	0xfc2c1bfd, 0xfd2d18fe, 0xfd2f16fe, 0xfd3114fe, 0xfd3212ff,
	0xfe3310ff, 0xff340eff, 0x00350cff, 0x00360a00, 0x01360900,
	0x02370700, 0x03370600,
	/* counter = 3 */
	0xff083207, 0xff093206, 0xff0a3205, 0xff0c3203, 0xff0d3103,
	0xff0e3102, 0xfe113001, 0xfe132f00, 0xfe142e00, 0xfe162dff,
	0xfe182bff, 0xfe192aff, 0xfe1b29fe, 0xfe1d27fe, 0xfe1f25fe,
	0xfd2124fe, 0xfe2222fe, 0xfe2421fd, 0xfe251ffe, 0xfe271dfe,
	0xfe291bfe, 0xff2a19fe, 0xff2b18fe, 0xff2d16fe, 0x002e14fe,
	0x002f12ff, 0x013010ff, 0x02300fff, 0x03310dff, 0x04310cff,
	0x05310a00, 0x06310900,
	/* counter = 4 */
	0xff0a2e09, 0xff0b2e08, 0xff0c2e07, 0xff0e2d06, 0xff0f2d05,
	0xff102d04, 0xff122c03, 0xfe142c02, 0xfe152b02, 0xfe172a01,
	0xfe182901, 0xfe1a2800, 0xfe1b2700, 0xfe1d2500, 0xff1e24ff,
	0xfe2023ff, 0xff2121ff, 0xff2320fe, 0xff241eff, 0x00251dfe,
	0x00261bff, 0x00281afe, 0x012818ff, 0x012a16ff, 0x022a15ff,
	0x032b13ff, 0x032c12ff, 0x052c10ff, 0x052d0fff, 0x062d0d00,
	0x072d0c00, 0x082d0b00,
	/* counter = 5 */
	0xff0c2a0b, 0xff0d2a0a, 0xff0e2a09, 0xff0f2a08, 0xff102a07,
	0xff112a06, 0xff132905, 0xff142904, 0xff162803, 0xff172703,
	0xff182702, 0xff1a2601, 0xff1b2501, 0xff1c2401, 0xff1e2300,
	0xff1f2200, 0x00202000, 0x00211f00, 0x01221d00, 0x01231c00,
	0x01251bff, 0x02251aff, 0x032618ff, 0x032717ff, 0x042815ff,
	0x052814ff, 0x052913ff, 0x06291100, 0x072a10ff, 0x082a0e00,
	0x092a0d00, 0x0a2a0c00,
	/* counter = 6 */
	0xff0d280c, 0xff0e280b, 0xff0f280a, 0xff102809, 0xff112808,
	0xff122708, 0xff142706, 0xff152705, 0xff162605, 0xff172604,
	0xff192503, 0xff1a2403, 0x001b2302, 0x001c2202, 0x001d2201,
	0x001e2101, 0x011f1f01, 0x01211e00, 0x01221d00, 0x02221c00,
	0x02231b00, 0x03241900, 0x04241800, 0x04251700, 0x052616ff,
	0x06261400, 0x072713ff, 0x08271100, 0x08271100, 0x09271000,
	0x0a280e00, 0x0b280d00,
	/* counter = 7 */
	0xff0e260d, 0xff0f260c, 0xff10260b, 0xff11260a, 0xff122609,
	0xff132608, 0xff142508, 0xff152507, 0x00152506, 0x00172405,
	0x00182305, 0x00192304, 0x001b2203, 0x001c2103, 0x011d2002,
	0x011d2002, 0x011f1f01, 0x021f1e01, 0x02201d01, 0x03211c00,
	0x03221b00, 0x04221a00, 0x04231801, 0x05241700, 0x06241600,
	0x07241500, 0x08251300, 0x09251200, 0x09261100, 0x0a261000,
	0x0b260f00, 0x0c260e00,
	/* counter = 8 */
	0xff0e250e, 0xff0f250d, 0xff10250c, 0xff11250b, 0x0011250a,
	0x00132409, 0x00142408, 0x00152407, 0x00162307, 0x00172306,
	0x00182206, 0x00192205, 0x011a2104, 0x011b2004, 0x011c2003,
	0x021c1f03, 0x021e1e02, 0x031e1d02, 0x03201c01, 0x04201b01,
	0x04211a01, 0x05221900, 0x05221801, 0x06231700, 0x07231600,
	0x07241500, 0x08241400, 0x09241300, 0x0a241200, 0x0b241100,
	0x0c241000, 0x0d240f00,
	/* counter = 9 */
	0x000e240e, 0x000f240d, 0x0010240c, 0x0011240b, 0x0013230a,
	0x0013230a, 0x00142309, 0x00152308, 0x00162208, 0x00172207,
	0x01182106, 0x01192105, 0x011a2005, 0x021b1f04, 0x021b1f04,
	0x021d1e03, 0x031d1d03, 0x031e1d02, 0x041e1c02, 0x041f1b02,
	0x05201a01, 0x05211901, 0x06211801, 0x07221700, 0x07221601,
	0x08231500, 0x09231400, 0x0a231300, 0x0a231300, 0x0b231200,
	0x0c231100, 0x0d231000,
	/* counter = 10 */
	0x000f220f, 0x0010220e, 0x0011220d, 0x0012220c, 0x0013220b,
	0x0013220b, 0x0015210a, 0x0015210a, 0x01162108, 0x01172008,
	0x01182007, 0x02191f06, 0x02191f06, 0x021a1e06, 0x031a1e05,
	0x031c1d04, 0x041c1c04, 0x041d1c03, 0x051d1b03, 0x051e1a03,
	0x061f1902, 0x061f1902, 0x07201801, 0x08201701, 0x08211601,
	0x09211501, 0x0a211500, 0x0b211400, 0x0b221300, 0x0c221200,
	0x0d221100, 0x0e221000,
	/* counter = 11 */
	0x0010210f, 0x0011210e, 0x0011210e, 0x0012210d, 0x0013210c,
	0x0014200c, 0x0114200b, 0x0115200a, 0x01161f0a, 0x01171f09,
	0x02171f08, 0x02181e08, 0x03181e07, 0x031a1d06, 0x031a1d06,
	0x041b1c05, 0x041c1c04, 0x051c1b04, 0x051d1a04, 0x061d1a03,
	0x071d1903, 0x071e1803, 0x081e1802, 0x081f1702, 0x091f1602,
	0x0a201501, 0x0b1f1501, 0x0b201401, 0x0c211300, 0x0d211200,
	0x0e201200, 0x0e211100,
	/* counter = 12 */
	0x00102010, 0x0011200f, 0x0012200e, 0x0013200d, 0x0013200d,
	0x01141f0c, 0x01151f0b, 0x01151f0b, 0x01161f0a, 0x02171e09,
	0x02171e09, 0x03181d08, 0x03191d07, 0x03191d07, 0x041a1c06,
	0x041b1c05, 0x051b1b05, 0x051c1b04, 0x061c1a04, 0x071d1903,
	0x071d1903, 0x081d1803, 0x081e1703, 0x091e1702, 0x0a1f1601,
	0x0a1f1502, 0x0b1f1501, 0x0c1f1401, 0x0d201300, 0x0d201300,
	0x0e201200, 0x0f201100,
	/* counter = 13 */
	0x00102010, 0x0011200f, 0x00121f0f, 0x00131f0e, 0x00141f0d,
	0x01141f0c, 0x01141f0c, 0x01151e0c, 0x02161e0a, 0x02171e09,
	0x03171d09, 0x03181d08, 0x03181d08, 0x04191c07, 0x041a1c06,
	0x051a1b06, 0x051b1b05, 0x061b1a05, 0x061c1a04, 0x071c1904,
	0x081c1903, 0x081d1803, 0x091d1703, 0x091e1702, 0x0a1e1602,
	0x0b1e1502, 0x0c1e1501, 0x0c1f1401, 0x0d1f1400, 0x0e1f1300,
	0x0e1f1201, 0x0f1f1200,
	/* counter = 14 */
	0x00111e11, 0x00121e10, 0x00131e0f, 0x00131e0f, 0x01131e0e,
	0x01141d0e, 0x02151d0c, 0x02151d0c, 0x02161d0b, 0x03161c0b,
	0x03171c0a, 0x04171c09, 0x04181b09, 0x05181b08, 0x05191b07,
	0x06191a07, 0x061a1a06, 0x071a1906, 0x071b1905, 0x081b1805,
	0x091b1804, 0x091c1704, 0x0a1c1703, 0x0a1c1604, 0x0b1d1602,
	0x0c1d1502, 0x0c1d1502, 0x0d1d1402, 0x0e1d1401, 0x0e1e1301,
	0x0f1e1300, 0x101e1200,
	/* counter = 15 */
	0x00111e11, 0x00121e10, 0x00131d10, 0x01131d0f, 0x01141d0e,
	0x01141d0e, 0x02151c0d, 0x02151c0d, 0x03161c0b, 0x03161c0b,
	0x04171b0a, 0x04171b0a, 0x05171b09, 0x05181a09, 0x06181a08,
	0x06191a07, 0x07191907, 0x071a1906, 0x081a1806, 0x081a1806,
	0x091a1805, 0x0a1b1704, 0x0a1b1704, 0x0b1c1603, 0x0b1c1603,
	0x0c1c1503, 0x0d1c1502, 0x0d1d1402, 0x0e1d1401, 0x0f1d1301,
	0x0f1d1301, 0x101e1200,
	/* counter = 16 */
};

/* Device limits */
#define G2D_MAX_WIDTH		2048
#define G2D_MAX_HEIGHT		2048
#define G2D_MIN_WIDTH		8
#define G2D_MIN_HEIGHT		8

/* Job queue limits */
#define G2D_MAX_JOBS		64

/* Forward declarations */
struct sunxi_g2d_dev;
struct sunxi_g2d_job;

/**
 * struct sunxi_g2d_dev - G2D device structure
 * 
 * Hardware resources:
 * @dev: Platform device
 * @base: MMIO base address
 * @ccu_base: CCU register base (for clock gating control)
 * @clk_bus: AHB/APB bus clock
 * @clk_mod: G2D module clock
 * @clk_mbus: MBUS clock for memory bandwidth
 * @rst: Reset controller
 * @irq: IRQ number
 * @icc_path: Interconnect path for MBUS bandwidth management
 * 
 * Device node:
 * @cdev: Character device
 * @dev_class: Device class
 * @dev_num: Device number (major:minor)
 * 
 * Power and resource management:
 * @users: Reference counter (open file descriptors)
 * @hw_enabled: Hardware is powered and clocked
 * @dev_mutex: Protects users, hw_enabled, and power state
 * 
 * Job queue and synchronization:
 * @job_lock: Spinlock for job queue
 * @job_queue: List of pending jobs
 * @job_wq: Workqueue for job processing
 * @job_work: Work struct for job processing
 * @current_job: Currently executing job
 * 
 * Statistics:
 * @hw_version: G2D IP version from VERSION register
 * @jobs_done: Total completed jobs
 * @jobs_failed: Total failed jobs
 */
struct sunxi_g2d_dev {
	/* Hardware resources */
	struct device *dev;
	void __iomem *base;
	void __iomem *ccu_base;
	struct clk *clk_bus;
	struct clk *clk_mod;
	struct clk *clk_mbus;
	struct reset_control *rst;
	int irq;
	struct icc_path *icc_path;
	
	/* Device node */
	struct cdev cdev;
	struct class *dev_class;
	dev_t dev_num;
	
	/* Power and resource management */
	atomic_t users;
	bool hw_enabled;
	struct mutex dev_mutex;
	
	/* Job queue */
	spinlock_t job_lock;
	struct list_head job_queue;
	struct workqueue_struct *job_wq;
	struct work_struct job_work;
	struct sunxi_g2d_job *current_job;
	
	/* IRQ synchronization */
	wait_queue_head_t irq_wait;
	atomic_t irq_done;
	
	/* Statistics */
	u32 hw_version;
	atomic64_t jobs_done;
	atomic64_t jobs_failed;
};

/**
 * struct sunxi_g2d_job - G2D job descriptor
 * 
 * @node: List node for job queue
 * @fence: DMA fence for synchronization
 * @fence_fd: File descriptor for fence (returned to userspace)
 * @type: Job type (blit, fillrect, etc)
 * @data: Job-specific data
 * @imported_bufs: List of imported DMA-BUFs (need to release)
 */
enum g2d_job_type {
	G2D_JOB_BLIT,
	G2D_JOB_FILLRECT,
};

struct sunxi_g2d_job {
	struct list_head node;
	struct dma_fence *fence;
	int fence_fd;
	enum g2d_job_type type;
	
	union {
		struct g2d_blit blit;
		struct g2d_fillrect fillrect;
	} data;
	
	/* Imported DMA-BUFs to clean up */
	struct dma_buf *src_dmabuf;
	struct dma_buf_attachment *src_attach;
	struct sg_table *src_sgt;
	
	struct dma_buf *dst_dmabuf;
	struct dma_buf_attachment *dst_attach;
	struct sg_table *dst_sgt;
};

/**
 * struct g2d_dma_buffer - Exported DMA-BUF buffer
 * 
 * @vaddr: CPU virtual address
 * @dma_addr: DMA address for hardware
 * @size: Buffer size in bytes
 * @dev: Device that owns the buffer
 */
struct g2d_dma_buffer {
	void *vaddr;
	dma_addr_t dma_addr;
	size_t size;
	struct device *dev;
};

/* ========== Register access helpers ========== */

static inline void g2d_write(struct sunxi_g2d_dev *g2d, u32 reg, u32 val)
{
	writel(val, g2d->base + reg);
}

static inline u32 g2d_read(struct sunxi_g2d_dev *g2d, u32 reg)
{
	return readl(g2d->base + reg);
}

static inline void g2d_ccu_write(struct sunxi_g2d_dev *g2d, u32 reg, u32 val)
{
	writel(val, g2d->ccu_base + reg);
}

static inline u32 g2d_ccu_read(struct sunxi_g2d_dev *g2d, u32 reg)
{
	return readl(g2d->ccu_base + reg);
}

/* ========== Power management ========== */

/**
 * sunxi_g2d_hw_enable - Enable G2D hardware
 * 
 * Called when users goes 0 -> 1
 * Sequence from fillrect v1.0.0 STABLE:
 * 1. Enable clocks
 * 2. Configure CCU clock gating
 * 3. Set MBUS bandwidth
 * 4. Reset G2D IP
 * 5. Configure RCQ_IRQ_CTL for DIRECT mode
 * 6. Enable interrupts
 * 7. Read and verify VERSION register
 */
static int sunxi_g2d_hw_enable(struct sunxi_g2d_dev *g2d)
{
	u32 val;
	int ret;
	
	dev_info(g2d->dev, "=== G2D hardware enable (fillrect v1.0.0 sequence) ===\n");
	
	/* Step 1: Enable clocks (BSP order: bus → mod@300MHz → mbus) */
	ret = clk_prepare_enable(g2d->clk_bus);
	if (ret) {
		dev_err(g2d->dev, "Failed to enable bus clock: %d\n", ret);
		return ret;
	}
	
	ret = clk_set_rate(g2d->clk_mod, 300000000);
	if (ret) {
		dev_warn(g2d->dev, "Failed to set module clock rate: %d\n", ret);
	}
	
	ret = clk_prepare_enable(g2d->clk_mod);
	if (ret) {
		dev_err(g2d->dev, "Failed to enable module clock: %d\n", ret);
		goto err_bus_clk;
	}
	
	ret = clk_prepare_enable(g2d->clk_mbus);
	if (ret) {
		dev_err(g2d->dev, "Failed to enable mbus clock: %d\n", ret);
		goto err_mod_clk;
	}
	
	dev_info(g2d->dev, "Clocks: mod=%lu bus=%lu mbus=%lu\n",
		 clk_get_rate(g2d->clk_mod),
		 clk_get_rate(g2d->clk_bus),
		 clk_get_rate(g2d->clk_mbus));
	
	/* Step 2: TOP enable - open gates, reset, read VERSION */
	g2d_write(g2d, G2D_SCLK_GATE, 0x3);  /* MIXER + ROT */
	g2d_write(g2d, G2D_HCLK_GATE, 0x3);  /* MIXER + ROT */
	
	/* Reset pulse */
	g2d_write(g2d, G2D_AHB_RESET, 0x0);  /* Assert reset */
	wmb();
	udelay(10);
	g2d_write(g2d, G2D_AHB_RESET, 0x3);  /* Release reset */
	wmb();
	
	/* Configure CMD_CTL for DRAM command control (needed for RCQ DMA) */
	g2d_write(g2d, G2D_CMD_CTL, 0x00010001);  /* Enable CORE0 and RT_WB */
	wmb();
	
	dev_info(g2d->dev, "TOP: SCLK=0x%08x HCLK=0x%08x RESET=0x%08x CMD_CTL=0x%08x\n",
		 g2d_read(g2d, G2D_SCLK_GATE),
		 g2d_read(g2d, G2D_HCLK_GATE),
		 g2d_read(g2d, G2D_AHB_RESET),
		 g2d_read(g2d, G2D_CMD_CTL));
	
	/* Read VERSION */
	val = g2d_read(g2d, G2D_VERSION);
	g2d->hw_version = val;
	dev_info(g2d->dev, "G2D_VERSION=0x%08x (IP version=0x%04x)\n",
		 val, (val >> 16) & 0xFFFF);
	
	/* Step 3: CCU enable - enable WB/RCQ sub-blocks */
	if (g2d->ccu_base) {
		val = readl(g2d->ccu_base + G2D_CLK_REG);
		val |= G2D_CLK_GATING;
		writel(val, g2d->ccu_base + G2D_CLK_REG);
		
		writel(0x00010001, g2d->ccu_base + G2D_BGR_REG);  /* Gate + RST */
		wmb();
		
		dev_info(g2d->dev, "CCU: CLK_REG=0x%08x BGR_REG=0x%08x\n",
			 readl(g2d->ccu_base + G2D_CLK_REG),
			 readl(g2d->ccu_base + G2D_BGR_REG));
	}
	
	/* Step 4: Set MBUS bandwidth (300 MB/s avg, 600 MB/s peak) */
	if (g2d->icc_path) {
		ret = icc_set_bw(g2d->icc_path,
				 Bps_to_icc(300 * 1024 * 1024),
				 Bps_to_icc(600 * 1024 * 1024));
		if (ret)
			dev_warn(g2d->dev, "Failed to set MBUS bandwidth: %d\n", ret);
		else
			dev_info(g2d->dev, "MBUS bandwidth set\n");
	}
	
	/* Step 5: Verify register access */
	dev_info(g2d->dev, "RCQ_CTRL=0x%08x RCQ_STATUS=0x%08x\n",
		 g2d_read(g2d, G2D_RCQ_CTRL),
		 g2d_read(g2d, G2D_RCQ_STATUS));
	
	/* Step 6: Configure interrupts for DIRECT mode */
	g2d_write(g2d, G2D_MIXER_INT, G2D_MIXER_INT_FINISH_IRQ_EN);
	
	g2d->hw_enabled = true;
	
	dev_info(g2d->dev, "=== G2D hardware enabled successfully ===\n");
	return 0;

err_mod_clk:
	clk_disable_unprepare(g2d->clk_mod);
err_bus_clk:
	clk_disable_unprepare(g2d->clk_bus);
	return ret;
}

/**
 * sunxi_g2d_hw_disable - Disable G2D hardware
 * 
 * Called when users goes 1 -> 0
 */
static void sunxi_g2d_hw_disable(struct sunxi_g2d_dev *g2d)
{
	dev_dbg(g2d->dev, "Disabling G2D hardware\n");
	
	if (!g2d->hw_enabled)
		return;
	
	/* Disable interrupts */
	g2d_write(g2d, G2D_MIXER_INT, 0);
	
	/* Reset IP */
	g2d_write(g2d, G2D_MIXER_CTL, G2D_MIXER_CTL_RESET);
	
	/* Release MBUS bandwidth */
	if (g2d->icc_path)
		icc_set_bw(g2d->icc_path, 0, 0);
	
	/* Disable clocks */
	clk_disable_unprepare(g2d->clk_mbus);
	clk_disable_unprepare(g2d->clk_mod);
	clk_disable_unprepare(g2d->clk_bus);
	
	g2d->hw_enabled = false;
	
	dev_info(g2d->dev, "G2D hardware disabled\n");
}

/* ========== IRQ handler ========== */

static irqreturn_t sunxi_g2d_irq(int irq, void *data)
{
	struct sunxi_g2d_dev *g2d = data;
	u32 mixer_status, rot_status;
	bool handled = false;
	
	/* Check MIXER interrupt */
	mixer_status = g2d_read(g2d, G2D_MIXER_INT);
	if (mixer_status & G2D_MIXER_INT_IRQ_PENDING) {
		/* Clear MIXER interrupt */
		g2d_write(g2d, G2D_MIXER_INT, G2D_MIXER_INT_IRQ_PENDING);
		dev_dbg(g2d->dev, "MIXER IRQ: status=0x%08x\n", mixer_status);
		
		/* Signal completion */
		atomic_set(&g2d->irq_done, 1);
		wake_up(&g2d->irq_wait);
		handled = true;
	}
	
	/* Check ROT interrupt */
	rot_status = g2d_read(g2d, ROT_INT);
	if (rot_status & ROT_INT_FINISH) {
		/* Clear ROT interrupt */
		g2d_write(g2d, ROT_INT, ROT_INT_FINISH);
		dev_info(g2d->dev, "ROT IRQ received: status=0x%08x\n", rot_status);
		
		/* Signal completion */
		atomic_set(&g2d->irq_done, 1);
		wake_up(&g2d->irq_wait);
		handled = true;
	}
	
	if (!handled)
		return IRQ_NONE;
	
	/* Update statistics */
	if (g2d->current_job) {
		/* TODO: Signal fence, mark job done, schedule next */
		atomic64_inc(&g2d->jobs_done);
	}
	
	return IRQ_HANDLED;
}

/* ========== G2D Operations ========== */

/**
 * sunxi_g2d_do_fillrect - Execute fillrect operation (DIRECT mode)
 * 
 * Based on fillrect v1.0.0 STABLE proven sequence
 */
static int sunxi_g2d_do_fillrect(struct sunxi_g2d_dev *g2d,
				  dma_addr_t dst_dma,
				  u32 width, u32 height,
				  u32 pitch, u32 color)
{
	unsigned long timeout;
	u32 mixer_ctl;
	
	dev_dbg(g2d->dev, "FILLRECT: %ux%u pitch=%u color=0x%08x dma=0x%llx\n",
		width, height, pitch, color, (u64)dst_dma);
	
	/* 1. Reset G2D */
	g2d_write(g2d, G2D_AHB_RESET, 0x0);
	g2d_write(g2d, G2D_AHB_RESET, 0x3);
	wmb();
	
	/* 2. Setup V0 layer - fillcolor mode */
	g2d_write(g2d, V0_ATTCTL, 0xff000011);  /* fillcolor_en=1, ARGB8888, EN=1, alpha=0xff */
	g2d_write(g2d, V0_MBSIZE, ((height - 1) << 16) | (width - 1));
	g2d_write(g2d, V0_SIZE, ((height - 1) << 16) | (width - 1));
	g2d_write(g2d, V0_COOR, 0x00000000);
	g2d_write(g2d, V0_PITCH0, pitch);
	g2d_write(g2d, V0_FILLC, color);
	
	/* 3. Setup BLD (blender) */
	g2d_write(g2d, BLD_EN_CTL, g2d_read(g2d, BLD_EN_CTL) | 0x00000100);  /* Enable pipe 0 */
	g2d_write(g2d, BLD_PREMUL_CTL, 0x00000000);
	g2d_write(g2d, BLD_CH_ISIZE0, ((height - 1) << 16) | (width - 1));
	g2d_write(g2d, BLD_CH_OFFSET0, 0x00000000);
	g2d_write(g2d, BLD_OUT_SIZE, ((height - 1) << 16) | (width - 1));
	g2d_write(g2d, BLD_OUT_COLOR, g2d_read(g2d, BLD_OUT_COLOR) & ~BIT(1));  /* RGB mode */
	g2d_write(g2d, BLD_CTL, 0x00000000);
	
	/* 4. Setup ROP */
	g2d_write(g2d, ROP_CTL, 0x000000f0);
	g2d_write(g2d, ROP_INDEX0, 0x00061080);
	
	/* 5. Setup WB (writeback) */
	g2d_write(g2d, WB_LADD0, lower_32_bits(dst_dma));
	g2d_write(g2d, WB_HADD0, upper_32_bits(dst_dma));
	g2d_write(g2d, WB_PITCH0, pitch);
	g2d_write(g2d, WB_SIZE, ((height - 1) << 16) | (width - 1));
	g2d_write(g2d, BLD_SIZE, ((height - 1) << 16) | (width - 1));  /* After WB config */
	g2d_write(g2d, WB_ATT, 0x00000000);  /* ARGB8888 */
	wmb();
	
	/* 7. Clear MIXER and IRQ */
	g2d_write(g2d, G2D_MIXER_INT, 0x00000000);
	g2d_write(g2d, G2D_MIXER_CTL, 0x00000000);
	wmb();
	
	/* 8. Clear IRQ done flag and enable IRQ */
	atomic_set(&g2d->irq_done, 0);
	g2d_write(g2d, G2D_MIXER_INT, 0x00000011);  /* Clear pending, enable IRQ */
	wmb();
	
	/* 9. Start MIXER */
	mixer_ctl = g2d_read(g2d, G2D_MIXER_CTL);
	mixer_ctl |= 0x80000000;  /* START bit */
	g2d_write(g2d, G2D_MIXER_CTL, mixer_ctl);
	wmb();
	
	/* 10. Wait for IRQ completion */
	timeout = wait_event_timeout(g2d->irq_wait,
				      atomic_read(&g2d->irq_done) == 1,
				      msecs_to_jiffies(1000));
	
	if (timeout == 0) {
		u32 status = g2d_read(g2d, G2D_MIXER_INT);
		dev_err(g2d->dev, "FILLRECT timeout (no IRQ), status=0x%08x\n", status);
		return -ETIMEDOUT;
	}
	
	dev_dbg(g2d->dev, "FILLRECT completed via IRQ\n");
	return 0;
}

/* ========== File operations ========== */

static int sunxi_g2d_open(struct inode *inode, struct file *file)
{
	struct sunxi_g2d_dev *g2d = container_of(inode->i_cdev,
						  struct sunxi_g2d_dev, cdev);
	int ret = 0;
	
	dev_dbg(g2d->dev, "Device opened\n");
	
	file->private_data = g2d;
	
	/* Increment user count and enable hardware if first user */
	mutex_lock(&g2d->dev_mutex);
	
	if (atomic_inc_return(&g2d->users) == 1) {
		ret = sunxi_g2d_hw_enable(g2d);
		if (ret) {
			atomic_dec(&g2d->users);
			mutex_unlock(&g2d->dev_mutex);
			return ret;
		}
	}
	
	mutex_unlock(&g2d->dev_mutex);
	
	return 0;
}

static int sunxi_g2d_release(struct inode *inode, struct file *file)
{
	struct sunxi_g2d_dev *g2d = file->private_data;
	
	dev_dbg(g2d->dev, "Device released\n");
	
	/* Decrement user count and disable hardware if last user */
	mutex_lock(&g2d->dev_mutex);
	
	if (atomic_dec_return(&g2d->users) == 0)
		sunxi_g2d_hw_disable(g2d);
	
	mutex_unlock(&g2d->dev_mutex);
	
	return 0;
}

/* ========== DMA-BUF export operations ========== */

static int g2d_dmabuf_attach(struct dma_buf *dmabuf,
			      struct dma_buf_attachment *attach)
{
	return 0;
}

static void g2d_dmabuf_detach(struct dma_buf *dmabuf,
			       struct dma_buf_attachment *attach)
{
}

static struct sg_table *g2d_dmabuf_map(struct dma_buf_attachment *attach,
					enum dma_data_direction dir)
{
	struct g2d_dma_buffer *buf = attach->dmabuf->priv;
	struct sg_table *sgt;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = dma_get_sgtable(buf->dev, sgt, buf->vaddr, buf->dma_addr, buf->size);
	if (ret < 0) {
		kfree(sgt);
		return ERR_PTR(ret);
	}

	return sgt;
}

static void g2d_dmabuf_unmap(struct dma_buf_attachment *attach,
			      struct sg_table *sgt,
			      enum dma_data_direction dir)
{
	sg_free_table(sgt);
	kfree(sgt);
}

static void g2d_dmabuf_release(struct dma_buf *dmabuf)
{
	struct g2d_dma_buffer *buf = dmabuf->priv;

	dma_free_coherent(buf->dev, buf->size, buf->vaddr, buf->dma_addr);
	kfree(buf);
}

static int g2d_dmabuf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct g2d_dma_buffer *buf = dmabuf->priv;
	int ret;

	pr_info("g2d_dmabuf_mmap: size=%zu vma_size=%lu\n", 
		buf->size, vma->vm_end - vma->vm_start);

	/* Let DMA framework handle the mmap */
	ret = dma_mmap_attrs(buf->dev, vma, buf->vaddr, buf->dma_addr, 
			      buf->size, DMA_ATTR_WRITE_COMBINE);
	
	pr_info("g2d_dmabuf_mmap: ret=%d\n", ret);
	return ret;
}

static int g2d_dmabuf_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct g2d_dma_buffer *buf = dmabuf->priv;

	iosys_map_set_vaddr(map, buf->vaddr);
	return 0;
}

static const struct dma_buf_ops g2d_dmabuf_ops = {
	.attach = g2d_dmabuf_attach,
	.detach = g2d_dmabuf_detach,
	.map_dma_buf = g2d_dmabuf_map,
	.unmap_dma_buf = g2d_dmabuf_unmap,
	.release = g2d_dmabuf_release,
	.mmap = g2d_dmabuf_mmap,
	.vmap = g2d_dmabuf_vmap,
};

/* ========== IOCTL handlers ========== */

static long sunxi_g2d_ioctl_get_version(struct sunxi_g2d_dev *g2d,
					 unsigned long arg)
{
	struct g2d_version ver = {
		.hw_version = g2d->hw_version,
		.driver_major = DRIVER_MAJOR,
		.driver_minor = DRIVER_MINOR,
		.driver_patchlevel = DRIVER_PATCHLEVEL,
	};
	
	if (copy_to_user((void __user *)arg, &ver, sizeof(ver)))
		return -EFAULT;
	
	return 0;
}

/**
 * g2d_vsu_calc_fir_coef - Calculate FIR coefficient offset
 * @step: Scale step value (fixed-point)
 *
 * Returns the offset (in words) into the coefficient table based on the
 * scaling ratio. Used to select appropriate filter coefficients for the
 * current scale factor.
 */
static u32 g2d_vsu_calc_fir_coef(u32 step)
{
	u32 pt_coef;
	u32 scale_ratio, int_part, float_part, fir_coef_ofst;

	scale_ratio = step >> (VSU_PHASE_FRAC_BITWIDTH - 3);
	int_part = scale_ratio >> 3;
	float_part = scale_ratio & 0x7;
	
	fir_coef_ofst = (int_part == 0) ? VSU_ZOOM0_SIZE :
	    (int_part == 1) ? VSU_ZOOM0_SIZE + float_part :
	    (int_part == 2) ? VSU_ZOOM0_SIZE + VSU_ZOOM1_SIZE + (float_part >> 1) :
	    (int_part == 3) ? VSU_ZOOM0_SIZE + VSU_ZOOM1_SIZE + VSU_ZOOM2_SIZE :
	    (int_part == 4) ? VSU_ZOOM0_SIZE + VSU_ZOOM1_SIZE + VSU_ZOOM2_SIZE + VSU_ZOOM3_SIZE :
	    VSU_ZOOM0_SIZE + VSU_ZOOM1_SIZE + VSU_ZOOM2_SIZE + VSU_ZOOM3_SIZE + VSU_ZOOM4_SIZE;
	
	pt_coef = fir_coef_ofst * VSU_PHASE_NUM;
	return pt_coef;
}

/**
 * sunxi_g2d_vsu_setup - Configure Video Scaler Unit for scaling operation
 * @g2d: G2D device
 * @fmt: Pixel format (G2D_FMT_*)
 * @in_w: Input width
 * @in_h: Input height
 * @out_w: Output width
 * @out_h: Output height
 * @alpha: Global alpha value (0-255)
 *
 * Configures the VSU block for scaling. For RGB formats, uses bilinear
 * filtering. For YUV formats (future), uses Lanczos filtering with proper
 * chroma handling.
 *
 * Returns 0 on success, negative error code on failure.
 */
static int sunxi_g2d_vsu_setup(struct sunxi_g2d_dev *g2d, u32 fmt,
			       u32 in_w, u32 in_h, u32 out_w, u32 out_h, u8 alpha)
{
	u64 temp;
	u32 yhstep, yvstep;
	u32 yhcoef_offset, yvcoef_offset;
	u32 format;
	int i;

	dev_dbg(g2d->dev, "VSU: Setup scaling %ux%u -> %ux%u, alpha=%u\n",
		in_w, in_h, out_w, out_h, alpha);

	/* Enable VSU with coefficient access */
	g2d_write(g2d, VS_CTRL, VS_CTRL_EN | VS_CTRL_COEF_ACCESS_SEL);

	/* Determine format type - for now only RGB */
	switch (fmt) {
	case G2D_FMT_ARGB8888:
	case G2D_FMT_XRGB8888:
	case G2D_FMT_ABGR8888:
	case G2D_FMT_XBGR8888:
	case G2D_FMT_RGB565:
		format = VSU_FORMAT_RGB;
		break;
	default:
		dev_err(g2d->dev, "VSU: Unsupported format %u\n", fmt);
		return -EINVAL;
	}

	/* Set output size */
	g2d_write(g2d, VS_OUT_SIZE, ((out_w - 1) & 0x1FFF) | (((out_h - 1) & 0x1FFF) << 16));
	g2d_write(g2d, VS_GLB_ALPHA, alpha);

	/* Set Y channel (luma/RGB) input size */
	g2d_write(g2d, VS_Y_SIZE, ((in_w - 1) & 0x1FFF) | (((in_h - 1) & 0x1FFF) << 16));

	/* Calculate horizontal step (input_width / output_width) in fixed-point */
	temp = (u64)in_w << VSU_PHASE_FRAC_BITWIDTH;
	if (out_w)
		do_div(temp, out_w);
	else
		temp = 0;
	yhstep = (u32)temp;
	g2d_write(g2d, VS_Y_HSTEP, yhstep << 1);

	/* Calculate vertical step (input_height / output_height) in fixed-point */
	temp = (u64)in_h << VSU_PHASE_FRAC_BITWIDTH;
	if (out_h)
		do_div(temp, out_h);
	else
		temp = 0;
	yvstep = (u32)temp;
	g2d_write(g2d, VS_Y_VSTEP, yvstep << 1);

	/* Calculate coefficient offsets based on scale factors */
	yhcoef_offset = g2d_vsu_calc_fir_coef(yhstep);
	yvcoef_offset = g2d_vsu_calc_fir_coef(yvstep);

	/* Load horizontal coefficients (Lanczos2) */
	for (i = 0; i < VSU_PHASE_NUM; i++) {
		g2d_write(g2d, VS_Y_HCOEF0 + i * 4,
			  lan2coefftab32_full[yhcoef_offset + i]);
	}

	/* For RGB: use linear (bilinear) vertical filtering */
	for (i = 0; i < VSU_PHASE_NUM; i++) {
		g2d_write(g2d, VS_Y_VCOEF0 + i * 4, linearcoefftab32[i]);
	}

	/* Set phases to 0 for RGB */
	g2d_write(g2d, VS_Y_HPHASE, 0);
	g2d_write(g2d, VS_Y_VPHASE0, 0);

	/* For RGB format, C channel mirrors Y channel settings */
	g2d_write(g2d, VS_C_SIZE, ((in_w - 1) & 0x1FFF) | (((in_h - 1) & 0x1FFF) << 16));
	g2d_write(g2d, VS_C_HSTEP, yhstep << 1);
	g2d_write(g2d, VS_C_VSTEP, yvstep << 1);
	
	/* Load chroma horizontal coefficients (same as luma for RGB) */
	for (i = 0; i < VSU_PHASE_NUM; i++) {
		g2d_write(g2d, VS_C_HCOEF0 + i * 4,
			  lan2coefftab32_full[yhcoef_offset + i]);
	}

	g2d_write(g2d, VS_C_HPHASE, 0);
	g2d_write(g2d, VS_C_VPHASE0, 0);

	dev_dbg(g2d->dev, "VSU: yhstep=0x%08x yvstep=0x%08x format=%u\n",
		yhstep, yvstep, format);

	return 0;
}

/*
 * sunxi_g2d_do_blit_alpha - Perform alpha blending operation
 *
 * Uses two UI layers (UI0=background, UI1=foreground) and BLD for blending.
 * This is the "Porter-Duff over" operation: dst = src*alpha + dst*(1-alpha)
 */
static int sunxi_g2d_do_blit_alpha(struct sunxi_g2d_dev *g2d,
				    dma_addr_t src_dma_addr, u32 src_w, u32 src_h,
				    u32 src_pitch, u32 src_format,
				    u32 src_x, u32 src_y, u32 src_crop_w, u32 src_crop_h,
				    dma_addr_t dst_dma_addr, u32 dst_w, u32 dst_h,
				    u32 dst_pitch, u32 dst_format,
				    u32 dst_x, u32 dst_y, u32 blend_w, u32 blend_h,
				    u32 global_alpha)
{
	u32 src_fmt_val, dst_fmt_val, ui_src_attr, ui_dst_attr;
	u32 bld_ctl;
	unsigned long timeout;
	int ret;

	dev_info(g2d->dev, "BLIT_ALPHA: src=%ux%u@(%u,%u) dst=%ux%u@(%u,%u) blend=%ux%u alpha=%u\n",
		 src_crop_w, src_crop_h, src_x, src_y,
		 dst_w, dst_h, dst_x, dst_y,
		 blend_w, blend_h, global_alpha);

	/* Enable hardware */
	ret = sunxi_g2d_hw_enable(g2d);
	if (ret)
		return ret;

	/* Map format to hardware value */
	switch (src_format) {
	case G2D_FMT_ARGB8888:
		src_fmt_val = 0x0;
		break;
	case G2D_FMT_XRGB8888:
		src_fmt_val = 0x4;
		break;
	case G2D_FMT_RGB565:
		src_fmt_val = 0xA;
		break;
	default:
		dev_err(g2d->dev, "Unsupported source format: %u\n", src_format);
		return -EINVAL;
	}

	switch (dst_format) {
	case G2D_FMT_ARGB8888:
		dst_fmt_val = 0x0;
		break;
	case G2D_FMT_XRGB8888:
		dst_fmt_val = 0x4;
		break;
	case G2D_FMT_RGB565:
		dst_fmt_val = 0xA;
		break;
	default:
		dev_err(g2d->dev, "Unsupported destination format: %u\n", dst_format);
		return -EINVAL;
	}

	/* Reset IRQ flag */
	atomic_set(&g2d->irq_done, 0);

	/* Clear and enable interrupts */
	g2d_write(g2d, G2D_MIXER_INT, 0x00000000);
	udelay(1);
	g2d_write(g2d, G2D_MIXER_INT, 0x00000011);  /* Clear pending, enable IRQ */

	/* === Configure MIXER === */
	g2d_write(g2d, MIXER_FILLCOLOR0, 0xFF000000);
	g2d_write(g2d, MIXER_SIZE, ((blend_w - 1) & 0x1FFF) | (((blend_h - 1) & 0x1FFF) << 16));

	dev_info(g2d->dev, "MIXER: fillcolor=0xFF000000 size=%ux%u\n", blend_w, blend_h);

	/* === Configure V0 (background/destination) === */
	/* CRITICAL: Must use V0 (Video layer) for PIPE0, not UI1!
	 * The blender's PIPE0 expects to read from V0 for proper alpha blending.
	 * Using UI1 causes the background to not participate in the blend operation.
	 * This matches the BSP implementation in g2d_mixer.c:g2d_bsp_bld()
	 */
	ui_dst_attr = V0_ATTCTL_EN |
		      (1 << 1) |  /* Alpha mode = 1 (layer global alpha) */
		      (dst_fmt_val << 8) |  /* Format at bits [13:8] */
		      (0xFF << 24);  /* Full alpha for background at bits [31:24] */
	
	g2d_write(g2d, V0_ATTCTL, ui_dst_attr);
	g2d_write(g2d, V0_MBSIZE, ((dst_w - 1) & 0x1FFF) | (((dst_h - 1) & 0x1FFF) << 16));
	g2d_write(g2d, V0_COOR, (dst_x & 0x1FFF) | ((dst_y & 0x1FFF) << 16));
	g2d_write(g2d, V0_PITCH0, dst_pitch & 0xFFFFF);
	g2d_write(g2d, V0_PITCH1, 0);  /* Not used for RGB */
	g2d_write(g2d, V0_PITCH2, 0);  /* Not used for RGB */
	g2d_write(g2d, V0_LADD0, (u32)dst_dma_addr);
	g2d_write(g2d, V0_LADD1, 0);  /* Not used for RGB */
	g2d_write(g2d, V0_LADD2, 0);  /* Not used for RGB */
	g2d_write(g2d, V0_HADD, (u32)((u64)dst_dma_addr >> 32));
	g2d_write(g2d, V0_SIZE, ((blend_w - 1) & 0x1FFF) | (((blend_h - 1) & 0x1FFF) << 16));

	dev_info(g2d->dev, "V0: attr=0x%08X addr=0x%llx size=%ux%u pitch=%u\n",
		 ui_dst_attr, (u64)dst_dma_addr, dst_w, dst_h, dst_pitch);

	/* === Configure UI2 (foreground/source) === */
	/* UI2 is UI layer 2, maps to PIPE1 of the BLD (same as BSP) */
	/* Global alpha controls the opacity of the foreground layer */
	ui_src_attr = UI_ATTR_EN |
		      (1 << UI_ATTR_ALPHA_MODE_SHIFT) |  /* Use layer global alpha mode */
		      (src_fmt_val << UI_ATTR_FMT_SHIFT) |
		      (global_alpha << UI_ATTR_ALPHA_SHIFT);
	
	g2d_write(g2d, UI2_ATTR, ui_src_attr);
	g2d_write(g2d, UI2_MBSIZE, ((src_w - 1) & 0x1FFF) | (((src_h - 1) & 0x1FFF) << 16));
	g2d_write(g2d, UI2_COOR, (src_x & 0x1FFF) | ((src_y & 0x1FFF) << 16));
	g2d_write(g2d, UI2_PITCH, src_pitch & 0xFFFFF);
	g2d_write(g2d, UI2_LADD, (u32)src_dma_addr);
	g2d_write(g2d, UI2_HADD, (u32)((u64)src_dma_addr >> 32));
	g2d_write(g2d, UI2_SIZE, ((src_crop_w - 1) & 0x1FFF) | (((src_crop_h - 1) & 0x1FFF) << 16));

	dev_info(g2d->dev, "UI2: attr=0x%08X addr=0x%llx size=%ux%u pitch=%u\n",
		 ui_src_attr, (u64)src_dma_addr, src_w, src_h, src_pitch);

	/* === Configure BLD (Blender) === */
	/* Enable both pipes and configure to read from layers (not fill color)
	 * Bit 0: p0_fcen = 0 (pipe0 reads from UI1 layer)
	 * Bit 1: p1_fcen = 0 (pipe1 reads from UI2 layer)  
	 * Bit 8: p0_en = 1 (enable pipe0)
	 * Bit 9: p1_en = 1 (enable pipe1)
	 */
	g2d_write(g2d, BLD_EN_CTL, 0x00000300);  /* p0_en=1, p1_en=1, both fcen=0 */
	
	/* Set input sizes */
	g2d_write(g2d, BLD_CH_ISIZE0, ((blend_w - 1) & 0x1FFF) | (((blend_h - 1) & 0x1FFF) << 16));
	g2d_write(g2d, BLD_CH_ISIZE1, ((blend_w - 1) & 0x1FFF) | (((blend_h - 1) & 0x1FFF) << 16));
	
	/* Set offsets (both at 0,0 for simple overlay) */
	g2d_write(g2d, BLD_CH_OFFSET0, 0);
	g2d_write(g2d, BLD_CH_OFFSET1, 0);
	
	/* Set output size */
	g2d_write(g2d, BLD_SIZE, ((blend_w - 1) & 0x1FFF) | (((blend_h - 1) & 0x1FFF) << 16));
	
	/* Configure blending control using BSP SRCOVER mode (source over destination)
	 * Value 0x03010301 from BSP porter_duff() function for G2D_BLD_SRCOVER
	 * This is the standard Porter-Duff "source over" alpha blend mode
	 */
	bld_ctl = 0x03010301;
	
	g2d_write(g2d, BLD_CTL, bld_ctl);
	
	/* No premultiplication for now */
	g2d_write(g2d, BLD_PREMUL_CTL, 0);
	
	/* Configure ROP (Raster Operation) - BSP uses 0xf0 for blending
	 * This is the standard "copy source" ROP (S = source)
	 * Also sets ch3_index0 to 0x41000 as BSP does
	 */
	g2d_write(g2d, ROP_CTL, 0xf0);
	g2d_write(g2d, ROP_INDEX0, 0x41000);
	
	/* Configure output color format 
	 * Bit 0 (premul_en): 0 = no premultiplication (our images are not premultiplied)
	 * Bit 1 (alpha_mode): 0 = RGB mode, 1 = YUV mode
	 */
	g2d_write(g2d, BLD_OUT_COLOR, 0x00000000);  /* RGB mode, no premul */

	dev_info(g2d->dev, "BLD: en_ctl=0x%08X ctl=0x%08X size=%ux%u\n",
		 BLD_PIPE0_EN | BLD_PIPE1_EN, bld_ctl, blend_w, blend_h);

	/* === Configure Writeback (output to destination) === */
	g2d_write(g2d, WB_ATT, dst_fmt_val);  /* Format only, no enable bit (BSP does this) */
	g2d_write(g2d, WB_SIZE, ((blend_w - 1) & 0x1FFF) | (((blend_h - 1) & 0x1FFF) << 16));
	g2d_write(g2d, WB_PITCH0, dst_pitch & 0xFFFFF);
	g2d_write(g2d, WB_LADD0, (u32)dst_dma_addr);
	g2d_write(g2d, WB_HADD0, (u32)((u64)dst_dma_addr >> 32));

	dev_info(g2d->dev, "WB: att=0x%08X addr=0x%llx size=%ux%u pitch=%u\n",
		 dst_fmt_val, (u64)dst_dma_addr, blend_w, blend_h, dst_pitch);

	/* === Start operation === */
	g2d_write(g2d, CMD_CTL, CMD_CTL_START);

	dev_dbg(g2d->dev, "MIXER started for alpha blending\n");

	/* Wait for completion */
	timeout = wait_event_timeout(g2d->irq_wait, atomic_read(&g2d->irq_done),
				      msecs_to_jiffies(100));

	dev_dbg(g2d->dev, "MIXER wait returned: timeout=%lu irq_done=%d\n",
		timeout, atomic_read(&g2d->irq_done));

	if (timeout == 0) {
		dev_err(g2d->dev, "Alpha blend timeout\n");
		sunxi_g2d_hw_disable(g2d);
		return -ETIMEDOUT;
	}

	if (!atomic_read(&g2d->irq_done)) {
		dev_err(g2d->dev, "Alpha blend failed (no IRQ)\n");
		sunxi_g2d_hw_disable(g2d);
		return -EIO;
	}

	dev_dbg(g2d->dev, "BLIT_ALPHA completed via IRQ\n");
	sunxi_g2d_hw_disable(g2d);
	return 0;
}

/**
 * sunxi_g2d_do_blit - Execute blit operation (copy/scale image)
 * 
 * @g2d: G2D device
 * @src_dma: Source DMA address
 * @src_width: Source width in pixels
 * @src_height: Source height in pixels
 * @src_pitch: Source pitch in bytes
 * @src_format: Source pixel format (enum g2d_pixel_format)
 * @src_x: Source crop X offset
 * @src_y: Source crop Y offset
 * @src_crop_w: Source crop width
 * @src_crop_h: Source crop height
 * @dst_dma: Destination DMA address
 * @dst_width: Destination buffer width in pixels
 * @dst_height: Destination buffer height in pixels
 * @dst_pitch: Destination pitch in bytes
 * @dst_format: Destination pixel format
 * @dst_x: Destination X position
 * @dst_y: Destination Y position
 * @dst_w: Destination blit width (for scaling)
 * @dst_h: Destination blit height (for scaling)
 * 
 * This performs a scaled/unscaled copy from source to destination.
 * Uses V0 layer for source image and WB for destination.
 */
static int sunxi_g2d_do_blit(struct sunxi_g2d_dev *g2d,
			      dma_addr_t src_dma, u32 src_width, u32 src_height,
			      u32 src_pitch, u32 src_format,
			      u32 src_x, u32 src_y, u32 src_crop_w, u32 src_crop_h,
			      dma_addr_t dst_dma, u32 dst_width, u32 dst_height,
			      u32 dst_pitch, u32 dst_format,
			      u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h)
{
	unsigned long timeout;
	u32 mixer_ctl;
	u32 v0_attctl = 0;
	dma_addr_t src_offset_dma;
	
	dev_dbg(g2d->dev, "BLIT: src=%ux%u@0x%llx crop=%u,%u,%ux%u -> dst=%u,%u,%ux%u@0x%llx\n",
		src_width, src_height, (u64)src_dma, src_x, src_y, src_crop_w, src_crop_h,
		dst_x, dst_y, dst_w, dst_h, (u64)dst_dma);
	
	/* Calculate source offset for crop */
	u32 src_bpp;
	switch (src_format) {
	case G2D_FMT_ARGB8888:
	case G2D_FMT_XRGB8888:
	case G2D_FMT_ABGR8888:
	case G2D_FMT_XBGR8888:
		src_bpp = 4;
		v0_attctl = 0x00; /* ARGB8888 format */
		break;
	case G2D_FMT_RGB565:
		src_bpp = 2;
		v0_attctl = 0x0a; /* RGB565 format */
		break;
	default:
		dev_err(g2d->dev, "Unsupported source format: %u\n", src_format);
		return -EINVAL;
	}
	
	src_offset_dma = src_dma + (src_y * src_pitch) + (src_x * src_bpp);
	
	/* 1. Reset G2D */
	g2d_write(g2d, G2D_AHB_RESET, 0x0);
	g2d_write(g2d, G2D_AHB_RESET, 0x3);
	wmb();
	
	/* 2. Setup V0 layer - image mode (NOT fillcolor) */
	v0_attctl |= 0xff000001;  /* alpha=0xff, fillcolor_en=0, EN=1 */
	g2d_write(g2d, V0_ATTCTL, v0_attctl);
	
	/* V0 memory block size = source crop size */
	g2d_write(g2d, V0_MBSIZE, ((src_crop_h - 1) << 16) | (src_crop_w - 1));
	
	/* V0 image size = source crop size */
	g2d_write(g2d, V0_SIZE, ((src_crop_h - 1) << 16) | (src_crop_w - 1));
	
	/* V0 coordinate in blender (0,0 since we handle offset in WB) */
	g2d_write(g2d, V0_COOR, 0x00000000);
	
	/* V0 source pitch and address */
	g2d_write(g2d, V0_PITCH0, src_pitch);
	g2d_write(g2d, V0_LADD0, lower_32_bits(src_offset_dma));
	g2d_write(g2d, V0_HADD, upper_32_bits(src_offset_dma));
	
	/* V0 fillcolor not used in image mode */
	g2d_write(g2d, V0_FILLC, 0x00000000);
	
	/* 3. Setup BLD (blender) - output size = destination blit size */
	g2d_write(g2d, BLD_EN_CTL, g2d_read(g2d, BLD_EN_CTL) | 0x00000100);  /* Enable pipe 0 */
	g2d_write(g2d, BLD_PREMUL_CTL, 0x00000000);
	
	/* BLD channel 0 input size = destination blit size (scaled if needed) */
	g2d_write(g2d, BLD_CH_ISIZE0, ((dst_h - 1) << 16) | (dst_w - 1));
	g2d_write(g2d, BLD_CH_OFFSET0, 0x00000000);
	
	/* BLD output size = destination blit size */
	g2d_write(g2d, BLD_OUT_SIZE, ((dst_h - 1) << 16) | (dst_w - 1));
	g2d_write(g2d, BLD_OUT_COLOR, g2d_read(g2d, BLD_OUT_COLOR) & ~BIT(1));  /* RGB mode */
	g2d_write(g2d, BLD_CTL, 0x00000000);
	
	/* 4. Setup ROP (copy mode) */
	g2d_write(g2d, ROP_CTL, 0x000000f0);  /* ROP3: 0xF0 = S (source copy) */
	g2d_write(g2d, ROP_INDEX0, 0x00061080);
	
	/* 5. Setup WB (writeback) with destination offset */
	u32 dst_bpp;
	u32 wb_att = 0;
	switch (dst_format) {
	case G2D_FMT_ARGB8888:
	case G2D_FMT_XRGB8888:
	case G2D_FMT_ABGR8888:
	case G2D_FMT_XBGR8888:
		dst_bpp = 4;
		wb_att = 0x00; /* ARGB8888 */
		break;
	case G2D_FMT_RGB565:
		dst_bpp = 2;
		wb_att = 0x0a; /* RGB565 */
		break;
	default:
		dev_err(g2d->dev, "Unsupported destination format: %u\n", dst_format);
		return -EINVAL;
	}
	
	/* Calculate destination address with offset */
	dma_addr_t dst_offset_dma = dst_dma + (dst_y * dst_pitch) + (dst_x * dst_bpp);
	
	g2d_write(g2d, WB_LADD0, lower_32_bits(dst_offset_dma));
	g2d_write(g2d, WB_HADD0, upper_32_bits(dst_offset_dma));
	g2d_write(g2d, WB_PITCH0, dst_pitch);
	
	/* WB size = destination blit size */
	g2d_write(g2d, WB_SIZE, ((dst_h - 1) << 16) | (dst_w - 1));
	g2d_write(g2d, BLD_SIZE, ((dst_h - 1) << 16) | (dst_w - 1));  /* Must match WB_SIZE */
	g2d_write(g2d, WB_ATT, wb_att);
	wmb();
	
	/* 6. Setup VSU (Video Scaler Unit) if scaling is needed */
	bool needs_scaling = (src_crop_w != dst_w) || (src_crop_h != dst_h);
	if (needs_scaling) {
		int ret;
		
		dev_info(g2d->dev, "VSU: Enabling scaler for %ux%u -> %ux%u\n",
			 src_crop_w, src_crop_h, dst_w, dst_h);
		
		ret = sunxi_g2d_vsu_setup(g2d, src_format, src_crop_w, src_crop_h,
					  dst_w, dst_h, 0xff);
		if (ret) {
			dev_err(g2d->dev, "VSU setup failed: %d\n", ret);
			return ret;
		}
	} else {
		/* Disable VSU for 1:1 copy */
		g2d_write(g2d, VS_CTRL, 0);
	}
	
	/* 7. Clear MIXER and IRQ */
	g2d_write(g2d, G2D_MIXER_INT, 0x00000000);
	g2d_write(g2d, G2D_MIXER_CTL, 0x00000000);
	wmb();
	
	/* 7. Clear IRQ done flag and enable IRQ */
	atomic_set(&g2d->irq_done, 0);
	g2d_write(g2d, G2D_MIXER_INT, 0x00000011);  /* Clear pending, enable IRQ */
	wmb();
	
	/* 8. Start MIXER */
	mixer_ctl = g2d_read(g2d, G2D_MIXER_CTL);
	mixer_ctl |= 0x80000000;  /* START bit */
	g2d_write(g2d, G2D_MIXER_CTL, mixer_ctl);
	wmb();
	
	/* 9. Wait for IRQ completion */
	timeout = wait_event_timeout(g2d->irq_wait,
				      atomic_read(&g2d->irq_done) == 1,
				      msecs_to_jiffies(1000));
	
	if (timeout == 0) {
		u32 status = g2d_read(g2d, G2D_MIXER_INT);
		dev_err(g2d->dev, "BLIT timeout (no IRQ), status=0x%08x\n", status);
		return -ETIMEDOUT;
	}
	
	dev_dbg(g2d->dev, "BLIT completed via IRQ\n");
	return 0;
}

/**
 * sunxi_g2d_do_blit_rot - Execute blit with rotation/flip using ROT block
 * 
 * This function uses the ROT (Rotator) hardware block for:
 * - Rotation: 90°, 180°, 270°
 * - Flip: horizontal, vertical
 * 
 * Note: In G2D v2.0, rotation and scaling cannot be used simultaneously.
 * This function does 1:1 copy with rotation/flip only.
 * 
 * Based on BSP g2d_rotate_set_para() implementation.
 */
static int sunxi_g2d_do_blit_rot(struct sunxi_g2d_dev *g2d,
				  dma_addr_t src_dma, u32 src_width, u32 src_height,
				  u32 src_pitch, u32 src_format,
				  u32 src_x, u32 src_y, u32 src_crop_w, u32 src_crop_h,
				  dma_addr_t dst_dma, u32 dst_width, u32 dst_height,
				  u32 dst_pitch, u32 dst_format,
				  u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h,
				  u32 flags)
{
	unsigned long timeout;
	u32 rot_ctl = ROT_CTL_EN;
	u32 rot_fmt = 0;
	dma_addr_t src_offset_dma, dst_offset_dma;
	u32 src_bpp, dst_bpp;
	u32 out_w, out_h;
	bool rotated_90_270;
	
	/* Determine output dimensions based on rotation */
	rotated_90_270 = (flags & (G2D_BLIT_FLAG_ROTATE_90 | G2D_BLIT_FLAG_ROTATE_270));
	if (rotated_90_270) {
		/* 90° and 270° rotations swap width and height */
		out_w = src_crop_h;
		out_h = src_crop_w;
	} else {
		/* 0°, 180°, and flips keep dimensions */
		out_w = src_crop_w;
		out_h = src_crop_h;
	}
	
	dev_info(g2d->dev, "BLIT_ROT: src=%ux%u crop=%u,%u,%ux%u -> dst=%ux%u@(%u,%u) flags=0x%x\n",
		 src_width, src_height, src_x, src_y, src_crop_w, src_crop_h,
		 dst_width, dst_height, dst_x, dst_y, flags);
	
	/* Reset ROT block */
	g2d_write(g2d, ROT_CTL, 0x0);
	g2d_write(g2d, ROT_INT, 0x0);
	g2d_write(g2d, ROT_TIMEOUT, 0xFFFF);
	
	/* Disable MIXER to avoid interference */
	g2d_write(g2d, G2D_MIXER_CTL, 0x00000000);
	g2d_write(g2d, G2D_MIXER_INT, 0x00000000);
	wmb();
	
	/* Map format to ROT format (based on BSP) */
	switch (src_format) {
	case G2D_FMT_ARGB8888:
	case G2D_FMT_XRGB8888:
	case G2D_FMT_ABGR8888:
	case G2D_FMT_XBGR8888:
		src_bpp = 4;
		rot_fmt = 0x00;  /* G2D_FORMAT_ARGB8888 */
		break;
	case G2D_FMT_RGB565:
		src_bpp = 2;
		rot_fmt = 0x0a;  /* G2D_FORMAT_RGB565 */
		break;
	default:
		dev_err(g2d->dev, "Unsupported ROT source format: %u\n", src_format);
		return -EINVAL;
	}
	
	/* Destination format should match source */
	switch (dst_format) {
	case G2D_FMT_ARGB8888:
	case G2D_FMT_XRGB8888:
	case G2D_FMT_ABGR8888:
	case G2D_FMT_XBGR8888:
		dst_bpp = 4;
		break;
	case G2D_FMT_RGB565:
		dst_bpp = 2;
		break;
	default:
		dev_err(g2d->dev, "Unsupported ROT destination format: %u\n", dst_format);
		return -EINVAL;
	}
	
	/* Apply rotation flags (ROT_CTL bits 4-5) */
	if (flags & G2D_BLIT_FLAG_ROTATE_90)
		rot_ctl |= ROT_CTL_ROT_90;
	else if (flags & G2D_BLIT_FLAG_ROTATE_180)
		rot_ctl |= ROT_CTL_ROT_180;
	else if (flags & G2D_BLIT_FLAG_ROTATE_270)
		rot_ctl |= ROT_CTL_ROT_270;
	
	/* Apply flip flags (ROT_CTL bits 6-7) */
	if (flags & G2D_BLIT_FLAG_FLIP_V)
		rot_ctl |= ROT_CTL_FLIP_V;
	if (flags & G2D_BLIT_FLAG_FLIP_H)
		rot_ctl |= ROT_CTL_FLIP_H;
	
	/* Calculate source address with offset */
	src_offset_dma = src_dma + (src_y * src_pitch) + (src_x * src_bpp);
	
	/* Calculate destination address with offset */
	dst_offset_dma = dst_dma + (dst_y * dst_pitch) + (dst_x * dst_bpp);
	
	/* Configure ROT input (source) - BSP format: (height-1 << 16) | width-1 */
	g2d_write(g2d, ROT_IFMT, rot_fmt);
	g2d_write(g2d, ROT_ISIZE, ((src_crop_h - 1) << 16) | (src_crop_w - 1));
	g2d_write(g2d, ROT_IPITCH0, src_pitch);
	g2d_write(g2d, ROT_IPITCH1, 0);  /* No U/V planes for RGB */
	g2d_write(g2d, ROT_IPITCH2, 0);
	g2d_write(g2d, ROT_ILADD0, lower_32_bits(src_offset_dma));
	g2d_write(g2d, ROT_IHADD0, upper_32_bits(src_offset_dma));
	g2d_write(g2d, ROT_ILADD1, 0);
	g2d_write(g2d, ROT_IHADD1, 0);
	g2d_write(g2d, ROT_ILADD2, 0);
	g2d_write(g2d, ROT_IHADD2, 0);
	
	/* Configure ROT output (destination) - BSP format: (height-1 << 16) | width-1 
	 * NOTE: ROT has no OFMT register - output format is same as input
	 * Output dimensions are adjusted for rotation (swapped for 90°/270°) */
	g2d_write(g2d, ROT_OSIZE, ((out_h - 1) << 16) | (out_w - 1));
	g2d_write(g2d, ROT_OPITCH0, dst_pitch);
	g2d_write(g2d, ROT_OPITCH1, 0);
	g2d_write(g2d, ROT_OPITCH2, 0);
	g2d_write(g2d, ROT_OLADD0, lower_32_bits(dst_offset_dma));
	g2d_write(g2d, ROT_OHADD0, upper_32_bits(dst_offset_dma));
	g2d_write(g2d, ROT_OLADD1, 0);
	g2d_write(g2d, ROT_OHADD1, 0);
	g2d_write(g2d, ROT_OLADD2, 0);
	g2d_write(g2d, ROT_OHADD2, 0);
	wmb();
	
	dev_info(g2d->dev, "ROT config: FMT=0x%x CTL=0x%x (flags=0x%x)\n", rot_fmt, rot_ctl, flags);
	dev_info(g2d->dev, "ROT sizes: in=%ux%u out=%ux%u (rotated=%d)\n",
		 src_crop_w, src_crop_h, out_w, out_h, rotated_90_270);
	dev_info(g2d->dev, "ROT addresses: src=0x%llx dst=0x%llx\n",
		 (u64)src_offset_dma, (u64)dst_offset_dma);
	
	/* Clear and enable ROT interrupt */
	g2d_write(g2d, ROT_INT, ROT_INT_FINISH_EN);  /* Enable finish interrupt */
	atomic_set(&g2d->irq_done, 0);
	
	/* Write ROT_CTL with enable bit (BSP: write once at the beginning) */
	g2d_write(g2d, ROT_CTL, rot_ctl);
	wmb();
	
	/* Start ROT by setting bit 31 (BSP: read-modify-write) */
	rot_ctl = g2d_read(g2d, ROT_CTL);
	rot_ctl |= (1 << 31);  /* START bit */
	g2d_write(g2d, ROT_CTL, rot_ctl);
	wmb();
	
	dev_info(g2d->dev, "ROT started: CTL=0x%08x\n", rot_ctl);
	
	dev_info(g2d->dev, "ROT started: src=%ux%u -> dst=%ux%u\n",
		 src_crop_w, src_crop_h, dst_w, dst_h);
	
	/* Wait for completion */
	dev_info(g2d->dev, "ROT waiting for IRQ... irq_done=%d\n", atomic_read(&g2d->irq_done));
	timeout = wait_event_timeout(g2d->irq_wait,
				      atomic_read(&g2d->irq_done) == 1,
				      msecs_to_jiffies(1000));
	
	dev_info(g2d->dev, "ROT wait returned: timeout=%lu irq_done=%d\n",
		 timeout, atomic_read(&g2d->irq_done));
	
	if (timeout == 0) {
		u32 status = g2d_read(g2d, ROT_INT);
		dev_err(g2d->dev, "BLIT_ROT timeout (no IRQ), status=0x%08x\n", status);
		return -ETIMEDOUT;
	}
	
	dev_dbg(g2d->dev, "BLIT_ROT completed via IRQ\n");
	return 0;
}

static long sunxi_g2d_ioctl_blit(struct sunxi_g2d_dev *g2d, unsigned long arg)
{
	struct g2d_blit blit;
	struct dma_buf *src_dmabuf = NULL, *dst_dmabuf = NULL;
	struct dma_buf_attachment *src_attach = NULL, *dst_attach = NULL;
	struct sg_table *src_sgt = NULL, *dst_sgt = NULL;
	dma_addr_t src_dma_addr, dst_dma_addr;
	u32 src_bpp, dst_bpp;
	u32 src_pitch, dst_pitch;
	u32 src_crop_w, src_crop_h;
	int ret;
	
	if (copy_from_user(&blit, (void __user *)arg, sizeof(blit)))
		return -EFAULT;
	
	/* Validate parameters */
	if (blit.src.width == 0 || blit.src.height == 0 ||
	    blit.dst.width == 0 || blit.dst.height == 0 ||
	    blit.dst_w == 0 || blit.dst_h == 0) {
		dev_err(g2d->dev, "Invalid dimensions\n");
		return -EINVAL;
	}
	
	if (blit.src.width > G2D_MAX_WIDTH || blit.src.height > G2D_MAX_HEIGHT ||
	    blit.dst.width > G2D_MAX_WIDTH || blit.dst.height > G2D_MAX_HEIGHT ||
	    blit.dst_w > G2D_MAX_WIDTH || blit.dst_h > G2D_MAX_HEIGHT) {
		dev_err(g2d->dev, "Dimensions exceed maximum %dx%d\n",
			G2D_MAX_WIDTH, G2D_MAX_HEIGHT);
		return -EINVAL;
	}
	
	/* For now, only support DMA-BUF mode */
	if (blit.src.dma_fd < 0 || blit.dst.dma_fd < 0) {
		dev_err(g2d->dev, "Physical address mode not supported yet\n");
		return -EINVAL;
	}
	
	/* Validate alpha value if blending enabled */
	if ((blit.flags & G2D_BLIT_FLAG_ALPHA_BLEND) && blit.global_alpha > 255) {
		dev_err(g2d->dev, "Invalid global_alpha value: %u (must be 0-255)\n",
			blit.global_alpha);
		return -EINVAL;
	}
	
	dev_info(g2d->dev, "BLIT: src=%ux%u fmt=%u -> dst=%u,%u,%ux%u fmt=%u flags=0x%x alpha=%u\n",
		 blit.src.width, blit.src.height, blit.src.format,
		 blit.dst_x, blit.dst_y, blit.dst_w, blit.dst_h, blit.dst.format,
		 blit.flags, blit.global_alpha);
	
	/* Import source DMA-BUF */
	src_dmabuf = dma_buf_get(blit.src.dma_fd);
	if (IS_ERR(src_dmabuf)) {
		ret = PTR_ERR(src_dmabuf);
		dev_err(g2d->dev, "Failed to get source dma_buf: %d\n", ret);
		return ret;
	}
	
	src_attach = dma_buf_attach(src_dmabuf, g2d->dev);
	if (IS_ERR(src_attach)) {
		ret = PTR_ERR(src_attach);
		dev_err(g2d->dev, "Failed to attach source dma_buf: %d\n", ret);
		goto err_put_src_dmabuf;
	}
	
	src_sgt = dma_buf_map_attachment(src_attach, DMA_TO_DEVICE);
	if (IS_ERR(src_sgt)) {
		ret = PTR_ERR(src_sgt);
		dev_err(g2d->dev, "Failed to map source dma_buf: %d\n", ret);
		goto err_detach_src;
	}
	
	src_dma_addr = sg_dma_address(src_sgt->sgl);
	
	/* Import destination DMA-BUF */
	dst_dmabuf = dma_buf_get(blit.dst.dma_fd);
	if (IS_ERR(dst_dmabuf)) {
		ret = PTR_ERR(dst_dmabuf);
		dev_err(g2d->dev, "Failed to get destination dma_buf: %d\n", ret);
		goto err_unmap_src;
	}
	
	dst_attach = dma_buf_attach(dst_dmabuf, g2d->dev);
	if (IS_ERR(dst_attach)) {
		ret = PTR_ERR(dst_attach);
		dev_err(g2d->dev, "Failed to attach destination dma_buf: %d\n", ret);
		goto err_put_dst_dmabuf;
	}
	
	dst_sgt = dma_buf_map_attachment(dst_attach, DMA_FROM_DEVICE);
	if (IS_ERR(dst_sgt)) {
		ret = PTR_ERR(dst_sgt);
		dev_err(g2d->dev, "Failed to map destination dma_buf: %d\n", ret);
		goto err_detach_dst;
	}
	
	dst_dma_addr = sg_dma_address(dst_sgt->sgl);
	
	/* Calculate bytes per pixel for source */
	switch (blit.src.format) {
	case G2D_FMT_ARGB8888:
	case G2D_FMT_XRGB8888:
	case G2D_FMT_ABGR8888:
	case G2D_FMT_XBGR8888:
		src_bpp = 4;
		break;
	case G2D_FMT_RGB565:
		src_bpp = 2;
		break;
	default:
		dev_err(g2d->dev, "Unsupported source format: %u\n", blit.src.format);
		ret = -EINVAL;
		goto err_unmap_dst;
	}
	
	/* Calculate bytes per pixel for destination */
	switch (blit.dst.format) {
	case G2D_FMT_ARGB8888:
	case G2D_FMT_XRGB8888:
	case G2D_FMT_ABGR8888:
	case G2D_FMT_XBGR8888:
		dst_bpp = 4;
		break;
	case G2D_FMT_RGB565:
		dst_bpp = 2;
		break;
	default:
		dev_err(g2d->dev, "Unsupported destination format: %u\n", blit.dst.format);
		ret = -EINVAL;
		goto err_unmap_dst;
	}
	
	/* Calculate pitch (stride) */
	src_pitch = blit.src.stride[0] ? blit.src.stride[0] : (blit.src.width * src_bpp);
	dst_pitch = blit.dst.stride[0] ? blit.dst.stride[0] : (blit.dst.width * dst_bpp);
	
	/* Calculate crop region (default to full source if not specified) */
	src_crop_w = blit.src.crop_w ? blit.src.crop_w : blit.src.width;
	src_crop_h = blit.src.crop_h ? blit.src.crop_h : blit.src.height;
	
	/* Validate crop region */
	if (blit.src.crop_x + src_crop_w > blit.src.width ||
	    blit.src.crop_y + src_crop_h > blit.src.height) {
		dev_err(g2d->dev, "Invalid crop region\n");
		ret = -EINVAL;
		goto err_unmap_dst;
	}
	
	/* Validate destination position */
	if (blit.dst_x + blit.dst_w > blit.dst.width ||
	    blit.dst_y + blit.dst_h > blit.dst.height) {
		dev_err(g2d->dev, "Invalid destination position\n");
		ret = -EINVAL;
		goto err_unmap_dst;
	}
	
	/* Choose execution path based on flags:
	 * - Alpha blending: Use MIXER with BLD (two UI layers)
	 * - Rotation/flip: Use ROT block (no scaling/blending)
	 * - Scaling only: Use MIXER with VSU
	 * - Simple copy: Use MIXER (fastest path)
	 * Note: G2D v2.0 cannot combine rotation with scaling or blending */
	bool needs_alpha = !!(blit.flags & G2D_BLIT_FLAG_ALPHA_BLEND);
	bool needs_rotation = !!(blit.flags & (G2D_BLIT_FLAG_ROTATE_90 |
					       G2D_BLIT_FLAG_ROTATE_180 |
					       G2D_BLIT_FLAG_ROTATE_270 |
					       G2D_BLIT_FLAG_FLIP_H |
					       G2D_BLIT_FLAG_FLIP_V));
	bool needs_scaling = (blit.dst_w != src_crop_w) || (blit.dst_h != src_crop_h);
	
	/* Check for incompatible combinations */
	if (needs_rotation && needs_scaling) {
		dev_err(g2d->dev, "Cannot do rotation and scaling simultaneously in G2D v2.0\n");
		ret = -EINVAL;
		goto err_unmap_dst;
	}
	
	if (needs_rotation && needs_alpha) {
		dev_err(g2d->dev, "Cannot do rotation and alpha blending simultaneously\n");
		ret = -EINVAL;
		goto err_unmap_dst;
	}
	
	if (needs_alpha && needs_scaling) {
		dev_err(g2d->dev, "Cannot do alpha blending and scaling simultaneously\n");
		ret = -EINVAL;
		goto err_unmap_dst;
	}
	
	if (needs_alpha) {
		/* Use MIXER with BLD for alpha blending (two UI layers) */
		dev_info(g2d->dev, "Using MIXER+BLD for alpha blending %ux%u alpha=%u\n",
			 src_crop_w, src_crop_h, blit.global_alpha);
		ret = sunxi_g2d_do_blit_alpha(g2d,
					       src_dma_addr, blit.src.width, blit.src.height,
					       src_pitch, blit.src.format,
					       blit.src.crop_x, blit.src.crop_y, src_crop_w, src_crop_h,
					       dst_dma_addr, blit.dst.width, blit.dst.height,
					       dst_pitch, blit.dst.format,
					       blit.dst_x, blit.dst_y, blit.dst_w, blit.dst_h,
					       blit.global_alpha);
	} else if (needs_rotation) {
		/* Use ROT block for rotation/flip (1:1 copy only) */
		dev_info(g2d->dev, "Using ROT for rotation/flip %ux%u flags=0x%x\n",
			 src_crop_w, src_crop_h, blit.flags);
		ret = sunxi_g2d_do_blit_rot(g2d,
					     src_dma_addr, blit.src.width, blit.src.height,
					     src_pitch, blit.src.format,
					     blit.src.crop_x, blit.src.crop_y, src_crop_w, src_crop_h,
					     dst_dma_addr, blit.dst.width, blit.dst.height,
					     dst_pitch, blit.dst.format,
					     blit.dst_x, blit.dst_y, blit.dst_w, blit.dst_h,
					     blit.flags);
	} else {
		/* Use MIXER (+VSU for scaling) for copy/scale */
		if (needs_scaling) {
			dev_info(g2d->dev, "Using MIXER+VSU for scaled blit %ux%u -> %ux%u\n",
				 src_crop_w, src_crop_h, blit.dst_w, blit.dst_h);
		} else {
			dev_dbg(g2d->dev, "Using MIXER for 1:1 blit %ux%u\n", src_crop_w, src_crop_h);
		}
		ret = sunxi_g2d_do_blit(g2d,
					 src_dma_addr, blit.src.width, blit.src.height,
					 src_pitch, blit.src.format,
					 blit.src.crop_x, blit.src.crop_y, src_crop_w, src_crop_h,
					 dst_dma_addr, blit.dst.width, blit.dst.height,
					 dst_pitch, blit.dst.format,
					 blit.dst_x, blit.dst_y, blit.dst_w, blit.dst_h);
	}
	
	if (ret < 0)
		goto err_unmap_dst;
	
	/* TODO: Create and return fence_fd if ASYNC flag set */
	blit.fence_fd_out = -1;
	
	if (copy_to_user((void __user *)arg, &blit, sizeof(blit))) {
		ret = -EFAULT;
		goto err_unmap_dst;
	}
	
err_unmap_dst:
	dma_buf_unmap_attachment(dst_attach, dst_sgt, DMA_FROM_DEVICE);
err_detach_dst:
	dma_buf_detach(dst_dmabuf, dst_attach);
err_put_dst_dmabuf:
	dma_buf_put(dst_dmabuf);
err_unmap_src:
	dma_buf_unmap_attachment(src_attach, src_sgt, DMA_TO_DEVICE);
err_detach_src:
	dma_buf_detach(src_dmabuf, src_attach);
err_put_src_dmabuf:
	dma_buf_put(src_dmabuf);
	
	return ret;
}

static long sunxi_g2d_ioctl_fillrect(struct sunxi_g2d_dev *g2d,
				      unsigned long arg)
{
	struct g2d_fillrect fill;
	struct dma_buf *dmabuf = NULL;
	struct dma_buf_attachment *attach = NULL;
	struct sg_table *sgt = NULL;
	dma_addr_t dma_addr;
	u32 width, height, pitch;
	int ret;
	
	if (copy_from_user(&fill, (void __user *)arg, sizeof(fill)))
		return -EFAULT;
	
	/* Validate parameters */
	if (fill.dst_w == 0 || fill.dst_h == 0 ||
	    fill.dst_w > G2D_MAX_WIDTH || fill.dst_h > G2D_MAX_HEIGHT) {
		dev_err(g2d->dev, "Invalid dimensions: %ux%u\n", fill.dst_w, fill.dst_h);
		return -EINVAL;
	}
	
	/* For now, only support DMA-BUF (dma_fd >= 0) */
	if (fill.dst.dma_fd < 0) {
		dev_err(g2d->dev, "Physical address mode not supported yet\n");
		return -EINVAL;
	}
	
	dev_info(g2d->dev, "FILLRECT: %ux%u at (%u,%u) color=0x%08x dma_fd=%d\n",
		 fill.dst_w, fill.dst_h, fill.dst_x, fill.dst_y, fill.color, fill.dst.dma_fd);
	
	/* Import DMA-BUF */
	dmabuf = dma_buf_get(fill.dst.dma_fd);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		dev_err(g2d->dev, "Failed to get dma_buf: %d\n", ret);
		return ret;
	}
	
	attach = dma_buf_attach(dmabuf, g2d->dev);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		dev_err(g2d->dev, "Failed to attach dma_buf: %d\n", ret);
		goto err_put_dmabuf;
	}
	
	sgt = dma_buf_map_attachment(attach, DMA_FROM_DEVICE);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		dev_err(g2d->dev, "Failed to map dma_buf: %d\n", ret);
		goto err_detach;
	}
	
	/* Get DMA address from first sg entry */
	dma_addr = sg_dma_address(sgt->sgl);
	
	/* Calculate dimensions and pitch */
	width = fill.dst_w;
	height = fill.dst_h;
	
	/* Calculate bytes per pixel based on format */
	u32 bpp;
	switch (fill.dst.format) {
	case G2D_FMT_ARGB8888:
	case G2D_FMT_XRGB8888:
		bpp = 4;
		break;
	case G2D_FMT_RGB565:
		bpp = 2;
		break;
	default:
		dev_err(g2d->dev, "Unsupported format: %u\n", fill.dst.format);
		ret = -EINVAL;
		goto err_unmap;
	}
	
	pitch = fill.dst.stride[0] ? fill.dst.stride[0] : (width * bpp);
	
	/* Adjust DMA address for dst_x/dst_y offset */
	dma_addr += (fill.dst_y * pitch) + (fill.dst_x * bpp);
	
	/* Execute fillrect operation */
	ret = sunxi_g2d_do_fillrect(g2d, dma_addr, width, height, pitch, fill.color);
	
	/* TODO: Create and return fence_fd if ASYNC flag set */
	fill.fence_fd_out = -1;
	
	if (copy_to_user((void __user *)arg, &fill, sizeof(fill))) {
		ret = -EFAULT;
		goto err_unmap;
	}
	
err_unmap:
	dma_buf_unmap_attachment(attach, sgt, DMA_FROM_DEVICE);
err_detach:
	dma_buf_detach(dmabuf, attach);
err_put_dmabuf:
	dma_buf_put(dmabuf);
	
	return ret;
}

static long sunxi_g2d_ioctl_alloc_buffer(struct sunxi_g2d_dev *g2d,
					  unsigned long arg)
{
	struct g2d_alloc_buffer alloc;
	struct g2d_dma_buffer *buf;
	struct dma_buf *dmabuf;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	int fd;

	if (copy_from_user(&alloc, (void __user *)arg, sizeof(alloc)))
		return -EFAULT;

	/* Validate size */
	if (alloc.size == 0 || alloc.size > 128 * 1024 * 1024) /* Max 128 MB */
		return -EINVAL;

	/* Allocate buffer metadata */
	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* Allocate DMA coherent memory */
	buf->vaddr = dma_alloc_coherent(g2d->dev, alloc.size, &buf->dma_addr, GFP_KERNEL);
	if (!buf->vaddr) {
		kfree(buf);
		return -ENOMEM;
	}

	buf->size = alloc.size;
	buf->dev = g2d->dev;

	/* Export as DMA-BUF */
	exp_info.ops = &g2d_dmabuf_ops;
	exp_info.size = alloc.size;
	exp_info.flags = O_RDWR | O_CLOEXEC;
	exp_info.priv = buf;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		dma_free_coherent(g2d->dev, buf->size, buf->vaddr, buf->dma_addr);
		kfree(buf);
		return PTR_ERR(dmabuf);
	}

	/* Get file descriptor */
	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0) {
		dma_buf_put(dmabuf);
		return fd;
	}

	alloc.dma_fd = fd;

	if (copy_to_user((void __user *)arg, &alloc, sizeof(alloc))) {
		/* Close fd on error */
		dma_buf_put(dmabuf);
		return -EFAULT;
	}

	dev_dbg(g2d->dev, "Allocated buffer: size=%llu fd=%d dma_addr=0x%llx\n",
		alloc.size, fd, (u64)buf->dma_addr);

	return 0;
}

static long sunxi_g2d_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct sunxi_g2d_dev *g2d = file->private_data;
	
	switch (cmd) {
	case G2D_IOC_GET_VERSION:
		return sunxi_g2d_ioctl_get_version(g2d, arg);
	case G2D_IOC_BLIT:
		return sunxi_g2d_ioctl_blit(g2d, arg);
	case G2D_IOC_FILLRECT:
		return sunxi_g2d_ioctl_fillrect(g2d, arg);
	/* TODO: Implement buffer allocation
	case G2D_IOC_ALLOC_BUFFER:
		return sunxi_g2d_ioctl_alloc_buffer(g2d, arg);
	*/
	case G2D_IOC_SYNC:
		/* TODO: Implement fence wait */
		return -ENOSYS;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations sunxi_g2d_fops = {
	.owner		= THIS_MODULE,
	.open		= sunxi_g2d_open,
	.release	= sunxi_g2d_release,
	.unlocked_ioctl	= sunxi_g2d_ioctl,
	.compat_ioctl	= sunxi_g2d_ioctl,
};

/* ========== Platform driver ========== */

static int sunxi_g2d_probe(struct platform_device *pdev)
{
	struct sunxi_g2d_dev *g2d;
	struct resource *res;
	int ret;
	
	dev_info(&pdev->dev, "Probing G2D driver v%s\n", DRIVER_VERSION);
	
	g2d = devm_kzalloc(&pdev->dev, sizeof(*g2d), GFP_KERNEL);
	if (!g2d)
		return -ENOMEM;
	
	g2d->dev = &pdev->dev;
	platform_set_drvdata(pdev, g2d);
	
	/* Get MMIO resources */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	g2d->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(g2d->base))
		return PTR_ERR(g2d->base);
	
	dev_info(&pdev->dev, "G2D MMIO base=0x%08llx size=0x%llx\n",
		 (u64)res->start, (u64)resource_size(res));
	
	/* Get CCU (clock control unit) base - use hardcoded address as in fillrect v1.0.0 */
	g2d->ccu_base = devm_ioremap(&pdev->dev, 0x02001000, 0x1000);
	if (!g2d->ccu_base) {
		dev_err(&pdev->dev, "Failed to map CCU registers\n");
		return -ENOMEM;
	}
	
	dev_info(&pdev->dev, "CCU base=0x02001000 (hardcoded)\n");
	
	/* BSP sequence: reset FIRST, then clocks (critical from fillrect v1.0.0) */
	g2d->rst = devm_reset_control_get_optional_exclusive(&pdev->dev, NULL);
	if (!IS_ERR(g2d->rst)) {
		ret = reset_control_deassert(g2d->rst);
		if (ret) {
			dev_err(&pdev->dev, "Failed to deassert reset: %d\n", ret);
			return ret;
		}
		dev_info(&pdev->dev, "Reset deasserted\n");
	} else {
		dev_warn(&pdev->dev, "No reset control available\n");
	}
	
	/* Configure DMA (from fillrect v1.0.0) */
	ret = of_dma_configure(&pdev->dev, pdev->dev.of_node, true);
	if (ret)
		dev_warn(&pdev->dev, "of_dma_configure failed: %d\n", ret);
	
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "Failed to set DMA mask: %d\n", ret);
		return ret;
	}
	
	/* Get clocks */
	g2d->clk_bus = devm_clk_get(&pdev->dev, "bus_g2d");
	if (IS_ERR(g2d->clk_bus)) {
		dev_err(&pdev->dev, "Failed to get bus_g2d clock\n");
		return PTR_ERR(g2d->clk_bus);
	}
	
	g2d->clk_mod = devm_clk_get(&pdev->dev, "g2d");
	if (IS_ERR(g2d->clk_mod)) {
		dev_err(&pdev->dev, "Failed to get g2d clock\n");
		return PTR_ERR(g2d->clk_mod);
	}
	
	g2d->clk_mbus = devm_clk_get(&pdev->dev, "mbus_g2d");
	if (IS_ERR(g2d->clk_mbus)) {
		dev_err(&pdev->dev, "Failed to get mbus clock\n");
		return PTR_ERR(g2d->clk_mbus);
	}
	
	/* Get interconnect path for MBUS */
	g2d->icc_path = devm_of_icc_get(&pdev->dev, "dma-mem");
	if (IS_ERR(g2d->icc_path)) {
		ret = PTR_ERR(g2d->icc_path);
		if (ret != -ENODEV) {
			dev_err(&pdev->dev, "Failed to get interconnect: %d\n", ret);
			return ret;
		}
		g2d->icc_path = NULL;
	}
	
	/* Get IRQ */
	g2d->irq = platform_get_irq(pdev, 0);
	if (g2d->irq < 0)
		return g2d->irq;
	
	ret = devm_request_irq(&pdev->dev, g2d->irq, sunxi_g2d_irq,
			       0, dev_name(&pdev->dev), g2d);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request IRQ %d: %d\n",
			g2d->irq, ret);
		return ret;
	}
	
	dev_info(&pdev->dev, "IRQ %d registered\n", g2d->irq);
	
	/* Initialize synchronization primitives */
	mutex_init(&g2d->dev_mutex);
	spin_lock_init(&g2d->job_lock);
	INIT_LIST_HEAD(&g2d->job_queue);
	init_waitqueue_head(&g2d->irq_wait);
	atomic_set(&g2d->irq_done, 0);
	atomic_set(&g2d->users, 0);
	atomic64_set(&g2d->jobs_done, 0);
	atomic64_set(&g2d->jobs_failed, 0);
	
	/* Create workqueue for job processing */
	g2d->job_wq = alloc_workqueue("sunxi-g2d", WQ_HIGHPRI | WQ_UNBOUND, 1);
	if (!g2d->job_wq) {
		dev_err(&pdev->dev, "Failed to create workqueue\n");
		return -ENOMEM;
	}
	
	/* Register character device */
	ret = alloc_chrdev_region(&g2d->dev_num, 0, 1, DRIVER_NAME);
	if (ret) {
		dev_err(&pdev->dev, "Failed to allocate device number: %d\n", ret);
		goto err_destroy_wq;
	}
	
	cdev_init(&g2d->cdev, &sunxi_g2d_fops);
	g2d->cdev.owner = THIS_MODULE;
	
	ret = cdev_add(&g2d->cdev, g2d->dev_num, 1);
	if (ret) {
		dev_err(&pdev->dev, "Failed to add cdev: %d\n", ret);
		goto err_unregister_chrdev;
	}
	
	g2d->dev_class = class_create(DRIVER_NAME);
	if (IS_ERR(g2d->dev_class)) {
		ret = PTR_ERR(g2d->dev_class);
		dev_err(&pdev->dev, "Failed to create device class: %d\n", ret);
		goto err_del_cdev;
	}
	
	device_create(g2d->dev_class, &pdev->dev, g2d->dev_num, NULL, "g2d");
	
	dev_info(&pdev->dev, "Character device /dev/g2d created\n");
	dev_info(&pdev->dev, "G2D driver v%s loaded successfully\n", DRIVER_VERSION);
	
#if 0  /* Internal fillrect test - enable to verify hardware */
	/* Simple fillrect test (like fillrect v1.0.0) */
	{
		void *dst = NULL;
		dma_addr_t dst_dma;
		const size_t surface_bytes = 64 * 64 * 4;
		volatile u32 *test_buf;
		
		dst = dmam_alloc_coherent(&pdev->dev, surface_bytes, &dst_dma, GFP_KERNEL);
		if (dst) {
			test_buf = (volatile u32 *)dst;
			memset((void *)dst, 0xEF, surface_bytes);
			test_buf[0] = 0xDEADBEEF;
			test_buf[10] = 0xDEADBEEF;
			
			dev_info(&pdev->dev, "\n=== FILLRECT TEST ===\n");
			dev_info(&pdev->dev, "Initial: [0]=0x%08x [10]=0x%08x\n",
				 test_buf[0], test_buf[10]);
			
			/* Enable hardware temporarily */
			ret = sunxi_g2d_hw_enable(g2d);
			if (ret == 0) {
				ret = sunxi_g2d_do_fillrect(g2d, dst_dma, 64, 64, 64*4, 0xFF00FF00);
				if (ret == 0) {
					dev_info(&pdev->dev, "After fillrect: [0]=0x%08x [10]=0x%08x [100]=0x%08x\n",
						 test_buf[0], test_buf[10], test_buf[100]);
					
					if (test_buf[0] == 0xFF00FF00 && test_buf[10] == 0xFF00FF00) {
						dev_info(&pdev->dev, "✅ FILLRECT TEST PASSED!\n");
					} else {
						dev_err(&pdev->dev, "❌ FILLRECT TEST FAILED!\n");
					}
				} else {
					dev_err(&pdev->dev, "fillrect operation failed: %d\n", ret);
				}
				sunxi_g2d_hw_disable(g2d);
			}
			dev_info(&pdev->dev, "=====================\n\n");
		}
	}
#endif
	
	return 0;
	
err_del_cdev:
	cdev_del(&g2d->cdev);
err_unregister_chrdev:
	unregister_chrdev_region(g2d->dev_num, 1);
err_destroy_wq:
	destroy_workqueue(g2d->job_wq);
	return ret;
}

static void sunxi_g2d_remove(struct platform_device *pdev)
{
	struct sunxi_g2d_dev *g2d = platform_get_drvdata(pdev);
	
	dev_info(&pdev->dev, "Removing G2D driver\n");
	
	/* Remove device node */
	device_destroy(g2d->dev_class, g2d->dev_num);
	class_destroy(g2d->dev_class);
	cdev_del(&g2d->cdev);
	unregister_chrdev_region(g2d->dev_num, 1);
	
	/* Flush and destroy workqueue */
	flush_workqueue(g2d->job_wq);
	destroy_workqueue(g2d->job_wq);
	
	/* Disable hardware if still enabled */
	mutex_lock(&g2d->dev_mutex);
	if (g2d->hw_enabled)
		sunxi_g2d_hw_disable(g2d);
	mutex_unlock(&g2d->dev_mutex);
	
	dev_info(&pdev->dev, "G2D driver removed\n");
}

static const struct of_device_id sunxi_g2d_of_match[] = {
	{ .compatible = "allwinner,sun20i-d1-g2d" },
	{ .compatible = "allwinner,t113-g2d" },
	{ .compatible = "allwinner,sun8i-g2d" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sunxi_g2d_of_match);

static struct platform_driver sunxi_g2d_driver = {
	.probe	= sunxi_g2d_probe,
	.remove	= sunxi_g2d_remove,
	.driver	= {
		.name		= DRIVER_NAME,
		.of_match_table	= sunxi_g2d_of_match,
	},
};

module_platform_driver(sunxi_g2d_driver);

MODULE_AUTHOR("Sergio Perez");
MODULE_DESCRIPTION("Allwinner G2D Hardware Accelerator");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("DMA_BUF");
