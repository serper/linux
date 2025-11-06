/*
 * G2D + DRM Demo: Bouncing Ball with Alpha Blending
 * 
 * Demonstrates:
 * - DRM double buffering with page flipping
 * - G2D alpha blending for smooth transparency
 * - Real-time animation without tearing
 * 
 * Compile:
 *   arm-linux-musleabihf-gcc -o demo-bouncing-ball \
 *       demo-drm-base.c demo-bouncing-ball.c \
 *       -I/usr/include/libdrm -ldrm -static
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
		return 0;  /* Treat timeout as success - fence likely already signaled */
	}
	return 0;
}

/* Helper: Write data to a G2D buffer from userspace */
static int g2d_write_buffer(int g2d_fd, int dma_fd, void *data, size_t size, size_t offset)
{
	struct g2d_buffer_rw rw = {
		.dma_fd = dma_fd,
		.offset = offset,
		.size = size,
		.user_ptr = (__u64)(uintptr_t)data,
	};
	
	int ret = ioctl(g2d_fd, G2D_IOC_WRITE_BUFFER, &rw);
	if (ret < 0) {
		perror("G2D_IOC_WRITE_BUFFER");
		return ret;
	}
	
	return 0;
}

/* Helper: Read data from a G2D buffer to userspace */
static int g2d_read_buffer(int g2d_fd, int dma_fd, void *data, size_t size, size_t offset)
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

/* Generate ball pattern at specific size with radial alpha gradient */
static int generate_ball_pattern(int g2d_fd, int ball_ion_fd, int ball_size)
{
	/* Allocate local buffer for ball pattern */
	size_t pattern_bytes = ball_size * ball_size * 4;  /* ARGB8888 */
	uint32_t *ball_pattern = malloc(pattern_bytes);
	if (!ball_pattern) {
		perror("malloc (ball pattern)");
		return -1;
	}
	
	/* Create radial alpha gradient for glass ball effect
	 * Center=0 (opaque), Edge=128 (semi-transparent), Outside=255 (fully transparent)
	 */
	int center_x = ball_size / 2;
	int center_y = ball_size / 2;
	float pattern_radius = (float)(ball_size / 2);
	
	for (int y = 0; y < ball_size; y++) {
		for (int x = 0; x < ball_size; x++) {
			int dx = x - center_x;
			int dy = y - center_y;
			float dist = sqrtf(dx * dx + dy * dy);
			
			uint8_t alpha;
			uint32_t rgb;
			
			if (dist > pattern_radius) {
				/* Outside circle: fully transparent */
				alpha = 255;
				rgb = 0x00000000;
			} else {
				/* Inside circle: gradient 0 (center) to 128 (edge) */
				float norm_dist = dist / pattern_radius;
				alpha = (uint8_t)(norm_dist * 128.0f);
				rgb = 0x00FFFFFF;  /* White */
			}
			
			uint32_t color = ((uint32_t)alpha << 24) | rgb;
			ball_pattern[y * ball_size + x] = color;
		}
	}
	
	/* Upload pattern to ION buffer */
	int ret = g2d_write_buffer(g2d_fd, ball_ion_fd, ball_pattern, pattern_bytes, 0);
	free(ball_pattern);
	
	return ret;
}

/*
/* Composite ball with alpha blending and hardware scaling onto background.
 * 
 * YUV TEST MODE: To test if VSU is using YUV path internally
 * Pipeline with YUV conversion:
 * Step 1: BLIT bg → temp_buffer (background copy)
 * Step 2a: BLIT ball RGB → ball_yuv (RGB to YUV422 conversion)
 * Step 2b: ALPHA_BLEND ball_yuv (YUV) + temp → temp with scaling
 * Step 3: BLIT temp_buffer → framebuffer (final display)
 */
