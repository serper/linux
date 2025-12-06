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
#include <linux/string.h>
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

static inline u32 g2d_rcq_header_len_bytes(u32 header_count)
{
	u32 aligned_headers = G2D_RCQ_HEADER_ALIGN(header_count);

	return G2D_RCQ_BYTE_ALIGN(aligned_headers *
				  sizeof(struct g2d_rcq_header));
}

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
	rcq->header_len_bytes = 0;
	
	dev_dbg(dev, "RCQ buffer allocated: virt=%p phys=0x%pad size=%u\n",
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
	rcq->header_len_bytes = 0;
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
	rcq->header_len_bytes = 0;
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
	u32 header_offset, data_offset;
	dma_addr_t data_phy_addr;
	u32 new_header_len;
	u32 data_size;
	u32 delta;
	u32 new_header_count;
	u32 header_capacity;
	u8 *base;
	u32 needed_total;
	u32 i;
	
	if (!rcq || !rcq->vir_addr || !data || size == 0) {
		pr_err("RCQ add_block: invalid args rcq=%p vir=%p data=%p size=%u\n",
		       rcq, rcq ? rcq->vir_addr : NULL, data, size);
		return -EINVAL;
	}
	
	/* Align data size to 32 bytes */
	aligned_size = G2D_RCQ_BYTE_ALIGN(size);

	new_header_count = rcq->header_count + 1;
	new_header_len = g2d_rcq_header_len_bytes(new_header_count);
	base = rcq->vir_addr;

	/* Track existing data payload length (including padding) */
	if (rcq->used > rcq->header_len_bytes)
		data_size = rcq->used - rcq->header_len_bytes;
	else
		data_size = 0;

	needed_total = new_header_len + data_size + aligned_size;
	if (needed_total > rcq->size) {
		pr_err("RCQ buffer full: need=%u (hdr=%u data=%u new=%u) size=%u\n",
		       needed_total, new_header_len, data_size, aligned_size,
		       rcq->size);
		return -ENOMEM;
	}

	/* Grow header table if needed and shift payload accordingly */
	if (new_header_len > rcq->header_len_bytes) {
		delta = new_header_len - rcq->header_len_bytes;

		if (data_size)
			memmove(base + new_header_len,
				base + rcq->header_len_bytes,
				data_size);

		for (i = 0; i < rcq->header_count; i++) {
			struct g2d_rcq_header *h =
				(struct g2d_rcq_header *)(base +
				(i * sizeof(*h)));
			u64 addr = h->low_addr |
				   ((u64)h->dw0.bits.high_addr << 32);

			addr += delta;
			h->low_addr = (u32)addr;
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
			h->dw0.bits.high_addr = (addr >> 32) & 0xFF;
#endif
		}

		rcq->header_len_bytes = new_header_len;
		rcq->used = new_header_len + data_size;
	} else if (!rcq->header_len_bytes) {
		rcq->header_len_bytes = new_header_len;
		rcq->used = new_header_len;
	}

	header_capacity = rcq->header_len_bytes / sizeof(struct g2d_rcq_header);
	if (new_header_count > header_capacity) {
		pr_err("RCQ header table overflow: count=%u capacity=%u\n",
		       new_header_count, header_capacity);
		return -EINVAL;
	}

	if (rcq->used < rcq->header_len_bytes)
		rcq->used = rcq->header_len_bytes;

	/* Header pointer in contiguous header table */
	header_offset = rcq->header_count * sizeof(struct g2d_rcq_header);
	header = (struct g2d_rcq_header *)(base + header_offset);

	/* Data payload placed after header table, 32-byte aligned */
	data_offset = G2D_RCQ_BYTE_ALIGN(rcq->used);
	data_dest = (void *)(base + data_offset);
	data_phy_addr = rcq->phy_addr + data_offset;

	if (data_phy_addr & 0x1F) {
		pr_err("RCQ data address misaligned: phy=0x%pad offset=%u\n",
		       &data_phy_addr, data_offset);
		return -EINVAL;
	}

	/* Fill in RCQ header */
	header->low_addr = (u32)(data_phy_addr & 0xFFFFFFFF);
	header->dw0.bits.len = size;
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	header->dw0.bits.high_addr = (u32)((data_phy_addr >> 32) & 0xFF);
#else
	header->dw0.bits.high_addr = 0;
#endif
	header->dirty.bits.dirty = 1;
	header->dirty.bits.n_header_len = 0;
	header->reg_offset = reg_offset;

	memcpy(data_dest, data, size);
	if (aligned_size > size)
		memset((u8 *)data_dest + size, 0, aligned_size - size);

	wmb();

	rcq->used = data_offset + aligned_size;
	rcq->header_count = new_header_count;

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
	pr_debug("RCQ setup: Initial state - IRQ_CTL=0x%08x CTRL=0x%08x STATUS=0x%08x\n",
	        readl(base + G2D_RCQ_IRQ_CTL),
	        readl(base + G2D_RCQ_CTRL),
	        readl(base + G2D_RCQ_STATUS));
	
	pr_debug("RCQ setup: CMD_CTL=0x%08x (should be 0x00010011 for DMA)\n",
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
	u32 header_len_bytes;
	/* Prefer explicit header_len_bytes when provided (BSP-like layout) */
	if (rcq->header_len_bytes) {
		header_len_bytes = rcq->header_len_bytes;
	} else {
		header_len_bytes = g2d_rcq_header_len_bytes(rcq->header_count);
		rcq->header_len_bytes = header_len_bytes;
	}
	
	writel((u32)(rcq->phy_addr & 0xFFFFFFFF), base + G2D_RCQ_HEAD_LOW);
	writel(high_addr, base + G2D_RCQ_HEAD_HIGH);
	writel(header_len_bytes, base + G2D_RCQ_HEAD_LEN);
	
	pr_debug("RCQ setup: addr=0x%08x (high=0x%02x) headers=%u (%u bytes)\n", 
	        (u32)(rcq->phy_addr & 0xFFFFFFFF), high_addr, 
	        rcq->header_count, header_len_bytes);
	
	/* v2.1.10: Verify RCQ buffer CPU-side accessibility.
	 *
	 * After alinearnos con el BSP T113-S3 RCQ v2, los reg_offset
	 * de los headers son offsets planos dentro del espacio de
	 * registros G2D (0x0000xxxx), no 0x28000+offset como antes.
	 *
	 * Mantén solo un log de depuración para comprobar que la CPU
	 * ve algo razonable; no fuerces un rango concreto aquí porque
	 * depende del bloque que vaya en el primer header.
	 */
	{
		volatile struct g2d_rcq_header *verify_hdr =
			(volatile struct g2d_rcq_header *)rcq->vir_addr;
		u32 i;

		/* Print all headers we have packed with a human-readable block tag */
		for (i = 0; i < rcq->header_count; i++) {
			volatile struct g2d_rcq_header *h = &verify_hdr[i];
			u32 low = h->low_addr;
			u32 len = h->dw0.bits.len;
			u32 high = h->dw0.bits.high_addr;
			u32 roff = h->reg_offset;
			const char *blk = "UNKNOWN";

			if (roff >= G2D_WB && roff < (G2D_WB + 0x1000))
				blk = "WB";
			else if (roff >= G2D_V0 && roff < (G2D_V0 + 0x1000))
				blk = "V0";
			else if (roff >= G2D_BLD && roff < (G2D_BLD + 0x1000))
				blk = "BLD";
			else if (roff >= G2D_MIXER && roff < (G2D_MIXER + 0x1000))
				blk = "MIXER";
			else if (roff >= G2D_VSU && roff < (G2D_VSU + 0x2000))
				blk = "SCAL";
			else if (roff >= G2D_ROT && roff < (G2D_ROT + 0x1000))
				blk = "ROT";
			else if (roff >= G2D_UI0 && roff < (G2D_UI0 + 0x1000))
				blk = "UI0";
			else if (roff >= G2D_UI1 && roff < (G2D_UI1 + 0x1000))
				blk = "UI1";
			else if (roff >= G2D_UI2 && roff < (G2D_UI2 + 0x1000))
				blk = "UI2";
			else if (roff >= G2D_VSU && roff < (G2D_VSU + 0x1000))
				blk = "VSU";
			else if (roff >= G2D_GSU && roff < (G2D_GSU + 0x1000))
				blk = "GSU";

			pr_debug("RCQ CPU hdr[%u]: low=0x%08x len=0x%08x high=0x%02x reg_off=0x%08x (%s)\n",
				  i, low, len, high, roff, blk);
		}
	}
	
	/* Read back to verify */
	pr_debug("RCQ regs written: HEAD_LOW=0x%08x HEAD_HIGH=0x%08x HEAD_LEN=0x%08x\n",
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

/*
 * Minimal helpers for BSP-like single-frame fillrect layout.
 *
 * These are intentionally simple: they assume a single frame and up to
 * four logical blocks (WB, V0, BLD, MIXER). Callers are responsible for
 * providing properly sized register payloads and for checking return
 * codes. For now we only support a flat layout with one header per block
 * and a contiguous header area at the start of the RCQ buffer.
 */

int sunxi_g2d_rcq_build_frame_layout_fillrect(struct sunxi_g2d_rcq_frame_layout *layout,
						      void __iomem *base)
{
	/* Base is unused for now; keep parameter for future extensions */
	if (!layout)
		return -EINVAL;

	memset(layout, 0, sizeof(*layout));

	/*
	 * BSP pattern (observed from rcq_dump_fillrect.hex):
	 * - RCQ contains only WB, V0, BLD (3 blocks)
	 * - MIXER registers (FILLCOLOR0, SIZE, CTL) are written via direct MMIO
	 * - This prevents potential RCQ hardware protection issues with MIXER_CTL
	 *
	 * Default to 3 blocks; caller can adjust if needed.
	 */
	layout->block_count = 3;
	layout->header_count = layout->block_count;
	/* Header region length in bytes (one header per block). */
	layout->header_len_bytes = g2d_rcq_header_len_bytes(layout->header_count);

	return 0;
}

int sunxi_g2d_rcq_pack_frame(struct g2d_rcq_mem *rcq,
				   struct sunxi_g2d_rcq_frame_layout *layout,
				   const void *wb_regs,
				   const void *v0_regs,
				   const void *bld_regs,
				   const void *mixer_regs)
{
	struct g2d_rcq_header *headers;
	const void *payloads[4];
	u32 i;
	u32 data_offset;
	u32 header_bytes;

	if (!rcq || !rcq->vir_addr || !layout)
		return -EINVAL;
	if (layout->block_count == 0 || layout->block_count > 4)
		return -EINVAL;

	payloads[0] = wb_regs;
	payloads[1] = v0_regs;
	payloads[2] = bld_regs;
	payloads[3] = mixer_regs;

	/* Header area starts at beginning of buffer. */
	layout->header_count = layout->block_count;
	header_bytes = g2d_rcq_header_len_bytes(layout->block_count);
	layout->header_len_bytes = header_bytes;
	if (header_bytes > rcq->size)
		return -ENOMEM;

	headers = (struct g2d_rcq_header *)rcq->vir_addr;
	memset(headers, 0, header_bytes);
	data_offset = G2D_RCQ_BYTE_ALIGN(header_bytes);

	for (i = 0; i < layout->block_count; i++) {
		struct sunxi_g2d_reg_block *blk = &layout->blocks[i];
		struct g2d_rcq_header *hdr;
		void *dst;
		u32 aligned_size;
		dma_addr_t data_phy_addr;

		/* Skip blocks with no size (disabled/unused) */
		if (!blk->size)
			continue;

		/* Validate payload is provided for active blocks */
		if (!payloads[i]) {
			pr_err("RCQ pack_frame: block[%u] has size=%u but payload is NULL\n",
			       i, blk->size);
			return -EINVAL;
		}

		/* Align each payload to 32 bytes */
		aligned_size = G2D_RCQ_BYTE_ALIGN(blk->size);
		if (data_offset + aligned_size > rcq->size)
			return -ENOMEM;

		hdr = &headers[i];
		dst = (u8 *)rcq->vir_addr + data_offset;
		data_phy_addr = rcq->phy_addr + data_offset;

		if (data_phy_addr & 0x1F)
			return -EINVAL;

		hdr->low_addr = (u32)(data_phy_addr & 0xFFFFFFFF);
		hdr->dw0.bits.len = blk->size;
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
		hdr->dw0.bits.high_addr = (u32)((data_phy_addr >> 32) & 0xFF);
#else
		hdr->dw0.bits.high_addr = 0;
#endif
		hdr->dirty.bits.dirty = blk->dirty ? 1 : 0;
		hdr->dirty.bits.n_header_len = 0;
		/* Use plain G2D register offsets, like BSP RCQ v2 */
		hdr->reg_offset = blk->reg_offset;

		memcpy(dst, payloads[i], blk->size);
		if (aligned_size > blk->size)
			memset((u8 *)dst + blk->size, 0, aligned_size - blk->size);

		data_offset += aligned_size;
	}

	wmb();

	rcq->used = data_offset;
	rcq->header_count = layout->header_count;
	rcq->header_len_bytes = layout->header_len_bytes;

	return 0;
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
	
	if (!base)
		return;
	
	/* v2.8.4: Configure RCQ IRQs based on enable_irq parameter
	 * BSP pattern (g2d_mixer.c:897-898):
	 *   g2d_top_rcq_irq_en(1);         // Enable task_end_irq
	 *   g2d_top_rcq_update_en(1);      // Trigger UPDATE
	 */
	irq_ctl.dwval = readl(base + G2D_RCQ_IRQ_CTL);
	if (enable_irq) {
		irq_ctl.bits.task_end_irq_en = 1;        /* Enable for RCQ v2 (BSP pattern) */
		irq_ctl.bits.rcq_cfg_finish_irq_en = 0;  /* Keep disabled - causes IRQ storm */
		pr_debug("RCQ IRQ enabled: task_end_irq_en=1 (BSP pattern for RCQ v2)\n");
	} else {
		irq_ctl.bits.task_end_irq_en = 0;        /* Disable - legacy behavior */
		irq_ctl.bits.rcq_cfg_finish_irq_en = 0;  /* Disable - causes IRQ storm */
		pr_debug("RCQ IRQs disabled: using MIXER_IRQ for completion (legacy)\n");
	}
	writel(irq_ctl.dwval, base + G2D_RCQ_IRQ_CTL);
	
	/* Trigger UPDATE - follow BSP: only update=1, ignore EN */
	ctrl.dwval = readl(base + G2D_RCQ_CTRL);
	ctrl.bits.update = 1;   /* Trigger RCQ execution */
	ctrl.bits.en = 1;       /* Force EN bit for T113 */
	pr_debug("RCQ writing CTRL=0x%08x (update=1, en=1)\n",
	        ctrl.dwval);
	writel(ctrl.dwval, base + G2D_RCQ_CTRL);
	wmb();  /* Ensure all writes are committed before checking status */
	
	/* Read status immediately after UPDATE to see if anything happened */
	pr_debug("RCQ started: IRQ_CTL=0x%08x\n", irq_ctl.dwval);
	pr_debug("RCQ status after UPDATE: CTRL=0x%08x STATUS=0x%08x\n",
	        readl(base + G2D_RCQ_CTRL), readl(base + G2D_RCQ_STATUS));
}

/*
 * Pack 7 blocks for BSP-compatible RCQ structure.
 * BSP always uses 7 blocks in fixed order: V0, U0, U1, U2, SCAL, BLD, WB
 * Inactive blocks have dirty=0 and are filled with zeros.
 */
int sunxi_g2d_rcq_pack_frame_7blocks(struct g2d_rcq_mem *rcq,
				     struct sunxi_g2d_rcq_frame_layout *layout,
				     const void *v0_regs,
				     const void *u0_regs,
				     const void *u1_regs,
				     const void *u2_regs,
				     const void *scal_regs,
				     const void *bld_regs,
				     const void *wb_regs)
{
	struct g2d_rcq_header *headers;
	const void *payloads[7];
	u32 i;
	u32 data_offset;
	u32 header_bytes;

	if (!rcq || !rcq->vir_addr || !layout) {
		pr_err("RCQ pack_7blocks: invalid params (rcq=%p vir=%p layout=%p)\n",
		       rcq, rcq ? rcq->vir_addr : NULL, layout);
		return -EINVAL;
	}
	if (layout->block_count != 7) {
		pr_err("RCQ pack_7blocks: block_count=%u (expected 7)\n",
		       layout->block_count);
		return -EINVAL;
	}

	/* Order: V0, U0, U1, U2, SCAL, BLD, WB */
	payloads[0] = v0_regs;
	payloads[1] = u0_regs;
	payloads[2] = u1_regs;
	payloads[3] = u2_regs;
	payloads[4] = scal_regs;
	payloads[5] = bld_regs;
	payloads[6] = wb_regs;

	/* Header area: 7 headers × 16 bytes = 112 bytes */
	layout->header_count = layout->block_count;
	header_bytes = g2d_rcq_header_len_bytes(layout->block_count);
	layout->header_len_bytes = header_bytes;
	if (header_bytes > rcq->size) {
		pr_err("RCQ pack_7blocks: header_len=%u > rcq_size=%u\n",
		       header_bytes, rcq->size);
		return -ENOMEM;
	}

	headers = (struct g2d_rcq_header *)rcq->vir_addr;
	memset(headers, 0, header_bytes);
	data_offset = G2D_RCQ_BYTE_ALIGN(header_bytes);

	for (i = 0; i < 7; i++) {
		struct sunxi_g2d_reg_block *blk = &layout->blocks[i];
		struct g2d_rcq_header *hdr;
		void *dst;
		u32 aligned_size;
		dma_addr_t data_phy_addr;

		if (!blk->size || !payloads[i]) {
			pr_err("RCQ pack_7blocks: block[%u] invalid (size=%u payload=%p)\n",
			       i, blk->size, payloads[i]);
			return -EINVAL;
		}

		aligned_size = G2D_RCQ_BYTE_ALIGN(blk->size);
		if (data_offset + aligned_size > rcq->size)
			return -ENOMEM;

		hdr = &headers[i];
		dst = (u8 *)rcq->vir_addr + data_offset;
		data_phy_addr = rcq->phy_addr + data_offset;

		if (data_phy_addr & 0x1F)
			return -EINVAL;

		hdr->low_addr = (u32)(data_phy_addr & 0xFFFFFFFF);
		hdr->dw0.bits.len = blk->size;
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
		hdr->dw0.bits.high_addr = (u32)((data_phy_addr >> 32) & 0xFF);
#else
		hdr->dw0.bits.high_addr = 0;
#endif
		hdr->dirty.bits.dirty = blk->dirty ? 1 : 0;
		hdr->dirty.bits.n_header_len = 0;
		hdr->reg_offset = blk->reg_offset;

		memcpy(dst, payloads[i], blk->size);
		if (aligned_size > blk->size)
			memset((u8 *)dst + blk->size, 0, aligned_size - blk->size);

		data_offset += aligned_size;
	}

	wmb();

	rcq->header_count = layout->header_count;
	rcq->header_len_bytes = header_bytes;
	rcq->used = data_offset;

	return 0;
}

/**
 * sunxi_g2d_rcq_pack_frame_3blocks - Pack minimal 3-block RCQ frame (V0+BLD+WB)
 *
 * Optimized packing for simple blit operations without UI layers or scaler.
 * Used by CMD_COPY to avoid dummy headers that may confuse RCQ v2 hardware.
 */
int sunxi_g2d_rcq_pack_frame_3blocks(struct g2d_rcq_mem *rcq,
				     struct sunxi_g2d_rcq_frame_layout *layout,
				     const void *v0_regs,
				     const void *bld_regs,
				     const void *wb_regs)
{
	struct g2d_rcq_header *headers;
	const void *payloads[3];
	u32 i;
	u32 data_offset;
	u32 header_bytes;

	if (!rcq || !rcq->vir_addr || !layout) {
		pr_err("RCQ pack_3blocks: invalid params (rcq=%p vir=%p layout=%p)\n",
		       rcq, rcq ? rcq->vir_addr : NULL, layout);
		return -EINVAL;
	}
	if (layout->block_count != 3) {
		pr_err("RCQ pack_3blocks: block_count=%u (expected 3)\n",
		       layout->block_count);
		return -EINVAL;
	}

	/* Order: V0, BLD, WB */
	payloads[0] = v0_regs;
	payloads[1] = bld_regs;
	payloads[2] = wb_regs;

	/* Header area: 3 headers × 16 bytes = 48 bytes */
	layout->header_count = layout->block_count;
	header_bytes = g2d_rcq_header_len_bytes(layout->block_count);
	layout->header_len_bytes = header_bytes;
	if (header_bytes > rcq->size) {
		pr_err("RCQ pack_3blocks: header_len=%u > rcq_size=%u\n",
		       header_bytes, rcq->size);
		return -ENOMEM;
	}

	headers = (struct g2d_rcq_header *)rcq->vir_addr;
	memset(headers, 0, header_bytes);
	data_offset = G2D_RCQ_BYTE_ALIGN(header_bytes);

	for (i = 0; i < 3; i++) {
		struct sunxi_g2d_reg_block *blk = &layout->blocks[i];
		struct g2d_rcq_header *hdr;
		void *dst;
		u32 aligned_size;
		dma_addr_t data_phy_addr;

		if (!blk->size || !payloads[i]) {
			pr_err("RCQ pack_3blocks: block[%u] invalid (size=%u payload=%p)\n",
			       i, blk->size, payloads[i]);
			return -EINVAL;
		}

		aligned_size = G2D_RCQ_BYTE_ALIGN(blk->size);
		if (data_offset + aligned_size > rcq->size)
			return -ENOMEM;

		hdr = &headers[i];
		dst = (u8 *)rcq->vir_addr + data_offset;
		data_phy_addr = rcq->phy_addr + data_offset;

		if (data_phy_addr & 0x1F)
			return -EINVAL;

		hdr->low_addr = (u32)(data_phy_addr & 0xFFFFFFFF);
		hdr->dw0.bits.len = blk->size;
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
		hdr->dw0.bits.high_addr = (u32)((data_phy_addr >> 32) & 0xFF);
#else
		hdr->dw0.bits.high_addr = 0;
#endif
		hdr->dirty.bits.dirty = blk->dirty ? 1 : 0;
		hdr->dirty.bits.n_header_len = 0;
		hdr->reg_offset = blk->reg_offset;

		/* Copy payload */
		memcpy(dst, payloads[i], blk->size);
		if (aligned_size > blk->size)
			memset((u8 *)dst + blk->size, 0, aligned_size - blk->size);

		data_offset += aligned_size;
	}

	wmb();

	rcq->header_count = layout->header_count;
	rcq->header_len_bytes = header_bytes;
	rcq->used = data_offset;

	return 0;
}

/**
 * sunxi_g2d_rcq_pack_frame_4blocks - Pack 4-block RCQ frame (V0+BLD+SCAL+WB)
 *
 * Used for YUV copy operations that require VSU scaler in passthrough mode.
 * Block order: V0 (video overlay), BLD (blender), SCAL (scaler), WB (writeback)
 */
int sunxi_g2d_rcq_pack_frame_4blocks(struct g2d_rcq_mem *rcq,
				     struct sunxi_g2d_rcq_frame_layout *layout,
				     const void *v0_regs,
				     const void *bld_regs,
				     const void *scal_regs,
				     const void *wb_regs)
{
	struct g2d_rcq_header *headers;
	const void *payloads[4];
	u32 i;
	u32 data_offset;
	u32 header_bytes;

	if (!rcq || !rcq->vir_addr || !layout) {
		pr_err("RCQ pack_4blocks: invalid params (rcq=%p vir=%p layout=%p)\n",
		       rcq, rcq ? rcq->vir_addr : NULL, layout);
		return -EINVAL;
	}
	if (layout->block_count != 4) {
		pr_err("RCQ pack_4blocks: block_count=%u (expected 4)\n",
		       layout->block_count);
		return -EINVAL;
	}

	/* Order: V0, BLD, SCAL, WB */
	payloads[0] = v0_regs;
	payloads[1] = bld_regs;
	payloads[2] = scal_regs;
	payloads[3] = wb_regs;

	/* Header area: 4 headers × 16 bytes = 64 bytes */
	layout->header_count = layout->block_count;
	header_bytes = g2d_rcq_header_len_bytes(layout->block_count);
	layout->header_len_bytes = header_bytes;
	if (header_bytes > rcq->size) {
		pr_err("RCQ pack_4blocks: header_len=%u > rcq_size=%u\n",
		       header_bytes, rcq->size);
		return -ENOMEM;
	}

	headers = (struct g2d_rcq_header *)rcq->vir_addr;
	memset(headers, 0, header_bytes);
	data_offset = G2D_RCQ_BYTE_ALIGN(header_bytes);

	for (i = 0; i < 4; i++) {
		struct sunxi_g2d_reg_block *blk = &layout->blocks[i];
		struct g2d_rcq_header *hdr;
		void *dst;
		u32 aligned_size;
		dma_addr_t data_phy_addr;

		if (!blk->size || !payloads[i]) {
			pr_err("RCQ pack_4blocks: block[%u] invalid (size=%u payload=%p)\n",
			       i, blk->size, payloads[i]);
			return -EINVAL;
		}

		aligned_size = G2D_RCQ_BYTE_ALIGN(blk->size);
		if (data_offset + aligned_size > rcq->size)
			return -ENOMEM;

		hdr = &headers[i];
		dst = (u8 *)rcq->vir_addr + data_offset;
		data_phy_addr = rcq->phy_addr + data_offset;

		if (data_phy_addr & 0x1F)
			return -EINVAL;

		hdr->low_addr = (u32)(data_phy_addr & 0xFFFFFFFF);
		hdr->dw0.bits.len = blk->size;
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
		hdr->dw0.bits.high_addr = (u32)((data_phy_addr >> 32) & 0xFF);
#else
		hdr->dw0.bits.high_addr = 0;
#endif
		hdr->dirty.bits.dirty = blk->dirty ? 1 : 0;
		hdr->dirty.bits.n_header_len = 0;
		hdr->reg_offset = blk->reg_offset;

		/* Copy payload */
		memcpy(dst, payloads[i], blk->size);
		if (aligned_size > blk->size)
			memset((u8 *)dst + blk->size, 0, aligned_size - blk->size);

		data_offset += aligned_size;
	}

	wmb();

	rcq->header_count = layout->header_count;
	rcq->header_len_bytes = header_bytes;
	rcq->used = data_offset;

	return 0;
}

/**
 * sunxi_g2d_rcq_pack_frame_5blocks - Pack 5-block RCQ frame (UI2+V0+BLD+SCAL+WB)
 *
 * Used for alpha blending operations with UI2 foreground + V0 background.
 * Block order: UI2 (UI layer 2), V0 (video overlay), BLD (blender), SCAL (scaler), WB (writeback)
 * SCAL can be NULL for operations without scaling.
 */
int sunxi_g2d_rcq_pack_frame_5blocks(struct g2d_rcq_mem *rcq,
				     struct sunxi_g2d_rcq_frame_layout *layout,
				     const void *ui2_regs,
				     const void *v0_regs,
				     const void *bld_regs,
				     const void *scal_regs,
				     const void *wb_regs)
{
	struct g2d_rcq_header *headers;
	const void *payloads[5];
	u32 i, active_blocks;
	u32 data_offset;
	u32 header_bytes;

	if (!rcq || !rcq->vir_addr || !layout) {
		pr_err("RCQ pack_5blocks: invalid params (rcq=%p vir=%p layout=%p)\n",
		       rcq, rcq ? rcq->vir_addr : NULL, layout);
		return -EINVAL;
	}

	/* Order: UI2, V0, BLD, SCAL (optional), WB */
	payloads[0] = ui2_regs;
	payloads[1] = v0_regs;
	payloads[2] = bld_regs;
	payloads[3] = scal_regs; /* Can be NULL */
	payloads[4] = wb_regs;

	/* Count active blocks (SCAL may be NULL) */
	active_blocks = 0;
	for (i = 0; i < 5; i++) {
		if (payloads[i])
			active_blocks++;
	}

	if (layout->block_count != active_blocks) {
		pr_err("RCQ pack_5blocks: block_count=%u (expected %u)\n",
		       layout->block_count, active_blocks);
		return -EINVAL;
	}

	/* Header area */
	layout->header_count = layout->block_count;
	header_bytes = g2d_rcq_header_len_bytes(layout->block_count);
	layout->header_len_bytes = header_bytes;
	if (header_bytes > rcq->size) {
		pr_err("RCQ pack_5blocks: header_len=%u > rcq_size=%u\n",
		       header_bytes, rcq->size);
		return -ENOMEM;
	}

	headers = (struct g2d_rcq_header *)rcq->vir_addr;
	memset(headers, 0, header_bytes);
	data_offset = G2D_RCQ_BYTE_ALIGN(header_bytes);

	for (i = 0; i < 5; i++) {
		struct sunxi_g2d_reg_block *blk;
		struct g2d_rcq_header *hdr;
		void *dst;
		u32 aligned_size;
		dma_addr_t data_phy_addr;
		u32 block_idx;

		/* Skip NULL blocks (e.g., SCAL when not scaling) */
		if (!payloads[i])
			continue;

		/* Find corresponding layout block (skip NULL entries) */
		block_idx = 0;
		for (u32 j = 0; j <= i; j++) {
			if (payloads[j])
				block_idx++;
		}
		block_idx--; /* Convert to 0-based index */

		blk = &layout->blocks[block_idx];

		if (!blk->size) {
			pr_err("RCQ pack_5blocks: block[%u] invalid (size=%u)\n",
			       block_idx, blk->size);
			return -EINVAL;
		}

		aligned_size = G2D_RCQ_BYTE_ALIGN(blk->size);
		if (data_offset + aligned_size > rcq->size)
			return -ENOMEM;

		hdr = &headers[block_idx];
		dst = (u8 *)rcq->vir_addr + data_offset;
		data_phy_addr = rcq->phy_addr + data_offset;

		if (data_phy_addr & 0x1F)
			return -EINVAL;

		hdr->low_addr = (u32)(data_phy_addr & 0xFFFFFFFF);
		hdr->dw0.bits.len = blk->size;
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
		hdr->dw0.bits.high_addr = (u32)((data_phy_addr >> 32) & 0xFF);
#else
		hdr->dw0.bits.high_addr = 0;
#endif
		hdr->dirty.bits.dirty = blk->dirty ? 1 : 0;
		hdr->dirty.bits.n_header_len = 0;
		hdr->reg_offset = blk->reg_offset;

		/* Copy payload */
		memcpy(dst, payloads[i], blk->size);
		if (aligned_size > blk->size)
			memset((u8 *)dst + blk->size, 0, aligned_size - blk->size);

		data_offset += aligned_size;
	}

	wmb();

	rcq->header_count = layout->header_count;
	rcq->header_len_bytes = header_bytes;
	rcq->used = data_offset;

	return 0;
}

/**
 * sunxi_g2d_rcq_pack_frame_8blocks - Pack 8-block RCQ frame (V0+U0+U1+U2+SCAL+SCAL_EN+BLD+WB)
 *
 * Used for advanced operations with multiple UI layers and scaling.
 * Block order: V0 (video overlay), U0 (UI layer 0), U1 (UI layer 1), U2 (UI layer 2),
 * SCAL (scaler), SCAL_EN (scaler enable), BLD (blender), WB (writeback)
 */
int sunxi_g2d_rcq_pack_frame_8blocks(struct g2d_rcq_mem *rcq,
				     struct sunxi_g2d_rcq_frame_layout *layout,
				     const void *v0_regs,
				     const void *u0_regs,
				     const void *u1_regs,
				     const void *u2_regs,
				     const void *scal_regs,
				     const void *scal_en_regs,
				     const void *bld_regs,
				     const void *wb_regs)
{
	struct g2d_rcq_header *headers;
	const void *payloads[8];
	u32 i;
	u32 data_offset;
	u32 header_bytes;

	if (!rcq || !rcq->vir_addr || !layout) {
		pr_err("RCQ pack_8blocks: invalid params (rcq=%p vir=%p layout=%p)\n",
		       rcq, rcq ? rcq->vir_addr : NULL, layout);
		return -EINVAL;
	}
	if (layout->block_count != 8) {
		pr_err("RCQ pack_8blocks: block_count=%u (expected 8)\n",
		       layout->block_count);
		return -EINVAL;
	}

	/* Order: V0, U0, U1, U2, SCAL, SCAL_EN, BLD, WB */
	payloads[0] = v0_regs;
	payloads[1] = u0_regs;
	payloads[2] = u1_regs;
	payloads[3] = u2_regs;
	payloads[4] = scal_regs;
	payloads[5] = scal_en_regs;
	payloads[6] = bld_regs;
	payloads[7] = wb_regs;

	/* Header area: 8 headers × 16 bytes = 128 bytes */
	layout->header_count = layout->block_count;
	header_bytes = g2d_rcq_header_len_bytes(layout->block_count);
	layout->header_len_bytes = header_bytes;
	if (header_bytes > rcq->size) {
		pr_err("RCQ pack_8blocks: header_len=%u > rcq_size=%u\n",
		       header_bytes, rcq->size);
		return -ENOMEM;
	}

	headers = (struct g2d_rcq_header *)rcq->vir_addr;
	memset(headers, 0, header_bytes);
	data_offset = G2D_RCQ_BYTE_ALIGN(header_bytes);

	/* Debug: show payload pointers and expected block sizes before packing */
	for (i = 0; i < 8; i++) {
		pr_debug("RCQ PACK PRE: payload[%u]=%p blk.size=%u reg_off=0x%08x\n",
				 i, payloads[i], layout->blocks[i].size, layout->blocks[i].reg_offset);
	}

	for (i = 0; i < 8; i++) {
		struct sunxi_g2d_reg_block *blk = &layout->blocks[i];
		struct g2d_rcq_header *hdr;
		void *dst;
		u32 aligned_size;
		dma_addr_t data_phy_addr;

		if (!blk->size || !payloads[i]) {
			/* Allow empty blocks (e.g. SCAL_EN when VSU not used) */
			if (blk->size == 0) {
				hdr = &headers[i];
				hdr->low_addr = 0;
				hdr->dw0.bits.high_addr = 0;
				hdr->dw0.bits.len = 0;
				hdr->dirty.bits.dirty = 0;
				hdr->dirty.bits.n_header_len = 0;
				hdr->reg_offset = blk->reg_offset;
				continue;
			}
			
			pr_err("RCQ pack_8blocks: block[%u] invalid (size=%u payload=%p)\n",
			       i, blk->size, payloads[i]);
			return -EINVAL;
		}

		aligned_size = G2D_RCQ_BYTE_ALIGN(blk->size);
		if (data_offset + aligned_size > rcq->size)
			return -ENOMEM;

		hdr = &headers[i];
		dst = (u8 *)rcq->vir_addr + data_offset;
		pr_debug("RCQ PACK: idx=%u data_offset=0x%08x dst=%p payload=%p size=%u aligned=%u\n",
			 i, data_offset, dst, payloads[i], blk->size, aligned_size);
		data_phy_addr = rcq->phy_addr + data_offset;

		if (data_phy_addr & 0x1F)
			return -EINVAL;

		hdr->low_addr = (u32)(data_phy_addr & 0xFFFFFFFF);
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
		hdr->dw0.bits.high_addr = (u32)((data_phy_addr >> 32) & 0xFF);
#else
		hdr->dw0.bits.high_addr = 0;
#endif
		hdr->dw0.bits.len = blk->size;
		hdr->dirty.bits.dirty = blk->dirty ? 1 : 0;
		hdr->dirty.bits.n_header_len = 0;
		hdr->reg_offset = blk->reg_offset;

		memcpy(dst, payloads[i], blk->size);
		if (aligned_size > blk->size)
			memset((u8 *)dst + blk->size, 0, aligned_size - blk->size);

		data_offset += aligned_size;
	}

	wmb();

	rcq->header_count = layout->header_count;
	rcq->header_len_bytes = header_bytes;
	rcq->used = data_offset;

	return 0;
}
