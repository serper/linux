// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Allwinner G2D Hardware Accelerator Driver
 * 
 * Copyright (C) 2025 Sergio Perez
 *
 * Version: v2.9.48 - STABLE VSU SCALING WORKING
 * 
 * CRITICAL FIX: T113-S3 VSU Hardware Quirk
 * ==========================================
 * T113-S3 G2D requires << 2 shift for VS_*_HSTEP/VSTEP registers
 * Standard Allwinner BSP uses << 1 (for A33/H3/other SoCs)
 * 
 * Experimental evidence (T113-S3):
 *   << 0: Only 1/8 of image visible (4x overscan)
 *   << 1: Only 1/4 of image visible (2x overscan) - BSP value
 *   << 2: Perfect full image - CORRECT for T113-S3
 * 
 * This is a silicon-specific quirk, not a driver bug.
 *
 * Architecture:
 * - Char device with custom UAPI ioctls for graphics/UI acceleration
 * - DMA-BUF import/export support
 * - Sync fence integration with DRM
 * - Power management with reference counting
 * - Job queue with workqueue for async operations
 * - VSU hardware-accelerated scaling with proper alpha blending
 *
 * Based on fillrect v1.0.0 STABLE proven initialization sequence
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
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
#include "sunxi-g2d-structs.h"
#include "sunxi-g2d-rcq.h"


#define DRIVER_NAME		"sunxi-g2d"
#define DRIVER_VERSION		"2.9.15"
#define DRIVER_MAJOR		2
#define DRIVER_MINOR		7
#define DRIVER_PATCHLEVEL	1

/* Module parameters for RCQ experimentation */
static bool rcq_enable_bit = true;
module_param(rcq_enable_bit, bool, 0644);
MODULE_PARM_DESC(rcq_enable_bit, "Enable RCQ_CTRL.EN bit (default: true, try false if RCQ fails)");

static bool rcq_use_mixer_irq = false;
module_param(rcq_use_mixer_irq, bool, 0644);
MODULE_PARM_DESC(rcq_use_mixer_irq, "Use MIXER_INT in addition to RCQ IRQ (default: false)");


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

/* Forward declarations for internal functions used before definition */

/**
 * sunxi_g2d_do_scale - Isolated scaling operation using VSU
 *
 * Performs ONLY scaling (via VSU), nothing else. Can be called before
 * blit, blend, mask operations when scaling is required.
 *
 * G2D V2 limitation: Cannot combine VSU+BLD without RCQ.
 * Solution: Call this first to scale, then do blend/blit/etc.
 */
static int sunxi_g2d_do_scale(struct sunxi_g2d_dev *g2d,
                              dma_addr_t src_dma, u32 src_width, u32 src_height,
                              u32 src_pitch, u32 src_format,
                              u32 src_x, u32 src_y, u32 src_crop_w, u32 src_crop_h,
                              dma_addr_t dst_dma, u32 dst_width, u32 dst_height,
                              u32 dst_pitch, u32 dst_format,
                              u32 dst_x, u32 dst_y, u32 dst_out_w, u32 dst_out_h,
                              void *dst_vaddr);

static int sunxi_g2d_do_blit(struct sunxi_g2d_dev *g2d,
                              dma_addr_t src_dma, u32 src_width, u32 src_height,
                              u32 src_pitch, u32 src_format,
                              u32 src_x, u32 src_y, u32 src_crop_w, u32 src_crop_h,
                              dma_addr_t dst_dma, u32 dst_width, u32 dst_height,
                              u32 dst_pitch, u32 dst_format,
                              u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h);/**
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

	/* Fence synchronization */
	spinlock_t fence_lock;
	
	/* RCQ (Register Command Queue) */
	struct g2d_rcq_mem rcq;
	bool rcq_enabled;

	/* Fence support */
	u64 fence_context;
	atomic64_t fence_seqno;
	
	/* Temporary scaling buffer (allocated per-operation, freed after blend) */
	void *temp_scale_buffer;
	dma_addr_t temp_scale_dma;
	size_t temp_scale_size;
	
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
	struct sync_file *sync_file;
	struct work_struct cleanup_work;
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

static void sunxi_g2d_job_cleanup_workfn(struct work_struct *work)
{
	struct sunxi_g2d_job *job = container_of(work, struct sunxi_g2d_job, cleanup_work);

	if (!job)
		return;

	/* Trace cleanup entry */
	pr_debug("sunxi_g2d: cleanup job %p sync_file=%p fence=%p fd=%d\n",
			 job, job->sync_file, job->fence, job->fence_fd);

	/* Release sync_file if still present (not installed) */
	if (job->sync_file) {
		pr_debug("sunxi_g2d: cleanup - fput sync_file->file=%p\n", job->sync_file->file);
		if (job->sync_file->file)
			fput(job->sync_file->file);
		kfree(job->sync_file);
		job->sync_file = NULL;
	}

	if (job->fence) {
		pr_debug("sunxi_g2d: cleanup - dma_fence_put(%p)\n", job->fence);
		dma_fence_put(job->fence);
	}

	pr_debug("sunxi_g2d: cleanup - free job %p\n", job);
	kfree(job);
}

/* ===== dma_fence wrapper for sunxi jobs ===== */
struct sunxi_g2d_fence {
	struct dma_fence base;
	struct sunxi_g2d_dev *g2d;
	u64 seqno;
};

static const char *sunxi_g2d_fence_get_driver_name(struct dma_fence *f)
{
	return DRIVER_NAME;
}

static const char *sunxi_g2d_fence_get_timeline_name(struct dma_fence *f)
{
	return DRIVER_NAME;
}

static void sunxi_g2d_fence_release(struct dma_fence *f)
{
	struct sunxi_g2d_fence *sf = container_of(f, struct sunxi_g2d_fence, base);
	kfree(sf);
}

static const struct dma_fence_ops sunxi_g2d_fence_ops = {
	.get_driver_name = sunxi_g2d_fence_get_driver_name,
	.get_timeline_name = sunxi_g2d_fence_get_timeline_name,
	.release = sunxi_g2d_fence_release,
};

static struct dma_fence *sunxi_g2d_fence_create(struct sunxi_g2d_dev *g2d)
{
	struct sunxi_g2d_fence *sf;
	u64 seq;

	sf = kzalloc(sizeof(*sf), GFP_KERNEL);
	if (!sf)
		return NULL;

	seq = atomic64_inc_return(&g2d->fence_seqno);
	sf->g2d = g2d;
	sf->seqno = seq;
	dma_fence_init(&sf->base, &sunxi_g2d_fence_ops, &g2d->fence_lock,
		       g2d->fence_context, seq);
	/* Lightweight trace for fence creation (visible with dynamic_debug) */
	if (g2d->dev) {
		dev_dbg(g2d->dev, "sunxi_g2d: fence_create seq=%llu fence=%p\n", seq, &sf->base);
		/* Also emit info-level trace so it's visible without dynamic_debug; include pid for correlation */
		dev_info(g2d->dev, "sunxi_g2d: fence_create seq=%llu fence=%p pid=%d\n", seq, &sf->base, task_tgid_nr(current));
	}
	return &sf->base;
}

/* Delayed work item used by the userspace selftest ioctl: signal the
 * fence after a configured timeout and release the fence ref.
 */
struct g2d_selftest_work {
	struct delayed_work dwork;
	struct dma_fence *fence;
};

static void g2d_selftest_work_func(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct g2d_selftest_work *w = container_of(dwork, struct g2d_selftest_work, dwork);

	if (w->fence) {
		struct sunxi_g2d_fence *sf = container_of(w->fence, struct sunxi_g2d_fence, base);
		if (sf && sf->g2d && sf->g2d->dev)
			dev_dbg(sf->g2d->dev, "selftest: signalling fence seq=%llu fence=%p\n", sf->seqno, w->fence);
		else
			pr_debug("sunxi_g2d: selftest: signalling fence %p\n", w->fence);

		dma_fence_signal(w->fence);

		/* drop our reference taken for the work item */
		if (sf && sf->g2d && sf->g2d->dev)
			dev_dbg(sf->g2d->dev, "selftest: dma_fence_put fence=%p\n", w->fence);
		else
			pr_debug("sunxi_g2d: selftest: dma_fence_put fence=%p\n", w->fence);
	}
	dma_fence_put(w->fence);
	kfree(w);
}


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

/* ========== MBUS Configuration ========== */

/* T113/D1 (sun8iw20/sun20iw1) uses different register layout than older SoCs
 * Based on Allwinner Tina Linux sunxi_mbus.c driver:
 *   - Older SoCs: MBUS_MAST_CFG0_REG(n) = 0x0010 + (0x8 * n)
 *   - T113/D1:    MBUS_MAST_CFG0_REG(n) = 0x0210 + (0x10 * n)
 * 
 * Each master gets 16 bytes (0x10) of config space starting at 0x0210:
 *   Master 0: 0x0210 - 0x021F
 *   Master 1: 0x0220 - 0x022F
 *   Master 2: 0x0230 - 0x023F
 *   Master 3: 0x0240 - 0x024F
 *   Master 4: 0x0250 - 0x025F
 *  ... Master 9: 0x02A0 - 0x02AF (G2D)
 */
#define MBUS_MAST_CFG0_BASE  0x0210  /* Base for T113/D1 master config */
#define MBUS_MAST_CFG_STRIDE 0x0010  /* 16 bytes per master */
#define G2D_PORT      9       /* G2D is master 9 */

/* Calculate register offset for G2D master 9:
 * MBUS_MAST_CFG0_REG(9) = 0x0210 + (0x10 * 9) = 0x02A0
 * MBUS_MAST_CFG1_REG(9) = 0x0214 + (0x10 * 9) = 0x02A4
 */
#define MBUS_MAST_CFG0_REG(n)  (MBUS_MAST_CFG0_BASE + (MBUS_MAST_CFG_STRIDE * (n)))
#define MBUS_MAST_CFG1_REG(n)  (MBUS_MAST_CFG0_BASE + 0x4 + (MBUS_MAST_CFG_STRIDE * (n)))
#define MBUS_MAST_ABS_BWL_REG(n) (MBUS_MAST_CFG0_BASE + 0x8 + 0x10 * (n))
#define MBUS_BW_CFG_REG          0x0200
#define MBUS_MAST_ACEN_CFG_REG(k) (0x0020 + 0x04 * (k))

/* MBUS_MAST_CFG0_REG bit layout (from BSP sunxi_mbus.c):
 * Bit 0:     BANDWIDTH_LIMIT_ENABLE (BWLEN)
 * Bit 1:     MASTER_N_ACCESS_PRIORITY (PRI) - 0=low, 1=high
 * Bits 3-2:  MASTER_N_QOS_VALUE (QOS) - 0=lowest, 3=highest
 * Bits 7-4:  MASTER_N_WAIT_TIME (WT)
 * Bits 15-8: COMMAND_NUMBER (ACS)
 * Bits 31-16: BANDWIDTH_LIMIT_0 (BWL0)
 */
#define MBUS_PRI_SHIFT   1
#define MBUS_QOS_SHIFT   2
#define MBUS_QOS_MASK    0x3

/** Get the MBUS base address for the G2D device
 *
 * This function retrieves the MBUS base address for the G2D device by
 * parsing the device tree and mapping the appropriate memory region.
 *
 * @dev: Pointer to the G2D device structure
 * @return: Pointer to the MBUS base address, or ERR_PTR on failure
 */
static void __iomem *sunxi_g2d_map_mbus(struct device *dev)
{
    struct device_node *mbus_np;
    struct resource res;
    void __iomem *base;

    mbus_np = of_parse_phandle(dev->of_node, "interconnects", 0);
    if (!mbus_np)
        return ERR_PTR(-ENODEV);

    if (of_address_to_resource(mbus_np, 0, &res)) {
        of_node_put(mbus_np);
        return ERR_PTR(-ENODEV);
    }
    of_node_put(mbus_np);

    base = devm_ioremap(dev, res.start, resource_size(&res));
    if (!base)
        return ERR_PTR(-ENOMEM);

    return base;
}

/**
 * sunxi_g2d_setup_mbus_priority - Configure MBUS priority for G2D master
 * 
 * This is a quirk/workaround until a proper interconnect driver is implemented
 * for sun20i-d1/sun8i-t113 SoCs.
 * 
 * @g2d: G2D device
 * @return: 0 on success, negative error code on failure
 */
static int sunxi_g2d_setup_mbus(struct device *dev, void __iomem *mbus)
{
    u32 off = MBUS_MAST_CFG0_REG(G2D_PORT);     /* -> 0x02A0 */
    u32 v, nv;

    /* 1) Asegúrate de habilitar el master en ACEN */
    u32 reg_acen = MBUS_MAST_ACEN_CFG_REG(G2D_PORT / 32); /* 0x0020 */
    u32 bit = G2D_PORT % 32;
    u32 acen = readl_relaxed(mbus + reg_acen);
    if (!(acen & BIT(bit))) {
        writel_relaxed(acen | BIT(bit), mbus + reg_acen);
        wmb();
    }

    /* 2) PRI=1 y QOS=3 en CFG0(9) */
    v  = readl_relaxed(mbus + off);
    nv = v | BIT(MBUS_PRI_SHIFT);
    nv &= ~(MBUS_QOS_MASK << MBUS_QOS_SHIFT);
    nv |=  (MBUS_QOS_MASK << MBUS_QOS_SHIFT);

    if (nv != v) {
        writel_relaxed(nv, mbus + off);
        wmb();
    }

    /* (opcional) log/verify */
    nv = readl_relaxed(mbus + off);
    dev_dbg(dev, "MBUS CFG0(9) @0x%04x before=0x%08x after=0x%08x\n", off, v, nv);

    return 0;
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
	
	/* Step 2: TOP enable - open gates (0x3 = MIXER + ROT) */
	g2d_write(g2d, G2D_SCLK_GATE, 0x3);   /* Source clocks */
	g2d_write(g2d, G2D_HCLK_GATE, 0x3);   /* AHB clocks */
	g2d_write(g2d, G2D_AHB_RESET, 0x0);   /* Assert reset */
	wmb();
	udelay(10);
	g2d_write(g2d, G2D_AHB_RESET, 0x3);   /* Release reset */
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
	
	/* Step 4.5: Configure MBUS priority for G2D master (T113-S3 quirk)
	 * CRITICAL: Without this, G2D DMA transactions are blocked/delayed,
	 * causing RCQ operations to timeout and IRQs to never fire.
	 */
	void __iomem *mbus = sunxi_g2d_map_mbus(g2d->dev);
	if (IS_ERR(mbus))
		return PTR_ERR(mbus);

	sunxi_g2d_setup_mbus(g2d->dev, mbus);

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
	static volatile struct g2d_top_reg *g2d_top;
	static volatile struct g2d_mixer_glb_reg *g2d_mixer;
	u32 rot_status, rcq_status_before, rcq_status_after;
	bool handled = false;
	
	/* Map base to hardware structures */
	g2d_top = (volatile struct g2d_top_reg *)g2d->base;
	g2d_mixer = (volatile struct g2d_mixer_glb_reg *)(g2d->base + G2D_MIXER);
	
	/* v2.1.6: CRITICAL - task_end_irq is for execution completion!
	 * cfg_finish_irq = RCQ config accepted (headers read)
	 * task_end_irq = RCQ commands executed (drawing done)
	 * We need task_end_irq for actual completion!
	 */
	if (g2d_top->rcq_status.bits.task_end_irq) {
		rcq_status_before = g2d_top->rcq_status.dwval;
		
		/* Disable task_end IRQ to prevent retrigger */
		g2d_top->rcq_irq_ctl.bits.task_end_irq_en = 0;
		wmb();
		
		/* v2.1.8: MIXER reset CRITICAL! BSP does this immediately after detecting task_end.
		 * Reset sequence: 0->1 (assert reset, then deassert)
		 */
		g2d_top->ahb_rst.bits.mixer_ahb_rst = 0;
		wmb();
		g2d_top->ahb_rst.bits.mixer_ahb_rst = 1;
		wmb();
		
		/* Clear ONLY task_end_irq bit (bit 0) with direct W1C write */
		writel(0x00000001, g2d->base + G2D_RCQ_STATUS);
		wmb();
		
		rcq_status_after = g2d_top->rcq_status.dwval;
		
		dev_info(g2d->dev, "RCQ IRQ: task_end! status before=0x%08x after=0x%08x (MIXER reset done)\n",
		         rcq_status_before, rcq_status_after);
		
		/* Signal completion */
		atomic_set(&g2d->irq_done, 1);
		wake_up(&g2d->irq_wait);
		handled = true;
	}
	
	/* v2.4.0: Check MIXER interrupt for RCQ completion
	 * T113-S3 G2D IP 0x0110 uses MIXER_IRQ (not task_end_irq) for completion.
	 * This is the ONLY reliable IRQ for detecting when MIXER finishes execution.
	 */
	if (g2d_mixer->mixer_interrupt.bits.mixer_irq) {
		u32 mixer_int_before = g2d_mixer->mixer_interrupt.dwval;
		
		/* v2.4.0: MIXER reset after completion (BSP pattern) */
		g2d_top->ahb_rst.bits.mixer_ahb_rst = 0;
		wmb();
		g2d_top->ahb_rst.bits.mixer_ahb_rst = 1;
		wmb();
		
		/* Clear MIXER interrupt (W1C: Write 1 to clear) */
		g2d_mixer->mixer_interrupt.bits.mixer_irq = 1;
		wmb();
		
		dev_info(g2d->dev, "MIXER IRQ: completion! mixer_int=0x%08x (MIXER reset done)\n",
		         mixer_int_before);
		
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
		/* Signal user fence (if any) and install user FD for sync_file */
		{
			struct sunxi_g2d_job *job;

			spin_lock(&g2d->job_lock);
			job = g2d->current_job;
			/* Detach current job so next operations can proceed */
			g2d->current_job = NULL;
			spin_unlock(&g2d->job_lock);

			if (job) {
				/* Signal the fence */
				if (job->fence) {
					/* Log before signalling so we can correlate with userspace fd */
					{
						struct sunxi_g2d_fence *sf = container_of(job->fence, struct sunxi_g2d_fence, base);
						dev_info(g2d->dev, "irq: about to signal fence=%p seq=%llu job=%p fd=%d\n",
								 job->fence, sf->seqno, job, job->fence_fd);
					}
					dma_fence_signal(job->fence);
					/* Check and log if fence is signaled (should be true) */
					{
						int signaled = dma_fence_is_signaled(job->fence);
						struct sunxi_g2d_fence *sf = container_of(job->fence, struct sunxi_g2d_fence, base);
						dev_info(g2d->dev, "irq: fence signalled fence=%p seq=%llu signaled=%d job=%p fd=%d\n",
								 job->fence, sf->seqno, signaled, job, job->fence_fd);
					}
				}

				/* FD installation is performed in process context when the job
				 * is created (IOCTL). IRQ context MUST NOT touch process fd
				 * tables or struct file objects — see mitigation. At this
				 * point the fd should already be installed and job->sync_file
				 * will be NULL if installation succeeded.
				 */

			atomic64_inc(&g2d->jobs_done);

			/* Schedule cleanup in workqueue to avoid freeing in IRQ context */
			INIT_WORK(&job->cleanup_work, sunxi_g2d_job_cleanup_workfn);
			if (!queue_work(g2d->job_wq, &job->cleanup_work)) {
				/* If queueing failed, perform cleanup synchronously (fallback) */
				sunxi_g2d_job_cleanup_workfn(&job->cleanup_work);
			}
			}
		}
	}
	
	return IRQ_HANDLED;
}

/* ========== G2D Operations ========== */

/**
 * sunxi_g2d_format_to_hw - Convert UAPI format to hardware format value
 * @fmt: UAPI pixel format (enum g2d_pixel_format)
 * @bpp: Output bytes per pixel (optional, can be NULL)
 * 
 * Returns: Hardware format value, or -EINVAL if unsupported
 * 
 * Supports all RGB and YUV formats that the G2D hardware can handle.
 * YUV formats (>= 0x20) are only valid for V0 (video) layer.
 */
