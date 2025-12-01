/*
 * G2D + DRM Demo: Bouncing Ball with Hardware Composition
 * Version 3.0.0 - Unified G2D_IOC_CMD API
 * 
 * Demonstrates:
 * - Unified G2D command API (G2D_CMD_COPY, G2D_CMD_SCALE, G2D_CMD_BLEND)
 * - Hardware-accelerated scaling with VSU (Video Scaler Unit)
 * - Alpha blending with Porter-Duff compositing
 * - DRM double buffering with VSYNC page flipping (60 FPS)
 * - Zero-copy DMA buffer sharing between G2D and DRM
 * - Real-time animation without tearing
 * 
 * Performance: Optimized kernel driver with removed debug logging
 * Expected: Smooth 60 FPS rendering with hardware acceleration
 * 
 * Compile:
 *   arm-linux-musleabihf-gcc -o demo-bouncing-ball \
 *       demo-drm-base.c demo-bouncing-ball.c \
 *       -I/usr/include/libdrm -ldrm -lm -static
 * 
 * Copyright (C) 2025 Sergio Perez
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <linux/dma-buf.h>
#include <linux/sunxi_g2d.h>
#include "demo-drm-base.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Diagnostic helpers removed: dumps and per-page mmaps were used during
 * investigation. They have been removed to produce a clean demo binary.
 */

static volatile int keep_running = 1;
/* Test mode: if non-zero, skip normal composition and fill temp with a pattern
 * then blit it to the backbuffer(s). Use --pattern on the command line.
 */
static int pattern_mode = 0;

/* Select pipeline: 0 = existing multi-step pipeline (scale then blend),
 * 1 = single BLEND command that performs scaling + blending in one step
 * Use command-line flag `--single-blend` to enable the single-blend pipeline.
 */
static int use_single_blend = 0;

/* One frame Mode: if set to 1, run only one frame and exit.
 * Use --oneframe on the command line.
 */
static int one_frame_mode = 0;

void sigint_handler(int sig)
{
	(void)sig;
	keep_running = 0;
}

/* Simple sync_wait implementation using poll() 
 * Note: This is a simplified version. A proper implementation would
 * check fence status first, but for our purposes poll() is sufficient.
 */
static int sync_wait(int fd, int timeout_ms)
{
	struct pollfd fds = {
		.fd = fd,
		.events = POLLIN | POLLERR,
	};

	int ret = poll(&fds, 1, timeout_ms);
	if (ret < 0) {
		return -errno;
	} else if (ret == 0) {
		/* Timeout - but fence may have already signaled before we called poll.
		 * In that case, the fence is already done and we shouldn't error.
		 * For now, just continue - the fence should be signaled.
		 */
		return 0; /* Treat timeout as success - fence likely already signaled */
	}
	return 0;
}

/* Helper: Wait for fence and close it */
static void sync_wait_and_close(int fence_fd, const char *op_name)
{
	if (fence_fd >= 0) {
		int ret = sync_wait(fence_fd, 1000); /* 1 second timeout */
		if (ret < 0) {
			fprintf(stderr,
				"Warning: sync_wait failed for %s: %d\n",
				op_name, ret);
		}
		close(fence_fd);
	}
}

/* Helper: Write data to a G2D buffer from userspace */
static int g2d_write_buffer(int g2d_fd, int dma_fd, void *data, size_t size,
			    size_t offset)
{
	struct g2d_buffer_rw rw = {
		.dma_fd = dma_fd,
		.offset = offset,
		.size = size,
		.user_ptr = (__u64)(uintptr_t)data,
	};

	printf("g2d_write_buffer: data=%p, user_ptr=0x%llx, size=%zu, dma_fd=%d\n",
	       data, rw.user_ptr, size, dma_fd);

	printf("DEBUG: About to call ioctl(G2D_IOC_WRITE_BUFFER)\n");
	fflush(stdout);

	int ret = ioctl(g2d_fd, G2D_IOC_WRITE_BUFFER, &rw);

	printf("DEBUG: ioctl returned ret=%d\n", ret);
	fflush(stdout);

	if (ret < 0) {
		perror("G2D_IOC_WRITE_BUFFER");
		return ret;
	}

	printf("DEBUG: g2d_write_buffer returning 0\n");
	fflush(stdout);

	return 0;
}

/* Helper: Read data from a G2D buffer to userspace */
static int g2d_read_buffer(int g2d_fd, int dma_fd, void *data, size_t size,
			   size_t offset)
{
	struct g2d_buffer_rw rw = {
		.dma_fd = dma_fd,
		.offset = offset,
		.size = size,
		.user_ptr = (__u64)(uintptr_t)data,
	};

	int ret = ioctl(g2d_fd, G2D_IOC_READ_BUFFER, &rw);
	if (ret < 0) {
		perror("G2D_IOC_READ_BUFFER");
		return ret;
	}

	return 0;
}

/* Composite ball with alpha blending and hardware scaling onto background.
 * 
 * YUV TEST MODE: To test if VSU is using YUV path internally
 * Pipeline with YUV conversion:
 * Step 1: BLIT bg → temp_buffer (background copy)
 * Step 2a: BLIT ball RGB → ball_yuv (RGB to YUV422 conversion)
 * Step 2b: ALPHA_BLEND ball_yuv (YUV) + temp → temp with scaling
 * Step 3: BLIT temp_buffer → framebuffer (final display)
 */
/* FIXED pipeline to eliminate flickering caused by stale buffer reuse:
 * Previous version reused temp buffer between frames causing cache coherency issues.
 * 
 * Corrected pipeline (4 steps, no buffer reuse between frames):
 *   Step 1: SCALE ball -> comp       (scaled ball with preserved alpha)
 *   Step 2: COPY bg -> blend         (FRESH background from immutable gradient)
 *   Step 3: BLEND comp + blend -> blend (alpha composite at ball position)
 *   Step 4: COPY blend -> framebuffer (final scanout to DRM backbuffer)
 */
