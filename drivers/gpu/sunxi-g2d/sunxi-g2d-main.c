// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 Sergio Perez
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
#include <linux/iommu.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/sync_file.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/iopoll.h>
#include <linux/pm_runtime.h>
#include <linux/interconnect.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/iosys-map.h>
#include <linux/sizes.h>
#include <uapi/linux/sunxi_g2d.h>
#include <linux/vmalloc.h>
#include "sunxi-g2d-regs.h"
#include "sunxi-g2d-structs.h"
#include "sunxi-g2d-scaler-coeffs.h"
#include "sunxi-g2d-rcq.h"
#include "sunxi-g2d-csc-tables.h"

#define DRIVER_NAME "sunxi-g2d"
#define DRIVER_VERSION "2.9.16"
#define DRIVER_MAJOR 2
#define DRIVER_MINOR 9
#define DRIVER_PATCHLEVEL 16

/* Module parameters */
/* Nivel de depuración RCQ: 0=off, 1=volcado completo headers+datos en fillrect */
static int g2d_rcq_debug;
module_param(g2d_rcq_debug, int, 0644);
MODULE_PARM_DESC(
	g2d_rcq_debug,
	"RCQ debug level: 0=off (default), 1=headers+data dump for fillrect RCQ path");

/* Allocation mode: 0=CMA (fallback to 1), 1=System Pages (IOMMU), 2=Coherent (CMA) */
static int g2d_alloc_mode = 1;
module_param(g2d_alloc_mode, int, 0644);
MODULE_PARM_DESC(g2d_alloc_mode, "Allocation mode: 0=CMA, 1=System (IOMMU), 2=Coherent");

/* Tamaño del bloque BLD cuando imitamos el RCQ del BSP (bytes, incluye ROP). */
#define G2D_BSP_BLD_BLOCK_SIZE 0x88

/* Device limits */
#define G2D_MAX_WIDTH 2048
#define G2D_MAX_HEIGHT 2048
#define G2D_MIN_WIDTH 2
#define G2D_MIN_HEIGHT 2

/* Job queue limits */
#define G2D_MAX_JOBS 64

/* Forward declarations */
struct sunxi_g2d_dev;
struct sunxi_g2d_job;
struct sunxi_g2d_ctx;

/* Forward declarations for internal functions used before definition */
static int sunxi_g2d_format_to_hw(u32 fmt, u32 *bpp);
static void sunxi_g2d_get_yuv_plane_info(u32 fmt, u32 width, u32 height,
					 const u32 *user_stride, u32 *stride,
					 u32 *plane_offset);

static struct dma_fence *sunxi_g2d_fence_create(struct sunxi_g2d_dev *g2d);
static void sunxi_g2d_job_cleanup_workfn(struct work_struct *work);

static int sunxi_g2d_do_blit_rcq(struct sunxi_g2d_dev *g2d,
				 struct g2d_rcq_mem *rcq, dma_addr_t src_dma,
				 u32 src_width, u32 src_height, u32 src_pitch,
				 u32 src_format, u32 src_x, u32 src_y,
				 u32 src_crop_w, u32 src_crop_h,
				 dma_addr_t dst_dma, u32 dst_width,
				 u32 dst_height, u32 dst_pitch, u32 dst_format,
				 u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h,
				 u8 src_color_space, u8 dst_color_space);

static int sunxi_g2d_do_fillrect_rcq(struct sunxi_g2d_dev *g2d,
				     struct g2d_rcq_mem *rcq,
				     dma_addr_t dst_dma, u32 width, u32 height,
				     u32 pitch, u32 color, u32 color_format,
				     u32 dst_format);

static int sunxi_g2d_do_scale_rcq(struct sunxi_g2d_dev *g2d,
				  struct g2d_rcq_mem *rcq, dma_addr_t src_dma,
				  u32 src_width, u32 src_height, u32 src_pitch,
				  u32 src_format, u32 src_x, u32 src_y,
				  u32 src_crop_w, u32 src_crop_h,
				  dma_addr_t dst_dma, u32 dst_width,
				  u32 dst_height, u32 dst_pitch, u32 dst_format,
				  u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h,
				  int src_colorspace, int dst_colorspace,
				  struct g2d_csc_state *csc_state);

static int sunxi_g2d_do_blend_rcq(
	struct sunxi_g2d_dev *g2d, struct g2d_rcq_mem *rcq, dma_addr_t src_dma,
	u32 src_width, u32 src_height, u32 src_pitch, u32 src_format, u32 src_x,
	u32 src_y, u32 src_crop_w, u32 src_crop_h, dma_addr_t dst_dma,
	u32 dst_width, u32 dst_height, u32 dst_pitch, u32 dst_format, u32 dst_x,
	u32 dst_y, dma_addr_t out_dma, u32 out_width, u32 out_height,
	u32 out_pitch, u32 out_format, u32 out_x, u32 out_y, u32 blend_w,
	u32 blend_h, u32 bld_mode, u32 alpha_mode, u32 global_alpha,
	u32 premul_mode, u8 src_color_space, u8 dst_color_space,
	u8 out_color_space, struct g2d_csc_state *csc_state);

static int sunxi_g2d_do_blit_rot_rcq(
	struct sunxi_g2d_dev *g2d, struct g2d_rcq_mem *rcq, dma_addr_t src_dma,
	u32 src_width, u32 src_height, u32 src_pitch, u32 src_format,
	u32 src_crop_x, u32 src_crop_y, u32 src_crop_w, u32 src_crop_h,
	dma_addr_t dst_dma, u32 dst_width, u32 dst_height, u32 dst_pitch,
	u32 dst_format, u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h, u32 flags);

static int sunxi_g2d_execute_rcq(struct sunxi_g2d_dev *g2d,
				 struct g2d_rcq_mem *rcq);

static inline bool sunxi_g2d_is_yuv_planar(u32 fmt);
static inline bool sunxi_g2d_is_yuv_semiplanar(u32 fmt);

static inline void sunxi_g2d_uv_div_factors(u32 fmt, u32 *x_div, u32 *y_div)
{
	*x_div = 1;
	*y_div = 1;

	if (!sunxi_g2d_is_yuv_planar(fmt) &&
	    !sunxi_g2d_is_yuv_semiplanar(fmt))
		return;

	if ((fmt >= G2D_FMT_YUV411_SP_UVUV && fmt <= G2D_FMT_YUV411_SP_VUVU) ||
	    fmt == G2D_FMT_YUV411_P) {
		*x_div = 4;
		*y_div = 1;
	} else if ((fmt >= G2D_FMT_YUV422_SP_UVUV &&
		    fmt <= G2D_FMT_YUV422_SP_VUVU) ||
		   fmt == G2D_FMT_YUV422_P) {
		*x_div = 2;
		*y_div = 1;
	} else {
		/* Default to 4:2:0 */
		*x_div = 2;
		*y_div = 2;
	}
}

static int sunxi_g2d_do_blit_rot(
	struct sunxi_g2d_dev *g2d, struct g2d_rcq_mem *rcq, dma_addr_t src_dma,
	u32 src_width, u32 src_height, u32 src_pitch, u32 src_format,
	u32 src_crop_x, u32 src_crop_y, u32 src_crop_w, u32 src_crop_h,
	dma_addr_t dst_dma, u32 dst_width, u32 dst_height, u32 dst_pitch,
	u32 dst_format, u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h, u32 flags);

static int sunxi_g2d_do_blit_rot_rcq(
	struct sunxi_g2d_dev *g2d, struct g2d_rcq_mem *rcq, dma_addr_t src_dma,
	u32 src_width, u32 src_height, u32 src_pitch, u32 src_format,
	u32 src_crop_x, u32 src_crop_y, u32 src_crop_w, u32 src_crop_h,
	dma_addr_t dst_dma, u32 dst_width, u32 dst_height, u32 dst_pitch,
	u32 dst_format, u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h, u32 flags);

struct g2d_dma_mem {
	void *vaddr;
	dma_addr_t dma_addr;
	size_t size;
	struct sg_table *sgt;
	/* For system-backed allocation (non-CMA): keep pages array to free later */
	struct page **pages;
	unsigned int nents;
	bool is_coherent; /* Allocado con dma_alloc_coherent: IOVA contigua */
};

/* Internal helper honoring an explicit allocation mode override.
 * mode values: 0=CMA/noncontig, 1=system pages (vmap+sgtable), 2=coherent/IOMMU.
 */
static int g2d_dma_mem_alloc_mode(struct device *dev, size_t size,
				    struct g2d_dma_mem *mem, gfp_t gfp,
				    int mode)
{
	struct sg_table *sgt = NULL;
	void *vaddr = NULL;
	int eff_mode;

	if (!mem || !size)
		return -EINVAL;

	memset(mem, 0, sizeof(*mem));
	mem->size = size;

	/* Decide effective allocation mode: explicit override or module default */
	eff_mode = (mode >= 0) ? mode : g2d_alloc_mode;
	if (eff_mode == 0)
		eff_mode = 1;

	/* Mode 2: coherent/IOMMU contiguous IOVA region with sgtable wrapper */
	if (eff_mode == 2) {
		dma_addr_t dma;
		void *va = dma_alloc_coherent(dev, size, &dma, gfp);
		struct sg_table *coh_sgt;
		int ret_sg;

		if (!va)
			return -ENOMEM;

		coh_sgt = kzalloc(sizeof(*coh_sgt), GFP_KERNEL);
		if (!coh_sgt) {
			dma_free_coherent(dev, size, va, dma);
			return -ENOMEM;
		}

		ret_sg = dma_get_sgtable_attrs(dev, coh_sgt, va, dma, size, 0);
		if (ret_sg) {
			kfree(coh_sgt);
			dma_free_coherent(dev, size, va, dma);
			return ret_sg;
		}

		mem->vaddr = va;
		mem->dma_addr = dma;
		mem->size = size;
		mem->sgt = coh_sgt;
		mem->is_coherent = true;
		return 0;
	}

	/* Mode 1: system-backed (vmap + sgtable built from pages) */
	if (eff_mode == 1) {
		unsigned int nents = DIV_ROUND_UP(size, PAGE_SIZE);
		struct page **pages = NULL;
		unsigned int i;
		int ret;

		pages = kcalloc(nents, sizeof(*pages), GFP_KERNEL);
		if (!pages)
			return -ENOMEM;

		for (i = 0; i < nents; i++) {
			pages[i] = alloc_page(gfp | __GFP_ZERO);
			if (!pages[i]) {
				int j;
				for (j = 0; j < i; j++)
					__free_page(pages[j]);
				kfree(pages);
				return -ENOMEM;
			}
		}

		vaddr = vmap(pages, nents, VM_MAP, PAGE_KERNEL);
		if (!vaddr) {
			for (i = 0; i < nents; i++)
				__free_page(pages[i]);
			kfree(pages);
			return -ENOMEM;
		}

		sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
		if (!sgt) {
			vunmap(vaddr);
			for (i = 0; i < nents; i++)
				__free_page(pages[i]);
			kfree(pages);
			return -ENOMEM;
		}

		ret = sg_alloc_table(sgt, nents, GFP_KERNEL);
		if (ret) {
			kfree(sgt);
			vunmap(vaddr);
			for (i = 0; i < nents; i++)
				__free_page(pages[i]);
			kfree(pages);
			return -ENOMEM;
		}

		{
			/* Use kernel helper to build an sg_table from pages coalescing
			 * physically contiguous runs. This correctly sets nents and
			 * orig_nents, avoiding inconsistencies that can crash in DMA map.
			 */
			int ret2 = sg_alloc_table_from_pages(
				sgt, pages, nents, 0, size, GFP_KERNEL);
			if (ret2) {
				vunmap(vaddr);
				for (i = 0; i < nents; i++)
					__free_page(pages[i]);
				kfree(pages);
				return -ENOMEM;
			}
		}

		ret = dma_map_sgtable(dev, sgt, DMA_BIDIRECTIONAL, 0);
		if (ret) {
			sg_free_table(sgt);
			kfree(sgt);
			vunmap(vaddr);
			for (i = 0; i < nents; i++)
				__free_page(pages[i]);
			kfree(pages);
			return -EIO;
		}

		/* Success: fill mem */
		memset(vaddr, 0, size);
		mem->vaddr = vaddr;
		mem->dma_addr = sg_dma_address(sgt->sgl);
		mem->size = size;
		mem->sgt = sgt;
		mem->pages = pages;
		mem->nents = nents;

		dev_dbg(dev, "Allocated Mode 1 (System): size=%zu, dma_addr=%pad, vaddr=%p\n",
			 size, &mem->dma_addr, mem->vaddr);

		return 0;
	}

	/* Default behavior: CMA/noncontiguous allocation */
	sgt = dma_alloc_noncontiguous(dev, size, DMA_BIDIRECTIONAL, gfp, 0);
	if (!sgt)
		return -ENOMEM;

	vaddr = dma_vmap_noncontiguous(dev, size, sgt);
	if (!vaddr) {
		dma_free_noncontiguous(dev, size, sgt, DMA_BIDIRECTIONAL);
		return -ENOMEM;
	}

	memset(vaddr, 0, size);
	mem->vaddr = vaddr;
	mem->dma_addr = sg_dma_address(sgt->sgl);
	mem->size = size;
	mem->sgt = sgt;

	return 0;
}

static void g2d_dma_mem_free(struct device *dev, struct g2d_dma_mem *mem)
{
	unsigned int i;

	if (!mem || !mem->sgt)
		return;

	if (mem->is_coherent) {
		/* Coherent path: free coherent and sgtable wrapper */
		if (mem->sgt) {
			sg_free_table(mem->sgt);
			kfree(mem->sgt);
		}
		if (mem->vaddr)
			dma_free_coherent(dev, mem->size, mem->vaddr,
					  mem->dma_addr);
	} else if (mem->pages) {
		/* system-backed allocation path */
		dma_unmap_sgtable(dev, mem->sgt, DMA_BIDIRECTIONAL, 0);
		sg_free_table(mem->sgt);
		kfree(mem->sgt);

		if (mem->vaddr)
			vunmap(mem->vaddr);

		for (i = 0; i < mem->nents; i++) {
			if (mem->pages[i])
				__free_page(mem->pages[i]);
		}
		kfree(mem->pages);
		mem->pages = NULL;
		mem->nents = 0;
	} else {
		/* CMA/noncontiguous path */
		if (mem->vaddr)
			dma_vunmap_noncontiguous(dev, mem->vaddr);
		dma_free_noncontiguous(dev, mem->size, mem->sgt,
				       DMA_BIDIRECTIONAL);
	}

	mem->sgt = NULL;
	mem->vaddr = NULL;
	mem->dma_addr = 0;
	mem->size = 0;
	mem->is_coherent = false;
}

/* Check if DMA addresses in a mapped sg_table are laid out contiguously */
static bool g2d_sgt_dma_is_contiguous(struct sg_table *table)
{
	struct scatterlist *sg;
	int i;
	dma_addr_t prev_end = 0;

	if (!table || !table->sgl || table->nents <= 0)
		return false;

	for_each_sg(table->sgl, sg, table->nents, i) {
		dma_addr_t a = sg_dma_address(sg);
		unsigned int l = sg_dma_len(sg);
		if (i == 0) {
			prev_end = a + (dma_addr_t)l;
			continue;
		}
		if (a != prev_end)
			return false;
		prev_end = a + (dma_addr_t)l;
	}
	return true;
}

static int g2d_copy_rect_to_sg(struct device *dev, struct sg_table *sgt,
			       size_t dst_offset, const u8 *src, u32 src_stride,
			       u32 bytes_per_row, u32 rows, u32 dst_stride)
{
	u32 i;

	if (!sgt || !sgt->sgl || !bytes_per_row || !rows)
		return 0;

	for (i = 0; i < rows; i++) {
		size_t copied = sg_pcopy_from_buffer(
			sgt->sgl, sgt->nents, src + (size_t)i * src_stride,
			bytes_per_row, dst_offset + (size_t)i * dst_stride);

		if (copied != bytes_per_row) {
			if (dev)
				dev_err(dev,
					"writeback: sg copy failed row=%u len=%u copied=%zu\n",
					i, bytes_per_row, copied);
			return -EIO;
		}
	}

	return 0;
}

static int g2d_sg_dma_address(struct device *dev, struct sg_table *sgt,
			      dma_addr_t *out, const char *label)
{
	dma_addr_t addr;

	if (!sgt || !sgt->sgl)
		return -EINVAL;

	/* Hardware limitation: T113 G2D expects a contiguous address range in the
	 * device-visible address space (IOVA) for linear surfaces. With IOMMU
	 * present, a multi-entry sg may still represent a fully contiguous IOVA.
	 * Accept such cases; otherwise, reject to avoid DMA overruns.
	 */
	if (sgt->nents > 1) {
		struct iommu_domain *dom = iommu_get_domain_for_dev(dev);
		if (!dom || !g2d_sgt_dma_is_contiguous(sgt)) {
			dev_err(dev,
				"%s: non-contiguous IOVA for linear surface (nents=%d)\n",
				label ? label : "buffer", sgt->nents);
			return -EOPNOTSUPP;
		}
	}

	addr = sg_dma_address(sgt->sgl);
	if (!addr) {
		dev_err(dev, "%s: missing DMA address (nents=%d)\n",
			label ? label : "buffer", sgt->nents);
		return -EINVAL;
	}

	*out = addr;
	return 0;
}

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
	void __iomem *mbus_base;
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

	/* CSC State (Brightness/Contrast/Saturation) */
	struct g2d_csc_state csc_state;

	/* Fence support */
	u64 fence_context;
	atomic64_t fence_seqno;

	/* Delayed work to safely disable hardware when idle */
	struct delayed_work disable_work;

	/* Statistics */
	u32 hw_version;
	atomic64_t jobs_submitted;
	atomic64_t jobs_done;
	atomic64_t jobs_failed;

	/* Staging telemetry */
	atomic64_t stage_src_count; /* Source staging (read) used */
	atomic64_t stage_wb_out_count; /* Writeback staging for explicit OUT */
	atomic64_t
		stage_wb_inplace_count; /* Writeback staging for in-place DST */
	atomic64_t stage_wb_bytes_copied; /* Bytes copied back during writeback */
	/* Live writeback telemetry (helps detect leaks/pressure under load) */
	atomic64_t
		stage_wb_active_jobs; /* In-flight jobs using writeback staging */
	atomic64_t
		stage_wb_active_bytes; /* Total bytes of temp writeback buffers currently allocated */
	/* NEW telemetry for vmalloc/vmap fragmentation diagnostics */
	atomic64_t vmap_attempts; /* Total vmap (or dma_buf_vmap) attempts */
	atomic64_t
		vmap_failures; /* Number of vmap failures (alloc_vmap_area failures) */
	atomic64_t
		stage_wb_req_bytes; /* Cumulative requested writeback bytes (logical) */

	/* Job pool to reduce alloc/free churn (avoids fragmentation/pressure) */
	spinlock_t job_pool_lock;
	struct list_head job_pool_free; /* Free-list of reusable job structs */
	int job_pool_free_count; /* Number of jobs currently in free-list */
	int job_pool_min; /* Minimum pool size to keep */
	int job_pool_max; /* Maximum pool size (beyond this: free) */
	struct kmem_cache *job_cache; /* Slab cache for job structs */
	/* Pool telemetry */
	atomic64_t job_pool_hits; /* Reused from pool */
	atomic64_t job_pool_misses; /* Needed fresh alloc */

	/* Persistent tasks */
	struct list_head task_list;
	u32 next_task_id;
	struct mutex task_lock;
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
	G2D_JOB_FILLRECT,
	G2D_JOB_BLIT,
	G2D_JOB_CMD_COPY,
	G2D_JOB_CMD_SCALE,
	G2D_JOB_CMD_BLEND,
	G2D_JOB_CMD_ROTATE,
	G2D_JOB_CMD_MASK,
};

/* Internal job data structures */
struct g2d_job_fillrect_data {
	u32 width;
	u32 height;
	u32 pitch;
	u32 color;
	u32 color_format;
	u32 dst_format;
};

struct g2d_job_blit_data {
	/* Source parameters */
	u32 src_width;
	u32 src_height;
	u32 src_pitch;
	u32 src_format;
	u32 src_crop_x;
	u32 src_crop_y;
	u32 src_crop_w;
	u32 src_crop_h;
	u8 src_alpha;
	u8 src_alpha_mode;
	u8 src_premul;
	u8 src_color_space;

	/* Destination parameters */
	u32 dst_width;
	u32 dst_height;
	u32 dst_pitch;
	u32 dst_format;
	u32 dst_x;
	u32 dst_y;
	u32 dst_w;
	u32 dst_h;
	u8 dst_alpha;
	u8 dst_alpha_mode;
	u8 dst_premul;
	u8 dst_color_space;

	/* Output buffer parameters (3-buffer mode) */
	u32 out_width;
	u32 out_height;
	u32 out_pitch;
	u32 out_format;
	u8 out_color_space;

	/* Operation flags */
	u32 flags;
	u32 bld_mode;
	u8 color_key_enable;
	u8 color_key_mode;
	u32 color_key_min;
	u32 color_key_max;
	u8 needs_alpha;
	u8 needs_rotation;
	u8 needs_scaling;
};

struct sunxi_g2d_job {
	struct list_head node;
	struct dma_fence *fence;
	int fence_fd;
	struct sync_file *sync_file;
	struct work_struct cleanup_work;
	enum g2d_job_type type;
	struct sunxi_g2d_dev *g2d; /* Back reference for cleanup */

	/* CSC State for this job */
	struct g2d_csc_state csc_state;

	union {
		struct g2d_job_blit_data blit;
		struct g2d_job_fillrect_data fillrect;
	} data;

	/* DMA addresses for HW (mapped from DMA-BUFs) */
	dma_addr_t src_dma;
	dma_addr_t dst_dma;
	dma_addr_t out_dma; /* For 3-buffer blit */

	/* Imported DMA-BUFs to clean up */
	struct dma_buf *src_dmabuf;
	struct dma_buf_attachment *src_attach;
	struct sg_table *src_sgt;

	struct dma_buf *dst_dmabuf;
	struct dma_buf_attachment *dst_attach;
	struct sg_table *dst_sgt;

	struct dma_buf *out_dmabuf;
	struct dma_buf_attachment *out_attach;
	struct sg_table *out_sgt;

	/* Writeback support */
	bool wb_out_active;
	bool wb_out_is_explicit;
	bool wb_out_is_persistent;
	struct g2d_dma_mem wb_out_buf;
	size_t wb_out_size;

	/* Prebuilt RCQ buffer for this job (built in ioctl context) */
	struct g2d_rcq_mem rcq;
	bool rcq_ready;
	bool rcq_external; /* true when rcq owned by persistent task */
	bool persistent_refs; /* true when DMA-BUF refs owned by persistent task */
};

struct g2d_task_step {
	struct list_head node;
	enum g2d_job_type type;
	struct g2d_rcq_mem rcq;
	bool rcq_ready;

	/* Persistent buffer references to keep mappings alive */
	struct dma_buf *src_dmabuf;
	struct dma_buf_attachment *src_attach;
	struct sg_table *src_sgt;

	struct dma_buf *dst_dmabuf;
	struct dma_buf_attachment *dst_attach;
	struct sg_table *dst_sgt;

	struct dma_buf *out_dmabuf;
	struct dma_buf_attachment *out_attach;
	struct sg_table *out_sgt;

	dma_addr_t src_dma;
	dma_addr_t dst_dma;
	dma_addr_t out_dma;

	struct g2d_job_blit_data blit;
	struct g2d_job_fillrect_data fillrect;
	struct g2d_csc_state csc_state;
};

struct g2d_task {
	struct list_head node;
	u32 id;
	struct list_head steps; /* list of g2d_task_step */
	unsigned int step_count;
	struct mutex lock;
	struct sunxi_g2d_dev *g2d;
	struct sunxi_g2d_ctx *owner;
};

/* Per-file-descriptor context */
struct sunxi_g2d_ctx {
	struct sunxi_g2d_dev *g2d;
	struct g2d_csc_state csc_state;
	bool csc_changed;
	struct list_head tasks; /* Tasks owned by this context */
};

/* ===== Job pool helpers ===== */
static struct sunxi_g2d_job *g2d_job_alloc(struct sunxi_g2d_dev *g2d)
{
	struct sunxi_g2d_job *job = NULL;
	unsigned long flags;

	spin_lock_irqsave(&g2d->job_pool_lock, flags);
	if (!list_empty(&g2d->job_pool_free)) {
		job = list_first_entry(&g2d->job_pool_free,
				       struct sunxi_g2d_job, node);
		list_del_init(&job->node);
		g2d->job_pool_free_count--;
		spin_unlock_irqrestore(&g2d->job_pool_lock, flags);
		atomic64_inc(&g2d->job_pool_hits);
		memset(job, 0, sizeof(*job));
		return job;
	}
	spin_unlock_irqrestore(&g2d->job_pool_lock, flags);

	atomic64_inc(&g2d->job_pool_misses);
	if (g2d->job_cache)
		return kmem_cache_zalloc(g2d->job_cache, GFP_KERNEL);
	return kzalloc(sizeof(*job), GFP_KERNEL);
}

static void g2d_job_free(struct sunxi_g2d_dev *g2d, struct sunxi_g2d_job *job)
{
	unsigned long flags;
	if (!job)
		return;

	spin_lock_irqsave(&g2d->job_pool_lock, flags);
	if (g2d->job_pool_free_count < g2d->job_pool_max) {
		INIT_LIST_HEAD(&job->node);
		list_add(&job->node, &g2d->job_pool_free);
		g2d->job_pool_free_count++;
		spin_unlock_irqrestore(&g2d->job_pool_lock, flags);
		return;
	}
	spin_unlock_irqrestore(&g2d->job_pool_lock, flags);

	if (g2d->job_cache)
		kmem_cache_free(g2d->job_cache, job);
	else
		kfree(job);
}

static int g2d_job_prepare_rcq(struct sunxi_g2d_dev *g2d,
			       struct sunxi_g2d_job *job)
{
	int ret;

	if (!g2d || !job)
		return -EINVAL;

	if (!g2d->rcq_enabled)
		return -EOPNOTSUPP;

	ret = sunxi_g2d_rcq_alloc(g2d->dev, &job->rcq, G2D_RCQ_MAX_SIZE);
	if (ret)
		return ret;

	job->rcq_ready = false;
	return 0;
}

/* ===== Task helpers ===== */
static void g2d_task_step_free(struct sunxi_g2d_dev *g2d,
			       struct g2d_task_step *step)
{
	if (!step)
		return;

	if (step->rcq.vir_addr)
		sunxi_g2d_rcq_free(g2d->dev, &step->rcq);

	if (step->src_sgt && step->src_attach)
		dma_buf_unmap_attachment(step->src_attach, step->src_sgt,
					 DMA_TO_DEVICE);
	if (step->src_attach && step->src_dmabuf)
		dma_buf_detach(step->src_dmabuf, step->src_attach);
	if (step->src_dmabuf)
		dma_buf_put(step->src_dmabuf);

	if (step->dst_sgt && step->dst_attach)
		dma_buf_unmap_attachment(step->dst_attach, step->dst_sgt,
					 DMA_FROM_DEVICE);
	if (step->dst_attach && step->dst_dmabuf)
		dma_buf_detach(step->dst_dmabuf, step->dst_attach);
	if (step->dst_dmabuf)
		dma_buf_put(step->dst_dmabuf);

	if (step->out_sgt && step->out_attach)
		dma_buf_unmap_attachment(step->out_attach, step->out_sgt,
					 DMA_FROM_DEVICE);
	if (step->out_attach && step->out_dmabuf)
		dma_buf_detach(step->out_dmabuf, step->out_attach);
	if (step->out_dmabuf)
		dma_buf_put(step->out_dmabuf);

	kfree(step);
}

static void g2d_task_destroy(struct sunxi_g2d_dev *g2d, struct g2d_task *task)
{
	struct g2d_task_step *step, *tmp;

	if (!task)
		return;

	mutex_lock(&task->lock);
	list_for_each_entry_safe(step, tmp, &task->steps, node) {
		list_del(&step->node);
		g2d_task_step_free(g2d, step);
	}
	task->step_count = 0;
	mutex_unlock(&task->lock);

	kfree(task);
}

static struct g2d_task *g2d_task_find(struct sunxi_g2d_dev *g2d,
				      struct sunxi_g2d_ctx *ctx, u32 id)
{
	struct g2d_task *task;

	list_for_each_entry(task, &g2d->task_list, node) {
		if (task->id == id && task->owner == ctx)
			return task;
	}
	return NULL;
}

static int g2d_task_run(struct sunxi_g2d_ctx *ctx, struct g2d_task *task,
			struct g2d_task_req *req)
{
	struct sunxi_g2d_dev *g2d = ctx->g2d;
	struct g2d_task_step *step;
	int fence_fd = -1;
	int ret = 0;

	if (!task || list_empty(&task->steps))
		return -EINVAL;

	mutex_lock(&task->lock);
	list_for_each_entry(step, &task->steps, node) {
		struct sunxi_g2d_job *job;
		bool is_last = (step->node.next == &task->steps);

		if (!step->rcq_ready || !step->rcq.vir_addr) {
			ret = -EINVAL;
			break;
		}

		job = g2d_job_alloc(g2d);
		if (IS_ERR(job) || !job) {
			ret = IS_ERR(job) ? PTR_ERR(job) : -ENOMEM;
			break;
		}

		INIT_WORK(&job->cleanup_work, sunxi_g2d_job_cleanup_workfn);
		job->g2d = g2d;
		job->csc_state = step->csc_state;
		job->type = step->type;

		job->src_dmabuf = step->src_dmabuf;
		job->src_attach = step->src_attach;
		job->src_sgt = step->src_sgt;
		job->dst_dmabuf = step->dst_dmabuf;
		job->dst_attach = step->dst_attach;
		job->dst_sgt = step->dst_sgt;
		job->out_dmabuf = step->out_dmabuf;
		job->out_attach = step->out_attach;
		job->out_sgt = step->out_sgt;

		job->src_dma = step->src_dma;
		job->dst_dma = step->dst_dma;
		job->out_dma = step->out_dma;

		if (step->type == G2D_JOB_FILLRECT)
			job->data.fillrect = step->fillrect;
		else
			job->data.blit = step->blit;

		job->rcq = step->rcq;
		job->rcq_ready = step->rcq_ready;
		job->rcq_external = true;
		job->persistent_refs = true;

		job->fence = sunxi_g2d_fence_create(g2d);
		if (!job->fence) {
			g2d_job_free(g2d, job);
			ret = -ENOMEM;
			break;
		}

		if (is_last) {
			job->sync_file = sync_file_create(job->fence);
			if (!job->sync_file) {
				dma_fence_put(job->fence);
				g2d_job_free(g2d, job);
				ret = -ENOMEM;
				break;
			}

			fence_fd = get_unused_fd_flags(O_CLOEXEC);
			if (fence_fd < 0) {
				struct sync_file *sf = job->sync_file;
				if (sf && sf->file)
					fput(sf->file);
				job->sync_file = NULL;
				dma_fence_put(job->fence);
				g2d_job_free(g2d, job);
				ret = fence_fd;
				break;
			}

			fd_install(fence_fd, job->sync_file->file);
			job->fence_fd = fence_fd;
			job->sync_file = NULL;
		} else {
			job->fence_fd = -1;
			job->sync_file = NULL;
		}

		/* Enqueue job */
		{
			unsigned long flags;
			spin_lock_irqsave(&g2d->job_lock, flags);
			list_add_tail(&job->node, &g2d->job_queue);
			spin_unlock_irqrestore(&g2d->job_lock, flags);
		}
		queue_work(g2d->job_wq, &g2d->job_work);
	}
	mutex_unlock(&task->lock);

	if (ret) {
		if (fence_fd >= 0)
			put_unused_fd(fence_fd);
		return ret;
	}

	req->fence_fd_out = fence_fd;
	return 0;
}

static int g2d_task_add_step(struct sunxi_g2d_ctx *ctx, struct g2d_task *task,
			     struct g2d_cmd *cmd)
{
	struct sunxi_g2d_dev *g2d = ctx->g2d;
	struct g2d_task_step *step;
	int ret = 0;

	step = kzalloc(sizeof(*step), GFP_KERNEL);
	if (!step)
		return -ENOMEM;

	INIT_LIST_HEAD(&step->node);
	step->csc_state = ctx->csc_changed ? ctx->csc_state : g2d->csc_state;

	ret = sunxi_g2d_rcq_alloc(g2d->dev, &step->rcq, G2D_RCQ_MAX_SIZE);
	if (ret) {
		kfree(step);
		return ret;
	}

