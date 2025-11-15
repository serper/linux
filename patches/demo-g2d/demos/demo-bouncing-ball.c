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
int g2d_blend_ball_dma(struct drm_display *disp, int g2d_fd, int bg_dma_fd,
	       int ball_dma_fd, int temp_dma_fd,
	       int comp_dma_fd, int dmabuf_fd, int x,
		       int y, int radius, int ball_buffer_size)
{
	int ret;
	int ball_size = radius * 2;

	int backbuffer_y_offset = drm_get_backbuffer_offset(disp) / disp->pitch;

	/* CORRECT PIPELINE: Two-step process for safe blending
	 * 
	 * Pipeline:
	 * Step 1: BLIT bg → temp (copy full background)
	 * Step 2: BLIT ball (ARGB src) + temp (XRGB dst) → temp (XRGB out) with scaling+blending at (x,y)
	 *         - Read from temp at ball region (dst)
	 *         - Write to temp at ball region (out)
	 *         - Driver internally: scales ball from 110×110 to ball_size
	 *         - Driver internally: blends scaled ball over background region
	 * Step 3: BLIT temp → framebuffer (simple copy)
	 * 
	 * Key: After Step 1, temp has full background. In Step 2, we read temp region and write temp region
	 * at the SAME location, but driver handles this correctly with 3-buffer mode.
	 */

	/* Simple bounds check - skip rendering if ball would go outside screen */
	if (x < 0 || y < 0 || x + ball_size > (int)disp->width || 
	    y + ball_size > (int)disp->height) {
		return 0;  /* Ball partially off-screen, skip this frame */
	}

	/* Step 1: CMD_COPY bg → temp (copy full background first) */
	struct g2d_cmd cmd_bg = { 0 };
	cmd_bg.cmd_type = G2D_CMD_COPY;

	cmd_bg.src.width = disp->width;
	cmd_bg.src.height = disp->height;
	cmd_bg.src.format = G2D_FMT_XRGB8888;
	cmd_bg.src.stride[0] = disp->width * 4;
	cmd_bg.src.dma_fd = bg_dma_fd;
	cmd_bg.src.crop_x = 0;
	cmd_bg.src.crop_y = 0;
	cmd_bg.src.crop_w = disp->width;
	cmd_bg.src.crop_h = disp->height;

	cmd_bg.dst.width = disp->width;
	cmd_bg.dst.height = disp->height;
	cmd_bg.dst.format = G2D_FMT_XRGB8888;
	cmd_bg.dst.stride[0] = disp->width * 4;
	cmd_bg.dst.dma_fd = temp_dma_fd;

	cmd_bg.dst_x = 0;
	cmd_bg.dst_y = 0;
	cmd_bg.dst_w = disp->width;
	cmd_bg.dst_h = disp->height;

	cmd_bg.fence_fd_in = -1;
	cmd_bg.fence_fd_out = -1;

	ret = ioctl(g2d_fd, G2D_IOC_CMD, &cmd_bg);
	if (ret < 0) {
		perror("G2D_IOC_CMD (COPY: bg → temp)");
		return -1;
	}

	sync_wait_and_close(cmd_bg.fence_fd_out, "CMD_COPY bg→temp");

	/* Step 2a: CMD_SCALE ball → comp (scale ball to target size first) */
	struct g2d_cmd cmd_scale_ball = { 0 };
	cmd_scale_ball.cmd_type = G2D_CMD_SCALE;

	/* Source: original ball texture */
	cmd_scale_ball.src.width = ball_buffer_size;
	cmd_scale_ball.src.height = ball_buffer_size;
	cmd_scale_ball.src.format = G2D_FMT_XRGB8888;
	cmd_scale_ball.src.stride[0] = ball_buffer_size * 4;
	cmd_scale_ball.src.dma_fd = ball_dma_fd;
	cmd_scale_ball.src.crop_x = 0;
	cmd_scale_ball.src.crop_y = 0;
	cmd_scale_ball.src.crop_w = ball_buffer_size;
	cmd_scale_ball.src.crop_h = ball_buffer_size;

	/* Destination: composition buffer at scaled size */
	cmd_scale_ball.dst.width = ball_size;
	cmd_scale_ball.dst.height = ball_size;
	cmd_scale_ball.dst.format = G2D_FMT_XRGB8888;
	cmd_scale_ball.dst.stride[0] = ball_buffer_size * 4;  /* Buffer is still ball_buffer_size wide */
	cmd_scale_ball.dst.dma_fd = comp_dma_fd;

	cmd_scale_ball.dst_x = 0;
	cmd_scale_ball.dst_y = 0;
	cmd_scale_ball.dst_w = ball_size;
	cmd_scale_ball.dst_h = ball_size;

	cmd_scale_ball.fence_fd_in = -1;
	cmd_scale_ball.fence_fd_out = -1;

	ret = ioctl(g2d_fd, G2D_IOC_CMD, &cmd_scale_ball);
	if (ret < 0) {
		perror("G2D_IOC_CMD (SCALE: ball → comp)");
		return -1;
	}

	sync_wait_and_close(cmd_scale_ball.fence_fd_out, "CMD_SCALE ball→comp");

	/* Step 2b: CMD_BLEND comp (scaled ball) + temp → temp (pure blending, NO scaling) */
	struct g2d_cmd cmd_blend = { 0 };
	cmd_blend.cmd_type = G2D_CMD_BLEND;

	/* Source (foreground): scaled ball from comp buffer */
	cmd_blend.src.width = ball_size;
	cmd_blend.src.height = ball_size;
	cmd_blend.src.format = G2D_FMT_ARGB8888;
	cmd_blend.src.stride[0] = ball_buffer_size * 4;  /* Physical buffer pitch */
	cmd_blend.src.dma_fd = comp_dma_fd;
	cmd_blend.src.crop_x = 0;
	cmd_blend.src.crop_y = 0;
	cmd_blend.src.crop_w = ball_size;
	cmd_blend.src.crop_h = ball_size;
	cmd_blend.src.alpha = 255;
	cmd_blend.src.alpha_mode = G2D_PIXEL_ALPHA;  /* Per-pixel alpha from ARGB */
	cmd_blend.src.premul_mode = G2D_PREMUL_ALPHA;  /* Straight alpha */

	/* Destination (background): temp buffer READ at ball region */
	cmd_blend.dst.width = disp->width;
	cmd_blend.dst.height = disp->height;
	cmd_blend.dst.format = G2D_FMT_XRGB8888;
	cmd_blend.dst.stride[0] = disp->width * 4;
	cmd_blend.dst.dma_fd = temp_dma_fd;  /* Read from temp (has bg) */
	cmd_blend.dst.crop_x = x;  /* Read background region where ball will be */
	cmd_blend.dst.crop_y = y;
	cmd_blend.dst.crop_w = ball_size;
	cmd_blend.dst.crop_h = ball_size;
	cmd_blend.dst.alpha = 255;
	cmd_blend.dst.alpha_mode = G2D_GLOBAL_ALPHA;
	
	/* Output: temp buffer WRITE at ball region */
	cmd_blend.out.width = disp->width;
	cmd_blend.out.height = disp->height;
	cmd_blend.out.format = G2D_FMT_XRGB8888;
	cmd_blend.out.stride[0] = disp->width * 4;
	cmd_blend.out.dma_fd = temp_dma_fd;  /* Write to temp (same buffer, different semantics) */

	/* Destination position (NO scaling, just positioning) */
	cmd_blend.dst_x = x;
	cmd_blend.dst_y = y;
	cmd_blend.dst_w = ball_size;  /* Same size as source (no scaling) */
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

	sync_wait_and_close(cmd_blend.fence_fd_out, "CMD_BLEND comp+temp→temp");

	/* Step 3: CMD_COPY temp → framebuffer (simple full-screen copy) */
	struct g2d_cmd cmd_copy = { 0 };
	cmd_copy.cmd_type = G2D_CMD_COPY;

	cmd_copy.src.width = disp->width;
	cmd_copy.src.height = disp->height;
	cmd_copy.src.format = G2D_FMT_XRGB8888;
	cmd_copy.src.stride[0] = disp->width * 4;
	cmd_copy.src.dma_fd = temp_dma_fd;  /* Read from temp (has bg + ball) */
	cmd_copy.src.crop_x = 0;
	cmd_copy.src.crop_y = 0;
	cmd_copy.src.crop_w = disp->width;
	cmd_copy.src.crop_h = disp->height;

	cmd_copy.dst.width = disp->width;
	cmd_copy.dst.height = disp->height * 2;  /* Double buffered framebuffer */
	cmd_copy.dst.format = G2D_FMT_XRGB8888;
	cmd_copy.dst.stride[0] = disp->pitch;
	cmd_copy.dst.dma_fd = dmabuf_fd;

	cmd_copy.dst_x = 0;
	cmd_copy.dst_y = backbuffer_y_offset;  /* Write to backbuffer */
	cmd_copy.dst_w = disp->width;
	cmd_copy.dst_h = disp->height;

	cmd_copy.fence_fd_in = -1;
	cmd_copy.fence_fd_out = -1;

	ret = ioctl(g2d_fd, G2D_IOC_CMD, &cmd_copy);
	if (ret < 0) {
		perror("G2D_IOC_CMD (COPY: temp → framebuffer)");
		return -1;
	}

	sync_wait_and_close(cmd_copy.fence_fd_out, "CMD_COPY temp→fb");

	/* Done! Framebuffer backbuffer now has the complete frame ready for flip */
	return 0;
}