static int sunxi_g2d_format_to_hw(u32 fmt, u32 *bpp)
{
	u32 hw_fmt;
	u32 bytes_pp = 0;
	
	switch (fmt) {
	/* RGB formats - 32bpp */
	case G2D_FMT_ARGB8888:
		hw_fmt = G2D_FORMAT_ARGB8888;  /* 0x00 */
		bytes_pp = 4;
		break;
	case G2D_FMT_ABGR8888:
		hw_fmt = G2D_FORMAT_ABGR8888;  /* 0x01 */
		bytes_pp = 4;
		break;
	case G2D_FMT_RGBA8888:
		hw_fmt = G2D_FORMAT_RGBA8888;  /* 0x02 */
		bytes_pp = 4;
		break;
	case G2D_FMT_BGRA8888:
		hw_fmt = G2D_FORMAT_BGRA8888;  /* 0x03 */
		bytes_pp = 4;
		break;
	case G2D_FMT_XRGB8888:
		hw_fmt = G2D_FORMAT_XRGB8888;  /* 0x04 */
		bytes_pp = 4;
		break;
	case G2D_FMT_XBGR8888:
		hw_fmt = G2D_FORMAT_XBGR8888;  /* 0x05 */
		bytes_pp = 4;
		break;
	case G2D_FMT_RGBX8888:
		hw_fmt = G2D_FORMAT_RGBX8888;  /* 0x06 */
		bytes_pp = 4;
		break;
	case G2D_FMT_BGRX8888:
		hw_fmt = G2D_FORMAT_BGRX8888;  /* 0x07 */
		bytes_pp = 4;
		break;
		
	/* RGB formats - 24bpp */
	case G2D_FMT_RGB888:
		hw_fmt = G2D_FORMAT_RGB888;  /* 0x08 */
		bytes_pp = 3;
		break;
	case G2D_FMT_BGR888:
		hw_fmt = G2D_FORMAT_BGR888;  /* 0x09 */
		bytes_pp = 3;
		break;
		
	/* RGB formats - 16bpp */
	case G2D_FMT_RGB565:
		hw_fmt = G2D_FORMAT_RGB565;  /* 0x0A */
		bytes_pp = 2;
		break;
	case G2D_FMT_BGR565:
		hw_fmt = G2D_FORMAT_BGR565;  /* 0x0B */
		bytes_pp = 2;
		break;
		
	/* RGB formats - 16bpp with alpha */
	case G2D_FMT_ARGB4444:
		hw_fmt = G2D_FORMAT_ARGB4444;  /* 0x0C */
		bytes_pp = 2;
		break;
	case G2D_FMT_ABGR4444:
		hw_fmt = G2D_FORMAT_ABGR4444;  /* 0x0D */
		bytes_pp = 2;
		break;
	case G2D_FMT_RGBA4444:
		hw_fmt = G2D_FORMAT_RGBA4444;  /* 0x0E */
		bytes_pp = 2;
		break;
	case G2D_FMT_BGRA4444:
		hw_fmt = G2D_FORMAT_BGRA4444;  /* 0x0F */
		bytes_pp = 2;
		break;
	case G2D_FMT_ARGB1555:
		hw_fmt = G2D_FORMAT_ARGB1555;  /* 0x10 */
		bytes_pp = 2;
		break;
	case G2D_FMT_ABGR1555:
		hw_fmt = G2D_FORMAT_ABGR1555;  /* 0x11 */
		bytes_pp = 2;
		break;
	case G2D_FMT_RGBA5551:
		hw_fmt = G2D_FORMAT_RGBA5551;  /* 0x12 */
		bytes_pp = 2;
		break;
	case G2D_FMT_BGRA5551:
		hw_fmt = G2D_FORMAT_BGRA5551;  /* 0x13 */
		bytes_pp = 2;
		break;
		
	/* YUV formats - Interleaved (packed) 422 */
	case G2D_FMT_YUV422_I_YVYU:
		hw_fmt = G2D_FORMAT_IYUV422_V0Y1U0Y0;  /* 0x20 */
		bytes_pp = 2;  /* 2 bytes per pixel (averaged) */
		break;
	case G2D_FMT_YUV422_I_YUYV:
		hw_fmt = G2D_FORMAT_IYUV422_Y1V0Y0U0;  /* 0x21 */
		bytes_pp = 2;
		break;
	case G2D_FMT_YUV422_I_UYVY:
		hw_fmt = G2D_FORMAT_IYUV422_U0Y1V0Y0;  /* 0x22 */
		bytes_pp = 2;
		break;
	case G2D_FMT_YUV422_I_VYUY:
		hw_fmt = G2D_FORMAT_IYUV422_Y1U0Y0V0;  /* 0x23 */
		bytes_pp = 2;
		break;
		
	/* YUV formats - Semi-planar 422 */
	case G2D_FMT_YUV422_SP_UVUV:
		hw_fmt = G2D_FORMAT_YUV422UVC_V1U1V0U0;  /* 0x24 */
		bytes_pp = 1;  /* Y plane: 1 byte per pixel */
		break;
	case G2D_FMT_YUV422_SP_VUVU:
		hw_fmt = G2D_FORMAT_YUV422UVC_U1V1U0V0;  /* 0x25 */
		bytes_pp = 1;
		break;
		
	/* YUV formats - Planar 422 */
	case G2D_FMT_YUV422_P:
		hw_fmt = G2D_FORMAT_YUV422_PLANAR;  /* 0x26 */
		bytes_pp = 1;  /* Y plane: 1 byte per pixel */
		break;
		
	/* YUV formats - Semi-planar 420 */
	case G2D_FMT_YUV420_SP_UVUV:
		hw_fmt = G2D_FORMAT_YUV420UVC_V1U1V0U0;  /* 0x28 */
		bytes_pp = 1;
		break;
	case G2D_FMT_YUV420_SP_VUVU:
		hw_fmt = G2D_FORMAT_YUV420UVC_U1V1U0V0;  /* 0x29 */
		bytes_pp = 1;
		break;
		
	/* YUV formats - Planar 420 (I420/YV12) */
	case G2D_FMT_YUV420_P:
		hw_fmt = G2D_FORMAT_YUV420_PLANAR;  /* 0x2A */
		bytes_pp = 1;
		break;
		
	/* YUV formats - Semi-planar 411 */
	case G2D_FMT_YUV411_SP_UVUV:
		hw_fmt = G2D_FORMAT_YUV411UVC_V1U1V0U0;  /* 0x2C */
		bytes_pp = 1;
		break;
	case G2D_FMT_YUV411_SP_VUVU:
		hw_fmt = G2D_FORMAT_YUV411UVC_U1V1U0V0;  /* 0x2D */
		bytes_pp = 1;
		break;
		
	/* YUV formats - Planar 411 */
	case G2D_FMT_YUV411_P:
		hw_fmt = G2D_FORMAT_YUV411_PLANAR;  /* 0x2E */
		bytes_pp = 1;
		break;
		
	/* Monochrome formats */
	case G2D_FMT_8BPP_MONO:
		hw_fmt = G2D_FORMAT_Y8;  /* 0x30 - Grayscale */
		bytes_pp = 1;
		break;
		
	default:
		return -EINVAL;
	}
	
	if (bpp)
		*bpp = bytes_pp;
		
	return hw_fmt;
}

/**
 * sunxi_g2d_do_fillrect - Execute fillrect operation (DIRECT mode)
 * 
 * @color_format: Format of the color value (enum g2d_pixel_format from UAPI)
 * 
 * Based on fillrect v1.0.0 STABLE proven sequence
 */