	switch (cmd->cmd_type) {
	case G2D_CMD_FILLRECT: {
		struct dma_buf *dst_dmabuf;
		struct dma_buf_attachment *dst_attach;
		struct sg_table *dst_sgt;
		dma_addr_t dst_dma;
		u32 dst_bpp;

		if (sunxi_g2d_format_to_hw(cmd->dst.format, &dst_bpp) < 0) {
			ret = -EOPNOTSUPP;
			break;
		}

		dst_dmabuf = dma_buf_get(cmd->dst.dma_fd);
		if (IS_ERR(dst_dmabuf)) {
			ret = PTR_ERR(dst_dmabuf);
			break;
		}

		dst_attach = dma_buf_attach(dst_dmabuf, g2d->dev);
		if (IS_ERR(dst_attach)) {
			ret = PTR_ERR(dst_attach);
			dma_buf_put(dst_dmabuf);
			break;
		}

		dst_sgt = dma_buf_map_attachment(dst_attach, DMA_TO_DEVICE);
		if (IS_ERR(dst_sgt)) {
			ret = PTR_ERR(dst_sgt);
			dma_buf_detach(dst_dmabuf, dst_attach);
			dma_buf_put(dst_dmabuf);
			break;
		}

		ret = g2d_sg_dma_address(g2d->dev, dst_sgt, &dst_dma,
					 "TASK_FILLRECT dst");
		if (ret) {
			dma_buf_unmap_attachment(dst_attach, dst_sgt,
						 DMA_TO_DEVICE);
			dma_buf_detach(dst_dmabuf, dst_attach);
			dma_buf_put(dst_dmabuf);
			break;
		}

		step->dst_dmabuf = dst_dmabuf;
		step->dst_attach = dst_attach;
		step->dst_sgt = dst_sgt;
		step->dst_dma = dst_dma;

		step->fillrect.width = cmd->dst_w;
		step->fillrect.height = cmd->dst_h;
		step->fillrect.pitch = cmd->dst.stride[0] ?
					       cmd->dst.stride[0] :
					       (cmd->dst.width * dst_bpp);
		step->fillrect.color = cmd->params.fillrect.color;
		step->fillrect.color_format = cmd->dst.format;
		step->fillrect.dst_format = cmd->dst.format;

		ret = sunxi_g2d_do_fillrect_rcq(
			g2d, &step->rcq, step->dst_dma, step->fillrect.width,
			step->fillrect.height, step->fillrect.pitch,
			step->fillrect.color, step->fillrect.color_format,
			step->fillrect.dst_format);
		if (!ret)
			step->rcq_ready = true;
		step->type = G2D_JOB_FILLRECT;
		break;
	}

	case G2D_CMD_COPY:
	case G2D_CMD_SCALE:
	case G2D_CMD_BLEND:
	case G2D_CMD_ROTATE:
	case G2D_CMD_MASK: {
		struct dma_buf *src_dmabuf = NULL, *dst_dmabuf = NULL,
			       *out_dmabuf = NULL;
		struct dma_buf_attachment *src_attach = NULL,
					  *dst_attach = NULL,
					  *out_attach = NULL;
		struct sg_table *src_sgt = NULL, *dst_sgt = NULL,
				*out_sgt = NULL;
		dma_addr_t src_dma = 0, dst_dma = 0, out_dma = 0;
		u32 src_bpp = 0, dst_bpp = 0, out_bpp = 0;
		u32 dst_stride[3] = { 0 }, dst_plane_offset[3] = { 0 };
		u32 out_stride[3] = { 0 }, out_plane_offset[3] = { 0 };
		u32 src_stride[3] = { 0 }, src_plane_offset[3] = { 0 };
		bool has_out = (cmd->out.dma_fd >= 0);

		if (sunxi_g2d_format_to_hw(cmd->src.format, &src_bpp) < 0) {
			ret = -EOPNOTSUPP;
			goto blit_cleanup;
		}
		if (sunxi_g2d_format_to_hw(cmd->dst.format, &dst_bpp) < 0) {
			ret = -EOPNOTSUPP;
			goto blit_cleanup;
		}
		if (has_out &&
		    sunxi_g2d_format_to_hw(cmd->out.format, &out_bpp) < 0) {
			ret = -EOPNOTSUPP;
			goto blit_cleanup;
		}

		if (cmd->cmd_type == G2D_CMD_COPY &&
		    (cmd->src.crop_w != cmd->dst_w ||
		     cmd->src.crop_h != cmd->dst_h)) {
			ret = -EINVAL;
			goto blit_cleanup;
		}

		if (cmd->cmd_type == G2D_CMD_BLEND && !has_out) {
			ret = -EINVAL;
			goto blit_cleanup;
		}

		src_dmabuf = dma_buf_get(cmd->src.dma_fd);
		if (IS_ERR(src_dmabuf)) {
			ret = PTR_ERR(src_dmabuf);
			goto blit_cleanup;
		}
		src_attach = dma_buf_attach(src_dmabuf, g2d->dev);
		if (IS_ERR(src_attach)) {
			ret = PTR_ERR(src_attach);
			goto blit_cleanup;
		}
		src_sgt = dma_buf_map_attachment(src_attach, DMA_TO_DEVICE);
		if (IS_ERR(src_sgt)) {
			ret = PTR_ERR(src_sgt);
			goto blit_cleanup;
		}

		sunxi_g2d_get_yuv_plane_info(cmd->src.format, cmd->src.width,
					     cmd->src.height, cmd->src.stride,
					     src_stride, src_plane_offset);

		ret = g2d_sg_dma_address(g2d->dev, src_sgt, &src_dma,
					 "TASK src");
		if (ret)
			goto blit_cleanup;

		dst_dmabuf = dma_buf_get(cmd->dst.dma_fd);
		if (IS_ERR(dst_dmabuf)) {
			ret = PTR_ERR(dst_dmabuf);
			goto blit_cleanup;
		}
		dst_attach = dma_buf_attach(dst_dmabuf, g2d->dev);
		if (IS_ERR(dst_attach)) {
			ret = PTR_ERR(dst_attach);
			goto blit_cleanup;
		}
		dst_sgt = dma_buf_map_attachment(dst_attach, DMA_FROM_DEVICE);
		if (IS_ERR(dst_sgt)) {
			ret = PTR_ERR(dst_sgt);
			goto blit_cleanup;
		}

		sunxi_g2d_get_yuv_plane_info(cmd->dst.format, cmd->dst.width,
					     cmd->dst.height, cmd->dst.stride,
					     dst_stride, dst_plane_offset);

		ret = g2d_sg_dma_address(g2d->dev, dst_sgt, &dst_dma,
					 "TASK dst");
		if (ret)
			goto blit_cleanup;

		if (has_out) {
			out_dmabuf = dma_buf_get(cmd->out.dma_fd);
			if (IS_ERR(out_dmabuf)) {
				ret = PTR_ERR(out_dmabuf);
				goto blit_cleanup;
			}
			out_attach = dma_buf_attach(out_dmabuf, g2d->dev);
			if (IS_ERR(out_attach)) {
				ret = PTR_ERR(out_attach);
				goto blit_cleanup;
			}
			out_sgt = dma_buf_map_attachment(out_attach,
							 DMA_FROM_DEVICE);
			if (IS_ERR(out_sgt)) {
				ret = PTR_ERR(out_sgt);
				goto blit_cleanup;
			}
			sunxi_g2d_get_yuv_plane_info(
				cmd->out.format, cmd->out.width,
				cmd->out.height, cmd->out.stride, out_stride,
				out_plane_offset);
			ret = g2d_sg_dma_address(g2d->dev, out_sgt, &out_dma,
						 "TASK out");
			if (ret)
				goto blit_cleanup;
		} else {
			out_dma = dst_dma;
			memcpy(out_stride, dst_stride, sizeof(out_stride));
			memcpy(out_plane_offset, dst_plane_offset,
			       sizeof(out_plane_offset));
		}

		step->src_dmabuf = src_dmabuf;
		step->src_attach = src_attach;
		step->src_sgt = src_sgt;
		step->dst_dmabuf = dst_dmabuf;
		step->dst_attach = dst_attach;
		step->dst_sgt = dst_sgt;
		step->out_dmabuf = out_dmabuf;
		step->out_attach = out_attach;
		step->out_sgt = out_sgt;

		step->src_dma = src_dma;
		step->dst_dma = dst_dma;
		step->out_dma = out_dma;

		/* Fill blit data for reuse */
		step->blit.src_width = cmd->src.width;
		step->blit.src_height = cmd->src.height;
		step->blit.src_pitch = src_stride[0];
		step->blit.src_format = cmd->src.format;
		step->blit.src_crop_x = cmd->src.crop_x;
		step->blit.src_crop_y = cmd->src.crop_y;
		step->blit.src_crop_w = cmd->src.crop_w;
		step->blit.src_crop_h = cmd->src.crop_h;
		step->blit.src_alpha = cmd->src.alpha;
		step->blit.src_alpha_mode = cmd->src.alpha_mode;
		step->blit.src_premul = cmd->src.premul_mode;
		step->blit.src_color_space = cmd->src.color_space;

		step->blit.dst_width = cmd->dst.width;
		step->blit.dst_height = cmd->dst.height;
		step->blit.dst_pitch = dst_stride[0];
		step->blit.dst_format = cmd->dst.format;
		step->blit.dst_x = cmd->dst_x;
		step->blit.dst_y = cmd->dst_y;
		step->blit.dst_w = cmd->dst_w;
		step->blit.dst_h = cmd->dst_h;
		step->blit.dst_alpha = cmd->dst.alpha;
		step->blit.dst_alpha_mode = cmd->dst.alpha_mode;
		step->blit.dst_premul = cmd->dst.premul_mode;
		step->blit.dst_color_space = cmd->dst.color_space;

		step->blit.out_width =
			has_out ? cmd->out.width : cmd->dst.width;
		step->blit.out_height =
			has_out ? cmd->out.height : cmd->dst.height;
		step->blit.out_pitch =
			has_out ? out_stride[0] : dst_stride[0];
		step->blit.out_format =
			has_out ? cmd->out.format : cmd->dst.format;
		step->blit.out_color_space =
			has_out ? cmd->out.color_space : cmd->dst.color_space;

		step->blit.bld_mode =
			(cmd->cmd_type == G2D_CMD_MASK) ?
				G2D_BLD_SRCOVER :
				cmd->params.blend.bld_mode;
		step->blit.color_key_enable =
			(cmd->cmd_type == G2D_CMD_MASK);
		step->blit.color_key_mode = cmd->params.mask.color_key_mode;
		step->blit.color_key_min = cmd->params.mask.color_key_min;
		step->blit.color_key_max = cmd->params.mask.color_key_max;
		step->blit.flags = cmd->flags;

		if (cmd->cmd_type == G2D_CMD_COPY) {
			step->type = G2D_JOB_CMD_COPY;
			ret = sunxi_g2d_do_blit_rcq(
				g2d, &step->rcq, step->src_dma,
				step->blit.src_width, step->blit.src_height,
				step->blit.src_pitch, step->blit.src_format,
				step->blit.src_crop_x, step->blit.src_crop_y,
				step->blit.src_crop_w, step->blit.src_crop_h,
				step->dst_dma, step->blit.dst_width,
				step->blit.dst_height, step->blit.dst_pitch,
				step->blit.dst_format, step->blit.dst_x,
				step->blit.dst_y, step->blit.dst_w,
				step->blit.dst_h, step->blit.src_color_space,
				step->blit.dst_color_space);
		} else if (cmd->cmd_type == G2D_CMD_SCALE) {
			step->type = G2D_JOB_CMD_SCALE;
			ret = sunxi_g2d_do_scale_rcq(
				g2d, &step->rcq, step->src_dma,
				step->blit.src_width, step->blit.src_height,
				step->blit.src_pitch, step->blit.src_format,
				step->blit.src_crop_x, step->blit.src_crop_y,
				step->blit.src_crop_w, step->blit.src_crop_h,
				step->dst_dma, step->blit.dst_width,
				step->blit.dst_height, step->blit.dst_pitch,
				step->blit.dst_format, step->blit.dst_x,
				step->blit.dst_y, step->blit.dst_w,
				step->blit.dst_h, step->blit.src_color_space,
				step->blit.dst_color_space, &step->csc_state);
		} else if (cmd->cmd_type == G2D_CMD_BLEND ||
			   cmd->cmd_type == G2D_CMD_MASK) {
			step->type = (cmd->cmd_type == G2D_CMD_BLEND) ?
					     G2D_JOB_CMD_BLEND :
					     G2D_JOB_CMD_MASK;
			ret = sunxi_g2d_do_blend_rcq(
				g2d, &step->rcq, step->src_dma,
				step->blit.src_width, step->blit.src_height,
				step->blit.src_pitch, step->blit.src_format,
				step->blit.src_crop_x, step->blit.src_crop_y,
				step->blit.src_crop_w, step->blit.src_crop_h,
				step->dst_dma, step->blit.dst_width,
				step->blit.dst_height, step->blit.dst_pitch,
				step->blit.dst_format, step->blit.dst_x,
				step->blit.dst_y, step->out_dma,
				step->blit.out_width, step->blit.out_height,
				step->blit.out_pitch, step->blit.out_format,
				step->blit.dst_x, step->blit.dst_y,
				step->blit.dst_w, step->blit.dst_h,
				step->blit.bld_mode, step->blit.src_alpha_mode,
				step->blit.src_alpha, step->blit.src_premul,
				step->blit.src_color_space,
				step->blit.dst_color_space,
				step->blit.out_color_space, &step->csc_state);
		} else if (cmd->cmd_type == G2D_CMD_ROTATE) {
			u32 flags = 0;
			step->type = G2D_JOB_CMD_ROTATE;
			if (cmd->params.rotate.angle == 90)
				flags |= G2D_BLIT_FLAG_ROTATE_90;
			else if (cmd->params.rotate.angle == 180)
				flags |= G2D_BLIT_FLAG_ROTATE_180;
			else if (cmd->params.rotate.angle == 270)
				flags |= G2D_BLIT_FLAG_ROTATE_270;
			if (cmd->params.rotate.flip_h)
				flags |= G2D_BLIT_FLAG_FLIP_H;
			if (cmd->params.rotate.flip_v)
				flags |= G2D_BLIT_FLAG_FLIP_V;

			ret = sunxi_g2d_do_blit_rot_rcq(
				g2d, &step->rcq, step->src_dma,
				step->blit.src_width, step->blit.src_height,
				step->blit.src_pitch, step->blit.src_format,
				step->blit.src_crop_x, step->blit.src_crop_y,
				step->blit.src_crop_w, step->blit.src_crop_h,
				step->dst_dma, step->blit.dst_width,
				step->blit.dst_height, step->blit.dst_pitch,
				step->blit.dst_format, step->blit.dst_x,
				step->blit.dst_y, step->blit.dst_w,
				step->blit.dst_h, flags);
		}

blit_cleanup:
		if (ret) {
			if (out_sgt && !IS_ERR(out_sgt))
				dma_buf_unmap_attachment(out_attach, out_sgt,
							 DMA_FROM_DEVICE);
			if (out_attach && out_dmabuf &&
			    !IS_ERR(out_attach))
				dma_buf_detach(out_dmabuf, out_attach);
			if (out_dmabuf && !IS_ERR(out_dmabuf))
				dma_buf_put(out_dmabuf);

			if (dst_sgt && !IS_ERR(dst_sgt))
				dma_buf_unmap_attachment(dst_attach, dst_sgt,
							 DMA_FROM_DEVICE);
			if (dst_attach && dst_dmabuf &&
			    !IS_ERR(dst_attach))
				dma_buf_detach(dst_dmabuf, dst_attach);
			if (dst_dmabuf && !IS_ERR(dst_dmabuf))
				dma_buf_put(dst_dmabuf);

			if (src_sgt && !IS_ERR(src_sgt))
				dma_buf_unmap_attachment(src_attach, src_sgt,
							 DMA_TO_DEVICE);
			if (src_attach && src_dmabuf &&
			    !IS_ERR(src_attach))
				dma_buf_detach(src_dmabuf, src_attach);
			if (src_dmabuf && !IS_ERR(src_dmabuf))
				dma_buf_put(src_dmabuf);

			step->src_dmabuf = NULL;
			step->src_attach = NULL;
			step->src_sgt = NULL;
			step->dst_dmabuf = NULL;
			step->dst_attach = NULL;
			step->dst_sgt = NULL;
			step->out_dmabuf = NULL;
			step->out_attach = NULL;
			step->out_sgt = NULL;
		} else {
			step->rcq_ready = true;
		}
		break;
	}
	default:
		ret = -EINVAL;
		break;
	}

	if (ret) {
		g2d_task_step_free(g2d, step);
		return ret;
	}

	mutex_lock(&task->lock);
	list_add_tail(&step->node, &task->steps);
	task->step_count++;
	mutex_unlock(&task->lock);

	return 0;
}

