// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * sunxi-g2d-rcq.c - Register Command Queue (RCQ) implementation
 * 
 * Copyright (C) 2025 Sergio Perez
 * 
 * RCQ allows the G2D hardware to execute a sequence of register writes
 * via DMA, eliminating CPU overhead and enabling atomic multi-operation
 * tasks.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include "sunxi-g2d-rcq.h"
#include "sunxi-g2d-regs.h"
#include "sunxi-g2d-structs.h"

/* G2D_RCQ_CTRL bits */
#define RCQ_CTRL_UPDATE           (1 << 0)  /* Trigger RCQ execution */
#define RCQ_CTRL_EN               (1 << 4)  /* Enable RCQ (T113 v2 RCQ) */

/* G2D_RCQ_IRQ_CTL bits (from union g2d_rcq_irq_ctl) */
#define RCQ_IRQ_SEL               (1 << 0)  /* RCQ select */
#define RCQ_IRQ_TASK_END_EN       (1 << 4)  /* Enable task end IRQ */
#define RCQ_IRQ_CFG_FINISH_EN     (1 << 6)  /* Enable config finish IRQ */

/**
 * sunxi_g2d_rcq_alloc - Allocate RCQ command buffer
 * @dev: Device for DMA allocation
 * @rcq: RCQ memory structure to initialize
 * @size: Requested size in bytes (will be 32-byte aligned)
 * 
 * Returns: 0 on success, negative error code on failure
 */
int sunxi_g2d_rcq_alloc(struct device *dev, struct g2d_rcq_mem *rcq, u32 size)
{
	u32 aligned_size;
	
	if (!dev || !rcq || size == 0)
		return -EINVAL;
	
	/* Align size to 32 bytes as required by RCQ hardware */
	aligned_size = G2D_RCQ_BYTE_ALIGN(size);
	
	/* Allocate DMA coherent memory (CPU can write, HW can read) */
	rcq->vir_addr = dma_alloc_coherent(dev, aligned_size, 
	                                   &rcq->phy_addr, GFP_KERNEL);
	if (!rcq->vir_addr) {
		dev_err(dev, "Failed to allocate %u bytes for RCQ buffer\n", 
		        aligned_size);
		return -ENOMEM;
	}
	
	rcq->size = aligned_size;
	rcq->used = 0;
	rcq->header_count = 0;
	
	dev_info(dev, "RCQ buffer allocated: virt=%p phys=0x%pad size=%u\n",
	         rcq->vir_addr, &rcq->phy_addr, aligned_size);
	
	return 0;
}

/**
 * sunxi_g2d_rcq_free - Free RCQ command buffer
 * @dev: Device for DMA deallocation
 * @rcq: RCQ memory structure to free
 */
void sunxi_g2d_rcq_free(struct device *dev, struct g2d_rcq_mem *rcq)
{
	if (!dev || !rcq || !rcq->vir_addr)
		return;
	
	dma_free_coherent(dev, rcq->size, rcq->vir_addr, rcq->phy_addr);
	
	rcq->vir_addr = NULL;
	rcq->phy_addr = 0;
	rcq->size = 0;
	rcq->used = 0;
	rcq->header_count = 0;
}

/**
 * sunxi_g2d_rcq_reset - Reset RCQ buffer to empty state
 * @rcq: RCQ memory structure to reset
 * 
 * Clears the buffer allowing it to be reused for a new command sequence.
 */
void sunxi_g2d_rcq_reset(struct g2d_rcq_mem *rcq)
{
	if (!rcq || !rcq->vir_addr)
		return;
	
	/* Zero out the buffer */
	memset(rcq->vir_addr, 0, rcq->size);
	rcq->used = 0;
	rcq->header_count = 0;
}

/**
 * sunxi_g2d_rcq_add_block - Add a register block write command to RCQ
 * @rcq: RCQ memory structure
 * @reg_offset: Register offset from G2D base (e.g. 0x0100 for MIXER)
 * @data: Pointer to register data to write
 * @size: Size of data in bytes
 * 
 * Builds an RCQ command that will write @size bytes from @data
 * to registers starting at @reg_offset when the RCQ is executed.
 * 
 * Returns: 0 on success, -ENOMEM if buffer full
 */