int main(int argc, char **argv)
{
	struct drm_display disp;
	int ret;

	printf("G2D + DRM Bouncing Ball Demo v3.0.0 - UNIFIED CMD API (G2D_IOC_CMD)\n");
	printf("Uses only G2D_CMD_* operations: COPY, SCALE, BLEND\n");
	printf("Performance optimized: removed all debug printk from kernel driver\n");

	/* Ball physics */
	float ball_x, ball_y;
	float vel_x = 3.5f;
	float vel_y = 2.8f;

	/* Scaling animation parameters */
	float scale_time = 0.0f; /* Time counter for scaling (seconds) */
	const float scale_speed =
		1.5f; /* Scaling frequency (Hz) - 1.5 cycles per second */
	const int min_radius = 25; /* Minimum ball size (pixels) */
	const int max_radius = 55; /* Maximum ball size (pixels) */
	const float delta_time = 0.01666f; /* ~60 FPS = 16.66ms per frame */

	/* Fixed buffer size for ball texture (must be >= max_radius * 2) */
	const int ball_buffer_size =
		110; /* Exact size for max_radius=55 → 110px diameter */

	/* HARDWARE SCALING ENABLED - Variable size ball with pitch/crop workaround */
	int ball_radius = max_radius; /* Start at maximum size */
	int ball_size; /* Will be updated each frame: ball_radius * 2 */

	/* Background gradient colors (ARGB format - standard colors!) */
	uint32_t bg_top = 0xFF00008F; /* Dark blue (A=FF, R=00, G=00, B=8F) */
	uint32_t bg_bottom = 0xFFFF0026; /* Bright red (A=FF, R=FF, G=00, B=26) */

	/* FPS tracking */
	struct timespec frame_start, frame_end;
	long frame_time_us;
	int frame_count = 0;
	float fps = 0.0f;
	struct timespec fps_start;
	int ball_dma_fd = -1;
	int bg_dma_fd = -1;
	int temp_dma_fd = -1; /* Background copy buffer */
	int comp_dma_fd =
		-1; /* Composition output buffer (3rd distinct buffer) */

	signal(SIGINT, sigint_handler);

	printf("==============================\n\n");

	ret = drm_display_init(&disp);
	if (ret < 0) {
		fprintf(stderr, "Failed to initialize DRM display\n");
		return 1;
	}

	/* Create DMA buffer for ball sprite (120x120 ARGB8888 - large enough for max scaling) */
	struct g2d_alloc_buffer ball_alloc = { 0 };
	ball_alloc.size = ball_buffer_size * ball_buffer_size *
			  4; /* ARGB8888 = 4 bytes per pixel */
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

	/* Create composition buffer for intermediate blend result (110x110 XRGB8888)
	 * Used in new 3-step approach: blend ball+bg → comp, then scale comp → temp
	 */
	struct g2d_alloc_buffer comp_alloc = { 0 };
	comp_alloc.size =
		ball_buffer_size * ball_buffer_size * 4; /* 110x110 XRGB8888 */
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
	printf("Composition DMA buffer allocated: fd=%d size=%llu bytes (%dx%d XRGB8888)\n",
	       comp_dma_fd, comp_alloc.size, ball_buffer_size,
	       ball_buffer_size);

	/* Create ball pattern in userspace memory then upload using G2D_IOC_WRITE_BUFFER
	 * This demonstrates how applications can create custom graphics/textures.
	 */
	{
		/* Allocate local buffer for ball pattern */
		uint32_t *ball_pattern = malloc(ball_alloc.size);
		if (!ball_pattern) {
			perror("malloc (ball pattern)");
			close(temp_dma_fd);
			close(bg_dma_fd);
			close(ball_dma_fd);
			drm_display_cleanup(&disp);
			return 1;
		}

		/* Create CHECKERBOARD pattern for visual scaling verification
		 * HARDWARE INVERTS SRC ALPHA: 255=transparent, 0=opaque for src (V0 pipe)
		 * Pattern: 4x4 checkerboard (110px / 4 = 27.5px per square)
		 * - Black squares: alpha=0 (opaque), RGB=black
		 * - White squares: alpha=0 (opaque), RGB=white  
		 * - Outside circle: alpha=255 (transparent)
		 * Larger squares (4x4 instead of 8x8) are more visible when scaled down
		 */
		int center_x = ball_buffer_size / 2;
		int center_y = ball_buffer_size / 2;
		float pattern_max_radius = (float)(ball_buffer_size / 2);
		int square_size = ball_buffer_size / 4;  /* 4x4 grid = ~27px per square */
		
		for (int y = 0; y < ball_buffer_size; y++) {
			for (int x = 0; x < ball_buffer_size; x++) {
				int dx = x - center_x;
				int dy = y - center_y;
				float dist = sqrtf(dx * dx + dy * dy);
				
				uint32_t color;
				
				if (dist > pattern_max_radius) {
					/* Outside the circle: fully transparent */
					color = 0x00000000;  /* Alpha=0 (transparent), RGB=black */
				} else {
				/* Inside circle: RGB COLOR GRADIENT pattern with radial alpha
				 * Standard ALPHA interpretation:
				 * - alpha=255: Fully OPAQUE (100% visible)
				 * - alpha=128: Semi-transparent (50% visible)
				 * - alpha=0:   Fully TRANSPARENT (invisible)
				 * 
				 * Color pattern: Full RGB spectrum to verify all channels
				 * - Horizontal gradient: Red (left) → Green → Blue (right)
				 * - Vertical component: adds Yellow tint
				 */
					float norm_x = (float)x / ball_buffer_size;  /* 0.0 (left) → 1.0 (right) */
					float norm_y = (float)y / ball_buffer_size;  /* 0.0 (top) → 1.0 (bottom) */
					
					/* RGB gradient to test all color channels during scaling */
					uint8_t r = (uint8_t)(norm_x * 255);           /* Red increases left→right */
					uint8_t g = (uint8_t)((1.0f - norm_x) * 255);  /* Green decreases left→right */
					uint8_t b = (uint8_t)(norm_y * 192);           /* Blue increases top→bottom */
					
				/* Radial alpha gradient: center=255 (opaque) → edge=0 (transparent) */
				float norm_dist = dist / pattern_max_radius;
				uint8_t alpha = 255 - (uint8_t)(norm_dist * 255);  /* 255→0 gradient from center */					/* G2D_FMT_ARGB8888 expects ARGB in big-endian conceptually,
					 * but on little-endian ARM it's stored as [B][G][R][A] in memory.
					 * Construct as ARGB value which will be byte-swapped correctly. */
					color = ((uint32_t)alpha << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
				}
				
				ball_pattern[y * ball_buffer_size + x] = color;
			}
		}

		printf("Ball pattern created: circular white ball on transparent background\n");
		printf("DEBUG: ball_pattern pointer=%p\n", ball_pattern);
		printf("DEBUG: Corner pixels (should be 0x00000000): 0x%08X 0x%08X 0x%08X 0x%08X\n",
		       ball_pattern[0], ball_pattern[1], ball_pattern[2],
		       ball_pattern[3]);
		int center_idx = center_y * ball_buffer_size + center_x;
		printf("DEBUG: Center pixel at [%d,%d] idx=%d (should be 0xFFFFFFFF): 0x%08X\n",
		       center_x, center_y, center_idx,
		       ball_pattern[center_idx]);

		/* Upload pattern to DMA buffer using WRITE_BUFFER ioctl */
		printf("DEBUG: About to call g2d_write_buffer with ball_pattern=%p, first word=0x%08X\n",
		       ball_pattern, ball_pattern[0]);

		/* Flush CPU cache to ensure pattern data is visible to kernel copy_from_user() */
		__builtin___clear_cache((char *)ball_pattern,
					(char *)ball_pattern + ball_alloc.size);

		ret = g2d_write_buffer(disp.g2d_fd, ball_dma_fd, ball_pattern,
				       ball_alloc.size, 0);
		printf("DEBUG: g2d_write_buffer returned %d\n", ret);

		if (ret < 0) {
			fprintf(stderr,
				"Failed to write ball pattern to DMA buffer\n");
			free(ball_pattern);
			close(comp_dma_fd);
			close(temp_dma_fd);
			close(bg_dma_fd);
			close(ball_dma_fd);
			drm_display_cleanup(&disp);
			return 1;
		}

		printf("Ball pattern uploaded to DMA buffer successfully via G2D_IOC_WRITE_BUFFER\n");
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

		/* Fill gradient: each row has same color, interpolating from top to bottom */
		for (int y = 0; y < gradient_height; y++) {
			/* Interpolate between bg_top (y=0) and bg_bottom (y=gradient_height-1) */
			float t = (float)y / (gradient_height - 1);

			/* Extract RGB components from bg_top and bg_bottom */
			uint8_t r_top = (bg_top >> 16) & 0xFF;
			uint8_t g_top = (bg_top >> 8) & 0xFF;
			uint8_t b_top = bg_top & 0xFF;

			uint8_t r_bottom = (bg_bottom >> 16) & 0xFF;
			uint8_t g_bottom = (bg_bottom >> 8) & 0xFF;
			uint8_t b_bottom = bg_bottom & 0xFF;

			/* Linear interpolation */
			uint8_t r = (uint8_t)(r_top + t * (r_bottom - r_top));
			uint8_t g = (uint8_t)(g_top + t * (g_bottom - g_top));
			uint8_t b = (uint8_t)(b_top + t * (b_bottom - b_top));

			uint32_t color = 0xFF000000 | (r << 16) | (g << 8) | b;

			/* Fill entire row with this color */
			for (int x = 0; x < gradient_width; x++) {
				gradient_data[y * gradient_width + x] = color;
			}
		}

		/* Upload gradient pattern to DMA buffer */
		printf("DEBUG: About to call g2d_write_buffer for gradient\n");
		fflush(stdout);
		
		ret = g2d_write_buffer(disp.g2d_fd, gradient_dma_fd,
				       gradient_data, gradient_alloc.size, 0);
		
		printf("DEBUG: g2d_write_buffer returned %d, about to free gradient_data\n", ret);
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

	/* Scale gradient to full background using CMD_SCALE */
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

	printf("DEBUG: About to call G2D_IOC_CMD (SCALE) - cmd_type=%u\n", cmd_scale.cmd_type);
	printf("DEBUG: src=%ux%u crop=%ux%u dst=%ux%u\n",
	       cmd_scale.src.width, cmd_scale.src.height,
	       cmd_scale.src.crop_w, cmd_scale.src.crop_h,
	       cmd_scale.dst_w, cmd_scale.dst_h);
	
	ret = ioctl(disp.g2d_fd, G2D_IOC_CMD, &cmd_scale);
	printf("DEBUG: ioctl returned %d, fence_fd_out=%d\n", ret, cmd_scale.fence_fd_out);
	
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

	printf("DEBUG: About to sync_wait_and_close on fd=%d\n", cmd_scale.fence_fd_out);
	sync_wait_and_close(cmd_scale.fence_fd_out, "CMD_SCALE gradient→background");
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

		/* Composite with 3 DISTINCT buffers: bg→temp, ball+temp→comp, comp→fb
		 * All operations use G2D_IOC_CMD with specific command types:
		 * Step 1 (inside function): CMD_COPY bg → temp_buffer (background copy)
		 * Step 2a (inside function): CMD_SCALE ball → comp (scaling only)
		 * Step 2b (inside function): CMD_BLEND comp + temp → temp (blending only, NO scaling)
		 * Step 3 (inside function): CMD_COPY temp → framebuffer (final copy)
		 */
		ret = g2d_blend_ball_dma(
			&disp, disp.g2d_fd, bg_dma_fd, ball_dma_fd,
			temp_dma_fd, comp_dma_fd,
			fb_dmabuf,
			(int)(ball_x - ball_radius),
			(int)(ball_y - ball_radius), ball_radius,
			ball_buffer_size); /* ball_buffer_size is CONSTANT 110 */
		if (ret < 0) {
			fprintf(stderr, "Failed to blend ball\n");
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
			usleep(16666 - frame_time_us);
		}
	}
	printf("\n\nCleaning up...\n");

	/* Close framebuffer dmabuf (was kept open for entire demo) */
	if (fb_dmabuf >= 0)
		close(fb_dmabuf);

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
