/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sunxi-g2d-rcq.h - Register Command Queue (RCQ) for Allwinner G2D
 * 
 * Copyright (C) 2025 Sergio Perez
 * 
 * RCQ allows building a command list in memory that the G2D hardware
 * will execute automatically via DMA, eliminating the need for CPU
 * to write each register individually.
 */

#ifndef __SUNXI_G2D_RCQ_H__
#define __SUNXI_G2D_RCQ_H__

#include <linux/types.h>
#include <linux/dma-mapping.h>

/* RCQ alignment requirements from hardware */
#define G2D_RCQ_BYTE_ALIGN(x)   (((x) + 31) & ~31)  /* 32-byte align */
#define G2D_RCQ_HEADER_ALIGN(x) (((x) + 1) & ~1)    /* 2-byte align */

/* Maximum RCQ command buffer size - one page should be enough for most cases */
#define G2D_RCQ_MAX_SIZE        (4 * 1024)

/**
 * struct g2d_rcq_header - RCQ command header
 * @low_addr: Low 32-bits of physical address (must be 32-byte aligned)
 * @dw0: Contains length (24-bit) and high address (8-bit)
 * @dirty: Dirty flag and next header length
 * @reg_offset: Register offset from G2D base address
 * 
 * Each header describes one block of register writes.
 * The hardware will DMA the data from @low_addr and write
 * to registers starting at @reg_offset.
 */
struct g2d_rcq_header {
	u32 low_addr;
	union {
		u32 dwval;
		struct {
			u32 len:24;       /* Data length in bytes */
			u32 high_addr:8;  /* High 8 bits of address */
		} bits;
	} dw0;
	union {
		u32 dwval;
		struct {
			u32 dirty:1;           /* 1=update this block */
			u32 res0:15;
			u32 n_header_len:16;   /* Next frame header length */
		} bits;
	} dirty;
	u32 reg_offset;   /* Offset from G2D base (e.g. 0x0100 for MIXER) */
};

/**
 * struct g2d_rcq_mem - RCQ memory buffer
 * @vir_addr: Virtual address for CPU writes
 * @phy_addr: Physical address for hardware DMA
 * @size: Total allocated size in bytes
 * @used: Currently used bytes
 * @header_count: Number of command headers in buffer
 * 
 * This structure manages the RCQ command buffer memory.
 */
struct g2d_rcq_mem {
	void *vir_addr;
	dma_addr_t phy_addr;
	u32 size;
	u32 used;
	u32 header_count;
};

/* Function prototypes */
int sunxi_g2d_rcq_alloc(struct device *dev, struct g2d_rcq_mem *rcq, u32 size);
void sunxi_g2d_rcq_free(struct device *dev, struct g2d_rcq_mem *rcq);
void sunxi_g2d_rcq_reset(struct g2d_rcq_mem *rcq);
int sunxi_g2d_rcq_add_block(struct g2d_rcq_mem *rcq, u32 reg_offset, 
                             const void *data, u32 size);
void sunxi_g2d_rcq_setup_hw(void __iomem *base, struct g2d_rcq_mem *rcq);
void sunxi_g2d_rcq_start(void __iomem *base, bool use_en_bit, bool enable_irq);

#endif /* __SUNXI_G2D_RCQ_H__ */