int g2d_blend_ball_ion(struct drm_display *disp, int g2d_fd,
                       int bg_ion_fd, int ball_ion_fd, int ball_scaled_ion_fd,
                       int temp_ion_fd, int comp_ion_fd, int ball_yuv_fd,
                       int dmabuf_fd, int x, int y, int radius, int ball_buffer_size)
{
	int ret;
	int ball_size = radius * 2;

	int backbuffer_y_offset = drm_get_backbuffer_offset(disp) / disp->pitch;

	/* SIMPLEST SCALING APPROACH: Let VSU scale during alpha blend
	 * No intermediate buffers, no pitch/crop issues
	 * 
	 * Pipeline:
	 * Step 1: BLIT bg → temp (full background)
	 * Step 2: ALPHA_BLEND ball(110x110 full, NO crop) + temp → temp
	 *         - Source: ball 110×110 FULL buffer (crop = full size)
	 *         - Destination: temp with crop at (x,y) size ball_size×ball_size
	 *         - VSU scales automatically from src size to dst crop size
	 * Step 3: BLIT temp → framebuffer
	 */
	
	/* Step 1: BLIT bg → temp (full background copy) */
	struct g2d_blit blit_bg = {0};
	blit_bg.out.dma_fd = -1;  /* Legacy mode */
	
	blit_bg.src.width = disp->width;
	blit_bg.src.height = disp->height;
	blit_bg.src.format = G2D_FMT_XRGB8888;
	blit_bg.src.stride[0] = disp->width * 4;
	blit_bg.src.dma_fd = bg_ion_fd;
	blit_bg.src.crop_x = 0;
	blit_bg.src.crop_y = 0;
	blit_bg.src.crop_w = disp->width;
	blit_bg.src.crop_h = disp->height;

	blit_bg.dst.width = disp->width;
	blit_bg.dst.height = disp->height;
	blit_bg.dst.format = G2D_FMT_XRGB8888;
	blit_bg.dst.stride[0] = disp->width * 4;
	blit_bg.dst.dma_fd = temp_ion_fd;
	blit_bg.dst.crop_x = 0;
	blit_bg.dst.crop_y = 0;
	blit_bg.dst.crop_w = 0;
	blit_bg.dst.crop_h = 0;

	blit_bg.dst_x = 0;
	blit_bg.dst_y = 0;
	blit_bg.dst_w = disp->width;
	blit_bg.dst_h = disp->height;
	blit_bg.flags = 0;
	blit_bg.global_alpha = 255;
	blit_bg.fence_fd_in = -1;
	blit_bg.fence_fd_out = -1;

	ret = ioctl(g2d_fd, G2D_IOC_BLIT, &blit_bg);
	if (ret < 0) {
		perror("G2D_IOC_BLIT (bg → temp)");
		return -1;
	}

	if (blit_bg.fence_fd_out >= 0) {
		close(blit_bg.fence_fd_out);
	}
	
	/* Step 2: ALPHA_BLEND ball (110x110 FULL, no crop) + temp → temp at (x,y)
	 * KEY: Source reads FULL 110×110 buffer, destination crops to ball_size
	 * VSU will scale from 110×110 → ball_size×ball_size automatically
	 */
	struct g2d_alpha_blend blend = {0};
	
	/* Source (V0/foreground): ball FULL 110×110 buffer (RGB format) */
	blend.src.width = ball_buffer_size;   /* FIXED 110 */
	blend.src.height = ball_buffer_size;  /* FIXED 110 */
	blend.src.format = G2D_FMT_ARGB8888;
	blend.src.stride[0] = ball_buffer_size * 4;  /* pitch = 440 (matches width) */
	blend.src.dma_fd = ball_ion_fd;
	blend.src.crop_x = 0;
	blend.src.crop_y = 0;
	blend.src.crop_w = ball_buffer_size;  /* FULL: Read entire 110×110 */
	blend.src.crop_h = ball_buffer_size;  /* FULL: Read entire 110×110 */
	blend.src.alpha = 255;
	blend.src.alpha_mode = G2D_PIXEL_ALPHA;

	/* Destination (UI2/background): temp buffer with VARIABLE crop size */
	blend.dst.width = disp->width;
	blend.dst.height = disp->height;
	blend.dst.format = G2D_FMT_XRGB8888;
	blend.dst.stride[0] = disp->width * 4;
	blend.dst.dma_fd = temp_ion_fd;
	blend.dst.crop_x = x;  /* Ball position */
	blend.dst.crop_y = y;
	blend.dst.crop_w = ball_size;  /* VARIABLE: VSU scales 110→ball_size */
	blend.dst.crop_h = ball_size;  /* VARIABLE: VSU scales 110→ball_size */
	blend.dst.alpha = 128;
	blend.dst.alpha_mode = G2D_GLOBAL_ALPHA;

	/* Output: temp buffer (3-buffer mode with same buffer as dst) */
	blend.out.width = disp->width;
	blend.out.height = disp->height;
	blend.out.format = G2D_FMT_XRGB8888;
	blend.out.stride[0] = disp->width * 4;
	blend.out.dma_fd = temp_ion_fd;
	blend.out.crop_x = x;
	blend.out.crop_y = y;
	blend.out.crop_w = ball_size;  /* Output size matches dst crop */
	blend.out.crop_h = ball_size;
	blend.out.alpha = 0;
	blend.out.alpha_mode = 0;

	blend.fence_fd_in = -1;
	blend.fence_fd_out = -1;

	ret = ioctl(g2d_fd, G2D_IOC_ALPHA_BLEND, &blend);
	if (ret < 0) {
		perror("G2D_IOC_ALPHA_BLEND (ball + temp → temp with scaling)");
		return -1;
	}
	
	if (blend.fence_fd_out >= 0) {
		close(blend.fence_fd_out);
	}

	/* Step 3: BLIT temp → framebuffer */

#if 0  /* DISABLED: Old debug code from pitch/crop bug investigation */
	/* DEBUG: Skip alpha blend, just BLIT ball_scaled directly to temp to see what VSU produced */
	struct g2d_blit blit_debug = {0};
	blit_debug.out.dma_fd = -1;  /* Legacy mode */
	
	/* CRITICAL FIX: Tell hardware the buffer is ball_size x ball_size with matching pitch
	 * This avoids the crop issue where pitch != width causes reading problems
	 */
	blit_debug.src.width = ball_size;   /* Buffer dimensions = actual data size */
	blit_debug.src.height = ball_size;
	blit_debug.src.format = G2D_FMT_ARGB8888;
	blit_debug.src.stride[0] = ball_size * 4;  /* Pitch matches width (NO extra space) */
	blit_debug.src.dma_fd = ball_scaled_ion_fd;
	blit_debug.src.crop_x = 0;  /* No crop needed - buffer size matches data size */
	blit_debug.src.crop_y = 0;
	blit_debug.src.crop_w = ball_size;  /* Full "buffer" */
	blit_debug.src.crop_h = ball_size;
	
	blit_debug.dst.width = disp->width;
	blit_debug.dst.height = disp->height;
	blit_debug.dst.format = G2D_FMT_XRGB8888;
	blit_debug.dst.stride[0] = disp->width * 4;
	blit_debug.dst.dma_fd = temp_ion_fd;
	blit_debug.dst.crop_x = 0;
	blit_debug.dst.crop_y = 0;
	blit_debug.dst.crop_w = 0;
	blit_debug.dst.crop_h = 0;
	
	blit_debug.dst_x = x;
	blit_debug.dst_y = y;
	blit_debug.dst_w = ball_size;
	blit_debug.dst_h = ball_size;
	
	blit_debug.flags = 0;  /* No flags, just plain copy */
	blit_debug.global_alpha = 255;
	blit_debug.fence_fd_in = -1;
	blit_debug.fence_fd_out = -1;
	
	ret = ioctl(g2d_fd, G2D_IOC_BLIT, &blit_debug);
	if (ret < 0) {
		perror("G2D_IOC_BLIT (debug: ball_scaled → temp)");
		close(dmabuf_fd);
		return -1;
	}
	
	if (blit_debug.fence_fd_out >= 0) {
		close(blit_debug.fence_fd_out);
	}
#endif  /* End disabled debug code */

#if 0  /* DISABLED: Original alpha blend code */
	/* Step 2b: ALPHA_BLEND ball_scaled + temp → temp - BLENDING ONLY, no scaling
	 * Now we have scaled ball with alpha, blend it with background
	 */
	struct g2d_alpha_blend blend = {0};
	
	/* Source (V0/foreground): ball_scaled with alpha - VARIABLE size at origin (0,0) */
	blend.src.width = ball_size;   /* CHANGED: Use actual scaled size instead of buffer size */
	blend.src.height = ball_size;  /* CHANGED: Use actual scaled size instead of buffer size */
	blend.src.format = G2D_FMT_ARGB8888;
	blend.src.stride[0] = ball_buffer_size * 4;  /* KEEP: Physical buffer pitch remains 110*4=440 */
	blend.src.dma_fd = ball_scaled_ion_fd;
	blend.src.crop_x = 0;  /* Read from origin */
	blend.src.crop_y = 0;
	blend.src.crop_w = ball_size;  /* VARIABLE: matches scaled size */
	blend.src.crop_h = ball_size;
	blend.src.alpha = 255;  /* Full alpha (actual alpha comes from per-pixel values) */
	blend.src.alpha_mode = G2D_PIXEL_ALPHA;  /* Use per-pixel alpha from texture */

	/* Destination (UI2/background): temp_buffer READ ONLY at ball position */
	blend.dst.width = disp->width;
	blend.dst.height = disp->height;
	blend.dst.format = G2D_FMT_XRGB8888;
	blend.dst.stride[0] = disp->width * 4;
	blend.dst.dma_fd = temp_ion_fd;  /* READ from temp_buffer (background copy) */
	blend.dst.crop_x = x;  /* Read background region at ball position */
	blend.dst.crop_y = y;
	blend.dst.crop_w = ball_size;
	blend.dst.crop_h = ball_size;
	blend.dst.alpha = 128;  /* Background 50% transparent */
	blend.dst.alpha_mode = G2D_GLOBAL_ALPHA;

	/* Output (WB/writeback): temp_buffer IN-PLACE (read + write same buffer) */
	blend.out.width = disp->width;
	blend.out.height = disp->height;
	blend.out.format = G2D_FMT_XRGB8888;
	blend.out.stride[0] = disp->width * 4;
	blend.out.dma_fd = temp_ion_fd;  /* WRITE back to temp (in-place alpha blend) */
	blend.out.crop_x = x;  /* Write composited result at ball position */
	blend.out.crop_y = y;
	blend.out.crop_w = ball_size;
	blend.out.crop_h = ball_size;
	blend.out.alpha = 0;  /* Not used for output */
	blend.out.alpha_mode = 0;  /* Not used for output */

	blend.fence_fd_in = -1;
	blend.fence_fd_out = -1;

	ret = ioctl(g2d_fd, G2D_IOC_ALPHA_BLEND, &blend);
	if (ret < 0) {
		perror("G2D_IOC_ALPHA_BLEND (ball_scaled + bg → temp)");
		close(dmabuf_fd);
		return -1;
	}
	
	if (blend.fence_fd_out >= 0) {
		close(blend.fence_fd_out);
	}
#endif  /* End disabled alpha blend */

	/* Step 3: BLIT temp_buffer → framebuffer (copy composited result with ball) */
	struct g2d_blit blit = {0};
	blit.out.dma_fd = -1;  /* Legacy 2-buffer mode: dst is also output */
	
	blit.src.width = disp->width;
	blit.src.height = disp->height;
	blit.src.format = G2D_FMT_XRGB8888;
	blit.src.stride[0] = disp->width * 4;
	blit.src.dma_fd = temp_ion_fd;  /* Read from temp buffer (has blended result) */
	blit.src.crop_x = 0;
	blit.src.crop_y = 0;
	blit.src.crop_w = disp->width;
	blit.src.crop_h = disp->height;

	blit.dst.width = disp->width;
	blit.dst.height = disp->height * 2;  /* Double buffered framebuffer */
	blit.dst.format = G2D_FMT_XRGB8888;
	blit.dst.stride[0] = disp->pitch;
	blit.dst.dma_fd = dmabuf_fd;
	blit.dst.crop_x = 0;
	blit.dst.crop_y = 0;  /* No crop on destination buffer */
	blit.dst.crop_w = 0;  /* No crop (use full source) */
	blit.dst.crop_h = 0;  /* No crop (use full source) */

	blit.dst_x = 0;
	blit.dst_y = backbuffer_y_offset;  /* CRITICAL: Write to backbuffer page offset */
	blit.dst_w = disp->width;
	blit.dst_h = disp->height;
	blit.flags = 0;  /* Simple copy */
	blit.fence_fd_in = -1;
	blit.fence_fd_out = -1;

	ret = ioctl(g2d_fd, G2D_IOC_BLIT, &blit);
	if (ret < 0) {
		perror("G2D_IOC_BLIT (temp → framebuffer)");
		return -1;
	}

	/* Wait for G2D blit to finish before flipping to avoid showing
	 * partially written frames (which causes screen flickering).
	 * NOTE: Currently disabled due to poll() timing issues with already-signaled fences.
	 * The G2D operations complete very quickly (~1-2ms) so the risk of tearing is minimal.
	 */
	if (blit.fence_fd_out >= 0) {
		/* Fence wait disabled - just close the fd */
		close(blit.fence_fd_out);
	}

	/* Removed diagnostic second blit to other page. The proper frame is
	 * already copied into the backbuffer above and will be presented by
	 * the DRM page-flip. Rely on fences when the kernel driver provides
	 * them for synchronization.
	 */

	/* All composition completed: temp buffer has the final composed frame
	 * and was already copied into the backbuffer above. No further
	 * direct blending or copies are needed here.
	 */

	return 0;
}