static void sunxi_g2d_job_cleanup_workfn(struct work_struct *work)
{
	struct sunxi_g2d_job *job =
		container_of(work, struct sunxi_g2d_job, cleanup_work);

	if (!job)
		return;

	/* Trace cleanup entry */
	pr_debug("sunxi_g2d: cleanup job %p sync_file=%p fence=%p fd=%d\n", job,
		 job->sync_file, job->fence, job->fence_fd);

	/* If writeback staging was used, copy the HW output from the temporary
	 * buffer back into the original destination/output dma-buf region before
	 * releasing attachments. Defer fence signal until after copy-back.
	 */
	if (job->wb_out_active) {
		struct dma_buf *target_dmabuf = job->wb_out_is_explicit ?
							job->out_dmabuf :
							job->dst_dmabuf;
		u32 stage_pitch0 = job->data.blit.out_pitch;
		u32 stage_format = job->data.blit.out_format;
		u32 target_x = job->wb_out_is_explicit ? 0 :
							 job->data.blit.dst_x;
		u32 target_y = job->wb_out_is_explicit ? 0 :
							 job->data.blit.dst_y;
		u32 copy_w = job->data.blit.dst_w;
		u32 copy_h = job->data.blit.dst_h;
		struct sg_table *target_sgt =
			job->wb_out_is_explicit ? job->out_sgt : job->dst_sgt;

		if (target_dmabuf && job->wb_out_buf.vaddr && target_sgt) {
			const u8 *src_base = job->wb_out_buf.vaddr;
			struct device *dev = job->g2d ? job->g2d->dev : NULL;
			u32 t_pitch0 = job->wb_out_is_explicit ?
					       job->data.blit.out_pitch :
					       job->data.blit.dst_pitch;
			u32 t_format = job->wb_out_is_explicit ?
					       job->data.blit.out_format :
					       job->data.blit.dst_format;
			u32 t_width = job->wb_out_is_explicit ?
					      job->data.blit.out_width :
					      job->data.blit.dst_width;
			u32 t_height = job->wb_out_is_explicit ?
					       job->data.blit.out_height :
					       job->data.blit.dst_height;

			int ret = dma_buf_begin_cpu_access(target_dmabuf,
							   DMA_TO_DEVICE);

			if (!ret) {
				if (sunxi_g2d_is_yuv_planar(stage_format) ||
				    sunxi_g2d_is_yuv_semiplanar(stage_format)) {
					u32 s_user_stride[3] = { stage_pitch0,
								 0, 0 };
					u32 s_stride[3], s_off[3];
					u32 t_user_stride[3] = { t_pitch0, 0,
								 0 };
					u32 t_stride[3], t_off[3];

					sunxi_g2d_get_yuv_plane_info(
						stage_format, copy_w, copy_h,
						s_user_stride, s_stride, s_off);
					sunxi_g2d_get_yuv_plane_info(
						t_format, t_width, t_height,
						t_user_stride, t_stride, t_off);

					/* Debugging: when forced staging is enabled, dump computed strides/offsets
					 * This helps diagnose mismatches between the staging buffer layout and
					 * the destination buffer (common cause for the 1st-lines / solid-square
					 * corruption observed under forced staging).
					 */
					ret = g2d_copy_rect_to_sg(
						dev, target_sgt,
						(size_t)t_off[0] +
							(size_t)target_y *
								t_stride[0] +
							(size_t)target_x,
						src_base + s_off[0],
						s_stride[0], copy_w, copy_h,
						t_stride[0]);

					if (!ret) {
						if (sunxi_g2d_is_yuv_planar(
							    stage_format)) {
							u32 sub_w =
								(stage_format ==
									 G2D_FMT_YUV420_P ||
								 stage_format ==
									 G2D_FMT_YUV420_P_VU) ?
									(copy_w >>
									 1) :
									(stage_format == G2D_FMT_YUV411_P ?
										 (copy_w >>
										  2) :
										 (copy_w >>
										  1));
							u32 sub_h =
								(stage_format ==
									 G2D_FMT_YUV420_P ||
								 stage_format ==
									 G2D_FMT_YUV420_P_VU) ?
									(copy_h >>
									 1) :
									copy_h;
							u32 cx =
								(stage_format ==
								 G2D_FMT_YUV411_P) ?
									(target_x >>
									 2) :
									(target_x >>
									 1);
							u32 cy =
								(stage_format ==
									 G2D_FMT_YUV420_P ||
								 stage_format ==
									 G2D_FMT_YUV420_P_VU) ?
									(target_y >>
									 1) :
									target_y;

							ret = g2d_copy_rect_to_sg(
								dev, target_sgt,
								(size_t)t_off[1] +
									(size_t)cy *
										t_stride[1] +
									(size_t)cx,
								src_base +
									s_off[1],
								s_stride[1],
								sub_w, sub_h,
								t_stride[1]);
							if (!ret)
								ret = g2d_copy_rect_to_sg(
									dev,
									target_sgt,
									(size_t)t_off[2] +
										(size_t)cy *
											t_stride[2] +
										(size_t)cx,
									src_base +
										s_off[2],
									s_stride[2],
									sub_w,
									sub_h,
									t_stride[2]);
						} else {
							u32 sub_h =
								(stage_format >=
									 G2D_FMT_YUV420_SP_UVUV &&
								 stage_format <=
									 G2D_FMT_YUV420_SP_VUVU) ?
									(copy_h >>
									 1) :
									copy_h;
							u32 uv_row_bytes =
								(stage_format >=
									 G2D_FMT_YUV420_SP_UVUV &&
								 stage_format <=
									 G2D_FMT_YUV420_SP_VUVU) ?
									ALIGN(copy_w,
									      2) :
									copy_w;
							u32 cx_bytes =
								(stage_format >=
									 G2D_FMT_YUV420_SP_UVUV &&
								 stage_format <=
									 G2D_FMT_YUV420_SP_VUVU) ?
									ALIGN_DOWN(
										target_x,
										2) :
									target_x;
							u32 cy =
								(stage_format >=
									 G2D_FMT_YUV420_SP_UVUV &&
								 stage_format <=
									 G2D_FMT_YUV420_SP_VUVU) ?
									(target_y >>
									 1) :
									target_y;

							ret = g2d_copy_rect_to_sg(
								dev, target_sgt,
								(size_t)t_off[1] +
									(size_t)cy *
										t_stride[1] +
									(size_t)cx_bytes,
								src_base +
									s_off[1],
								s_stride[1],
								uv_row_bytes,
								sub_h,
								t_stride[1]);
						}
					}
				} else {
					u32 bpp = 0;
					size_t row_bytes;

					sunxi_g2d_format_to_hw(stage_format,
							       &bpp);
					row_bytes = (size_t)copy_w * bpp;

					ret = g2d_copy_rect_to_sg(
						dev, target_sgt,
						(size_t)target_y * t_pitch0 +
							(size_t)target_x * bpp,
						src_base, stage_pitch0,
						row_bytes, copy_h, t_pitch0);
				}

				if (!ret && job->g2d) {
					size_t total_bytes = 0;

					if (sunxi_g2d_is_yuv_planar(
						    stage_format)) {
						total_bytes =
							(size_t)copy_w *
								copy_h +
							(size_t)(copy_w >> 1) *
								((stage_format ==
									  G2D_FMT_YUV420_P ||
								  stage_format ==
									  G2D_FMT_YUV420_P_VU) ?
									 (copy_h >>
									  1) :
									 copy_h) *
								2;
					} else if (sunxi_g2d_is_yuv_semiplanar(
							   stage_format)) {
						total_bytes =
							(size_t)copy_w *
								copy_h +
							(size_t)ALIGN(copy_w,
								      2) *
								((stage_format >=
									  G2D_FMT_YUV420_SP_UVUV &&
								  stage_format <=
									  G2D_FMT_YUV420_SP_VUVU) ?
									 (copy_h >>
									  1) :
									 copy_h);
					} else {
						u32 bpp = 0;
						sunxi_g2d_format_to_hw(
							stage_format, &bpp);
						total_bytes = (size_t)copy_w *
							      copy_h * bpp;
					}
					atomic64_add(
						total_bytes,
						&job->g2d->stage_wb_bytes_copied);
					dev_dbg(job->g2d->dev,
						"cleanup: writeback copyback ~%zu bytes (dst=%s)\n",
						total_bytes,
						job->wb_out_is_explicit ?
							"out" :
							"dst");
				}

				dma_buf_end_cpu_access(target_dmabuf,
						       DMA_TO_DEVICE);
			}
		}

		/* Release writeback staging resources/accounting */
		if (job->g2d) {
			if (job->wb_out_is_persistent) {
				/* Persistent buffer stays allocated on the device; only update counters */
				atomic64_sub(job->wb_out_size,
					     &job->g2d->stage_wb_active_bytes);
				atomic64_dec(&job->g2d->stage_wb_active_jobs);
			} else if (job->wb_out_buf.sgt) {
				/* Live accounting: writeback temp being released (ephemeral buffer) */
				atomic64_sub(job->wb_out_buf.size,
					     &job->g2d->stage_wb_active_bytes);
				atomic64_dec(&job->g2d->stage_wb_active_jobs);
				g2d_dma_mem_free(job->g2d->dev,
						 &job->wb_out_buf);
			}
		}

		/* Now that the content is in the original buffer, signal fence if not already */
		if (job->fence && !dma_fence_is_signaled(job->fence))
			dma_fence_signal(job->fence);
	}

	/* Release sync_file if still present (not installed) */
	if (job->sync_file) {
		pr_debug("sunxi_g2d: cleanup - fput sync_file->file=%p\n",
			 job->sync_file->file);
		if (job->sync_file->file)
			fput(job->sync_file->file);
		kfree(job->sync_file);
		job->sync_file = NULL;
	}

	if (job->fence) {
		pr_debug("sunxi_g2d: cleanup - dma_fence_put(%p)\n",
			 job->fence);
		dma_fence_put(job->fence);
	}

	/* Release imported DMA-BUFs (if any) */
	if (!job->persistent_refs) {
		if (job->src_sgt && job->src_attach) {
			dma_buf_unmap_attachment(job->src_attach, job->src_sgt,
						 DMA_TO_DEVICE);
			job->src_sgt = NULL;
		}
		if (job->src_attach && job->src_dmabuf) {
			dma_buf_detach(job->src_dmabuf, job->src_attach);
			job->src_attach = NULL;
		}
		if (job->src_dmabuf) {
			dma_buf_put(job->src_dmabuf);
			job->src_dmabuf = NULL;
		}

		if (job->dst_sgt && job->dst_attach) {
			dma_buf_unmap_attachment(job->dst_attach, job->dst_sgt,
						 DMA_FROM_DEVICE);
			job->dst_sgt = NULL;
		}
		if (job->dst_attach && job->dst_dmabuf) {
			dma_buf_detach(job->dst_dmabuf, job->dst_attach);
			job->dst_attach = NULL;
		}
		if (job->dst_dmabuf) {
			dma_buf_put(job->dst_dmabuf);
			job->dst_dmabuf = NULL;
		}

		if (job->out_sgt && job->out_attach) {
			dma_buf_unmap_attachment(job->out_attach, job->out_sgt,
						 DMA_FROM_DEVICE);
			job->out_sgt = NULL;
		}
		if (job->out_attach && job->out_dmabuf) {
			dma_buf_detach(job->out_dmabuf, job->out_attach);
			job->out_attach = NULL;
		}
		if (job->out_dmabuf) {
			dma_buf_put(job->out_dmabuf);
			job->out_dmabuf = NULL;
		}
	}

	/* Release temporary buffers allocated by the driver */
	if (job->g2d) {
		if (job->rcq.vir_addr && !job->rcq_external) {
			sunxi_g2d_rcq_free(job->g2d->dev, &job->rcq);
			job->rcq_ready = false;
		}
	}

	pr_debug("sunxi_g2d: cleanup - free/return job %p\n", job);
	if (job->g2d)
		g2d_job_free(job->g2d, job);
	else
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
	struct sunxi_g2d_fence *sf =
		container_of(f, struct sunxi_g2d_fence, base);
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
	dev_dbg(g2d->dev, "sunxi_g2d: fence_create seq=%llu fence=%p pid=%d\n",
		seq, &sf->base, task_tgid_nr(current));
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
	struct g2d_selftest_work *w =
		container_of(dwork, struct g2d_selftest_work, dwork);

	if (w->fence) {
		struct sunxi_g2d_fence *sf =
			container_of(w->fence, struct sunxi_g2d_fence, base);
		if (sf && sf->g2d && sf->g2d->dev)
			dev_dbg(sf->g2d->dev,
				"selftest: signalling fence seq=%llu fence=%p\n",
				sf->seqno, w->fence);
		else
			pr_debug("sunxi_g2d: selftest: signalling fence %p\n",
				 w->fence);

		dma_fence_signal(w->fence);

		/* drop our reference taken for the work item */
		if (sf && sf->g2d && sf->g2d->dev)
			dev_dbg(sf->g2d->dev,
				"selftest: dma_fence_put fence=%p\n", w->fence);
		else
			pr_debug(
				"sunxi_g2d: selftest: dma_fence_put fence=%p\n",
				w->fence);
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
	struct g2d_dma_mem mem;
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
#define MBUS_MAST_CFG0_BASE 0x0210 /* Base for T113/D1 master config */
#define MBUS_MAST_CFG_STRIDE 0x0010 /* 16 bytes per master */
#define G2D_PORT 9 /* G2D is master 9 */

/* Calculate register offset for G2D master 9:
 * MBUS_MAST_CFG0_REG(9) = 0x0210 + (0x10 * 9) = 0x02A0
 * MBUS_MAST_CFG1_REG(9) = 0x0214 + (0x10 * 9) = 0x02A4
 */
#define MBUS_MAST_CFG0_REG(n) \
	(MBUS_MAST_CFG0_BASE + (MBUS_MAST_CFG_STRIDE * (n)))
#define MBUS_MAST_CFG1_REG(n) \
	(MBUS_MAST_CFG0_BASE + 0x4 + (MBUS_MAST_CFG_STRIDE * (n)))
#define MBUS_MAST_ABS_BWL_REG(n) (MBUS_MAST_CFG0_BASE + 0x8 + 0x10 * (n))
#define MBUS_BW_CFG_REG 0x0200
#define MBUS_MAST_ACEN_CFG_REG(k) (0x0020 + 0x04 * (k))

/* MBUS_MAST_CFG0_REG bit layout (from BSP sunxi_mbus.c):
 * Bit 0:     BANDWIDTH_LIMIT_ENABLE (BWLEN)
 * Bit 1:     MASTER_N_ACCESS_PRIORITY (PRI) - 0=low, 1=high
 * Bits 3-2:  MASTER_N_QOS_VALUE (QOS) - 0=lowest, 3=highest
 * Bits 7-4:  MASTER_N_WAIT_TIME (WT)
 * Bits 15-8: COMMAND_NUMBER (ACS)
 * Bits 31-16: BANDWIDTH_LIMIT_0 (BWL0)
 */
#define MBUS_PRI_SHIFT 1
#define MBUS_QOS_SHIFT 2
#define MBUS_QOS_MASK 0x3

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
	u32 off = MBUS_MAST_CFG0_REG(G2D_PORT); /* -> 0x02A0 */
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
	v = readl_relaxed(mbus + off);
	/* Disable Bandwidth Limit (Bit 0) just in case */
	nv = v & ~BIT(0);
	/* Set Priority and QoS */
	nv |= BIT(MBUS_PRI_SHIFT);
	nv &= ~(MBUS_QOS_MASK << MBUS_QOS_SHIFT);
	nv |= (MBUS_QOS_MASK << MBUS_QOS_SHIFT);

	if (nv != v) {
		writel_relaxed(nv, mbus + off);
		wmb();
	}

	/* (opcional) log/verify */
	nv = readl_relaxed(mbus + off);
	dev_dbg(dev, "MBUS CFG0(9) @0x%04x before=0x%08x after=0x%08x\n", off,
		v, nv);

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

	/* Step 1: Enable clocks (BSP order: bus → mod@300MHz → mbus) */
	ret = clk_prepare_enable(g2d->clk_bus);
	if (ret) {
		dev_err(g2d->dev, "Failed to enable bus clock: %d\n", ret);
		return ret;
	}

	ret = clk_set_rate(g2d->clk_mod, 300000000);
	if (ret) {
		dev_warn(g2d->dev, "Failed to set module clock rate: %d\n",
			 ret);
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

	dev_dbg(g2d->dev, "Clocks: mod=%lu bus=%lu mbus=%lu\n",
		clk_get_rate(g2d->clk_mod), clk_get_rate(g2d->clk_bus),
		clk_get_rate(g2d->clk_mbus));

	/* Step 2: TOP enable - open gates (0x3 = MIXER + ROT) */
	g2d_write(g2d, G2D_SCLK_GATE, 0x3); /* Source clocks */
	g2d_write(g2d, G2D_HCLK_GATE, 0x3); /* AHB clocks */
	g2d_write(g2d, G2D_AHB_RESET, 0x0); /* Assert reset */
	wmb();
	udelay(10);
	g2d_write(g2d, G2D_AHB_RESET, 0x3); /* Release reset */
	wmb();

	/* Configure CMD_CTL for DRAM command control (needed for RCQ DMA) */
	/* Enable CORE0 (Mixer), CORE1 (Rotator?), and RT_WB */
	/* Restore this write as it might be critical for Rotator DMA access */
	g2d_write(g2d, G2D_CMD_CTL, 0x00010011); 
	wmb();

	dev_dbg(g2d->dev,
		"TOP: SCLK=0x%08x HCLK=0x%08x RESET=0x%08x CMD_CTL=0x%08x\n",
		g2d_read(g2d, G2D_SCLK_GATE), g2d_read(g2d, G2D_HCLK_GATE),
		g2d_read(g2d, G2D_AHB_RESET), g2d_read(g2d, G2D_CMD_CTL));

	/* Read VERSION */
	val = g2d_read(g2d, G2D_VERSION);
	g2d->hw_version = val;
	dev_dbg(g2d->dev, "Hardware version 0x%04x\n", (val >> 16) & 0xFFFF);

	/* Step 3: CCU enable - enable WB/RCQ sub-blocks */
	if (g2d->ccu_base) {
		val = readl(g2d->ccu_base + G2D_CLK_REG);
		val |= G2D_CLK_GATING;
		writel(val, g2d->ccu_base + G2D_CLK_REG);

		/* Enable G2D (bit 0) and Rotator (bit 1?) gates and de-assert resets (bits 16, 17?) */
		writel(0x00030003,
		       g2d->ccu_base + G2D_BGR_REG); /* Gate + RST */
		wmb();

		dev_dbg(g2d->dev, "CCU: CLK_REG=0x%08x BGR_REG=0x%08x\n",
			readl(g2d->ccu_base + G2D_CLK_REG),
			readl(g2d->ccu_base + G2D_BGR_REG));
	}

	/* Step 4: Set MBUS bandwidth (300 MB/s avg, 600 MB/s peak) */
	if (g2d->icc_path) {
		ret = icc_set_bw(g2d->icc_path, Bps_to_icc(300 * 1024 * 1024),
				 Bps_to_icc(600 * 1024 * 1024));
		if (ret)
			dev_warn(g2d->dev, "Failed to set MBUS bandwidth: %d\n",
				 ret);
	}

	/* Step 4.5: Configure MBUS priority for G2D master (T113-S3 quirk) */
	if (g2d->mbus_base)
		sunxi_g2d_setup_mbus(g2d->dev, g2d->mbus_base);
	else
		dev_warn_once(
			g2d->dev,
			"MBUS base not mapped; skipping priority setup\n");

	dev_dbg(g2d->dev, "RCQ_CTRL=0x%08x RCQ_STATUS=0x%08x\n",
		g2d_read(g2d, G2D_RCQ_CTRL), g2d_read(g2d, G2D_RCQ_STATUS));

	/* Step 6: Configure interrupts for DIRECT mode */
	g2d_write(g2d, G2D_MIXER_INT, G2D_MIXER_INT_FINISH_IRQ_EN);

	g2d->hw_enabled = true;

	dev_dbg(g2d->dev, "G2D hardware enabled\n");
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

	dev_dbg(g2d->dev, "G2D hardware disabled\n");
}

/* Delayed work handler: disable HW only when no users, no queued jobs and no current job */
static void sunxi_g2d_disable_workfn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct sunxi_g2d_dev *g2d =
		container_of(dwork, struct sunxi_g2d_dev, disable_work);
	unsigned long flags;
	bool should_disable = false;

	/* Quick check under job_lock to avoid races with job enqueue/dequeue */
	spin_lock_irqsave(&g2d->job_lock, flags);
	/* CRITICAL: Do NOT disable if current_job is set, even if users==0.
	 * This handles the case where userspace closes the fd (users->0)
	 * while a job is still running in the worker.
	 */
	if (atomic_read(&g2d->users) == 0 && list_empty(&g2d->job_queue) &&
	    g2d->current_job == NULL)
		should_disable = true;
	spin_unlock_irqrestore(&g2d->job_lock, flags);

	if (should_disable) {
		/* Re-check under dev_mutex and disable safely */
		mutex_lock(&g2d->dev_mutex);
		/* Double check current_job under mutex just in case */
		spin_lock_irqsave(&g2d->job_lock, flags);
		if (g2d->current_job != NULL) {
			should_disable = false;
		}
		spin_unlock_irqrestore(&g2d->job_lock, flags);

		if (should_disable && atomic_read(&g2d->users) == 0 &&
		    list_empty(&g2d->job_queue))
			sunxi_g2d_hw_disable(g2d);
		mutex_unlock(&g2d->dev_mutex);
	} else {
		/* Not idle yet — reschedule after 100 ms */
		queue_delayed_work(g2d->job_wq, &g2d->disable_work,
				   msecs_to_jiffies(100));
	}
}

/* ========== Async Job Queue Worker ========== */

/* Forward declarations for job execution helpers */
static int sunxi_g2d_do_fillrect_rcq(struct sunxi_g2d_dev *g2d,
				     struct g2d_rcq_mem *rcq,
				     dma_addr_t dst_dma, u32 width, u32 height,
				     u32 pitch, u32 color, u32 color_format,
				     u32 dst_format);

static int sunxi_g2d_do_blend_rcq(
	struct sunxi_g2d_dev *g2d, struct g2d_rcq_mem *rcq, dma_addr_t src_dma,
	u32 src_width, u32 src_height, u32 src_pitch, u32 src_format, u32 src_x,
	u32 src_y, u32 src_crop_w, u32 src_crop_h, dma_addr_t dst_dma,
	u32 dst_width, u32 dst_height, u32 dst_pitch, u32 dst_format, u32 dst_x,
	u32 dst_y, dma_addr_t out_dma, u32 out_width, u32 out_height,
	u32 out_pitch, u32 out_format, u32 out_x, u32 out_y, u32 blend_w,
	u32 blend_h, u32 bld_mode, u32 alpha_mode, u32 global_alpha,
	u32 premul_mode, u8 src_color_space, u8 dst_color_space,
	u8 out_color_space, struct g2d_csc_state *csc_state);

static int sunxi_g2d_do_blit_rot(
	struct sunxi_g2d_dev *g2d, struct g2d_rcq_mem *rcq, dma_addr_t src_dma,
	u32 src_width, u32 src_height, u32 src_pitch, u32 src_format,
	u32 src_crop_x, u32 src_crop_y, u32 src_crop_w, u32 src_crop_h,
	dma_addr_t dst_dma, u32 dst_width, u32 dst_height, u32 dst_pitch,
	u32 dst_format, u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h, u32 flags)
{
	u32 rot_ctl = ROT_CTL_EN;
	u32 rot_fmt = 0;
	dma_addr_t src_offset_dma, dst_offset_dma;
	u32 src_bpp, dst_bpp;
	u32 out_w, out_h;
	bool rotated_90_270;
	u32 src_uv_x_div = 1, src_uv_y_div = 1;
	u32 dst_uv_x_div = 1, dst_uv_y_div = 1;
	bool src_is_planar = false, dst_is_planar = false;
	bool src_is_semiplanar = false, dst_is_semiplanar = false;
	bool restore_mixer_irq = false;
	int ret = 0;

	dev_dbg(g2d->dev, "BLIT_ROT: src_dma=%pad dst_dma=%pad size=%ux%u->%ux%u flags=0x%x\n",
		 &src_dma, &dst_dma, src_width, src_height, dst_width, dst_height, flags);

	/* CRITICAL: Check for valid base address to prevent paging request errors */
	if (IS_ERR_OR_NULL(g2d->base)) {
		dev_err(g2d->dev, "BLIT_ROT: Invalid MMIO base address: %p\n", g2d->base);
		ret = -EIO;
		goto out;
	}

	/* Check for zero dimensions to prevent integer underflow in size calculations */
	if (src_width == 0 || src_height == 0 || dst_width == 0 || dst_height == 0) {
		dev_err(g2d->dev, "BLIT_ROT: Zero dimensions detected (src=%ux%u dst=%ux%u)\n",
			src_width, src_height, dst_width, dst_height);
		ret = -EINVAL;
		goto out;
	}
	if (src_crop_w == 0 || src_crop_h == 0 || dst_w == 0 || dst_h == 0) {
		dev_err(g2d->dev,
			"BLIT_ROT: Zero crop/output size (crop=%ux%u dst=%ux%u)\n",
			src_crop_w, src_crop_h, dst_w, dst_h);
		ret = -EINVAL;
		goto out;
	}

	/* Check for zero pitch */
	if (src_pitch == 0 || dst_pitch == 0) {
		dev_err(g2d->dev, "BLIT_ROT: Zero pitch detected (src=%u dst=%u)\n",
			src_pitch, dst_pitch);
		ret = -EINVAL;
		goto out;
	}

	/* Check for invalid DMA addresses (0 is likely invalid on this platform) */
	if (src_dma == 0 || dst_dma == 0) {
		dev_err(g2d->dev, "BLIT_ROT: Invalid DMA address (src=%pad dst=%pad)\n",
			&src_dma, &dst_dma);
		ret = -EFAULT;
		goto out;
	}

	/* Check for format mismatch (ROT doesn't support format conversion) */
	if (src_format != dst_format) {
		dev_warn_once(g2d->dev, "BLIT_ROT: Format conversion not supported (src=0x%x dst=0x%x)\n",
			      src_format, dst_format);
		ret = -EOPNOTSUPP;
		goto out;
	}

	/* Determine output dimensions based on rotation */
	rotated_90_270 =
		(flags & (G2D_BLIT_FLAG_ROTATE_90 | G2D_BLIT_FLAG_ROTATE_270));
	if (rotated_90_270) {
		/* 90° and 270° rotations swap width and height */
		out_w = src_crop_h;
		out_h = src_crop_w;
	} else {
		/* 0°, 180°, and flips keep dimensions */
		out_w = src_crop_w;
		out_h = src_crop_h;
	}

	/* Check for scaling (not supported) */
	if (dst_w != out_w || dst_h != out_h) {
		dev_warn_once(g2d->dev, "BLIT_ROT: Scaling not supported (out=%ux%u dst=%ux%u)\n",
			      out_w, out_h, dst_w, dst_h);
		ret = -EOPNOTSUPP;
		goto out;
	}

	/* Reset ROT/MIXER to a clean state before programming registers */
	{
		volatile struct g2d_top_reg *g2d_top =
			(volatile struct g2d_top_reg *)g2d->base;
		g2d_top->ahb_rst.bits.mixer_ahb_rst = 0;
		g2d_top->ahb_rst.bits.rot_ahb_rst = 0;
		wmb();
		g2d_top->ahb_rst.bits.mixer_ahb_rst = 1;
		g2d_top->ahb_rst.bits.rot_ahb_rst = 1;
		wmb();
	}

	/* Reset ROT block */
	g2d_write(g2d, ROT_CTL, 0x0);
	g2d_write(g2d, ROT_INT, ROT_INT_FINISH | ROT_INT_FINISH_EN);
	g2d_write(g2d, ROT_TIMEOUT, 0xFFFF);

	/* Disable MIXER to avoid interference */
	g2d_write(g2d, G2D_MIXER_CTL, 0x00000000);
	g2d_write(g2d, G2D_MIXER_INT, 0x00000000);
	wmb();
	restore_mixer_irq = true;

	/* Get ROT format codes and bpp using unified format function */
	int hw_src_fmt = sunxi_g2d_format_to_hw(src_format, &src_bpp);
	if (hw_src_fmt < 0) {
		dev_err(g2d->dev, "Unsupported ROT source format: %u\n",
			src_format);
		ret = -EOPNOTSUPP;
		goto out;
	}
	rot_fmt = (u32)hw_src_fmt; /* Hardware format code for ROT_IFMT */

	/* Destination format should match source */
	int hw_dst_fmt = sunxi_g2d_format_to_hw(dst_format, &dst_bpp);
	if (hw_dst_fmt < 0) {
		dev_err(g2d->dev, "Unsupported ROT destination format: %u\n",
			dst_format);
		ret = -EOPNOTSUPP;
		goto out;
	}
	src_is_planar = sunxi_g2d_is_yuv_planar(src_format);
	src_is_semiplanar = sunxi_g2d_is_yuv_semiplanar(src_format);
	dst_is_planar = sunxi_g2d_is_yuv_planar(dst_format);
	dst_is_semiplanar = sunxi_g2d_is_yuv_semiplanar(dst_format);
	sunxi_g2d_uv_div_factors(src_format, &src_uv_x_div, &src_uv_y_div);
	sunxi_g2d_uv_div_factors(dst_format, &dst_uv_x_div, &dst_uv_y_div);

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
	src_offset_dma = src_dma + (src_crop_y * src_pitch) + (src_crop_x * src_bpp);

	/* Calculate destination address with offset */
	dst_offset_dma = dst_dma + (dst_y * dst_pitch) + (dst_x * dst_bpp);

	/* Configure ROT input (source) - BSP format: (height-1 << 16) | width-1 */
	g2d_write(g2d, ROT_IFMT, rot_fmt);
	g2d_write(g2d, ROT_ISIZE, ((src_crop_h - 1) << 16) | (src_crop_w - 1));
	
	/* Configure ROT input with YUV multi-plane support */
	{
		u32 user_stride[3] = { src_pitch, 0, 0 };
		u32 src_stride[3], src_plane_offset[3];
		dma_addr_t plane1_dma, plane2_dma;
		
		sunxi_g2d_get_yuv_plane_info(src_format, src_width, src_height,
					     user_stride, src_stride, src_plane_offset);
		
		g2d_write(g2d, ROT_IPITCH0, src_stride[0]);
		g2d_write(g2d, ROT_IPITCH1, src_stride[1]);
		g2d_write(g2d, ROT_IPITCH2, src_stride[2]);
		
		g2d_write(g2d, ROT_ILADD0, lower_32_bits(src_offset_dma));
		g2d_write(g2d, ROT_IHADD0, upper_32_bits(src_offset_dma));
		
		if (src_is_planar || src_is_semiplanar) {
			u32 chroma_x = src_crop_x / src_uv_x_div;
			u32 chroma_y = src_crop_y / src_uv_y_div;

			plane1_dma = src_dma + src_plane_offset[1] +
				     (dma_addr_t)chroma_y * src_stride[1] +
				     (dma_addr_t)chroma_x *
					     (src_is_semiplanar ? 2 : 1);
			g2d_write(g2d, ROT_ILADD1, lower_32_bits(plane1_dma));
			g2d_write(g2d, ROT_IHADD1, upper_32_bits(plane1_dma));
			
			if (src_is_planar) {
				/* For planar, V plane has same geometry as U plane */
				plane2_dma = src_dma + src_plane_offset[2] +
					     (dma_addr_t)chroma_y *
						     src_stride[2] +
					     (dma_addr_t)chroma_x;
				g2d_write(g2d, ROT_ILADD2, lower_32_bits(plane2_dma));
				g2d_write(g2d, ROT_IHADD2, upper_32_bits(plane2_dma));
			} else {
				g2d_write(g2d, ROT_ILADD2, 0);
				g2d_write(g2d, ROT_IHADD2, 0);
			}
		} else {
			g2d_write(g2d, ROT_ILADD1, 0);
			g2d_write(g2d, ROT_IHADD1, 0);
			g2d_write(g2d, ROT_ILADD2, 0);
			g2d_write(g2d, ROT_IHADD2, 0);
		}
	}

	/* Configure ROT output (destination) */
	g2d_write(g2d, ROT_OSIZE, ((out_h - 1) << 16) | (out_w - 1));
	
	/* Configure ROT output with YUV multi-plane support */
	{
		u32 user_stride[3] = { dst_pitch, 0, 0 };
		u32 dst_stride[3], dst_plane_offset[3];
		dma_addr_t plane1_dma, plane2_dma;
		
		sunxi_g2d_get_yuv_plane_info(dst_format, dst_width, dst_height,
					     user_stride, dst_stride, dst_plane_offset);
		
		g2d_write(g2d, ROT_OPITCH0, dst_stride[0]);
		g2d_write(g2d, ROT_OPITCH1, dst_stride[1]);
		g2d_write(g2d, ROT_OPITCH2, dst_stride[2]);
		
		g2d_write(g2d, ROT_OLADD0, lower_32_bits(dst_offset_dma));
		g2d_write(g2d, ROT_OHADD0, upper_32_bits(dst_offset_dma));
		
		if (dst_is_planar || dst_is_semiplanar) {
			u32 chroma_x = dst_x / dst_uv_x_div;
			u32 chroma_y = dst_y / dst_uv_y_div;

			plane1_dma = dst_dma + dst_plane_offset[1] +
				     (dma_addr_t)chroma_y * dst_stride[1] +
				     (dma_addr_t)chroma_x *
					     (dst_is_semiplanar ? 2 : 1);
			g2d_write(g2d, ROT_OLADD1, lower_32_bits(plane1_dma));
			g2d_write(g2d, ROT_OHADD1, upper_32_bits(plane1_dma));
			
			if (dst_is_planar) {
				plane2_dma = dst_dma + dst_plane_offset[2] +
					     (dma_addr_t)chroma_y *
						     dst_stride[2] +
					     (dma_addr_t)chroma_x;
				g2d_write(g2d, ROT_OLADD2, lower_32_bits(plane2_dma));
				g2d_write(g2d, ROT_OHADD2, upper_32_bits(plane2_dma));
			} else {
				g2d_write(g2d, ROT_OLADD2, 0);
				g2d_write(g2d, ROT_OHADD2, 0);
			}
		} else {
			g2d_write(g2d, ROT_OLADD1, 0);
			g2d_write(g2d, ROT_OHADD1, 0);
			g2d_write(g2d, ROT_OLADD2, 0);
			g2d_write(g2d, ROT_OHADD2, 0);
		}
	}

	/* Clear interrupt status, ENABLE IRQ for async wait */
	g2d_write(g2d, ROT_INT, ROT_INT_FINISH | ROT_INT_FINISH_EN);
	atomic_set(&g2d->irq_done, 0);

	/* Write ROT_CTL with enable bit (BSP: write once at the beginning) */
	g2d_write(g2d, ROT_CTL, rot_ctl);
	wmb();

	/* Start ROT by setting bit 31 (BSP: read-modify-write) */
	rot_ctl = g2d_read(g2d, ROT_CTL);
	rot_ctl |= ROT_CTL_START;
	g2d_write(g2d, ROT_CTL, rot_ctl);
	wmb();

	/* Wait for completion (INTERRUPT) */
	{
		int timeout_jiffies = msecs_to_jiffies(1000); /* 1s timeout */
		int wait_result;

		wait_result = wait_event_interruptible_timeout(
			g2d->irq_wait, atomic_read(&g2d->irq_done),
			timeout_jiffies);
		
		if (wait_result <= 0) {
			u32 status = g2d_read(g2d, ROT_INT);
			u32 ctl = g2d_read(g2d, ROT_CTL);
			ret = wait_result ? -ERESTARTSYS : -ETIMEDOUT;
			
			dev_err(g2d->dev, "BLIT_ROT timeout (IRQ), status=0x%08x ctl=0x%08x ret=%d\n", 
				status, ctl, ret);
			goto out;
		}
	}

out:
	/* Restore mixer IRQ enable so RCQ path keeps receiving completion interrupts */
	if (restore_mixer_irq && !IS_ERR_OR_NULL(g2d->base)) {
		g2d_write(g2d, G2D_MIXER_INT,
			  G2D_MIXER_INT_FINISH_IRQ_EN |
				  G2D_MIXER_INT_IRQ_PENDING);
		wmb();
	}

	/* After MMIO rotation, fully reset mixer/rot to avoid corrupting next RCQ jobs.
	 * Do this even on error to keep hardware in a known state.
	 */
	if (!IS_ERR_OR_NULL(g2d->base)) {
		volatile struct g2d_top_reg *g2d_top =
			(volatile struct g2d_top_reg *)g2d->base;

		/* Clear RCQ interrupt/status just in case */
		writel(0, g2d->base + G2D_RCQ_IRQ_CTL);
		writel(G2D_RCQ_STATUS_TASK_END | G2D_RCQ_STATUS_CFG_FINISH,
		       g2d->base + G2D_RCQ_STATUS);
		/* Clear RCQ head registers to remove stale configuration */
		writel(0, g2d->base + G2D_RCQ_HEAD_LOW);
		writel(0, g2d->base + G2D_RCQ_HEAD_HIGH);
		writel(0, g2d->base + G2D_RCQ_HEAD_LEN);
		writel(0, g2d->base + G2D_RCQ_CTRL);

		g2d_top->ahb_rst.bits.mixer_ahb_rst = 0;
		g2d_top->ahb_rst.bits.rot_ahb_rst = 0;
		wmb();
		g2d_top->ahb_rst.bits.mixer_ahb_rst = 1;
		g2d_top->ahb_rst.bits.rot_ahb_rst = 1;
		wmb();

		/* Restore DRAM command control for RCQ DMA access */
		g2d_write(g2d, G2D_CMD_CTL, 0x00010011);

		/* Re-enable mixer finish IRQ */
		g2d_write(g2d, G2D_MIXER_INT,
			  G2D_MIXER_INT_FINISH_IRQ_EN |
				  G2D_MIXER_INT_IRQ_PENDING);
		wmb();
	}

	return ret;
}

/* RCQ-based ROT implementation to keep pipeline consistent with other ops */
static int sunxi_g2d_do_blit_rot_rcq(
	struct sunxi_g2d_dev *g2d, struct g2d_rcq_mem *rcq, dma_addr_t src_dma,
	u32 src_width, u32 src_height, u32 src_pitch, u32 src_format,
	u32 src_crop_x, u32 src_crop_y, u32 src_crop_w, u32 src_crop_h,
	dma_addr_t dst_dma, u32 dst_width, u32 dst_height, u32 dst_pitch,
	u32 dst_format, u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h, u32 flags)
{
	struct g2d_rot_reg *rot_regs = NULL;
	u32 rot_size = 0;
	u32 src_bpp, dst_bpp;
	u32 rot_mode = 0;
	u32 rot_hw_fmt;
	dma_addr_t src_offset_dma, dst_offset_dma;
	u32 out_w, out_h;
	int ret;

	/* Hardware on T113 ignores ROT writes through RCQ; fall back to MMIO */
	return -EOPNOTSUPP;

	dev_dbg(g2d->dev, "BLIT_ROT_RCQ: src_dma=%pad dst_dma=%pad size=%ux%u->%ux%u flags=0x%x\n",
		 &src_dma, &dst_dma, src_width, src_height, dst_width, dst_height, flags);

	if (!g2d || !rcq || !rcq->vir_addr || !g2d->rcq_enabled)
		return -EOPNOTSUPP;

	/* Validate basic params (mirror MMIO path) */
	if (src_format != dst_format)
		return -EOPNOTSUPP;
	if (!src_width || !src_height || !dst_width || !dst_height ||
	    !src_crop_w || !src_crop_h || !dst_w || !dst_h)
		return -EINVAL;
	if (!src_pitch || !dst_pitch)
		return -EINVAL;
	if (!src_dma || !dst_dma)
		return -EFAULT;

	/* Rotated output dimensions */
	if (flags & (G2D_BLIT_FLAG_ROTATE_90 | G2D_BLIT_FLAG_ROTATE_270)) {
		out_w = src_crop_h;
		out_h = src_crop_w;
	} else {
		out_w = src_crop_w;
		out_h = src_crop_h;
	}
	if (dst_w != out_w || dst_h != out_h)
		return -EOPNOTSUPP; /* no scaling */

	rot_hw_fmt = sunxi_g2d_format_to_hw(src_format, &src_bpp);
	if ((int)rot_hw_fmt < 0)
		return -EOPNOTSUPP;
	if (sunxi_g2d_format_to_hw(dst_format, &dst_bpp) < 0)
		return -EOPNOTSUPP;

	/* Limit RCQ ROT to packed formats for now */
	if (sunxi_g2d_is_yuv_planar(src_format) ||
	    sunxi_g2d_is_yuv_semiplanar(src_format))
		return -EOPNOTSUPP;

	/* Address with crop offsets */
	src_offset_dma = src_dma +
			 (dma_addr_t)src_crop_y * src_pitch +
			 (dma_addr_t)src_crop_x * src_bpp;
	dst_offset_dma = dst_dma +
			 (dma_addr_t)dst_y * dst_pitch +
			 (dma_addr_t)dst_x * dst_bpp;

	/* Build rot_mode flags for RCQ builder */
	if (flags & G2D_BLIT_FLAG_ROTATE_90)
		rot_mode |= G2D_ROT_90;
	else if (flags & G2D_BLIT_FLAG_ROTATE_180)
		rot_mode |= G2D_ROT_180;
	else if (flags & G2D_BLIT_FLAG_ROTATE_270)
		rot_mode |= G2D_ROT_270;
	if (flags & G2D_BLIT_FLAG_FLIP_H)
		rot_mode |= G2D_ROT_H;
	if (flags & G2D_BLIT_FLAG_FLIP_V)
		rot_mode |= G2D_ROT_V;

	sunxi_g2d_rcq_reset(rcq);

	ret = g2d_rcq_build_rot(src_crop_w, src_crop_h, src_pitch,
				src_offset_dma, src_format, dst_w, dst_h,
				dst_pitch, dst_offset_dma, dst_format, rot_mode,
				&rot_regs, &rot_size);
	if (ret)
		goto out;

	/* Program ROT registers block (start bit is 0 inside) */
	ret = sunxi_g2d_rcq_add_block(rcq, ROT_CTL, rot_regs, rot_size);
	if (ret)
		goto out;

	/* Add start write as a separate small block so start is last */
	{
		u32 rot_start = rot_regs->rot_ctrl.dwval | ROT_CTL_START;
		ret = sunxi_g2d_rcq_add_block(rcq, ROT_CTL, &rot_start,
					      sizeof(rot_start));
		if (ret)
			goto out;
	}

	/* Execute RCQ with polling on ROT_INT/task_end to avoid mixing MMIO path */
	{
		volatile struct g2d_top_reg *g2d_top =
			(volatile struct g2d_top_reg *)g2d->base;
		unsigned long timeout = jiffies + msecs_to_jiffies(200);
		u32 sts;

		/* Reset RCQ/mixer/rot blocks before launch */
		g2d_top->ahb_rst.bits.mixer_ahb_rst = 0;
		g2d_top->ahb_rst.bits.rot_ahb_rst = 0;
		wmb();
		g2d_top->ahb_rst.bits.mixer_ahb_rst = 1;
		g2d_top->ahb_rst.bits.rot_ahb_rst = 1;
		wmb();

		atomic_set(&g2d->irq_done, 0);
		g2d_write(g2d, G2D_RCQ_IRQ_CTL, G2D_RCQ_IRQ_TASK_END_EN);
		wmb();

		sunxi_g2d_rcq_setup_hw(g2d->base, rcq);
		sunxi_g2d_rcq_start(g2d->base, false, true);
		wmb();

		/* Poll for completion via ROT_INT or RCQ task_end */
		while (time_before(jiffies, timeout)) {
			u32 rot_int = g2d_read(g2d, ROT_INT);
			sts = g2d_read(g2d, G2D_RCQ_STATUS);

			if (rot_int & ROT_INT_FINISH) {
				g2d_write(g2d, ROT_INT, ROT_INT_FINISH);
				ret = 0;
				break;
			}
			if (sts & G2D_RCQ_STATUS_TASK_END) {
				/* Clear task_end */
				writel(G2D_RCQ_STATUS_TASK_END,
				       g2d->base + G2D_RCQ_STATUS);
				ret = 0;
				break;
			}
			cpu_relax();
		}

		if (time_after_eq(jiffies, timeout))
			ret = -ETIMEDOUT;

		/* Reset blocks after completion/timeout */
		g2d_top->ahb_rst.bits.mixer_ahb_rst = 0;
		g2d_top->ahb_rst.bits.rot_ahb_rst = 0;
		wmb();
		g2d_top->ahb_rst.bits.mixer_ahb_rst = 1;
		g2d_top->ahb_rst.bits.rot_ahb_rst = 1;
		wmb();
	}

out:
	kfree(rot_regs);
	return ret;
}

static int sunxi_g2d_execute_rcq(struct sunxi_g2d_dev *g2d,
				 struct g2d_rcq_mem *rcq)
{
	volatile struct g2d_top_reg *g2d_top;
	int ret = 0;

	if (!g2d || !rcq || !rcq->vir_addr)
		return -EINVAL;

	if (!g2d->rcq_enabled)
		return -EOPNOTSUPP;

	g2d_top = (volatile struct g2d_top_reg *)g2d->base;

	/* Reset RCQ/mixer block before launching a new task */
	g2d_top->ahb_rst.bits.mixer_ahb_rst = 0;
	g2d_top->ahb_rst.bits.rot_ahb_rst = 0;
	wmb();
	g2d_top->ahb_rst.bits.mixer_ahb_rst = 1;
	g2d_top->ahb_rst.bits.rot_ahb_rst = 1;
	wmb();

	/* Prepare IRQ */
	atomic_set(&g2d->irq_done, 0);
	{
		union g2d_rcq_irq_ctl irq_ctl;
		irq_ctl.dwval = 0;
		irq_ctl.bits.task_end_irq_en = 1;
		g2d_write(g2d, G2D_RCQ_IRQ_CTL, irq_ctl.dwval);
		wmb();
	}

	/* Ensure MIXER finish IRQ is enabled (rotation path temporarily disables it) */
	g2d_write(g2d, G2D_MIXER_INT,
		  G2D_MIXER_INT_FINISH_IRQ_EN | G2D_MIXER_INT_IRQ_PENDING);
	wmb();

	/* Program RCQ HEAD/LEN and start */
	sunxi_g2d_rcq_setup_hw(g2d->base, rcq);
	sunxi_g2d_rcq_start(g2d->base, false, true);
	wmb();

	/* Wait for completion */
	{
		int timeout_jiffies = msecs_to_jiffies(100);
		int wait_result;

		wait_result = wait_event_interruptible_timeout(
			g2d->irq_wait, atomic_read(&g2d->irq_done),
			timeout_jiffies);
		if (wait_result <= 0) {
			ret = wait_result ? -ERESTARTSYS : -ETIMEDOUT;
			/* Fallback: if task_end already set, treat as success */
			if (g2d_read(g2d, G2D_RCQ_STATUS) &
			    G2D_RCQ_STATUS_TASK_END) {
				/* Clear task_end and continue */
				writel(G2D_RCQ_STATUS_TASK_END,
				       g2d->base + G2D_RCQ_STATUS);
				ret = 0;
			}
			dev_err(g2d->dev,
				"RCQ execution timeout or interrupted: %d\n",
				ret);
			
			/* Debug: Dump ROT registers to diagnose timeout */
			{
				u32 rot_ctl = readl(g2d->base + G2D_ROT);
				u32 rot_int = readl(g2d->base + ROT_INT);
				u32 rot_isize = readl(g2d->base + ROT_ISIZE);
				u32 rot_ifmt = readl(g2d->base + ROT_IFMT);
				u32 cmd_ctl = readl(g2d->base + G2D_CMD_CTL);
				u32 sclk = readl(g2d->base + G2D_SCLK_GATE);
				u32 rcq_status = readl(g2d->base + G2D_RCQ_STATUS);
				dev_err(g2d->dev, "DEBUG: Timeout! ROT_CTL=0x%08x ROT_INT=0x%08x ROT_ISIZE=0x%08x ROT_IFMT=0x%08x\n",
					rot_ctl, rot_int, rot_isize, rot_ifmt);
				dev_err(g2d->dev, "DEBUG: CMD_CTL=0x%08x SCLK=0x%08x RCQ_STS=0x%08x\n",
					cmd_ctl, sclk, rcq_status);
			}

			/* Try to recover from a hung RCQ so subsequent jobs don't get stuck */
			g2d_write(g2d, G2D_RCQ_CTRL, 0);
			g2d_write(g2d, G2D_RCQ_IRQ_CTL, 0);
			g2d_write(g2d, G2D_RCQ_STATUS,
				  G2D_RCQ_STATUS_TASK_END | G2D_RCQ_STATUS_CFG_FINISH);
			g2d_write(g2d, G2D_MIXER_INT, G2D_MIXER_INT_IRQ_PENDING);
			g2d_write(g2d, G2D_MIXER_CTL, G2D_MIXER_CTL_RESET);
			wmb();

			g2d_top->ahb_rst.bits.mixer_ahb_rst = 0;
			g2d_top->ahb_rst.bits.rot_ahb_rst = 0;
			wmb();
			g2d_top->ahb_rst.bits.mixer_ahb_rst = 1;
			g2d_top->ahb_rst.bits.rot_ahb_rst = 1;
			wmb();

			/* Re-arm DMA and mixer IRQ after recovery */
			g2d_write(g2d, G2D_CMD_CTL, 0x00010011);
			g2d_write(g2d, G2D_MIXER_INT,
				  G2D_MIXER_INT_FINISH_IRQ_EN | G2D_MIXER_INT_IRQ_PENDING);
			wmb();
		}
	}

	/* Reset after completion to keep mixer clean for next task */
	g2d_top->ahb_rst.bits.mixer_ahb_rst = 0;
	g2d_top->ahb_rst.bits.rot_ahb_rst = 0;
	wmb();
	g2d_top->ahb_rst.bits.mixer_ahb_rst = 1;
	g2d_top->ahb_rst.bits.rot_ahb_rst = 1;
	wmb();

	return ret;
}



/**
 * sunxi_g2d_job_worker - Workqueue worker for async job processing
 * @work: work_struct embedded in sunxi_g2d_dev
 * 
 * Dequeues jobs from job_queue and executes them without blocking.
 * When HW completes, IRQ handler will signal the fence and schedule next job.
 */
static void sunxi_g2d_job_worker(struct work_struct *work)
{
	struct sunxi_g2d_dev *g2d =
		container_of(work, struct sunxi_g2d_dev, job_work);
	struct sunxi_g2d_job *job = NULL;
	unsigned long flags;
	int ret;

	/* Dequeue next job under lock */
	spin_lock_irqsave(&g2d->job_lock, flags);

	/* Don't start new job if one is already running */
	if (g2d->current_job) {
		spin_unlock_irqrestore(&g2d->job_lock, flags);
		return;
	}

	/* Check if queue has jobs */
	if (list_empty(&g2d->job_queue)) {
		spin_unlock_irqrestore(&g2d->job_lock, flags);
		return;
	}

	/* Dequeue first job */
	job = list_first_entry(&g2d->job_queue, struct sunxi_g2d_job, node);
	list_del(&job->node);
	g2d->current_job = job;
	atomic64_inc(&g2d->jobs_submitted);

	spin_unlock_irqrestore(&g2d->job_lock, flags);

	dev_dbg(g2d->dev, "job_worker: executing job=%p type=%d fence=%p\n",
		job, job->type, job->fence);

	if (!job->rcq_ready || !job->rcq.vir_addr) {
		dev_err(g2d->dev,
			"job_worker: RCQ not prepared for job type %d\n",
			job->type);
		dma_fence_set_error(job->fence, -EINVAL);
		dma_fence_signal(job->fence);

		spin_lock_irqsave(&g2d->job_lock, flags);
		g2d->current_job = NULL;
		atomic64_inc(&g2d->jobs_failed);
		spin_unlock_irqrestore(&g2d->job_lock, flags);

		queue_work(g2d->job_wq, &job->cleanup_work);
		queue_work(g2d->job_wq, &g2d->job_work);
		return;
	}

	/* Execute RCQ or MMIO based on job type */
	if (job->type == G2D_JOB_CMD_ROTATE) {
		/* Prefer RCQ rotation; fallback to MMIO if not supported */
		ret = sunxi_g2d_do_blit_rot_rcq(
			g2d, &job->rcq, job->src_dma, job->data.blit.src_width,
			job->data.blit.src_height, job->data.blit.src_pitch,
			job->data.blit.src_format, job->data.blit.src_crop_x,
			job->data.blit.src_crop_y, job->data.blit.src_crop_w,
			job->data.blit.src_crop_h, job->dst_dma,
			job->data.blit.dst_width, job->data.blit.dst_height,
			job->data.blit.dst_pitch, job->data.blit.dst_format,
			job->data.blit.dst_x, job->data.blit.dst_y,
			job->data.blit.dst_w, job->data.blit.dst_h,
			job->data.blit.flags);
		if (ret == -EOPNOTSUPP) {
			ret = sunxi_g2d_do_blit_rot(
				g2d, &job->rcq, job->src_dma,
				job->data.blit.src_width,
				job->data.blit.src_height,
				job->data.blit.src_pitch,
				job->data.blit.src_format,
				job->data.blit.src_crop_x,
				job->data.blit.src_crop_y,
				job->data.blit.src_crop_w,
				job->data.blit.src_crop_h, job->dst_dma,
				job->data.blit.dst_width,
				job->data.blit.dst_height,
				job->data.blit.dst_pitch,
				job->data.blit.dst_format,
				job->data.blit.dst_x, job->data.blit.dst_y,
				job->data.blit.dst_w, job->data.blit.dst_h,
				job->data.blit.flags);
		}
	} else {
		/* Use RCQ for everything else */
		ret = sunxi_g2d_execute_rcq(g2d, &job->rcq);
	}

	if (ret < 0) {
		/* Signal failure and cleanup */
		dma_fence_set_error(job->fence, ret);
		dma_fence_signal(job->fence);

		spin_lock_irqsave(&g2d->job_lock, flags);
		g2d->current_job = NULL;
		atomic64_inc(&g2d->jobs_failed);
		spin_unlock_irqrestore(&g2d->job_lock, flags);

		queue_work(g2d->job_wq, &job->cleanup_work);
		queue_work(g2d->job_wq, &g2d->job_work);
	} else if (job->type == G2D_JOB_CMD_ROTATE) {
		/* For MMIO rotation, we are DONE here (synchronous execution).
		 * We must signal completion manually because there is no IRQ handler
		 * logic for MMIO completion that signals the fence (sunxi_g2d_do_blit_rot
		 * waits for IRQ internally but doesn't signal the fence).
		 */
		dma_fence_signal(job->fence);

		spin_lock_irqsave(&g2d->job_lock, flags);
		g2d->current_job = NULL;
		/* atomic64_inc(&g2d->jobs_completed); */
		spin_unlock_irqrestore(&g2d->job_lock, flags);

		queue_work(g2d->job_wq, &job->cleanup_work);
		queue_work(g2d->job_wq, &g2d->job_work);
	}

	/* On success (RCQ), HW is now running. IRQ handler will:
	 *  1. Signal job->fence
	 *  2. Schedule cleanup_work
	 *  3. Schedule next job_work
	 */
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
	g2d_mixer =
		(volatile struct g2d_mixer_glb_reg *)(g2d->base + G2D_MIXER);

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

		dev_dbg(g2d->dev,
			"RCQ IRQ: task_end! status before=0x%08x after=0x%08x\n",
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

		dev_dbg(g2d->dev, "MIXER IRQ: completion! mixer_int=0x%08x\n",
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

		handled = true;
		dev_dbg(g2d->dev, "ROT IRQ received: status=0x%08x\n",
			rot_status);
		atomic_set(&g2d->irq_done, 1);
		wake_up(&g2d->irq_wait);
	}

	if (!handled)
		return IRQ_NONE;

	/* Update statistics */
	if (g2d->current_job) {
		/* For ROTATE jobs (MMIO), the job_worker waits synchronously for the IRQ.
		 * We must NOT handle completion/cleanup here, otherwise we race with
		 * job_worker and cause Use-After-Free or double-free.
		 * Just return handled (we already woke up the waiter above).
		 */
		if (g2d->current_job->type == G2D_JOB_CMD_ROTATE)
			return IRQ_HANDLED;

		/* Signal user fence (if any) and install user FD for sync_file */
		{
			struct sunxi_g2d_job *job;

			spin_lock(&g2d->job_lock);
			job = g2d->current_job;
			/* Detach current job so next operations can proceed */
			g2d->current_job = NULL;
			spin_unlock(&g2d->job_lock);

			if (job) {
				/* CRITICAL: Synchronize DMA buffer for CPU access AFTER G2D writes.
				 * With IOMMU enabled, addresses are IOVAs and cache coherency is NOT
				 * automatic. Must invalidate CPU caches BEFORE signaling fence to
				 * ensure CPU reads fresh data written by G2D hardware.
				 */
				if (job->dst_sgt) {
					dev_dbg(
						g2d->dev,
						"IRQ: cache sync executing (dst_sgt=%p nents=%u)\n",
						job->dst_sgt,
						job->dst_sgt->nents);
					dma_sync_sg_for_cpu(g2d->dev,
							    job->dst_sgt->sgl,
							    job->dst_sgt->nents,
							    DMA_FROM_DEVICE);
					/* Ensure all writes are visible before signaling fence */
					wmb();
					dev_dbg(g2d->dev,
						 "IRQ: cache sync completed\n");
				} else {
					dev_warn(
						g2d->dev,
						"IRQ: dst_sgt NULL, cache sync SKIPPED!\n");
				}

				/* Signal the fence unless we must defer due to writeback staging */
				if (job->fence && !job->wb_out_active) {
					struct sunxi_g2d_fence *sf =
						container_of(
							job->fence,
							struct sunxi_g2d_fence,
							base);
					dev_dbg(g2d->dev,
						"IRQ: signaling fence seq=%llu fd=%d\n",
						sf->seqno, job->fence_fd);
					dma_fence_signal(job->fence);
				}

				/* FD installation is performed in process context when the job
				 * is created (IOCTL). IRQ context MUST NOT touch process fd
				 * tables or struct file objects — see mitigation. At this
				 * point the fd should already be installed and job->sync_file
				 * will be NULL if installation succeeded.
				 */

				atomic64_inc(&g2d->jobs_done);

				/* Schedule cleanup in workqueue to avoid freeing in IRQ context */
				INIT_WORK(&job->cleanup_work,
					  sunxi_g2d_job_cleanup_workfn);
				if (!queue_work(g2d->job_wq,
						&job->cleanup_work)) {
					/* If queueing failed, perform cleanup synchronously (fallback) */
					sunxi_g2d_job_cleanup_workfn(
						&job->cleanup_work);
				}

				/* Schedule next job if queue not empty */
				queue_work(g2d->job_wq, &g2d->job_work);

				/* If there are no users, schedule immediate disable check to speed shutdown */
				if (atomic_read(&g2d->users) == 0) {
					unsigned long __flags;
					spin_lock_irqsave(&g2d->job_lock,
							  __flags);
					if (list_empty(&g2d->job_queue) &&
					    g2d->current_job == NULL)
						queue_delayed_work(
							g2d->job_wq,
							&g2d->disable_work, 0);
					spin_unlock_irqrestore(&g2d->job_lock,
							       __flags);
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
 * Returns: Hardware format value, or -EOPNOTSUPP if unsupported
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
		hw_fmt =
			G2D_FORMAT_ABGR8888; /* 0x01 - Swapped to fix R/B inversion */
		bytes_pp = 4;
		break;
	case G2D_FMT_ABGR8888:
		hw_fmt =
			G2D_FORMAT_ARGB8888; /* 0x00 - Swapped to fix R/B inversion */
		bytes_pp = 4;
		break;
	case G2D_FMT_RGBA8888:
		hw_fmt =
			G2D_FORMAT_BGRA8888; /* 0x03 - Swapped to fix R/B inversion */
		bytes_pp = 4;
		break;
	case G2D_FMT_BGRA8888:
		hw_fmt =
			G2D_FORMAT_RGBA8888; /* 0x02 - Swapped to fix R/B inversion */
		bytes_pp = 4;
		break;
	case G2D_FMT_XRGB8888:
		hw_fmt =
			G2D_FORMAT_XBGR8888; /* 0x05 - Swapped to fix R/B inversion */
		bytes_pp = 4;
		break;
	case G2D_FMT_XBGR8888:
		hw_fmt =
			G2D_FORMAT_XRGB8888; /* 0x04 - Swapped to fix R/B inversion */
		bytes_pp = 4;
		break;
	case G2D_FMT_RGBX8888:
		hw_fmt =
			G2D_FORMAT_BGRX8888; /* 0x07 - Swapped to fix R/B inversion */
		bytes_pp = 4;
		break;
	case G2D_FMT_BGRX8888:
		hw_fmt =
			G2D_FORMAT_RGBX8888; /* 0x06 - Swapped to fix R/B inversion */
		bytes_pp = 4;
		break;

	/* RGB formats - 24bpp */
	case G2D_FMT_RGB888:
		hw_fmt = G2D_FORMAT_RGB888; /* 0x08 */
		bytes_pp = 3;
		break;
	case G2D_FMT_BGR888:
		hw_fmt = G2D_FORMAT_BGR888; /* 0x09 */
		bytes_pp = 3;
		break;

	/* RGB formats - 16bpp */
	case G2D_FMT_RGB565:
		hw_fmt = G2D_FORMAT_RGB565; /* 0x0A */
		bytes_pp = 2;
		break;
	case G2D_FMT_BGR565:
		hw_fmt = G2D_FORMAT_BGR565; /* 0x0B */
		bytes_pp = 2;
		break;

	/* RGB formats - 16bpp with alpha */
	case G2D_FMT_ARGB4444:
		hw_fmt = G2D_FORMAT_ARGB4444; /* 0x0C */
		bytes_pp = 2;
		break;
	case G2D_FMT_ABGR4444:
		hw_fmt = G2D_FORMAT_ABGR4444; /* 0x0D */
		bytes_pp = 2;
		break;
	case G2D_FMT_RGBA4444:
		hw_fmt = G2D_FORMAT_RGBA4444; /* 0x0E */
		bytes_pp = 2;
		break;
	case G2D_FMT_BGRA4444:
		hw_fmt = G2D_FORMAT_BGRA4444; /* 0x0F */
		bytes_pp = 2;
		break;
	case G2D_FMT_ARGB1555:
		hw_fmt = G2D_FORMAT_ARGB1555; /* 0x10 */
		bytes_pp = 2;
		break;
	case G2D_FMT_ABGR1555:
		hw_fmt = G2D_FORMAT_ABGR1555; /* 0x11 */
		bytes_pp = 2;
		break;
	case G2D_FMT_RGBA5551:
		hw_fmt = G2D_FORMAT_RGBA5551; /* 0x12 */
		bytes_pp = 2;
		break;
	case G2D_FMT_BGRA5551:
		hw_fmt = G2D_FORMAT_BGRA5551; /* 0x13 */
		bytes_pp = 2;
		break;

	/* YUV formats - Interleaved (packed) 422 */
	case G2D_FMT_YUV422_I_YVYU:
		hw_fmt = G2D_FORMAT_IYUV422_V0Y1U0Y0; /* 0x20 */
		bytes_pp = 2; /* 2 bytes per pixel (averaged) */
		break;
	case G2D_FMT_YUV422_I_YUYV:
		hw_fmt = G2D_FORMAT_IYUV422_Y1V0Y0U0; /* 0x21 */
		bytes_pp = 2;
		break;
	case G2D_FMT_YUV422_I_UYVY:
		hw_fmt = G2D_FORMAT_IYUV422_U0Y1V0Y0; /* 0x22 */
		bytes_pp = 2;
		break;
	case G2D_FMT_YUV422_I_VYUY:
		hw_fmt = G2D_FORMAT_IYUV422_Y1U0Y0V0; /* 0x23 */
		bytes_pp = 2;
		break;

	/* YUV formats - Semi-planar 422 */
	case G2D_FMT_YUV422_SP_UVUV:
		hw_fmt = G2D_FORMAT_YUV422UVC_V1U1V0U0; /* 0x24 */
		bytes_pp = 1; /* Y plane: 1 byte per pixel */
		break;
	case G2D_FMT_YUV422_SP_VUVU:
		hw_fmt = G2D_FORMAT_YUV422UVC_U1V1U0V0; /* 0x25 */
		bytes_pp = 1;
		break;

	/* YUV formats - Planar 422 */
	case G2D_FMT_YUV422_P:
		hw_fmt = G2D_FORMAT_YUV422_PLANAR; /* 0x26 */
		bytes_pp = 1; /* Y plane: 1 byte per pixel */
		break;

	/* YUV formats - Semi-planar 420 */
	case G2D_FMT_YUV420_SP_UVUV:
		hw_fmt = G2D_FORMAT_YUV420UVC_V1U1V0U0; /* 0x28 */
		bytes_pp = 1;
		break;
	case G2D_FMT_YUV420_SP_VUVU:
		hw_fmt = G2D_FORMAT_YUV420UVC_U1V1U0V0; /* 0x29 */
		bytes_pp = 1;
		break;

	/* YUV formats - Planar 420 (I420/YV12) */
	case G2D_FMT_YUV420_P:
	case G2D_FMT_YUV420_P_VU:
		/* NOTE: Hardware expects planar order Y,U,V with strides: Y=pitch, U=pitch/2, V=pitch/2.
		 * Artifact hypothesis: misinterpretation of VSU path when planar 420 used sin scaler identity.
		 * Diagnostic: allow optional remap to semi-planar UV (NV12) for experiment via module param. */
		hw_fmt = G2D_FORMAT_YUV420_PLANAR; /* 0x2A */
		bytes_pp = 1;
		break;

	/* YUV formats - Semi-planar 411 */
	case G2D_FMT_YUV411_SP_UVUV:
		hw_fmt = G2D_FORMAT_YUV411UVC_V1U1V0U0; /* 0x2C */
		bytes_pp = 1;
		break;
	case G2D_FMT_YUV411_SP_VUVU:
		hw_fmt = G2D_FORMAT_YUV411UVC_U1V1U0V0; /* 0x2D */
		bytes_pp = 1;
		break;

	/* YUV formats - Planar 411 */
	case G2D_FMT_YUV411_P:
		hw_fmt = G2D_FORMAT_YUV411_PLANAR; /* 0x2E */
		bytes_pp = 1;
		break;

	/* Monochrome formats */
	case G2D_FMT_8BPP_MONO:
		hw_fmt = G2D_FORMAT_Y8; /* 0x30 - Grayscale */
		bytes_pp = 1;
		break;

	default:
		return -EOPNOTSUPP;
	}

	if (bpp)
		*bpp = bytes_pp;

	return hw_fmt;
}

/**
 * sunxi_g2d_is_yuv_format - Check if format is YUV
 * @fmt: UAPI pixel format
 * 
 * Returns: true if YUV format (>= 0x20), false otherwise
 */
static inline bool sunxi_g2d_is_yuv_format(u32 fmt)
{
	return fmt >= G2D_FMT_YUV422_I_YVYU;
}

/**
 * sunxi_g2d_is_yuv_planar - Check if format is YUV planar (3 separate planes)
 * @fmt: UAPI pixel format
 * 
 * Returns: true if planar YUV (Y, U, V in separate planes), false otherwise
 */
static inline bool sunxi_g2d_is_yuv_planar(u32 fmt)
{
	return (fmt == G2D_FMT_YUV420_P || fmt == G2D_FMT_YUV420_P_VU ||
		fmt == G2D_FMT_YUV422_P || fmt == G2D_FMT_YUV411_P);
}

/**
 * sunxi_g2d_is_yuv_semiplanar - Check if format is YUV semi-planar (Y + UV)
 * @fmt: UAPI pixel format
 * 
 * Returns: true if semi-planar YUV (Y plane + interleaved UV), false otherwise
 */
static inline bool sunxi_g2d_is_yuv_semiplanar(u32 fmt)
{
	return ((fmt >= G2D_FMT_YUV422_SP_UVUV &&
		 fmt <= G2D_FMT_YUV422_SP_VUVU) ||
		(fmt >= G2D_FMT_YUV420_SP_UVUV &&
		 fmt <= G2D_FMT_YUV420_SP_VUVU) ||
		(fmt >= G2D_FMT_YUV411_SP_UVUV &&
		 fmt <= G2D_FMT_YUV411_SP_VUVU));
}

/**
 * sunxi_g2d_get_yuv_plane_info - Calculate YUV plane strides and offsets
 * @fmt: UAPI pixel format
 * @width: Image width in pixels
 * @height: Image height in pixels
 * @user_stride: User-provided stride array (can be NULL for auto-calculation)
 * @stride: Output array for plane strides [3]
 * @plane_offset: Output array for plane byte offsets from base [3]
 * 
 * Calculates stride and offset for each plane in YUV formats.
 * For RGB formats, only stride[0] is set.
 * 
 * YUV420P layout example (1920x1080):
 *   Plane 0 (Y):  1920x1080 bytes, stride=1920, offset=0
 *   Plane 1 (U):  960x540 bytes, stride=960, offset=1920*1080
 *   Plane 2 (V):  960x540 bytes, stride=960, offset=1920*1080+960*540
 */
static void sunxi_g2d_get_yuv_plane_info(u32 fmt, u32 width, u32 height,
					 const u32 *user_stride, u32 stride[3],
					 u32 plane_offset[3])
{
	/* Initialize to zero */
	stride[0] = stride[1] = stride[2] = 0;
	plane_offset[0] = plane_offset[1] = plane_offset[2] = 0;

	/* Use user-provided stride if available, otherwise calculate */
	if (sunxi_g2d_is_yuv_planar(fmt)) {
		/* Planar YUV: Y, U, V in separate planes */

		/* Plane 0: Y (full resolution) */
		stride[0] = user_stride && user_stride[0] ? user_stride[0] :
							    width;
		plane_offset[0] = 0;

		if (fmt == G2D_FMT_YUV420_P) {
			/* YUV420 Planar (I420): Y, U, V */
			stride[1] = user_stride && user_stride[1] ?
					    user_stride[1] :
					    (width / 2);
			stride[2] = user_stride && user_stride[2] ?
					    user_stride[2] :
					    (width / 2);
			
			u32 y_size = stride[0] * height;
			u32 u_size = stride[1] * (height / 2);
			
			/* Plane 1 (LADD1) -> U (2nd block) */
			plane_offset[1] = y_size;
			/* Plane 2 (LADD2) -> V (3rd block) */
			plane_offset[2] = y_size + u_size;
			
		} else if (fmt == G2D_FMT_YUV420_P_VU) {
			/* YUV420 Planar (YV12): Y, V, U */
			u32 cstride = user_stride && user_stride[1] ?
					      user_stride[1] :
					      (width / 2);
			stride[1] = cstride; /* V stride (Plane 1) */
			stride[2] = user_stride && user_stride[2] ?
					    user_stride[2] :
					    (width / 2); /* U stride (Plane 2) */
			
			u32 y_size = stride[0] * height;
			u32 v_size = stride[1] * (height / 2);
			
			/* Plane 1 (LADD1) -> U (3rd block) */
			plane_offset[1] = y_size + v_size;
			/* Plane 2 (LADD2) -> V (2nd block) */
			plane_offset[2] = y_size;
		} else if (fmt == G2D_FMT_YUV422_P) {
			/* YUV422: U/V planes are 1/2 width, full height */
			stride[1] = user_stride && user_stride[1] ?
					    user_stride[1] :
					    (width / 2);
			stride[2] = user_stride && user_stride[2] ?
					    user_stride[2] :
					    (width / 2);
			plane_offset[1] = stride[0] * height;
			plane_offset[2] = plane_offset[1] + stride[1] * height;
		} else if (fmt == G2D_FMT_YUV411_P) {
			/* YUV411: U/V planes are 1/4 width, full height */
			stride[1] = user_stride && user_stride[1] ?
					    user_stride[1] :
					    (width / 4);
			stride[2] = user_stride && user_stride[2] ?
					    user_stride[2] :
					    (width / 4);
			plane_offset[1] = stride[0] * height;
			plane_offset[2] = plane_offset[1] + stride[1] * height;
		}
	} else if (sunxi_g2d_is_yuv_semiplanar(fmt)) {
		/* Semi-planar YUV: Y plane + interleaved UV plane */

		/* Plane 0: Y (full resolution) */
		stride[0] = user_stride && user_stride[0] ? user_stride[0] :
							    width;
		plane_offset[0] = 0;

		if (fmt >= G2D_FMT_YUV420_SP_UVUV &&
		    fmt <= G2D_FMT_YUV420_SP_VUVU) {
			/* YUV420: UV plane is full width (interleaved), 1/2 height */
			stride[1] = user_stride && user_stride[1] ?
					    user_stride[1] :
					    width;
			plane_offset[1] = stride[0] * height;
		} else if (fmt >= G2D_FMT_YUV422_SP_UVUV &&
			   fmt <= G2D_FMT_YUV422_SP_VUVU) {
			/* YUV422: UV plane is full width (interleaved), full height */
			stride[1] = user_stride && user_stride[1] ?
					    user_stride[1] :
					    width;
			plane_offset[1] = stride[0] * height;
		} else if (fmt >= G2D_FMT_YUV411_SP_UVUV &&
			   fmt <= G2D_FMT_YUV411_SP_VUVU) {
			/* YUV411: UV plane is 1/2 width (interleaved), full height */
			stride[1] = user_stride && user_stride[1] ?
					    user_stride[1] :
					    (width / 2);
			plane_offset[1] = stride[0] * height;
		}
	} else {
		/* RGB or packed YUV: single plane */
		if (user_stride && user_stride[0]) {
			/* User provided stride, use it */
			stride[0] = user_stride[0];
		} else {
			/* Calculate stride based on format */
			u32 bpp = 4; /* Default to 4 bytes per pixel */
			sunxi_g2d_format_to_hw(fmt, &bpp);
			stride[0] = width * bpp;
		}
		plane_offset[0] = 0;
	}
}

/**
 * sunxi_g2d_do_fillrect_rcq - Perform fill rectangle using RCQ modular builder
 * @g2d: G2D device
 * @dst_dma: Destination DMA address
 * @width: Rectangle width
 * @height: Rectangle height
 * @pitch: Destination pitch (bytes per line)
 * @color: Fill color value
 * @color_format: Fill color format (UAPI g2d_pixel_format)
 * @dst_format: Destination pixel format (UAPI g2d_pixel_format)
 * 
 * Builds and executes a fill rectangle operation using the RCQ modular
 * builder functions. Configures V0 layer for fill color and dummy layers
 * for UI and scaler.
 * 
 * Returns 0 on success or negative error code.
 */
static int sunxi_g2d_do_fillrect_rcq(struct sunxi_g2d_dev *g2d,
				     struct g2d_rcq_mem *rcq,
				     dma_addr_t dst_dma, u32 width, u32 height,
				     u32 pitch, u32 color, u32 color_format,
				     u32 dst_format)
{
	struct sunxi_g2d_rcq_frame_layout layout;
	int color_fmt_val, dst_fmt_val;
	int ret;

	/* Blocks allocated by builders (must kfree at end) */
	u32 *v0_regs = NULL, v0_size;
	u32 *u0_regs = NULL, u0_size;
	u32 *u1_regs = NULL, u1_size;
	u32 *u2_regs = NULL, u2_size;
	u32 *scal_regs = NULL, scal_size;
	struct g2d_mixer_bld_reg *bld_regs = NULL;
	u32 bld_size;
	u32 *wb_regs = NULL, wb_size;

	/* Convert formats. For the fill color format we preserve the value
	 * provided by the caller (UAPI `g2d_pixel_format`) and pass it through
	 * to the RCQ builder/packers. The destination format still needs to be
	 * converted to the hardware enum via sunxi_g2d_format_to_hw().
	 *
	 * Rationale: fillrect color semantics differ from memory-layer formats
	 * because the color is written via the V0 fillcolor register and the
	 * byte-order handling is performed conditionally below (swap only for
	 * framebuffer XBGR). Passing the UAPI format here avoids forcing an
	 * implicit ARGB convention on userspace callers.
	 */
	color_fmt_val = (int)color_format;
	dst_fmt_val = sunxi_g2d_format_to_hw(dst_format, NULL);
	if (dst_fmt_val < 0)
		return -EOPNOTSUPP;

	dev_dbg(
		g2d->dev,
		"FILLRECT_RCQ(modular): %ux%u color=0x%08x fmt=%u->0x%02X dst_fmt=%u->0x%02X\n",
		width, height, color, color_format, color_fmt_val, dst_format,
		dst_fmt_val);

	/* Convert color value ONLY when writing to framebuffer (XBGR destination)
	 * For ION/G2D buffers (ABGR): DON'T swap. The fill_color writes to memory via
	 * writeback, and little-endian byte order automatically produces the correct
	 * layout for subsequent reads.
	 * For framebuffer (XBGR): DO swap. The writeback format differs from V0 format,
	 * requiring explicit byte swap.
	 */
	if (dst_fmt_val == G2D_FORMAT_XBGR8888) {
		color = (color & 0xFF00FF00) | ((color & 0x00FF0000) >> 16) |
			((color & 0x000000FF) << 16);
		dev_dbg(g2d->dev, "FILLRECT_RCQ: color swapped for XBGR framebuffer: 0x%08x\n", color);
	}

	if (!g2d->rcq_enabled || !rcq || !rcq->vir_addr)
		return -EOPNOTSUPP;

	/* Reset RCQ buffer */
	sunxi_g2d_rcq_reset(rcq);

	/* ========== BUILD BLOCKS USING MODULAR FUNCTIONS ========== */

	/* Block 0: V0 (fill color) - ACTIVE */
	ret = g2d_rcq_build_v0_fillcolor(width, height, pitch, color,
					 color_fmt_val, &v0_regs, &v0_size);
	if (ret) {
		dev_err(g2d->dev, "Failed to build V0 block: %d\n", ret);
		goto cleanup;
	}

	/* Blocks 1-3: U0, U1, U2 (dummy UI layers) - INACTIVE */
	ret = g2d_rcq_build_ui_dummy(&u0_regs, &u0_size);
	if (ret) {
		dev_err(g2d->dev, "Failed to build U0 block: %d\n", ret);
		goto cleanup;
	}
	ret = g2d_rcq_build_ui_dummy(&u1_regs, &u1_size);
	if (ret) {
		dev_err(g2d->dev, "Failed to build U1 block: %d\n", ret);
		goto cleanup;
	}
	ret = g2d_rcq_build_ui_dummy(&u2_regs, &u2_size);
	if (ret) {
		dev_err(g2d->dev, "Failed to build U2 block: %d\n", ret);
		goto cleanup;
	}

	/* Block 4: SCAL (dummy scaler) - INACTIVE */
	ret = g2d_rcq_build_scaler_dummy(&scal_regs, &scal_size);
	if (ret) {
		dev_err(g2d->dev, "Failed to build SCAL block: %d\n", ret);
		goto cleanup;
	}

	/* Block 5: BLD (blender with fill color) - ACTIVE */
	ret = g2d_rcq_build_bld_fillcolor(width, height, color,
					  0x03010301, /* SRCOVER Porter-Duff */
					  &bld_regs, &bld_size);
	if (ret) {
		dev_err(g2d->dev, "Failed to build BLD block: %d\n", ret);
		goto cleanup;
	}

	/* Block 6: WB (writeback) - ACTIVE */
	ret = g2d_rcq_build_wb(width, height, pitch, dst_dma, dst_fmt_val,
			       &wb_regs, &wb_size);
	if (ret) {
		dev_err(g2d->dev, "Failed to build WB block: %d\n", ret);
		goto cleanup;
	}

	dev_dbg(g2d->dev, "RCQ(modular) WB setup: dst_dma=0x%pad\n", &dst_dma);

	/* ========== SETUP RCQ LAYOUT (7-block BSP structure) ========== */

	layout.block_count = 7;
	layout.header_len_bytes = 7 * sizeof(struct g2d_rcq_header);

	layout.blocks[0].size = v0_size;
	layout.blocks[0].reg_offset = V0_ATTCTL; /* 0x0800 */
	layout.blocks[0].dirty = 1; /* ACTIVE */

	layout.blocks[1].size = u0_size;
	layout.blocks[1].reg_offset = 0x1000; /* G2D_UI0 */
	layout.blocks[1].dirty = 0; /* INACTIVE */

	layout.blocks[2].size = u1_size;
	layout.blocks[2].reg_offset = 0x1800; /* G2D_UI1 */
	layout.blocks[2].dirty = 0; /* INACTIVE */

	layout.blocks[3].size = u2_size;
	layout.blocks[3].reg_offset = 0x2000; /* G2D_UI2 */
	layout.blocks[3].dirty = 0; /* INACTIVE */

	layout.blocks[4].size = scal_size;
	layout.blocks[4].reg_offset = 0x8000; /* G2D_SCALER */
	layout.blocks[4].dirty = 0; /* INACTIVE */

	layout.blocks[5].size = bld_size;
	layout.blocks[5].reg_offset = BLD_EN_CTL; /* 0x0400 */
	layout.blocks[5].dirty = 1; /* ACTIVE */

	layout.blocks[6].size = wb_size;
	layout.blocks[6].reg_offset = WB_ATT; /* 0x3000 */
	layout.blocks[6].dirty = 1; /* ACTIVE */

	/* ========== PACK INTO RCQ BUFFER ========== */

	ret = sunxi_g2d_rcq_pack_frame_7blocks(rcq, &layout, v0_regs,
					       u0_regs, u1_regs, u2_regs,
					       scal_regs, bld_regs, wb_regs);
	if (ret) {
		dev_err(g2d->dev, "Failed to pack RCQ frame: %d\n", ret);
		goto cleanup;
	}

	dev_dbg(g2d->dev,
		 "RCQ(modular) packed: headers=%u used=%u phy_addr=0x%pad\n",
		 rcq->header_count, rcq->used, &rcq->phy_addr);

	/* DEBUG: Dump RCQ buffer contents when enabled */
	if (g2d_rcq_debug) {
		dev_dbg(g2d->dev,
			 "RCQ(modular) full dump (%u headers, %u bytes):\n",
			 rcq->header_count, rcq->used);
		print_hex_dump(KERN_INFO, "RCQ: ", DUMP_PREFIX_OFFSET, 16, 4,
			       rcq->vir_addr, rcq->used, false);
	}

	ret = 0;

cleanup:
	/* Free all allocated blocks */
	kfree(v0_regs);
	kfree(u0_regs);
	kfree(u1_regs);
	kfree(u2_regs);
	kfree(bld_regs);
	kfree(scal_regs);
	kfree(wb_regs);

	return ret;
}




static int sunxi_g2d_open(struct inode *inode, struct file *file)
{
	struct sunxi_g2d_dev *g2d =
		container_of(inode->i_cdev, struct sunxi_g2d_dev, cdev);
	struct sunxi_g2d_ctx *ctx;
	int ret = 0;

	dev_dbg(g2d->dev, "=== MODULE WITH NEW G2D_IOC_CMD API LOADED ===\n");
	dev_dbg(g2d->dev,
		"sizeof(g2d_buf)=%zu sizeof(g2d_cmd)=%zu G2D_IOC_CMD=0x%08lx\n",
		sizeof(struct g2d_buf), sizeof(struct g2d_cmd),
		(unsigned long)G2D_IOC_CMD);
	dev_dbg(g2d->dev, "Device opened\n");

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->g2d = g2d;
	INIT_LIST_HEAD(&ctx->tasks);
	/* CSC state will be initialized from global state inside mutex */
	ctx->csc_changed = false;

	file->private_data = ctx;

	dev_dbg(g2d->dev, "About to lock mutex and enable hardware\n");

	/* Increment user count and enable hardware if first user */
	mutex_lock(&g2d->dev_mutex);

	/* Initialize context CSC state from global state (inheritance) */
	memcpy(&ctx->csc_state, &g2d->csc_state, sizeof(ctx->csc_state));

	dev_dbg(g2d->dev, "Mutex locked, users=%d\n", atomic_read(&g2d->users));

	if (atomic_inc_return(&g2d->users) == 1) {
		dev_dbg(g2d->dev, "First user, enabling hardware\n");
		ret = sunxi_g2d_hw_enable(g2d);
		if (ret) {
			dev_err(g2d->dev, "Hardware enable failed: %d\n", ret);
			atomic_dec(&g2d->users);
			mutex_unlock(&g2d->dev_mutex);
			kfree(ctx);
			return ret;
		}
		dev_dbg(g2d->dev, "Hardware enabled successfully\n");
	}

	mutex_unlock(&g2d->dev_mutex);

	dev_dbg(g2d->dev, "Open completed successfully\n");

	return 0;
}

static int sunxi_g2d_release(struct inode *inode, struct file *file)
{
	struct sunxi_g2d_ctx *ctx = file->private_data;
	struct sunxi_g2d_dev *g2d = ctx->g2d;
	struct g2d_task *task, *tmp;

	dev_dbg(g2d->dev, "Device released\n");

	/* Destroy all tasks owned by this context */
	mutex_lock(&g2d->task_lock);
	list_for_each_entry_safe(task, tmp, &g2d->task_list, node) {
		if (task->owner == ctx) {
			list_del(&task->node);
			g2d_task_destroy(g2d, task);
		}
	}
	mutex_unlock(&g2d->task_lock);

	/* Decrement user count and schedule hardware disable if last user.
	 * We schedule a delayed work to ensure the HW is not disabled while
	 * there are still jobs pending or running. The work will re-check
	 * the queue and current_job before disabling.
	 */
	mutex_lock(&g2d->dev_mutex);

	if (atomic_dec_return(&g2d->users) == 0) {
		/* Schedule delayed disable: gives time for in-flight jobs to finish.
		 * disable_work will re-check and reschedule until fully idle.
		 */
		queue_delayed_work(g2d->job_wq, &g2d->disable_work,
				   msecs_to_jiffies(100));
	}

	mutex_unlock(&g2d->dev_mutex);

	kfree(ctx);

	return 0;
}

/* ========== DMA-BUF export operations ========== */

struct g2d_dmabuf_attachment {
	struct sg_table sgt;
	bool mapped;
};

static int g2d_dup_sg_table(struct sg_table *dst, const struct sg_table *src)
{
	struct scatterlist *s, *d;
	int ret, i;

	ret = sg_alloc_table(dst, src->orig_nents, GFP_KERNEL);
	if (ret)
		return ret;

	s = src->sgl;
	d = dst->sgl;
	for (i = 0; i < src->orig_nents; i++) {
		sg_set_page(d, sg_page(s), s->length, s->offset);
		s = sg_next(s);
		d = sg_next(d);
	}

	return 0;
}

static int g2d_dmabuf_attach(struct dma_buf *dmabuf,
			     struct dma_buf_attachment *attach)
{
	struct g2d_dma_buffer *buf = dmabuf->priv;
	struct g2d_dmabuf_attachment *a;
	int ret;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	ret = g2d_dup_sg_table(&a->sgt, buf->mem.sgt);
	if (ret) {
		kfree(a);
		return ret;
	}

	a->mapped = false;
	attach->priv = a;

	dev_dbg(buf->dev,
		"g2d_dmabuf_attach: dmabuf=%p attach->dev=%p orig_nents=%u\n",
		dmabuf, attach->dev,
		buf->mem.sgt ? buf->mem.sgt->orig_nents : 0);

	return 0;
}

static void g2d_dmabuf_detach(struct dma_buf *dmabuf,
			      struct dma_buf_attachment *attach)
{
	struct g2d_dmabuf_attachment *a = attach->priv;

	if (!a)
		return;

	if (a->mapped)
		dma_unmap_sgtable(attach->dev, &a->sgt, DMA_BIDIRECTIONAL, 0);

	sg_free_table(&a->sgt);
	kfree(a);
}

static struct sg_table *g2d_dmabuf_map(struct dma_buf_attachment *attach,
				       enum dma_data_direction dir)
{
	struct g2d_dmabuf_attachment *a = attach->priv;
	int ret;

	dev_dbg(attach->dev,
		"g2d_dmabuf_map: attach->dev=%p dir=%d orig_nents=%u\n",
		attach->dev, dir, a->sgt.orig_nents);

	ret = dma_map_sgtable(attach->dev, &a->sgt, dir, 0);
	if (ret) {
		dev_err(attach->dev,
			"g2d_dmabuf_map: dma_map_sgtable failed ret=%d\n", ret);
		return ERR_PTR(ret);
	}

	a->mapped = true;
	dev_dbg(attach->dev, "g2d_dmabuf_map: mapped, first DMA addr=0x%pad\n",
		&sg_dma_address(a->sgt.sgl));
	return &a->sgt;
}

static void g2d_dmabuf_unmap(struct dma_buf_attachment *attach,
			     struct sg_table *sgt, enum dma_data_direction dir)
{
	struct g2d_dmabuf_attachment *a = attach->priv;

	if (!a || !a->mapped)
		return;

	dma_unmap_sgtable(attach->dev, &a->sgt, dir, 0);
	a->mapped = false;
}

static void g2d_dmabuf_release(struct dma_buf *dmabuf)
{
	struct g2d_dma_buffer *buf = dmabuf->priv;

	g2d_dma_mem_free(buf->dev, &buf->mem);
	kfree(buf);
}

static int g2d_dmabuf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct g2d_dma_buffer *buf = dmabuf->priv;
	int ret;

	dev_dbg(buf->dev, "g2d_dmabuf_mmap: size=%zu vma_size=%lu\n",
		buf->mem.size, vma->vm_end - vma->vm_start);

	if (buf->mem.is_coherent) {
		/* Map coherent memory directly */
		ret = dma_mmap_attrs(buf->dev, vma, buf->mem.vaddr,
				     buf->mem.dma_addr, buf->mem.size, 0);
	} else {
		if (!buf->mem.sgt) {
			dev_err(buf->dev,
				"g2d_dmabuf_mmap: no sgt available\n");
			return -EINVAL;
		}
		ret = dma_mmap_noncontiguous(buf->dev, vma, buf->mem.size,
					     buf->mem.sgt);
	}
	dev_dbg(buf->dev, "g2d_dmabuf_mmap: ret=%d\n", ret);
	return ret;
}

static int g2d_dmabuf_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct g2d_dma_buffer *buf = dmabuf->priv;

	if (!buf->mem.vaddr)
		return -ENOMEM;

	iosys_map_set_vaddr(map, buf->mem.vaddr);
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

/*
 * G2D_CMD_SCALE handler: Scaling operation
 * Scales src buffer to dst_w x dst_h size using VSU
 */
static long sunxi_g2d_cmd_scale(struct sunxi_g2d_ctx *ctx, unsigned long arg)
{
	struct sunxi_g2d_dev *g2d = ctx->g2d;
	struct g2d_cmd cmd;
	struct dma_buf *src_dmabuf = NULL, *dst_dmabuf = NULL;
	struct dma_buf_attachment *src_attach = NULL, *dst_attach = NULL;
	struct sg_table *src_sgt = NULL, *dst_sgt = NULL;
	dma_addr_t src_dma_addr, dst_dma_addr;
	u32 src_bpp, dst_bpp;
	int ret = 0;
	struct dma_fence *fence = NULL;
	struct sync_file *sync_file = NULL;
	int fence_fd = -1;
	bool fd_installed = false;
	struct sunxi_g2d_job *job = NULL;
	u64 dst_offset_bytes;

	/* Copy command from userspace */
	if (copy_from_user(&cmd, (void __user *)arg, sizeof(cmd))) {
		dev_err(g2d->dev, "SCALE: copy_from_user failed\n");
		return -EFAULT;
	}

	dev_dbg(
		g2d->dev,
		"G2D_CMD_SCALE: src=%ux%u fmt=0x%02x stride=%u crop=%ux%u@%u,%u dma_fd=%d\n",
		cmd.src.width, cmd.src.height, cmd.src.format,
		cmd.src.stride[0], cmd.src.crop_w, cmd.src.crop_h,
		cmd.src.crop_x, cmd.src.crop_y, cmd.src.dma_fd);
	dev_dbg(
		g2d->dev,
		"G2D_CMD_SCALE: dst=%ux%u fmt=0x%02x stride=%u → %ux%u@%u,%u dma_fd=%d\n",
		cmd.dst.width, cmd.dst.height, cmd.dst.format,
		cmd.dst.stride[0], cmd.dst_w, cmd.dst_h, cmd.dst_x, cmd.dst_y,
		cmd.dst.dma_fd);

	/* Validate buffer formats */
	if (sunxi_g2d_format_to_hw(cmd.src.format, &src_bpp) < 0) {
		dev_err(g2d->dev, "Unsupported source format: %u\n",
			cmd.src.format);
		return -EOPNOTSUPP;
	}
	if (sunxi_g2d_format_to_hw(cmd.dst.format, &dst_bpp) < 0) {
		dev_err(g2d->dev, "Unsupported destination format: %u\n",
			cmd.dst.format);
		return -EOPNOTSUPP;
	}

	/* Enforce 32-byte aligned destination offset/width to avoid HW lockups */
	{
		u32 align_px = max(1U, 32 / dst_bpp);
		u64 dst_offset_bytes = (u64)cmd.dst_y * cmd.dst.stride[0] +
				       (u64)cmd.dst_x * dst_bpp;
		if (dst_offset_bytes & 0x1F) {
			dev_err(g2d->dev,
				"SCALE: dst offset not 32-byte aligned (0x%llx)\n",
				dst_offset_bytes);
			return -EINVAL;
		}
		if (cmd.dst_w % align_px) {
			dev_err(g2d->dev,
				"SCALE: dst width %u not aligned to %u pixels\n",
				cmd.dst_w, align_px);
			return -EINVAL;
		}
	}

	/* Validate rectangles and sizes */
	if (!cmd.src.width || !cmd.src.height || !cmd.dst.width ||
	    !cmd.dst.height || !cmd.dst_w || !cmd.dst_h || !cmd.src.crop_w ||
	    !cmd.src.crop_h) {
		dev_err(g2d->dev,
			"SCALE: zero dimension (src=%ux%u crop=%ux%u dst=%ux%u rect=%ux%u)\n",
			cmd.src.width, cmd.src.height, cmd.src.crop_w,
			cmd.src.crop_h, cmd.dst.width, cmd.dst.height,
			cmd.dst_w, cmd.dst_h);
		return -EINVAL;
	}
	if (cmd.src.crop_x + cmd.src.crop_w > cmd.src.width ||
	    cmd.src.crop_y + cmd.src.crop_h > cmd.src.height) {
		dev_err(g2d->dev,
			"SCALE: src crop out of bounds (%u,%u %ux%u vs %ux%u)\n",
			cmd.src.crop_x, cmd.src.crop_y, cmd.src.crop_w,
			cmd.src.crop_h, cmd.src.width, cmd.src.height);
		return -EINVAL;
	}
	if (cmd.dst_x + cmd.dst_w > cmd.dst.width ||
	    cmd.dst_y + cmd.dst_h > cmd.dst.height) {
		dev_err(g2d->dev,
			"SCALE: dst rect out of bounds (%u,%u %ux%u vs %ux%u)\n",
			cmd.dst_x, cmd.dst_y, cmd.dst_w, cmd.dst_h,
			cmd.dst.width, cmd.dst.height);
		return -EINVAL;
	}

	/* Hardware stability quirk: WB base must be 32-byte aligned.
	 * If the destination rectangle offset is not 32-byte aligned,
	 * reject to avoid RCQ hangs.
	 */
	dst_offset_bytes =
		((u64)cmd.dst_y * cmd.dst.stride[0]) +
		((u64)cmd.dst_x * dst_bpp);
	if (dst_offset_bytes & 0x1F) {
		dev_err(g2d->dev,
			"SCALE: dst offset not 32-byte aligned (offset=0x%llx)\n",
			dst_offset_bytes);
		return -EINVAL;
	}

	/* Import source buffer */
	src_dmabuf = dma_buf_get(cmd.src.dma_fd);
	if (IS_ERR(src_dmabuf)) {
		dev_err(g2d->dev, "SCALE: dma_buf_get(src) failed: %ld\n",
			PTR_ERR(src_dmabuf));
		return PTR_ERR(src_dmabuf);
	}

	src_attach = dma_buf_attach(src_dmabuf, g2d->dev);
	if (IS_ERR(src_attach)) {
		dev_err(g2d->dev, "SCALE: dma_buf_attach(src) failed: %ld\n",
			PTR_ERR(src_attach));
		ret = PTR_ERR(src_attach);
		goto cleanup;
	}

	src_sgt = dma_buf_map_attachment(src_attach, DMA_TO_DEVICE);
	if (IS_ERR(src_sgt)) {
		dev_err(g2d->dev,
			"SCALE: dma_buf_map_attachment(src) failed: %ld\n",
			PTR_ERR(src_sgt));
		ret = PTR_ERR(src_sgt);
		goto cleanup;
	}

	/* Pre-compute strides/offsets for possible staging */
	u32 src_stride[3], src_plane_offset[3];
	sunxi_g2d_get_yuv_plane_info(cmd.src.format, cmd.src.width,
				     cmd.src.height, cmd.src.stride, src_stride,
				     src_plane_offset);

	/* Basic stride sanity (packed formats) */
	if (!sunxi_g2d_is_yuv_planar(cmd.src.format) &&
	    !sunxi_g2d_is_yuv_semiplanar(cmd.src.format)) {
		if (src_stride[0] < cmd.src.crop_w * src_bpp) {
			dev_err(g2d->dev,
				"SCALE: src stride too small (%u < %u)\n",
				src_stride[0], cmd.src.crop_w * src_bpp);
			ret = -EINVAL;
			goto cleanup;
		}
	}

	/* If force_staging is enabled, stage source only if buffer exceeds threshold.
	 * Small intermediate buffers should use direct DMA path.
	 */
	/* Determine source DMA address */
	ret = g2d_sg_dma_address(g2d->dev, src_sgt, &src_dma_addr,
				 "SCALE src");
	if (ret) {
		goto cleanup;
	}

	/* Import destination buffer */
	dst_dmabuf = dma_buf_get(cmd.dst.dma_fd);
	if (IS_ERR(dst_dmabuf)) {
		dev_err(g2d->dev, "SCALE: dma_buf_get(dst) failed: %ld\n",
			PTR_ERR(dst_dmabuf));
		ret = PTR_ERR(dst_dmabuf);
		goto cleanup;
	}

	dst_attach = dma_buf_attach(dst_dmabuf, g2d->dev);
	if (IS_ERR(dst_attach)) {
		dev_err(g2d->dev, "SCALE: dma_buf_attach(dst) failed: %ld\n",
			PTR_ERR(dst_attach));
		ret = PTR_ERR(dst_attach);
		goto cleanup;
	}

	dst_sgt = dma_buf_map_attachment(dst_attach, DMA_FROM_DEVICE);
	if (IS_ERR(dst_sgt)) {
		dev_err(g2d->dev,
			"SCALE: dma_buf_map_attachment(dst) failed: %ld\n",
			PTR_ERR(dst_sgt));
		ret = PTR_ERR(dst_sgt);
		goto cleanup;
	}

	/* Determine destination DMA */
	ret = g2d_sg_dma_address(g2d->dev, dst_sgt, &dst_dma_addr, "SCALE dst");
	if (ret) {
		goto cleanup;
	}

	/* Create fence for async operation */
	fence = sunxi_g2d_fence_create(g2d);
	if (!fence) {
		dev_err(g2d->dev, "SCALE: fence_create failed\n");
		ret = -ENOMEM;
		goto cleanup;
	}

	/* Create sync_file for userspace */
	sync_file = sync_file_create(fence);
	if (!sync_file) {
		dev_err(g2d->dev, "SCALE: sync_file_create failed\n");
		dma_fence_put(fence);
		ret = -ENOMEM;
		goto cleanup;
	}

	fence_fd = get_unused_fd_flags(O_CLOEXEC);
	if (fence_fd < 0) {
		dev_err(g2d->dev, "SCALE: get_unused_fd failed\n");
		fput(sync_file->file);
		ret = fence_fd;
		goto cleanup;
	}

	/* Create/obtain async job (from pool if available) */
	job = g2d_job_alloc(g2d);
	if (IS_ERR(job)) {
		put_unused_fd(fence_fd);
		fput(sync_file->file);
		ret = PTR_ERR(job);
		goto cleanup;
	}

	INIT_WORK(&job->cleanup_work, sunxi_g2d_job_cleanup_workfn);
	job->g2d = g2d;
	job->csc_state = ctx->csc_changed ? ctx->csc_state : g2d->csc_state;
	job->fence = fence;
	job->src_dmabuf = src_dmabuf;
	job->dst_dmabuf = dst_dmabuf;
	job->src_attach = src_attach;
	job->dst_attach = dst_attach;
	job->src_sgt = src_sgt;
	job->dst_sgt = dst_sgt;

	/* Setup job parameters */
	job->type = G2D_JOB_CMD_SCALE;
	job->src_dma = src_dma_addr;
	job->dst_dma = dst_dma_addr;

	/* ALIGNMENT CHECK: G2D requires 32KB alignment for buffer addresses */
	if (src_dma_addr & 0x7FFF) {
		dev_dbg(
			g2d->dev,
			"SCALE: src_dma=0x%llx NOT 32KB aligned (misalign=0x%llx)\n",
			(unsigned long long)src_dma_addr,
			(unsigned long long)(src_dma_addr & 0x7FFF));
	}
	if (dst_dma_addr & 0x7FFF) {
		dev_dbg(
			g2d->dev,
			"SCALE: dst_dma=0x%llx NOT 32KB aligned (misalign=0x%llx)\n",
			(unsigned long long)dst_dma_addr,
			(unsigned long long)(dst_dma_addr & 0x7FFF));
	}

	/* Calculate proper strides for YUV formats */
	u32 dst_stride[3], dst_plane_offset[3];

	sunxi_g2d_get_yuv_plane_info(cmd.dst.format, cmd.dst.width,
				     cmd.dst.height, cmd.dst.stride, dst_stride,
				     dst_plane_offset);

	if (!sunxi_g2d_is_yuv_planar(cmd.dst.format) &&
	    !sunxi_g2d_is_yuv_semiplanar(cmd.dst.format)) {
		if (dst_stride[0] < cmd.dst_w * dst_bpp) {
			dev_err(g2d->dev,
				"SCALE: dst stride too small (%u < %u)\n",
				dst_stride[0], cmd.dst_w * dst_bpp);
			ret = -EINVAL;
			goto cleanup;
		}
	}

	/* Fill blit data structure */
	job->data.blit.src_width = cmd.src.width;
	job->data.blit.src_height = cmd.src.height;
	job->data.blit.src_pitch = src_stride[0];
	job->data.blit.src_format = cmd.src.format;
	job->data.blit.src_crop_x = cmd.src.crop_x;
	job->data.blit.src_crop_y = cmd.src.crop_y;
	job->data.blit.src_crop_w = cmd.src.crop_w;
	job->data.blit.src_crop_h = cmd.src.crop_h;
	job->data.blit.src_color_space = cmd.src.color_space;

	job->data.blit.dst_width = cmd.dst.width;
	job->data.blit.dst_height = cmd.dst.height;
	/* CRITICAL FIX: When staging is active, use out_pitch (based on actual output size)
	 * instead of dst_stride (based on user buffer size). Otherwise hardware writes
	 * with wrong stride causing corruption. */
	job->data.blit.dst_pitch =
		job->wb_out_active ? job->data.blit.out_pitch : dst_stride[0];
	job->data.blit.dst_format = cmd.dst.format;
	job->data.blit.dst_x = cmd.dst_x;
	job->data.blit.dst_y = cmd.dst_y;
	job->data.blit.dst_w = cmd.dst_w;
	job->data.blit.dst_h = cmd.dst_h;
	job->data.blit.dst_color_space = cmd.dst.color_space;
	job->data.blit.out_color_space = job->data.blit.dst_color_space;
	job->data.blit.out_color_space = job->data.blit.dst_color_space;

	ret = g2d_job_prepare_rcq(g2d, job);
	if (ret)
		goto cleanup;

	ret = sunxi_g2d_do_scale_rcq(
		g2d, &job->rcq, job->src_dma, job->data.blit.src_width,
		job->data.blit.src_height, job->data.blit.src_pitch,
		job->data.blit.src_format, job->data.blit.src_crop_x,
		job->data.blit.src_crop_y, job->data.blit.src_crop_w,
		job->data.blit.src_crop_h, job->dst_dma, job->data.blit.dst_width,
		job->data.blit.dst_height, job->data.blit.dst_pitch,
		job->data.blit.dst_format, job->data.blit.dst_x,
		job->data.blit.dst_y, job->data.blit.dst_w, job->data.blit.dst_h,
		job->data.blit.src_color_space, job->data.blit.dst_color_space,
		&job->csc_state);
	if (ret) {
		sunxi_g2d_rcq_free(g2d->dev, &job->rcq);
		goto cleanup;
	}
	job->rcq_ready = true;

	/* Enqueue job to worker */
	{
		unsigned long flags;
		spin_lock_irqsave(&g2d->job_lock, flags);
		list_add_tail(&job->node, &g2d->job_queue);
		spin_unlock_irqrestore(&g2d->job_lock, flags);
	}

	queue_work(g2d->job_wq, &g2d->job_work);

	/* Install fence fd */
	fd_install(fence_fd, sync_file->file);
	fd_installed = true;

	dev_dbg(g2d->dev, "G2D_CMD_SCALE: enqueued job, fence_fd=%d\n",
		 fence_fd);

	/* Return fence fd to userspace */
	if (put_user(fence_fd, &((struct g2d_cmd __user *)arg)->fence_fd_out)) {
		dev_err(g2d->dev, "Failed to copy fence_fd_out to userspace\n");
		/* Job already enqueued, can't cleanly abort */
		return -EFAULT;
	}

	return 0;

cleanup:
	if (!IS_ERR_OR_NULL(job)) {
		if (job->rcq.vir_addr)
			sunxi_g2d_rcq_free(g2d->dev, &job->rcq);
		g2d_job_free(g2d, job);
	}

	if (!fd_installed && fence_fd >= 0)
		put_unused_fd(fence_fd);
	if (!fd_installed && sync_file && sync_file->file)
		fput(sync_file->file);
	if (fence)
		dma_fence_put(fence);

	if (dst_sgt && dst_attach)
		dma_buf_unmap_attachment(dst_attach, dst_sgt, DMA_FROM_DEVICE);
	if (dst_attach && dst_dmabuf)
		dma_buf_detach(dst_dmabuf, dst_attach);
	if (dst_dmabuf && !IS_ERR(dst_dmabuf))
		dma_buf_put(dst_dmabuf);
	if (src_sgt && src_attach)
		dma_buf_unmap_attachment(src_attach, src_sgt, DMA_TO_DEVICE);
	if (src_attach && src_dmabuf)
		dma_buf_detach(src_dmabuf, src_attach);
	if (src_dmabuf && !IS_ERR(src_dmabuf))
		dma_buf_put(src_dmabuf);

	return ret;
}

/*
 * G2D_CMD_BLEND handler: Alpha blending operation
 * Blends src + dst → out using specified Porter-Duff mode
 */
static long sunxi_g2d_cmd_blend(struct sunxi_g2d_ctx *ctx, unsigned long arg)
{
	struct sunxi_g2d_dev *g2d = ctx->g2d;
	struct g2d_cmd cmd;
	struct dma_buf *src_dmabuf = NULL, *dst_dmabuf = NULL,
		       *out_dmabuf = NULL;
	struct dma_buf_attachment *src_attach = NULL, *dst_attach = NULL,
				  *out_attach = NULL;
	struct sg_table *src_sgt = NULL, *dst_sgt = NULL, *out_sgt = NULL;
	dma_addr_t src_dma_addr, dst_dma_addr, out_dma_addr;
	u32 src_bpp, dst_bpp, out_bpp;
	int ret = 0;
	struct dma_fence *fence = NULL;
	struct sync_file *sync_file = NULL;
	int fence_fd = -1;
	bool fd_installed = false;
	struct sunxi_g2d_job *job = NULL;

	/* Copy command from userspace */
	if (copy_from_user(&cmd, (void __user *)arg, sizeof(cmd)))
		return -EFAULT;

	dev_dbg(
		g2d->dev,
		"CMD_BLEND from userspace: src=%ux%u fmt=0x%02x stride=%u crop=%ux%u+%u+%u\n",
		cmd.src.width, cmd.src.height, cmd.src.format,
		cmd.src.stride[0], cmd.src.crop_w, cmd.src.crop_h,
		cmd.src.crop_x, cmd.src.crop_y);
	dev_dbg(
		g2d->dev,
		"CMD_BLEND from userspace: dst=%ux%u fmt=0x%02x dst_pos=(%u,%u) dst_size=%ux%u\n",
		cmd.dst.width, cmd.dst.height, cmd.dst.format, cmd.dst_x,
		cmd.dst_y, cmd.dst_w, cmd.dst_h);

	/* Validate buffer formats */
	if (sunxi_g2d_format_to_hw(cmd.src.format, &src_bpp) < 0) {
		dev_err(g2d->dev, "Unsupported source format: %u\n",
			cmd.src.format);
		return -EOPNOTSUPP;
	}
	if (sunxi_g2d_format_to_hw(cmd.dst.format, &dst_bpp) < 0) {
		dev_err(g2d->dev, "Unsupported destination format: %u\n",
			cmd.dst.format);
		return -EOPNOTSUPP;
	}
	if (sunxi_g2d_format_to_hw(cmd.out.format, &out_bpp) < 0) {
		dev_err(g2d->dev, "Unsupported output format: %u\n",
			cmd.out.format);
		return -EOPNOTSUPP;
	}

	/* BLEND requires output buffer */
	if (cmd.out.dma_fd < 0) {
		dev_err(g2d->dev, "G2D_CMD_BLEND requires valid out.dma_fd\n");
		return -EINVAL;
	}

	/* Validate blend mode */
	if (cmd.params.blend.bld_mode > G2D_BLD_XOR) {
		dev_err(g2d->dev, "Invalid blend mode: %u\n",
			cmd.params.blend.bld_mode);
		return -EINVAL;
	}

	/* Historically BLEND rejected scaling requests. The driver now
	 * supports scaling inside BLEND (VSU-based) so do not reject here.
	 * Keep a debug trace if a scale is requested so developers can
	 * verify behavior at runtime when debugging.
	 */
	if (cmd.src.crop_w != cmd.dst_w || cmd.src.crop_h != cmd.dst_h) {
		dev_dbg(g2d->dev,
			"BLEND requested with scaling: src_crop=%ux%u dst=%ux%u\n",
			cmd.src.crop_w, cmd.src.crop_h, cmd.dst_w, cmd.dst_h);
	}

	/* Import source buffer */
	src_dmabuf = dma_buf_get(cmd.src.dma_fd);
	if (IS_ERR(src_dmabuf))
		return PTR_ERR(src_dmabuf);

	src_attach = dma_buf_attach(src_dmabuf, g2d->dev);
	if (IS_ERR(src_attach)) {
		ret = PTR_ERR(src_attach);
		goto cleanup;
	}

	src_sgt = dma_buf_map_attachment(src_attach, DMA_TO_DEVICE);
	if (IS_ERR(src_sgt)) {
		ret = PTR_ERR(src_sgt);
		goto cleanup;
	}

	/* Pre-compute strides/offsets for possible staging */
	u32 src_stride[3], src_plane_offset[3];
	sunxi_g2d_get_yuv_plane_info(cmd.src.format, cmd.src.width,
				     cmd.src.height, cmd.src.stride, src_stride,
				     src_plane_offset);

	/* If force staging is enabled, stage source only if buffer exceeds threshold.
	 * Small intermediate buffers should use direct DMA path.
	 */
	ret = g2d_sg_dma_address(g2d->dev, src_sgt, &src_dma_addr,
				 "CMD_BLEND src");
	if (ret) {
		goto cleanup;
	}

	/* Import destination buffer */
	dst_dmabuf = dma_buf_get(cmd.dst.dma_fd);
	if (IS_ERR(dst_dmabuf)) {
		ret = PTR_ERR(dst_dmabuf);
		goto cleanup;
	}

	dst_attach = dma_buf_attach(dst_dmabuf, g2d->dev);
	if (IS_ERR(dst_attach)) {
		ret = PTR_ERR(dst_attach);
		goto cleanup;
	}

	dst_sgt = dma_buf_map_attachment(dst_attach, DMA_TO_DEVICE);
	if (IS_ERR(dst_sgt)) {
		ret = PTR_ERR(dst_sgt);
		goto cleanup;
	}

	/* Allow staging for DST (background) if non-linear or when forced */
	{
		/* Pre-compute dst strides for potential staging */
		u32 dst_stride_calc[3], dst_plane_offset[3];
		sunxi_g2d_get_yuv_plane_info(cmd.dst.format, cmd.dst.width,
					     cmd.dst.height, cmd.dst.stride,
					     dst_stride_calc, dst_plane_offset);
		ret = g2d_sg_dma_address(g2d->dev, dst_sgt,
					 &dst_dma_addr,
					 "CMD_BLEND dst");
		if (ret) {
			goto cleanup;
		}
	}

	/* Import output buffer */
	out_dmabuf = dma_buf_get(cmd.out.dma_fd);
	if (IS_ERR(out_dmabuf)) {
		ret = PTR_ERR(out_dmabuf);
		goto cleanup;
	}

	out_attach = dma_buf_attach(out_dmabuf, g2d->dev);
	if (IS_ERR(out_attach)) {
		ret = PTR_ERR(out_attach);
		goto cleanup;
	}

	out_sgt = dma_buf_map_attachment(out_attach, DMA_FROM_DEVICE);
	if (IS_ERR(out_sgt)) {
		ret = PTR_ERR(out_sgt);
		goto cleanup;
	}

	ret = g2d_sg_dma_address(g2d->dev, out_sgt, &out_dma_addr,
				 "CMD_BLEND out");
	if (ret)
		goto cleanup;

	/* Create fence for async operation */
	fence = sunxi_g2d_fence_create(g2d);
	if (!fence) {
		ret = -ENOMEM;
		goto cleanup;
	}

	/* Create sync_file for userspace */
	sync_file = sync_file_create(fence);
	if (!sync_file) {
		dma_fence_put(fence);
		ret = -ENOMEM;
		goto cleanup;
	}

	fence_fd = get_unused_fd_flags(O_CLOEXEC);
	if (fence_fd < 0) {
		fput(sync_file->file);
		ret = fence_fd;
		goto cleanup;
	}

	/* Create/obtain async job (from pool if available) */
	job = g2d_job_alloc(g2d);
	if (IS_ERR(job)) {
		put_unused_fd(fence_fd);
		fput(sync_file->file);
		ret = PTR_ERR(job);
		goto cleanup;
	}

	INIT_WORK(&job->cleanup_work, sunxi_g2d_job_cleanup_workfn);
	job->g2d = g2d;
	job->csc_state = ctx->csc_changed ? ctx->csc_state : g2d->csc_state;
	job->fence = fence;
	job->src_dmabuf = src_dmabuf;
	job->dst_dmabuf = dst_dmabuf;
	job->out_dmabuf = out_dmabuf;
	job->src_attach = src_attach;
	job->dst_attach = dst_attach;
	job->out_attach = out_attach;
	job->src_sgt = src_sgt;
	job->dst_sgt = dst_sgt;
	job->out_sgt = out_sgt;

	/* Setup job parameters */
	job->type = G2D_JOB_CMD_BLEND;
	job->src_dma = src_dma_addr;
	job->dst_dma = dst_dma_addr;
	job->out_dma = out_dma_addr;

	/* Calculate proper strides for YUV formats */
	u32 dst_stride[3], dst_plane_offset[3];
	u32 out_stride[3], out_plane_offset[3];

	sunxi_g2d_get_yuv_plane_info(cmd.src.format, cmd.src.width,
				     cmd.src.height, cmd.src.stride, src_stride,
				     src_plane_offset);
	sunxi_g2d_get_yuv_plane_info(cmd.dst.format, cmd.dst.width,
				     cmd.dst.height, cmd.dst.stride, dst_stride,
				     dst_plane_offset);
	sunxi_g2d_get_yuv_plane_info(cmd.out.format, cmd.out.width,
				     cmd.out.height, cmd.out.stride, out_stride,
				     out_plane_offset);

	/* Fill blit data structure */
	job->data.blit.src_width = cmd.src.width;
	job->data.blit.src_height = cmd.src.height;
	job->data.blit.src_pitch = src_stride[0];
	job->data.blit.src_format = cmd.src.format;
	job->data.blit.src_crop_x = cmd.src.crop_x;
	job->data.blit.src_crop_y = cmd.src.crop_y;
	job->data.blit.src_crop_w = cmd.src.crop_w;
	job->data.blit.src_crop_h = cmd.src.crop_h;
	job->data.blit.src_alpha = cmd.src.alpha;
	job->data.blit.src_alpha_mode = cmd.src.alpha_mode;
	job->data.blit.src_premul = cmd.src.premul_mode;
	job->data.blit.src_color_space = cmd.src.color_space;

	job->data.blit.dst_width = cmd.dst.width;
	job->data.blit.dst_height = cmd.dst.height;
	job->data.blit.dst_pitch = dst_stride[0];
	job->data.blit.dst_format = cmd.dst.format;
	job->data.blit.dst_x = cmd.dst_x;
	job->data.blit.dst_y = cmd.dst_y;
	job->data.blit.dst_w = cmd.dst_w;
	job->data.blit.dst_h = cmd.dst_h;
	job->data.blit.dst_alpha = cmd.dst.alpha;
	job->data.blit.dst_alpha_mode = cmd.dst.alpha_mode;
	job->data.blit.dst_premul = cmd.dst.premul_mode;
	job->data.blit.dst_color_space = cmd.dst.color_space;

	job->data.blit.out_width = cmd.out.width;
	job->data.blit.out_height = cmd.out.height;
	job->data.blit.out_pitch = out_stride[0];
	job->data.blit.out_format = cmd.out.format;
	job->data.blit.out_color_space = cmd.out.color_space;

	job->data.blit.bld_mode = cmd.params.blend.bld_mode;

	ret = g2d_job_prepare_rcq(g2d, job);
	if (ret) {
		goto cleanup;
	}

	ret = sunxi_g2d_do_blend_rcq(
		g2d, &job->rcq, job->src_dma, job->data.blit.src_width,
		job->data.blit.src_height, job->data.blit.src_pitch,
		job->data.blit.src_format, job->data.blit.src_crop_x,
		job->data.blit.src_crop_y, job->data.blit.src_crop_w,
		job->data.blit.src_crop_h, job->dst_dma, job->data.blit.dst_width,
		job->data.blit.dst_height, job->data.blit.dst_pitch,
		job->data.blit.dst_format, job->data.blit.dst_x,
		job->data.blit.dst_y, job->out_dma, job->data.blit.out_width,
		job->data.blit.out_height, job->data.blit.out_pitch,
		job->data.blit.out_format, job->data.blit.dst_x,
		job->data.blit.dst_y, job->data.blit.dst_w, job->data.blit.dst_h,
		job->data.blit.bld_mode, job->data.blit.src_alpha_mode,
		job->data.blit.src_alpha, job->data.blit.src_premul,
		job->data.blit.src_color_space, job->data.blit.dst_color_space,
		job->data.blit.out_color_space, &job->csc_state);
	if (ret)
		goto cleanup;
	job->rcq_ready = true;

	/* Enqueue job to worker */
	{
		unsigned long flags;
		spin_lock_irqsave(&g2d->job_lock, flags);
		list_add_tail(&job->node, &g2d->job_queue);
		spin_unlock_irqrestore(&g2d->job_lock, flags);
	}

	queue_work(g2d->job_wq, &g2d->job_work);

	/* Install fence fd */
	fd_install(fence_fd, sync_file->file);
	fd_installed = true;

	/* Return fence fd to userspace */
	if (put_user(fence_fd, &((struct g2d_cmd __user *)arg)->fence_fd_out)) {
		dev_err(g2d->dev, "Failed to copy fence_fd_out to userspace\n");
		/* Job already enqueued, can't cleanly abort */
		return -EFAULT;
	}

	dev_dbg(g2d->dev, "G2D_CMD_BLEND: enqueued job, fence_fd=%d\n",
		fence_fd);

	return 0;

cleanup:
	if (!IS_ERR_OR_NULL(job)) {
		if (job->rcq.vir_addr)
			sunxi_g2d_rcq_free(g2d->dev, &job->rcq);
		g2d_job_free(g2d, job);
	}

	if (!fd_installed && fence_fd >= 0)
		put_unused_fd(fence_fd);
	if (!fd_installed && sync_file && sync_file->file)
		fput(sync_file->file);
	if (fence)
		dma_fence_put(fence);

	if (out_sgt && out_attach)
		dma_buf_unmap_attachment(out_attach, out_sgt,
					 DMA_FROM_DEVICE);
	if (out_attach && out_dmabuf)
		dma_buf_detach(out_dmabuf, out_attach);
	if (out_dmabuf && !IS_ERR(out_dmabuf))
		dma_buf_put(out_dmabuf);

	if (dst_sgt && dst_attach)
		dma_buf_unmap_attachment(dst_attach, dst_sgt, DMA_TO_DEVICE);
	if (dst_attach && dst_dmabuf)
		dma_buf_detach(dst_dmabuf, dst_attach);
	if (dst_dmabuf && !IS_ERR(dst_dmabuf))
		dma_buf_put(dst_dmabuf);

	if (src_sgt && src_attach)
		dma_buf_unmap_attachment(src_attach, src_sgt, DMA_TO_DEVICE);
	if (src_attach && src_dmabuf)
		dma_buf_detach(src_dmabuf, src_attach);
	if (src_dmabuf && !IS_ERR(src_dmabuf))
		dma_buf_put(src_dmabuf);

	return ret;
}

/*
 * G2D_CMD_ROTATE handler: Rotation/flip operation
 * Rotates/flips src buffer using ROT block
 */
static long sunxi_g2d_cmd_rotate(struct sunxi_g2d_ctx *ctx, unsigned long arg)
{
	struct sunxi_g2d_dev *g2d = ctx->g2d;
	struct g2d_cmd cmd;
	struct dma_buf *src_dmabuf = NULL, *dst_dmabuf = NULL;
	struct dma_buf_attachment *src_attach = NULL, *dst_attach = NULL;
	struct sg_table *src_sgt = NULL, *dst_sgt = NULL;
	dma_addr_t src_dma_addr, dst_dma_addr;
	u32 src_bpp, dst_bpp;
	u32 flags = 0;
	int ret = 0;
	struct dma_fence *fence = NULL;
	struct sync_file *sync_file = NULL;
	int fence_fd = -1;
	struct sunxi_g2d_job *job = NULL;

	/* Copy command from userspace */
	if (copy_from_user(&cmd, (void __user *)arg, sizeof(cmd)))
		return -EFAULT;

	/* Validate buffer formats */
	if (sunxi_g2d_format_to_hw(cmd.src.format, &src_bpp) < 0) {
		dev_err(g2d->dev, "Unsupported source format: %u\n",
			cmd.src.format);
		return -EOPNOTSUPP;
	}
	if (sunxi_g2d_format_to_hw(cmd.dst.format, &dst_bpp) < 0) {
		dev_err(g2d->dev, "Unsupported destination format: %u\n",
			cmd.dst.format);
		return -EOPNOTSUPP;
	}

	/* Validate and convert rotation angle to flags */
	switch (cmd.params.rotate.angle) {
	case 0:
		/* No rotation, only flips allowed */
		break;
	case 90:
		flags |= G2D_BLIT_FLAG_ROTATE_90;
		break;
	case 180:
		flags |= G2D_BLIT_FLAG_ROTATE_180;
		break;
	case 270:
		flags |= G2D_BLIT_FLAG_ROTATE_270;
		break;
	default:
		dev_err(g2d->dev,
			"Invalid rotation angle: %u (must be 0, 90, 180, or 270)\n",
			cmd.params.rotate.angle);
		return -EINVAL;
	}

	if (cmd.params.rotate.flip_h)
		flags |= G2D_BLIT_FLAG_FLIP_H;
	if (cmd.params.rotate.flip_v)
		flags |= G2D_BLIT_FLAG_FLIP_V;

	/* Validate crop/dst rectangles are inside their buffers */
	if (cmd.src.crop_x + cmd.src.crop_w > cmd.src.width ||
	    cmd.src.crop_y + cmd.src.crop_h > cmd.src.height) {
		dev_err(g2d->dev,
			"G2D_CMD_ROTATE: src crop out of bounds (%u,%u %ux%u vs %ux%u)\n",
			cmd.src.crop_x, cmd.src.crop_y, cmd.src.crop_w,
			cmd.src.crop_h, cmd.src.width, cmd.src.height);
		return -EINVAL;
	}
	if (cmd.dst_x + cmd.dst_w > cmd.dst.width ||
	    cmd.dst_y + cmd.dst_h > cmd.dst.height) {
		dev_err(g2d->dev,
			"G2D_CMD_ROTATE: dst rect out of bounds (%u,%u %ux%u vs %ux%u)\n",
			cmd.dst_x, cmd.dst_y, cmd.dst_w, cmd.dst_h,
			cmd.dst.width, cmd.dst.height);
		return -EINVAL;
	}
	if (cmd.src.crop_w == 0 || cmd.src.crop_h == 0 || cmd.dst_w == 0 ||
	    cmd.dst_h == 0) {
		dev_err(g2d->dev,
			"G2D_CMD_ROTATE: zero-sized crop/dst (crop=%ux%u dst=%ux%u)\n",
			cmd.src.crop_w, cmd.src.crop_h, cmd.dst_w, cmd.dst_h);
		return -EINVAL;
	}

	/* Check for scaling (not supported in ROTATE command) */
	if (cmd.params.rotate.angle == 90 || cmd.params.rotate.angle == 270) {
		if (cmd.src.crop_w != cmd.dst_h || cmd.src.crop_h != cmd.dst_w) {
			dev_err(g2d->dev,
				"G2D_CMD_ROTATE: Scaling not supported with rotation (src=%ux%u dst=%ux%u)\n",
				cmd.src.crop_w, cmd.src.crop_h, cmd.dst_w,
				cmd.dst_h);
			return -EINVAL;
		}
	} else {
		if (cmd.src.crop_w != cmd.dst_w || cmd.src.crop_h != cmd.dst_h) {
			dev_err(g2d->dev,
				"G2D_CMD_ROTATE: Scaling not supported with rotation (src=%ux%u dst=%ux%u)\n",
				cmd.src.crop_w, cmd.src.crop_h, cmd.dst_w,
				cmd.dst_h);
			return -EINVAL;
		}
	}

	/* Import source buffer */
	src_dmabuf = dma_buf_get(cmd.src.dma_fd);
	if (IS_ERR(src_dmabuf))
		return PTR_ERR(src_dmabuf);

	src_attach = dma_buf_attach(src_dmabuf, g2d->dev);
	if (IS_ERR(src_attach)) {
		ret = PTR_ERR(src_attach);
		goto cleanup;
	}

	src_sgt = dma_buf_map_attachment(src_attach, DMA_TO_DEVICE);
	if (IS_ERR(src_sgt)) {
		ret = PTR_ERR(src_sgt);
		goto cleanup;
	}

	/* Pre-compute strides/offsets for possible staging */
	u32 src_stride[3], src_plane_offset[3];
	sunxi_g2d_get_yuv_plane_info(cmd.src.format, cmd.src.width,
				     cmd.src.height, cmd.src.stride, src_stride,
				     src_plane_offset);

	/* Force or fallback source staging */
	ret = g2d_sg_dma_address(g2d->dev, src_sgt, &src_dma_addr,
				 "CMD_ROTATE src");
	if (ret) {
		goto cleanup;
	}

	/* Import destination buffer */
	dst_dmabuf = dma_buf_get(cmd.dst.dma_fd);
	if (IS_ERR(dst_dmabuf)) {
		ret = PTR_ERR(dst_dmabuf);
		goto cleanup;
	}

	dst_attach = dma_buf_attach(dst_dmabuf, g2d->dev);
	if (IS_ERR(dst_attach)) {
		ret = PTR_ERR(dst_attach);
		goto cleanup;
	}

	dst_sgt = dma_buf_map_attachment(dst_attach, DMA_FROM_DEVICE);
	if (IS_ERR(dst_sgt)) {
		ret = PTR_ERR(dst_sgt);
		goto cleanup;
	}

	/* Determine if writeback staging is needed for destination (output) */
	{
		ret = g2d_sg_dma_address(g2d->dev, dst_sgt, &dst_dma_addr,
					 "CMD_ROTATE dst");
		if (ret) {
			goto cleanup;
		}
	}

	/* Create fence for async operation */
	fence = sunxi_g2d_fence_create(g2d);
	if (!fence) {
		ret = -ENOMEM;
		goto cleanup;
	}

	/* Create sync_file for userspace */
	sync_file = sync_file_create(fence);
	if (!sync_file) {
		dma_fence_put(fence);
		ret = -ENOMEM;
		goto cleanup;
	}

	fence_fd = get_unused_fd_flags(O_CLOEXEC);
	if (fence_fd < 0) {
		fput(sync_file->file);
		ret = fence_fd;
		goto cleanup;
	}

	/* Create/obtain async job (from pool if available) */
	job = g2d_job_alloc(g2d);
	if (IS_ERR(job)) {
		put_unused_fd(fence_fd);
		fput(sync_file->file);
		ret = PTR_ERR(job);
		goto cleanup;
	}

	INIT_WORK(&job->cleanup_work, sunxi_g2d_job_cleanup_workfn);
	job->g2d = g2d;
	job->csc_state = ctx->csc_changed ? ctx->csc_state : g2d->csc_state;
	job->fence = fence;
	job->src_dmabuf = src_dmabuf;
	job->dst_dmabuf = dst_dmabuf;
	job->src_attach = src_attach;
	job->dst_attach = dst_attach;
	job->src_sgt = src_sgt;
	job->dst_sgt = dst_sgt;

	/* Setup job parameters */
	job->type = G2D_JOB_CMD_ROTATE;
	job->src_dma = src_dma_addr;
	job->dst_dma = dst_dma_addr;

	/* Calculate proper strides for YUV formats (dst only here; src already computed) */
	u32 dst_stride[3], dst_plane_offset[3];
	sunxi_g2d_get_yuv_plane_info(cmd.dst.format, cmd.dst.width,
				     cmd.dst.height, cmd.dst.stride, dst_stride,
				     dst_plane_offset);

	/* Fill blit data structure */
	job->data.blit.src_width = cmd.src.width;
	job->data.blit.src_height = cmd.src.height;
	job->data.blit.src_pitch = src_stride[0];
	job->data.blit.src_format = cmd.src.format;
	job->data.blit.src_crop_x = cmd.src.crop_x;
	job->data.blit.src_crop_y = cmd.src.crop_y;
	job->data.blit.src_crop_w = cmd.src.crop_w;
	job->data.blit.src_crop_h = cmd.src.crop_h;
	job->data.blit.src_color_space = cmd.src.color_space;

	job->data.blit.dst_width = cmd.dst.width;
	job->data.blit.dst_height = cmd.dst.height;
	job->data.blit.dst_pitch = dst_stride[0];
	job->data.blit.dst_format = cmd.dst.format;
	job->data.blit.dst_x = cmd.dst_x;
	job->data.blit.dst_y = cmd.dst_y;
	job->data.blit.dst_w = cmd.dst_w;
	job->data.blit.dst_h = cmd.dst_h;
	job->data.blit.dst_color_space = cmd.dst.color_space;

	job->data.blit.flags = flags;

	ret = g2d_job_prepare_rcq(g2d, job);
	if (ret)
		goto cleanup;

	job->rcq_ready = true;

	/* MMIO execution successful - Signal fence and cleanup immediately */
	/* dma_fence_signal(job->fence); */
	/* queue_work(g2d->job_wq, &job->cleanup_work); */

	/* Skip enqueuing to worker since we already executed it via MMIO */
	
	{
		unsigned long iflags;
		spin_lock_irqsave(&g2d->job_lock, iflags);
		list_add_tail(&job->node, &g2d->job_queue);
		spin_unlock_irqrestore(&g2d->job_lock, iflags);
	}

	queue_work(g2d->job_wq, &g2d->job_work);
	

	/* Install fence fd */
	fd_install(fence_fd, sync_file->file);

	/* Return fence fd to userspace */
	if (put_user(fence_fd, &((struct g2d_cmd __user *)arg)->fence_fd_out)) {
		dev_err(g2d->dev, "Failed to copy fence_fd_out to userspace\n");
		/* Job already enqueued, can't cleanly abort */
		return -EFAULT;
	}

	dev_dbg(g2d->dev,
		"G2D_CMD_ROTATE: enqueued job, fence_fd=%d angle=%u flip_h=%u flip_v=%u\n",
		fence_fd, cmd.params.rotate.angle, cmd.params.rotate.flip_h,
		cmd.params.rotate.flip_v);

	return 0;

cleanup:
	if (!IS_ERR_OR_NULL(job)) {
		if (job->rcq.vir_addr)
			sunxi_g2d_rcq_free(g2d->dev, &job->rcq);
		g2d_job_free(g2d, job);
	}

	if (sync_file && sync_file->file)
		fput(sync_file->file);
	if (fence)
		dma_fence_put(fence);
	if (fence_fd >= 0)
		put_unused_fd(fence_fd);

	if (!IS_ERR_OR_NULL(dst_sgt) && !IS_ERR_OR_NULL(dst_attach))
		dma_buf_unmap_attachment(dst_attach, dst_sgt,
					 DMA_FROM_DEVICE);
	if (!IS_ERR_OR_NULL(dst_attach) && dst_dmabuf)
		dma_buf_detach(dst_dmabuf, dst_attach);
	if (dst_dmabuf && !IS_ERR(dst_dmabuf))
		dma_buf_put(dst_dmabuf);

	if (!IS_ERR_OR_NULL(src_sgt) && !IS_ERR_OR_NULL(src_attach))
		dma_buf_unmap_attachment(src_attach, src_sgt, DMA_TO_DEVICE);
	if (!IS_ERR_OR_NULL(src_attach) && src_dmabuf)
		dma_buf_detach(src_dmabuf, src_attach);
	if (src_dmabuf && !IS_ERR(src_dmabuf))
		dma_buf_put(src_dmabuf);

	return ret;
}

/*
 * G2D_CMD_FILLRECT handler: Solid color fill operation
 */
static long sunxi_g2d_cmd_fillrect(struct sunxi_g2d_ctx *ctx, unsigned long arg)
{
	struct sunxi_g2d_dev *g2d = ctx->g2d;
	struct g2d_cmd cmd;
	struct dma_buf *dst_dmabuf = NULL;
	struct dma_buf_attachment *dst_attach = NULL;
	struct sg_table *dst_sgt = NULL;
	dma_addr_t dst_dma_addr;
	u32 dst_bpp;
	int ret;

	if (copy_from_user(&cmd, (void __user *)arg, sizeof(cmd)))
		return -EFAULT;

	if (sunxi_g2d_format_to_hw(cmd.dst.format, &dst_bpp) < 0) {
		dev_err(g2d->dev, "Unsupported destination format: %u\n",
			cmd.dst.format);
		return -EOPNOTSUPP;
	}

	dst_dmabuf = dma_buf_get(cmd.dst.dma_fd);
	if (IS_ERR(dst_dmabuf))
		return PTR_ERR(dst_dmabuf);

	dst_attach = dma_buf_attach(dst_dmabuf, g2d->dev);
	if (IS_ERR(dst_attach)) {
		ret = PTR_ERR(dst_attach);
		goto err_put_dst;
	}

	dst_sgt = dma_buf_map_attachment(dst_attach, DMA_TO_DEVICE);
	if (IS_ERR(dst_sgt)) {
		ret = PTR_ERR(dst_sgt);
		goto err_detach_dst;
	}

	ret = g2d_sg_dma_address(g2d->dev, dst_sgt, &dst_dma_addr,
				 "CMD_FILLRECT dst");
	if (ret)
		goto err_unmap_dst;

	/* Enqueue as an async job and return a fence fd immediately. This matches
	 * the legacy behavior of G2D_IOC_FILLRECT: create a job, install a
	 * sync_file FD into the caller, enqueue the job and return the FD in the
	 * userspace structure so callers can poll/wait on it. */
	{
		struct sunxi_g2d_job *job;
		int out_fd = -1;
		unsigned long flags;

		/* If userspace provided an input fence fd, wait on it first */
		if (cmd.fence_fd_in >= 0) {
			struct dma_fence *in_fence = sync_file_get_fence(cmd.fence_fd_in);
			if (!in_fence) {
				ret = -EINVAL;
				goto err_unmap_dst;
			}
			dma_fence_wait(in_fence, false);
			dma_fence_put(in_fence);
		}

		/* Allocate job structure */
		job = kzalloc(sizeof(*job), GFP_KERNEL);
		if (!job) {
			ret = -ENOMEM;
			goto err_unmap_dst;
		}

		/* Create DMA fence for this job */
		job->fence = sunxi_g2d_fence_create(g2d);
		if (!job->fence) {
			kfree(job);
			ret = -ENOMEM;
			goto err_unmap_dst;
		}

		/* Reserve file descriptor for userspace */
		out_fd = get_unused_fd_flags(O_CLOEXEC);
		if (out_fd < 0) {
			dma_fence_put(job->fence);
			kfree(job);
			ret = out_fd;
			goto err_unmap_dst;
		}

		/* Create sync_file wrapping the fence */
		job->fence_fd = out_fd;
		job->sync_file = sync_file_create(job->fence);
		if (!job->sync_file) {
			put_unused_fd(out_fd);
			dma_fence_put(job->fence);
			kfree(job);
			ret = -ENOMEM;
			goto err_unmap_dst;
		}

		/* Install FD in process context (must do before queueing) */
		if (job->sync_file && job->sync_file->file) {
			fd_install(out_fd, job->sync_file->file);
			job->sync_file = NULL; /* fd table owns the ref now */
		} else {
			put_unused_fd(out_fd);
			dma_fence_put(job->fence);
			kfree(job);
			ret = -ENOMEM;
			goto err_unmap_dst;
		}

		/* Fill job with operation parameters */
		job->g2d = g2d;
		job->csc_state = ctx->csc_changed ? ctx->csc_state : g2d->csc_state;
		INIT_WORK(&job->cleanup_work, sunxi_g2d_job_cleanup_workfn);
		job->type = G2D_JOB_FILLRECT;
		job->dst_dma = dst_dma_addr;
		job->data.fillrect.width = cmd.dst_w;
		job->data.fillrect.height = cmd.dst_h;
		job->data.fillrect.pitch = cmd.dst.stride[0] ? cmd.dst.stride[0] : (cmd.dst.width * dst_bpp);
		job->data.fillrect.color = cmd.params.fillrect.color;
		job->data.fillrect.color_format = cmd.dst.format;
		job->data.fillrect.dst_format = cmd.dst.format;

		ret = g2d_job_prepare_rcq(g2d, job);
		if (ret) {
			dma_fence_put(job->fence);
			kfree(job);
			goto err_unmap_dst;
		}

		ret = sunxi_g2d_do_fillrect_rcq(
			g2d, &job->rcq, job->dst_dma, job->data.fillrect.width,
			job->data.fillrect.height, job->data.fillrect.pitch,
			job->data.fillrect.color,
			job->data.fillrect.color_format,
			job->data.fillrect.dst_format);
		if (ret) {
			sunxi_g2d_rcq_free(g2d->dev, &job->rcq);
			dma_fence_put(job->fence);
			kfree(job);
			goto err_unmap_dst;
		}
		job->rcq_ready = true;

		/* Store DMA-BUF references for cleanup */
		job->dst_dmabuf = dst_dmabuf;
		job->dst_attach = dst_attach;
		job->dst_sgt = dst_sgt;

		/* Enqueue job (under spinlock) */
		spin_lock_irqsave(&g2d->job_lock, flags);
		list_add_tail(&job->node, &g2d->job_queue);
		spin_unlock_irqrestore(&g2d->job_lock, flags);

		/* Schedule worker to process the job */
		queue_work(g2d->job_wq, &g2d->job_work);

		/* Return fence_fd to userspace */
		cmd.fence_fd_out = job->fence_fd;
		ret = 0;
		if (copy_to_user((void __user *)arg, &cmd, sizeof(cmd))) {
			ret = -EFAULT;
			/* On copy_to_user failure, abort the job and cleanup */
			/* Note: cleanup work will free resources when executed */
			goto err_unmap_dst;
		}

		/* Successfully enqueued and returned fence fd */
		return 0;
	}

err_unmap_dst:
	dma_buf_unmap_attachment(dst_attach, dst_sgt, DMA_TO_DEVICE);
err_detach_dst:
	dma_buf_detach(dst_dmabuf, dst_attach);
err_put_dst:
	dma_buf_put(dst_dmabuf);
	return ret;
}

/*
 * G2D_CMD_MASK handler: Color keying operation
 * Makes specified color range transparent (chromakey)
 */
static long sunxi_g2d_cmd_mask(struct sunxi_g2d_ctx *ctx, unsigned long arg)
{
	struct sunxi_g2d_dev *g2d = ctx->g2d;
	struct g2d_cmd cmd;
	struct dma_buf *src_dmabuf = NULL, *dst_dmabuf = NULL,
		       *out_dmabuf = NULL;
	struct dma_buf_attachment *src_attach = NULL, *dst_attach = NULL,
				  *out_attach = NULL;
	struct sg_table *src_sgt = NULL, *dst_sgt = NULL, *out_sgt = NULL;
	dma_addr_t src_dma_addr, dst_dma_addr, out_dma_addr;
	u32 src_bpp, dst_bpp, out_bpp;
	int ret = 0;
	struct dma_fence *fence = NULL;
	struct sync_file *sync_file = NULL;
	int fence_fd = -1;
	bool fd_installed = false;
	struct sunxi_g2d_job *job = NULL;
	bool has_out_buffer;

	/* Copy command from userspace */
	if (copy_from_user(&cmd, (void __user *)arg, sizeof(cmd)))
		return -EFAULT;

	/* Validate buffer formats */
	if (sunxi_g2d_format_to_hw(cmd.src.format, &src_bpp) < 0) {
		dev_err(g2d->dev, "Unsupported source format: %u\n",
			cmd.src.format);
		return -EOPNOTSUPP;
	}
	if (sunxi_g2d_format_to_hw(cmd.dst.format, &dst_bpp) < 0) {
		dev_err(g2d->dev, "Unsupported destination format: %u\n",
			cmd.dst.format);
		return -EOPNOTSUPP;
	}

	/* Check if output buffer is provided */
	has_out_buffer = (cmd.out.dma_fd >= 0);

	if (has_out_buffer) {
		if (sunxi_g2d_format_to_hw(cmd.out.format, &out_bpp) < 0) {
			dev_err(g2d->dev, "Unsupported output format: %u\n",
				cmd.out.format);
			return -EOPNOTSUPP;
		}
	}

	/* Import source buffer */
	src_dmabuf = dma_buf_get(cmd.src.dma_fd);
	if (IS_ERR(src_dmabuf))
		return PTR_ERR(src_dmabuf);

	src_attach = dma_buf_attach(src_dmabuf, g2d->dev);
	if (IS_ERR(src_attach)) {
		ret = PTR_ERR(src_attach);
		goto cleanup;
	}

	src_sgt = dma_buf_map_attachment(src_attach, DMA_TO_DEVICE);
	if (IS_ERR(src_sgt)) {
		ret = PTR_ERR(src_sgt);
		goto cleanup;
	}

	/* Pre-compute strides/offsets for potential staging */
	u32 src_stride[3], src_plane_offset[3];
	sunxi_g2d_get_yuv_plane_info(cmd.src.format, cmd.src.width,
				     cmd.src.height, cmd.src.stride, src_stride,
				     src_plane_offset);

	ret = g2d_sg_dma_address(g2d->dev, src_sgt, &src_dma_addr,
				 "CMD_MASK src");
	if (ret) {
		goto cleanup;
	}

	/* Import destination buffer */
	dst_dmabuf = dma_buf_get(cmd.dst.dma_fd);
	if (IS_ERR(dst_dmabuf)) {
		ret = PTR_ERR(dst_dmabuf);
		goto cleanup;
	}

	dst_attach = dma_buf_attach(dst_dmabuf, g2d->dev);
	if (IS_ERR(dst_attach)) {
		ret = PTR_ERR(dst_attach);
		goto cleanup;
	}

	dst_sgt = dma_buf_map_attachment(
		dst_attach, has_out_buffer ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
	if (IS_ERR(dst_sgt)) {
		ret = PTR_ERR(dst_sgt);
		goto cleanup;
	}

	ret = g2d_sg_dma_address(g2d->dev, dst_sgt, &dst_dma_addr,
				 "CMD_MASK dst");
	if (ret)
		goto cleanup;

	/* Import output buffer if provided */
	if (has_out_buffer) {
		out_dmabuf = dma_buf_get(cmd.out.dma_fd);
		if (IS_ERR(out_dmabuf)) {
			ret = PTR_ERR(out_dmabuf);
			goto cleanup;
		}

		out_attach = dma_buf_attach(out_dmabuf, g2d->dev);
		if (IS_ERR(out_attach)) {
			ret = PTR_ERR(out_attach);
			goto cleanup;
		}

		out_sgt = dma_buf_map_attachment(out_attach, DMA_FROM_DEVICE);
		if (IS_ERR(out_sgt)) {
			ret = PTR_ERR(out_sgt);
			goto cleanup;
		}

		ret = g2d_sg_dma_address(g2d->dev, out_sgt, &out_dma_addr,
					 "CMD_MASK out");
		if (ret)
			goto cleanup;
	} else {
		/* No output buffer - mask in-place (dst is both input and output) */
		out_dma_addr = dst_dma_addr;
	}

	/* Create fence for async operation */
	fence = sunxi_g2d_fence_create(g2d);
	if (!fence) {
		ret = -ENOMEM;
		goto cleanup;
	}

	/* Create sync_file for userspace */
	sync_file = sync_file_create(fence);
	if (!sync_file) {
		dma_fence_put(fence);
		ret = -ENOMEM;
		goto cleanup;
	}

	fence_fd = get_unused_fd_flags(O_CLOEXEC);
	if (fence_fd < 0) {
		fput(sync_file->file);
		ret = fence_fd;
		goto cleanup;
	}

	/* Create/obtain async job (from pool if available) */
	job = g2d_job_alloc(g2d);
	if (IS_ERR(job)) {
		put_unused_fd(fence_fd);
		fput(sync_file->file);
		ret = PTR_ERR(job);
		goto cleanup;
	}

	INIT_WORK(&job->cleanup_work, sunxi_g2d_job_cleanup_workfn);
	job->g2d = g2d;
	job->csc_state = ctx->csc_changed ? ctx->csc_state : g2d->csc_state;
	job->fence = fence;
	job->src_dmabuf = src_dmabuf;
	job->dst_dmabuf = dst_dmabuf;
	job->src_attach = src_attach;
	job->dst_attach = dst_attach;
	job->src_sgt = src_sgt;
	job->dst_sgt = dst_sgt;

	if (has_out_buffer) {
		job->out_dmabuf = out_dmabuf;
		job->out_attach = out_attach;
		job->out_sgt = out_sgt;
	}

	/* Setup job parameters */
	job->type = G2D_JOB_CMD_MASK;
	job->src_dma = src_dma_addr;
	job->dst_dma = dst_dma_addr;
	job->out_dma = out_dma_addr;

	/* Calculate proper strides for YUV formats (dst/out only; src precomputed) */
	u32 dst_stride[3], dst_plane_offset[3];
	u32 out_stride[3], out_plane_offset[3];
	sunxi_g2d_get_yuv_plane_info(cmd.dst.format, cmd.dst.width,
				     cmd.dst.height, cmd.dst.stride, dst_stride,
				     dst_plane_offset);
	if (has_out_buffer) {
		sunxi_g2d_get_yuv_plane_info(cmd.out.format, cmd.out.width,
					     cmd.out.height, cmd.out.stride,
					     out_stride, out_plane_offset);
	}

	/* Fill blit data structure */
	job->data.blit.src_width = cmd.src.width;
	job->data.blit.src_height = cmd.src.height;
	job->data.blit.src_pitch = src_stride[0];
	job->data.blit.src_format = cmd.src.format;
	job->data.blit.src_crop_x = cmd.src.crop_x;
	job->data.blit.src_crop_y = cmd.src.crop_y;
	job->data.blit.src_crop_w = cmd.src.crop_w;
	job->data.blit.src_crop_h = cmd.src.crop_h;
	job->data.blit.src_alpha = cmd.src.alpha;
	job->data.blit.src_alpha_mode = cmd.src.alpha_mode;
	job->data.blit.src_premul = cmd.src.premul_mode;
	job->data.blit.src_color_space = cmd.src.color_space;

	job->data.blit.dst_width = cmd.dst.width;
	job->data.blit.dst_height = cmd.dst.height;
	job->data.blit.dst_pitch = dst_stride[0];
	job->data.blit.dst_format = cmd.dst.format;
	job->data.blit.dst_x = cmd.dst_x;
	job->data.blit.dst_y = cmd.dst_y;
	job->data.blit.dst_w = cmd.dst_w;
	job->data.blit.dst_h = cmd.dst_h;
	job->data.blit.dst_alpha = cmd.dst.alpha;
	job->data.blit.dst_alpha_mode = cmd.dst.alpha_mode;
	job->data.blit.dst_premul = cmd.dst.premul_mode;
	job->data.blit.dst_color_space = cmd.dst.color_space;

	if (has_out_buffer) {
		job->data.blit.out_width = cmd.out.width;
		job->data.blit.out_height = cmd.out.height;
		job->data.blit.out_pitch = out_stride[0];
		job->data.blit.out_format = cmd.out.format;
		job->data.blit.out_color_space = cmd.out.color_space;
	} else {
		/* In-place: out parameters = dst parameters */
		job->data.blit.out_width = cmd.dst.width;
		job->data.blit.out_height = cmd.dst.height;
		job->data.blit.out_pitch = dst_stride[0];
		job->data.blit.out_format = cmd.dst.format;
		job->data.blit.out_color_space = cmd.dst.color_space;
	}

	/* Color keying parameters */
	job->data.blit.color_key_enable = 1;
	job->data.blit.color_key_mode = cmd.params.mask.color_key_mode;
	job->data.blit.color_key_min = cmd.params.mask.color_key_min;
	job->data.blit.color_key_max = cmd.params.mask.color_key_max;

	/* Use SRCOVER blend mode for masking */
	job->data.blit.bld_mode = G2D_BLD_SRCOVER;

	ret = g2d_job_prepare_rcq(g2d, job);
	if (ret)
		goto cleanup;

	ret = sunxi_g2d_do_blend_rcq(
		g2d, &job->rcq, job->src_dma, job->data.blit.src_width,
		job->data.blit.src_height, job->data.blit.src_pitch,
		job->data.blit.src_format, job->data.blit.src_crop_x,
		job->data.blit.src_crop_y, job->data.blit.src_crop_w,
		job->data.blit.src_crop_h, job->dst_dma, job->data.blit.dst_width,
		job->data.blit.dst_height, job->data.blit.dst_pitch,
		job->data.blit.dst_format, job->data.blit.dst_x,
		job->data.blit.dst_y, job->out_dma, job->data.blit.out_width,
		job->data.blit.out_height, job->data.blit.out_pitch,
		job->data.blit.out_format, job->data.blit.dst_x,
		job->data.blit.dst_y, job->data.blit.dst_w, job->data.blit.dst_h,
		job->data.blit.bld_mode, job->data.blit.src_alpha_mode,
		job->data.blit.src_alpha, job->data.blit.src_premul,
		job->data.blit.src_color_space, job->data.blit.dst_color_space,
		job->data.blit.out_color_space, &job->csc_state);
	if (ret)
		goto cleanup;
	job->rcq_ready = true;

	/* Enqueue job to worker */
	{
		unsigned long flags;
		spin_lock_irqsave(&g2d->job_lock, flags);
		list_add_tail(&job->node, &g2d->job_queue);
		spin_unlock_irqrestore(&g2d->job_lock, flags);
	}

	queue_work(g2d->job_wq, &g2d->job_work);

	/* Install fence fd */
	fd_install(fence_fd, sync_file->file);
	fd_installed = true;

	/* Return fence fd to userspace */
	if (put_user(fence_fd, &((struct g2d_cmd __user *)arg)->fence_fd_out)) {
		dev_err(g2d->dev, "Failed to copy fence_fd_out to userspace\n");
		/* Job already enqueued, can't cleanly abort */
		return -EFAULT;
	}

	dev_dbg(g2d->dev, "G2D_CMD_MASK: enqueued job, fence_fd=%d\n",
		fence_fd);

	return 0;

cleanup:
	if (!IS_ERR_OR_NULL(job)) {
		if (job->rcq.vir_addr)
			sunxi_g2d_rcq_free(g2d->dev, &job->rcq);
		g2d_job_free(g2d, job);
	}

	if (!fd_installed && fence_fd >= 0)
		put_unused_fd(fence_fd);
	if (!fd_installed && sync_file && sync_file->file)
		fput(sync_file->file);
	if (fence)
		dma_fence_put(fence);

	if (has_out_buffer && out_sgt && out_attach)
		dma_buf_unmap_attachment(out_attach, out_sgt, DMA_FROM_DEVICE);
	if (has_out_buffer && out_attach && out_dmabuf)
		dma_buf_detach(out_dmabuf, out_attach);
	if (has_out_buffer && out_dmabuf && !IS_ERR(out_dmabuf))
		dma_buf_put(out_dmabuf);

	if (dst_sgt && dst_attach)
		dma_buf_unmap_attachment(dst_attach, dst_sgt,
					 has_out_buffer ? DMA_TO_DEVICE :
							  DMA_FROM_DEVICE);
	if (dst_attach && dst_dmabuf)
		dma_buf_detach(dst_dmabuf, dst_attach);
	if (dst_dmabuf && !IS_ERR(dst_dmabuf))
		dma_buf_put(dst_dmabuf);

	if (src_sgt && src_attach)
		dma_buf_unmap_attachment(src_attach, src_sgt, DMA_TO_DEVICE);
	if (src_attach && src_dmabuf)
		dma_buf_detach(src_dmabuf, src_attach);
	if (src_dmabuf && !IS_ERR(src_dmabuf))
		dma_buf_put(src_dmabuf);

	return ret;
}

/*
 * G2D_CMD_COPY handler: Simple copy/blit operation
 * Copies src buffer region to dst buffer at specified position (no scaling)
 */
static long sunxi_g2d_cmd_copy(struct sunxi_g2d_ctx *ctx, unsigned long arg)
{
	struct sunxi_g2d_dev *g2d = ctx->g2d;
	struct g2d_cmd cmd;
	struct dma_buf *src_dmabuf = NULL, *dst_dmabuf = NULL;
	struct dma_buf_attachment *src_attach = NULL, *dst_attach = NULL;
	struct sg_table *src_sgt = NULL, *dst_sgt = NULL;
	dma_addr_t src_dma_addr, dst_dma_addr;
	u32 src_bpp, dst_bpp;
	int ret = 0;
	struct dma_fence *fence = NULL;
	struct sync_file *sync_file = NULL;
	int fence_fd = -1;
	bool fd_installed = false;
	struct sunxi_g2d_job *job = NULL;

	/* Copy command from userspace */
	if (copy_from_user(&cmd, (void __user *)arg, sizeof(cmd)))
		return -EFAULT;

	/* Validate buffer formats */
	if (sunxi_g2d_format_to_hw(cmd.src.format, &src_bpp) < 0) {
		dev_err(g2d->dev, "Unsupported source format: %u\n",
			cmd.src.format);
		return -EOPNOTSUPP;
	}
	if (sunxi_g2d_format_to_hw(cmd.dst.format, &dst_bpp) < 0) {
		dev_err(g2d->dev, "Unsupported destination format: %u\n",
			cmd.dst.format);
		return -EOPNOTSUPP;
	}

	/* Validate dimensions - for COPY, no scaling allowed */
	if (cmd.src.crop_w != cmd.dst_w || cmd.src.crop_h != cmd.dst_h) {
		dev_err(g2d->dev,
			"G2D_CMD_COPY requires src crop size == dst size (no scaling)\n");
		return -EINVAL;
	}

	/* Import source buffer */
	src_dmabuf = dma_buf_get(cmd.src.dma_fd);
	if (IS_ERR(src_dmabuf))
		return PTR_ERR(src_dmabuf);

	src_attach = dma_buf_attach(src_dmabuf, g2d->dev);
	if (IS_ERR(src_attach)) {
		ret = PTR_ERR(src_attach);
		goto cleanup;
	}

	src_sgt = dma_buf_map_attachment(src_attach, DMA_TO_DEVICE);
	if (IS_ERR(src_sgt)) {
		ret = PTR_ERR(src_sgt);
		goto cleanup;
	}

	/* Pre-compute strides/offsets for possible staging */
	u32 src_stride[3], src_plane_offset[3];
	sunxi_g2d_get_yuv_plane_info(cmd.src.format, cmd.src.width,
				     cmd.src.height, cmd.src.stride, src_stride,
				     src_plane_offset);

	/* Force or fallback source staging */
	ret = g2d_sg_dma_address(g2d->dev, src_sgt, &src_dma_addr,
				 "CMD_COPY src");
	if (ret) {
		goto cleanup;
	}

	/* Import destination buffer */
	dst_dmabuf = dma_buf_get(cmd.dst.dma_fd);
	if (IS_ERR(dst_dmabuf)) {
		ret = PTR_ERR(dst_dmabuf);
		goto cleanup;
	}

	dst_attach = dma_buf_attach(dst_dmabuf, g2d->dev);
	if (IS_ERR(dst_attach)) {
		ret = PTR_ERR(dst_attach);
		goto cleanup;
	}

	dst_sgt = dma_buf_map_attachment(dst_attach, DMA_FROM_DEVICE);
	if (IS_ERR(dst_sgt)) {
		ret = PTR_ERR(dst_sgt);
		goto cleanup;
	}

	/* Determine if writeback staging is needed for destination (output) */
	{
		ret = g2d_sg_dma_address(g2d->dev, dst_sgt, &dst_dma_addr,
					 "CMD_COPY dst");
		if (ret) {
			goto cleanup;
		}
	}

	/* Create fence for async operation */
	fence = sunxi_g2d_fence_create(g2d);
	if (!fence) {
		ret = -ENOMEM;
		goto cleanup;
	}

	/* Create sync_file for userspace */
	sync_file = sync_file_create(fence);
	if (!sync_file) {
		dma_fence_put(fence);
		ret = -ENOMEM;
		goto cleanup;
	}

	fence_fd = get_unused_fd_flags(O_CLOEXEC);
	if (fence_fd < 0) {
		fput(sync_file->file);
		ret = fence_fd;
		goto cleanup;
	}

	/* Create/obtain async job (from pool if available) */
	job = g2d_job_alloc(g2d);
	if (!job) {
		put_unused_fd(fence_fd);
		fput(sync_file->file);
		ret = -ENOMEM;
		goto cleanup;
	}

	INIT_WORK(&job->cleanup_work, sunxi_g2d_job_cleanup_workfn);
	job->g2d = g2d;
	job->csc_state = ctx->csc_changed ? ctx->csc_state : g2d->csc_state;
	job->fence = fence;
	job->src_dmabuf = src_dmabuf;
	job->dst_dmabuf = dst_dmabuf;
	job->src_attach = src_attach;
	job->dst_attach = dst_attach;
	job->src_sgt = src_sgt;
	job->dst_sgt = dst_sgt;

	/* Setup job parameters */
	job->type = G2D_JOB_CMD_COPY;
	job->src_dma = src_dma_addr;
	job->dst_dma = dst_dma_addr;

	/* Calculate proper strides for YUV formats (dst only; src already computed) */
	u32 dst_stride[3], dst_plane_offset[3];
	sunxi_g2d_get_yuv_plane_info(cmd.dst.format, cmd.dst.width,
				     cmd.dst.height, cmd.dst.stride, dst_stride,
				     dst_plane_offset);

	/* Fill blit data structure */
	job->data.blit.src_width = cmd.src.width;
	job->data.blit.src_height = cmd.src.height;
	job->data.blit.src_pitch = src_stride[0]; /* Use calculated stride */
	job->data.blit.src_format = cmd.src.format;
	job->data.blit.src_crop_x = cmd.src.crop_x;
	job->data.blit.src_crop_y = cmd.src.crop_y;
	job->data.blit.src_crop_w = cmd.src.crop_w;
	job->data.blit.src_crop_h = cmd.src.crop_h;
	job->data.blit.src_color_space = cmd.src.color_space;

	job->data.blit.dst_width = cmd.dst.width;
	job->data.blit.dst_height = cmd.dst.height;
	job->data.blit.dst_pitch = dst_stride[0]; /* Use calculated stride */
	job->data.blit.dst_format = cmd.dst.format;
	job->data.blit.dst_x = cmd.dst_x;
	job->data.blit.dst_y = cmd.dst_y;
	job->data.blit.dst_w = cmd.dst_w;
	job->data.blit.dst_h = cmd.dst_h;
	job->data.blit.dst_color_space = cmd.dst.color_space;

	ret = g2d_job_prepare_rcq(g2d, job);
	if (ret)
		goto cleanup;

	ret = sunxi_g2d_do_blit_rcq(
		g2d, &job->rcq, job->src_dma, job->data.blit.src_width,
		job->data.blit.src_height, job->data.blit.src_pitch,
		job->data.blit.src_format, job->data.blit.src_crop_x,
		job->data.blit.src_crop_y, job->data.blit.src_crop_w,
		job->data.blit.src_crop_h, job->dst_dma, job->data.blit.dst_width,
		job->data.blit.dst_height, job->data.blit.dst_pitch,
		job->data.blit.dst_format, job->data.blit.dst_x,
		job->data.blit.dst_y, job->data.blit.dst_w, job->data.blit.dst_h,
		job->data.blit.src_color_space, job->data.blit.dst_color_space);
	if (ret)
		goto cleanup;
	job->rcq_ready = true;

	/* Enqueue job to worker */
	{
		unsigned long flags;
		spin_lock_irqsave(&g2d->job_lock, flags);
		list_add_tail(&job->node, &g2d->job_queue);
		spin_unlock_irqrestore(&g2d->job_lock, flags);
	}

	queue_work(g2d->job_wq, &g2d->job_work);

	/* Install fence fd */
	fd_install(fence_fd, sync_file->file);
	fd_installed = true;

	/* Return fence fd to userspace */
	if (put_user(fence_fd, &((struct g2d_cmd __user *)arg)->fence_fd_out)) {
		dev_err(g2d->dev, "Failed to copy fence_fd_out to userspace\n");
		/* Job already enqueued, can't cleanly abort */
		return -EFAULT;
	}

	dev_dbg(g2d->dev, "G2D_CMD_COPY: enqueued job, fence_fd=%d\n",
		fence_fd);

	return 0;

cleanup:
	if (!IS_ERR_OR_NULL(job)) {
		if (job->rcq.vir_addr)
			sunxi_g2d_rcq_free(g2d->dev, &job->rcq);
		g2d_job_free(g2d, job);
	}

	if (!fd_installed && fence_fd >= 0)
		put_unused_fd(fence_fd);
	if (!fd_installed && sync_file && sync_file->file)
		fput(sync_file->file);
	if (fence)
		dma_fence_put(fence);

	if (dst_sgt && dst_attach)
		dma_buf_unmap_attachment(dst_attach, dst_sgt, DMA_FROM_DEVICE);
	if (dst_attach && dst_dmabuf)
		dma_buf_detach(dst_dmabuf, dst_attach);
	if (dst_dmabuf && !IS_ERR(dst_dmabuf))
		dma_buf_put(dst_dmabuf);

	if (src_sgt && src_attach)
		dma_buf_unmap_attachment(src_attach, src_sgt, DMA_TO_DEVICE);
	if (src_attach && src_dmabuf)
		dma_buf_detach(src_dmabuf, src_attach);
	if (src_dmabuf && !IS_ERR(src_dmabuf))
		dma_buf_put(src_dmabuf);

	return ret;
}

/**
 * sunxi_g2d_do_blit_rcq - Execute bitblt/copy with RCQ using modular builders
 * 
 * RCQ version of simple copy/blit operation (no scaling, no rotation).
 * Uses builder functions for V0 (memory source), BLD (simple copy), and WB (output).
 */
static int sunxi_g2d_do_blit_rcq(struct sunxi_g2d_dev *g2d,
				 struct g2d_rcq_mem *rcq, dma_addr_t src_dma,
				 u32 src_width, u32 src_height, u32 src_pitch,
				 u32 src_format, u32 src_x, u32 src_y,
				 u32 src_crop_w, u32 src_crop_h,
				 dma_addr_t dst_dma, u32 dst_width,
				 u32 dst_height, u32 dst_pitch, u32 dst_format,
				 u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h,
				 u8 src_color_space, u8 dst_color_space)
{
	struct sunxi_g2d_rcq_frame_layout layout;
	int src_fmt_val, dst_fmt_val;
	int ret;
	u32 src_bpp, dst_bpp;

	/* Blocks allocated by builders (BSP order V0+UI0+UI1+UI2+SCAL+BLD+WB) */
	u32 *v0_regs = NULL, v0_size;
	u32 *u0_regs = NULL, u0_size;
	u32 *u1_regs = NULL, u1_size;
	u32 *u2_regs = NULL, u2_size;
	struct g2d_mixer_bld_reg *bld_regs = NULL;
	u32 bld_size;
	u32 *scal_regs = NULL, scal_size = 0;
	u32 *scal_en_regs = NULL, scal_en_size = 0;
	u32 *wb_regs = NULL, wb_size;

	/* Convert formats */
	src_fmt_val = sunxi_g2d_format_to_hw(src_format, &src_bpp);
	if (src_fmt_val < 0) {
		dev_err(g2d->dev, "Unsupported source format: %u\n",
			src_format);
		return -EOPNOTSUPP;
	}
	dst_fmt_val = sunxi_g2d_format_to_hw(dst_format, &dst_bpp);
	if (dst_fmt_val < 0) {
		dev_err(g2d->dev, "Unsupported destination format: %u\n",
			dst_format);
		return -EOPNOTSUPP;
	}

	/* FIX: If source is YUV, do not swap R/B in destination format.
	 * sunxi_g2d_format_to_hw() swaps ARGB<->ABGR to fix R/B inversion for
	 * standard blits, but CSC (YUV->RGB) seems to not need this inversion.
	 */
	if (sunxi_g2d_is_yuv_planar(src_format) || sunxi_g2d_is_yuv_semiplanar(src_format)) {
		switch (dst_format) {
		case G2D_FMT_ARGB8888:
			dst_fmt_val = G2D_FORMAT_ARGB8888;
			break;
		case G2D_FMT_ABGR8888:
			dst_fmt_val = G2D_FORMAT_ABGR8888;
			break;
		case G2D_FMT_RGBA8888:
			dst_fmt_val = G2D_FORMAT_RGBA8888;
			break;
		case G2D_FMT_BGRA8888:
			dst_fmt_val = G2D_FORMAT_BGRA8888;
			break;
		case G2D_FMT_XRGB8888:
			dst_fmt_val = G2D_FORMAT_XRGB8888;
			break;
		case G2D_FMT_XBGR8888:
			dst_fmt_val = G2D_FORMAT_XBGR8888;
			break;
		}
	}

	if (g2d_rcq_debug) {
		dev_dbg(
			g2d->dev,
			"BLIT_RCQ: src=%ux%u@%u,%u crop=%ux%u fmt=0x%02X -> dst=%u,%u,%ux%u fmt=0x%02X\n",
			src_width, src_height, src_x, src_y, src_crop_w,
			src_crop_h, src_fmt_val, dst_x, dst_y, dst_w, dst_h,
			dst_fmt_val);
		dev_dbg(
			g2d->dev,
			"BLIT_RCQ: src_dma=0x%pad dst_dma=0x%pad src_pitch=%u dst_pitch=%u\n",
			&src_dma, &dst_dma, src_pitch, dst_pitch);
	}

	if (!g2d->rcq_enabled || !rcq || !rcq->vir_addr)
		return -EOPNOTSUPP;

	/* Reset RCQ buffer for this job */
	sunxi_g2d_rcq_reset(rcq);

	/* Keep destination base; positioning is handled via BLD mem_coor */
	dma_addr_t dst_offset_dma;
	u64 dst_offset_bytes = 0;

	/* Handle case where dst_y is passed as a byte offset (legacy/quirk) */
	if (dst_y > dst_height * 2) {
		/* Heuristic: if y is huge, assume it's an offset */
		dst_offset_bytes = dst_y;
		dev_dbg(
			g2d->dev,
			"BLIT_RCQ: dst_y=%u looks like offset, using as offset\n",
			dst_y);
	} else {
		/* Standard coordinate */
		dst_offset_bytes =
			(u64)dst_y * dst_pitch + (u64)dst_x * dst_bpp;
	}

	dst_offset_dma = dst_dma + dst_offset_bytes;

	/* Calculate plane offsets and strides for multi-plane YUV formats
	 * Use the same helper function as legacy MMIO code for consistency
	 */
	u32 src_user_stride[3] = { src_pitch, 0, 0 };
	u32 src_stride[3], src_plane_offset[3];

	sunxi_g2d_get_yuv_plane_info(src_format, src_width, src_height,
				     src_user_stride, src_stride,
				     src_plane_offset);

	/* Apply crop offset to each plane individually if needed
	 * CRITICAL: Crop offset must account for subsampling ratios
	 */
	if (src_x || src_y) {
		if (sunxi_g2d_is_yuv_planar(src_format)) {
			/* YUV planar: 3 separate planes with subsampling */
			src_plane_offset[0] += (src_y * src_stride[0]) + src_x;
			src_plane_offset[1] +=
				((src_y / 2) * src_stride[1]) + (src_x / 2);
			src_plane_offset[2] +=
				((src_y / 2) * src_stride[2]) + (src_x / 2);
		} else if (sunxi_g2d_is_yuv_semiplanar(src_format)) {
			/* YUV semi-planar: 2 planes (Y + UV interleaved) */
			src_plane_offset[0] += (src_y * src_stride[0]) + src_x;
			src_plane_offset[1] +=
				((src_y / 2) * src_stride[1]) + src_x;
		} else {
			/* RGB/packed: single plane */
			src_plane_offset[0] +=
				(src_y * src_stride[0]) + (src_x * src_bpp);
		}
	}

	if (g2d_rcq_debug && (sunxi_g2d_is_yuv_planar(src_format) ||
			      sunxi_g2d_is_yuv_semiplanar(src_format))) {
		dev_dbg(
			g2d->dev,
			"YUV planes: Y@0x%x stride=%u, U@0x%x stride=%u, V@0x%x stride=%u\n",
			src_plane_offset[0], src_stride[0], src_plane_offset[1],
			src_stride[1], src_plane_offset[2], src_stride[2]);
	}

	/* Sanity check: destination region must fit within buffer
	 * The region spans from (dst_x, dst_y) to (dst_x + dst_w, dst_y + dst_h)
	 * Last line starts at: (dst_y + dst_h - 1) * dst_pitch
	 * Last pixel ends at: last_line_start + (dst_x + dst_w) * dst_bpp
	 */
	u64 last_pixel_end = dst_offset_bytes + (u64)(dst_h - 1) * dst_pitch +
			     (u64)dst_w * dst_bpp;
	u64 dst_buffer_size = (u64)dst_height * dst_pitch;
	if (last_pixel_end > dst_buffer_size) {
		dev_err(g2d->dev,
			"BLIT_RCQ: dst region out of bounds! last_pixel_end=%llu buffer=%llu\n",
			last_pixel_end, dst_buffer_size);
		dev_err(g2d->dev,
			"  dst: base=0x%pad size=%ux%u pitch=%u x=%u y=%u w=%u h=%u\n",
			&dst_dma, dst_width, dst_height, dst_pitch, dst_x,
			dst_y, dst_w, dst_h);
		return -EINVAL;
	}

	/* ========== BUILD BLOCKS USING MODULAR FUNCTIONS ========== */

	/* Block 0: V0 (memory source) - ACTIVE
	 * Pass base DMA address (before crop offset) and plane_offset array
	 * Builder will calculate final plane addresses: base_dma + plane_offset[i]
	 * Window: full crop size at (0,0)
	 */
	ret = g2d_rcq_build_v0_memory(src_crop_w, src_crop_h, src_stride,
				      src_dma, src_plane_offset, src_fmt_val, 0,
				      0, src_crop_w, src_crop_h, &v0_regs,
				      &v0_size);
	if (ret) {
		dev_err(g2d->dev, "Failed to build V0 block: %d\n", ret);
		goto cleanup;
	}

	/* Block 1: BLD (blender, simple copy mode) - ACTIVE
	 * BLD operates on the cropped region size (what we're actually copying)
	 * Pass source format so BLD can enable CSC1 if needed for YUV→RGB
	 * CRITICAL: BLD position must be (0,0) for simple copy/blit!
	 * The destination offset is handled by WB_LADD0 address calculation.
	 * BLD_CH_OFFSET positions the layer within the blender canvas, not the dest buffer.
	 * 
	 * FIX: Output size must be the CROP size (dst_w, dst_h), not the full buffer size.
	 * We are building a mini-pipeline for just the region being copied.
	 */
	/* Block 2: SCAL (scaler) - Conditionally ACTIVE when scaling is needed
	 * BSP pattern (g2d_bsp_v2.c:1999): VSU only activated if:
	 *   1. Format >= IYUV422 (0x2c, packed YUV422), OR
	 *   2. Scaling occurs (src_size != dst_size)
	 * However, legacy sunxi_g2d_do_blit shows YUV->RGB can be done via CSC0
	 * without VSU if no scaling is needed.
	 * 
	 * UPDATE: For YUV420 formats on T113, V0 layer subsampling logic seems
	 * unreliable (hangs or artifacts). We force VSU usage for YUV420
	 * to let the scaler handle the chroma subsampling correctly.
	 */
	bool is_yuv = (src_fmt_val >= 0x20 && src_fmt_val <= 0x2f);
	bool needs_scaling = (src_crop_w != dst_w) || (src_crop_h != dst_h);
	/* Force VSU for YUV420 formats (0x28-0x2B) to handle subsampling via scaler */
	bool is_yuv420 = (src_fmt_val >= 0x28 && src_fmt_val <= 0x2B);
	/*
	 * Force VSU even for 1:1 RGB copies.
	 * Empirically the RCQ path hangs after a while when the SCAL blocks are
	 * completely omitted (headers with len=0). Enabling a passthrough VSU
	 * keeps the BSP-like block layout (SCAL + SCAL_EN valid) and avoids
	 * RCQ timeouts seen after repeated rotate+copy sequences.
	 */

	dev_dbg(
		g2d->dev,
		"COPY_RCQ: src=0x%pad→dst=0x%pad crop(%u,%u)→dst(%u,%u,%ux%u) canvas=%ux%u\n",
		&src_dma, &dst_offset_dma, src_crop_w, src_crop_h, dst_x, dst_y,
		dst_w, dst_h, dst_w, dst_h);

	/* Configure BLD:
	 * - Always use Pipe 0 (V0 path) for the source image.
	 * - VSU processes V0 data if enabled, but it still feeds Pipe 0.
	 * - Pipe 1 (UI2) is NOT used for simple blit/copy.
	 */
	ret = g2d_rcq_build_bld(dst_w, dst_h, dst_w, dst_h, dst_w, dst_h,
				src_fmt_val, src_fmt_val, dst_fmt_val,
				src_color_space, src_color_space,
				dst_color_space, true, false, 0, 0, 0, 0,
				G2D_BLD_COPY, 0, false, &g2d->csc_state,
				(u32 **)&bld_regs, &bld_size);
	if (ret) {
		dev_err(g2d->dev, "Failed to build BLD block: %d\n", ret);
		goto cleanup;
	}

	if (needs_scaling || is_yuv420) {
		/* Use active scaler builder for scaling operations OR YUV420 upsampling.
		 * YUV420 requires chroma upscaling (1/2 -> 1/1) even if Luma is 1:1.
		 * The passthrough builder assumes 1:1 for ALL channels, which is wrong for YUV420.
		 */
		ret = g2d_rcq_build_scaler_active(src_crop_w, src_crop_h, dst_w,
						  dst_h, src_fmt_val, 0xFF,
						  &scal_regs, &scal_size);
	} else {
		/* Use passthrough builder for 1:1 operations (optimized) */
		ret = g2d_rcq_build_scaler_passthrough(dst_w, dst_h, src_fmt_val,
						       &scal_regs,
						       &scal_size);
	}

	if (ret) {
		dev_err(g2d->dev, "Failed to build SCAL block: %d\n", ret);
		goto cleanup;
	}

	/* Always build SCAL_EN block to keep BSP RCQ layout */
	ret = g2d_rcq_build_scaler_enable(src_fmt_val, &scal_en_regs,
					  &scal_en_size);
	if (ret) {
		dev_err(g2d->dev, "Failed to build SCAL_EN block: %d\n", ret);
		goto cleanup;
	}

	dev_dbg(g2d->dev, "VSU activated: scaling=%d is_yuv=%d\n",
		needs_scaling, is_yuv);

	/* Block 3: WB (writeback) - ACTIVE
	 * CRITICAL: WB size must match BLD output size (the region being copied),
	 * and address must point to the correct offset within destination buffer */

	ret = g2d_rcq_build_wb(dst_w, dst_h, dst_pitch, dst_offset_dma,
			       dst_fmt_val, &wb_regs, &wb_size);
	if (ret) {
		dev_err(g2d->dev, "Failed to build WB block: %d\n", ret);
		goto cleanup;
	}

	/* Dummy UI headers to preserve BSP ordering */
	ret = g2d_rcq_build_ui_dummy(&u0_regs, &u0_size);
	if (ret)
		goto cleanup;
	ret = g2d_rcq_build_ui_dummy(&u1_regs, &u1_size);
	if (ret)
		goto cleanup;
	ret = g2d_rcq_build_ui_dummy(&u2_regs, &u2_size);
	if (ret)
		goto cleanup;

	/* ========== SETUP RCQ LAYOUT (BSP 7-block order -> 8-block with SCAL_EN) ========== */
	layout.block_count = 8;
	layout.header_len_bytes =
		layout.block_count * sizeof(struct g2d_rcq_header);

	layout.blocks[0].size = v0_size;
	layout.blocks[0].reg_offset = V0_ATTCTL;
	layout.blocks[0].dirty = 1;

	layout.blocks[1].size = u0_size;
	layout.blocks[1].reg_offset = UI0_ATTR;
	layout.blocks[1].dirty = 0;

	layout.blocks[2].size = u1_size;
	layout.blocks[2].reg_offset = UI1_ATTR;
	layout.blocks[2].dirty = 0;

	layout.blocks[3].size = u2_size;
	layout.blocks[3].reg_offset = UI2_ATTR;
	layout.blocks[3].dirty = 0;

	layout.blocks[4].size = scal_size;
	layout.blocks[4].reg_offset = VS_CTRL;
	layout.blocks[4].dirty = 1;

	/* New block: SCAL_EN (VS_CTRL write) */
	layout.blocks[5].size = scal_en_size;
	layout.blocks[5].reg_offset = VS_CTRL;
	layout.blocks[5].dirty = 1;

	layout.blocks[6].size = bld_size;
	layout.blocks[6].reg_offset = BLD_EN_CTL;
	layout.blocks[6].dirty = 1;

	layout.blocks[7].size = wb_size;
	layout.blocks[7].reg_offset = WB_ATT;
	layout.blocks[7].dirty = 1;

	/* ========== PACK INTO RCQ BUFFER ========== */
	/* Instrumentation: dump scaler pointers/sizes and layout for debugging
	 * If SCAL payload is missing in the packed RCQ, these logs help determine
	 * whether scal_regs/scal_en_regs are NULL or sizes are zero at pack time.
	 */
	/*
	dev_dbg(g2d->dev, "RCQ DEBUG: scal_regs=%p scal_size=%u scal_en_regs=%p scal_en_size=%u needs_vsu=%d\n",
		   scal_regs, scal_size, scal_en_regs, scal_en_size, needs_vsu);
	for (int __i = 0; __i < layout.block_count; __i++) {
		dev_dbg(g2d->dev, "RCQ DEBUG: block[%d]=size=%u reg_off=0x%08x dirty=%u\n",
		       __i, layout.blocks[__i].size, layout.blocks[__i].reg_offset,
		       layout.blocks[__i].dirty);
	}
	*/

	ret = sunxi_g2d_rcq_pack_frame_8blocks(rcq, &layout, v0_regs, u0_regs,
					       u1_regs, u2_regs, scal_regs,
					       scal_en_regs, bld_regs,
					       wb_regs);
	if (ret) {
		dev_err(g2d->dev, "Failed to pack RCQ frame: %d\n", ret);
		goto cleanup;
	}

	/* DEBUG: Dump RCQ buffer contents when enabled */
	if (g2d_rcq_debug) {
		dev_dbg(
			g2d->dev,
			"BLIT_RCQ packed: headers=%u used=%u phy_addr=0x%pad\n",
			rcq->header_count, rcq->used, &rcq->phy_addr);

		/* Dump V0 block data */
		dev_dbg(g2d->dev, "V0 block: size=%u bytes\n", v0_size);
		print_hex_dump(KERN_INFO, "V0: ", DUMP_PREFIX_OFFSET, 16, 4,
			       v0_regs, v0_size, false);

		/* Dump BLD block data */
		dev_dbg(g2d->dev, "BLD block: size=%u bytes\n", bld_size);
		print_hex_dump(KERN_INFO, "BLD: ", DUMP_PREFIX_OFFSET, 16, 4,
			       bld_regs, bld_size, false);

		/* Dump WB block data */
		dev_dbg(g2d->dev, "WB block: size=%u bytes\n", wb_size);
		print_hex_dump(KERN_INFO, "WB: ", DUMP_PREFIX_OFFSET, 16, 4,
			       wb_regs, wb_size, false);

		dev_dbg(g2d->dev,
			 "BLIT_RCQ full dump (%u headers, %u bytes):\n",
			 rcq->header_count, rcq->used);
		print_hex_dump(KERN_INFO, "RCQ: ", DUMP_PREFIX_OFFSET, 16, 4,
			       rcq->vir_addr, rcq->used, false);
	}

	ret = 0;

cleanup:
	/* Free all allocated blocks */
	kfree(v0_regs);
	kfree(u0_regs);
	kfree(u1_regs);
	kfree(u2_regs);
	kfree(bld_regs);
	kfree(scal_regs);
	kfree(scal_en_regs);
	kfree(wb_regs);

	return ret;
}

/**
 * sunxi_g2d_do_scale_rcq - Execute scaling with RCQ using modular builders
 * 
 * RCQ version of scaling operation using active VSU scaler.
 * Uses builder functions for V0 (memory source), SCAL (active scaling with VSU),
 * BLD (passthrough), and WB (output).
 * 
 * Architecture: Pure RCQ 4-block configuration
 *   Block 0: V0 (source with crop)
 *   Block 1: BLD (blender passthrough)
 *   Block 2: SCAL (active VSU scaler)
 *   Block 3: WB (writeback output)
 */
static int sunxi_g2d_do_scale_rcq(struct sunxi_g2d_dev *g2d,
				  struct g2d_rcq_mem *rcq, dma_addr_t src_dma,
				  u32 src_width, u32 src_height, u32 src_pitch,
				  u32 src_format, u32 src_x, u32 src_y,
				  u32 src_crop_w, u32 src_crop_h,
				  dma_addr_t dst_dma, u32 dst_width,
				  u32 dst_height, u32 dst_pitch, u32 dst_format,
				  u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h,
				  int src_colorspace, int dst_colorspace,
				  struct g2d_csc_state *csc_state)
{
	struct sunxi_g2d_rcq_frame_layout layout;
	int src_fmt_val, dst_fmt_val;
	int ret;
	u32 src_bpp, dst_bpp;
	bool is_yuv, needs_scaling, is_packed_yuv422, needs_vsu;
	u64 dst_offset_bytes;

	/* Blocks allocated by builders (BSP order V0+UI0+UI1+UI2+SCAL+BLD+WB) */
	u32 *v0_regs = NULL, v0_size;
	u32 *u0_regs = NULL, u0_size;
	u32 *u1_regs = NULL, u1_size;
	u32 *u2_regs = NULL, u2_size;
	struct g2d_mixer_bld_reg *bld_regs = NULL;
	u32 bld_size;
	u32 *scal_regs = NULL, scal_size;
	u32 *scal_en_regs = NULL, scal_en_size;
	u32 *wb_regs = NULL, wb_size;

	/* Convert formats */
	src_fmt_val = sunxi_g2d_format_to_hw(src_format, &src_bpp);
	if (src_fmt_val < 0) {
		dev_err(g2d->dev, "Unsupported source format: %u\n",
			src_format);
		return -EOPNOTSUPP;
	}
	dst_fmt_val = sunxi_g2d_format_to_hw(dst_format, &dst_bpp);
	if (dst_fmt_val < 0) {
		dev_err(g2d->dev, "Unsupported destination format: %u\n",
			dst_format);
		return -EOPNOTSUPP;
	}

	is_yuv = (src_fmt_val >= 0x20 && src_fmt_val <= 0x2f);
	needs_scaling = (src_crop_w != dst_w) || (src_crop_h != dst_h);
	is_packed_yuv422 = (src_fmt_val >= 0x2c);
	/* VSU (scaler) needed for ANY scaling (RGB or YUV) OR any YUV format on V0 */
	needs_vsu = needs_scaling || is_yuv;

	/* Require WB offset alignment to avoid hangs */
	dst_offset_bytes =
		((u64)dst_y * dst_pitch) + ((u64)dst_x * dst_bpp);
	if (dst_offset_bytes & 0x1F) {
		dev_err(g2d->dev,
			"SCALE_RCQ: dst offset not 32-byte aligned (0x%llx)\n",
			dst_offset_bytes);
		return -EINVAL;
	}

	/* Guard against zero sizes/invalid pitches */
	if (!src_crop_w || !src_crop_h || !dst_w || !dst_h) {
		dev_err(g2d->dev,
			"SCALE_RCQ: zero size (src_crop=%ux%u dst=%ux%u)\n",
			src_crop_w, src_crop_h, dst_w, dst_h);
		return -EINVAL;
	}
	if (!src_pitch || !dst_pitch) {
		dev_err(g2d->dev, "SCALE_RCQ: zero pitch (src=%u dst=%u)\n",
			src_pitch, dst_pitch);
		return -EINVAL;
	}
	if (!sunxi_g2d_is_yuv_planar(src_format) &&
	    !sunxi_g2d_is_yuv_semiplanar(src_format)) {
		if (src_pitch < src_width * src_bpp) {
			dev_err(g2d->dev,
				"SCALE_RCQ: src pitch %u too small for width %u bpp %u\n",
				src_pitch, src_width, src_bpp);
			return -EINVAL;
		}
	}
	if (!sunxi_g2d_is_yuv_planar(dst_format) &&
	    !sunxi_g2d_is_yuv_semiplanar(dst_format)) {
		if (dst_pitch < dst_width * dst_bpp) {
			dev_err(g2d->dev,
				"SCALE_RCQ: dst pitch %u too small for width %u bpp %u\n",
				dst_pitch, dst_width, dst_bpp);
			return -EINVAL;
		}
	}

	/* ALIGNMENT CHECK: Verify 32KB alignment for DMA addresses */
	if (src_dma & 0x7FFF) {
		dev_dbg(
			g2d->dev,
			"SCALE_RCQ: src_dma=0x%llx NOT 32KB aligned (offset=0x%llx)\n",
			(unsigned long long)src_dma,
			(unsigned long long)(src_dma & 0x7FFF));
	}
	if (dst_dma & 0x7FFF) {
		dev_dbg(
			g2d->dev,
			"SCALE_RCQ: dst_dma=0x%llx NOT 32KB aligned (offset=0x%llx)\n",
			(unsigned long long)dst_dma,
			(unsigned long long)(dst_dma & 0x7FFF));
	}

	if (g2d_rcq_debug) {
		dev_dbg(g2d->dev, "SCALE_RCQ: src_dma=0x%llx dst_dma=0x%llx\n",
			 (unsigned long long)src_dma,
			 (unsigned long long)dst_dma);
		dev_dbg(
			g2d->dev,
			"SCALE_RCQ: src=%ux%u@%u,%u crop=%ux%u fmt=0x%02X -> dst=%u,%u,%ux%u fmt=0x%02X\n",
			src_width, src_height, src_x, src_y, src_crop_w,
			src_crop_h, src_fmt_val, dst_x, dst_y, dst_w, dst_h,
			dst_fmt_val);
		dev_dbg(g2d->dev, "SCALE_RCQ: Scaling %ux%u → %ux%u\n",
			 src_crop_w, src_crop_h, dst_w, dst_h);
	}

	if (!g2d->rcq_enabled || !rcq || !rcq->vir_addr)
		return -EOPNOTSUPP;

	/* Reset RCQ buffer */
	sunxi_g2d_rcq_reset(rcq);

	/* Calculate plane offsets and strides for multi-plane YUV formats */
	u32 src_user_stride[3] = { src_pitch, 0, 0 };
	u32 src_stride[3], src_plane_offset[3];

	sunxi_g2d_get_yuv_plane_info(src_format, src_width, src_height,
				     src_user_stride, src_stride,
				     src_plane_offset);

	/* Apply crop offset to each plane individually if needed */
	if (src_x || src_y) {
		if (sunxi_g2d_is_yuv_planar(src_format)) {
			/* YUV planar: 3 separate planes with subsampling */
			src_plane_offset[0] += (src_y * src_stride[0]) + src_x;
			src_plane_offset[1] +=
				((src_y / 2) * src_stride[1]) + (src_x / 2);
			src_plane_offset[2] +=
				((src_y / 2) * src_stride[2]) + (src_x / 2);
		} else if (sunxi_g2d_is_yuv_semiplanar(src_format)) {
			/* YUV semi-planar: 2 planes (Y + UV interleaved) */
			src_plane_offset[0] += (src_y * src_stride[0]) + src_x;
			src_plane_offset[1] +=
				((src_y / 2) * src_stride[1]) + src_x;
		} else {
			/* RGB/packed: single plane */
			src_plane_offset[0] +=
				(src_y * src_stride[0]) + (src_x * src_bpp);
		}
	}

	if (g2d_rcq_debug && (sunxi_g2d_is_yuv_planar(src_format) ||
			      sunxi_g2d_is_yuv_semiplanar(src_format))) {
		dev_dbg(
			g2d->dev,
			"YUV planes: Y@0x%x stride=%u, U@0x%x stride=%u, V@0x%x stride=%u\n",
			src_plane_offset[0], src_stride[0], src_plane_offset[1],
			src_stride[1], src_plane_offset[2], src_stride[2]);
	}

	/* ========== BUILD BLOCKS USING MODULAR FUNCTIONS ========== */

	/* Block 0: V0 (memory source) - ACTIVE
	 * Configure source overlay with crop region
	 * Window: full crop size at (0,0)
	 */
	ret = g2d_rcq_build_v0_memory(src_crop_w, src_crop_h, src_stride,
				      src_dma, src_plane_offset, src_fmt_val, 0,
				      0, src_crop_w, src_crop_h, &v0_regs,
				      &v0_size);
	if (ret) {
		dev_err(g2d->dev, "Failed to build V0 block: %d\n", ret);
		goto cleanup;
	}

	/* UI dummy blocks (BSP always emits 3 UI headers) */
	ret = g2d_rcq_build_ui_dummy(&u0_regs, &u0_size);
	if (ret)
		goto cleanup;
	ret = g2d_rcq_build_ui_dummy(&u1_regs, &u1_size);
	if (ret)
		goto cleanup;
	ret = g2d_rcq_build_ui_dummy(&u2_regs, &u2_size);
	if (ret)
		goto cleanup;

	ret = g2d_rcq_build_bld(dst_w, dst_h, dst_w, dst_h, dst_width,
				dst_height, src_fmt_val, src_fmt_val,
				dst_fmt_val, src_colorspace, src_colorspace,
				dst_colorspace, true, false, dst_x, dst_y,
				dst_x, dst_y, G2D_BLD_COPY, 0, false, csc_state,
				(u32 **)&bld_regs, &bld_size);
	if (ret) {
		dev_err(g2d->dev, "Failed to build BLD block: %d\n", ret);
		goto cleanup;
	}

	/* SCAL */
	if (needs_vsu) {
		/* Active scaler configuration */
		ret = g2d_rcq_build_scaler_active(
			src_crop_w, src_crop_h, dst_w, dst_h, src_fmt_val,
			0xFF, /* alpha = 255 (opaque) */
			&scal_regs, &scal_size);
	} else {
		/* Passthrough scaler (1:1) */
		ret = g2d_rcq_build_scaler_passthrough(dst_w, dst_h,
						       src_fmt_val, &scal_regs,
						       &scal_size);
	}
	if (ret) {
		dev_err(g2d->dev, "Failed to build SCAL block: %d\n", ret);
		goto cleanup;
	}

	/* SCAL_EN (Enable VSU) */
	if (needs_vsu) {
		ret = g2d_rcq_build_scaler_enable(src_fmt_val, &scal_en_regs,
						  &scal_en_size);
		if (ret) {
			dev_err(g2d->dev,
				"Failed to build SCAL_EN block: %d\n", ret);
			goto cleanup;
		}
	} else {
		scal_en_size = 0;
	}

	/* WB: Write full destination buffer (BSP pattern) */
	ret = g2d_rcq_build_wb(dst_width, dst_height, dst_pitch, dst_dma,
			       dst_fmt_val, &wb_regs, &wb_size);
	if (ret) {
		dev_err(g2d->dev, "Failed to build WB block: %d\n", ret);
		goto cleanup;
	}

	/* ========== SETUP RCQ LAYOUT (BSP 7-block order -> 8-block with SCAL_EN) ========== */
	layout.block_count = 8;
	layout.header_len_bytes =
		layout.block_count * sizeof(struct g2d_rcq_header);

	layout.blocks[0].size = v0_size;
	layout.blocks[0].reg_offset = V0_ATTCTL;
	layout.blocks[0].dirty = 1;

	layout.blocks[1].size = u0_size;
	layout.blocks[1].reg_offset = UI0_ATTR;
	layout.blocks[1].dirty = 0;

	layout.blocks[2].size = u1_size;
	layout.blocks[2].reg_offset = UI1_ATTR;
	layout.blocks[2].dirty = 0;

	layout.blocks[3].size = u2_size;
	layout.blocks[3].reg_offset = UI2_ATTR;
	layout.blocks[3].dirty = 0;

	layout.blocks[4].size = scal_size;
	layout.blocks[4].reg_offset = VS_CTRL;
	layout.blocks[4].dirty = needs_vsu ? 1 : 0;

	/* New block: SCAL_EN (VS_CTRL write) */
	layout.blocks[5].size = scal_en_size;
	layout.blocks[5].reg_offset = VS_CTRL;
	layout.blocks[5].dirty = (needs_vsu && scal_en_size) ? 1 : 0;

	layout.blocks[6].size = bld_size;
	layout.blocks[6].reg_offset = BLD_EN_CTL;
	layout.blocks[6].dirty = 1;

	layout.blocks[7].size = wb_size;
	layout.blocks[7].reg_offset = WB_ATT;
	layout.blocks[7].dirty = 1;

	/* ========== PACK INTO RCQ BUFFER ========== */
	ret = sunxi_g2d_rcq_pack_frame_8blocks(rcq, &layout, v0_regs, u0_regs,
					       u1_regs, u2_regs, scal_regs,
					       scal_en_regs, bld_regs, wb_regs);
	if (ret) {
		dev_err(g2d->dev, "Failed to pack RCQ frame: %d\n", ret);
		goto cleanup;
	}

	/* DEBUG: Dump RCQ buffer contents when enabled */
	if (g2d_rcq_debug) {
		dev_dbg(
			g2d->dev,
			"SCALE_RCQ packed: headers=%u used=%u phy_addr=0x%pad\n",
			rcq->header_count, rcq->used, &rcq->phy_addr);

		dev_dbg(g2d->dev, "V0 block: size=%u bytes\n", v0_size);
		print_hex_dump(KERN_INFO, "V0: ", DUMP_PREFIX_OFFSET, 16, 4,
			       v0_regs, v0_size, false);

		dev_dbg(g2d->dev, "BLD block: size=%u bytes\n", bld_size);
		print_hex_dump(KERN_INFO, "BLD: ", DUMP_PREFIX_OFFSET, 16, 4,
			       bld_regs, bld_size, false);

		dev_dbg(g2d->dev, "SCAL block: size=%u bytes\n", scal_size);
		print_hex_dump(KERN_INFO, "SCAL: ", DUMP_PREFIX_OFFSET, 16, 4,
			       scal_regs, scal_size, false);

		dev_dbg(g2d->dev, "WB block: size=%u bytes\n", wb_size);
		print_hex_dump(KERN_INFO, "WB: ", DUMP_PREFIX_OFFSET, 16, 4,
			       wb_regs, wb_size, false);
	}

	ret = 0;

cleanup:
	/* Free all allocated blocks */
	kfree(v0_regs);
	kfree(u0_regs);
	kfree(u1_regs);
	kfree(u2_regs);
	kfree(bld_regs);
	kfree(scal_regs);
	kfree(scal_en_regs);
	kfree(wb_regs);

	return ret;
}

static u32 sunxi_g2d_swap_blend_mode(u32 mode)
{
	switch (mode) {
	case G2D_BLD_CLEAR:
		return G2D_BLD_CLEAR;
	case G2D_BLD_COPY:
		return G2D_BLD_DST;
	case G2D_BLD_DST:
		return G2D_BLD_COPY;
	case G2D_BLD_SRCOVER:
		return G2D_BLD_DSTOVER;
	case G2D_BLD_DSTOVER:
		return G2D_BLD_SRCOVER;
	case G2D_BLD_SRCIN:
		return G2D_BLD_DSTIN;
	case G2D_BLD_DSTIN:
		return G2D_BLD_SRCIN;
	case G2D_BLD_SRCOUT:
		return G2D_BLD_DSTOUT;
	case G2D_BLD_DSTOUT:
		return G2D_BLD_SRCOUT;
	case G2D_BLD_SRCATOP:
		return G2D_BLD_DSTATOP;
	case G2D_BLD_DSTATOP:
		return G2D_BLD_SRCATOP;
	case G2D_BLD_XOR:
		return G2D_BLD_XOR;
	default:
		return mode;
	}
}

/**
 * sunxi_g2d_do_blend_rcq - Alpha blending via RCQ (CLEAN pipeline, no MMIO interference)
 * 
 * Implements alpha blending using RCQ exclusively:
 * - UI2 (src foreground) + V0 (dst background) → BLD → WB (out)
 * 
 * This replaces sunxi_g2d_do_blit_alpha_3buf() MMIO path which causes
 * register contamination affecting subsequent RCQ operations.
 */
static int sunxi_g2d_do_blend_rcq(
	struct sunxi_g2d_dev *g2d, struct g2d_rcq_mem *rcq, dma_addr_t src_dma,
	u32 src_width, u32 src_height, u32 src_pitch, u32 src_format, u32 src_x,
	u32 src_y, u32 src_crop_w, u32 src_crop_h, dma_addr_t dst_dma,
	u32 dst_width, u32 dst_height, u32 dst_pitch, u32 dst_format, u32 dst_x,
	u32 dst_y, dma_addr_t out_dma, u32 out_width, u32 out_height,
	u32 out_pitch, u32 out_format, u32 out_x, u32 out_y, u32 blend_w,
	u32 blend_h, u32 bld_mode, u32 alpha_mode, u32 global_alpha,
	u32 premul_mode, u8 src_color_space, u8 dst_color_space,
	u8 out_color_space, struct g2d_csc_state *csc_state)
{
	struct g2d_mixer_ovl_u_reg *ui2_regs = NULL; /* Foreground (src) */
	struct g2d_mixer_ovl_v_reg *v0_regs = NULL; /* Background (dst) */
	struct g2d_mixer_bld_reg *bld_regs = NULL;
	u32 *wb_regs = NULL;
	u32 *u0_regs = NULL, *u1_regs = NULL;
	u32 ui2_size, v0_size, bld_size, wb_size;
	u32 u0_size = 0, u1_size = 0;
	u32 *scal_regs = NULL, scal_size = 0;
	u32 *scal_en_regs = NULL, scal_en_size = 0;
	u32 src_bpp, dst_bpp, out_bpp;
	int src_hw_fmt, dst_hw_fmt, out_hw_fmt;
	int ret;
	bool needs_scaling;
	bool swap_layers = false; /* If true, Src=V0, Dst=UI2 (for scaling) */

	dev_dbg(
		g2d->dev,
		"BLEND_RCQ ENTRY: src=%ux%u pitch=%u @(%u,%u) crop=%ux%u fmt=0x%02x\n",
		src_width, src_height, src_pitch, src_x, src_y, src_crop_w,
		src_crop_h, src_format);
	dev_dbg(g2d->dev,
		 "BLEND_RCQ ENTRY: dst=%ux%u pitch=%u @(%u,%u) fmt=0x%02x\n",
		 dst_width, dst_height, dst_pitch, dst_x, dst_y, dst_format);
	dev_dbg(
		g2d->dev,
		"BLEND_RCQ ENTRY: out=%ux%u pitch=%u @(%u,%u) blend=%ux%u fmt=0x%02x mode=%u\n",
		out_width, out_height, out_pitch, out_x, out_y, blend_w,
		blend_h, out_format, bld_mode);
	dev_dbg(g2d->dev,
		 "BLEND_RCQ ENTRY: DMA src=0x%llx dst=0x%llx out=0x%llx\n",
		 (u64)src_dma, (u64)dst_dma, (u64)out_dma);
	dev_dbg(g2d->dev,
		 "BLEND_RCQ ENTRY: alpha_mode=%u global_alpha=%u premul=%u\n",
		 alpha_mode, global_alpha, premul_mode);

	/* Get bytes-per-pixel and HW format (swapped if needed) */
	src_hw_fmt = sunxi_g2d_format_to_hw(src_format, &src_bpp);
	dst_hw_fmt = sunxi_g2d_format_to_hw(dst_format, &dst_bpp);
	out_hw_fmt = sunxi_g2d_format_to_hw(out_format, &out_bpp);

	if (src_hw_fmt < 0 || dst_hw_fmt < 0 || out_hw_fmt < 0) {
		dev_err(g2d->dev,
			"BLEND_RCQ: invalid format src=0x%02x dst=0x%02x out=0x%02x\n",
			src_format, dst_format, out_format);
		return -EINVAL;
	}

	/* Check if scaling is needed */
	needs_scaling = (src_crop_w != blend_w) || (src_crop_h != blend_h);

	if (needs_scaling) {
		/* Scaling required: Must use V0 for Source (to use VSU).
		 * This forces Source to be on Pipe 1 (Bottom) and Dest on Pipe 0 (Top).
		 * Result: Source appears UNDER Destination.
		 */
		swap_layers = true;
		dev_dbg(g2d->dev, "BLEND_RCQ: Scaling enabled. Swapping layers: Src=V0(Bottom), Dst=UI2(Top).\n");

		/* Fix Z-order inversion:
		 * Since V0 (Src) is physically below UI2 (Dst), we must invert the blend mode
		 * to maintain the logical "Src over Dst" operation.
		 * e.g., SRCOVER becomes DSTOVER.
		 */
		u32 old_mode = bld_mode;
		bld_mode = sunxi_g2d_swap_blend_mode(bld_mode);
		if (bld_mode != old_mode)
			dev_dbg(g2d->dev, "BLEND_RCQ: Swapped blend mode %u -> %u to correct Z-order.\n",
				 old_mode, bld_mode);
	}

	/* Calculate DMA offsets:
	 * - src: crop offset (src_x, src_y) for UI2
	 * - dst: crop offset (dst_x, dst_y) for V0 (read only the blend region)
	 * - out: crop offset (out_x, out_y) for WB (write only the blend region)
	 * 
	 * STRATEGY CHANGE: Instead of configuring V0/WB for the full 800x480 buffer
	 * and relying on window positioning, we configure the entire pipeline
	 * to operate ONLY on the crop region (e.g. 50x50).
	 * - V0 reads 50x50 from dst_dma + offset
	 * - UI2 reads 50x50 from src_dma + offset
	 * - BLD mixes 50x50
	 * - WB writes 50x50 to out_dma + offset
	 * This ensures we don't rely on hardware windowing logic which might be buggy.
	 */
	u32 src_crop_offset = (src_y * src_pitch) + (src_x * src_bpp);
	u32 dst_crop_offset;
	u32 out_crop_offset;

	if (dst_y > dst_height * 2) {
		dst_crop_offset = dst_y;
		dev_dbg(g2d->dev, "BLEND_RCQ: dst_y=%u looks like offset\n",
			 dst_y);
	} else {
		dst_crop_offset = (dst_y * dst_pitch) + (dst_x * dst_bpp);
	}

	if (out_y > out_height * 2) {
		out_crop_offset = out_y;
		dev_dbg(g2d->dev, "BLEND_RCQ: out_y=%u looks like offset\n",
			 out_y);
	} else {
		out_crop_offset = (out_y * out_pitch) + (out_x * out_bpp);
	}

	/* Plane offsets for V0 (if used for Src or Dst) */
	u32 v0_plane_offset[3];
	u32 v0_stride[3];

	if (swap_layers) {
		/* Src is V0 (YUV/RGB), Dst is UI2 (RGB) */
		/* Calculate plane offsets for Src (V0) */
		u32 src_user_stride[3] = { src_pitch, 0, 0 };
		u32 src_hw_stride[3];
		u32 src_hw_offset[3];
		
		sunxi_g2d_get_yuv_plane_info(src_format, src_width, src_height,
					     src_user_stride, src_hw_stride,
					     src_hw_offset);
		
		/* Apply crop offset */
		if (src_x || src_y) {
			if (sunxi_g2d_is_yuv_planar(src_format)) {
				src_hw_offset[0] += (src_y * src_hw_stride[0]) + src_x;
				src_hw_offset[1] += ((src_y / 2) * src_hw_stride[1]) + (src_x / 2);
				src_hw_offset[2] += ((src_y / 2) * src_hw_stride[2]) + (src_x / 2);
			} else if (sunxi_g2d_is_yuv_semiplanar(src_format)) {
				src_hw_offset[0] += (src_y * src_hw_stride[0]) + src_x;
				src_hw_offset[1] += ((src_y / 2) * src_hw_stride[1]) + src_x;
			} else {
				src_hw_offset[0] += (src_y * src_hw_stride[0]) + (src_x * src_bpp);
			}
		}
		
		v0_plane_offset[0] = src_hw_offset[0];
		v0_plane_offset[1] = src_hw_offset[1];
		v0_plane_offset[2] = src_hw_offset[2];
		v0_stride[0] = src_hw_stride[0];
		v0_stride[1] = src_hw_stride[1];
		v0_stride[2] = src_hw_stride[2];
	} else {
		/* Src is UI2, Dst is V0 */
		v0_plane_offset[0] = dst_crop_offset;
		v0_plane_offset[1] = 0;
		v0_plane_offset[2] = 0;
		v0_stride[0] = dst_pitch;
		v0_stride[1] = 0;
		v0_stride[2] = 0;
	}

	dev_dbg(g2d->dev,
		"BLEND_RCQ: src_offset=0x%x dst_offset=0x%x out_offset=0x%x\n",
		src_crop_offset, dst_crop_offset, out_crop_offset);

	if (swap_layers) {
		/* Build UI2 block (Background/Dst) - reads dst crop */
		ret = g2d_rcq_build_ui2_memory(blend_w, blend_h, dst_pitch,
					       dst_dma, dst_crop_offset, dst_hw_fmt, 0,
					       0, blend_w, blend_h, G2D_PIXEL_ALPHA,
					       0xFF, G2D_PREMUL_NONE, &ui2_regs,
					       &ui2_size);
		
		/* Build V0 block (Foreground/Src) - reads src crop */
		/* Note: V0 size is src_crop_w/h (input to scaler) */
		ret = g2d_rcq_build_v0_memory(src_crop_w, src_crop_h, v0_stride, src_dma,
					      v0_plane_offset, src_hw_fmt, 0, 0,
					      src_crop_w, src_crop_h, (u32 **)&v0_regs,
					      &v0_size);
	} else {
		/* Build UI2 block (Foreground/Src) - reads src crop
		 * Position at (0,0) relative to the mini-pipeline
		 */
		ret = g2d_rcq_build_ui2_memory(src_crop_w, src_crop_h, src_pitch,
					       src_dma, src_crop_offset, src_hw_fmt, 0,
					       0, blend_w, blend_h, alpha_mode,
					       global_alpha, premul_mode, &ui2_regs,
					       &ui2_size);
		if (ret) {
			dev_err(g2d->dev, "BLEND_RCQ: failed to build UI2 block: %d\n",
				ret);
			goto cleanup;
		}

		/* Build V0 block (Background/Dst) - reads dst crop
		 * Position at (0,0) relative to the mini-pipeline
		 */
		ret = g2d_rcq_build_v0_memory(blend_w, blend_h, v0_stride, dst_dma,
					      v0_plane_offset, dst_hw_fmt, 0, 0,
					      blend_w, blend_h, (u32 **)&v0_regs,
					      &v0_size);
	}
	
	if (ret) {
		dev_err(g2d->dev, "BLEND_RCQ: failed to build UI2/V0 block: %d\n",
			ret);
		goto cleanup;
	}

	/* Build BLD block:
	 * - Output size: blend_w x blend_h
	 * - Pipe 0 (UI2) pos: 0,0
	 * - Pipe 1 (V0) pos: 0,0
	 * 
	 * CORRECTION: UI2 is Pipe 0, V0 is Pipe 1.
	 * We must pass src_hw_fmt (UI2) as fmt_p0 and dst_hw_fmt (V0) as fmt_p1.
	 */
	u32 fmt_p0 = swap_layers ? dst_hw_fmt : src_hw_fmt;
	u32 fmt_p1 = swap_layers ? src_hw_fmt : dst_hw_fmt;
	u8 cs_p0 = swap_layers ? dst_color_space : src_color_space;
	u8 cs_p1 = swap_layers ? src_color_space : dst_color_space;
	
	/* Fix: Pass blend_w/h for both pipes.
	 * If scaling is active, VSU scales Pipe 0 to blend_w.
	 * Pipe 1 (V0) is configured to blend_w ROI.
	 */
	ret = g2d_rcq_build_bld(blend_w, blend_h, blend_w, blend_h, blend_w,
				blend_h, fmt_p0, fmt_p1, out_hw_fmt, cs_p0,
				cs_p1, out_color_space, true, true, 0, 0, 0, 0,
				bld_mode, premul_mode, false, csc_state,
				(u32 **)&bld_regs, &bld_size);
	if (ret) {
		dev_err(g2d->dev, "BLEND_RCQ: failed to build BLD block: %d\n",
			ret);
		goto cleanup;
	}

	/* Build WB block (output) - writes out crop
	 * Writes to out_dma + out_crop_offset
	 */
	ret = g2d_rcq_build_wb(blend_w, blend_h, out_pitch,
			       out_dma + out_crop_offset, out_hw_fmt,
			       (u32 **)&wb_regs, &wb_size);
	if (ret) {
		dev_err(g2d->dev, "BLEND_RCQ: failed to build WB block: %d\n",
			ret);
		goto cleanup;
	}

	/* Debug: show WB register values */
	if (wb_regs) {
		dev_dbg(g2d->dev,
			 "BLEND_RCQ WB regs: att=0x%08x size=0x%08x pitch=%u\n",
			 wb_regs[0], wb_regs[1], wb_regs[2]);
		dev_dbg(g2d->dev,
			 "BLEND_RCQ WB regs: laddr0=0x%08x haddr=0x%08x\n",
			 wb_regs[5], wb_regs[6]);
	}

	/* Dummy UI0/UI1 (BSP keeps fixed UI slots) */
	ret = g2d_rcq_build_ui_dummy(&u0_regs, &u0_size);
	if (ret)
		goto cleanup;
	ret = g2d_rcq_build_ui_dummy(&u1_regs, &u1_size);
	if (ret)
		goto cleanup;
	/* Build Scaler block (Active or Dummy) */
	if (needs_scaling) {
		/* Active Scaler on V0 (Pipe 1) */
		/* If swap_layers is true, V0 is Source.
		 * Input size: src_crop_w x src_crop_h
		 * Output size: blend_w x blend_h
		 */
		ret = g2d_rcq_build_scaler_active(src_crop_w, src_crop_h,
						  blend_w, blend_h,
						  src_hw_fmt, 0xFF,
						  &scal_regs,
						  &scal_size);
		if (ret) {
			dev_err(g2d->dev, "BLEND_RCQ: failed to build Active Scaler: %d\n", ret);
			goto cleanup;
		}
		
		/* Build Scaler Enable block */
		ret = g2d_rcq_build_scaler_enable(src_hw_fmt, &scal_en_regs, &scal_en_size);
		if (ret) {
			dev_err(g2d->dev, "BLEND_RCQ: failed to build Scaler Enable: %d\n", ret);
			goto cleanup;
		}
	} else {
		/* Dummy Scaler (Bypass) */
		ret = g2d_rcq_build_scaler_dummy(&scal_regs, &scal_size);
		if (ret) {
			dev_err(g2d->dev, "BLEND_RCQ: failed to build Dummy Scaler: %d\n", ret);
			goto cleanup;
		}
		scal_en_size = 0;
	}

	/* ========== INITIALIZE RCQ LAYOUT ========== */
	struct sunxi_g2d_rcq_frame_layout layout;
	memset(&layout, 0, sizeof(layout));

	/* Layout depends on scaling:
	 * - No Scaling: 7 blocks (V0, U0, U1, U2, SCAL(dummy), BLD, WB)
	 * - Scaling: 8 blocks (V0, U0, U1, U2, SCAL(active), SCAL_EN, BLD, WB)
	 */
	layout.block_count = needs_scaling ? 8 : 7;
	layout.header_len_bytes =
		layout.block_count * sizeof(struct g2d_rcq_header);

	layout.blocks[0].size = v0_size;
	layout.blocks[0].reg_offset = V0_ATTCTL;
	layout.blocks[0].dirty = 1;

	layout.blocks[1].size = u0_size;
	layout.blocks[1].reg_offset = UI0_ATTR;
	layout.blocks[1].dirty = 0;

	layout.blocks[2].size = u1_size;
	layout.blocks[2].reg_offset = UI1_ATTR;
	layout.blocks[2].dirty = 0;

	layout.blocks[3].size = ui2_size;
	layout.blocks[3].reg_offset = UI2_ATTR;
	layout.blocks[3].dirty = 1;

	layout.blocks[4].size = scal_size;
	layout.blocks[4].reg_offset = VS_CTRL;
	layout.blocks[4].dirty = 1;

	if (needs_scaling) {
		/* Block 5: SCAL_EN */
		layout.blocks[5].size = scal_en_size;
		layout.blocks[5].reg_offset = VS_CTRL;
		layout.blocks[5].dirty = 1;

		/* Block 6: BLD */
		layout.blocks[6].size = bld_size;
		layout.blocks[6].reg_offset = BLD_EN_CTL;
		layout.blocks[6].dirty = 1;

		/* Block 7: WB */
		layout.blocks[7].size = wb_size;
		layout.blocks[7].reg_offset = WB_ATT;
		layout.blocks[7].dirty = 1;
	} else {
		/* Block 5: BLD */
		layout.blocks[5].size = bld_size;
		layout.blocks[5].reg_offset = BLD_EN_CTL;
		layout.blocks[5].dirty = 1;

		/* Block 6: WB */
		layout.blocks[6].size = wb_size;
		layout.blocks[6].reg_offset = WB_ATT;
		layout.blocks[6].dirty = 1;
	}

	/* ========== PACK INTO RCQ BUFFER ========== */
	if (needs_scaling) {
		ret = sunxi_g2d_rcq_pack_frame_8blocks(rcq, &layout,
						       (u32 *)v0_regs, u0_regs,
						       u1_regs, (u32 *)ui2_regs,
						       scal_regs, scal_en_regs,
						       (u32 *)bld_regs,
						       (u32 *)wb_regs);
	} else {
		ret = sunxi_g2d_rcq_pack_frame_7blocks(rcq, &layout,
						       (u32 *)v0_regs, u0_regs,
						       u1_regs, (u32 *)ui2_regs,
						       scal_regs, (u32 *)bld_regs,
						       (u32 *)wb_regs);
	}
	
	if (ret) {
		dev_err(g2d->dev, "BLEND_RCQ: failed to pack RCQ frame: %d\n",
			ret);
		goto cleanup;
	}

	dev_dbg(g2d->dev,
		"BLEND_RCQ packed: headers=%u used=%u phy_addr=0x%llx\n",
		rcq->header_count, rcq->used, (u64)rcq->phy_addr);

	dev_dbg(g2d->dev, "BLEND_RCQ: prepared successfully\n");
	ret = 0;

cleanup:
	kfree(ui2_regs);
	kfree(u0_regs);
	kfree(u1_regs);
	kfree(scal_regs);
	kfree(scal_en_regs);
	kfree(v0_regs);
	kfree(bld_regs);
	kfree(wb_regs);

	return ret;
}

/**
 * g2d_csc_init - Initialize CSC state with default values
 * @state: Pointer to CSC state structure
 */
void g2d_csc_init(struct g2d_csc_state *state)
{
	if (!state)
		return;

	state->base_601 = Ycbcr2rgb_601;
	state->base_709 = Ycbcr2rgb_709;
	state->base_2020 = Ycbcr2rgb_2020;

	/* Initialize current tables with base values */
	/* Offset 36 (Limit->Full) is what we use for YUV->RGB */
	memcpy(state->current_601, Ycbcr2rgb_601, sizeof(state->current_601));
	memcpy(state->current_709, Ycbcr2rgb_709, sizeof(state->current_709));
	memcpy(state->current_2020, Ycbcr2rgb_2020,
	       sizeof(state->current_2020));

	/* Default adjustments */
	state->adj.brightness = 0;
	state->adj.contrast = 100;
	state->adj.saturation = 100;

	state->dirty_601 = false;
	state->dirty_709 = false;
	state->dirty_2020 = false;
}

/**
 * g2d_csc_update - Recalculate CSC tables based on adjustments
 * @state: Pointer to CSC state structure
 *
 * Recalculates the "Limit -> Full" (offset 36) block of the CSC matrices.
 */
void g2d_csc_update(struct g2d_csc_state *state)
{
	int r;
	s32 *curr;
	const s32 *base;
	int offset = 36; /* Limit -> Full block */

	if (!state)
		return;

	/* Update 601 table */
	curr = state->current_601 + offset;
	base = state->base_601 + offset;

	for (r = 0; r < 3; r++) {
		/* Col 0: Y (Contrast only) */
		curr[r * 4 + 0] = (base[r * 4 + 0] * state->adj.contrast) / 100;

		/* Col 1: U (Contrast * Saturation) */
		curr[r * 4 + 1] =
			(base[r * 4 + 1] * state->adj.contrast * state->adj.saturation) /
			10000;

		/* Col 2: V (Contrast * Saturation) */
		curr[r * 4 + 2] =
			(base[r * 4 + 2] * state->adj.contrast * state->adj.saturation) /
			10000;

		/* Col 3: Constant (Contrast + Brightness) */
		/* Brightness is added in RGB space (after matrix), so we add it to the constant term.
		 * Constant term is in Q10 fixed point (approx), so shift brightness by 10.
		 */
		curr[r * 4 + 3] = (base[r * 4 + 3] * state->adj.contrast) / 100 +
				  (state->adj.brightness << 10);
	}

	/* Update  709 table */
	curr = state->current_709 + offset;
	base = state->base_709 + offset;

	for (r = 0; r < 3; r++) {
		curr[r * 4 + 0] = (base[r * 4 + 0] * state->adj.contrast) / 100;
		curr[r * 4 + 1] =
			(base[r * 4 + 1] * state->adj.contrast * state->adj.saturation) /
			10000;
		curr[r * 4 + 2] =
			(base[r * 4 + 2] * state->adj.contrast * state->adj.saturation) /
			10000;
		curr[r * 4 + 3] = (base[r * 4 + 3] * state->adj.contrast) / 100 +
				  (state->adj.brightness << 10);
	}

	/* Update 2020 table */
	curr = state->current_2020 + offset;
	base = state->base_2020 + offset;

	for (r = 0; r < 3; r++) {
		curr[r * 4 + 0] = (base[r * 4 + 0] * state->adj.contrast) / 100;
		curr[r * 4 + 1] =
			(base[r * 4 + 1] * state->adj.contrast * state->adj.saturation) /
			10000;
		curr[r * 4 + 2] =
			(base[r * 4 + 2] * state->adj.contrast * state->adj.saturation) /
			10000;
		curr[r * 4 + 3] = (base[r * 4 + 3] * state->adj.contrast) / 100 +
				  (state->adj.brightness << 10);
	}
}

/*
 * Handler for specialized G2D_IOC_CMD ioctl
 * Dispatches to command-specific handlers based on cmd_type
 */
static long sunxi_g2d_ioctl_cmd(struct sunxi_g2d_ctx *ctx, unsigned long arg)
{
	struct sunxi_g2d_dev *g2d = ctx->g2d;
	struct g2d_cmd cmd;

	if (copy_from_user(&cmd, (void __user *)arg, sizeof(cmd)))
		return -EFAULT;

	/* Dispatch to command-specific handler */
	switch (cmd.cmd_type) {
	case G2D_CMD_COPY:
		return sunxi_g2d_cmd_copy(ctx, arg);
	case G2D_CMD_SCALE:
		return sunxi_g2d_cmd_scale(ctx, arg);
	case G2D_CMD_BLEND:
		return sunxi_g2d_cmd_blend(ctx, arg);
	case G2D_CMD_ROTATE:
		return sunxi_g2d_cmd_rotate(ctx, arg);
	case G2D_CMD_MASK:
		return sunxi_g2d_cmd_mask(ctx, arg);
	case G2D_CMD_FILLRECT:
		return sunxi_g2d_cmd_fillrect(ctx, arg);
	default:
		dev_err(g2d->dev, "Invalid cmd_type: %u\n", cmd.cmd_type);
		return -EINVAL;
	}
}

/*
 * Handler for G2D_IOC_ALLOC_BUFFER ioctl
 * Allocates a DMA buffer and returns a file descriptor to user space
 */
static long sunxi_g2d_ioctl_alloc_buffer(struct sunxi_g2d_dev *g2d,
					 unsigned long arg)
{
	struct g2d_alloc_buffer alloc;
	struct g2d_dma_buffer *buf;
	struct dma_buf *dmabuf;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	int fd, ret;

	if (copy_from_user(&alloc, (void __user *)arg, sizeof(alloc))) {
		dev_err(g2d->dev, "ALLOC_BUFFER: copy_from_user failed\n");
		return -EFAULT;
	}

	/* Validate size */
	if (alloc.size == 0 || alloc.size > 128 * 1024 * 1024) /* Max 128 MB */
		return -EINVAL;

	/* Allocate buffer metadata */
	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf) {
		dev_err(g2d->dev, "ALLOC_BUFFER: kzalloc failed\n");
		return -ENOMEM;
	}

	buf->dev = g2d->dev;
	/* Honor per-allocation flags to select backend */
	if (alloc.flags & (G2D_ALLOC_F_CONTIGUOUS | G2D_ALLOC_F_COHERENT)) {
		int mode = (alloc.flags & G2D_ALLOC_F_COHERENT) ? 2 : 0;
		if (mode == 0 && (alloc.flags & G2D_ALLOC_F_CONTIGUOUS))
			mode = 2; /* contiguous requirement maps to coherent backend here */
		ret = g2d_dma_mem_alloc_mode(buf->dev, alloc.size, &buf->mem,
					       GFP_KERNEL, mode);
	} else {
		ret = g2d_dma_mem_alloc_mode(buf->dev, alloc.size, &buf->mem,
					GFP_KERNEL, -1);
	}
	if (ret) {
		dev_err(g2d->dev,
			"ALLOC_BUFFER: dma_alloc_noncontiguous FAILED\n");
		kfree(buf);
		return ret;
	}

	/* Export as DMA-BUF */
	exp_info.ops = &g2d_dmabuf_ops;
	exp_info.size = alloc.size;
	/* export flags: O_RDWR to allow mmap with PROT_WRITE */
	exp_info.flags = O_RDWR;
	exp_info.priv = buf;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		dev_err(g2d->dev, "ALLOC_BUFFER: dma_buf_export failed: %ld\n", PTR_ERR(dmabuf));
		g2d_dma_mem_free(g2d->dev, &buf->mem);
		kfree(buf);
		return PTR_ERR(dmabuf);
	}

	/* Get file descriptor */
	fd = dma_buf_fd(dmabuf, O_CLOEXEC);
	if (fd < 0) {
		dev_err(g2d->dev, "ALLOC_BUFFER: dma_buf_fd failed: %d\n", fd);
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
		alloc.size, fd, (u64)buf->mem.dma_addr);

	return 0;
}

static long sunxi_g2d_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct sunxi_g2d_ctx *ctx = file->private_data;
	struct sunxi_g2d_dev *g2d = ctx->g2d;
	struct g2d_task_req task_req;

	switch (cmd) {
	case G2D_IOC_GET_VERSION:
		return sunxi_g2d_ioctl_get_version(g2d, arg);
	/* G2D_IOC_UNIFIED removed */
	case G2D_IOC_CMD:
		return sunxi_g2d_ioctl_cmd(ctx, arg);
	case G2D_IOC_ALLOC_BUFFER:
		return sunxi_g2d_ioctl_alloc_buffer(g2d, arg);
	case G2D_IOC_TASK:
		if (copy_from_user(&task_req, (void __user *)arg,
				   sizeof(task_req)))
			return -EFAULT;

		switch (task_req.task_cmd) {
		case G2D_TASK_CREATE: {
			struct g2d_task *task;

			task = kzalloc(sizeof(*task), GFP_KERNEL);
			if (!task)
				return -ENOMEM;

			mutex_init(&task->lock);
			INIT_LIST_HEAD(&task->steps);
			task->g2d = g2d;
			task->owner = ctx;

			mutex_lock(&g2d->task_lock);
			task->id = g2d->next_task_id++;
			list_add_tail(&task->node, &g2d->task_list);
			mutex_unlock(&g2d->task_lock);

			task_req.task_id = task->id;
			task_req.fence_fd_out = -1;
			if (copy_to_user((void __user *)arg, &task_req,
					 sizeof(task_req)))
				return -EFAULT;
			return 0;
		}
		case G2D_TASK_ADD: {
			struct g2d_task *task;
			int ret;

			mutex_lock(&g2d->task_lock);
			task = g2d_task_find(g2d, ctx, task_req.task_id);
			mutex_unlock(&g2d->task_lock);
			if (!task)
				return -ENOENT;

			ret = g2d_task_add_step(ctx, task, &task_req.step);
			return ret;
		}
		case G2D_TASK_RUN: {
			struct g2d_task *task;
			int ret;

			mutex_lock(&g2d->task_lock);
			task = g2d_task_find(g2d, ctx, task_req.task_id);
			mutex_unlock(&g2d->task_lock);
			if (!task)
				return -ENOENT;

			task_req.fence_fd_out = -1;
			ret = g2d_task_run(ctx, task, &task_req);
			if (!ret) {
				if (copy_to_user((void __user *)arg, &task_req,
						 sizeof(task_req)))
					return -EFAULT;
			}
			return ret;
		}
		case G2D_TASK_DEL: {
			struct g2d_task *task;

			mutex_lock(&g2d->task_lock);
			task = g2d_task_find(g2d, ctx, task_req.task_id);
			if (task) {
				list_del(&task->node);
			}
			mutex_unlock(&g2d->task_lock);

			if (!task)
				return -ENOENT;
			g2d_task_destroy(g2d, task);
			return 0;
		}
		default:
			return -EINVAL;
		}
	case G2D_IOC_SET_CSC_ADJUST: {
		struct g2d_csc_adjust adj;
		if (copy_from_user(&adj, (void __user *)arg, sizeof(adj)))
			return -EFAULT;
		
		dev_dbg(g2d->dev, "IOC_SET_CSC_ADJUST: B=%d C=%d S=%d\n",
			adj.brightness, adj.contrast, adj.saturation);

		mutex_lock(&g2d->dev_mutex);
		/* Update GLOBAL device state to support external control tools */
		g2d->csc_state.adj = adj;
		g2d_csc_update(&g2d->csc_state);
		
		/* Also update context state for completeness */
		ctx->csc_state.adj = adj;
		g2d_csc_update(&ctx->csc_state);
		ctx->csc_changed = true;
		mutex_unlock(&g2d->dev_mutex);
		return 0;
	}
	case G2D_IOC_GET_CSC_ADJUST: {
		struct g2d_csc_adjust adj;
		mutex_lock(&g2d->dev_mutex);
		adj = ctx->csc_changed ? ctx->csc_state.adj : g2d->csc_state.adj;
		mutex_unlock(&g2d->dev_mutex);
		
		if (copy_to_user((void __user *)arg, &adj, sizeof(adj)))
			return -EFAULT;
		return 0;
	}
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

		dev_dbg(g2d->dev,
			"selftest: installing fd=%d sync_file=%p file=%p fence=%p pid=%d\n",
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
		queue_delayed_work(g2d->job_wq, &w->dwork,
				   msecs_to_jiffies(timeout_ms));

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

		if (copy_from_user(&user_fd, (void __user *)arg,
				   sizeof(user_fd)))
			return -EFAULT;

		if (user_fd < 0)
			return -EINVAL;

		dev_dbg(g2d->dev, "sync ioctl: importing fence fd=%d pid=%d\n",
			user_fd, task_tgid_nr(current));
		in_fence = sync_file_get_fence(user_fd);
		if (!in_fence)
			return -EINVAL;
		dev_dbg(g2d->dev, "sync ioctl: got in_fence=%p signaled=%d\n",
			in_fence, dma_fence_is_signaled(in_fence));

		/* Wait uninterruptibly for the fence to signal */
		dma_fence_wait(in_fence, false);
		dev_dbg(g2d->dev,
			"sync ioctl: in_fence=%p wait done signaled=%d\n",
			in_fence, dma_fence_is_signaled(in_fence));
		dma_fence_put(in_fence);
		return 0;
	}
	case G2D_IOC_WRITE_BUFFER: {
		struct g2d_buffer_rw rw;
		struct dma_buf *dmabuf;
		struct dma_buf_attachment *attach;
		struct sg_table *sgt;
		void *bounce;
		void __user *up;
		size_t chunk;
		int ret;

		dev_dbg(g2d->dev, "=== G2D_IOC_WRITE_BUFFER entered ===\n");

		if (copy_from_user(&rw, (void __user *)arg, sizeof(rw))) {
			dev_dbg(g2d->dev,
				"WRITE_BUFFER: copy_from_user failed\n");
			return -EFAULT;
		}

		dev_dbg(g2d->dev, "WRITE_BUFFER: fd=%d size=%llu offset=%llu\n",
			rw.dma_fd, rw.size, rw.offset);

		/* Validate parameters */
		if (rw.dma_fd < 0 || rw.size == 0 || !rw.user_ptr)
			return -EINVAL;

		/* Validate userspace pointer and size early to avoid kernel faults */
		up = u64_to_user_ptr(rw.user_ptr);
		if (!access_ok(up, (unsigned long)rw.size))
			return -EFAULT;

		dmabuf = dma_buf_get(rw.dma_fd);
		if (IS_ERR(dmabuf))
			return PTR_ERR(dmabuf);

		dev_dbg(g2d->dev,
			"READ_BUFFER: user_ptr=%p size=%llu offset=%llu (validated)\n",
			up, (unsigned long long)rw.size,
			(unsigned long long)rw.offset);

		/* Check bounds */
		if (rw.offset + rw.size > dmabuf->size) {
			dma_buf_put(dmabuf);
			return -EINVAL;
		}

		dev_dbg(g2d->dev, "READ_BUFFER: attaching dmabuf\n");
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

		chunk = min_t(size_t, rw.size, SZ_64K);
		bounce = kmalloc(chunk, GFP_KERNEL);
		if (!bounce) {
			ret = -ENOMEM;
			goto end_cpu_access;
		}

		ret = 0;
		{
			size_t remaining = rw.size;
			size_t user_off = 0;
			size_t offset = rw.offset;

			while (remaining) {
				size_t cur = min(remaining, chunk);
				size_t copied;

				if (copy_from_user(
					    bounce,
					    (void __user
						     *)(uintptr_t)(rw.user_ptr +
								   user_off),
					    cur)) {
					ret = -EFAULT;
					break;
				}

				copied = sg_pcopy_from_buffer(sgt->sgl,
							      sgt->nents,
							      bounce, cur,
							      offset);
				if (copied != cur) {
					ret = -EIO;
					break;
				}

				remaining -= cur;
				user_off += cur;
				offset += cur;
			}
		}

		kfree(bounce);

end_cpu_access:
		dma_buf_end_cpu_access(dmabuf, DMA_TO_DEVICE);
		dma_buf_unmap_attachment(attach, sgt, DMA_TO_DEVICE);
		dma_buf_detach(dmabuf, attach);
		dma_buf_put(dmabuf);

		dev_dbg(g2d->dev,
			"WRITE_BUFFER: fd=%d offset=%llu size=%llu ret=%d\n",
			rw.dma_fd, rw.offset, rw.size, ret);

		return ret;
	}
	case G2D_IOC_READ_BUFFER: {
		struct g2d_buffer_rw rw;
		struct dma_buf *dmabuf;
		struct iosys_map map;
		void *vaddr;
		void *bounce;
		size_t chunk;
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

		/* CRITICAL: Do NOT call dma_buf_map_attachment() here!
		 * The buffer might still have active IOMMU mappings from previous
		 * operations. Creating a new attachment would generate a second IOVA
		 * that corrupts page tables (especially with iommu.strict=1).
		 * 
		 * Instead, sync CPU cache and use vmap for kernel-space read access.
		 */
		ret = dma_buf_begin_cpu_access(dmabuf, DMA_FROM_DEVICE);
		if (ret) {
			dma_buf_put(dmabuf);
			return ret;
		}

		/* vmap to get kernel virtual address (no new IOVA created) */
		ret = dma_buf_vmap(dmabuf, &map);
		if (ret) {
			dma_buf_end_cpu_access(dmabuf, DMA_FROM_DEVICE);
			dma_buf_put(dmabuf);
			return ret;
		}

		vaddr = map.vaddr;
		if (!vaddr) {
			ret = -ENOMEM;
			goto read_vunmap;
		}

		/* Allocate bounce buffer for chunked copy_to_user */
		chunk = min_t(size_t, rw.size, SZ_64K);
		bounce = kmalloc(chunk, GFP_KERNEL);
		if (!bounce) {
			ret = -ENOMEM;
			goto read_vunmap;
		}

		ret = 0;
		{
			size_t remaining = rw.size;
			size_t user_off = 0;
			size_t offset = rw.offset;

			while (remaining) {
				size_t cur = min(remaining, chunk);

				/* Direct memcpy from vmap'd kernel address */
				memcpy(bounce, (u8 *)vaddr + offset, cur);

				if (copy_to_user(
					    (void __user
						     *)(uintptr_t)(rw.user_ptr +
								   user_off),
					    bounce, cur)) {
					ret = -EFAULT;
					break;
				}

				remaining -= cur;
				user_off += cur;
				offset += cur;
			}
		}

		kfree(bounce);

read_vunmap:
		dma_buf_vunmap(dmabuf, &map);
		dma_buf_end_cpu_access(dmabuf, DMA_FROM_DEVICE);
		dma_buf_put(dmabuf);

		dev_dbg(g2d->dev,
			"READ_BUFFER: fd=%d offset=%llu size=%llu ret=%d\n",
			rw.dma_fd, rw.offset, rw.size, ret);

		return ret;
	}
	default:
		dev_err(g2d->dev, "Unknown ioctl cmd: 0x%08x\n", cmd);
		return -ENOTTY;
	}
}

static const struct file_operations sunxi_g2d_fops = {
	.owner = THIS_MODULE,
	.open = sunxi_g2d_open,
	.release = sunxi_g2d_release,
	.unlocked_ioctl = sunxi_g2d_ioctl,
	.compat_ioctl = sunxi_g2d_ioctl,
};

/* ===== sysfs: staging telemetry (per platform device) ===== */
static ssize_t staging_src_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct sunxi_g2d_dev *g2d = dev_get_drvdata(dev);
	return sysfs_emit(
		buf, "%llu\n",
		(unsigned long long)atomic64_read(&g2d->stage_src_count));
}

static ssize_t staging_wb_out_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct sunxi_g2d_dev *g2d = dev_get_drvdata(dev);
	return sysfs_emit(
		buf, "%llu\n",
		(unsigned long long)atomic64_read(&g2d->stage_wb_out_count));
}