static int sunxi_g2d_do_fillrect(struct sunxi_g2d_dev *g2d,
				  dma_addr_t dst_dma,
				  u32 width, u32 height,
				  u32 pitch, u32 color,
				  u32 color_format, u32 dst_format)
{
	struct g2d_mixer_ovl_v_reg v0 = {0};
	struct g2d_mixer_bld_reg bld = {0};
	struct g2d_mixer_write_back_reg wb = {0};
	union g2d_mixer_ctrl mixer_ctrl = {0};
	union g2d_mixer_interrupt mixer_int = {0};
	unsigned long timeout;
	int color_fmt_val, dst_fmt_val;
	
	/* Convert formats using helper function */
	color_fmt_val = sunxi_g2d_format_to_hw(color_format, NULL);
	if (color_fmt_val < 0) {
		dev_err(g2d->dev, "Unsupported color format: %u\n", color_format);
		return -EINVAL;
	}
	
	dst_fmt_val = sunxi_g2d_format_to_hw(dst_format, NULL);
	if (dst_fmt_val < 0) {
		dev_err(g2d->dev, "Unsupported destination format: %u\n", dst_format);
		return -EINVAL;
	}
	
	dev_info(g2d->dev, "FILLRECT: %ux%u color=0x%08x fmt=%u->0x%02X dst_fmt=%u->0x%02X\n",
		width, height, color, color_format, color_fmt_val, dst_format, dst_fmt_val);
	
	/* Convert color value ONLY when writing to framebuffer (XBGR destination)
	 * 
	 * For ION buffers (ABGR): DON'T swap. The fill_color writes to memory via
	 * writeback, and little-endian byte order automatically produces the correct
	 * layout for subsequent reads.
	 * 
	 * For framebuffer (XBGR): DO swap. The writeback format differs from V0 format,
	 * requiring explicit byte swap.
	 * 
	 * Input: 0xAARRGGBB (ARGB format from application)
	 * Output: 0xAABBGGRR (ABGR for XBGR framebuffer only)
	 */
	u32 hw_color = color;
	if (dst_fmt_val == G2D_FORMAT_XBGR8888) {
		hw_color = (color & 0xFF00FF00) |        /* Keep A and G */
		           ((color & 0x00FF0000) >> 16) | /* R -> B position */
		           ((color & 0x000000FF) << 16);  /* B -> R position */
		dev_info(g2d->dev, "Color swap for XBGR framebuffer: 0x%08x -> 0x%08x\n", 
			color, hw_color);
	}
	
	/* 1. Reset G2D */
	g2d_write(g2d, G2D_AHB_RESET, 0x0);
	g2d_write(g2d, G2D_AHB_RESET, 0x3);
	wmb();
	
	/* 2. Setup V0 layer - fillcolor mode using struct */
	v0.ovl_attr.bits.lay_en = 1;
	v0.ovl_attr.bits.lay_fillcolor_en = 1;  /* Use fill color, not memory */
	v0.ovl_attr.bits.lay_fbfmt = color_fmt_val;  /* Use specified color format */
	v0.ovl_attr.bits.lay_glbalpha = 0xFF;
	
	v0.ovl_mem.bits.lay_width = width - 1;
	v0.ovl_mem.bits.lay_height = height - 1;
	
	v0.ovl_winsize.bits.width = width - 1;
	v0.ovl_winsize.bits.height = height - 1;
	
	v0.ovl_mem_coor.bits.lay_xcoor = 0;
	v0.ovl_mem_coor.bits.lay_ycoor = 0;
	
	v0.ovl_mem_pitch0 = pitch;
	v0.ovl_fill_color = hw_color;  /* Use converted color for hardware */
	
	/* Write V0 registers */
	g2d_write(g2d, V0_ATTCTL, v0.ovl_attr.dwval);
	g2d_write(g2d, V0_MBSIZE, v0.ovl_mem.dwval);
	g2d_write(g2d, V0_SIZE, v0.ovl_winsize.dwval);
	g2d_write(g2d, V0_COOR, v0.ovl_mem_coor.dwval);
	g2d_write(g2d, V0_PITCH0, v0.ovl_mem_pitch0);
	g2d_write(g2d, V0_FILLC, v0.ovl_fill_color);
	
	/* 3. Setup BLD (blender) using struct */
	bld.bld_en_ctrl.bits.p0_en = 1;  /* Enable pipe 0 (V0) */
	bld.bld_en_ctrl.bits.p0_fcen = 0;  /* Read from layer, not fill color */
	
	bld.premulti_ctrl.dwval = 0;  /* No premultiplication */
	
	bld.mem_size[0].bits.width = width - 1;
	bld.mem_size[0].bits.height = height - 1;
	
	bld.mem_coor[0].bits.xcoor = 0;
	bld.mem_coor[0].bits.ycoor = 0;
	
	bld.out_size.bits.width = width - 1;
	bld.out_size.bits.height = height - 1;
	
	bld.out_color.bits.alpha_mode = 0;  /* RGB mode (not YUV) */
	bld.out_color.bits.premul_en = 0;
	
	bld.bld_ctrl.dwval = 0;  /* Simple copy mode */
	
	/* Write BLD registers */
	g2d_write(g2d, BLD_EN_CTL, bld.bld_en_ctrl.dwval);
	g2d_write(g2d, BLD_PREMUL_CTL, bld.premulti_ctrl.dwval);
	g2d_write(g2d, BLD_CH_ISIZE0, bld.mem_size[0].dwval);
	g2d_write(g2d, BLD_CH_OFFSET0, bld.mem_coor[0].dwval);
	g2d_write(g2d, BLD_OUT_SIZE, bld.out_size.dwval);
	g2d_write(g2d, BLD_SIZE, bld.out_size.dwval);
	g2d_write(g2d, BLD_OUT_COLOR, bld.out_color.dwval);
	g2d_write(g2d, BLD_CTL, bld.bld_ctrl.dwval);
	
	/* 4. Setup ROP (keep legacy values for now) */
	g2d_write(g2d, ROP_CTL, 0x000000f0);
	g2d_write(g2d, ROP_INDEX0, 0x00061080);
	
	/* 5. Setup WB (writeback) using struct */
	wb.wb_attr.bits.fmt = dst_fmt_val;  /* Use destination format */
	wb.wb_attr.bits.round_en = 0;
	
	wb.data_size.bits.width = width - 1;
	wb.data_size.bits.height = height - 1;
	
	wb.pitch0 = pitch;
	wb.laddr0 = lower_32_bits(dst_dma);
	wb.haddr0 = upper_32_bits(dst_dma);
	
	/* Write WB registers */
	g2d_write(g2d, WB_ATT, wb.wb_attr.dwval);
	g2d_write(g2d, WB_SIZE, wb.data_size.dwval);
	g2d_write(g2d, WB_PITCH0, wb.pitch0);
	g2d_write(g2d, WB_LADD0, wb.laddr0);
	g2d_write(g2d, WB_HADD0, wb.haddr0);
	wmb();
	
	/* 6. Setup MIXER control and interrupts */
	mixer_int.bits.mixer_irq = 0;  /* Clear any pending */
	mixer_int.bits.finish_irq_en = 1;  /* Enable finish interrupt */
	
	mixer_ctrl.bits.start = 0;  /* Clear before setting */
	
	/* Clear MIXER and IRQ */
	g2d_write(g2d, G2D_MIXER_INT, 0x00000000);
	g2d_write(g2d, G2D_MIXER_CTL, mixer_ctrl.dwval);
	wmb();
	
	/* Enable IRQ */
	atomic_set(&g2d->irq_done, 0);
	g2d_write(g2d, G2D_MIXER_INT, mixer_int.dwval);
	wmb();
	
	/* 7. Start MIXER */
	mixer_ctrl.bits.start = 1;
	g2d_write(g2d, G2D_MIXER_CTL, mixer_ctrl.dwval);
	wmb();
	
	/* v2.8.5: DUMP all critical G2D registers after legacy fillrect setup
	 * This allows comparing with RCQ buffer contents to find differences
	 */
	dev_info(g2d->dev, "=== LEGACY FILLRECT REGISTER DUMP ===\n");
	dev_info(g2d->dev, "MIXER: CTL=0x%08x FILLCOLOR0=0x%08x\n",
		 g2d_read(g2d, G2D_MIXER_CTL),
		 g2d_read(g2d, MIXER_FILLCOLOR0));
	dev_info(g2d->dev, "V0: ATTCTL=0x%08x MBSIZE=0x%08x SIZE=0x%08x COOR=0x%08x\n",
		 g2d_read(g2d, V0_ATTCTL),
		 g2d_read(g2d, V0_MBSIZE),
		 g2d_read(g2d, V0_SIZE),
		 g2d_read(g2d, V0_COOR));
	dev_info(g2d->dev, "V0: PITCH0=0x%08x FILLC=0x%08x\n",
		 g2d_read(g2d, V0_PITCH0),
		 g2d_read(g2d, V0_FILLC));
	dev_info(g2d->dev, "BLD: EN=0x%08x PREMUL=0x%08x ISIZE0=0x%08x OFFSET0=0x%08x\n",
		 g2d_read(g2d, BLD_EN_CTL),
		 g2d_read(g2d, BLD_PREMUL_CTL),
		 g2d_read(g2d, BLD_CH_ISIZE0),
		 g2d_read(g2d, BLD_CH_OFFSET0));
	dev_info(g2d->dev, "BLD: ISIZE1=0x%08x OFFSET1=0x%08x SIZE=0x%08x CTL=0x%08x\n",
		 g2d_read(g2d, BLD_CH_ISIZE1),
		 g2d_read(g2d, BLD_CH_OFFSET1),
		 g2d_read(g2d, BLD_SIZE),
		 g2d_read(g2d, BLD_CTL));
	dev_info(g2d->dev, "BLD: COLOR=0x%08x\n",
		 g2d_read(g2d, BLD_OUT_COLOR));
	dev_info(g2d->dev, "WB: ATTR=0x%08x SIZE=0x%08x PITCH0=0x%08x LADD0=0x%08x\n",
		 g2d_read(g2d, WB_ATT),
		 g2d_read(g2d, WB_SIZE),
		 g2d_read(g2d, WB_PITCH0),
		 g2d_read(g2d, WB_LADD0));
	dev_info(g2d->dev, "=== END REGISTER DUMP ===\n");
	dev_info(g2d->dev, "=== END REGISTER DUMP ===\n");
	
	/* 8. Wait for IRQ completion */
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

/**
 * sunxi_g2d_do_fillrect_rcq - Execute fillrect operation (RCQ mode)
 * 
 * ⚠️  ADVERTENCIA: Esta función NO ES FUNCIONAL AÚN en T113-S3
 * 
 * Estado: EXPERIMENTAL / NO FUNCIONAL
 * Problema: RCQ procesa comandos pero MIXER nunca ejecuta
 * Síntoma: cfg_finish_irq OK, pero task_end_irq timeout (100ms)
 * 
 * Ver: drivers/gpu/sunxi-g2d/RCQ-INVESTIGATION-REPORT.md para análisis completo
 * 
 * Mantenida solo para referencia futura e investigación.
 * Para uso productivo, utilizar sunxi_g2d_do_fillrect() (modo legacy).
 * 
 * @color_format: Format of the color value (enum g2d_pixel_format from UAPI)
 * @dst_format: Format of the destination buffer
 */
#if 0
static int sunxi_g2d_do_fillrect_rcq(struct sunxi_g2d_dev *g2d,
				      dma_addr_t dst_dma,
				      u32 width, u32 height,
				      u32 pitch, u32 color,
				      u32 color_format, u32 dst_format)
{
	struct g2d_mixer_ovl_v_reg v0 = {0};
	struct g2d_mixer_bld_reg bld = {0};
	struct g2d_mixer_write_back_reg wb = {0};
	volatile struct g2d_top_reg *g2d_top;
	volatile struct g2d_mixer_glb_reg *g2d_mixer;
	u32 color_fmt_val, dst_fmt_val;
	int ret;
	
	/* Map base to hardware structures */
	g2d_top = (volatile struct g2d_top_reg *)g2d->base;
	g2d_mixer = (volatile struct g2d_mixer_glb_reg *)(g2d->base + G2D_MIXER);
	
	/* Convert formats using helper function */
	int color_fmt_val = sunxi_g2d_format_to_hw(color_format, NULL);
	if (color_fmt_val < 0) {
		dev_err(g2d->dev, "Unsupported color format: %u\n", color_format);
		return -EINVAL;
	}
	
	int dst_fmt_val = sunxi_g2d_format_to_hw(dst_format, NULL);
	if (dst_fmt_val < 0) {
		dev_err(g2d->dev, "Unsupported destination format: %u\n", dst_format);
		return -EINVAL;
	}
	
	dev_info(g2d->dev, "FILLRECT_RCQ: %ux%u color=0x%08x fmt=%u->0x%02X dst_fmt=%u->0x%02X\n",
		width, height, color, color_format, color_fmt_val, dst_format, dst_fmt_val);
	
	/* Convert color value for XBGR framebuffer */
	u32 hw_color = color;
	if (dst_fmt_val == G2D_FORMAT_XBGR8888) {
		hw_color = (color & 0xFF00FF00) |        /* Keep A and G */
		           ((color & 0x00FF0000) >> 16) | /* R -> B position */
		           ((color & 0x000000FF) << 16);  /* B -> R position */
		dev_info(g2d->dev, "Color swap for XBGR framebuffer: 0x%08x -> 0x%08x\n", 
			color, hw_color);
	}
	
	/* Reset RCQ buffer */
	sunxi_g2d_rcq_reset(&g2d->rcq);
	
	/* v2.7.2: Reset MIXER BEFORE building RCQ buffer
	 * CRITICAL: Must reset before RCQ adds register blocks, otherwise
	 * reset will clear registers that RCQ configured
	 */
	g2d_top->ahb_rst.bits.mixer_ahb_rst = 0;
	wmb();
	g2d_top->ahb_rst.bits.mixer_ahb_rst = 1;
	wmb();
	
	dev_info(g2d->dev, "MIXER reset before RCQ setup (AHB_RST=0x%08x)\n",
		 g2d_top->ahb_rst.dwval);
	
	/* Configure V0 - fillcolor mode */
	v0.ovl_attr.bits.lay_en = 1;
	v0.ovl_attr.bits.lay_fillcolor_en = 1;  /* Use fill color */
	v0.ovl_attr.bits.lay_fbfmt = color_fmt_val;
	v0.ovl_attr.bits.lay_glbalpha = 0xFF;
	
	v0.ovl_mem.bits.lay_width = width - 1;
	v0.ovl_mem.bits.lay_height = height - 1;
	v0.ovl_mem_coor.bits.lay_xcoor = 0;
	v0.ovl_mem_coor.bits.lay_ycoor = 0;
	v0.ovl_mem_pitch0 = pitch;
	v0.ovl_fill_color = hw_color;
	v0.ovl_winsize.bits.width = width - 1;
	v0.ovl_winsize.bits.height = height - 1;
	
	/* Configure BLD (Blender) - pipe 0 with V0 fillcolor */
	bld.bld_en_ctrl.bits.p0_en = 1;  /* Enable p0 for V0 */
	bld.bld_en_ctrl.bits.p0_fcen = 0;  /* V0 provides fillcolor, not BLD */
	
	bld.mem_size[0].bits.width = width - 1;
	bld.mem_size[0].bits.height = height - 1;
	bld.mem_coor[0].bits.xcoor = 0;
	bld.mem_coor[0].bits.ycoor = 0;
	
	bld.out_size.bits.width = width - 1;
	bld.out_size.bits.height = height - 1;
	
	/* Simple COPY mode (V0 fillcolor → output) */
	bld.bld_ctrl.dwval = 0x00010001;  /* G2D_BLD_COPY */
	bld.premulti_ctrl.dwval = 0;
	bld.rop_ctrl.dwval = G2D_ROP3_SRCCOPY;
	bld.ch3_index0.dwval = 0x41000;
	bld.out_color.bits.alpha_mode = G2D_GLOBAL_ALPHA;
	bld.out_color.bits.premul_en = 0;
	
	/* Configure WB (Writeback) */
	wb.wb_attr.bits.fmt = dst_fmt_val;
	wb.data_size.bits.width = width - 1;
	wb.data_size.bits.height = height - 1;
	wb.pitch0 = pitch;
	wb.laddr0 = (u32)dst_dma;
	wb.haddr0 = (u32)((u64)dst_dma >> 32);
	
	/* Add register blocks to RCQ */
	
	/* MIXER control registers */
	u32 mixer_regs[2] __aligned(32);
	mixer_regs[0] = 0xFF000000;  /* FILLCOLOR0 */
	mixer_regs[1] = ((width - 1) & 0x1FFF) | (((height - 1) & 0x1FFF) << 16);  /* SIZE */
	ret = sunxi_g2d_rcq_add_block(&g2d->rcq, MIXER_FILLCOLOR0, mixer_regs, 8);
	if (ret) goto err_disable;
	
	/* V0 registers */
	u32 v0_regs[12] __aligned(32);
	v0_regs[0] = v0.ovl_attr.dwval;
	v0_regs[1] = v0.ovl_mem.dwval;
	v0_regs[2] = v0.ovl_mem_coor.dwval;
	v0_regs[3] = v0.ovl_mem_pitch0;
	v0_regs[4] = 0;  /* pitch1 */
	v0_regs[5] = 0;  /* pitch2 */
	v0_regs[6] = 0;  /* laddr0 - not used for fillcolor */
	v0_regs[7] = 0;  /* haddr */
	v0_regs[8] = 0;  /* laddr1 */
	v0_regs[9] = 0;  /* laddr2 */
	v0_regs[10] = v0.ovl_winsize.dwval;
	v0_regs[11] = v0.ovl_fill_color;
	ret = sunxi_g2d_rcq_add_block(&g2d->rcq, V0_ATTCTL, v0_regs, 48);
	if (ret) goto err_disable;
	
	/* BLD registers */
	u32 bld_en[1] __aligned(32);
	bld_en[0] = bld.bld_en_ctrl.dwval;
	ret = sunxi_g2d_rcq_add_block(&g2d->rcq, BLD_EN_CTL, bld_en, 4);
	if (ret) goto err_disable;
	
	/* Pipe 0 size and offset */
	u32 bld_sizes0[1] __aligned(32);
	bld_sizes0[0] = bld.mem_size[0].dwval;
	ret = sunxi_g2d_rcq_add_block(&g2d->rcq, BLD_CH_ISIZE0, bld_sizes0, 4);
	if (ret) goto err_disable;
	
	u32 bld_offsets0[1] __aligned(32);
	bld_offsets0[0] = bld.mem_coor[0].dwval;
	ret = sunxi_g2d_rcq_add_block(&g2d->rcq, BLD_CH_OFFSET0, bld_offsets0, 4);
	if (ret) goto err_disable;
	
	u32 bld_ctrl[4] __aligned(32);
	bld_ctrl[0] = bld.premulti_ctrl.dwval;
	bld_ctrl[1] = 0;  /* BK_COLOR */
	bld_ctrl[2] = bld.out_size.dwval;
	bld_ctrl[3] = bld.bld_ctrl.dwval;
	ret = sunxi_g2d_rcq_add_block(&g2d->rcq, BLD_PREMUL_CTL, bld_ctrl, 16);
	if (ret) goto err_disable;
	
	u32 bld_rop[2] __aligned(32);
	bld_rop[0] = bld.rop_ctrl.dwval;
	bld_rop[1] = bld.ch3_index0.dwval;
	ret = sunxi_g2d_rcq_add_block(&g2d->rcq, ROP_CTL, bld_rop, 8);
	if (ret) goto err_disable;
	
	u32 bld_out[1] __aligned(32);
	bld_out[0] = bld.out_color.dwval;
	ret = sunxi_g2d_rcq_add_block(&g2d->rcq, BLD_OUT_COLOR, bld_out, 4);
	if (ret) goto err_disable;
	
	
	
	
	/* WB registers */
	u32 wb_regs[5] __aligned(32);
	wb_regs[0] = wb.wb_attr.dwval;
	wb_regs[1] = wb.data_size.dwval;
	wb_regs[2] = wb.pitch0;
	wb_regs[3] = wb.laddr0;
	wb_regs[4] = wb.haddr0;
	ret = sunxi_g2d_rcq_add_block(&g2d->rcq, WB_ATT, wb_regs, 20);
	if (ret) goto err_disable;
	
	/* v2.8.1: Do NOT include MIXER_CTL in RCQ - hardware may protect this register
	 * We'll activate MIXER_START directly BEFORE RCQ UPDATE instead
	 */
	u32 mixer_int[1] __aligned(32);
	mixer_int[0] = 0x00000010;  /* finish_irq_en=1 (bit 4), mixer_irq=0 (clear pending) */
	ret = sunxi_g2d_rcq_add_block(&g2d->rcq, G2D_MIXER_INT, mixer_int, 4);
	if (ret) goto err_disable;
	
	dev_info(g2d->dev, "RCQ: Added all register blocks for fillrect (%u headers)\n",
		 g2d->rcq.header_count);
	
	/* DEBUG: Dump RCQ buffer contents */
	dev_info(g2d->dev, "RCQ buffer dump (%u headers, %u bytes):\n",
		 g2d->rcq.header_count, g2d->rcq.used);
	print_hex_dump(KERN_INFO, "RCQ: ", DUMP_PREFIX_OFFSET,
		       16, 4, g2d->rcq.vir_addr, g2d->rcq.used, false);
	
	
	
	/* v2.5.1: Read frame_cnt BEFORE starting RCQ to get baseline */
	u32 initial_frame_cnt = (g2d_read(g2d, G2D_RCQ_STATUS) >> 8) & 0xFF;
	
	/* v2.8.4: Enable RCQ task_end_irq BEFORE starting RCQ (exact BSP pattern)
	 * BSP sequence (g2d_mixer.c:873-901):
	 *   g2d_top_rcq_update_en(0);      // Clear UPDATE
	 *   g2d_top_set_rcq_head(...);     // Set HEAD/LEN
	 *   ... apply frames ...            // Build RCQ buffer
	 *   g2d_top_rcq_irq_en(1);         // Enable task_end_irq  <-- CRITICAL!
	 *   g2d_top_rcq_update_en(1);      // Trigger RCQ execution
	 *   
	 * Theory: T113-S3 RCQ v2 may REQUIRE task_end_irq_en=1 for MIXER to execute!
	 * We were disabling it, thinking we'd use MIXER_IRQ instead, but hardware
	 * might gate MIXER execution until this IRQ enable is set.
	 */
	dev_info(g2d->dev, "Enabling RCQ task_end_irq before RCQ UPDATE (BSP pattern)...\n");
	
	/* Clear IRQ completion flag */
	atomic_set(&g2d->irq_done, 0);
	
	/* Enable task_end_irq in RCQ */
	{
		union g2d_rcq_irq_ctl irq_ctl;
		irq_ctl.dwval = 0;
		irq_ctl.bits.task_end_irq_en = 1;  /* BSP does this BEFORE update */
		g2d_write(g2d, G2D_RCQ_IRQ_CTL, irq_ctl.dwval);
		wmb();
	}
	
	sunxi_g2d_rcq_setup_hw(g2d->base, &g2d->rcq);
	sunxi_g2d_rcq_start(g2d->base, rcq_enable_bit, true);  /* enable_irq=true for BSP pattern */
	
	dev_info(g2d->dev, "RCQ started (frame_cnt baseline=%u), waiting for task_end_irq via IRQ handler...\n",
		 initial_frame_cnt);

	/* v2.8.4: Wait for IRQ handler to signal completion via task_end_irq
	 * This is the EXACT BSP pattern - no manual MIXER_START, no polling.
	 * RCQ should configure everything and MIXER should auto-execute.
	 */
	{
		int timeout_jiffies = msecs_to_jiffies(100);  /* 100ms timeout */
		int wait_result;
		
		wait_result = wait_event_interruptible_timeout(
			g2d->irq_wait,
			atomic_read(&g2d->irq_done),
			timeout_jiffies
		);
		
		if (wait_result == 0) {
			/* Timeout */
			u32 mixer_ctl = g2d_read(g2d, G2D_MIXER_CTL);
			u32 mixer_int = g2d_read(g2d, G2D_MIXER_INT);
			u32 rcq_status = g2d_read(g2d, G2D_RCQ_STATUS);
			u32 rcq_irq_ctl = g2d_read(g2d, G2D_RCQ_IRQ_CTL);
			u32 current_frame_cnt = (rcq_status >> 8) & 0xFF;
			
			dev_err(g2d->dev, "RCQ IRQ timeout after 100ms\n");
			dev_err(g2d->dev, "  MIXER_CTL=0x%08X (START=%d)\n", 
				mixer_ctl, !!(mixer_ctl & G2D_MIXER_CTL_START));
			dev_err(g2d->dev, "  MIXER_INT=0x%08X (finish=%d)\n",
				mixer_int, !!(mixer_int & 0x1));
			dev_err(g2d->dev, "  RCQ_STATUS=0x%08X frame_cnt %u->%u task_end=%d cfg_finish=%d\n",
				rcq_status, initial_frame_cnt, current_frame_cnt,
				!!(rcq_status & 0x1), !!(rcq_status & 0x4));
			dev_err(g2d->dev, "  RCQ_IRQ_CTL=0x%08X (task_end_en=%d cfg_finish_en=%d)\n",
				rcq_irq_ctl, !!(rcq_irq_ctl & 0x10), !!(rcq_irq_ctl & 0x20));
			ret = -ETIMEDOUT;
			goto err_disable;
		} else if (wait_result < 0) {
			/* Signal interrupted */
			dev_err(g2d->dev, "RCQ wait interrupted by signal\n");
			ret = -ERESTARTSYS;
			goto err_disable;
		}
		
		/* Success */
		{
			u32 mixer_ctl = g2d_read(g2d, G2D_MIXER_CTL);
			u32 mixer_int = g2d_read(g2d, G2D_MIXER_INT);
			u32 rcq_status = g2d_read(g2d, G2D_RCQ_STATUS);
			u32 current_frame_cnt = (rcq_status >> 8) & 0xFF;
			
			dev_info(g2d->dev, "RCQ task_end_irq received!\n");
			dev_info(g2d->dev, "  MIXER_CTL=0x%08X MIXER_INT=0x%08X\n",
				 mixer_ctl, mixer_int);
			dev_info(g2d->dev, "  RCQ_STATUS=0x%08X frame_cnt %u->%u\n",
				 rcq_status, initial_frame_cnt, current_frame_cnt);
		}
	}


	/* MIXER reset after completion (BSP pattern) */
	g2d_top->ahb_rst.bits.mixer_ahb_rst = 0;
	wmb();
	g2d_top->ahb_rst.bits.mixer_ahb_rst = 1;
	wmb();
	
	dev_info(g2d->dev, "FILLRECT_RCQ completed via RCQ+MIXER_INT (BSP pattern, MIXER reset done)\n");
	
	/* DON'T disable hardware after success - keep it running for next operation */
	return 0;
	
err_disable:
	/* Only disable on setup errors, not timeouts */
	return ret;
}
#endif /* Deprecated sunxi_g2d_do_fillrect_rcq */

/*
 * DEPRECATED: sunxi_g2d_ioctl_fillrect_rcq()
 * This function is no longer used. G2D_IOC_FILLRECT_RCQ now redirects to
 * sunxi_g2d_ioctl_fillrect() in the ioctl switch.
 */
#if 0
static long sunxi_g2d_ioctl_fillrect_rcq(struct sunxi_g2d_dev *g2d, unsigned long arg)
{
	struct g2d_fillrect fill;
	struct dma_buf *dmabuf = NULL;
	struct dma_buf_attachment *attach = NULL;
	struct sg_table *sgt = NULL;
	dma_addr_t dma_addr;
	u32 width, height, pitch;
	int ret;

	/* RCQ no funcional en T113-S3 - ver RCQ-INVESTIGATION-REPORT.md */
	if (!g2d->rcq_enabled) {
		dev_warn_once(g2d->dev, 
			"G2D_IOC_FILLRECT_RCQ no soportado en T113-S3 (RCQ no funcional). "
			"Usar G2D_IOC_FILLRECT en su lugar.\n");
		return -EOPNOTSUPP;
	}

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

	dev_info(g2d->dev, "FILLRECT_RCQ ioctl: %ux%u at (%u,%u) color=0x%08x dma_fd=%d\n",
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

	/* Workaround for T113-S3 without IOMMU: sg_dma_address() may return 0x0
		* In this case, use physical address directly from the page
		*/
	if (dma_addr == 0 && sgt->nents > 0) {
		struct scatterlist *sg = sgt->sgl;
		struct page *page = sg_page(sg);
		if (page) {
			dma_addr = page_to_phys(page) + sg->offset;
			dev_info(g2d->dev, "T113 workaround: fillrect_rcq using physical address 0x%llx\n",
						(u64)dma_addr);
		} else {
			dev_err(g2d->dev, "Failed to get physical address from page\n");
			ret = -EINVAL;
			goto err_unmap;
		}
	}

	/* If userspace passed a fence_fd_in for this fill, wait on it */
	if (fill.fence_fd_in >= 0) {
		dev_info(g2d->dev, "fillrect_rcq: importing input fence fd=%d pid=%d\n",
					fill.fence_fd_in, task_tgid_nr(current));
		struct dma_fence *in_fence = sync_file_get_fence(fill.fence_fd_in);
		if (!in_fence) {
			ret = -EINVAL;
			goto err_unmap;
		}
		dev_info(g2d->dev, "fillrect_rcq: got in_fence=%p signaled=%d\n",
					in_fence, dma_fence_is_signaled(in_fence));
		dma_fence_wait(in_fence, false);
		dev_info(g2d->dev, "fillrect_rcq: in_fence=%p wait done signaled=%d\n",
					in_fence, dma_fence_is_signaled(in_fence));
		dma_fence_put(in_fence);
	}

	/* Calculate bytes per pixel based on format */
	u32 bpp;
	switch (fill.dst.format) {
	case G2D_FMT_ARGB8888:
	case G2D_FMT_ABGR8888:
	case G2D_FMT_RGBA8888:
	case G2D_FMT_BGRA8888:
	case G2D_FMT_XRGB8888:
	case G2D_FMT_XBGR8888:
	case G2D_FMT_RGBX8888:
	case G2D_FMT_BGRX8888:
		bpp = 4;
		break;
	case G2D_FMT_RGB888:
	case G2D_FMT_BGR888:
		bpp = 3;
		break;
	case G2D_FMT_RGB565:
	case G2D_FMT_BGR565:
	case G2D_FMT_ARGB4444:
	case G2D_FMT_ABGR4444:
	case G2D_FMT_RGBA4444:
	case G2D_FMT_BGRA4444:
	case G2D_FMT_ARGB1555:
	case G2D_FMT_ABGR1555:
	case G2D_FMT_RGBA5551:
	case G2D_FMT_BGRA5551:
		bpp = 2;
		break;
	default:
		dev_err(g2d->dev, "Unsupported destination format: %u\n", fill.dst.format);
		ret = -EINVAL;
		goto err_unmap;
	}

	/* Rectangle dimensions */
	width = fill.dst_w;
	height = fill.dst_h;

	/* Pitch is the stride of the BUFFER, not the rectangle */
	pitch = fill.dst.stride[0] ? fill.dst.stride[0] : (fill.dst.width * bpp);

	/* Adjust DMA address for dst_x/dst_y offset */
	dma_addr += (fill.dst_y * pitch) + (fill.dst_x * bpp);

	/* Use RCQ path */
	ret = sunxi_g2d_do_fillrect_rcq(g2d, dma_addr, width, height, pitch,
									fill.color, fill.color_format, fill.dst.format);

	/* Create job + fence and return fence_fd_out to userspace. */
	{
		struct sunxi_g2d_job *job;
		int out_fd = -1;

		job = kzalloc(sizeof(*job), GFP_KERNEL);
		if (!job) {
			ret = -ENOMEM;
			goto err_unmap;
		}

		job->fence = sunxi_g2d_fence_create(g2d);
		if (!job->fence) {
			kfree(job);
			ret = -ENOMEM;
			goto err_unmap;
		}

		out_fd = get_unused_fd_flags(O_CLOEXEC);
		if (out_fd < 0) {
			dma_fence_put(job->fence);
			kfree(job);
			ret = out_fd;
			goto err_unmap;
		}

		job->fence_fd = out_fd;
		job->sync_file = sync_file_create(job->fence);
		if (!job->sync_file) {
			put_unused_fd(out_fd);
			dma_fence_put(job->fence);
			kfree(job);
			ret = -ENOMEM;
			goto err_unmap;
		}

		dev_dbg(g2d->dev, "fillrect_rcq: created job=%p sync_file=%p file=%p fence=%p reserved fd=%d\n",
				job, job->sync_file, job->sync_file ? job->sync_file->file : NULL,
				job->fence, out_fd);

		if (job->sync_file && job->sync_file->file) {
			struct file *tmpf = job->sync_file->file;
			if (job->fence) {
				struct sunxi_g2d_fence *sf = container_of(job->fence, struct sunxi_g2d_fence, base);
				dev_info(g2d->dev, "fillrect_rcq: installing fd=%d file=%p job=%p fence=%p seq=%llu pid=%d\n",
							out_fd, tmpf, job, job->fence, sf->seqno, task_tgid_nr(current));
			} else {
				dev_info(g2d->dev, "fillrect_rcq: installing fd=%d file=%p job=%p fence=NULL pid=%d\n",
							out_fd, tmpf, job, task_tgid_nr(current));
			}
			fd_install(out_fd, tmpf);
			job->sync_file = NULL;
		} else {
			dev_err(g2d->dev, "fillrect_rcq: unexpected NULL sync_file/file for job=%p fd=%d\n", job, out_fd);
		}

		spin_lock(&g2d->job_lock);
		g2d->current_job = job;
		spin_unlock(&g2d->job_lock);

		fill.fence_fd_out = job->fence_fd;
	}

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
#endif /* Deprecated sunxi_g2d_ioctl_fillrect_rcq */

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

	dev_info(g2d->dev, "🔧 VSU: Setup scaling %ux%u -> %ux%u, alpha=%u fmt=%u (%s)\n",
		 in_w, in_h, out_w, out_h, alpha, fmt,
		 fmt == G2D_FMT_ARGB8888 ? "API:ARGB8888" :
		 fmt == G2D_FMT_ABGR8888 ? "API:ABGR8888" :
		 fmt == G2D_FMT_XRGB8888 ? "API:XRGB8888" :
		 fmt == G2D_FMT_XBGR8888 ? "API:XBGR8888" :
		 fmt == G2D_FMT_RGB565 ? "API:RGB565" : "UNKNOWN");

	/* Determine format type
	 * CRITICAL: Now accepts API format enums (G2D_FMT_*), not hardware (G2D_FORMAT_*)
	 * VSU processes pixels byte-by-byte, doesn't care about ARGB vs ABGR
	 * V0/WB handle the byte order interpretation when reading/writing
	 */
	switch (fmt) {
	case G2D_FMT_ARGB8888:
	case G2D_FMT_XRGB8888:
	case G2D_FMT_ABGR8888:
	case G2D_FMT_XBGR8888:
	case G2D_FMT_RGB565:
		format = VSU_FORMAT_RGB;
		dev_info(g2d->dev, "   VSU format determined: VSU_FORMAT_RGB (0x03)\n");
		break;
	default:
		dev_err(g2d->dev, "VSU: Unsupported format %u\n", fmt);
		return -EINVAL;
	}

	/* BSP CRITICAL ORDER: Write VS_CTRL FIRST to enable coefficient access
	 * This must be done BEFORE writing size registers
	 * 
	 * CRITICAL FIX: filter_type meaning based on BSP enum check:
	 * BSP: if (fmt > G2D_FORMAT_IYUV422_Y1U0Y0V0) filter_type=1; else filter_type=0
	 * RGB formats (0-23) are NOT > 0x23, so filter_type=0 for RGB
	 * filter_type=1 causes timeout - confirmed incorrect for RGB
	 */
	if (format == VSU_FORMAT_RGB) {
		g2d_write(g2d, VS_CTRL, 0x00101);  /* RGB mode: filter_type=0 + COEF_ACCESS + EN */
		dev_info(g2d->dev, "📝 VS_CTRL(initial) = 0x00101 (filter_type=0 RGB)\n");
	} else {
		g2d_write(g2d, VS_CTRL, 0x10101);  /* YUV mode: filter_type=1 + COEF_ACCESS + EN */
		dev_info(g2d->dev, "📝 VS_CTRL(initial) = 0x10101 (filter_type=1 YUV)\n");
	}

	/* Set output size */
	g2d_write(g2d, VS_OUT_SIZE, ((out_w - 1) & 0x1FFF) | (((out_h - 1) & 0x1FFF) << 16));
	g2d_write(g2d, VS_GLB_ALPHA, alpha);

	/* Set Y channel (luma/RGB) input size */
	g2d_write(g2d, VS_Y_SIZE, ((in_w - 1) & 0x1FFF) | (((in_h - 1) & 0x1FFF) << 16));
	
	dev_info(g2d->dev, "VSU REGS: VS_OUT_SIZE=0x%08X (out=%ux%u) VS_Y_SIZE=0x%08X (in=%ux%u)\n",
		 ((out_w - 1) & 0x1FFF) | (((out_h - 1) & 0x1FFF) << 16), out_w, out_h,
		 ((in_w - 1) & 0x1FFF) | (((in_h - 1) & 0x1FFF) << 16), in_w, in_h);

	/* Calculate horizontal step (input_width / output_width) in fixed-point */
	temp = (u64)in_w << VSU_PHASE_FRAC_BITWIDTH;
	if (out_w)
		do_div(temp, out_w);
	else
		temp = 0;
	yhstep = (u32)temp;
	
	/* T113 G2D spec: Supports 1/16× to 32× resize ratio
	 * However, empirical testing shows timeout at yhstep=0x5F229 (110→74)
	 * Last working: yhstep=0x58000 (110→80)
	 * 
	 * THEORY: The issue may not be the step limit itself, but rather:
	 * 1. Some other VSU configuration (phase, coefficients, window size)
	 * 2. Interaction with the << 1 shift when writing to register
	 * 3. A hardware erratum not documented in the spec
	 * 
	 * For now, we'll log a warning but ALLOW the operation to proceed,
	 * so we can gather more data about what actually fails.
	 */
	#define VSU_EMPIRICAL_SAFE_MAX 0x58000
	
	if (yhstep == 0) {
		dev_err(g2d->dev, "❌ VSU: yhstep is zero (out_w=%u)\n", out_w);
		return -EINVAL;
	}
	
	if (yhstep > VSU_EMPIRICAL_SAFE_MAX) {
		u32 ratio_x1000 = (out_w * 1000) / in_w;  /* Ratio * 1000 for 3 decimals */
		dev_warn(g2d->dev, "⚠️  VSU: yhstep=0x%08x exceeds empirical safe limit 0x%08x\n",
			 yhstep, VSU_EMPIRICAL_SAFE_MAX);
		dev_warn(g2d->dev, "   Scaling %ux%u→%ux%u (ratio=%u.%03u) - may timeout\n",
			 in_w, in_h, out_w, out_h, ratio_x1000 / 1000, ratio_x1000 % 1000);
		dev_warn(g2d->dev, "   Spec allows 1/16x to 32x (this is %u.%03ux), proceeding anyway...\n",
			 ratio_x1000 / 1000, ratio_x1000 % 1000);
	}
	
	/* VSU Step Register Write
	 * CRITICAL CORRECTION after extensive investigation:
	 * 
	 * Previous misunderstanding:
	 *   - We used VSU_PHASE_FRAC_BITWIDTH = 18 (WRONG)
	 *   - BSP actually uses 19 bits, NOT 18
	 * 
	 * Correct implementation (matching BSP):
	 *   - yhstep = (in_w << 19) / out_w  (19-bit fractional part)
	 *   - Register = yhstep << 1         (shift for hardware format)
	 *   - Total precision: 20 bits
	 * 
	 * BSP code (g2d_scal.c:306):
	 *   temp = in_w << VSU_PHASE_FRAC_BITWIDTH;  // 19 bits
	 *   yhstep = temp / out_w;
	 *   p_reg->y_hor_step.dwval = yhstep << 1;
	 */
	g2d_write(g2d, VS_Y_HSTEP, yhstep << 1);  /* BSP standard: << 1 */

	/* Calculate vertical step (input_height / output_height) in fixed-point */
	temp = (u64)in_h << VSU_PHASE_FRAC_BITWIDTH;
	if (out_h)
		do_div(temp, out_h);
	else
		temp = 0;
	yvstep = (u32)temp;
	
	if (yvstep == 0) {
		dev_err(g2d->dev, "❌ VSU: yvstep is zero (out_h=%u)\n", out_h);
		return -EINVAL;
	}
	
	if (yvstep > VSU_EMPIRICAL_SAFE_MAX) {
		dev_warn(g2d->dev, "⚠️  VSU: yvstep=0x%08x exceeds empirical safe limit 0x%08x\n",
			 yvstep, VSU_EMPIRICAL_SAFE_MAX);
		dev_warn(g2d->dev, "   Proceeding anyway (spec allows 1/16× to 32×)...\n");
	}
	
	/* BSP standard: << 1 for vertical step */
	g2d_write(g2d, VS_Y_VSTEP, yvstep << 1);
	
	/* BSP ORDER: Load Y coefficients IMMEDIATELY after steps, BEFORE chroma config
	 * CRITICAL: Calculate coefficient offsets based on step values!
	 * BSP: yhcoef_offset = g2d_vsu_calc_fir_coef(yhstep);
	 */
	yhcoef_offset = g2d_vsu_calc_fir_coef(yhstep);
	yvcoef_offset = g2d_vsu_calc_fir_coef(yvstep);

	/* Load horizontal coefficients (Lanczos with adaptive offset) */
	for (i = 0; i < VSU_PHASE_NUM; i++) {
		g2d_write(g2d, VS_Y_HCOEF0 + i * 4,
			  lan2coefftab32_full[yhcoef_offset + i]);
	}

	/* BSP CRITICAL: RGB uses LINEAR vertical coefficients!
	 * RGB formats: linearcoefftab32 (simple interpolation)
	 * YUV formats: lan2coefftab32_full (Lanczos filtering)
	 * This affects how hardware processes vertical scaling
	 */
	if (format == VSU_FORMAT_RGB) {
		/* RGB: Use linear interpolation for vertical (BSP line 1217) */
		for (i = 0; i < VSU_PHASE_NUM; i++) {
			g2d_write(g2d, VS_Y_VCOEF0 + i * 4, linearcoefftab32[i]);
		}
	} else {
		/* YUV: Use Lanczos for vertical */
		for (i = 0; i < VSU_PHASE_NUM; i++) {
			g2d_write(g2d, VS_Y_VCOEF0 + i * 4,
				  lan2coefftab32_full[yvcoef_offset + i]);
		}
	}
	
	g2d_write(g2d, VS_Y_HPHASE, 0);
	g2d_write(g2d, VS_Y_VPHASE0, 0);

	dev_info(g2d->dev, "🎯 VSU: About to configure chroma, format=%u (RGB=%u)\n",
		 format, VSU_FORMAT_RGB);

	/* === Configure Chroma (UV) channels for RGB ===
	 * BSP uses << 1 for step registers (with 19-bit fractional calculation)
	 * 
	 * BSP RGB default case (g2d_scal.c:439-440):
	 *   p_reg->c_hor_step.dwval = yhstep << 1;
	 *   p_reg->c_ver_step.dwval = yvstep << 1;
	 * 
	 * Where yhstep was calculated with 19-bit shift, then shifted << 1 for register
	 */
	if (format == VSU_FORMAT_RGB) {
		/* Set chroma input/output size same as luma */
		g2d_write(g2d, VS_C_SIZE, ((in_w - 1) & 0x1FFF) | (((in_h - 1) & 0x1FFF) << 16));
		
		/* BSP standard: << 1 for chroma step registers */
		g2d_write(g2d, VS_C_HSTEP, yhstep << 1);
		g2d_write(g2d, VS_C_VSTEP, yvstep << 1);
		
		/* Load chroma horizontal coefficients (same as luma for RGB) */
		for (i = 0; i < VSU_PHASE_NUM; i++) {
			g2d_write(g2d, VS_C_HCOEF0 + i * 4,
				  lan2coefftab32_full[yhcoef_offset + i]);
		}
		
		/* Zero phase for chroma */
		g2d_write(g2d, VS_C_HPHASE, 0);
		g2d_write(g2d, VS_C_VPHASE0, 0);
		
		dev_info(g2d->dev, "🎨 VSU CHROMA: size=%ux%u hstep=0x%08x vstep=0x%08x (BSP << 1)\n",
			 in_w, in_h, yhstep << 1, yvstep << 1);
	}

	/* BSP CRITICAL: For RGB, DO NOT load VS_C_VCOEF0!
	 * BSP only loads: VS_C_HCOEF0 + VS_Y_VCOEF0 for RGB
	 * Loading VS_C_VCOEF0 may confuse hardware in RGB mode
	 */

	/* BSP CRITICAL: Final VS_CTRL write after loading coefficients
	 * MUST disable COEF_ACCESS (bit8) after loading coefficients!
	 * 
	 * CRITICAL FIX: filter_type meaning based on BSP enum check:
	 * BSP: if (fmt > G2D_FORMAT_IYUV422_Y1U0Y0V0) filter_type=1; else filter_type=0
	 * RGB formats (0-23) are NOT > 0x23, so filter_type=0 for RGB
	 * filter_type=1 causes timeout - confirmed incorrect for RGB
	 * 
	 * RGB formats: VS_CTRL = 0x00001 (filter_type=0 + EN)
	 * YUV formats: VS_CTRL = 0x10001 (filter_type=1 + EN)
	 */
	if (format == VSU_FORMAT_RGB) {
		g2d_write(g2d, VS_CTRL, 0x00001);  /* RGB: filter_type=0 + EN */
		dev_info(g2d->dev, "📝 VS_CTRL(final) = 0x00001 (filter_type=0 RGB)\n");
	} else {
		g2d_write(g2d, VS_CTRL, 0x10001);  /* YUV: filter_type=1 + EN */
		dev_info(g2d->dev, "📝 VS_CTRL(final) = 0x10001 (filter_type=1 YUV)\n");
	}

	dev_info(g2d->dev, "✅ VSU: Enabled for scaling %ux%u->%ux%u (yhstep=0x%08x yvstep=0x%08x)\n",
		in_w, in_h, out_w, out_h, yhstep, yvstep);

	return 0;
}

/*
 * DEPRECATED: sunxi_g2d_do_blit_alpha_rcq()
 * RCQ (Register Command Queue) never worked on T113-S3.
 * Keeping as reference but disabled.
 */
#if 0
/*
 * sunxi_g2d_do_blit_alpha_rcq - Perform alpha blending operation using RCQ
 *
 * Uses RCQ (Register Command Queue) for hardware register writes.
 * This is the "Porter-Duff over" operation: dst = src*alpha + dst*(1-alpha)
 * 
 * @src_alpha: Global alpha value for source (0-255)
 * @src_alpha_mode: Alpha mode for source (G2D_PIXEL_ALPHA, G2D_GLOBAL_ALPHA, etc.)
 * @dst_alpha: Global alpha value for destination (0-255)
 * @dst_alpha_mode: Alpha mode for destination
 */
static int sunxi_g2d_do_blit_alpha_rcq(struct sunxi_g2d_dev *g2d,
				        dma_addr_t src_dma_addr, u32 src_w, u32 src_h,
				        u32 src_pitch, u32 src_format,
				        u32 src_x, u32 src_y, u32 src_crop_w, u32 src_crop_h,
				        u32 src_alpha, u32 src_alpha_mode,
				        dma_addr_t dst_dma_addr, u32 dst_w, u32 dst_h,
				        u32 dst_pitch, u32 dst_format,
				        u32 dst_x, u32 dst_y, u32 blend_w, u32 blend_h,
				        u32 dst_alpha, u32 dst_alpha_mode)
{
	/* RCQ never worked on T113-S3, return error immediately */
	dev_info(g2d->dev, "RCQ alpha blend not supported (never worked)\n");
	return -ENOSYS;
}
#endif /* Deprecated sunxi_g2d_do_blit_alpha_rcq */

/*
 * Get BLD_CTL value for Porter-Duff blending mode
 * Based on BSP driver implementation
 */
static u32 sunxi_g2d_get_bld_mode(u32 mode)
{
	switch (mode) {
	case 0:  /* G2D_BLD_CLEAR */
		return 0x00000000;
	case 1:  /* G2D_BLD_COPY */
		return 0x00010001;
	case 2:  /* G2D_BLD_DST */
		return 0x01000100;
	case 3:  /* G2D_BLD_SRCOVER */
		return 0x03010301;
	case 4:  /* G2D_BLD_DSTOVER */
		return 0x01030103;
	case 5:  /* G2D_BLD_SRCIN */
		return 0x00020002;
	case 6:  /* G2D_BLD_DSTIN */
		return 0x02000200;
	case 7:  /* G2D_BLD_SRCOUT */
		return 0x00030003;
	case 8:  /* G2D_BLD_DSTOUT */
		return 0x03000300;
	case 9:  /* G2D_BLD_SRCATOP */
		return 0x03020302;
	case 10: /* G2D_BLD_DSTATOP */
		return 0x02030203;
	case 11: /* G2D_BLD_XOR */
		return 0x03030303;
	default:
		return 0x03010301;  /* Default to SRCOVER */
	}
}

/*
 * sunxi_g2d_do_blit_alpha_3buf - Alpha blending with 3 separate buffers
 *
 * v2.9.16: 3-buffer alpha blending to avoid read/write conflicts
 * - src/V0: foreground layer (ball) - READ ONLY
 * - dst/UI2: background layer (gradient) - READ ONLY
 * - out/WB: output layer (temp) - WRITE ONLY
 */
static int sunxi_g2d_do_blit_alpha_3buf(struct sunxi_g2d_dev *g2d,
				    dma_addr_t src_dma_addr, u32 src_w, u32 src_h,
				    u32 src_pitch, u32 src_format,
				    u32 src_x, u32 src_y, u32 src_crop_w, u32 src_crop_h,
				    u8 src_alpha, u8 src_alpha_mode, u8 src_premul,
				    dma_addr_t dst_dma_addr, dma_addr_t dst_base_addr,
				    u32 dst_w, u32 dst_h,
				    u32 dst_pitch, u32 dst_format,
				    u32 dst_x, u32 dst_y, u32 blend_w, u32 blend_h,
				    u8 dst_alpha, u8 dst_alpha_mode, u8 dst_premul,
				    dma_addr_t out_dma_addr, u32 out_w, u32 out_h,
				    u32 out_pitch, u32 out_format,
				    u32 out_crop_w, u32 out_crop_h,
				    u32 bld_mode,
				    u32 color_key_enable, u32 color_key_mode,
				    u32 color_key_min, u32 color_key_max)
{
	/* v2.9.16: 3-buffer alpha blending - UI2 (dst READ) + V0 (src READ) → WB (out WRITE) */
	struct g2d_mixer_ovl_u_reg ui2 = {0};  /* Pipe0: background (dst) - READ ONLY */
	struct g2d_mixer_ovl_v_reg v0 = {0};   /* Pipe1: foreground (src) - READ ONLY */
	struct g2d_mixer_bld_reg bld = {0};    /* Blender */
	struct g2d_mixer_write_back_reg wb = {0}; /* Writeback (out) - WRITE ONLY */

	int src_fmt_val, dst_fmt_val, out_fmt_val;
	dma_addr_t ui2_addr, v0_addr, wb_addr;
	u32 ui2_bpp, v0_bpp, wb_bpp;
	unsigned long timeout;
	int ret;

	dev_info(g2d->dev, "BLIT_ALPHA_3BUF: src=%ux%u@(%u,%u) alpha=%u/%u premul=%u dst=%ux%u@(%u,%u) alpha=%u/%u premul=%u out=%ux%u blend=%ux%u\n",
		 src_crop_w, src_crop_h, src_x, src_y, src_alpha, src_alpha_mode, src_premul,
		 dst_w, dst_h, dst_x, dst_y, dst_alpha, dst_alpha_mode, dst_premul,
		 out_crop_w, out_crop_h, blend_w, blend_h);

	/* Check if scaling is needed (G2D V2 requires TWO operations for scale+blend) */
	bool needs_scaling = (src_crop_w != blend_w) || (src_crop_h != blend_h);
	
	if (needs_scaling) {
		/*
		 * G2D V2 CRITICAL LIMITATION: VSU cannot do positioning!
		 * VSU ONLY scales, it always writes to (0,0) of the output buffer.
		 * 
		 * WRONG approach (causes artifacts):
		 *   Scale 110×110 → 84×84 into 800×480 buffer at (345,185)
		 *   → VSU writes at (0,0), ignoring position!
		 * 
		 * CORRECT approach (BSP-compatible):
		 *   1. Allocate temporary buffer EXACTLY 84×84 (scaled size)
		 *   2. Scale 110×110 → temp buffer 84×84 at (0,0)
		 *   3. Use scaled temp as source for alpha blend
		 * 
		 * This matches BSP behavior: g2d_bsp_bitblt() allocates temp buffers
		 * for intermediate scaling results.
		 */
		dev_info(g2d->dev, "╔═══════════════════════════════════════════════════════════════╗\n");
		dev_info(g2d->dev, "║ SCALING REQUIRED: MULTI-STEP OPERATION                       ║\n");
		dev_info(g2d->dev, "╚═══════════════════════════════════════════════════════════════╝\n");
		
		/*
		 * CRITICAL: Allocate internal temp buffer for scaled result
		 * Size: blend_w × blend_h (exact scaled size, no positioning)
		 * 
		 * PITCH ALIGNMENT: G2D hardware requires pitch aligned to 128 bytes
		 * Tight pitch (width * 4) causes horizontal line shifting artifacts
		 */
		u32 temp_pitch = ALIGN(blend_w * 4, 128);  /* Align pitch to 128 bytes */
		u32 temp_size = temp_pitch * blend_h;      /* Use aligned pitch for buffer size */
		void *temp_vaddr = NULL;
		dma_addr_t temp_dma = 0;
		
		/* Force ARGB format to preserve alpha channel */
		u32 temp_format = (src_format == G2D_FMT_ARGB8888) ? G2D_FMT_ARGB8888 : out_format;
		
		dev_info(g2d->dev, "⚠️  Allocating internal temp buffer: %ux%u pitch=%u (%u bytes) fmt=%u\n",
			 blend_w, blend_h, temp_pitch, temp_size, temp_format);
		
		temp_vaddr = dma_alloc_coherent(g2d->dev, temp_size, &temp_dma, GFP_KERNEL);
		if (!temp_vaddr) {
			dev_err(g2d->dev, "Failed to allocate temp buffer for scaling\n");
			return -ENOMEM;
		}
		
		/* Clear temp buffer to avoid garbage/stale data affecting blend */
		memset(temp_vaddr, 0, temp_size);
		
		dev_info(g2d->dev, "✅ Temp buffer allocated: vaddr=%p dma=0x%llx pitch=%u (aligned to 128)\n",
			temp_vaddr, (u64)temp_dma, temp_pitch);
		
		/* 
		* STEP 1: Scale src → temp buffer (EXACT size, no positioning)
		* VSU writes to (0,0) of temp buffer, which is perfect because
		* temp buffer is exactly blend_w × blend_h
		*/
		dev_info(g2d->dev, "STEP 1: SCALE %ux%u → %ux%u (into temp buffer)\n",
			src_crop_w, src_crop_h, blend_w, blend_h);
		dev_info(g2d->dev, "  src: 0x%llx (%ux%u) fmt=%u crop=(%u,%u,%ux%u) pitch=%u\n",
			(u64)src_dma_addr, src_w, src_h, src_format,
			src_x, src_y, src_crop_w, src_crop_h, src_pitch);
		dev_info(g2d->dev, "  temp: 0x%llx (%ux%u) fmt=%u pitch=%u (ALIGNED)\n",
			(u64)temp_dma, blend_w, blend_h, temp_format, temp_pitch);
		
		ret = sunxi_g2d_do_scale(g2d,
		                         src_dma_addr, src_w, src_h,
		                         src_pitch, src_format,
		                         src_x, src_y, src_crop_w, src_crop_h,
		                         temp_dma, blend_w, blend_h,  /* TEMP: exact size */
		                         temp_pitch, temp_format,     /* pitch = ALIGNED */
		                         0, 0, blend_w, blend_h,       /* Write at (0,0) */
		                         temp_vaddr);                  /* DUMP temp buffer for debug */
		if (ret) {
			dev_err(g2d->dev, "❌ Step 1 (scale) failed: %d\n", ret);
			dma_free_coherent(g2d->dev, temp_size, temp_vaddr, temp_dma);
			return ret;
		}
		
		dev_info(g2d->dev, "✅ Step 1 complete: ball scaled %ux%u → %ux%u in temp\n",
			 src_crop_w, src_crop_h, blend_w, blend_h);
		
		/* 
		 * STEP 2: Alpha blend using scaled temp buffer
		 * Now we blend:
		 * - src = scaled ball (in temp buffer, full buffer read)
		 * - dst = background (from dst_dma_addr)
		 * - out = final result (to out_dma_addr at dst_x, dst_y)
		 */
		dev_info(g2d->dev, "STEP 2: ALPHA BLEND (temp + bg → out)\n");
		dev_info(g2d->dev, "  OLD src: dma=0x%llx fmt=%u w=%u h=%u\n",
			 (u64)src_dma_addr, src_format, src_crop_w, src_crop_h);
		
		/* Update src parameters to point to temp buffer */
		src_dma_addr = temp_dma;           /* Read from temp buffer */
		src_w = blend_w;                   /* Temp buffer width */
		src_h = blend_h;                   /* Temp buffer height */
		src_pitch = temp_pitch;            /* Temp buffer pitch (ALIGNED to 128) */
		src_format = temp_format;          /* Preserved alpha format */
		src_x = 0;                         /* Read from (0,0) of temp */
		src_y = 0;
		src_crop_w = blend_w;              /* Read entire temp buffer */
		src_crop_h = blend_h;
		
		dev_info(g2d->dev, "  NEW src: dma=0x%llx fmt=%u (%s) w=%u h=%u pitch=%u (ALIGNED)\n",
			 (u64)src_dma_addr, src_format,
			 src_format == G2D_FMT_ARGB8888 ? "ARGB8888" :
			 src_format == G2D_FMT_XRGB8888 ? "XRGB8888" : "OTHER",
			 src_w, src_h, src_pitch);
		dev_info(g2d->dev, "  src_crop: (%u,%u) %ux%u (FULL TEMP BUFFER)\n",
			 src_x, src_y, src_crop_w, src_crop_h);
		
		dev_info(g2d->dev, "→ Proceeding to STEP 2 (alpha blend)...\n");
		
		/* Fall through to normal alpha blend below (without scaling) */
		/* IMPORTANT: temp buffer will be freed after blend completes */
		
		/* Store temp buffer info for cleanup after blend */
		g2d->temp_scale_buffer = temp_vaddr;
		g2d->temp_scale_dma = temp_dma;
		g2d->temp_scale_size = temp_size;
	}

	/* Enable hardware */
	ret = sunxi_g2d_hw_enable(g2d);
	if (ret) {
		/* Cleanup temp buffer if allocated */
		if (g2d->temp_scale_buffer) {
			dma_free_coherent(g2d->dev, g2d->temp_scale_size,
					  g2d->temp_scale_buffer, g2d->temp_scale_dma);
			g2d->temp_scale_buffer = NULL;
		}
		return ret;
	}

	/* Map formats to hardware values using helper function */
	src_fmt_val = sunxi_g2d_format_to_hw(src_format, NULL);
	if (src_fmt_val < 0) {
		dev_err(g2d->dev, "Unsupported source format: %u\n", src_format);
		return -EINVAL;
	}

	dst_fmt_val = sunxi_g2d_format_to_hw(dst_format, &ui2_bpp);
	if (dst_fmt_val < 0) {
		dev_err(g2d->dev, "Unsupported destination format: %u\n", dst_format);
		return -EINVAL;
	}

	out_fmt_val = sunxi_g2d_format_to_hw(out_format, NULL);
	if (out_fmt_val < 0) {
		dev_err(g2d->dev, "Unsupported output format: %u\n", out_format);
		return -EINVAL;
	}

	/* Reset IRQ flag */
	atomic_set(&g2d->irq_done, 0);

	/* Clear and enable interrupts - SAME as fillrect */
	g2d_write(g2d, G2D_MIXER_INT, 0x00000000);
	udelay(1);
	g2d_write(g2d, G2D_MIXER_INT, 0x00000011);

	/* === Configure MIXER - single pipe mode === */
	g2d_write(g2d, MIXER_FILLCOLOR0, 0xFF000000);
	g2d_write(g2d, MIXER_SIZE, ((blend_w - 1) & 0x1FFF) | (((blend_h - 1) & 0x1FFF) << 16));

	dev_info(g2d->dev, "MIXER: single pipe mode size=%ux%u\n", blend_w, blend_h);

	/* === Configure UI2 (Pipe0: background from temp buffer) === */

	/* UI2 reads from dst buffer at the position where the ball is (dst_x, dst_y)
	 * IMPORTANT: Apply dst_x/dst_y offset to the address
	 * Note: ui2_bpp was calculated by sunxi_g2d_format_to_hw() above
	 */
	ui2_addr = dst_dma_addr + (dst_y * dst_pitch) + (dst_x * ui2_bpp);

	dev_info(g2d->dev, "UI2 addressing: base=0x%llx offset=(x=%u y=%u) → addr=0x%llx pitch=%u\n",
		 (u64)dst_dma_addr, dst_x, dst_y, (u64)ui2_addr, dst_pitch);

	/* Configure UI2 attributes - background with user alpha settings */
	ui2.ovl_attr.bits.lay_en = 1;
	ui2.ovl_attr.bits.alpha_mode = dst_alpha_mode;  /* User's dst alpha mode */
	ui2.ovl_attr.bits.lay_fbfmt = dst_fmt_val;
	ui2.ovl_attr.bits.lay_glbalpha = dst_alpha;  /* User's dst alpha value */
	
	dev_info(g2d->dev, "UI2 config: alpha_mode=%u (0=PIXEL,1=GLOBAL) global_alpha=%u\n",
		 dst_alpha_mode, dst_alpha);

	/* UI2 memory configuration - read the blend region from dst buffer
	 * The address (ui2_addr = dst_dma_addr) already points to (dst_x, dst_y),
	 * so we read blend_w x blend_h starting from (0,0) relative to that address
	 */
	ui2.ovl_mem.bits.lay_width = blend_w - 1;   /* Blend area width */
	ui2.ovl_mem.bits.lay_height = blend_h - 1;  /* Blend area height */
	ui2.ovl_mem_coor.bits.lay_xcoor = 0;  /* Already at correct position via address */
	ui2.ovl_mem_coor.bits.lay_ycoor = 0;
	ui2.ovl_mem_pitch0 = dst_pitch;  /* Full framebuffer stride */
	ui2.ovl_mem_low_addr0 = (u32)ui2_addr;
	ui2.ovl_mem_high_addr = (u32)((u64)ui2_addr >> 32);
	ui2.ovl_winsize.bits.width = blend_w - 1;   /* Output to blend area size */
	ui2.ovl_winsize.bits.height = blend_h - 1;

	/* Write UI2 to hardware */
	g2d_write(g2d, UI2_ATTR, ui2.ovl_attr.dwval);
	g2d_write(g2d, UI2_MBSIZE, ui2.ovl_mem.dwval);
	g2d_write(g2d, UI2_COOR, ui2.ovl_mem_coor.dwval);
	g2d_write(g2d, UI2_PITCH, ui2.ovl_mem_pitch0);
	g2d_write(g2d, UI2_LADD, ui2.ovl_mem_low_addr0);
	g2d_write(g2d, UI2_HADD, ui2.ovl_mem_high_addr);
	g2d_write(g2d, UI2_SIZE, ui2.ovl_winsize.dwval);

	dev_info(g2d->dev, "UI2 (Pipe0/BG): addr=0x%llx size=%ux%u pitch=%u win=%ux%u alpha=%u mode=%u\n",
		 (u64)ui2_addr, blend_w, blend_h, dst_pitch, blend_w, blend_h, dst_alpha, dst_alpha_mode);

	/* === Configure V0 (Pipe1: foreground/ball with alpha) === */

	/* Calculate bytes per pixel for V0 */
	switch (src_format) {
	case G2D_FMT_ARGB8888:
	case G2D_FMT_XRGB8888:
	case G2D_FMT_ABGR8888:
	case G2D_FMT_XBGR8888:
		v0_bpp = 4;
		break;
	case G2D_FMT_RGB565:
		v0_bpp = 2;
		break;
	default:
		v0_bpp = 4;
		break;
	}

	/* V0 address = ball buffer base + offset */
	v0_addr = src_dma_addr + (src_y * src_pitch) + (src_x * v0_bpp);
	
	dev_info(g2d->dev, "ALPHA_BLEND V0 ADDR: base=0x%llx crop_xy=(%u,%u) pitch=%u bpp=%u -> v0_addr=0x%llx\n",
		 (u64)src_dma_addr, src_x, src_y, src_pitch, v0_bpp, (u64)v0_addr);

	/* Configure V0 attributes - foreground with user alpha */
	v0.ovl_attr.bits.lay_en = 1;
	v0.ovl_attr.bits.alpha_mode = src_alpha_mode;  /* User's alpha mode */
	v0.ovl_attr.bits.lay_fbfmt = src_fmt_val;
	v0.ovl_attr.bits.lay_glbalpha = src_alpha;  /* User's alpha value */
	
	dev_info(g2d->dev, "V0 config: alpha_mode=%u (0=PIXEL,1=GLOBAL) global_alpha=%u\n",
		 src_alpha_mode, src_alpha);

	/* V0 memory configuration */
	v0.ovl_mem.bits.lay_width = src_crop_w - 1;
	v0.ovl_mem.bits.lay_height = src_crop_h - 1;
	v0.ovl_mem_coor.bits.lay_xcoor = 0;  /* Already offset in v0_addr */
	v0.ovl_mem_coor.bits.lay_ycoor = 0;
	v0.ovl_mem_pitch0 = src_pitch;
	v0.ovl_mem_low_addr0 = (u32)v0_addr;
	v0.ovl_mem_high_addr.bits.lay_y_hadd = (u32)((u64)v0_addr >> 32);
	v0.ovl_winsize.bits.width = src_crop_w - 1;
	v0.ovl_winsize.bits.height = src_crop_h - 1;

	dev_info(g2d->dev, "ALPHA_BLEND V0 PITCH WRITE: v0.ovl_mem_pitch0=%u src_pitch=%u\n",
		 v0.ovl_mem_pitch0, src_pitch);
	dev_info(g2d->dev, "ALPHA_BLEND V0 REGS: MBSIZE=0x%08X (lay_w=%u lay_h=%u) SIZE=0x%08X (win_w=%u win_h=%u)\n",
		 v0.ovl_mem.dwval, v0.ovl_mem.bits.lay_width, v0.ovl_mem.bits.lay_height,
		 v0.ovl_winsize.dwval, v0.ovl_winsize.bits.width, v0.ovl_winsize.bits.height);

	/* Write V0 to hardware */
	g2d_write(g2d, V0_ATTCTL, v0.ovl_attr.dwval);
	g2d_write(g2d, V0_MBSIZE, v0.ovl_mem.dwval);
	g2d_write(g2d, V0_COOR, v0.ovl_mem_coor.dwval);
	g2d_write(g2d, V0_PITCH0, v0.ovl_mem_pitch0);
	g2d_write(g2d, V0_LADD0, v0.ovl_mem_low_addr0);
	g2d_write(g2d, V0_HADD, v0.ovl_mem_high_addr.dwval);
	g2d_write(g2d, V0_SIZE, v0.ovl_winsize.dwval);

	dev_info(g2d->dev, "V0 (Pipe1/FOREGROUND): attr=0x%08X addr=0x%llx size=%ux%u alpha=%u mode=%u\n",
		 v0.ovl_attr.dwval, (u64)v0_addr, src_crop_w, src_crop_h, src_alpha, src_alpha_mode);

	/* === Configure BLD (Blender) - Two pipe alpha blending === */

	bld.bld_en_ctrl.bits.p0_en = 1;    /* Enable pipe0 (UI2/background) */
	bld.bld_en_ctrl.bits.p0_fcen = 0;  /* Use UI2 layer */
	bld.bld_en_ctrl.bits.p1_en = 1;    /* Enable pipe1 (V0/foreground) */
	bld.bld_en_ctrl.bits.p1_fcen = 0;  /* Use V0 layer */

	/* Configure premultiplication for alpha blending
	 * 0 = non-premultiplied alpha (straight alpha) - standard ARGB data
	 * 1 = premultiplied alpha (color already multiplied by alpha)
	 */
	bld.premulti_ctrl.bits.p0_alpha_mode = dst_premul;  /* Pipe0 (UI2/background) */
	bld.premulti_ctrl.bits.p1_alpha_mode = src_premul;  /* Pipe1 (V0/foreground) */

	/* Pipe input sizes - MUST match actual layer sizes for correct alpha blending */
	bld.mem_size[0].bits.width = blend_w - 1;     /* UI2: blend region size */
	bld.mem_size[0].bits.height = blend_h - 1;
	bld.mem_size[1].bits.width = blend_w - 1;     /* V0: blend region size (NOT src_crop!) */
	bld.mem_size[1].bits.height = blend_h - 1;

	/* Pipe positions - both at (0,0) for overlay */
	bld.mem_coor[0].bits.xcoor = 0;
	bld.mem_coor[0].bits.ycoor = 0;
	bld.mem_coor[1].bits.xcoor = 0;
	bld.mem_coor[1].bits.ycoor = 0;

	/* Output size */
	bld.out_size.bits.width = blend_w - 1;
	bld.out_size.bits.height = blend_h - 1;

	/* BLD control - Porter-Duff blending mode
	 * Default SRCOVER: out_color = src_color + dst_color * (1 - src_alpha)
	 * This creates proper transparency effect - foreground over background
	 * 
	 * BLD_CTL format: [31:24]=pipe3 [23:16]=pipe2 [15:8]=pipe1 [7:0]=pipe0
	 * SRCOVER uses 0x03010301:
	 *   pipe3=0x03, pipe2=0x01, pipe1=0x03 (V0/foreground), pipe0=0x01 (UI2/background)
	 */
	bld.bld_ctrl.dwval = sunxi_g2d_get_bld_mode(bld_mode);

	/* Output in RGB mode (framebuffer is XRGB8888) */
	bld.out_color.dwval = 0;  /* Clear all first */
	bld.out_color.bits.alpha_mode = 0;  /* RGB mode (not YUV) */
	bld.out_color.bits.premul_en = 0;   /* No premultiplication on output */

	/* === Configure Color Key (Chroma Key) === */
	if (color_key_enable) {
		/* Extract RGB components from min/max values (0xRRGGBB format) */
		u8 min_r = (color_key_min >> 16) & 0xFF;
		u8 min_g = (color_key_min >> 8) & 0xFF;
		u8 min_b = color_key_min & 0xFF;
		u8 max_r = (color_key_max >> 16) & 0xFF;
		u8 max_g = (color_key_max >> 8) & 0xFF;
		u8 max_b = color_key_max & 0xFF;

		/* Enable color key on pipe1 (V0/source layer) */
		bld.color_key.bits.key0_en = 1;
		bld.color_key.bits.key0_match_dir = color_key_mode;  /* 0=inside, 1=outside */

		/* Match all RGB channels */
		bld.color_key_cfg.bits.key0b_match = 1;  /* Match blue */
		bld.color_key_cfg.bits.key0g_match = 1;  /* Match green */
		bld.color_key_cfg.bits.key0y_match = 1;  /* Match red */

		/* Set color range */
		bld.color_key_min.bits.min_r = min_r;
		bld.color_key_min.bits.min_g = min_g;
		bld.color_key_min.bits.min_b = min_b;
		bld.color_key_max.bits.max_r = max_r;
		bld.color_key_max.bits.max_g = max_g;
		bld.color_key_max.bits.max_b = max_b;

		dev_info(g2d->dev, "CHROMAKEY: enabled mode=%u range=0x%06X-0x%06X (R:%u-%u G:%u-%u B:%u-%u)\n",
			 color_key_mode, color_key_min, color_key_max,
			 min_r, max_r, min_g, max_g, min_b, max_b);
	} else {
		/* Disable color key */
		bld.color_key.dwval = 0;
		bld.color_key_cfg.dwval = 0;
		bld.color_key_min.dwval = 0;
		bld.color_key_max.dwval = 0;
	}

	/* ROP: Source copy for alpha blending */
	bld.rop_ctrl.dwval = 0xf0;
	bld.ch3_index0.dwval = 0x00000000;

	/* Write BLD configuration */
	g2d_write(g2d, BLD_EN_CTL, bld.bld_en_ctrl.dwval);
	g2d_write(g2d, BLD_PREMUL_CTL, bld.premulti_ctrl.dwval);
	g2d_write(g2d, BLD_CH_ISIZE0, bld.mem_size[0].dwval);
	g2d_write(g2d, BLD_CH_ISIZE1, bld.mem_size[1].dwval);
	g2d_write(g2d, BLD_CH_OFFSET0, bld.mem_coor[0].dwval);
	g2d_write(g2d, BLD_CH_OFFSET1, bld.mem_coor[1].dwval);
	g2d_write(g2d, BLD_SIZE, bld.out_size.dwval);
	g2d_write(g2d, BLD_CTL, bld.bld_ctrl.dwval);
	g2d_write(g2d, ROP_CTL, bld.rop_ctrl.dwval);
	g2d_write(g2d, ROP_INDEX0, bld.ch3_index0.dwval);
	g2d_write(g2d, BLD_OUT_COLOR, bld.out_color.dwval);

	dev_info(g2d->dev, "BLD: en=0x%08X ctl=0x%08X (UI2+alpha V0 blending)\n",
		 bld.bld_en_ctrl.dwval, bld.bld_ctrl.dwval);

	/* === Configure Writeback - Write result to OUTPUT buffer (separate from inputs) === */

	/* Calculate bytes per pixel for writeback first (needed for offset calculation) */
	switch (out_format) {
	case G2D_FMT_ARGB8888:
	case G2D_FMT_XRGB8888:
	case G2D_FMT_ABGR8888:
	case G2D_FMT_XBGR8888:
		wb_bpp = 4;
		break;
	case G2D_FMT_RGB565:
		wb_bpp = 2;
		break;
	default:
		wb_bpp = 4;
		break;
	}

	/* WB address calculation:
	 * - If out_dma == dst_dma (in-place): Apply position offset (dst_x, dst_y)
	 * - If out_dma != dst_dma (separate out buffer): Write at (0, 0) of out buffer
	 * 
	 * CRITICAL: When using temp buffer (e.g., demo-bouncing-ball with 110x110 temp),
	 * dst_x/dst_y are framebuffer positions (345, 185) which would overflow temp buffer!
	 */
	if (out_dma_addr == dst_base_addr) {
		/* In-place operation: apply position offset */
		wb_addr = out_dma_addr + (dst_y * out_pitch) + (dst_x * wb_bpp);
		dev_info(g2d->dev, "WB (IN-PLACE): base=0x%llx offset=(x=%u y=%u) → addr=0x%llx\n",
			 (u64)out_dma_addr, dst_x, dst_y, (u64)wb_addr);
	} else {
		/* Separate output buffer: write at (0, 0) */
		wb_addr = out_dma_addr;
		dev_info(g2d->dev, "WB (SEPARATE): base=0x%llx (writing at 0,0 of output buffer)\n",
			 (u64)out_dma_addr);
	}

	/* Configure writeback */
	wb.wb_attr.dwval = 0;  /* Clear all bits first */
	wb.wb_attr.bits.fmt = out_fmt_val;
	wb.wb_attr.bits.round_en = 0;
	wb.data_size.bits.width = blend_w - 1;
	wb.data_size.bits.height = blend_h - 1;
	wb.pitch0 = out_pitch;
	wb.laddr0 = (u32)wb_addr;
	wb.haddr0 = (u32)((u64)wb_addr >> 32);

	/* Write WB to hardware */
	g2d_write(g2d, WB_ATT, wb.wb_attr.dwval);
	g2d_write(g2d, WB_SIZE, wb.data_size.dwval);
	g2d_write(g2d, WB_PITCH0, wb.pitch0);
	g2d_write(g2d, WB_LADD0, wb.laddr0);
	g2d_write(g2d, WB_HADD0, wb.haddr0);

	dev_info(g2d->dev, "WB: addr=0x%llx size=%ux%u pitch=%u offset=0x%llx (WRITE to output buffer at position)\n",
		 (u64)wb_addr, blend_w, blend_h, out_pitch, (u64)(wb_addr - out_dma_addr));

	/* 
	 * Note: VSU (scaling) is NOT configured here because:
	 * - If scaling was needed, it was already done in Step 1 (BLIT with VSU)
	 * - This step (Step 2) only does alpha blending without scaling
	 * - src_crop_w/h now equals blend_w/h (1:1 operation)
	 */
	g2d_write(g2d, VS_CTRL, 0);  /* Ensure VSU is disabled for alpha blend */

	/* === Start operation === */
	g2d_write(g2d, CMD_CTL, CMD_CTL_START);

	dev_dbg(g2d->dev, "MIXER started for correct alpha blending\n");

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

	dev_dbg(g2d->dev, "BLIT_ALPHA_CORRECT completed via IRQ\n");
	sunxi_g2d_hw_disable(g2d);
	
	/* Free temporary scaling buffer if it was allocated */
	if (g2d->temp_scale_buffer) {
		dev_info(g2d->dev, "Freeing temp scale buffer: vaddr=%p dma=0x%llx size=%zu\n",
			 g2d->temp_scale_buffer, (u64)g2d->temp_scale_dma, g2d->temp_scale_size);
		dma_free_coherent(g2d->dev, g2d->temp_scale_size,
				  g2d->temp_scale_buffer, g2d->temp_scale_dma);
		g2d->temp_scale_buffer = NULL;
		g2d->temp_scale_dma = 0;
		g2d->temp_scale_size = 0;
	}
	
	return 0;
}

/**
 * sunxi_g2d_dump_buffer - Dump buffer contents to /tmp for debugging
 * @g2d: G2D device
 * @vaddr: Virtual address of buffer (must be CPU-accessible)
 * @size: Buffer size in bytes
 * @name: Filename prefix (e.g., "vsu-output")
 *
 * Writes buffer to /tmp/<name>-<timestamp>.raw
 */
// static void sunxi_g2d_dump_buffer(struct sunxi_g2d_dev *g2d, void *vaddr,
//                                   size_t size, const char *name)
// {
// 	struct file *f;
// 	char path[128];
// 	loff_t pos = 0;
// 	ssize_t written;
	
// 	snprintf(path, sizeof(path), "/tmp/%s-%lld.raw", name, ktime_get_ns());
	
// 	f = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
// 	if (IS_ERR(f)) {
// 		dev_err(g2d->dev, "Failed to create dump file %s: %ld\n",
// 			path, PTR_ERR(f));
// 		return;
// 	}
	
// 	written = kernel_write(f, vaddr, size, &pos);
// 	filp_close(f, NULL);
	
// 	if (written != size) {
// 		dev_err(g2d->dev, "Dump incomplete: wrote %zd/%zu bytes to %s\n",
// 			written, size, path);
// 	} else {
// 		dev_info(g2d->dev, "✅ Dumped %zu bytes to %s\n", size, path);
// 	}
// }

/**
 * sunxi_g2d_do_scale - Isolated scaling operation using VSU
 *
 * Performs ONLY scaling (via Video Scaler Unit), no blending or other effects.
 * This is a building block that can be called before blend/blit/mask operations
 * when scaling is required.
 *
 * G2D V2 Architecture Limitation:
 * - Without RCQ, cannot combine VSU+BLD in single operation
 * - Solution: Call this first to scale, then call blend/blit/etc separately
 *
 * @src_crop_w x src_crop_h: Input size (what to read from source)
 * @dst_out_w x dst_out_h: Output size (what to write to destination)
 * VSU will scale from input size to output size.
 * @dst_vaddr: Optional CPU-accessible address of dst buffer for debugging (NULL = no dump)
 *
 * Returns: 0 on success, negative error code on failure
 */
static int sunxi_g2d_do_scale(struct sunxi_g2d_dev *g2d,
                              dma_addr_t src_dma, u32 src_width, u32 src_height,
                              u32 src_pitch, u32 src_format,
                              u32 src_x, u32 src_y, u32 src_crop_w, u32 src_crop_h,
                              dma_addr_t dst_dma, u32 dst_width, u32 dst_height,
                              u32 dst_pitch, u32 dst_format,
                              u32 dst_x, u32 dst_y, u32 dst_out_w, u32 dst_out_h,
                              void *dst_vaddr)
{
	struct g2d_mixer_ovl_v_reg v0 = {0};      /* V0 layer (video pipe with VSU) */
	struct g2d_mixer_bld_reg bld = {0};       /* Blender */
	struct g2d_mixer_write_back_reg wb = {0}; /* Writeback */
	
	u32 hw_src_fmt, hw_dst_fmt;
	dma_addr_t src_offset_dma, dst_offset_dma;
	u32 src_bpp, dst_bpp;
	unsigned long timeout;
	u32 mixer_ctl;
	int ret;
	
	dev_info(g2d->dev, "SCALE: %ux%u@(%u,%u) crop=%ux%u → %ux%u@(%u,%u) scaled=%ux%u\n",
		 src_width, src_height, src_x, src_y, src_crop_w, src_crop_h,
		 dst_width, dst_height, dst_x, dst_y, dst_out_w, dst_out_h);
	
	/* === Convert format enums to hardware values === */
	
	/* Source format conversion
	 * CRITICAL: ION buffers created by userspace are in native ARGB format
	 * The VSU reads pixels byte-by-byte and expects the format to match memory layout
	 * DO NOT swap R↔B for source - use direct mapping like BSP does
	 */
	switch (src_format) {
	case G2D_FMT_ARGB8888:
		src_bpp = 4;
		hw_src_fmt = G2D_FORMAT_ARGB8888;  /* Direct mapping, NO swap */
		break;
	case G2D_FMT_XRGB8888:
		src_bpp = 4;
		hw_src_fmt = G2D_FORMAT_XRGB8888;  /* Direct mapping, NO swap */
		break;
	case G2D_FMT_ABGR8888:
		src_bpp = 4;
		hw_src_fmt = G2D_FORMAT_ABGR8888;  /* Direct mapping */
		break;
	case G2D_FMT_XBGR8888:
		src_bpp = 4;
		hw_src_fmt = G2D_FORMAT_XBGR8888;  /* Direct mapping */
		break;
	case G2D_FMT_RGB565:
		src_bpp = 2;
		hw_src_fmt = G2D_FORMAT_RGB565;
		break;
	default:
		dev_err(g2d->dev, "Unsupported source format: %u\n", src_format);
		return -EINVAL;
	}
	
	/* Destination format conversion
	 * For DRM framebuffer output, we may need R↔B swap depending on DRM format
	 * But for intermediate buffers (temp), use direct mapping
	 */
	switch (dst_format) {
	case G2D_FMT_ARGB8888:
		dst_bpp = 4;
		hw_dst_fmt = G2D_FORMAT_ARGB8888;  /* Direct mapping for temp buffers */
		break;
	case G2D_FMT_XRGB8888:
		dst_bpp = 4;
		hw_dst_fmt = G2D_FORMAT_XRGB8888;  /* Direct mapping */
		break;
	case G2D_FMT_ABGR8888:
		dst_bpp = 4;
		hw_dst_fmt = G2D_FORMAT_ABGR8888;  /* Direct mapping */
		break;
	case G2D_FMT_XBGR8888:
		dst_bpp = 4;
		hw_dst_fmt = G2D_FORMAT_XBGR8888;  /* Direct mapping */
		break;
	case G2D_FMT_RGB565:
		dst_bpp = 2;
		hw_dst_fmt = G2D_FORMAT_RGB565;
		break;
	default:
		dev_err(g2d->dev, "Unsupported destination format: %u\n", dst_format);
		return -EINVAL;
	}
	
	/* Calculate DMA addresses with offsets */
	src_offset_dma = src_dma + (src_y * src_pitch) + (src_x * src_bpp);
	dst_offset_dma = dst_dma + (dst_y * dst_pitch) + (dst_x * dst_bpp);
	
	/* Enable hardware */
	ret = sunxi_g2d_hw_enable(g2d);
	if (ret)
		return ret;
	
	/* Reset IRQ flag */
	atomic_set(&g2d->irq_done, 0);
	
	/* === Hardware Configuration === */
	
	/* 1. Reset G2D */
	g2d_write(g2d, G2D_AHB_RESET, 0x0);
	g2d_write(g2d, G2D_AHB_RESET, 0x3);
	wmb();
	
	/* Clear and enable interrupts */
	g2d_write(g2d, G2D_MIXER_INT, 0x00000000);
	udelay(1);
	g2d_write(g2d, G2D_MIXER_INT, 0x00000011);
	
	/* === Configure V0 Layer (Source with VSU) === */
	
	v0.ovl_attr.dwval = 0;
	v0.ovl_attr.bits.lay_en = 1;                      /* Enable layer */
	v0.ovl_attr.bits.alpha_mode = 0;                  /* Pixel alpha mode */
	v0.ovl_attr.bits.lay_fbfmt = src_format;          /* TEST: Use API format directly, no conversion */
	v0.ovl_attr.bits.lay_glbalpha = 0xFF;             /* Global alpha */
	
	v0.ovl_mem.bits.lay_width = src_crop_w - 1;       /* Input width (buffer size) */
	v0.ovl_mem.bits.lay_height = src_crop_h - 1;      /* Input height (buffer size) */
	
	v0.ovl_mem_coor.bits.lay_xcoor = 0;               /* Position in blender */
	v0.ovl_mem_coor.bits.lay_ycoor = 0;
	
	v0.ovl_mem_pitch0 = src_pitch;
	v0.ovl_mem_low_addr0 = (u32)src_offset_dma;
	v0.ovl_mem_high_addr.bits.lay_y_hadd = (u32)((u64)src_offset_dma >> 32);
	
	v0.ovl_fill_color = 0;  /* Not used in image mode */
	
	/* TEST v2.9.43: ovl_winsize should match INPUT size when using VSU
	 * The VSU output size is configured separately in VS_OUT_SIZE
	 * ovl_winsize may define the "window" of pixels to READ, not WRITE
	 */
	v0.ovl_winsize.bits.width = src_crop_w - 1;   /* TEST: Use INPUT size */
	v0.ovl_winsize.bits.height = src_crop_h - 1;  /* TEST: Use INPUT size */
	
	/* BSP: DO NOT write HDS/VDS registers for RGB formats
	 * Only YUV formats or very aggressive scaling (>8x/4x) use HDS/VDS
	 * For RGB with VSU, leave HDS/VDS untouched (default disabled)
	 */
	/* Removed HDS/VDS writes */
	
	/* Write V0 registers using struct */
	dev_info(g2d->dev, "📐 VSU INPUT: src_crop=%ux%u, dst_out=%ux%u\n",
		 src_crop_w, src_crop_h, dst_out_w, dst_out_h);
	dev_info(g2d->dev, "📐 V0 CONFIG: mem=%ux%u, win=%ux%u, pitch=%u\n",
		 src_crop_w - 1, src_crop_h - 1, dst_out_w - 1, dst_out_h - 1, src_pitch);
	
	g2d_write(g2d, V0_ATTCTL, v0.ovl_attr.dwval);
	g2d_write(g2d, V0_MBSIZE, v0.ovl_mem.dwval);
	g2d_write(g2d, V0_COOR, v0.ovl_mem_coor.dwval);
	g2d_write(g2d, V0_PITCH0, v0.ovl_mem_pitch0);
	g2d_write(g2d, V0_LADD0, v0.ovl_mem_low_addr0);
	g2d_write(g2d, V0_HADD, v0.ovl_mem_high_addr.dwval);
	g2d_write(g2d, V0_FILLC, v0.ovl_fill_color);
	g2d_write(g2d, V0_SIZE, v0.ovl_winsize.dwval);
	/* BSP: DO NOT write HDS/VDS for RGB - removed writes */
	
	dev_info(g2d->dev, "V0: fmt=%u (hw=%u) mem=%ux%u win=%ux%u pitch=%u addr=0x%llx alpha=0x%02X\n",
		 src_format, hw_src_fmt, src_crop_w, src_crop_h,
		 dst_out_w, dst_out_h, src_pitch,
		 (u64)src_offset_dma, v0.ovl_attr.bits.lay_glbalpha);
	
	/* === Setup VSU (Video Scaler Unit) === */
	/* CRITICAL FIX: Pass API format (src_format), NOT hardware format (hw_src_fmt)!
	 * VSU processes pixels byte-by-byte agnostic to ARGB/ABGR order
	 * The ARGB↔ABGR conversion is for V0/WB overlay layers, not VSU
	 * V0 reads from VSU output with the correct hw_src_fmt interpretation
	 */
	ret = sunxi_g2d_vsu_setup(g2d, src_format, src_crop_w, src_crop_h,
	                          dst_out_w, dst_out_h, 0xff);
	if (ret) {
		dev_err(g2d->dev, "VSU setup failed: %d\n", ret);
		sunxi_g2d_hw_disable(g2d);
		return ret;
	}
	
	/* === Configure BLD (Blender) === */
	
	bld.bld_en_ctrl.dwval = 0;
	bld.bld_en_ctrl.bits.p0_en = 1;  /* Enable pipe 0 (V0) */
	
	bld.premulti_ctrl.dwval = 0;   /* No premultiply */
	
	bld.mem_coor[0].bits.xcoor = 0;  /* Channel 0 position */
	bld.mem_coor[0].bits.ycoor = 0;
	
	/* v2.9.45: REVERT v2.9.44 - BSP uses OUTPUT size for BLD_CH_ISIZE0
	 * BSP code (line 2007-2011): rect0.w = dst->clip_rect.w (OUTPUT)
	 * When VSU is enabled, BLD receives VSU OUTPUT, not input
	 */
	bld.mem_size[0].bits.width = dst_out_w - 1;   /* BSP: Use OUTPUT size */
	bld.mem_size[0].bits.height = dst_out_h - 1;  /* BSP: Use OUTPUT size */
	
	bld.out_size.bits.width = dst_out_w - 1;      /* Output size */
	bld.out_size.bits.height = dst_out_h - 1;
	
	bld.out_color.dwval = 0;
	bld.out_color.bits.alpha_mode = 0;  /* RGB mode (not YUV) */
	bld.out_color.bits.premul_en = 0;   /* No premultiplication on output */
	
	bld.bld_ctrl.dwval = 0;  /* Simple passthrough */
	
	bld.rop_ctrl.dwval = 0x000000f0;     /* ROP3: S (source copy) */
	bld.ch3_index0.dwval = 0x00061080;
	
	/* Write BLD registers using struct */
	g2d_write(g2d, BLD_EN_CTL, bld.bld_en_ctrl.dwval);
	g2d_write(g2d, BLD_PREMUL_CTL, bld.premulti_ctrl.dwval);
	g2d_write(g2d, BLD_CH_ISIZE0, bld.mem_size[0].dwval);
	g2d_write(g2d, BLD_CH_OFFSET0, bld.mem_coor[0].dwval);
	g2d_write(g2d, BLD_OUT_SIZE, bld.out_size.dwval);
	g2d_write(g2d, BLD_OUT_COLOR, bld.out_color.dwval);
	g2d_write(g2d, BLD_CTL, bld.bld_ctrl.dwval);
	g2d_write(g2d, ROP_CTL, bld.rop_ctrl.dwval);
	g2d_write(g2d, ROP_INDEX0, bld.ch3_index0.dwval);
	
	/* === Configure Writeback === */
	
	wb.wb_attr.dwval = 0;
	wb.wb_attr.bits.fmt = hw_dst_fmt;    /* Destination format (converted) */
	wb.wb_attr.bits.round_en = 0;
	
	wb.data_size.bits.width = dst_out_w - 1;
	wb.data_size.bits.height = dst_out_h - 1;
	
	wb.pitch0 = dst_pitch;
	wb.laddr0 = (u32)dst_offset_dma;
	wb.haddr0 = (u32)((u64)dst_offset_dma >> 32);
	
	/* Write WB registers using struct */
	g2d_write(g2d, WB_ATT, wb.wb_attr.dwval);
	g2d_write(g2d, WB_SIZE, wb.data_size.dwval);
	g2d_write(g2d, WB_PITCH0, wb.pitch0);
	g2d_write(g2d, WB_LADD0, wb.laddr0);
	g2d_write(g2d, WB_HADD0, wb.haddr0);
	
	dev_info(g2d->dev, "WB: fmt=%u (hw=%u) size=%ux%u pitch=%u addr=0x%llx\n",
		 dst_format, hw_dst_fmt, dst_out_w, dst_out_h, dst_pitch,
		 (u64)dst_offset_dma);
	
	/* === Start Operation === */
	
	mixer_ctl = G2D_MIXER_CTL_START;
	g2d_write(g2d, G2D_MIXER_CTL, mixer_ctl);
	
	/* Wait for completion */
	timeout = wait_event_timeout(g2d->irq_wait,
	                              atomic_read(&g2d->irq_done),
	                              msecs_to_jiffies(100));
	if (timeout == 0) {
		dev_err(g2d->dev, "Scale operation timeout (100ms)\n");
		sunxi_g2d_hw_disable(g2d);
		return -ETIMEDOUT;
	}
	
	if (!atomic_read(&g2d->irq_done)) {
		dev_err(g2d->dev, "Scale failed (no IRQ)\n");
		sunxi_g2d_hw_disable(g2d);
		return -EIO;
	}
	
	dev_info(g2d->dev, "✅ SCALE completed: %ux%u → %ux%u\n",
		 src_crop_w, src_crop_h, dst_out_w, dst_out_h);
	
	/* DEBUG: Dump output buffer if vaddr provided */
	// if (dst_vaddr) {
	// 	size_t dump_size = dst_out_h * dst_pitch;
	// 	dev_info(g2d->dev, "🔍 DUMPING VSU OUTPUT: %ux%u (pitch=%u) = %zu bytes to /tmp/vsu-output-*.raw\n",
	// 		 dst_out_w, dst_out_h, dst_pitch, dump_size);
	// 	dev_info(g2d->dev, "   To convert: convert -size %ux%u -depth 8 BGRA:vsu-output-*.raw vsu-output.png\n",
	// 		 dst_out_w, dst_out_h);
		
	// 	/* Log first few pixels to verify RGB data */
	// 	u32 *pixels = (u32 *)dst_vaddr;
	// 	int mid_offset = (dst_out_h / 2) * (dst_pitch / 4) + (dst_out_w / 2);
	// 	dev_info(g2d->dev, "   VSU OUTPUT PIXELS: [0]=0x%08x [1]=0x%08x [mid=%d]=0x%08x [mid+5]=0x%08x\n",
	// 		 pixels[0], pixels[1], mid_offset, pixels[mid_offset], pixels[mid_offset + 5]);
		
	// 	/* Detailed RGB byte analysis for first pixel (should be 0x804488EE if color preserved) */
	// 	u32 p0 = pixels[0];
	// 	u8 b0 = p0 & 0xFF;
	// 	u8 g0 = (p0 >> 8) & 0xFF;
	// 	u8 r0 = (p0 >> 16) & 0xFF;
	// 	u8 a0 = (p0 >> 24) & 0xFF;
	// 	dev_info(g2d->dev, "   🔬 PIXEL[0] BYTES: B=0x%02x G=0x%02x R=0x%02x A=0x%02x (expected: B=0xEE G=0x88 R=0x44 A=0x80)\n",
	// 		 b0, g0, r0, a0);
		
	// 	/* Check a few more pixels for pattern */
	// 	u32 p1 = pixels[1];
	// 	u32 pm = pixels[mid_offset];
	// 	dev_info(g2d->dev, "   🔬 PIXEL[1] = 0x%08x (B=%02x G=%02x R=%02x A=%02x)\n",
	// 		 p1, (u8)(p1 & 0xFF), (u8)((p1 >> 8) & 0xFF), (u8)((p1 >> 16) & 0xFF), (u8)((p1 >> 24) & 0xFF));
	// 	dev_info(g2d->dev, "   🔬 PIXEL[mid=%d] = 0x%08x (B=%02x G=%02x R=%02x A=%02x)\n",
	// 		 mid_offset, pm, (u8)(pm & 0xFF), (u8)((pm >> 8) & 0xFF), (u8)((pm >> 16) & 0xFF), (u8)((pm >> 24) & 0xFF));

		
	// 	sunxi_g2d_dump_buffer(g2d, dst_vaddr, dump_size, "vsu-output");
	// }
	
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
	
	/* Calculate source format info for V0 configuration */
	u32 src_bpp;
	switch (src_format) {
	case G2D_FMT_ARGB8888:
	case G2D_FMT_XRGB8888:
	case G2D_FMT_ABGR8888:
	case G2D_FMT_XBGR8888:
		src_bpp = 4;
		v0_attctl = 0x00; /* ARGB8888 format code for V0_ATTCTL */
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
	
	/* 2. Setup V0 layer - image mode with pixel alpha */
	v0_attctl |= 0xff010001;  /* alpha=0xff, pixel_alpha=1, fillcolor_en=0, EN=1 */
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
	
	dev_info(g2d->dev, "BLIT V0 CONFIG: MBSIZE=%ux%u SIZE=%ux%u PITCH=%u addr=0x%llx\n",
		 src_crop_w, src_crop_h, src_crop_w, src_crop_h, src_pitch, (u64)src_offset_dma);
	
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
	
	dev_info(g2d->dev, "BLIT WB CONFIG: size=%ux%u pitch=%u dst_xy=(%u,%u) addr=0x%llx (offset from base=0x%llx)\n",
		 dst_w, dst_h, dst_pitch, dst_x, dst_y, (u64)dst_offset_dma, (u64)(dst_offset_dma - dst_dma));
	
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
		
		/* CRITICAL FIX: Pass API format (src_format), NOT hardware format (hw_src_fmt)!
		 * VSU processes pixels byte-by-byte agnostic to ARGB/ABGR order
		 * The ARGB↔ABGR conversion is for V0/WB overlay layers, not VSU
		 * V0 reads from VSU output with the correct hw_src_fmt interpretation
		 */
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

/*
 * sunxi_g2d_do_blit_unified - Unified blit operation handler
 *
 * This function handles all blit combinations intelligently:
 * - Simple copy (no scaling, no blending, no rotation)
 * - Scaling only (VSU enabled)
 * - Alpha blending (BLD enabled, with or without scaling)
 * - Rotation/flip (ROT enabled, no scaling allowed)
 * - Blending + rotation (BLD + ROT, no scaling)
 * 
 * Hardware restrictions:
 * - Cannot do scaling + rotation simultaneously (share VSU module)
 * - For scaling + rotation: TODO implement 2-pass with temp buffer
 * 
 * Alpha blending is enabled when:
 * - needs_alpha=true (auto-detected from alpha_mode or formats)
 */
static int sunxi_g2d_do_blit_unified(struct sunxi_g2d_dev *g2d,
				     dma_addr_t src_dma_addr, u32 src_w, u32 src_h,
				     u32 src_pitch, u32 src_format,
				     u32 src_x, u32 src_y, u32 src_crop_w, u32 src_crop_h,
				     u8 src_alpha, u8 src_alpha_mode, u8 src_premul,
				     dma_addr_t dst_dma_addr, u32 dst_w, u32 dst_h,
				     u32 dst_pitch, u32 dst_format,
				     u32 dst_x, u32 dst_y, u32 blend_w, u32 blend_h,
				     u8 dst_alpha, u8 dst_alpha_mode, u8 dst_premul,
				     dma_addr_t out_dma_addr,  /* Explicit output buffer (may == dst_dma_addr) */
				     u32 out_w, u32 out_h, u32 out_pitch, u32 out_format,  /* Output dimensions */
				     u32 flags,
				     u32 bld_mode,  /* Porter-Duff blend mode */
				     u32 color_key_enable, u32 color_key_mode,
				     u32 color_key_min, u32 color_key_max,
				     bool needs_alpha, bool needs_rotation, bool needs_scaling)
{
	/* For now, delegate to existing implementations based on operation type
	 * TODO: Consolidate into single implementation that enables/disables blocks as needed
	 */
	
	if (needs_rotation) {
		/* Rotation path - use ROT block (no scaling, blending ignored for now) */
		return sunxi_g2d_do_blit_rot(g2d,
					     src_dma_addr, src_w, src_h,
					     src_pitch, src_format,
					     src_x, src_y, src_crop_w, src_crop_h,
					     out_dma_addr, out_w, out_h,  /* Write to out */
					     out_pitch, out_format,
					     dst_x, dst_y, blend_w, blend_h,
					     flags);
	} else if (needs_alpha) {
		/* Alpha blending path - can include scaling */
		/* For 3-buffer approach: use explicit out dimensions */
		return sunxi_g2d_do_blit_alpha_3buf(g2d,
						    src_dma_addr, src_w, src_h,
						    src_pitch, src_format,
						    src_x, src_y, src_crop_w, src_crop_h,
						    src_alpha, src_alpha_mode, src_premul,
						    dst_dma_addr, dst_dma_addr, /* dst_base same as dst */
						    dst_w, dst_h,
						    dst_pitch, dst_format,
						    dst_x, dst_y, blend_w, blend_h,
						    dst_alpha, dst_alpha_mode, dst_premul,
						    out_dma_addr, /* Write to out (may == dst for in-place) */
						    out_w, out_h,
						    out_pitch, out_format,
						    blend_w, blend_h,  /* out_crop = blend size */
						    bld_mode,
						    color_key_enable, color_key_mode,
						    color_key_min, color_key_max);         /* Porter-Duff blend mode */
	} else {
		/* Simple copy/scale path - use MIXER with optional VSU */
		return sunxi_g2d_do_blit(g2d,
					 src_dma_addr, src_w, src_h,
					 src_pitch, src_format,
					 src_x, src_y, src_crop_w, src_crop_h,
					 out_dma_addr, out_w, out_h,  /* Write to out */
					 out_pitch, out_format,
					 dst_x, dst_y, blend_w, blend_h);
	}
}

static long sunxi_g2d_ioctl_blit(struct sunxi_g2d_dev *g2d, unsigned long arg)
{
	struct g2d_blit blit;
	struct dma_buf *src_dmabuf = NULL, *dst_dmabuf = NULL, *out_dmabuf = NULL;
	struct dma_buf_attachment *src_attach = NULL, *dst_attach = NULL, *out_attach = NULL;
	struct sg_table *src_sgt = NULL, *dst_sgt = NULL, *out_sgt = NULL;
	dma_addr_t src_dma_addr, dst_dma_addr, out_dma_addr;
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
	
	dev_info(g2d->dev, "BLIT: src=%ux%u fmt=%u (alpha=%u mode=%u) -> dst=%u,%u,%ux%u fmt=%u (alpha=%u mode=%u) flags=0x%x\n",
		 blit.src.width, blit.src.height, blit.src.format,
		 blit.src.alpha, blit.src.alpha_mode,
		 blit.dst_x, blit.dst_y, blit.dst_w, blit.dst_h, blit.dst.format,
		 blit.dst.alpha, blit.dst.alpha_mode,
		 blit.flags);
	
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
	if (!src_dma_addr) {
		/* Fallback to physical address if DMA address not set */
		src_dma_addr = sg_phys(src_sgt->sgl);
	}

	/* If userspace provided an input fence fd, wait on it first */
	if (blit.fence_fd_in >= 0) {
		dev_info(g2d->dev, "blit: importing input fence fd=%d pid=%d\n",
			 blit.fence_fd_in, task_tgid_nr(current));
		struct dma_fence *in_fence = sync_file_get_fence(blit.fence_fd_in);
		if (!in_fence) {
			ret = -EINVAL;
			goto err_unmap_src;
		}
		dev_info(g2d->dev, "blit: got in_fence=%p signaled=%d\n",
			 in_fence, dma_fence_is_signaled(in_fence));
		dma_fence_wait(in_fence, false);
		dev_info(g2d->dev, "blit: in_fence=%p wait done signaled=%d\n",
			 in_fence, dma_fence_is_signaled(in_fence));
		dma_fence_put(in_fence);
	}
	
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
	
	/* For alpha blending, destination needs BIDIRECTIONAL since we read background and write result */
	dst_sgt = dma_buf_map_attachment(dst_attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(dst_sgt)) {
		ret = PTR_ERR(dst_sgt);
		dev_err(g2d->dev, "Failed to map destination dma_buf: %d\n", ret);
		goto err_detach_dst;
	}
	
	dst_dma_addr = sg_dma_address(dst_sgt->sgl);
	if (!dst_dma_addr) {
		/* Fallback to physical address if DMA address not set */
		dst_dma_addr = sg_phys(dst_sgt->sgl);
	}
	
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
	
	/* Import output buffer if provided (for 3-buffer operations) */
	if (blit.out.dma_fd >= 0) {
		out_dmabuf = dma_buf_get(blit.out.dma_fd);
		if (IS_ERR(out_dmabuf)) {
			ret = PTR_ERR(out_dmabuf);
			dev_err(g2d->dev, "Failed to get output dma_buf: %d\n", ret);
			goto err_unmap_dst;
		}
		
		out_attach = dma_buf_attach(out_dmabuf, g2d->dev);
		if (IS_ERR(out_attach)) {
			ret = PTR_ERR(out_attach);
			dev_err(g2d->dev, "Failed to attach output dma_buf: %d\n", ret);
			goto err_put_out_dmabuf;
		}
		
		out_sgt = dma_buf_map_attachment(out_attach, DMA_FROM_DEVICE);
		if (IS_ERR(out_sgt)) {
			ret = PTR_ERR(out_sgt);
			dev_err(g2d->dev, "Failed to map output dma_buf: %d\n", ret);
			goto err_detach_out;
		}
		
		out_dma_addr = sg_dma_address(out_sgt->sgl);
		if (!out_dma_addr) {
			/* Fallback to physical address if DMA address not set */
			out_dma_addr = sg_phys(out_sgt->sgl);
		}
		
		dev_info(g2d->dev, "3-buffer operation: out buffer imported (fd=%d addr=0x%llx)\n",
			 blit.out.dma_fd, (u64)out_dma_addr);
	} else {
		/* No explicit out buffer - write to dst (in-place operation) */
		out_dma_addr = dst_dma_addr;
	}
	
	/* Determine operation requirements:
	 * - Alpha blending: Auto-detected from:
	 *   1. Presence of out buffer (3-buffer operation = blending)
	 *   2. Source has alpha format (ARGB) with PIXEL_ALPHA mode
	 *   3. Global alpha mode on src or dst
	 *   4. Legacy ALPHA_BLEND flag
	 * - Rotation/flip: From flags  
	 * - Scaling: When dst size != src crop size
	 * 
	 * Hardware restriction: Cannot do scaling + rotation simultaneously (share VSU)
	 * Solution: If both needed → pre-scale to temp buffer, then rotate
	 * 
	 * Supported single-pass combinations:
	 * - Blending + rotation (no scaling)
	 * - Blending + scaling (via BLD + VSU)
	 * - Scaling only (no blending, no rotation)
	 * - Rotation only (no scaling, no blending)
	 */
	
	/* Auto-detect alpha blending need:
	 * - 3-buffer operation (out.dma_fd >= 0) always needs blending
	 * - ARGB format with PIXEL_ALPHA mode needs blending
	 * - GLOBAL_ALPHA or MIXER_ALPHA modes need blending
	 */
	bool has_out_buffer = (blit.out.dma_fd >= 0);
	bool src_has_alpha_format = (blit.src.format == G2D_FMT_ARGB8888 ||
				     blit.src.format == G2D_FMT_ABGR8888);
	bool needs_alpha = has_out_buffer ||  /* 3-buffer = blending */
			   (src_has_alpha_format && blit.src.alpha_mode == G2D_PIXEL_ALPHA) ||
			   (blit.src.alpha_mode == G2D_GLOBAL_ALPHA) ||
			   (blit.src.alpha_mode == G2D_MIXER_ALPHA) ||
			   (blit.dst.alpha_mode == G2D_GLOBAL_ALPHA) ||
			   (blit.dst.alpha_mode == G2D_MIXER_ALPHA) ||
			   (blit.flags & G2D_BLIT_FLAG_ALPHA_BLEND);  /* Legacy flag */
	
	bool needs_rotation = !!(blit.flags & (G2D_BLIT_FLAG_ROTATE_90 |
					       G2D_BLIT_FLAG_ROTATE_180 |
					       G2D_BLIT_FLAG_ROTATE_270 |
					       G2D_BLIT_FLAG_FLIP_H |
					       G2D_BLIT_FLAG_FLIP_V));
	
	bool needs_scaling = (blit.dst_w != src_crop_w) || (blit.dst_h != src_crop_h);
	
	dev_info(g2d->dev, "BLIT auto-detect: has_out=%d src_alpha_fmt=%d needs_alpha=%d needs_rotation=%d needs_scaling=%d\n",
		 has_out_buffer, src_has_alpha_format, needs_alpha, needs_rotation, needs_scaling);
	
	/* Check for incompatible single-pass combinations */
	if (needs_rotation && needs_scaling) {
		/* TODO: Support alpha blending with scale+rotate (requires 3-pass) */
		if (needs_alpha) {
			dev_warn(g2d->dev, "Scale+rotate+alpha not yet supported (would need 3-pass)\n");
			ret = -ENOSYS;
			goto err_unmap_out;
		}
		
		dev_info(g2d->dev, "Rotation + scaling requested: will do 2-pass (scale then rotate)\n");
		
		/* 2-pass approach:
		 * STEP 1: Scale src to temporary buffer (no rotation, no alpha)
		 * STEP 2: Rotate temp buffer to dst (no scaling, no alpha)
		 * 
		 * This handles the hardware limitation where VSU and ROT
		 * cannot operate simultaneously.
		 */
		
		/* Calculate scaled size (before rotation) */
		u32 temp_w = blit.dst_w;
		u32 temp_h = blit.dst_h;
		u32 temp_pitch = ALIGN(temp_w * dst_bpp, 32);
		size_t temp_size = temp_pitch * temp_h;
		
		/* Allocate temporary buffer using CMA */
		void *temp_vaddr = NULL;
		dma_addr_t temp_dma_addr = 0;
		
		temp_vaddr = dma_alloc_coherent(g2d->dev, temp_size, &temp_dma_addr, GFP_KERNEL);
		if (!temp_vaddr) {
			dev_err(g2d->dev, "Failed to allocate temp buffer for scale+rotate: size=%zu\n", temp_size);
			ret = -ENOMEM;
			goto err_unmap_out;
		}
		
		dev_info(g2d->dev, "Allocated temp buffer: vaddr=%p dma=0x%llx size=%zu\n",
			 temp_vaddr, (u64)temp_dma_addr, temp_size);
		
		/* STEP 1: Scale src → temp (no rotation, no alpha) */
		dev_info(g2d->dev, "STEP 1: SCALE %ux%u -> %ux%u to temp buffer\n",
			 src_crop_w, src_crop_h, temp_w, temp_h);
		
		/* Simple scaling only */
		ret = sunxi_g2d_do_blit(g2d,
					src_dma_addr, blit.src.width, blit.src.height,
					src_pitch, blit.src.format,
					blit.src.crop_x, blit.src.crop_y, src_crop_w, src_crop_h,
					temp_dma_addr, temp_w, temp_h,
					temp_pitch, blit.dst.format,
					0, 0, temp_w, temp_h);  /* Fill entire temp buffer */
		
		if (ret < 0) {
			dev_err(g2d->dev, "STEP 1 (scale) failed: %d\n", ret);
			dma_free_coherent(g2d->dev, temp_size, temp_vaddr, temp_dma_addr);
			goto err_unmap_out;
		}
		
		dev_info(g2d->dev, "✅ Step 1 complete: scaled to temp buffer\n");
		
		/* STEP 2: Rotate temp → out (no scaling) */
		dev_info(g2d->dev, "STEP 2: ROTATE %ux%u -> out at (%u,%u)\n",
			 temp_w, temp_h, blit.dst_x, blit.dst_y);
		
		ret = sunxi_g2d_do_blit_rot(g2d,
					    temp_dma_addr, temp_w, temp_h,
					    temp_pitch, blit.dst.format,
					    0, 0, temp_w, temp_h,  /* Full temp buffer as source */
					    out_dma_addr, blit.dst.width, blit.dst.height,  /* Write to out */
					    dst_pitch, blit.dst.format,
					    blit.dst_x, blit.dst_y, temp_w, temp_h,
					    blit.flags);  /* Apply rotation flags */
		
		/* Free temporary buffer */
		dev_info(g2d->dev, "Freeing temp buffer: vaddr=%p dma=0x%llx size=%zu\n",
			 temp_vaddr, (u64)temp_dma_addr, temp_size);
		dma_free_coherent(g2d->dev, temp_size, temp_vaddr, temp_dma_addr);
		
		if (ret < 0) {
			dev_err(g2d->dev, "STEP 2 (rotate) failed: %d\n", ret);
			goto err_unmap_out;
		}
		
		dev_info(g2d->dev, "✅ 2-pass scale+rotate completed successfully\n");
		
		/* Skip unified path - we already did the work */
		goto done_blit;
	}
	
	/* All other combinations are valid for single-pass:
	 * - alpha + rotation (OK)
	 * - alpha + no_scale (OK)  
	 * - scale only (OK)
	 * - rotation only (OK)
	 * - simple copy (OK)
	 */
	
	/* Execute unified blit operation 
	 * The hardware will automatically:
	 * - Enable VSU if scaling needed
	 * - Enable BLD if alpha blending needed
	 * - Enable ROT if rotation needed
	 * - Do simple copy if none of the above
	 */
	if (needs_alpha || needs_rotation || needs_scaling) {
		char op_desc[128];
		snprintf(op_desc, sizeof(op_desc), "%s%s%s",
			 needs_scaling ? "SCALE" : "",
			 needs_alpha ? (needs_scaling ? "+BLEND" : "BLEND") : "",
			 needs_rotation ? (needs_scaling || needs_alpha ? "+ROT" : "ROT") : "");
		dev_info(g2d->dev, "BLIT operation: %s %ux%u -> %ux%u\n",
			 op_desc, src_crop_w, src_crop_h, blit.dst_w, blit.dst_h);
	} else {
		dev_dbg(g2d->dev, "BLIT: simple copy %ux%u\n", src_crop_w, src_crop_h);
	}
	
	/* Call unified blit function that handles all combinations */
	u32 out_w, out_h, out_pitch, out_format;
	if (has_out_buffer) {
		/* Explicit output buffer - use its dimensions */
		out_w = blit.out.width;
		out_h = blit.out.height;
		out_pitch = blit.out.stride[0] ? blit.out.stride[0] : (blit.out.width * dst_bpp);
		out_format = blit.out.format;
	} else {
		/* In-place operation - output uses dst dimensions */
		out_w = blit.dst.width;
		out_h = blit.dst.height;
		out_pitch = dst_pitch;
		out_format = blit.dst.format;
	}
	
	ret = sunxi_g2d_do_blit_unified(g2d,
					src_dma_addr, blit.src.width, blit.src.height,
					src_pitch, blit.src.format,
					blit.src.crop_x, blit.src.crop_y, src_crop_w, src_crop_h,
					blit.src.alpha, blit.src.alpha_mode, blit.src.premul_mode,
					dst_dma_addr, blit.dst.width, blit.dst.height,
					dst_pitch, blit.dst.format,
					blit.dst_x, blit.dst_y, blit.dst_w, blit.dst_h,
					blit.dst.alpha, blit.dst.alpha_mode, blit.dst.premul_mode,
					out_dma_addr,  /* Explicit output buffer (or dst if none) */
					out_w, out_h, out_pitch, out_format,  /* Output dimensions */
					blit.flags,
					blit.bld_mode,  /* Porter-Duff blend mode */
					blit.color_key_enable, blit.color_key_mode,
					blit.color_key_min, blit.color_key_max,
					needs_alpha, needs_rotation, needs_scaling);
	
	if (ret < 0)
		goto err_unmap_out;

done_blit:
	/* Create job + fence and return fence_fd_out to userspace. */
	{
		struct sunxi_g2d_job *job;
		int out_fd = -1;

		job = kzalloc(sizeof(*job), GFP_KERNEL);
		if (!job) {
			ret = -ENOMEM;
			goto err_unmap_out;
		}

		job->fence = sunxi_g2d_fence_create(g2d);
		if (!job->fence) {
			kfree(job);
			ret = -ENOMEM;
			goto err_unmap_out;
		}

		out_fd = get_unused_fd_flags(O_CLOEXEC);
		if (out_fd < 0) {
			dma_fence_put(job->fence);
			kfree(job);
			ret = out_fd;
			goto err_unmap_out;
		}

		job->fence_fd = out_fd;
		job->sync_file = sync_file_create(job->fence);
		if (!job->sync_file) {
			put_unused_fd(out_fd);
			dma_fence_put(job->fence);
			kfree(job);
			ret = -ENOMEM;
			goto err_unmap_out;
		}

		/* Trace sync_file creation and fd reservation */
		dev_dbg(g2d->dev, "blit: created job=%p sync_file=%p file=%p fence=%p reserved fd=%d\n",
			 job, job->sync_file, job->sync_file ? job->sync_file->file : NULL,
			 job->fence, out_fd);

		/* Install FD into current process now (process context) so IRQ won't
		 * need to touch file-descriptors. After fd_install, the fd table owns
		 * the file ref; clear job->sync_file so cleanup won't fput it.
		 */
		if (job->sync_file && job->sync_file->file) {
			struct file *tmpf = job->sync_file->file;
			/* get seqno from fence for correlation */
			if (job->fence) {
				struct sunxi_g2d_fence *sf = container_of(job->fence, struct sunxi_g2d_fence, base);
				dev_info(g2d->dev, "blit: installing fd=%d file=%p job=%p fence=%p seq=%llu pid=%d\n",
						 out_fd, tmpf, job, job->fence, sf->seqno, task_tgid_nr(current));
			} else {
				dev_info(g2d->dev, "blit: installing fd=%d file=%p job=%p fence=NULL pid=%d\n",
						 out_fd, tmpf, job, task_tgid_nr(current));
			}
			fd_install(out_fd, tmpf);
			/* After fd_install the fd table owns the file ref */
			job->sync_file = NULL;
		} else {
			dev_err(g2d->dev, "blit: unexpected NULL sync_file/file for job=%p fd=%d\n", job, out_fd);
		}

		/* Install as current job so IRQ handler can signal */
		spin_lock(&g2d->job_lock);
		g2d->current_job = job;
		spin_unlock(&g2d->job_lock);

		blit.fence_fd_out = job->fence_fd;
	}
	
	if (copy_to_user((void __user *)arg, &blit, sizeof(blit))) {
		ret = -EFAULT;
		goto err_unmap_out;
	}

err_unmap_out:
	if (out_sgt)
		dma_buf_unmap_attachment(out_attach, out_sgt, DMA_FROM_DEVICE);
err_detach_out:
	if (out_attach)
		dma_buf_detach(out_dmabuf, out_attach);
err_put_out_dmabuf:
	if (out_dmabuf)
		dma_buf_put(out_dmabuf);
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
	
	/* Workaround for T113-S3 without IOMMU: sg_dma_address() may return 0x0
	 * In this case, use physical address directly from the page
	 */
	if (dma_addr == 0 && sgt->nents > 0) {
		struct scatterlist *sg = sgt->sgl;
		struct page *page = sg_page(sg);
		if (page) {
			dma_addr = page_to_phys(page) + sg->offset;
			dev_info(g2d->dev, "T113 workaround: fillrect using physical address 0x%llx\n",
				 (u64)dma_addr);
		} else {
			dev_err(g2d->dev, "Failed to get physical address from page\n");
			ret = -EINVAL;
			goto err_unmap;
		}
	}

	/* If userspace passed a fence_fd_in for this fill, wait on it */
	if (fill.fence_fd_in >= 0) {
		dev_info(g2d->dev, "fillrect: importing input fence fd=%d pid=%d\n",
			 fill.fence_fd_in, task_tgid_nr(current));
		struct dma_fence *in_fence = sync_file_get_fence(fill.fence_fd_in);
		if (!in_fence) {
			ret = -EINVAL;
			goto err_unmap;
		}
		dev_info(g2d->dev, "fillrect: got in_fence=%p signaled=%d\n",
			 in_fence, dma_fence_is_signaled(in_fence));
		dma_fence_wait(in_fence, false);
		dev_info(g2d->dev, "fillrect: in_fence=%p wait done signaled=%d\n",
			 in_fence, dma_fence_is_signaled(in_fence));
		dma_fence_put(in_fence);
	}
	
	/* Calculate bytes per pixel based on format */
	u32 bpp;
	switch (fill.dst.format) {
	case G2D_FMT_ARGB8888:
	case G2D_FMT_ABGR8888:
	case G2D_FMT_RGBA8888:
	case G2D_FMT_BGRA8888:
	case G2D_FMT_XRGB8888:
	case G2D_FMT_XBGR8888:
	case G2D_FMT_RGBX8888:
	case G2D_FMT_BGRX8888:
		bpp = 4;
		break;
	case G2D_FMT_RGB888:
	case G2D_FMT_BGR888:
		bpp = 3;
		break;
	case G2D_FMT_RGB565:
	case G2D_FMT_BGR565:
	case G2D_FMT_ARGB4444:
	case G2D_FMT_ABGR4444:
	case G2D_FMT_RGBA4444:
	case G2D_FMT_BGRA4444:
	case G2D_FMT_ARGB1555:
	case G2D_FMT_ABGR1555:
	case G2D_FMT_RGBA5551:
	case G2D_FMT_BGRA5551:
		bpp = 2;
		break;
	default:
		dev_err(g2d->dev, "Unsupported destination format: %u\n", fill.dst.format);
		ret = -EINVAL;
		goto err_unmap;
	}
	
	/* Rectangle dimensions */
	width = fill.dst_w;
	height = fill.dst_h;
	
	/* Pitch is the stride of the BUFFER, not the rectangle */
	pitch = fill.dst.stride[0] ? fill.dst.stride[0] : (fill.dst.width * bpp);
	
	/* Adjust DMA address for dst_x/dst_y offset */
	dma_addr += (fill.dst_y * pitch) + (fill.dst_x * bpp);
	
	/* v2.8.0: Switch to direct register writes (NO RCQ) for fillrect
	 * Reason: RCQ mode on T113-S3 has issues - MIXER START never auto-clears
	 * BSP also uses direct writes for fillrect, not RCQ
	 */
	ret = sunxi_g2d_do_fillrect(g2d, dma_addr, width, height, pitch, 
				    fill.color, fill.color_format, fill.dst.format);
	
	/* Create job + fence and return fence_fd_out to userspace. */
	{
		struct sunxi_g2d_job *job;
		int out_fd = -1;

		job = kzalloc(sizeof(*job), GFP_KERNEL);
		if (!job) {
			ret = -ENOMEM;
			goto err_unmap;
		}

		job->fence = sunxi_g2d_fence_create(g2d);
		if (!job->fence) {
			kfree(job);
			ret = -ENOMEM;
			goto err_unmap;
		}

		out_fd = get_unused_fd_flags(O_CLOEXEC);
		if (out_fd < 0) {
			dma_fence_put(job->fence);
			kfree(job);
			ret = out_fd;
			goto err_unmap;
		}

		job->fence_fd = out_fd;
		job->sync_file = sync_file_create(job->fence);
		if (!job->sync_file) {
			put_unused_fd(out_fd);
			dma_fence_put(job->fence);
			kfree(job);
			ret = -ENOMEM;
			goto err_unmap;
		}

		/* Trace sync_file creation and fd reservation */
		dev_dbg(g2d->dev, "fillrect: created job=%p sync_file=%p file=%p fence=%p reserved fd=%d\n",
			 job, job->sync_file, job->sync_file ? job->sync_file->file : NULL,
			 job->fence, out_fd);

		/* Install FD into current process now (process context) so IRQ won't
		 * need to touch file-descriptors. After fd_install, the fd table owns
		 * the file ref; clear job->sync_file so cleanup won't fput it.
		 */
		if (job->sync_file && job->sync_file->file) {
			struct file *tmpf = job->sync_file->file;
			if (job->fence) {
				struct sunxi_g2d_fence *sf = container_of(job->fence, struct sunxi_g2d_fence, base);
				dev_info(g2d->dev, "fillrect: installing fd=%d file=%p job=%p fence=%p seq=%llu pid=%d\n",
						 out_fd, tmpf, job, job->fence, sf->seqno, task_tgid_nr(current));
			} else {
				dev_info(g2d->dev, "fillrect: installing fd=%d file=%p job=%p fence=NULL pid=%d\n",
						 out_fd, tmpf, job, task_tgid_nr(current));
			}
			fd_install(out_fd, tmpf);
			/* After fd_install the fd table owns the file ref */
			job->sync_file = NULL;
		} else {
			dev_err(g2d->dev, "fillrect: unexpected NULL sync_file/file for job=%p fd=%d\n", job, out_fd);
		}

		spin_lock(&g2d->job_lock);
		g2d->current_job = job;
		spin_unlock(&g2d->job_lock);

		fill.fence_fd_out = job->fence_fd;
	}
	
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

/*
 * DEPRECATED: sunxi_g2d_ioctl_alpha_blend()
 * This function is no longer used. G2D_IOC_ALPHA_BLEND now redirects to
 * sunxi_g2d_ioctl_blit() in the ioctl switch, which handles alpha blending
 * through the unified BLIT operation with auto-detection.
 * 
 * The entire function is commented out to avoid compilation errors with
 * the removed struct g2d_alpha_blend from the UAPI.
 */
#if 0
static long sunxi_g2d_ioctl_alpha_blend(struct sunxi_g2d_dev *g2d,
					 unsigned long arg)
{
	struct g2d_alpha_blend blend;
	struct dma_buf *src_dmabuf = NULL, *dst_dmabuf = NULL, *out_dmabuf = NULL;
	struct dma_buf_attachment *src_attach = NULL, *dst_attach = NULL, *out_attach = NULL;
	struct sg_table *src_sgt = NULL, *dst_sgt = NULL, *out_sgt = NULL;
	dma_addr_t src_dma_addr, dst_dma_addr, out_dma_addr;
	u32 src_bpp, dst_bpp, out_bpp;
	u32 src_pitch, dst_pitch, out_pitch;
	int ret;
	
	if (copy_from_user(&blend, (void __user *)arg, sizeof(blend)))
		return -EFAULT;
	
	dev_info(g2d->dev, "ALPHA_BLEND IOCTL: src.crop=%ux%u dst.crop=%ux%u out.crop=%ux%u\n",
		 blend.src.crop_w, blend.src.crop_h,
		 blend.dst.crop_w, blend.dst.crop_h,
		 blend.out.crop_w, blend.out.crop_h);
	
	dev_info(g2d->dev, "ALPHA_BLEND: dst.crop_x=%u dst.crop_y=%u dst.crop_w=%u dst.crop_h=%u\n",
		 blend.dst.crop_x, blend.dst.crop_y, blend.dst.crop_w, blend.dst.crop_h);
	
	/* Validate parameters */
	if (blend.src.crop_w == 0 || blend.src.crop_h == 0 ||
	    blend.dst.crop_w == 0 || blend.dst.crop_h == 0 ||
	    blend.out.crop_w == 0 || blend.out.crop_h == 0) {
		dev_err(g2d->dev, "Invalid dimensions\n");
		return -EINVAL;
	}
	
	/* For now, only support DMA-BUF */
	if (blend.src.dma_fd < 0 || blend.dst.dma_fd < 0 || blend.out.dma_fd < 0) {
		dev_err(g2d->dev, "Physical address mode not supported (need 3 dma_fds)\n");
		return -EINVAL;
	}
	
	dev_info(g2d->dev, "ALPHA_BLEND (3-buffer): src=%dx%d (alpha=%u mode=%u) dst=%dx%d (alpha=%u mode=%u) out=%dx%d\n",
		 blend.src.crop_w, blend.src.crop_h, blend.src.alpha, blend.src.alpha_mode,
		 blend.dst.crop_w, blend.dst.crop_h, blend.dst.alpha, blend.dst.alpha_mode,
		 blend.out.crop_w, blend.out.crop_h);
	
	/* Import source DMA-BUF */
	src_dmabuf = dma_buf_get(blend.src.dma_fd);
	if (IS_ERR(src_dmabuf)) {
		ret = PTR_ERR(src_dmabuf);
		dev_err(g2d->dev, "Failed to get src dma_buf: %d\n", ret);
		return ret;
	}
	
	dev_info(g2d->dev, "src_dmabuf: size=%zu\n", src_dmabuf->size);
	
	src_attach = dma_buf_attach(src_dmabuf, g2d->dev);
	if (IS_ERR(src_attach)) {
		ret = PTR_ERR(src_attach);
		dev_err(g2d->dev, "Failed to attach src: %d\n", ret);
		goto err_put_src;
	}
	
	dev_info(g2d->dev, "src attached, mapping with DMA_TO_DEVICE...\n");
	
	src_sgt = dma_buf_map_attachment(src_attach, DMA_TO_DEVICE);
	if (IS_ERR(src_sgt)) {
		ret = PTR_ERR(src_sgt);
		dev_err(g2d->dev, "Failed to map src: %d\n", ret);
		goto err_detach_src;
	}
	
	dev_info(g2d->dev, "src mapped: nents=%u orig_nents=%u\n",
		 src_sgt->nents, src_sgt->orig_nents);
	
	/* Import destination DMA-BUF */
	dst_dmabuf = dma_buf_get(blend.dst.dma_fd);
	if (IS_ERR(dst_dmabuf)) {
		ret = PTR_ERR(dst_dmabuf);
		goto err_unmap_src;
	}
	
	dst_attach = dma_buf_attach(dst_dmabuf, g2d->dev);
	if (IS_ERR(dst_attach)) {
		ret = PTR_ERR(dst_attach);
		goto err_put_dst;
	}
	
	/* Import output DMA-BUF (writeback destination) first to detect same buffer */
	out_dmabuf = dma_buf_get(blend.out.dma_fd);
	if (IS_ERR(out_dmabuf)) {
		ret = PTR_ERR(out_dmabuf);
		dev_err(g2d->dev, "Failed to get out dma_buf: %d\n", ret);
		goto err_detach_dst;
	}
	
	/* Check if dst and out are the same buffer (in-place blending)
	 * If so, we need DMA_BIDIRECTIONAL for proper cache coherency */
	bool same_buffer = (dst_dmabuf == out_dmabuf);
	enum dma_data_direction dst_dir = same_buffer ? DMA_BIDIRECTIONAL : DMA_TO_DEVICE;
	enum dma_data_direction out_dir = same_buffer ? DMA_BIDIRECTIONAL : DMA_FROM_DEVICE;
	
	dev_info(g2d->dev, "dst/out same buffer: %s (using %s)\n",
		 same_buffer ? "YES" : "NO",
		 same_buffer ? "DMA_BIDIRECTIONAL" : "DMA_TO/FROM_DEVICE");
	
	dst_sgt = dma_buf_map_attachment(dst_attach, dst_dir);
	if (IS_ERR(dst_sgt)) {
		ret = PTR_ERR(dst_sgt);
		goto err_put_out;
	}
	
	dev_info(g2d->dev, "out_dmabuf: size=%zu\n", out_dmabuf->size);
	
	out_attach = dma_buf_attach(out_dmabuf, g2d->dev);
	if (IS_ERR(out_attach)) {
		ret = PTR_ERR(out_attach);
		dev_err(g2d->dev, "Failed to attach out: %d\n", ret);
		goto err_unmap_dst;
	}
	
	dev_info(g2d->dev, "out attached, mapping with direction %s...\n",
		 same_buffer ? "DMA_BIDIRECTIONAL" : "DMA_FROM_DEVICE");
	
	out_sgt = dma_buf_map_attachment(out_attach, out_dir);
	if (IS_ERR(out_sgt)) {
		ret = PTR_ERR(out_sgt);
		dev_err(g2d->dev, "Failed to map out: %d\n", ret);
		goto err_detach_out;
	}
	
	dev_info(g2d->dev, "out mapped: nents=%u orig_nents=%u\n",
		 out_sgt->nents, out_sgt->orig_nents);
	
	/* Get DMA addresses */
	src_dma_addr = sg_dma_address(src_sgt->sgl);
	dst_dma_addr = sg_dma_address(dst_sgt->sgl);
	out_dma_addr = sg_dma_address(out_sgt->sgl);
	
	/* Workaround for T113-S3 without IOMMU: sg_dma_address() may return 0x0
	 * In this case, use physical address directly from the page
	 */
	if (src_dma_addr == 0 && src_sgt->nents > 0) {
		struct scatterlist *sg = src_sgt->sgl;
		struct page *page = sg_page(sg);
		if (page) {
			src_dma_addr = page_to_phys(page) + sg->offset;
			dev_info(g2d->dev, "T113 workaround: src using physical address 0x%llx\n",
				 (u64)src_dma_addr);
		}
	}
	
	if (dst_dma_addr == 0 && dst_sgt->nents > 0) {
		struct scatterlist *sg = dst_sgt->sgl;
		struct page *page = sg_page(sg);
		if (page) {
			dst_dma_addr = page_to_phys(page) + sg->offset;
			dev_info(g2d->dev, "T113 workaround: dst using physical address 0x%llx\n",
				 (u64)dst_dma_addr);
		}
	}
	
	if (out_dma_addr == 0 && out_sgt->nents > 0) {
		struct scatterlist *sg = out_sgt->sgl;
		struct page *page = sg_page(sg);
		if (page) {
			out_dma_addr = page_to_phys(page) + sg->offset;
			dev_info(g2d->dev, "T113 workaround: out using physical address 0x%llx\n",
				 (u64)out_dma_addr);
		}
	}

	/* If userspace provided an input fence fd for this alpha blend, wait on it */
	if (blend.fence_fd_in >= 0) {
		dev_info(g2d->dev, "alpha_blend: importing input fence fd=%d pid=%d\n",
			 blend.fence_fd_in, task_tgid_nr(current));
		struct dma_fence *in_fence = sync_file_get_fence(blend.fence_fd_in);
		if (!in_fence) {
			ret = -EINVAL;
			goto err_unmap_out;  /* FIX: was err_unmap_dst, leaked out! */
		}
		dev_info(g2d->dev, "alpha_blend: got in_fence=%p signaled=%d\n",
			 in_fence, dma_fence_is_signaled(in_fence));
		dma_fence_wait(in_fence, false);
		dev_info(g2d->dev, "alpha_blend: in_fence=%p wait done signaled=%d\n",
			 in_fence, dma_fence_is_signaled(in_fence));
		dma_fence_put(in_fence);
	}
	
	dev_info(g2d->dev, "DMA addresses: src_fd=%d->0x%llx dst_fd=%d->0x%llx out_fd=%d->0x%llx\n",
		 blend.src.dma_fd, (u64)src_dma_addr,
		 blend.dst.dma_fd, (u64)dst_dma_addr,
		 blend.out.dma_fd, (u64)out_dma_addr);
	
	/* Calculate bytes per pixel */
	switch (blend.src.format) {
	case G2D_FMT_ARGB8888:
	case G2D_FMT_XRGB8888:
		src_bpp = 4;
		break;
	case G2D_FMT_RGB565:
		src_bpp = 2;
		break;
	default:
		ret = -EINVAL;
		goto err_unmap_out;
	}
	
	switch (blend.dst.format) {
	case G2D_FMT_ARGB8888:
	case G2D_FMT_XRGB8888:
		dst_bpp = 4;
		break;
	case G2D_FMT_RGB565:
		dst_bpp = 2;
		break;
	default:
		ret = -EINVAL;
		goto err_unmap_out;
	}
	
	switch (blend.out.format) {
	case G2D_FMT_ARGB8888:
	case G2D_FMT_XRGB8888:
		out_bpp = 4;
		break;
	case G2D_FMT_RGB565:
		out_bpp = 2;
		break;
	default:
		ret = -EINVAL;
		goto err_unmap_out;
	}
	
	/* Adjust DMA addresses for crop offsets */
	src_pitch = blend.src.stride[0] ? blend.src.stride[0] : (blend.src.width * src_bpp);
	dst_pitch = blend.dst.stride[0] ? blend.dst.stride[0] : (blend.dst.width * dst_bpp);
	out_pitch = blend.out.stride[0] ? blend.out.stride[0] : (blend.out.width * out_bpp);
	
	dev_info(g2d->dev, "ALPHA_BLEND PITCH: src_pitch=%u (stride=%u width=%u bpp=%u) crop=(%u,%u,%u,%u)\n",
		 src_pitch, blend.src.stride[0], blend.src.width, src_bpp,
		 blend.src.crop_x, blend.src.crop_y, blend.src.crop_w, blend.src.crop_h);
	
	/* Save base addresses BEFORE applying crop offsets */
	dma_addr_t src_base_addr = src_dma_addr;
	dma_addr_t dst_base_addr = dst_dma_addr;
	dma_addr_t out_base_addr = out_dma_addr;
	
	/* Apply crop offsets */
	src_dma_addr += (blend.src.crop_y * src_pitch) + (blend.src.crop_x * src_bpp);
	dst_dma_addr += (blend.dst.crop_y * dst_pitch) + (blend.dst.crop_x * dst_bpp);
	out_dma_addr += (blend.out.crop_y * out_pitch) + (blend.out.crop_x * out_bpp);
	
	dev_info(g2d->dev, "CROP APPLIED: dst crop=(%u,%u) bpp=%u pitch=%u offset=0x%llx base=0x%llx\n",
		 blend.dst.crop_x, blend.dst.crop_y, dst_bpp, dst_pitch,
		 (u64)(dst_dma_addr - dst_base_addr), (u64)dst_base_addr);
	
	dev_info(g2d->dev, "ALPHA_BLEND CALL PARAMS: src.crop=%ux%u dst.crop=%ux%u out.crop=%ux%u\n",
		 blend.src.crop_w, blend.src.crop_h,
		 blend.dst.crop_w, blend.dst.crop_h,
		 blend.out.crop_w, blend.out.crop_h);
	
	/* v2.9.16: 3-buffer alpha blending (NO read/write conflict)
	 * - src (V0): ball buffer - READ only
	 * - dst (UI2): background buffer - READ only  
	 * - out (WB): temp buffer - WRITE only
	 * This avoids G2D reading and writing the same buffer simultaneously
	 */
	ret = sunxi_g2d_do_blit_alpha_3buf(g2d,
				       src_dma_addr, blend.src.width, blend.src.height,
				       src_pitch, blend.src.format,
				       0, 0, blend.src.crop_w, blend.src.crop_h,
				       blend.src.alpha, blend.src.alpha_mode, blend.src.premul_mode,
				       dst_dma_addr, dst_base_addr, blend.dst.width, blend.dst.height,
				       dst_pitch, blend.dst.format,
				       0, 0, blend.dst.crop_w, blend.dst.crop_h,
				       blend.dst.alpha, blend.dst.alpha_mode, blend.dst.premul_mode,
				       out_dma_addr, blend.out.width, blend.out.height,
				       out_pitch, blend.out.format,
				       blend.out.crop_w, blend.out.crop_h,
				       blend.bld_mode,
				       0, 0, 0, 0);  /* No chromakey for RCQ blend */
	
	
	/* Create job + fence and return fence_fd_out to userspace. */
	{
		struct sunxi_g2d_job *job;
		int out_fd = -1;

		job = kzalloc(sizeof(*job), GFP_KERNEL);
		if (!job) {
			ret = -ENOMEM;
			goto err_unmap_dst;
		}

		job->fence = sunxi_g2d_fence_create(g2d);
		if (!job->fence) {
			kfree(job);
			ret = -ENOMEM;
			goto err_unmap_dst;
		}

		out_fd = get_unused_fd_flags(O_CLOEXEC);
		if (out_fd < 0) {
			dma_fence_put(job->fence);
			kfree(job);
			ret = out_fd;
			goto err_unmap_dst;
		}

		job->fence_fd = out_fd;
		job->sync_file = sync_file_create(job->fence);
		if (!job->sync_file) {
			put_unused_fd(out_fd);
			dma_fence_put(job->fence);
			kfree(job);
			ret = -ENOMEM;
			goto err_unmap_dst;
		}

		/* Trace sync_file creation and fd reservation */
		dev_dbg(g2d->dev, "alpha_blend: created job=%p sync_file=%p file=%p fence=%p reserved fd=%d\n",
			 job, job->sync_file, job->sync_file ? job->sync_file->file : NULL,
			 job->fence, out_fd);

		/* Install FD into current process now (process context) so IRQ won't
		 * need to touch file-descriptors. After fd_install, the fd table owns
		 * the file ref; clear job->sync_file so cleanup won't fput it.
		 */
		if (job->sync_file && job->sync_file->file) {
			struct file *tmpf = job->sync_file->file;
			if (job->fence) {
				struct sunxi_g2d_fence *sf = container_of(job->fence, struct sunxi_g2d_fence, base);
				dev_info(g2d->dev, "alpha_blend: installing fd=%d file=%p job=%p fence=%p seq=%llu pid=%d\n",
						 out_fd, tmpf, job, job->fence, sf->seqno, task_tgid_nr(current));
			} else {
				dev_info(g2d->dev, "alpha_blend: installing fd=%d file=%p job=%p fence=NULL pid=%d\n",
						 out_fd, tmpf, job, task_tgid_nr(current));
			}
			fd_install(out_fd, tmpf);
			/* After fd_install the fd table owns the file ref */
			job->sync_file = NULL;
		} else {
			dev_err(g2d->dev, "alpha_blend: unexpected NULL sync_file/file for job=%p fd=%d\n", job, out_fd);
		}

		spin_lock(&g2d->job_lock);
		g2d->current_job = job;
		spin_unlock(&g2d->job_lock);

		blend.fence_fd_out = job->fence_fd;
	}

	if (copy_to_user((void __user *)arg, &blend, sizeof(blend)))
		ret = -EFAULT;
	
err_unmap_out:
	dma_buf_unmap_attachment(out_attach, out_sgt, out_dir);
err_detach_out:
	dma_buf_detach(out_dmabuf, out_attach);
err_unmap_dst:
	dma_buf_unmap_attachment(dst_attach, dst_sgt, dst_dir);
err_detach_dst:
	dma_buf_detach(dst_dmabuf, dst_attach);
err_put_out:
	dma_buf_put(out_dmabuf);
err_put_dst:
	dma_buf_put(dst_dmabuf);
err_unmap_src:
	dma_buf_unmap_attachment(src_attach, src_sgt, DMA_TO_DEVICE);
err_detach_src:
	dma_buf_detach(src_dmabuf, src_attach);
err_put_src:
	dma_buf_put(src_dmabuf);
	
	return ret;
}
#endif /* Deprecated sunxi_g2d_ioctl_alpha_blend */

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
	case G2D_IOC_FILLRECT_RCQ:
		/* Deprecated: FILLRECT and FILLRECT_RCQ now use same path */
		return sunxi_g2d_ioctl_fillrect(g2d, arg);
	case G2D_IOC_ALPHA_BLEND:
		/* Deprecated: ALPHA_BLEND is now handled by unified BLIT */
		dev_info_once(g2d->dev, "G2D_IOC_ALPHA_BLEND is deprecated, use G2D_IOC_BLIT instead\n");
		return sunxi_g2d_ioctl_blit(g2d, arg);
	case G2D_IOC_ALLOC_BUFFER:
		return sunxi_g2d_ioctl_alloc_buffer(g2d, arg);
	case G2D_IOC_SELFTEST_FENCE: {
		/* Userspace requests a kernel-created fence that will be signalled
		 * after a timeout (milliseconds) provided in a struct g2d_selftest
		 * passed by pointer. This helps isolate fence lifecycle without
		 * touching hardware paths.
		 */
		struct g2d_selftest st;
		int timeout_ms;
		struct dma_fence *f;
		struct sync_file *sf;
		int out_fd;
		struct g2d_selftest_work *w;

		if (copy_from_user(&st, (void __user *)arg, sizeof(st)))
			return -EFAULT;

		timeout_ms = st.timeout_ms;
		if (timeout_ms < 0)
			return -EINVAL;

		/* create fence */
		f = sunxi_g2d_fence_create(g2d);
		if (!f)
			return -ENOMEM;

		/* create sync_file and return fd to userspace (install in process ctx)
		 */
		sf = sync_file_create(f);
		if (!sf) {
			dma_fence_put(f);
			return -ENOMEM;
		}

		out_fd = get_unused_fd_flags(O_CLOEXEC);
		if (out_fd < 0) {
			/* cleanup sync_file: drop file ref and free struct since we won't
			 * install it into an fd
			 */
			fput(sf->file);
			kfree(sf);
			dma_fence_put(f);
			return out_fd;
		}

	dev_info(g2d->dev, "selftest: installing fd=%d sync_file=%p file=%p fence=%p pid=%d\n",
		out_fd, sf, sf->file, f, task_tgid_nr(current));
	fd_install(out_fd, sf->file);
	/* drop our sync_file reference (fd now holds a ref to file)
	 * keep fence ref for the scheduled work: bump it.
	 */
		/* sync_file struct and its file are now owned by the fd table; do
		 * not free them here. The file will be released when userspace closes
		 * the returned fd. */
		dma_fence_get(f);

		/* schedule delayed work to signal the fence */
		w = kzalloc(sizeof(*w), GFP_KERNEL);
		if (!w) {
			put_unused_fd(out_fd);
			dma_fence_put(f);
			return -ENOMEM;
		}
		w->fence = f;
		INIT_DELAYED_WORK(&w->dwork, g2d_selftest_work_func);
		queue_delayed_work(g2d->job_wq, &w->dwork, msecs_to_jiffies(timeout_ms));

		st.fence_fd = out_fd;
		if (copy_to_user((void __user *)arg, &st, sizeof(st))) {
			/* user failed to receive fd; cleanup: cancel work if possible */
			/* best-effort: leave work to signal and cleanup; close fd */
			put_unused_fd(out_fd);
			return -EFAULT;
		}

		return 0;
	}
	case G2D_IOC_SYNC: {
		int user_fd;
		struct dma_fence *in_fence;

		if (copy_from_user(&user_fd, (void __user *)arg, sizeof(user_fd)))
			return -EFAULT;

		if (user_fd < 0)
			return -EINVAL;

		dev_info(g2d->dev, "sync ioctl: importing fence fd=%d pid=%d\n",
			 user_fd, task_tgid_nr(current));
		in_fence = sync_file_get_fence(user_fd);
		if (!in_fence)
			return -EINVAL;
		dev_info(g2d->dev, "sync ioctl: got in_fence=%p signaled=%d\n",
			 in_fence, dma_fence_is_signaled(in_fence));

		/* Wait uninterruptibly for the fence to signal */
		dma_fence_wait(in_fence, false);
		dev_info(g2d->dev, "sync ioctl: in_fence=%p wait done signaled=%d\n",
			 in_fence, dma_fence_is_signaled(in_fence));
		dma_fence_put(in_fence);
		return 0;
	}
	case G2D_IOC_WRITE_BUFFER: {
		struct g2d_buffer_rw rw;
		struct dma_buf *dmabuf;
		struct dma_buf_attachment *attach;
		struct sg_table *sgt;
		struct iosys_map map;
		void *vaddr;
		int ret;

		if (copy_from_user(&rw, (void __user *)arg, sizeof(rw)))
			return -EFAULT;

		/* Validate parameters */
		if (rw.dma_fd < 0 || rw.size == 0 || !rw.user_ptr)
			return -EINVAL;

		dmabuf = dma_buf_get(rw.dma_fd);
		if (IS_ERR(dmabuf))
			return PTR_ERR(dmabuf);

		/* Check bounds */
		if (rw.offset + rw.size > dmabuf->size) {
			dma_buf_put(dmabuf);
			return -EINVAL;
		}

		attach = dma_buf_attach(dmabuf, g2d->dev);
		if (IS_ERR(attach)) {
			ret = PTR_ERR(attach);
			dma_buf_put(dmabuf);
			return ret;
		}

		sgt = dma_buf_map_attachment(attach, DMA_TO_DEVICE);
		if (IS_ERR(sgt)) {
			ret = PTR_ERR(sgt);
			dma_buf_detach(dmabuf, attach);
			dma_buf_put(dmabuf);
			return ret;
		}

		/* Map for CPU access */
		ret = dma_buf_begin_cpu_access(dmabuf, DMA_TO_DEVICE);
		if (ret) {
			dma_buf_unmap_attachment(attach, sgt, DMA_TO_DEVICE);
			dma_buf_detach(dmabuf, attach);
			dma_buf_put(dmabuf);
			return ret;
		}

		ret = dma_buf_vmap(dmabuf, &map);
		if (ret) {
			dma_buf_end_cpu_access(dmabuf, DMA_TO_DEVICE);
			dma_buf_unmap_attachment(attach, sgt, DMA_TO_DEVICE);
			dma_buf_detach(dmabuf, attach);
			dma_buf_put(dmabuf);
			return ret;
		}

		vaddr = map.vaddr;
		if (!vaddr) {
			dma_buf_vunmap(dmabuf, &map);
			dma_buf_end_cpu_access(dmabuf, DMA_TO_DEVICE);
			dma_buf_unmap_attachment(attach, sgt, DMA_TO_DEVICE);
			dma_buf_detach(dmabuf, attach);
			dma_buf_put(dmabuf);
			return -ENOMEM;
		}

		/* Copy from userspace to device buffer */
		if (copy_from_user(vaddr + rw.offset, (void __user *)rw.user_ptr, rw.size)) {
			ret = -EFAULT;
		} else {
			ret = 0;
		}

		dma_buf_vunmap(dmabuf, &map);
		dma_buf_end_cpu_access(dmabuf, DMA_TO_DEVICE);
		dma_buf_unmap_attachment(attach, sgt, DMA_TO_DEVICE);
		dma_buf_detach(dmabuf, attach);
		dma_buf_put(dmabuf);

		dev_info(g2d->dev, "WRITE_BUFFER: fd=%d offset=%llu size=%llu ret=%d\n",
			 rw.dma_fd, rw.offset, rw.size, ret);

		return ret;
	}
	case G2D_IOC_READ_BUFFER: {
		struct g2d_buffer_rw rw;
		struct dma_buf *dmabuf;
		struct dma_buf_attachment *attach;
		struct sg_table *sgt;
		struct iosys_map map;
		void *vaddr;
		int ret;

		if (copy_from_user(&rw, (void __user *)arg, sizeof(rw)))
			return -EFAULT;

		/* Validate parameters */
		if (rw.dma_fd < 0 || rw.size == 0 || !rw.user_ptr)
			return -EINVAL;

		dmabuf = dma_buf_get(rw.dma_fd);
		if (IS_ERR(dmabuf))
			return PTR_ERR(dmabuf);

		/* Check bounds */
		if (rw.offset + rw.size > dmabuf->size) {
			dma_buf_put(dmabuf);
			return -EINVAL;
		}

		attach = dma_buf_attach(dmabuf, g2d->dev);
		if (IS_ERR(attach)) {
			ret = PTR_ERR(attach);
			dma_buf_put(dmabuf);
			return ret;
		}

		sgt = dma_buf_map_attachment(attach, DMA_FROM_DEVICE);
		if (IS_ERR(sgt)) {
			ret = PTR_ERR(sgt);
			dma_buf_detach(dmabuf, attach);
			dma_buf_put(dmabuf);
			return ret;
		}

		/* Map for CPU access */
		ret = dma_buf_begin_cpu_access(dmabuf, DMA_FROM_DEVICE);
		if (ret) {
			dma_buf_unmap_attachment(attach, sgt, DMA_FROM_DEVICE);
			dma_buf_detach(dmabuf, attach);
			dma_buf_put(dmabuf);
			return ret;
		}

		ret = dma_buf_vmap(dmabuf, &map);
		if (ret) {
			dma_buf_end_cpu_access(dmabuf, DMA_FROM_DEVICE);
			dma_buf_unmap_attachment(attach, sgt, DMA_FROM_DEVICE);
			dma_buf_detach(dmabuf, attach);
			dma_buf_put(dmabuf);
			return ret;
		}

		vaddr = map.vaddr;
		if (!vaddr) {
			dma_buf_vunmap(dmabuf, &map);
			dma_buf_end_cpu_access(dmabuf, DMA_FROM_DEVICE);
			dma_buf_unmap_attachment(attach, sgt, DMA_FROM_DEVICE);
			dma_buf_detach(dmabuf, attach);
			dma_buf_put(dmabuf);
			return -ENOMEM;
		}

		/* Copy to userspace from device buffer */
		if (copy_to_user((void __user *)rw.user_ptr, vaddr + rw.offset, rw.size)) {
			ret = -EFAULT;
		} else {
			ret = 0;
		}

		dma_buf_vunmap(dmabuf, &map);
		dma_buf_end_cpu_access(dmabuf, DMA_FROM_DEVICE);
		dma_buf_unmap_attachment(attach, sgt, DMA_FROM_DEVICE);
		dma_buf_detach(dmabuf, attach);
		dma_buf_put(dmabuf);

		dev_info(g2d->dev, "READ_BUFFER: fd=%d offset=%llu size=%llu ret=%d\n",
			 rw.dma_fd, rw.offset, rw.size, ret);

		return ret;
	}
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

	/* Initialize fence context and lock */
	spin_lock_init(&g2d->fence_lock);
	atomic64_set(&g2d->fence_seqno, 0);
	g2d->fence_context = dma_fence_context_alloc(1);
	
	/* Initialize temporary scaling buffer fields */
	g2d->temp_scale_buffer = NULL;
	g2d->temp_scale_dma = 0;
	g2d->temp_scale_size = 0;
	
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

	/* Initialize fence context/sequence for dma-fence support */
	g2d->fence_context = dma_fence_context_alloc(1);
	atomic64_set(&g2d->fence_seqno, 0);
	
	/* RCQ (Register Configuration Queue) - DESHABILITADO
	 * 
	 * RCQ no funciona en T113-S3 G2D IP 0x0110 (ver RCQ-INVESTIGATION-REPORT.md)
	 * Síntoma: RCQ procesa comandos pero MIXER nunca ejecuta (task_end_irq timeout)
	 * 
	 * Modo legacy (escritura directa de registros) funciona perfectamente.
	 * RCQ queda disponible para investigación futura con mejor documentación.
	 */
#if 0  /* RCQ DISABLED - NO FUNCIONAL */
	ret = sunxi_g2d_rcq_alloc(g2d->dev, &g2d->rcq, G2D_RCQ_MAX_SIZE);
	if (ret) {
		dev_warn(&pdev->dev, "Failed to allocate RCQ buffer: %d, RCQ disabled\n", ret);
		g2d->rcq_enabled = false;
	} else {
		g2d->rcq_enabled = true;
		dev_info(&pdev->dev, "RCQ enabled with %u byte buffer\n", g2d->rcq.size);
	}
#else
	g2d->rcq_enabled = false;
	dev_info(&pdev->dev, "RCQ disabled (not functional on T113-S3), using legacy mode\n");
#endif
	
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
	
	/* Free RCQ buffer */
	if (g2d->rcq_enabled)
		sunxi_g2d_rcq_free(g2d->dev, &g2d->rcq);
	
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