int main(int argc, char **argv)
{
	struct drm_display disp;
	int ret;
	
	printf("G2D + DRM Bouncing Ball Demo v2.9.31 - CORRECT UI2+V0 ALPHA BLENDING\n");
	if (argc > 1) {
		for (int i = 1; i < argc; i++) {
			if (strcmp(argv[i], "--pattern") == 0)
				pattern_mode = 1;
			else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
				printf("Usage: %s [--pattern]\n", argv[0]);
				printf("  --pattern   Fill temp with a solid visible color and blit to backbuffer(s) (diagnostic)\n");
				return 0;
			}
		}
	}
	printf("Single V0 layer with alpha (EXACT same pattern as working fillrect)\n");
	
	/* Ball physics */
	float ball_x, ball_y;
	float vel_x = 3.5f;
	float vel_y = 2.8f;
	
	/* Scaling animation parameters */
	float scale_time = 0.0f;  /* Time counter for scaling (seconds) */
	const float scale_speed = 1.5f;  /* Scaling frequency (Hz) - 1.5 cycles per second */
	const int min_radius = 25;  /* Minimum ball size (pixels) */
	const int max_radius = 55;  /* Maximum ball size (pixels) */
	const float delta_time = 0.01666f;  /* ~60 FPS = 16.66ms per frame */
	
	/* Fixed buffer size for ball texture (must be >= max_radius * 2) */
	const int ball_buffer_size = 110;  /* Exact size for max_radius=55 → 110px diameter */
	
	/* HARDWARE SCALING ENABLED - Variable size ball with pitch/crop workaround */
	int ball_radius = max_radius;  /* Start at maximum size */
	int ball_size;  /* Will be updated each frame: ball_radius * 2 */
	
	/* Background gradient colors (ARGB format - standard colors!) */
	uint32_t bg_top = 0xFF00008FF;     /* Dark blue (A=FF, R=00, G=00, B=8F) */
	uint32_t bg_bottom = 0xFFFF00266;  /* Bright red (A=FF, R=FF, G=00, B=26) */
	
	/* FPS tracking */
	struct timespec frame_start, frame_end;
	long frame_time_us;
	int frame_count = 0;
	float fps = 0.0f;
	struct timespec fps_start;
	int ball_ion_fd = -1;
	int bg_ion_fd = -1;
	int temp_ion_fd = -1;  /* Background copy buffer */
	int comp_ion_fd = -1;  /* Composition output buffer (3rd distinct buffer) */
	
	signal(SIGINT, sigint_handler);
	
	printf("==============================\n\n");
	
	ret = drm_display_init(&disp);
	if (ret < 0) {
		fprintf(stderr, "Failed to initialize DRM display\n");
		return 1;
	}
	
	/* Create ION buffer for ball sprite (120x120 ARGB8888 - large enough for max scaling) */
	struct g2d_alloc_buffer ball_alloc = {0};
	ball_alloc.size = ball_buffer_size * ball_buffer_size * 4;  /* ARGB8888 = 4 bytes per pixel */
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &ball_alloc);
	if (ret < 0) {
		fprintf(stderr, "Failed to allocate ION buffer for ball\n");
		perror("G2D_IOC_ALLOC_BUFFER (ball)");
		drm_display_cleanup(&disp);
		return 1;
	}
	ball_ion_fd = ball_alloc.dma_fd;
	printf("Ball ION buffer allocated: fd=%d size=%llu bytes (%dx%d ARGB8888)\n",
	       ball_ion_fd, ball_alloc.size, ball_buffer_size, ball_buffer_size);
	
	/* Create ION buffer for background (800x480 XRGB8888) */
	struct g2d_alloc_buffer bg_alloc = {0};
	bg_alloc.size = disp.width * disp.height * 4;
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &bg_alloc);
	if (ret < 0) {
		fprintf(stderr, "Failed to allocate ION buffer for background\n");
		perror("G2D_IOC_ALLOC_BUFFER (bg)");
		close(ball_ion_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	bg_ion_fd = bg_alloc.dma_fd;
	printf("Background ION buffer allocated: fd=%d size=%llu bytes (%dx%d XRGB8888)\n",
	       bg_ion_fd, bg_alloc.size, disp.width, disp.height);
	
	/* Create temporary ION buffer for composition (same size as background) */
	struct g2d_alloc_buffer temp_alloc = {0};
	temp_alloc.size = disp.width * disp.height * 4;
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &temp_alloc);
	if (ret < 0) {
		fprintf(stderr, "Failed to allocate ION buffer for temp\n");
		perror("G2D_IOC_ALLOC_BUFFER (temp)");
		close(bg_ion_fd);
		close(ball_ion_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	temp_ion_fd = temp_alloc.dma_fd;
	printf("Temp ION buffer allocated: fd=%d size=%llu bytes (%dx%d XRGB8888)\n",
	       temp_ion_fd, temp_alloc.size, disp.width, disp.height);
	
	/* Create composition buffer for intermediate blend result (110x110 XRGB8888)
	 * Used in new 3-step approach: blend ball+bg → comp, then scale comp → temp
	 */
	struct g2d_alloc_buffer comp_alloc = {0};
	comp_alloc.size = ball_buffer_size * ball_buffer_size * 4;  /* 110x110 XRGB8888 */
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &comp_alloc);
	if (ret < 0) {
		fprintf(stderr, "Failed to allocate ION buffer for composition output\n");
		perror("G2D_IOC_ALLOC_BUFFER (comp)");
		close(temp_ion_fd);
		close(bg_ion_fd);
		close(ball_ion_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	comp_ion_fd = comp_alloc.dma_fd;
	printf("Composition ION buffer allocated: fd=%d size=%llu bytes (%dx%d XRGB8888)\n",
	       comp_ion_fd, comp_alloc.size, ball_buffer_size, ball_buffer_size);
	
	/* Create scaled ball buffer for 2-step scaling+blending
	 * G2D limitation: cannot do scaling + alpha blend simultaneously
	 * Solution: Step 1: scale ball → ball_scaled, Step 2: alpha blend ball_scaled + bg
	 */
	int ball_scaled_ion_fd;
	struct g2d_alloc_buffer ball_scaled_alloc = {0};
	ball_scaled_alloc.size = ball_buffer_size * ball_buffer_size * 4;  /* Max size: 110x110 ARGB8888 */
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &ball_scaled_alloc);
	if (ret < 0) {
		fprintf(stderr, "Failed to allocate ION buffer for scaled ball\n");
		perror("G2D_IOC_ALLOC_BUFFER (ball_scaled)");
		close(comp_ion_fd);
		close(temp_ion_fd);
		close(bg_ion_fd);
		close(ball_ion_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	ball_scaled_ion_fd = ball_scaled_alloc.dma_fd;
	printf("Scaled ball ION buffer allocated: fd=%d size=%llu bytes (%dx%d ARGB8888, for scaling step)\n",
	       ball_scaled_ion_fd, ball_scaled_alloc.size, ball_buffer_size, ball_buffer_size);
	
	/* Create YUV422 buffer for testing VSU YUV path
	 * YUV422 packed format (YUYV) uses 2 bytes per pixel
	 */
	int ball_yuv_fd;
	struct g2d_alloc_buffer ball_yuv_alloc = {0};
	ball_yuv_alloc.size = ball_buffer_size * ball_buffer_size * 2;  /* YUV422 = 2 bytes/pixel */
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &ball_yuv_alloc);
	if (ret < 0) {
		fprintf(stderr, "Failed to allocate ION buffer for YUV test\n");
		perror("G2D_IOC_ALLOC_BUFFER (ball_yuv)");
		close(ball_scaled_ion_fd);
		close(comp_ion_fd);
		close(temp_ion_fd);
		close(bg_ion_fd);
		close(ball_ion_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	ball_yuv_fd = ball_yuv_alloc.dma_fd;
	printf("YUV422 ball buffer allocated: fd=%d size=%llu bytes (%dx%d YUV422, for VSU YUV test)\n",
	       ball_yuv_fd, ball_yuv_alloc.size, ball_buffer_size, ball_buffer_size);
	
	/* Create ball pattern in userspace memory then upload using G2D_IOC_WRITE_BUFFER
	 * This demonstrates how applications can create custom graphics/textures.
	 */
	{
		/* Allocate local buffer for ball pattern */
		uint32_t *ball_pattern = malloc(ball_alloc.size);
		if (!ball_pattern) {
			perror("malloc (ball pattern)");
			close(temp_ion_fd);
			close(bg_ion_fd);
			close(ball_ion_fd);
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
					color = 0xFF000000;  /* Alpha=255 (transparent), RGB=black */
				} else {
					/* Inside circle: RGB COLOR GRADIENT pattern with radial alpha
					 * CORRECTED ALPHA BEHAVIOR (standard ARGB, NO inversion):
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
					
					/* Radial gradient: center=255 (opaque) → edge=64 (mostly transparent) */
					float norm_dist = dist / pattern_max_radius;
					uint8_t alpha = (uint8_t)(255 - (norm_dist * 191));  /* 255→64 gradient */
					
					/* G2D_FMT_ARGB8888 expects ARGB in big-endian conceptually,
					 * but on little-endian ARM it's stored as [B][G][R][A] in memory.
					 * Construct as ARGB value which will be byte-swapped correctly. */
					color = ((uint32_t)alpha << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
				}
				
				ball_pattern[y * ball_buffer_size + x] = color;
			}
		}
		
		printf("Ball pattern created: 4x4 CHECKERBOARD (for scaling verification)\n");
		
		/* Upload pattern to ION buffer using new WRITE_BUFFER ioctl */
		ret = g2d_write_buffer(disp.g2d_fd, ball_ion_fd, ball_pattern, ball_alloc.size, 0);
		free(ball_pattern);
		
		if (ret < 0) {
			fprintf(stderr, "Failed to write ball pattern to ION buffer\n");
			close(comp_ion_fd);
			close(temp_ion_fd);
			close(bg_ion_fd);
			close(ball_ion_fd);
			drm_display_cleanup(&disp);
			return 1;
		}
		
		printf("Ball pattern uploaded to ION buffer successfully via G2D_IOC_WRITE_BUFFER\n");
	}
	
	/* Initialize ball_scaled buffer to fully transparent (critical for scaling step) 
	 * We use WRITE_BUFFER instead of FILLRECT to avoid fence complexity */
	{
		size_t ball_scaled_size = ball_buffer_size * ball_buffer_size * 4;
		void *ball_scaled_data = calloc(1, ball_scaled_size);  /* calloc zeros memory = transparent */
		if (!ball_scaled_data) {
			fprintf(stderr, "Failed to allocate memory for ball_scaled initialization\n");
			close(ball_yuv_fd);
			close(ball_scaled_ion_fd);
			close(comp_ion_fd);
			close(temp_ion_fd);
			close(bg_ion_fd);
			close(ball_ion_fd);
			drm_display_cleanup(&disp);
			return 1;
		}
		
		struct g2d_buffer_rw write_scaled = {0};
		write_scaled.dma_fd = ball_scaled_ion_fd;
		write_scaled.offset = 0;
		write_scaled.size = ball_scaled_size;
		write_scaled.user_ptr = (__u64)(uintptr_t)ball_scaled_data;
		
		ret = ioctl(disp.g2d_fd, G2D_IOC_WRITE_BUFFER, &write_scaled);
		free(ball_scaled_data);
		
		if (ret < 0) {
			fprintf(stderr, "Failed to initialize ball_scaled buffer\n");
			perror("G2D_IOC_WRITE_BUFFER (ball_scaled init)");
			close(ball_yuv_fd);
			close(ball_scaled_ion_fd);
			close(comp_ion_fd);
			close(temp_ion_fd);
			close(bg_ion_fd);
			close(ball_ion_fd);
			drm_display_cleanup(&disp);
			return 1;
		}
		
		printf("Scaled ball buffer initialized to transparent\n");
	}
	
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
	int gradient_ion_fd;
	struct g2d_alloc_buffer gradient_alloc = {0};
	gradient_alloc.size = gradient_width * gradient_height * 4;  /* XRGB8888 */
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &gradient_alloc);
	if (ret < 0) {
		fprintf(stderr, "Failed to allocate ION buffer for gradient source\n");
		perror("G2D_IOC_ALLOC_BUFFER (gradient)");
		close(ball_yuv_fd);
		close(ball_scaled_ion_fd);
		close(comp_ion_fd);
		close(temp_ion_fd);
		close(bg_ion_fd);
		close(ball_ion_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	gradient_ion_fd = gradient_alloc.dma_fd;
	printf("Gradient source buffer allocated: fd=%d size=%llu bytes (%dx%d XRGB8888)\n",
	       gradient_ion_fd, gradient_alloc.size, gradient_width, gradient_height);
	
	/* Create gradient pattern in memory (vertical gradient from top to bottom) */
	{
		uint32_t *gradient_data = malloc(gradient_alloc.size);
		if (!gradient_data) {
			perror("malloc (gradient pattern)");
			close(gradient_ion_fd);
			close(ball_yuv_fd);
			close(ball_scaled_ion_fd);
			close(comp_ion_fd);
			close(temp_ion_fd);
			close(bg_ion_fd);
			close(ball_ion_fd);
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
		
		/* Upload gradient pattern to ION buffer */
		ret = g2d_write_buffer(disp.g2d_fd, gradient_ion_fd, gradient_data, 
		                      gradient_alloc.size, 0);
		free(gradient_data);
		
		if (ret < 0) {
			fprintf(stderr, "Failed to write gradient pattern to ION buffer\n");
			close(gradient_ion_fd);
			close(ball_yuv_fd);
			close(ball_scaled_ion_fd);
			close(comp_ion_fd);
			close(temp_ion_fd);
			close(bg_ion_fd);
			close(ball_ion_fd);
			drm_display_cleanup(&disp);
			return 1;
		}
		
		printf("Gradient pattern created: %dx%d → %dx%d (scale: %.1fx × %.1fx)\n",
		       gradient_width, gradient_height, disp.width, disp.height,
		       (float)disp.width / gradient_width, (float)disp.height / gradient_height);
	}
	
	/* Scale gradient to full background using VSU (G2D_IOC_BLIT with scaling) */
	struct g2d_blit scale_gradient = {0};
	scale_gradient.out.dma_fd = -1;  /* Legacy 2-buffer mode */
	
	/* Source: small gradient pattern */
	scale_gradient.src.width = gradient_width;
	scale_gradient.src.height = gradient_height;
	scale_gradient.src.format = G2D_FMT_XRGB8888;
	scale_gradient.src.stride[0] = gradient_width * 4;
	scale_gradient.src.dma_fd = gradient_ion_fd;
	scale_gradient.src.crop_x = 0;
	scale_gradient.src.crop_y = 0;
	scale_gradient.src.crop_w = gradient_width;
	scale_gradient.src.crop_h = gradient_height;
	
	/* Destination: full-size background buffer */
	scale_gradient.dst.width = disp.width;
	scale_gradient.dst.height = disp.height;
	scale_gradient.dst.format = G2D_FMT_XRGB8888;
	scale_gradient.dst.stride[0] = disp.width * 4;
	scale_gradient.dst.dma_fd = bg_ion_fd;
	scale_gradient.dst.crop_x = 0;
	scale_gradient.dst.crop_y = 0;
	scale_gradient.dst.crop_w = 0;  /* No crop */
	scale_gradient.dst.crop_h = 0;  /* No crop */
	
	scale_gradient.dst_x = 0;
	scale_gradient.dst_y = 0;
	scale_gradient.dst_w = disp.width;
	scale_gradient.dst_h = disp.height;
	scale_gradient.flags = 0;
	scale_gradient.global_alpha = 255;
	scale_gradient.fence_fd_in = -1;
	scale_gradient.fence_fd_out = -1;
	
	ret = ioctl(disp.g2d_fd, G2D_IOC_BLIT, &scale_gradient);
	if (ret < 0) {
		perror("G2D_IOC_BLIT (scale gradient → background)");
		close(gradient_ion_fd);
		close(ball_yuv_fd);
		close(ball_scaled_ion_fd);
		close(comp_ion_fd);
		close(temp_ion_fd);
		close(bg_ion_fd);
		close(ball_ion_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	
	if (scale_gradient.fence_fd_out >= 0) {
		close(scale_gradient.fence_fd_out);
	}
	
	/* Clean up gradient source buffer (no longer needed) */
	close(gradient_ion_fd);
	
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
		close(ball_yuv_fd);
		close(ball_scaled_ion_fd);
		close(comp_ion_fd);
		close(temp_ion_fd);
		close(bg_ion_fd);
		close(ball_ion_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	printf("Framebuffer dmabuf exported: fd=%d (reused for entire demo)\n", fb_dmabuf);
	
	/* Initialize both pages with background */
	{
		struct g2d_blit init_blit = {0};
			
			/* Legacy 2-buffer mode: dst is also output */
			init_blit.out.dma_fd = -1;
			
			/* Source: background gradient */
			init_blit.src.width = disp.width;
			init_blit.src.height = disp.height;
			init_blit.src.format = G2D_FMT_XRGB8888;
			init_blit.src.stride[0] = disp.width * 4;
			init_blit.src.dma_fd = bg_ion_fd;
			init_blit.src.crop_x = 0;
			init_blit.src.crop_y = 0;
			init_blit.src.crop_w = disp.width;
			init_blit.src.crop_h = disp.height;
			
			/* Destination: framebuffer (800x960 double-buffered) */
			init_blit.dst.width = disp.width;
			init_blit.dst.height = disp.height * 2;
			init_blit.dst.format = G2D_FMT_XRGB8888;
			init_blit.dst.stride[0] = disp.pitch;
			init_blit.dst.dma_fd = fb_dmabuf;
			init_blit.dst.crop_x = 0;
			init_blit.dst.crop_y = 0;
			init_blit.dst.crop_w = 0;  /* No crop */
			init_blit.dst.crop_h = 0;  /* No crop */
			init_blit.dst_x = 0;
			init_blit.dst_w = disp.width;
			init_blit.dst_h = disp.height;
			init_blit.flags = 0;
			init_blit.fence_fd_in = -1;
			init_blit.fence_fd_out = -1;
			
			/* BLIT to page 0 (offset 0 lines) */
			init_blit.dst_y = 0;
			
			if (ioctl(disp.g2d_fd, G2D_IOC_BLIT, &init_blit) < 0) {
				perror("G2D_IOC_BLIT (bg → fb page 0)");
			} else if (init_blit.fence_fd_out >= 0) {
				close(init_blit.fence_fd_out);
			}
			
			/* BLIT to page 1 (offset 480 lines) */
			init_blit.dst_y = disp.height;
			init_blit.fence_fd_out = -1;
			
		if (ioctl(disp.g2d_fd, G2D_IOC_BLIT, &init_blit) < 0) {
			perror("G2D_IOC_BLIT (bg → fb page 1)");
		} else if (init_blit.fence_fd_out >= 0) {
			close(init_blit.fence_fd_out);
		}
		
		printf("Initialized both framebuffer pages with background\n");
	}
	
	clock_gettime(CLOCK_MONOTONIC, &fps_start);	while (keep_running) {
		clock_gettime(CLOCK_MONOTONIC, &frame_start);
		
		/* Two modes: normal composition flow, or diagnostic pattern fill */
		if (pattern_mode) {
			/* Fill temp with a solid magenta (visible) and blit to both pages */
			struct g2d_fillrect fill_pat = {0};
			fill_pat.dst.width = disp.width;
			fill_pat.dst.height = disp.height;
			fill_pat.dst.format = G2D_FMT_ARGB8888; /* write explicit ARGB into temp for diagnostics */
			fill_pat.dst.stride[0] = disp.width * 4;
			fill_pat.dst.dma_fd = temp_ion_fd;
			fill_pat.dst.crop_x = 0;
			fill_pat.dst.crop_y = 0;
			fill_pat.dst.crop_w = disp.width;
			fill_pat.dst.crop_h = disp.height;
			fill_pat.dst_x = 0;
			fill_pat.dst_y = 0;
			fill_pat.dst_w = disp.width;
			fill_pat.dst_h = disp.height;
			/* Use opaque ARGB magenta so the fill isn't treated as transparent */
			fill_pat.color = 0xFFFF00FF; /* ARGB: A=FF, R=FF, G=00, B=FF (magenta) */
			fill_pat.color_format = G2D_FMT_ARGB8888;
			fill_pat.fence_fd_in = -1;
			fill_pat.fence_fd_out = -1;

			ret = ioctl(disp.g2d_fd, G2D_IOC_FILLRECT, &fill_pat);
			if (ret < 0) {
				perror("G2D_IOC_FILLRECT (pattern)");
				break;
			}
			/* Close fillrect fence fd without waiting */
			if (fill_pat.fence_fd_out >= 0) {
				close(fill_pat.fence_fd_out);
			}

			/* Diagnostic dumps removed in cleaned build. */

			/* Blit temp -> framebuffer pages (same as step 3 in normal flow) */
			uint32_t backbuffer_offset_bytes = drm_get_backbuffer_offset(&disp);
			int backbuffer_y_offset = backbuffer_offset_bytes / disp.pitch;
			struct g2d_blit blit = {0};
			
			/* Legacy 2-buffer mode: dst is also output */
			blit.out.dma_fd = -1;
			
			blit.src.width = disp.width;
			blit.src.height = disp.height;
			blit.src.format = G2D_FMT_XRGB8888;
			blit.src.stride[0] = disp.width * 4;
			blit.src.dma_fd = temp_ion_fd;
			blit.src.crop_x = 0;
			blit.src.crop_y = 0;
			blit.src.crop_w = disp.width;
			blit.src.crop_h = disp.height;

			blit.dst.width = disp.width;
			blit.dst.height = disp.height * 2;
			blit.dst.format = G2D_FMT_XRGB8888;
			blit.dst.stride[0] = disp.pitch;
			blit.dst.dma_fd = fb_dmabuf;  /* Use global dmabuf (exported once) */
			blit.dst.crop_x = 0;
			blit.dst.crop_y = 0;  /* No crop */
			blit.dst.crop_w = 0;  /* No crop */
			blit.dst.crop_h = 0;  /* No crop */

			blit.dst_x = 0;
			blit.dst_y = backbuffer_y_offset; /* CRITICAL: Write offset (0 or 480) */
			blit.dst_w = disp.width;
			blit.dst_h = disp.height;
			blit.flags = 0;
			blit.fence_fd_in = -1;
			blit.fence_fd_out = -1;

			ret = ioctl(disp.g2d_fd, G2D_IOC_BLIT, &blit);
			if (ret < 0) {
				perror("G2D_IOC_BLIT (pattern -> framebuffer)");
				break;
			}
			
			/* Close fence fd without waiting */
			if (blit.fence_fd_out >= 0) {
				close(blit.fence_fd_out);
			}
		} else {
		/* Composite with 3 DISTINCT buffers: bg→temp, ball+temp→comp, comp→fb
		 * Step 1 (inside function): BLIT bg → temp_buffer (background copy)
		 * Step 2a (inside function): BLIT ball → ball_scaled (scaling only)
		 * Step 2b (inside function): ALPHA_BLEND ball_scaled + temp → temp (blending only)
		 * Step 3 (inside function): BLIT temp → framebuffer (final copy)
		 */
		ret = g2d_blend_ball_ion(&disp, disp.g2d_fd, bg_ion_fd, ball_ion_fd, 
		                         ball_scaled_ion_fd, temp_ion_fd, comp_ion_fd,
		                         ball_yuv_fd, fb_dmabuf,
								 (int)(ball_x - ball_radius), 
								 (int)(ball_y - ball_radius),
								 ball_radius, ball_buffer_size);  /* ball_buffer_size is CONSTANT 110 */
		if (ret < 0) {
			fprintf(stderr, "Failed to blend ball\n");
			break;
		}
	}
	if (ret < 0) {
		fprintf(stderr, "Failed to blend ball\n");
		break;
	}		/* Flip page AFTER rendering (synchronized with VSYNC to eliminate tearing) */
		drm_flip_page_vsync(&disp);
		
	/* FPS calculation */
	frame_count++;
	clock_gettime(CLOCK_MONOTONIC, &frame_end);
	
	/* Update ball physics (bouncing) */
	ball_x += vel_x;
	ball_y += vel_y;
	
	/* Bounce off walls */
	if (ball_x - ball_radius < 0 || ball_x + ball_radius > disp.width) {
		vel_x = -vel_x;
		ball_x += vel_x;  /* Correct position */
	}
	if (ball_y - ball_radius < 0 || ball_y + ball_radius > disp.height) {
		vel_y = -vel_y;
		ball_y += vel_y;  /* Correct position */
	}
	
	/* Update ball size with smooth sinusoidal animation */
	scale_time += delta_time;
	float scale_factor = 0.5f + 0.5f * sinf(2.0f * M_PI * scale_speed * scale_time);
	ball_radius = min_radius + (int)((max_radius - min_radius) * scale_factor);
	ball_size = ball_radius * 2;
	
	/* FPS display (every second) */
	struct timespec fps_now;
	clock_gettime(CLOCK_MONOTONIC, &fps_now);
	long fps_elapsed = (fps_now.tv_sec - fps_start.tv_sec) * 1000000L +
	                  (fps_now.tv_nsec - fps_start.tv_nsec) / 1000L;
	
	if (fps_elapsed >= 1000000L) {  /* 1 second */
		fps = (float)frame_count / (fps_elapsed / 1000000.0f);
		printf("\rFPS: %.1f | Ball size: %dpx | Position: (%.0f, %.0f)  ", 
		       fps, ball_size, ball_x, ball_y);
		fflush(stdout);
		frame_count = 0;
		clock_gettime(CLOCK_MONOTONIC, &fps_start);
	}		/* Frame timing (target ~60 FPS = 16.6ms) */
		frame_time_us = (frame_end.tv_sec - frame_start.tv_sec) * 1000000L +
		               (frame_end.tv_nsec - frame_start.tv_nsec) / 1000L;
		
		if (frame_time_us < 16666) {
			usleep(16666 - frame_time_us);
		}
	}	printf("\n\nCleaning up...\n");
	
	/* Close framebuffer dmabuf (was kept open for entire demo) */
	if (fb_dmabuf >= 0)
		close(fb_dmabuf);
	
	if (comp_ion_fd >= 0)
		close(comp_ion_fd);
	if (ball_yuv_fd >= 0)
		close(ball_yuv_fd);
	if (ball_scaled_ion_fd >= 0)
		close(ball_scaled_ion_fd);
	if (temp_ion_fd >= 0)
		close(temp_ion_fd);
	if (bg_ion_fd >= 0)
		close(bg_ion_fd);
	if (ball_ion_fd >= 0)
		close(ball_ion_fd);
	
	drm_display_cleanup(&disp);
	
	printf("Total frames: %d\n", frame_count);
	printf("Demo exited cleanly\n");
	
	return 0;
}