static ssize_t staging_wb_inplace_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct sunxi_g2d_dev *g2d = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%llu\n",
			  (unsigned long long)atomic64_read(
				  &g2d->stage_wb_inplace_count));
}

/* staging_wb_bytes is kept as an internal counter (not exposed via sysfs) */

static ssize_t staging_wb_active_jobs_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct sunxi_g2d_dev *g2d = dev_get_drvdata(dev);
	return sysfs_emit(
		buf, "%llu\n",
		(unsigned long long)atomic64_read(&g2d->stage_wb_active_jobs));
}

static ssize_t staging_wb_active_bytes_show(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct sunxi_g2d_dev *g2d = dev_get_drvdata(dev);
	return sysfs_emit(
		buf, "%llu\n",
		(unsigned long long)atomic64_read(&g2d->stage_wb_active_bytes));
}

static DEVICE_ATTR_RO(staging_src);
static DEVICE_ATTR_RO(staging_wb_out);
static DEVICE_ATTR_RO(staging_wb_inplace);
/* staging_wb_bytes intentionally not exposed via sysfs (kept internal) */
static DEVICE_ATTR_RO(staging_wb_active_jobs);
static DEVICE_ATTR_RO(staging_wb_active_bytes);
/* New sysfs attributes for extended telemetry */
static ssize_t vmap_attempts_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct sunxi_g2d_dev *g2d = dev_get_drvdata(dev);
	return sysfs_emit(
		buf, "%llu\n",
		(unsigned long long)atomic64_read(&g2d->vmap_attempts));
}
static ssize_t vmap_failures_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct sunxi_g2d_dev *g2d = dev_get_drvdata(dev);
	return sysfs_emit(
		buf, "%llu\n",
		(unsigned long long)atomic64_read(&g2d->vmap_failures));
}
/* staging_wb_req_bytes is retained as an internal counter but not exposed */
static DEVICE_ATTR_RO(vmap_attempts);
static DEVICE_ATTR_RO(vmap_failures);
/* staging_wb_req_bytes: not exposed */