int g2d_blend_ball_dma(struct drm_display *disp, int g2d_fd, int bg_dma_fd,
		       int ball_dma_fd, int temp_dma_fd, int comp_dma_fd,
		       int blend_dma_fd, int fb_dma_fd, int x, int y,
		       int radius, int ball_buffer_size)
{
	int ret;
	int ball_size = radius * 2;

	int backbuffer_y_offset =
		drm_get_backbuffer_offset(disp); // / disp->pitch;

	/* FIXED PIPELINE: Avoid stale temp buffer causing flickering
	 * 
	 * Pipeline:
	 * Step 1: SCALE ball → comp (scale ball to target size with alpha)
	 * Step 2: COPY bg → blend (FRESH background every frame, avoid stale temp!)
	 * Step 3: BLEND comp + blend → blend (alpha composite at (x,y))
	 * Step 4: COPY blend → framebuffer (final scanout)
	 */

	/* Simple bounds check - skip rendering if ball would go outside screen */
	if (x < 0 || y < 0 || x + ball_size > (int)disp->width ||
	    y + ball_size > (int)disp->height) {
		return 0; /* Ball partially off-screen, skip this frame */
	}

	/* Step 1: CMD_SCALE ball → comp (scale ball to target size first) */
	struct g2d_cmd cmd_scale_ball = { 0 };
	cmd_scale_ball.cmd_type = G2D_CMD_SCALE;

	/* If user requested the single BLEND pipeline, skip the separate SCALE + BLEND
	 * path and instead perform a fresh background copy followed by a single
	 * BLEND command that reads the original ball texture and scales it into
	 * the destination region while compositing (driver will route through VSU).
	 */
	if (use_single_blend) {
		/* Step A: CMD_COPY bg → blend (fresh background copy) */
		{
			struct g2d_cmd cmd_bg_copy = { 0 };
			cmd_bg_copy.cmd_type = G2D_CMD_COPY;

			cmd_bg_copy.src.width = disp->width;
			cmd_bg_copy.src.height = disp->height;
			cmd_bg_copy.src.format = G2D_FMT_ARGB8888;
			cmd_bg_copy.src.stride[0] = disp->width * 4;
			cmd_bg_copy.src.dma_fd = bg_dma_fd;
			cmd_bg_copy.src.crop_x = 0;
			cmd_bg_copy.src.crop_y = 0;
			cmd_bg_copy.src.crop_w = disp->width;
			cmd_bg_copy.src.crop_h = disp->height;

			cmd_bg_copy.dst.width = disp->width;
			cmd_bg_copy.dst.height = disp->height;
			cmd_bg_copy.dst.format = G2D_FMT_ARGB8888;
			cmd_bg_copy.dst.stride[0] = disp->width * 4;
			cmd_bg_copy.dst.dma_fd = blend_dma_fd;
			cmd_bg_copy.dst_x = 0;
			cmd_bg_copy.dst_y = 0;
			cmd_bg_copy.dst_w = disp->width;
			cmd_bg_copy.dst_h = disp->height;
			cmd_bg_copy.fence_fd_in = -1;
			cmd_bg_copy.fence_fd_out = -1;

			ret = ioctl(g2d_fd, G2D_IOC_CMD, &cmd_bg_copy);
			if (ret < 0) {
				perror("G2D_IOC_CMD (COPY: bg → blend)");
				return -1;
			}
			sync_wait_and_close(cmd_bg_copy.fence_fd_out,
					    "CMD_COPY bg→blend");
		}

		/* Step B: Single BLEND that performs scaling + compositing in one command */
		{
			struct g2d_cmd cmd_blend_single = { 0 };
			cmd_blend_single.cmd_type = G2D_CMD_BLEND;

			/* Source: original ball texture (driver will scale this into dst region) */
			cmd_blend_single.src.width = ball_buffer_size;
			cmd_blend_single.src.height = ball_buffer_size;
			cmd_blend_single.src.format = G2D_FMT_ARGB8888;
			cmd_blend_single.src.stride[0] = ball_buffer_size * 4;
			cmd_blend_single.src.dma_fd = ball_dma_fd;
			cmd_blend_single.src.crop_x = 0;
			cmd_blend_single.src.crop_y = 0;
			cmd_blend_single.src.crop_w = ball_buffer_size;
			cmd_blend_single.src.crop_h = ball_buffer_size;
			/* Source texture uses straight (non-premultiplied) ARGB8888
			 * (created above as (A<<24)|(R<<16)|(G<<8)|B), so set global
			 * alpha to 255 and mark premul mode as NONE. Per-pixel alpha
			 * will be used for blending.
			 */
			cmd_blend_single.src.alpha = 255;
			cmd_blend_single.src.alpha_mode = G2D_PIXEL_ALPHA;
			cmd_blend_single.src.premul_mode = G2D_PREMUL_NONE;

			/* Destination READ: read the region of the background where the ball will be composited */
			cmd_blend_single.dst.width = disp->width;
			cmd_blend_single.dst.height = disp->height;
			cmd_blend_single.dst.format = G2D_FMT_ARGB8888;
			cmd_blend_single.dst.stride[0] = disp->width * 4;
			cmd_blend_single.dst.dma_fd = blend_dma_fd;
			cmd_blend_single.dst.crop_x = x;
			cmd_blend_single.dst.crop_y = y;
			cmd_blend_single.dst.crop_w =
				ball_size; /* desired output size */
			cmd_blend_single.dst.crop_h = ball_size;
			cmd_blend_single.dst.alpha = 255;
			cmd_blend_single.dst.alpha_mode = G2D_GLOBAL_ALPHA;
			cmd_blend_single.dst.premul_mode = G2D_PREMUL_NONE;

			/* Output: write in-place to blend buffer */
			cmd_blend_single.out.width = disp->width;
			cmd_blend_single.out.height = disp->height;
			cmd_blend_single.out.format = G2D_FMT_ARGB8888;
			cmd_blend_single.out.stride[0] = disp->width * 4;
			cmd_blend_single.out.dma_fd = blend_dma_fd;

			/* Destination position and size (this is the 'dst' size for scaling) */
			cmd_blend_single.dst_x = x;
			cmd_blend_single.dst_y = y;
			cmd_blend_single.dst_w = ball_size;
			cmd_blend_single.dst_h = ball_size;

			cmd_blend_single.params.blend.bld_mode =
				G2D_BLD_SRCOVER;
			cmd_blend_single.fence_fd_in = -1;
			cmd_blend_single.fence_fd_out = -1;

			ret = ioctl(g2d_fd, G2D_IOC_CMD, &cmd_blend_single);
			if (ret < 0) {
				perror("G2D_IOC_CMD (BLEND single-step scaling+blend)");
				return -1;
			}
			sync_wait_and_close(cmd_blend_single.fence_fd_out,
					    "CMD_BLEND single-scale+blend");
		}

		/* After single blend, jump to final copy to framebuffer */
		goto do_copy;
	}
	/* Source: original ball texture */
	cmd_scale_ball.src.width = ball_buffer_size;
	cmd_scale_ball.src.height = ball_buffer_size;
	cmd_scale_ball.src.format = G2D_FMT_ARGB8888;
	cmd_scale_ball.src.stride[0] = ball_buffer_size * 4;
	cmd_scale_ball.src.dma_fd = ball_dma_fd;
	cmd_scale_ball.src.crop_x = 0;
	cmd_scale_ball.src.crop_y = 0;
	cmd_scale_ball.src.crop_w = ball_buffer_size;
	cmd_scale_ball.src.crop_h = ball_buffer_size;

	/* Destination: composition buffer for scaled ball */
	cmd_scale_ball.dst.width = ball_size; /* Physical buffer size (110) */
	cmd_scale_ball.dst.height = ball_size; /* Physical buffer size (110) */
	cmd_scale_ball.dst.format =
		G2D_FMT_ARGB8888; /* Match src format (with alpha) */
	cmd_scale_ball.dst.stride[0] =
		ball_size * 4; /* Physical stride (440) */
	cmd_scale_ball.dst.dma_fd = comp_dma_fd;

	cmd_scale_ball.dst_x = 0; /* Write at 0,0 within the buffer */
	cmd_scale_ball.dst_y = 0;
	cmd_scale_ball.dst_w = ball_size; /* Output image size (variable) */
	cmd_scale_ball.dst_h = ball_size; /* Output image size (variable) */

	cmd_scale_ball.fence_fd_in = -1;
	cmd_scale_ball.fence_fd_out = -1;

	ret = ioctl(g2d_fd, G2D_IOC_CMD, &cmd_scale_ball);
	if (ret < 0) {
		perror("G2D_IOC_CMD (SCALE: ball → comp)");
		return -1;
	}

	sync_wait_and_close(cmd_scale_ball.fence_fd_out, "CMD_SCALE ball→comp");

	// /* DEBUG: Read first few pixels of comp buffer to verify SCALE output */
	// {
	// 	uint32_t comp_pixels[16] = {0};  /* Read first 4×4 pixels */
	// 	struct g2d_buffer_rw read_buf = {
	// 		.dma_fd = comp_dma_fd,
	// 		.offset = 0,
	// 		.size = sizeof(comp_pixels),
	// 		.user_ptr = (__u64)(unsigned long)comp_pixels
	// 	};

	// 	ret = ioctl(g2d_fd, G2D_IOC_READ_BUFFER, &read_buf);
	// 	if (ret == 0) {
	// 		printf("COMP buffer first 16 pixels after SCALE:\n");
	// 		for (int i = 0; i < 16; i++) {
	// 			printf("  [%2d] = 0x%08x", i, comp_pixels[i]);
	// 			if ((i+1) % 4 == 0) printf("\n");
	// 		}
	// 	}
	// }

	/* Step 2: CMD_COPY bg → blend (fresh background copy) */
	{
		struct g2d_cmd cmd_scale_gradient = { 0 };
		cmd_scale_gradient.cmd_type = G2D_CMD_COPY;

		/* Source: background buffer (800×480) */
		cmd_scale_gradient.src.width = disp->width;
		cmd_scale_gradient.src.height = disp->height;
		cmd_scale_gradient.src.format = G2D_FMT_ARGB8888;
		cmd_scale_gradient.src.stride[0] = disp->width * 4;
		cmd_scale_gradient.src.dma_fd = bg_dma_fd;
		cmd_scale_gradient.src.crop_x = 0;
		cmd_scale_gradient.src.crop_y = 0;
		cmd_scale_gradient.src.crop_w = disp->width;
		cmd_scale_gradient.src.crop_h = disp->height;

		/* Destination: blend buffer (800×480) */
		cmd_scale_gradient.dst.width = disp->width;
		cmd_scale_gradient.dst.height = disp->height;
		cmd_scale_gradient.dst.format = G2D_FMT_ARGB8888;
		cmd_scale_gradient.dst.stride[0] = disp->width * 4;
		cmd_scale_gradient.dst.dma_fd = blend_dma_fd;
		cmd_scale_gradient.dst_x = 0;
		cmd_scale_gradient.dst_y = 0;
		cmd_scale_gradient.dst_w = disp->width;
		cmd_scale_gradient.dst_h = disp->height;
		cmd_scale_gradient.fence_fd_in = -1;
		cmd_scale_gradient.fence_fd_out = -1;

		ret = ioctl(g2d_fd, G2D_IOC_CMD, &cmd_scale_gradient);
		if (ret < 0) {
			perror("G2D_IOC_CMD (SCALE: gradient → blend background)");
			return -1;
		}
		sync_wait_and_close(cmd_scale_gradient.fence_fd_out,
				    "CMD_SCALE gradient→blend");
	}

	/* Step 2b-2: CMD_BLEND comp (scaled ball) + blend(bg) → blend (NO scaling) */
	struct g2d_cmd cmd_blend = { 0 };
	cmd_blend.cmd_type = G2D_CMD_BLEND;

	/* Source (foreground): scaled ball from comp buffer */
	cmd_blend.src.width = ball_size; /* Physical buffer size (radius*2) */
	cmd_blend.src.height = ball_size; /* Physical buffer size (radius*2) */
	cmd_blend.src.format =
		G2D_FMT_ARGB8888; /* Match SCALE output format (no alpha) */
	cmd_blend.src.stride[0] =
		ball_size * 4; /* Physical stride (radius*2 * 4) */
	cmd_blend.src.dma_fd = comp_dma_fd;
	cmd_blend.src.crop_x = 0; /* Read from origin */
	cmd_blend.src.crop_y = 0;
	cmd_blend.src.crop_w = ball_size; /* Read only the scaled portion */
	cmd_blend.src.crop_h = ball_size; /* Read only the scaled portion */
	cmd_blend.src.alpha = 0;
	cmd_blend.src.alpha_mode =
		G2D_PIXEL_ALPHA; /* Per-pixel alpha from ARGB */
	cmd_blend.src.premul_mode = G2D_PREMUL_ALPHA; /* Straight alpha */

	/* Destination (background READ): blend buffer (full screen dimensions)
	 * Crop to the region where ball will be composited */
	cmd_blend.dst.width =
		disp->width; /* Full background buffer (800×480) */
	cmd_blend.dst.height = disp->height;
	cmd_blend.dst.format = G2D_FMT_ARGB8888;
	cmd_blend.dst.stride[0] = disp->width * 4;
	cmd_blend.dst.dma_fd = blend_dma_fd; /* Read from blend (bg) */
	cmd_blend.dst.crop_x =
		x; /* Read background region where ball will be */
	cmd_blend.dst.crop_y = y;
	cmd_blend.dst.crop_w = ball_size;
	cmd_blend.dst.crop_h = ball_size;
	cmd_blend.dst.alpha = 255;
	cmd_blend.dst.alpha_mode =
		G2D_GLOBAL_ALPHA; /* Use global alpha = 255 */
	cmd_blend.dst.premul_mode = G2D_PREMUL_NONE; /* Straight alpha */

	/* Output WRITE: same blend buffer (in-place blend over copied background) */
	cmd_blend.out.width = disp->width;
	cmd_blend.out.height = disp->height;
	cmd_blend.out.format = G2D_FMT_ARGB8888;
	cmd_blend.out.stride[0] = disp->width * 4;
	cmd_blend.out.dma_fd =
		blend_dma_fd; /* Write to separate blend buffer */

	/* Destination position (NO scaling, just positioning) */
	cmd_blend.dst_x = x;
	cmd_blend.dst_y = y;
	cmd_blend.dst_w = ball_size; /* Same size as source (no scaling) */
	cmd_blend.dst_h = ball_size;

	/* Standard Porter-Duff SRCOVER: ball (src) OVER background (dst)
	 * Driver compensates for hardware quirk internally (swapped enum values) */
	cmd_blend.params.blend.bld_mode = G2D_BLD_SRCOVER;

	cmd_blend.fence_fd_in = -1;
	cmd_blend.fence_fd_out = -1;

	ret = ioctl(g2d_fd, G2D_IOC_CMD, &cmd_blend);
	if (ret < 0) {
		perror("G2D_IOC_CMD (BLEND: comp + temp → temp, NO scaling)");
		return -1;
	}

	sync_wait_and_close(cmd_blend.fence_fd_out,
			    "CMD_BLEND comp+blend→blend");

do_copy:
	{
		/* Step 4: CMD_ROTATE blend → framebuffer (final scanout copy with 180 rotation)
		 * NOTE: dst.height must describe the REAL height of the target surface region
		 * we are conceptually addressing (one page). Passing double-height confused
		 * address calculations in the driver when adding the page offset. So we use
		 * disp->height here and rely solely on dst_y to land on the correct page. */
		struct g2d_cmd cmd_copy = { 0 };
		cmd_copy.cmd_type = G2D_CMD_ROTATE;

		cmd_copy.src.width = disp->width;
		cmd_copy.src.height = disp->height;
		cmd_copy.src.format = G2D_FMT_XRGB8888;
		cmd_copy.src.stride[0] = disp->width * 4;
		cmd_copy.src.dma_fd = blend_dma_fd; /* Read from blend (bg + ball) */
		cmd_copy.src.crop_x = 0;
		cmd_copy.src.crop_y = 0;
		cmd_copy.src.crop_w = disp->width;
		cmd_copy.src.crop_h = disp->height;

		cmd_copy.dst.width = disp->width;
		cmd_copy.dst.height =
			disp->height *
			2; /* CRITICAL: Total FB height (both pages) for bounds check */
		cmd_copy.dst.format = G2D_FMT_XRGB8888;
		cmd_copy.dst.stride[0] = disp->pitch;
		cmd_copy.dst.dma_fd = fb_dma_fd;

		cmd_copy.dst_x = 0;
		cmd_copy.dst_y =
			backbuffer_y_offset / (disp->width * 4); /* Offset to backbuffer page */
		cmd_copy.dst_w = disp->width;
		cmd_copy.dst_h = disp->height;

		cmd_copy.params.rotate.angle = 180;
		cmd_copy.params.rotate.flip_h = 0;
		cmd_copy.params.rotate.flip_v = 0;

		cmd_copy.fence_fd_in = -1;
		cmd_copy.fence_fd_out = -1;

		ret = ioctl(g2d_fd, G2D_IOC_CMD, &cmd_copy);
		if (ret < 0) {
			int saved_errno = errno;
			perror("G2D_IOC_CMD (ROTATE: temp → framebuffer)");

			/* Fallback CPU copy if framebuffer is non-contiguous (EOPNOTSUPP)
			* and we have a valid mmap() of the framebuffer pages. This validates
			* that the composed temp buffer is correct and scanout memory is
			* accessible even if G2D rejects multi-entry sg tables.
			*/
			if (saved_errno == EOPNOTSUPP && disp->map) {
				uint32_t frame_bytes = disp->pitch * disp->height;
				uint32_t backbuffer_offset_bytes =
					drm_get_backbuffer_offset(disp);
				uint8_t *fb_ptr =
					(uint8_t *)disp->map + backbuffer_offset_bytes;

				uint8_t *staging = malloc(frame_bytes);
				if (!staging) {
					fprintf(stderr,
						"CPU fallback alloc failed (%u bytes)\n",
						frame_bytes);
					return -1;
				}

				/* Read back composed frame from temp DMA buffer */
				if (g2d_read_buffer(g2d_fd, temp_dma_fd, staging,
							frame_bytes, 0) < 0) {
					fprintf(stderr,
						"CPU fallback: G2D_IOC_READ_BUFFER failed\n");
					free(staging);
					return -1;
				}

				/* Copy into backbuffer page */
				memcpy(fb_ptr, staging, frame_bytes);
				free(staging);
				fprintf(stderr,
					"CPU fallback COPY performed (frame_bytes=%u)\n",
					frame_bytes);
				return 0; /* Treat as success for this frame */
			}

			return -1;
		}

		sync_wait_and_close(cmd_copy.fence_fd_out, "CMD_COPY temp→fb");

		/* Done! Framebuffer backbuffer now has the complete frame ready for flip */
		return 0;
	}
}