int sunxi_g2d_rcq_add_block(struct g2d_rcq_mem *rcq, u32 reg_offset,
                             const void *data, u32 size)
{
	struct g2d_rcq_header *header;
	void *data_dest;
	u32 aligned_size;
	u32 total_needed;
	u32 header_offset, data_offset;
	dma_addr_t data_phy_addr;
	
	if (!rcq || !rcq->vir_addr || !data || size == 0)
		return -EINVAL;
	
	/* Align data size to 32 bytes */
	aligned_size = G2D_RCQ_BYTE_ALIGN(size);
	
	/* Header goes at current position */
	header_offset = rcq->used;
	
	/* Data must start at next 32-byte aligned position after header */
	data_offset = G2D_RCQ_BYTE_ALIGN(header_offset + sizeof(struct g2d_rcq_header));
	
	/* Calculate total space needed */
	total_needed = data_offset - header_offset + aligned_size;
	
	if (rcq->used + total_needed > rcq->size) {
		pr_err("RCQ buffer full: used=%u needed=%u size=%u\n",
		       rcq->used, total_needed, rcq->size);
		return -ENOMEM;
	}
	
	/* Get pointer to next header slot */
	header = (struct g2d_rcq_header *)((u8 *)rcq->vir_addr + header_offset);
	
	/* Data goes at aligned offset */
	data_dest = (void *)((u8 *)rcq->vir_addr + data_offset);
	
	/* Calculate physical address of data for DMA */
	data_phy_addr = rcq->phy_addr + data_offset;
	
	/* Verify data address is 32-byte aligned */
	if (data_phy_addr & 0x1F) {
		pr_err("RCQ data address 0x%pad not 32-byte aligned\n", 
		       &data_phy_addr);
		return -EINVAL;
	}
	
	/* Fill in RCQ header */
	header->low_addr = (u32)(data_phy_addr & 0xFFFFFFFF);
	header->dw0.bits.len = aligned_size;
	/* For ARM32, dma_addr_t is 32-bit, so high_addr is always 0 */
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	header->dw0.bits.high_addr = (u32)((data_phy_addr >> 32) & 0xFF);
#else
	header->dw0.bits.high_addr = 0;
#endif
	header->dirty.bits.dirty = 1;  /* Mark block for update */
	header->dirty.bits.n_header_len = 0;  /* Not used for single frame */
	header->reg_offset = reg_offset + 0x28000;  /* RCQ mode: registers at base+0x28000 */
	
	/* Copy register data */
	memcpy(data_dest, data, size);
	
	/* Zero-pad to aligned size */
	if (aligned_size > size)
		memset((u8 *)data_dest + size, 0, aligned_size - size);
	
	/* v2.1.10: Force write-through to RAM for DMA coherency
	 * The hardware will read this via MBUS DMA, so ensure cache is flushed
	 */
	wmb();  /* Write Memory Barrier - ensure CPU writes complete */
	
	/* Update counters */
	rcq->used += total_needed;
	rcq->header_count++;
	
	return 0;
}

/**
 * sunxi_g2d_rcq_setup_hw - Configure hardware to use RCQ buffer
 * @base: G2D register base address
 * @rcq: RCQ memory structure with commands
 * 
 * Writes the RCQ buffer address and header count to hardware registers.
 * Does NOT enable IRQ or start execution - call sunxi_g2d_rcq_start() for that.
 */