/* Job-pool telemetry (read-only): hits/misses and pool sizing */
static ssize_t job_pool_hits_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct sunxi_g2d_dev *g2d = dev_get_drvdata(dev);
	return sysfs_emit(
		buf, "%llu\n",
		(unsigned long long)atomic64_read(&g2d->job_pool_hits));
}

static ssize_t job_pool_misses_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct sunxi_g2d_dev *g2d = dev_get_drvdata(dev);
	return sysfs_emit(
		buf, "%llu\n",
		(unsigned long long)atomic64_read(&g2d->job_pool_misses));
}

static ssize_t job_pool_free_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct sunxi_g2d_dev *g2d = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", g2d->job_pool_free_count);
}

static ssize_t job_pool_min_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct sunxi_g2d_dev *g2d = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", g2d->job_pool_min);
}

static ssize_t job_pool_max_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct sunxi_g2d_dev *g2d = dev_get_drvdata(dev);
	return sysfs_emit(buf, "%d\n", g2d->job_pool_max);
}

static DEVICE_ATTR_RO(job_pool_hits);
static DEVICE_ATTR_RO(job_pool_misses);
static DEVICE_ATTR_RO(job_pool_free);
static DEVICE_ATTR_RO(job_pool_min);
static DEVICE_ATTR_RO(job_pool_max);