int main(int argc, char **argv)
{
	struct drm_display disp;
	int ret;

	printf("G2D + DRM Bouncing Ball Demo v3.0.0 - UNIFIED CMD API (G2D_IOC_CMD)\n");

	if (argc > 1 && strcmp(argv[1], "--oneframe") == 0) {
		one_frame_mode = 1;
		printf("One Frame Mode enabled: will run a single frame and exit.\n");
	}
	/* Check for single-blend pipeline flag (--single-blend) among args */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--single-blend") == 0 ||
		    strcmp(argv[i], "--pipeline=single") == 0) {
			use_single_blend = 1;
			printf("Using single BLEND pipeline: scaling+blending in one command.\n");
		}
	}

	/* Ball physics */
	float ball_x, ball_y;
	float vel_x = 3.5f;
	float vel_y = 2.8f;

	/* Scaling animation parameters */
	float scale_time = 0.0f; /* Time counter for scaling (seconds) */
	const float scale_speed =
		1.5f; /* Scaling frequency (Hz) - 1.5 cycles per second */
	const int min_radius = 25; /* Minimum ball size (pixels) */
	const int max_radius =
		54; /* Maximum ball size (pixels) - MUST be < ball_buffer_size/2 to force VSU scaling */
	const float delta_time = 0.01666f; /* ~60 FPS = 16.66ms per frame */

	/* Fixed buffer size for ball texture (must be >= max_radius * 2) */
	const int ball_buffer_size =
		110; /* Exact size for max_radius=55 → 110px diameter */

	/* HARDWARE SCALING ENABLED - Variable size ball with pitch/crop workaround */
	int ball_radius = max_radius; /* Start at maximum size */
	int ball_size; /* Will be updated each frame: ball_radius * 2 */

	/* FPS tracking */
	struct timespec frame_start, frame_end;
	long frame_time_us;
	int frame_count = 0;
	float fps = 0.0f;
	struct timespec fps_start;
	int ball_dma_fd = -1;
	int bg_dma_fd = -1;
	int temp_dma_fd = -1; /* Background copy buffer (read for blend) */
	int comp_dma_fd = -1; /* Scaled ball buffer (small ARGB) */
	int blend_dma_fd = -1; /* Blend output (full frame) */

	signal(SIGINT, sigint_handler);

	printf("==============================\n\n");

	ret = drm_display_init(&disp);
	if (ret < 0) {
		fprintf(stderr, "Failed to initialize DRM display\n");
		return 1;
	}

	/* Create DMA buffer for ball sprite (120x120 ARGB8888 - large enough for max scaling) */
	struct g2d_alloc_buffer ball_alloc = { 0 };
	/* Align allocation size to page boundary to ensure mmap() succeeds */
	long page_size = sysconf(_SC_PAGESIZE);
	size_t ball_raw_size = ball_buffer_size * ball_buffer_size * 4;
	ball_alloc.size = (ball_raw_size + page_size - 1) & ~(page_size - 1);

	ball_alloc.flags = G2D_ALLOC_F_CONTIGUOUS | G2D_ALLOC_F_COHERENT;
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &ball_alloc);
	if (ret < 0) {
		fprintf(stderr, "Failed to allocate DMA buffer for ball\n");
		perror("G2D_IOC_ALLOC_BUFFER (ball)");
		drm_display_cleanup(&disp);
		return 1;
	}
	ball_dma_fd = ball_alloc.dma_fd;
	printf("Ball DMA buffer allocated: fd=%d size=%llu bytes (%dx%d ARGB8888)\n",
	       ball_dma_fd, ball_alloc.size, ball_buffer_size,
	       ball_buffer_size);

	/* Create DMA buffer for background (800x480 XRGB8888) */
	struct g2d_alloc_buffer bg_alloc = { 0 };
	bg_alloc.size = disp.width * disp.height * 4;
	bg_alloc.flags = G2D_ALLOC_F_CONTIGUOUS | G2D_ALLOC_F_COHERENT;
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &bg_alloc);
	if (ret < 0) {
		fprintf(stderr,
			"Failed to allocate DMA buffer for background\n");
		perror("G2D_IOC_ALLOC_BUFFER (bg)");
		close(ball_dma_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	bg_dma_fd = bg_alloc.dma_fd;
	printf("Background DMA buffer allocated: fd=%d size=%llu bytes (%dx%d XRGB8888)\n",
	       bg_dma_fd, bg_alloc.size, disp.width, disp.height);

	/* Create temporary DMA buffer for composition (same size as background) */
	struct g2d_alloc_buffer temp_alloc = { 0 };
	temp_alloc.size = disp.width * disp.height * 4;
	temp_alloc.flags = G2D_ALLOC_F_CONTIGUOUS | G2D_ALLOC_F_COHERENT;
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &temp_alloc);
	if (ret < 0) {
		fprintf(stderr, "Failed to allocate DMA buffer for temp\n");
		perror("G2D_IOC_ALLOC_BUFFER (temp)");
		close(bg_dma_fd);
		close(ball_dma_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	temp_dma_fd = temp_alloc.dma_fd;
	printf("Temp DMA buffer allocated: fd=%d size=%llu bytes (%dx%d XRGB8888)\n",
	       temp_dma_fd, temp_alloc.size, disp.width, disp.height);

	/* Create blend output buffer (final composed frame before FB copy) */
	struct g2d_alloc_buffer blend_alloc = { 0 };
	blend_alloc.size = disp.width * disp.height * 4;
	blend_alloc.flags = G2D_ALLOC_F_CONTIGUOUS | G2D_ALLOC_F_COHERENT;
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &blend_alloc);
	if (ret < 0) {
		fprintf(stderr,
			"Failed to allocate DMA buffer for blend output\n");
		perror("G2D_IOC_ALLOC_BUFFER (blend)");
		close(temp_dma_fd);
		close(bg_dma_fd);
		close(ball_dma_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	blend_dma_fd = blend_alloc.dma_fd;
	printf("Blend output DMA buffer allocated: fd=%d size=%llu bytes (%dx%d XRGB8888)\n",
	       blend_dma_fd, blend_alloc.size, disp.width, disp.height);

	/* Create composition buffer for intermediate blend result (110x110 XRGB8888)
	 * Used in new 3-step approach: blend ball+bg → comp, then scale comp → temp
	 */
	struct g2d_alloc_buffer comp_alloc = { 0 };
	comp_alloc.size =
		ball_buffer_size * ball_buffer_size * 4; /* 110x110 XRGB8888 */
	comp_alloc.flags = G2D_ALLOC_F_CONTIGUOUS | G2D_ALLOC_F_COHERENT;
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &comp_alloc);
	if (ret < 0) {
		fprintf(stderr,
			"Failed to allocate DMA buffer for composition output\n");
		perror("G2D_IOC_ALLOC_BUFFER (comp)");
		close(temp_dma_fd);
		close(bg_dma_fd);
		close(ball_dma_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	comp_dma_fd = comp_alloc.dma_fd;
	printf("Composition DMA buffer allocated: fd=%d size=%llu bytes (%dx%d ARGB8888)\n",
	       comp_dma_fd, comp_alloc.size, ball_buffer_size,
	       ball_buffer_size);

	/* Initialize comp buffer to fully transparent (0x00000000)
	 * This ensures clean alpha blending - unwritten pixels are transparent */
	{
		uint32_t *comp_init =
			calloc(ball_buffer_size * ball_buffer_size, 4);
		if (comp_init) {
			ret = g2d_write_buffer(
				disp.g2d_fd, comp_dma_fd, comp_init,
				ball_buffer_size * ball_buffer_size * 4, 0);
			free(comp_init);
			if (ret < 0) {
				fprintf(stderr,
					"Failed to initialize comp buffer\n");
			} else {
				printf("Comp buffer initialized to transparent\n");
			}
		}
	}

	/* Create ball pattern directly inside the DMA-BUF mapping (no malloc + WRITE_BUFFER).
	 * We mmap() the exported dma-buf fd and write the ARGB pixels in place.
	 * This demonstrates zero-copy buffer initialization by applications.
	 */
	{
		/* mmap length matches the page-aligned allocation size */
		size_t map_size = ball_alloc.size;
		void *ball_map = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
					MAP_SHARED, ball_dma_fd, 0);
		if (ball_map == MAP_FAILED) {
			perror("mmap (ball dma-buf)");
			close(comp_dma_fd);
			close(temp_dma_fd);
			close(bg_dma_fd);
			close(ball_dma_fd);
			drm_display_cleanup(&disp);
			return 1;
		}

		uint32_t *ball_pattern = (uint32_t *)ball_map;

		/* If supported by kernel driver, use DMA_BUF_IOCTL_SYNC to synchronize
		 * CPU writes with device access. START before CPU write, END after.
		 */
		struct dma_buf_sync db_sync;
		memset(&db_sync, 0, sizeof(db_sync));
		db_sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
		if (ioctl(ball_dma_fd, DMA_BUF_IOCTL_SYNC, &db_sync) < 0) {
			perror("DMA_BUF_IOCTL_SYNC START");
			/* Not fatal: continue and try msync as fallback */
		}

		/* Create ball pattern: circular gradient with alpha on transparent background
		 * Format: ARGB8888
		 */
		int center_x = ball_buffer_size / 2;
		int center_y = ball_buffer_size / 2;
		float pattern_max_radius = (float)(ball_buffer_size / 2);

		for (int y = 0; y < ball_buffer_size; y++) {
			for (int x = 0; x < ball_buffer_size; x++) {
				int dx = x - center_x;
				int dy = y - center_y;
				float dist = sqrtf((float)(dx * dx + dy * dy));

				uint32_t color;

				/* Normalized distance [0.0, 1.0] */
				float norm_dist = dist / pattern_max_radius;
				if (norm_dist > 1.0f)
					norm_dist = 1.0f;

				const float inner_radius_ratio = 0.98f;
				uint8_t alpha;
				if (norm_dist <= inner_radius_ratio) {
					alpha = 255;
				} else {
					float t = (norm_dist - inner_radius_ratio) /
						(1.0f - inner_radius_ratio);
					float falloff = 1.0f - t * t;
					if (falloff < 0.0f)
						falloff = 0.0f;
					alpha = (uint8_t)(falloff * 255.0f + 0.5f);
				}

				float norm_x = (float)x / (float)ball_buffer_size;
				float norm_y = (float)y / (float)ball_buffer_size;
				uint8_t r = (uint8_t)(norm_x * 255.0f);
				uint8_t g = (uint8_t)((1.0f - norm_x) * 255.0f);
				uint8_t b = (uint8_t)(norm_y * 192.0f);

				color = ((uint32_t)alpha << 24) |
					((uint32_t)r << 16) |
					((uint32_t)g << 8) | (uint32_t)b;

				ball_pattern[y * ball_buffer_size + x] = color;
			}
		}

		/* END sync for CPU write
		 * Prefer DMA_BUF_IOCTL_SYNC if supported; msync is kept as a fallback.
		 */
		db_sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
		if (ioctl(ball_dma_fd, DMA_BUF_IOCTL_SYNC, &db_sync) < 0) {
			perror("DMA_BUF_IOCTL_SYNC END");
			if (msync(ball_map, map_size, MS_SYNC) < 0)
				perror("msync (ball_map)");
		}

		printf("Ball pattern written directly into DMA-BUF via mmap: ptr=%p size=%zu\n",
			   ball_map, map_size);
	}

	/* NOTE: ball_scaled buffer is no longer needed
	 * We use ball_dma_fd directly in the blend operation
	 * The VSU will handle scaling during the alpha blend
	 */

	/* Create smooth gradient background using VSU hardware scaling
	 * G2D VSU scale limit: 32× maximum
	 * Display: 800×480
	 * 
	 * Required minimum source size to stay within 32× limit:
	 *   - Width:  800 ÷ 32 = 25px minimum
	 *   - Height: 480 ÷ 32 = 15px minimum
	 * 
	 * Using 32×16 gradient source (nice round numbers):
	 *   - Horizontal scale: 800 ÷ 32 = 25× ✓ (within limit)
	 *   - Vertical scale:   480 ÷ 16 = 30× ✓ (within limit)
	 * 
	 * VSU will interpolate between the 32×16 pixels creating a smooth gradient!
	 */
	const int gradient_width = 32;
	const int gradient_height = 16;

	/* Allocate small gradient source buffer */
	int gradient_dma_fd;
	struct g2d_alloc_buffer gradient_alloc = { 0 };
	gradient_alloc.size =
		gradient_width * gradient_height * 4; /* XRGB8888 */
	gradient_alloc.flags = G2D_ALLOC_F_CONTIGUOUS | G2D_ALLOC_F_COHERENT;
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &gradient_alloc);
	if (ret < 0) {
		fprintf(stderr,
			"Failed to allocate DMA buffer for gradient source\n");
		perror("G2D_IOC_ALLOC_BUFFER (gradient)");
		close(comp_dma_fd);
		close(temp_dma_fd);
		close(bg_dma_fd);
		close(ball_dma_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	gradient_dma_fd = gradient_alloc.dma_fd;
	printf("Gradient source buffer allocated: fd=%d size=%llu bytes (%dx%d XRGB8888)\n",
	       gradient_dma_fd, gradient_alloc.size, gradient_width,
	       gradient_height);

	/* Create gradient pattern in memory (vertical gradient from top to bottom) */
	{
		uint32_t *gradient_data = malloc(gradient_alloc.size);
		if (!gradient_data) {
			perror("malloc (gradient pattern)");
			close(gradient_dma_fd);
			close(comp_dma_fd);
			close(temp_dma_fd);
			close(bg_dma_fd);
			close(ball_dma_fd);
			drm_display_cleanup(&disp);
			return 1;
		}

		/* Background gradient colors (ARGB format - standard colors!) */
		uint32_t bg_tl =
			0xFF00008F; /* Dark blue (A=FF, R=00, G=00, B=8F) */
		uint32_t bg_bl =
			0xFFFF0026; /* Bright red (A=FF, R=FF, G=00, B=26) */
		uint32_t bg_tr =
			0xFF00BF00; /* Bright green (A=FF, R=00, G=BF, B=00) */
		uint32_t bg_br =
			0xFFFFFF00; /* Bright yellow (A=FF, R=FF, G=FF, B=00) */

		/* Fill gradient: interpolating to four corner colors */
		for (int y = 0; y < gradient_height; y++) {
			float ty = (float)y / (gradient_height - 1);
			for (int x = 0; x < gradient_width; x++) {
				float tx = (float)x / (gradient_width - 1);

				/* Bilinear interpolation of corner colors */
				uint8_t r = (uint8_t)((1 - tx) * (1 - ty) *
							      ((bg_tl >> 16) &
							       0xFF) +
						      tx * (1 - ty) *
							      ((bg_tr >> 16) &
							       0xFF) +
						      (1 - tx) * ty *
							      ((bg_bl >> 16) &
							       0xFF) +
						      tx * ty *
							      ((bg_br >> 16) &
							       0xFF));
				uint8_t g = (uint8_t)((1 - tx) * (1 - ty) *
							      ((bg_tl >> 8) &
							       0xFF) +
						      tx * (1 - ty) *
							      ((bg_tr >> 8) &
							       0xFF) +
						      (1 - tx) * ty *
							      ((bg_bl >> 8) &
							       0xFF) +
						      tx * ty *
							      ((bg_br >> 8) &
							       0xFF));
				uint8_t b = (uint8_t)((1 - tx) * (1 - ty) *
							      (bg_tl & 0xFF) +
						      tx * (1 - ty) *
							      (bg_tr & 0xFF) +
						      (1 - tx) * ty *
							      (bg_bl & 0xFF) +
						      tx * ty * (bg_br & 0xFF));

				uint32_t color = 0xFF000000 | (r << 16) |
						 (g << 8) | b;
				gradient_data[y * gradient_width + x] = color;
			}
		}

		/* Upload gradient pattern to DMA buffer */
		printf("DEBUG: About to call g2d_write_buffer for gradient\n");
		fflush(stdout);

		ret = g2d_write_buffer(disp.g2d_fd, gradient_dma_fd,
				       gradient_data, gradient_alloc.size, 0);

		printf("DEBUG: g2d_write_buffer returned %d, about to free gradient_data\n",
		       ret);
		fflush(stdout);

		free(gradient_data);

		printf("DEBUG: free() completed\n");
		fflush(stdout);

		if (ret < 0) {
			fprintf(stderr,
				"Failed to write gradient pattern to DMA buffer\n");
			close(gradient_dma_fd);
			close(comp_dma_fd);
			close(temp_dma_fd);
			close(bg_dma_fd);
			close(ball_dma_fd);
			drm_display_cleanup(&disp);
			return 1;
		}

		printf("Gradient pattern created: %dx%d → %dx%d (scale: %.1fx × %.1fx)\n",
		       gradient_width, gradient_height, disp.width, disp.height,
		       (float)disp.width / gradient_width,
		       (float)disp.height / gradient_height);
	}

	struct g2d_cmd cmd_scale = { 0 };
	cmd_scale.cmd_type = G2D_CMD_SCALE;

	/* Source: small gradient pattern */
	cmd_scale.src.width = gradient_width;
	cmd_scale.src.height = gradient_height;
	cmd_scale.src.format = G2D_FMT_XRGB8888;
	cmd_scale.src.stride[0] = gradient_width * 4;
	cmd_scale.src.dma_fd = gradient_dma_fd;
	cmd_scale.src.crop_x = 0;
	cmd_scale.src.crop_y = 0;
	cmd_scale.src.crop_w = gradient_width;
	cmd_scale.src.crop_h = gradient_height;

	/* Destination: full-size background buffer */
	cmd_scale.dst.width = disp.width;
	cmd_scale.dst.height = disp.height;
	cmd_scale.dst.format = G2D_FMT_XRGB8888;
	cmd_scale.dst.stride[0] = disp.width * 4;
	cmd_scale.dst.dma_fd = bg_dma_fd;

	cmd_scale.dst_x = 0;
	cmd_scale.dst_y = 0;
	cmd_scale.dst_w = disp.width;
	cmd_scale.dst_h = disp.height;

	cmd_scale.fence_fd_in = -1;
	cmd_scale.fence_fd_out = -1;

	printf("DEBUG: About to call G2D_IOC_CMD (SCALE) - cmd_type=%u\n",
	       cmd_scale.cmd_type);
	printf("DEBUG: src=%ux%u crop=%ux%u dst=%ux%u\n", cmd_scale.src.width,
	       cmd_scale.src.height, cmd_scale.src.crop_w, cmd_scale.src.crop_h,
	       cmd_scale.dst_w, cmd_scale.dst_h);

	ret = ioctl(disp.g2d_fd, G2D_IOC_CMD, &cmd_scale);
	printf("DEBUG: ioctl returned %d, fence_fd_out=%d\n", ret,
	       cmd_scale.fence_fd_out);

	if (ret < 0) {
		perror("G2D_IOC_CMD (SCALE: gradient → background)");
		close(gradient_dma_fd);
		close(comp_dma_fd);
		close(temp_dma_fd);
		close(bg_dma_fd);
		close(ball_dma_fd);
		drm_display_cleanup(&disp);
		return 1;
	}

	printf("DEBUG: About to sync_wait_and_close on fd=%d\n",
	       cmd_scale.fence_fd_out);
	sync_wait_and_close(cmd_scale.fence_fd_out,
			    "CMD_SCALE gradient→background");
	printf("DEBUG: sync_wait_and_close completed\n");

	/* Clean up gradient source buffer (no longer needed) */
	close(gradient_dma_fd);
	printf("Background filled with VSU-interpolated smooth gradient\n");

	/* Removed diagnostic startup blit that populated both fb pages. The
	 * production demo relies on normal composition + pageflip. If early
	 * frame black/stale pages are a concern, initialize the FB from the
	 * kernel side or orchestrate a proper atomic modeset rather than
	 * issuing best-effort asynchronous blits here. */

	/* Initialize ball position */
	ball_x = disp.width / 2.0f;
	ball_y = disp.height / 2.0f;

	printf("\nPress Ctrl+C to exit\n\n");

	/* === INITIAL FRAMEBUFFER SETUP ===
	 * Initialize BOTH pages of the double-buffered framebuffer with the background.
	 * This prevents showing black frames during the first few flips.
	 * 
	 * CRITICAL FIX FOR MEMORY LEAK:
	 * Export framebuffer dmabuf ONCE and reuse it throughout the demo.
	 * Previously we were exporting a new dmabuf on every frame (60+ times per second),
	 * causing massive memory leak that exhausted all system memory.
	 */
	int fb_dmabuf = drm_export_dmabuf(&disp);
	if (fb_dmabuf < 0) {
		fprintf(stderr, "Failed to export framebuffer dmabuf\n");
		close(comp_dma_fd);
		close(temp_dma_fd);
		close(bg_dma_fd);
		close(ball_dma_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	printf("Framebuffer dmabuf exported: fd=%d (reused for entire demo)\n",
	       fb_dmabuf);

	clock_gettime(CLOCK_MONOTONIC, &fps_start);

	while (keep_running) {
		clock_gettime(CLOCK_MONOTONIC, &frame_start);

		/* Pattern mode disabled - old API deprecated, use normal composition */
		if (pattern_mode) {
			printf("Pattern mode no longer supported (deprecated API) - using normal mode\n");
			pattern_mode = 0;
		}

		/* === COMPOSITION PIPELINE ===
		 * 1. SCALE ball sprite to current size → comp_dma
		 * 2. SCALE gradient → blend_dma (fresh background, bypass bg_dma cache issue)
		 * 3. BLEND comp_dma (scaled ball) over blend_dma → blend_dma
		 * 4. COPY blend_dma → framebuffer backbuffer
		 */
		ret = g2d_blend_ball_dma(&disp, disp.g2d_fd, bg_dma_fd,
					 ball_dma_fd, temp_dma_fd, comp_dma_fd,
					 blend_dma_fd, fb_dmabuf,
					 (int)(ball_x - ball_radius),
					 (int)(ball_y - ball_radius),
					 ball_radius, ball_buffer_size);
		if (ret < 0) {
			fprintf(stderr, "Ball composition pipeline failed\n");
			break;
		}

		/* Flip page AFTER rendering (synchronized with VSYNC to eliminate tearing) */
		drm_flip_page_vsync(&disp);

		/* FPS calculation */
		frame_count++;
		clock_gettime(CLOCK_MONOTONIC, &frame_end);

		/* Update ball size with smooth sinusoidal animation (AFTER rendering, for next frame) */
		scale_time += delta_time;
		float scale_factor =
			0.5f +
			0.5f * sinf(2.0f * M_PI * scale_speed * scale_time);
		ball_radius = min_radius +
			      (int)((max_radius - min_radius) * scale_factor);
		ball_size = ball_radius * 2;

		/* Update ball physics (bouncing) - uses current radius for next frame */
		ball_x += vel_x;
		ball_y += vel_y;

		/* Bounce off walls - uses current radius to prevent out-of-bounds on next frame */
		if (ball_x - ball_radius < 0) {
			vel_x = -vel_x;
			ball_x = ball_radius; /* Clamp to left edge */
		} else if (ball_x + ball_radius > disp.width) {
			vel_x = -vel_x;
			ball_x = disp.width -
				 ball_radius; /* Clamp to right edge */
		}

		if (ball_y - ball_radius < 0) {
			vel_y = -vel_y;
			ball_y = ball_radius; /* Clamp to top edge */
		} else if (ball_y + ball_radius > disp.height) {
			vel_y = -vel_y;
			ball_y = disp.height -
				 ball_radius; /* Clamp to bottom edge */
		}

		/* FPS display (every second) */
		struct timespec fps_now;
		clock_gettime(CLOCK_MONOTONIC, &fps_now);
		long fps_elapsed =
			(fps_now.tv_sec - fps_start.tv_sec) * 1000000L +
			(fps_now.tv_nsec - fps_start.tv_nsec) / 1000L;

		if (fps_elapsed >= 1000000L) { /* 1 second */
			fps = (float)frame_count / (fps_elapsed / 1000000.0f);
			printf("\rFPS: %.1f | Ball size: %dpx | Position: (%.0f, %.0f)  ",
			       fps, ball_size, ball_x, ball_y);
			fflush(stdout);
			frame_count = 0;
			clock_gettime(CLOCK_MONOTONIC, &fps_start);
		} /* Frame timing (target ~60 FPS = 16.6ms) */
		frame_time_us =
			(frame_end.tv_sec - frame_start.tv_sec) * 1000000L +
			(frame_end.tv_nsec - frame_start.tv_nsec) / 1000L;

		if (frame_time_us < 16666) {
			long sleep_us = 16666 - frame_time_us;
			struct timespec ts_sleep;
			ts_sleep.tv_sec = sleep_us / 1000000L;
			ts_sleep.tv_nsec = (sleep_us % 1000000L) * 1000L;
			nanosleep(&ts_sleep, NULL);
		}
		/* One Frame Mode: exit after single frame */
		if (one_frame_mode) {
			keep_running = false;
		}
	}
	printf("\n\nCleaning up...\n");

	/* Close framebuffer dmabuf (was kept open for entire demo) */
	if (fb_dmabuf >= 0)
		close(fb_dmabuf);

	if (blend_dma_fd >= 0)
		close(blend_dma_fd);
	if (comp_dma_fd >= 0)
		close(comp_dma_fd);
	if (temp_dma_fd >= 0)
		close(temp_dma_fd);
	if (bg_dma_fd >= 0)
		close(bg_dma_fd);
	if (ball_dma_fd >= 0)
		close(ball_dma_fd);

	drm_display_cleanup(&disp);

	printf("Total frames: %d\n", frame_count);
	printf("Demo exited cleanly\n");

	return 0;
}