void sunxi_g2d_rcq_setup_hw(void __iomem *base, struct g2d_rcq_mem *rcq)
{
	union g2d_rcq_irq_ctl irq_ctl;
	union g2d_rcq_ctrl ctrl;
	u32 high_addr;
	
	if (!base || !rcq || !rcq->vir_addr || rcq->header_count == 0)
		return;
	
	/* v2.1.5: Disable RCQ IRQ FIRST before any status operations
	 * This prevents spurious IRQ from firing during cleanup/setup
	 */
	irq_ctl.dwval = readl(base + G2D_RCQ_IRQ_CTL);
	irq_ctl.bits.task_end_irq_en = 0;
	irq_ctl.bits.rcq_cfg_finish_irq_en = 0;
	writel(irq_ctl.dwval, base + G2D_RCQ_IRQ_CTL);
	wmb();
	
	/* Log initial state */
	pr_info("RCQ setup: Initial state - IRQ_CTL=0x%08x CTRL=0x%08x STATUS=0x%08x\n",
	        readl(base + G2D_RCQ_IRQ_CTL),
	        readl(base + G2D_RCQ_CTRL),
	        readl(base + G2D_RCQ_STATUS));
	
	pr_info("RCQ setup: CMD_CTL=0x%08x (should be 0x00010001 for DMA)\n",
	        readl(base + G2D_CMD_CTL));
	
	/* v2.1.7: DON'T clear STATUS here - BSP doesn't do it!
	 * The cfg_finish_irq bit persists even after W1C because hardware
	 * continuously sets it while RCQ is "configured". The IRQ handler
	 * will clear it when the actual task_end_irq fires.
	 * BSP sequence: disable IRQ → setup HEAD → write commands → enable IRQ+UPDATE → wait
	 */
	
	/* Disable UPDATE */
	ctrl.dwval = readl(base + G2D_RCQ_CTRL);
	ctrl.bits.update = 0;
	writel(ctrl.dwval, base + G2D_RCQ_CTRL);
	
	/* Extract high 8 bits of physical address (0 for ARM32) */
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	high_addr = (u32)((rcq->phy_addr >> 32) & 0xFF);
#else
	high_addr = 0;
#endif
	
	/* Write RCQ buffer address to hardware using writel for proper barriers
	 * CRITICAL: HEAD_LEN is in BYTES, not header count!
	 * BSP: rcq_header_len = alloc_num * sizeof(g2d_rcq_head) = count * 16
	 */
	u32 header_len_bytes = rcq->header_count * sizeof(struct g2d_rcq_header);
	
	writel((u32)(rcq->phy_addr & 0xFFFFFFFF), base + G2D_RCQ_HEAD_LOW);
	writel(high_addr, base + G2D_RCQ_HEAD_HIGH);
	writel(header_len_bytes, base + G2D_RCQ_HEAD_LEN);
	
	pr_info("RCQ setup: addr=0x%08x (high=0x%02x) headers=%u (%u bytes)\n", 
	        (u32)(rcq->phy_addr & 0xFFFFFFFF), high_addr, 
	        rcq->header_count, header_len_bytes);
	
	/* v2.1.10: Verify RCQ buffer CPU-side accessibility */
	{
		volatile struct g2d_rcq_header *verify_hdr = 
			(volatile struct g2d_rcq_header *)rcq->vir_addr;
		u32 read_low = verify_hdr->low_addr;
		u32 read_reg = verify_hdr->reg_offset;
		
		pr_info("RCQ CPU readback: First header low_addr=0x%08x reg_offset=0x%08x\n",
			read_low, read_reg);
		
		/* Sanity: reg_offset should be 0x28000 + MIXER offset */
		if ((read_reg < 0x28000) || (read_reg > 0x2C000)) {
			pr_err("RCQ ERROR: Invalid reg_offset=0x%08x (expected 0x28000-0x2C000)\n",
			       read_reg);
			pr_err("  This suggests memory corruption or cache coherency issue!\n");
		}
	}
	
	/* Read back to verify */
	pr_info("RCQ regs written: HEAD_LOW=0x%08x HEAD_HIGH=0x%08x HEAD_LEN=0x%08x\n",
	        readl(base + G2D_RCQ_HEAD_LOW),
	        readl(base + G2D_RCQ_HEAD_HIGH),
	        readl(base + G2D_RCQ_HEAD_LEN));

	/* If the hardware reads back zeros, provide additional diagnostics that
	 * help determine whether the IP is clocked/reset or otherwise inaccessible.
	 */
	if (readl(base + G2D_RCQ_HEAD_LOW) == 0) {
		pr_warn("RCQ regs readback zero - dumping extra diagnostics:\n");
		pr_warn(" CMD_CTL=0x%08x SCLK_GATE=0x%08x HCLK_GATE=0x%08x AHB_RESET=0x%08x\n",
		       readl(base + G2D_CMD_CTL),
		       readl(base + G2D_SCLK_GATE),
		       readl(base + G2D_HCLK_GATE),
		       readl(base + G2D_AHB_RESET));
		pr_warn(" RCQ_CTRL=0x%08x RCQ_STATUS=0x%08x VERSION=0x%08x\n",
		       readl(base + G2D_RCQ_CTRL),
		       readl(base + G2D_RCQ_STATUS),
		       readl(base + G2D_VERSION));
		pr_warn(" RCQ buffer phys=0x%08x headers=%u used=%u\n",
		       (u32)(rcq->phy_addr & 0xFFFFFFFF), rcq->header_count, rcq->used);
	}
}