/* ========== Platform driver ========== */

static int sunxi_g2d_probe(struct platform_device *pdev)
{
	struct sunxi_g2d_dev *g2d;
	struct resource *res;
	int ret;

	dev_dbg(&pdev->dev, "Probing G2D driver v%s\n", DRIVER_VERSION);

	g2d = devm_kzalloc(&pdev->dev, sizeof(*g2d), GFP_KERNEL);
	if (!g2d)
		return -ENOMEM;

	g2d->dev = &pdev->dev;
	platform_set_drvdata(pdev, g2d);

	/* Initialize fence context and lock */
	spin_lock_init(&g2d->fence_lock);
	atomic64_set(&g2d->fence_seqno, 0);
	g2d->fence_context = dma_fence_context_alloc(1);

	/* Initialize CSC state */
	g2d_csc_init(&g2d->csc_state);

	/* Get MMIO resources */
	dev_dbg(&pdev->dev, "Getting MMIO resources\n");
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	g2d->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(g2d->base))
		return PTR_ERR(g2d->base);

	dev_dbg(&pdev->dev, "G2D MMIO base=0x%08llx size=0x%llx\n",
		(u64)res->start, (u64)resource_size(res));

	/* Get CCU (clock control unit) base - use hardcoded address as in fillrect v1.0.0 */
	g2d->ccu_base = devm_ioremap(&pdev->dev, 0x02001000, 0x1000);
	if (!g2d->ccu_base) {
		dev_err(&pdev->dev, "Failed to map CCU registers\n");
		return -ENOMEM;
	}

	dev_dbg(&pdev->dev, "CCU base=0x02001000 (hardcoded)\n");

	/* BSP sequence: reset FIRST, then clocks (critical from fillrect v1.0.0) */
	g2d->rst = devm_reset_control_get_optional_exclusive(&pdev->dev, NULL);
	if (!IS_ERR(g2d->rst)) {
		ret = reset_control_deassert(g2d->rst);
		if (ret) {
			dev_err(&pdev->dev, "Failed to deassert reset: %d\n",
				ret);
			return ret;
		}
		dev_dbg(&pdev->dev, "Reset deasserted\n");
	} else {
		dev_warn(&pdev->dev, "No reset control available\n");
	}

	/*
	 * IOMMU Configuration for G2D:
	 * G2D REQUIRES IOMMU to handle multiple buffers with limited CMA (128MB).
	 * Unlike VE/DE (fixed buffers), G2D uses dynamic allocation causing
	 * severe CMA fragmentation without IOMMU.
	 * 
	 * CRITICAL: dma_set_mask_and_coherent() MUST be called FIRST to trigger
	 * automatic IOMMU attachment from DT "iommus" property. If called after
	 * of_dma_configure(), the binding fails.
	 * 
	 * BSP driver also uses IOMMU for T113-S3 G2D.
	 */

	struct device *dev = &pdev->dev;

	/* Step 1: Set DMA mask FIRST (triggers IOMMU attach from DT) */
	dev_dbg(dev, "Setting DMA mask to 32 bits (triggers IOMMU attach)\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "Failed to set DMA mask: %d\n", ret);
		return ret;
	}

	/* Step 2: Verify IOMMU domain attachment */
	{
		struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
		struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);

		if (fwspec) {
			dev_dbg(dev, "✓ IOMMU fwspec: PRESENT (num_ids=%u)\n",
				 fwspec->num_ids);
		} else {
			dev_warn(
				dev,
				"⚠ IOMMU fwspec: NULL (IOMMU binding failed)\n");
		}

		if (domain) {
			dev_info(
				dev,
				"✓ IOMMU domain attached: type=%d (IOVA translation enabled)\n",
				domain->type);
			dev_info(
				dev,
				"✓ Using IOMMU for dynamic buffer allocation (avoids CMA fragmentation)\n");
		} else {
			dev_err(
				dev,
				"❌ IOMMU domain: NULL (Critical for Mode 1)\n");
		}
	}

	/* Get clocks */
	dev_dbg(&pdev->dev, "Getting clocks\n");
	g2d->clk_bus = devm_clk_get(&pdev->dev, "bus_g2d");
	if (IS_ERR(g2d->clk_bus)) {
		dev_err(&pdev->dev, "Failed to get bus_g2d clock\n");
		return PTR_ERR(g2d->clk_bus);
	}

	dev_dbg(&pdev->dev, "Getting g2d clock\n");
	g2d->clk_mod = devm_clk_get(&pdev->dev, "g2d");
	if (IS_ERR(g2d->clk_mod)) {
		dev_err(&pdev->dev, "Failed to get g2d clock\n");
		return PTR_ERR(g2d->clk_mod);
	}

	dev_dbg(&pdev->dev, "Getting mbus clock\n");
	g2d->clk_mbus = devm_clk_get(&pdev->dev, "mbus_g2d");
	if (IS_ERR(g2d->clk_mbus)) {
		dev_err(&pdev->dev, "Failed to get mbus clock\n");
		return PTR_ERR(g2d->clk_mbus);
	}

	/*
	 * MBUS (Memory Bus) mapping y prioridad
	 * ------------------------------------
	 * El G2D en T113/D1 es el master #9 del MBUS. Para evitar
	 * caídas de rendimiento bajo carga (latencias en lecturas/escrituras
	 * que pueden manifestarse como flicker o reducción de FPS), el driver:
	 *   1. Mapea el bloque de registros MBUS para poder tocar CFG0(9).
	 *   2. Fuerza PRI=1 y QOS=3 (máxima prioridad actualmente soportada).
	 *   3. Habilita el bit ACEN correspondiente al master 9 si aún no está.
	 * Este ajuste es un workaround temporal hasta que exista un driver
	 * interconnect genérico para sunxi que gestione QoS/bandwidth.
	 *
	 * Si el mapeo falla se retorna error y el G2D no se habilita, ya que
	 * operar sin prioridad garantizada puede provocar inestabilidad visual
	 * en escenarios de alta competencia de memoria.
	 *
	 * Ver sección "MBUS y Prioridad" en `drivers/gpu/sunxi-g2d/README.md`.
	 */

	g2d->mbus_base = sunxi_g2d_map_mbus(&pdev->dev);
	if (IS_ERR(g2d->mbus_base)) {
		dev_err(&pdev->dev, "Failed to map MBUS registers: %ld\n",
			PTR_ERR(g2d->mbus_base));
		return PTR_ERR(g2d->mbus_base);
	}

	/* Get interconnect path for MBUS */
	dev_dbg(&pdev->dev, "Getting interconnect path for MBUS\n");
	g2d->icc_path = devm_of_icc_get(&pdev->dev, "dma-mem");
	if (IS_ERR(g2d->icc_path)) {
		ret = PTR_ERR(g2d->icc_path);
		if (ret != -ENODEV) {
			dev_err(&pdev->dev, "Failed to get interconnect: %d\n",
				ret);
			return ret;
		}
		g2d->icc_path = NULL;
	}

	/* Get IRQ */
	dev_dbg(&pdev->dev, "Getting IRQ\n");
	g2d->irq = platform_get_irq(pdev, 0);
	if (g2d->irq < 0)
		return g2d->irq;

	/* CRITICAL: Clean up any pending interrupts from bootloader or previous runs
	 * BEFORE requesting the IRQ. If the IRQ line is asserted, requesting it
	 * will immediately trigger the ISR. If clocks are off, the ISR will read 0
	 * and return IRQ_NONE, leading to "nobody cared" and IRQ disablement.
	 */
	{
		/* Enable clocks temporarily to access registers */
		clk_prepare_enable(g2d->clk_bus);
		clk_prepare_enable(g2d->clk_mod);
		clk_prepare_enable(g2d->clk_mbus);

		/* Disable and clear RCQ interrupts */
		writel(0, g2d->base + G2D_RCQ_IRQ_CTL);
		writel(G2D_RCQ_STATUS_TASK_END | G2D_RCQ_STATUS_CFG_FINISH,
		       g2d->base + G2D_RCQ_STATUS);

		/* Disable and clear MIXER interrupts */
		/* Writing 1 to status bit clears it. Writing 0 to enable bit disables it.
		 * G2D_MIXER_INT has status in bit 0, enable in bit 4.
		 * We write status bit 1, enable bit 0.
		 */
		writel(G2D_MIXER_INT_IRQ_PENDING, g2d->base + G2D_MIXER_INT);

		/* Disable and clear ROT interrupts */
		/* ROT_INT has status in bit 0, enable in bit 16. */
		writel(ROT_INT_FINISH, g2d->base + ROT_INT);

		/* Ensure writes complete */
		wmb();

		/* Disable clocks again */
		clk_disable_unprepare(g2d->clk_mbus);
		clk_disable_unprepare(g2d->clk_mod);
		clk_disable_unprepare(g2d->clk_bus);
	}

	ret = devm_request_irq(&pdev->dev, g2d->irq, sunxi_g2d_irq, 0,
			       dev_name(&pdev->dev), g2d);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request IRQ %d: %d\n", g2d->irq,
			ret);
		return ret;
	}

	dev_dbg(&pdev->dev, "IRQ %d registered\n", g2d->irq);

	/* Initialize synchronization primitives */
	mutex_init(&g2d->dev_mutex);
	spin_lock_init(&g2d->job_lock);
	INIT_LIST_HEAD(&g2d->job_queue);
	/* Init job pool */
	spin_lock_init(&g2d->job_pool_lock);
	INIT_LIST_HEAD(&g2d->job_pool_free);
	g2d->job_pool_free_count = 0;
	g2d->job_pool_min = 32; /* default min */
	g2d->job_pool_max = 256; /* default max */
	g2d->job_cache = kmem_cache_create("sunxi_g2d_job",
					   sizeof(struct sunxi_g2d_job),
					   __alignof__(struct sunxi_g2d_job),
					   SLAB_HWCACHE_ALIGN, NULL);
	atomic64_set(&g2d->job_pool_hits, 0);
	atomic64_set(&g2d->job_pool_misses, 0);
	if (g2d->job_cache) {
		int i;
		for (i = 0; i < g2d->job_pool_min; i++) {
			struct sunxi_g2d_job *job =
				kmem_cache_zalloc(g2d->job_cache, GFP_KERNEL);
			if (!job)
				break;
			list_add(&job->node, &g2d->job_pool_free);
			g2d->job_pool_free_count++;
		}
	}
	init_waitqueue_head(&g2d->irq_wait);
	atomic_set(&g2d->irq_done, 0);
	atomic_set(&g2d->users, 0);
	atomic64_set(&g2d->jobs_submitted, 0);
	atomic64_set(&g2d->jobs_done, 0);
	atomic64_set(&g2d->jobs_failed, 0);
	atomic64_set(&g2d->stage_src_count, 0);
	atomic64_set(&g2d->stage_wb_out_count, 0);
	atomic64_set(&g2d->stage_wb_inplace_count, 0);
	atomic64_set(&g2d->stage_wb_bytes_copied, 0);
	atomic64_set(&g2d->stage_wb_active_jobs, 0);
	atomic64_set(&g2d->stage_wb_active_bytes, 0);
	atomic64_set(&g2d->vmap_attempts, 0);
	atomic64_set(&g2d->vmap_failures, 0);
	atomic64_set(&g2d->stage_wb_req_bytes, 0);

	/* Initialize persistent task tracking */
	INIT_LIST_HEAD(&g2d->task_list);
	mutex_init(&g2d->task_lock);
	g2d->next_task_id = 1;

	/* Initialize fence context/sequence for dma-fence support */
	g2d->fence_context = dma_fence_context_alloc(1);
	atomic64_set(&g2d->fence_seqno, 0);

	/* RCQ (Register Configuration Queue)
	 *
	 * Antes se deshabilitaba siempre en T113-S3 porque no teníamos
	 * una secuencia funcional. Para poder investigar con el nuevo
	 * helper y el test de fillrect simple, volvemos a habilitar el
	 * buffer RCQ, pero seguimos usando modo legacy por defecto.
	 */
	ret = sunxi_g2d_rcq_alloc(g2d->dev, &g2d->rcq, G2D_RCQ_MAX_SIZE);
	if (ret) {
		dev_warn(&pdev->dev,
			 "Failed to allocate RCQ buffer: %d, RCQ disabled\n",
			 ret);
		g2d->rcq_enabled = false;
	} else {
		g2d->rcq_enabled = true;
		dev_dbg(&pdev->dev,
			"RCQ enabled with %u byte buffer (experimental)\n",
			g2d->rcq.size);
	}

	/* Create workqueue for job processing */
	g2d->job_wq = alloc_workqueue("sunxi-g2d", WQ_HIGHPRI | WQ_UNBOUND, 1);
	if (!g2d->job_wq) {
		dev_err(&pdev->dev, "Failed to create workqueue\n");
		return -ENOMEM;
	}

	/* Initialize job worker */
	INIT_WORK(&g2d->job_work, sunxi_g2d_job_worker);
	dev_dbg(&pdev->dev, "Job worker initialized for async operations\n");

	/* initialize delayed work for safe HW disable */
	INIT_DELAYED_WORK(&g2d->disable_work, sunxi_g2d_disable_workfn);

	/* Register character device */
	ret = alloc_chrdev_region(&g2d->dev_num, 0, 1, DRIVER_NAME);
	if (ret) {
		dev_err(&pdev->dev, "Failed to allocate device number: %d\n",
			ret);
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

	dev_dbg(&pdev->dev, "Character device /dev/g2d created\n");
	dev_dbg(&pdev->dev, "G2D driver v%s loaded successfully\n",
		DRIVER_VERSION);
	dev_info(&pdev->dev, "registered as /dev/g2d\n");

	/* Create sysfs telemetry attributes on the platform device */
	device_create_file(&pdev->dev, &dev_attr_staging_src);
	device_create_file(&pdev->dev, &dev_attr_staging_wb_out);
	device_create_file(&pdev->dev, &dev_attr_staging_wb_inplace);
	device_create_file(&pdev->dev, &dev_attr_staging_wb_active_jobs);
	device_create_file(&pdev->dev, &dev_attr_staging_wb_active_bytes);
	device_create_file(&pdev->dev, &dev_attr_vmap_attempts);
	device_create_file(&pdev->dev, &dev_attr_vmap_failures);
	/* NOTE: staging_wb_bytes and staging_wb_req_bytes are kept as
	 * internal counters for occasional in-kernel diagnostics but are
	 * noisy in normal operation. Do not expose them by default via
	 * sysfs to reduce surface area; they can be converted to debug
	 * prints if needed in future. */
	/* Job-pool telemetry */
	device_create_file(&pdev->dev, &dev_attr_job_pool_hits);
	device_create_file(&pdev->dev, &dev_attr_job_pool_misses);
	device_create_file(&pdev->dev, &dev_attr_job_pool_free);
	device_create_file(&pdev->dev, &dev_attr_job_pool_min);
	device_create_file(&pdev->dev, &dev_attr_job_pool_max);

	dev_dbg(&pdev->dev,
		 "job-pool: free=%d min=%d max=%d hits=%llu misses=%llu\n",
		 g2d->job_pool_free_count, g2d->job_pool_min, g2d->job_pool_max,
		 (unsigned long long)atomic64_read(&g2d->job_pool_hits),
		 (unsigned long long)atomic64_read(&g2d->job_pool_misses));

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

	dev_dbg(&pdev->dev, "Removing G2D driver\n");

	/* Remove device node */
	device_destroy(g2d->dev_class, g2d->dev_num);
	class_destroy(g2d->dev_class);
	cdev_del(&g2d->cdev);
	unregister_chrdev_region(g2d->dev_num, 1);

	/* Flush and destroy workqueue */

	/* Cancel pending disable work to avoid running after device removal */
	cancel_delayed_work_sync(&g2d->disable_work);

	flush_workqueue(g2d->job_wq);
	destroy_workqueue(g2d->job_wq);

	/* Free RCQ buffer */
	if (g2d->rcq_enabled)
		sunxi_g2d_rcq_free(g2d->dev, &g2d->rcq);

	/* Warn if any writeback-staged jobs still active at remove time */
	if (atomic64_read(&g2d->stage_wb_active_jobs) != 0)
		dev_warn(&pdev->dev,
			 "Removing with %llu active writeback-staged jobs\n",
			 (unsigned long long)atomic64_read(
				 &g2d->stage_wb_active_jobs));

	/* Disable hardware if still enabled */
	mutex_lock(&g2d->dev_mutex);
	if (g2d->hw_enabled)
		sunxi_g2d_hw_disable(g2d);
	mutex_unlock(&g2d->dev_mutex);

	/* Print brief staging telemetry on removal */
	dev_dbg(
		&pdev->dev,
		"staging: src=%llu wb_out=%llu wb_inplace=%llu wb_bytes=%llu wb_active_jobs=%llu wb_active_bytes=%llu vmap_attempts=%llu vmap_failures=%llu wb_req_bytes=%llu\n",
		(unsigned long long)atomic64_read(&g2d->stage_src_count),
		(unsigned long long)atomic64_read(&g2d->stage_wb_out_count),
		(unsigned long long)atomic64_read(&g2d->stage_wb_inplace_count),
		(unsigned long long)atomic64_read(&g2d->stage_wb_bytes_copied),
		(unsigned long long)atomic64_read(&g2d->stage_wb_active_jobs),
		(unsigned long long)atomic64_read(&g2d->stage_wb_active_bytes),
		(unsigned long long)atomic64_read(&g2d->vmap_attempts),
		(unsigned long long)atomic64_read(&g2d->vmap_failures),
		(unsigned long long)atomic64_read(&g2d->stage_wb_req_bytes));

	dev_info(&pdev->dev, "unregistered /dev/g2d\n");

	/* Destroy job pool */
	if (g2d->job_cache) {
		unsigned long flags;
		spin_lock_irqsave(&g2d->job_pool_lock, flags);
		while (!list_empty(&g2d->job_pool_free)) {
			struct sunxi_g2d_job *job =
				list_first_entry(&g2d->job_pool_free,
						 struct sunxi_g2d_job, node);
			list_del(&job->node);
			kmem_cache_free(g2d->job_cache, job);
		}
		g2d->job_pool_free_count = 0;
		spin_unlock_irqrestore(&g2d->job_pool_lock, flags);
		kmem_cache_destroy(g2d->job_cache);
		g2d->job_cache = NULL;
	}

	/* Destroy any remaining tasks */
	mutex_lock(&g2d->task_lock);
	while (!list_empty(&g2d->task_list)) {
		struct g2d_task *task =
			list_first_entry(&g2d->task_list, struct g2d_task,
					 node);
		list_del(&task->node);
		g2d_task_destroy(g2d, task);
	}
	mutex_unlock(&g2d->task_lock);

	/* Remove sysfs telemetry attributes */
	device_remove_file(&pdev->dev, &dev_attr_staging_src);
	device_remove_file(&pdev->dev, &dev_attr_staging_wb_out);
	device_remove_file(&pdev->dev, &dev_attr_staging_wb_inplace);
	device_remove_file(&pdev->dev, &dev_attr_staging_wb_active_jobs);
	device_remove_file(&pdev->dev, &dev_attr_staging_wb_active_bytes);
	device_remove_file(&pdev->dev, &dev_attr_vmap_attempts);
	device_remove_file(&pdev->dev, &dev_attr_vmap_failures);
	/* Remove any debug-only attributes were we to add them. The
	 * following two counters (staging_wb_bytes / staging_wb_req_bytes)
	 * remain in-memory for crash-time inspection or optional debug
	 * exposure, but are not created as sysfs files by default. */
	/* Job-pool telemetry */
	device_remove_file(&pdev->dev, &dev_attr_job_pool_hits);
	device_remove_file(&pdev->dev, &dev_attr_job_pool_misses);
	device_remove_file(&pdev->dev, &dev_attr_job_pool_free);
	device_remove_file(&pdev->dev, &dev_attr_job_pool_min);
	device_remove_file(&pdev->dev, &dev_attr_job_pool_max);
}

static const struct of_device_id sunxi_g2d_of_match[] = {
	{ .compatible = "allwinner,sun20i-d1-g2d" },
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
