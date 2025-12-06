/**
 * sunxi_g2d_do_fillrect_rcq_modular - Execute fillrect with RCQ using modular builders
 * 
 * EJEMPLO de refactorización usando las nuevas funciones builder modulares.
 * Demuestra cómo simplificar el código reutilizando bloques pre-configurados.
 * 
 * @g2d: G2D device
 * @dst_dma: Destination DMA address
 * @width: Width in pixels
 * @height: Height in pixels  
 * @pitch: Stride in bytes
 * @color: Fill color value
 * @color_format: Color format
 * @dst_format: Destination format
 * 
 * Returns: 0 on success, negative error on failure
 */
static int sunxi_g2d_do_fillrect_rcq_modular(struct sunxi_g2d_dev *g2d,
					      dma_addr_t dst_dma,
					      u32 width, u32 height,
					      u32 pitch, u32 color,
					      u32 color_format, u32 dst_format)
{
	struct sunxi_g2d_rcq_frame_layout layout;
	volatile struct g2d_top_reg *g2d_top;
	volatile struct g2d_mixer_glb_reg *g2d_mixer;
	int color_fmt_val, dst_fmt_val;
	int ret;
	
	/* Bloques allocatados por builders (kfree al final) */
	u32 *v0_regs = NULL, *v0_size;
	u32 *u0_regs = NULL, *u0_size;
	u32 *u1_regs = NULL, *u1_size;
	u32 *u2_regs = NULL, *u2_size;
	u32 *scal_regs = NULL, *scal_size;
	struct g2d_mixer_bld_reg *bld_regs = NULL;
	u32 bld_size;
	u32 *wb_regs = NULL, *wb_size;
	
	g2d_top = (volatile struct g2d_top_reg *)g2d->base;
	g2d_mixer = (volatile struct g2d_mixer_glb_reg *)(g2d->base + G2D_MIXER);
	
	/* Convert formats */
	color_fmt_val = sunxi_g2d_format_to_hw(color_format, NULL);
	if (color_fmt_val < 0)
		return -EOPNOTSUPP;
	dst_fmt_val = sunxi_g2d_format_to_hw(dst_format, NULL);
	if (dst_fmt_val < 0)
		return -EOPNOTSUPP;
	
	/* Color byte swap for XRGB8888 (hardware expects BGR) */
	if (dst_fmt_val == G2D_FORMAT_XRGB8888) {
		color = (color & 0xFF00FF00) |
			((color & 0x00FF0000) >> 16) |
			((color & 0x000000FF) << 16);
	}
	
	if (!g2d->rcq_enabled || !g2d->rcq.vir_addr)
		return -EOPNOTSUPP;
	
	/* Reset RCQ buffer and hardware */
	sunxi_g2d_rcq_reset(&g2d->rcq);
	g2d_top->ahb_rst.bits.mixer_ahb_rst = 0;
	wmb();
	g2d_top->ahb_rst.bits.mixer_ahb_rst = 1;
	wmb();
	
	/* ========== BUILD BLOCKS USING MODULAR FUNCTIONS ========== */
	
	/* Block 0: V0 (fill color) - ACTIVE */
	ret = g2d_rcq_build_v0_fillcolor(width, height, pitch, color, 
	                                  color_fmt_val, &v0_regs, &v0_size);
	if (ret)
		goto cleanup;
	
	/* Blocks 1-3: U0, U1, U2 (dummy UI layers) - INACTIVE */
	ret = g2d_rcq_build_ui_dummy(&u0_regs, &u0_size);
	if (ret)
		goto cleanup;
	ret = g2d_rcq_build_ui_dummy(&u1_regs, &u1_size);
	if (ret)
		goto cleanup;
	ret = g2d_rcq_build_ui_dummy(&u2_regs, &u2_size);
	if (ret)
		goto cleanup;
	
	/* Block 4: SCAL (dummy scaler) - INACTIVE */
	ret = g2d_rcq_build_scaler_dummy(&scal_regs, &scal_size);
	if (ret)
		goto cleanup;
	
	/* Block 5: BLD (blender with fill color) - ACTIVE */
	ret = g2d_rcq_build_bld_fillcolor(width, height, color, 
	                                   0x03010301,  /* SRCOVER Porter-Duff */
	                                   false, &bld_regs, &bld_size);
	if (ret)
		goto cleanup;
	
	/* Block 6: WB (writeback) - ACTIVE */
	ret = g2d_rcq_build_wb(width, height, pitch, dst_dma, dst_fmt_val,
	                        &wb_regs, &wb_size);
	if (ret)
		goto cleanup;
	
	/* ========== SETUP RCQ LAYOUT (7-block BSP structure) ========== */
	
	layout.block_count = 7;
	layout.header_len_bytes = 7 * sizeof(struct g2d_rcq_header);
	
	layout.blocks[0].size = v0_size;
	layout.blocks[0].reg_offset = V0_ATTCTL;  /* 0x0800 */
	layout.blocks[0].dirty = 1;  /* ACTIVE */
	
	layout.blocks[1].size = u0_size;
	layout.blocks[1].reg_offset = 0x1000;  /* G2D_UI0 */
	layout.blocks[1].dirty = 0;  /* INACTIVE */
	
	layout.blocks[2].size = u1_size;
	layout.blocks[2].reg_offset = 0x1800;  /* G2D_UI1 */
	layout.blocks[2].dirty = 0;  /* INACTIVE */
	
	layout.blocks[3].size = u2_size;
	layout.blocks[3].reg_offset = 0x2000;  /* G2D_UI2 */
	layout.blocks[3].dirty = 0;  /* INACTIVE */
	
	layout.blocks[4].size = scal_size;
	layout.blocks[4].reg_offset = 0x8000;  /* G2D_SCALER */
	layout.blocks[4].dirty = 0;  /* INACTIVE */
	
	layout.blocks[5].size = bld_size;
	layout.blocks[5].reg_offset = BLD_EN_CTL;  /* 0x0400 */
	layout.blocks[5].dirty = 1;  /* ACTIVE */
	
	layout.blocks[6].size = wb_size;
	layout.blocks[6].reg_offset = WB_ATT;  /* 0x3000 */
	layout.blocks[6].dirty = 1;  /* ACTIVE */
	
	/* ========== PACK INTO RCQ BUFFER ========== */
	
	ret = sunxi_g2d_rcq_pack_frame_7blocks(&g2d->rcq, &layout,
					       v0_regs, u0_regs, u1_regs, u2_regs,
					       scal_regs, bld_regs, wb_regs);
	if (ret)
		goto cleanup;
	
	/* ========== EXECUTE RCQ ========== */
	
	atomic_set(&g2d->irq_done, 0);
	{
		union g2d_rcq_irq_ctl irq_ctl;
		irq_ctl.dwval = 0;
		irq_ctl.bits.task_end_irq_en = 1;
		g2d_write(g2d, G2D_RCQ_IRQ_CTL, irq_ctl.dwval);
		wmb();
	}
	
	sunxi_g2d_rcq_setup_hw(g2d->base, &g2d->rcq);
	
	/* BSP pattern: MIXER global registers via MMIO before RCQ */
	g2d_write(g2d, MIXER_FILLCOLOR0, 0xFF000000);
	g2d_write(g2d, MIXER_SIZE, ((width - 1) & 0x1FFF) |
				(((height - 1) & 0x1FFF) << 16));
	wmb();
	
	/* Start RCQ and MIXER */
	sunxi_g2d_rcq_start(g2d->base, false, true);
	g2d_write(g2d, G2D_MIXER_CTL, G2D_MIXER_CTL_START);
	wmb();
	
	/* Wait for completion */
	{
		int timeout_jiffies = msecs_to_jiffies(100);
		int wait_result;
		
		wait_result = wait_event_interruptible_timeout(
			g2d->irq_wait,
			atomic_read(&g2d->irq_done),
			timeout_jiffies);
		if (wait_result <= 0) {
			ret = wait_result ? -ERESTARTSYS : -ETIMEDOUT;
			goto cleanup;
		}
	}
	
	/* Reset after completion */
	g2d_top->ahb_rst.bits.mixer_ahb_rst = 0;
	wmb();
	g2d_top->ahb_rst.bits.mixer_ahb_rst = 1;
	wmb();
	
	ret = 0;
	
cleanup:
	/* Free all allocated blocks */
	kfree(v0_regs);
	kfree(u0_regs);
	kfree(u1_regs);
	kfree(u2_regs);
	kfree(scal_regs);
	kfree(bld_regs);
	kfree(wb_regs);
	
	return ret;
}