/**
 * sunxi_g2d_rcq_start - Start RCQ execution
 * @base: G2D register base address
 * @use_en_bit: Whether to set RCQ_CTRL.EN bit (experimental)
 * @enable_irq: Whether to enable task_end_irq (BSP pattern for RCQ v2)
 * 
 * v2.8.4: Added enable_irq parameter to follow BSP pattern.
 * BSP enables task_end_irq_en=1 BEFORE triggering UPDATE for RCQ v2.
 * 
 * The EN bit (bit 4 of RCQ_CTRL) is not documented. Try both:
 * - use_en_bit=true: Set EN=1 (T113 v2 might need this)
 * - use_en_bit=false: Only UPDATE=1 (strict BSP pattern)
 */
void sunxi_g2d_rcq_start(void __iomem *base, bool use_en_bit, bool enable_irq)
{
	union g2d_rcq_irq_ctl irq_ctl;
	union g2d_rcq_ctrl ctrl;
	union g2d_rcq_status status;
	
	if (!base)
		return;
	
	/* Clear any pending RCQ status flags by writing 1 to them */
	status.dwval = 0;
	status.bits.task_end_irq = 1;      /* Clear task end flag (if any) */
	status.bits.cfg_finish_irq = 1;    /* Clear config finish flag */
	writel(status.dwval, base + G2D_RCQ_STATUS);
	
	/* v2.8.4: Configure RCQ IRQs based on enable_irq parameter
	 * BSP pattern (g2d_mixer.c:897-898):
	 *   g2d_top_rcq_irq_en(1);         // Enable task_end_irq
	 *   g2d_top_rcq_update_en(1);      // Trigger UPDATE
	 * 
	 * For RCQ v2 (T113), BSP enables task_end_irq_en=1 to allow MIXER to execute.
	 */
	irq_ctl.dwval = readl(base + G2D_RCQ_IRQ_CTL);
	if (enable_irq) {
		irq_ctl.bits.task_end_irq_en = 1;        /* Enable for RCQ v2 (BSP pattern) */
		irq_ctl.bits.rcq_cfg_finish_irq_en = 0;  /* Keep disabled - causes IRQ storm */
		pr_info("RCQ IRQ enabled: task_end_irq_en=1 (BSP pattern for RCQ v2)\n");
	} else {
		irq_ctl.bits.task_end_irq_en = 0;        /* Disable - legacy behavior */
		irq_ctl.bits.rcq_cfg_finish_irq_en = 0;  /* Disable - causes IRQ storm */
		pr_info("RCQ IRQs disabled: using MIXER_IRQ for completion (legacy)\n");
	}
	writel(irq_ctl.dwval, base + G2D_RCQ_IRQ_CTL);
	
	/* Trigger UPDATE - with optional EN bit */
	ctrl.dwval = readl(base + G2D_RCQ_CTRL);
	if (use_en_bit)
		ctrl.bits.en = 1;       /* Enable RCQ (T113 v2 - experimental) */
	ctrl.bits.update = 1;   /* Trigger RCQ execution */
	pr_info("RCQ writing CTRL=0x%08x (en=%d update=1 use_en_bit=%d)\n", 
	        ctrl.dwval, ctrl.bits.en, use_en_bit);
	writel(ctrl.dwval, base + G2D_RCQ_CTRL);
	wmb();  /* Ensure all writes are committed before checking status */
	
	/* Read status immediately after UPDATE to see if anything happened */
	pr_info("RCQ started: IRQ_CTL=0x%08x\n", irq_ctl.dwval);
	pr_info("RCQ status after UPDATE: CTRL=0x%08x STATUS=0x%08x\n",
	        readl(base + G2D_RCQ_CTRL), readl(base + G2D_RCQ_STATUS));
}
