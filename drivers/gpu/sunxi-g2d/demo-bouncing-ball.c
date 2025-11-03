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
#include <time.h>
#include <math.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/sunxi_g2d.h>
#include "demo-drm-base.h"

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

/*
 * Simplified 2-step blending flow:
 * Step 1: BLIT bg → temp_buffer (clean background copy)
 * Step 2: ALPHA_BLEND ball → temp_buffer (compositing at x,y position)
 * Step 3: BLIT temp_buffer → framebuffer (final display copy)
 */
int g2d_blend_ball_ion(struct drm_display *disp, int g2d_fd,
                       int bg_ion_fd, int ball_ion_fd, int temp_ion_fd,
                       int x, int y, int radius)
{
	struct g2d_alpha_blend blend = {0};
	struct g2d_blit blit = {0};
	int dmabuf_fd;
	int ret;
	int ball_size = radius * 2;

	/* Export DRM framebuffer as dmabuf */
	dmabuf_fd = drm_export_dmabuf(disp);
	if (dmabuf_fd < 0)
		return -1;

	int backbuffer_y_offset = drm_get_backbuffer_offset(disp) / disp->pitch;

	/* Production: do not print per-frame diagnostics here. */

	/* Step 1: BLIT background → temp_buffer */
	blit.src.width = disp->width;
	blit.src.height = disp->height;
			/* Temp was filled as ARGB8888 above, ensure src format matches */
			blit.src.format = G2D_FMT_ARGB8888;
	blit.src.stride[0] = disp->width * 4;
	blit.src.dma_fd = bg_ion_fd;
	blit.src.crop_x = 0;
	blit.src.crop_y = 0;
	blit.src.crop_w = disp->width;
	blit.src.crop_h = disp->height;

	blit.dst.width = disp->width;
	blit.dst.height = disp->height;
	blit.dst.format = G2D_FMT_XRGB8888;
	blit.dst.stride[0] = disp->width * 4;
	blit.dst.dma_fd = temp_ion_fd;
	blit.dst.crop_x = 0;
	blit.dst.crop_y = 0;
	blit.dst.crop_w = disp->width;
	blit.dst.crop_h = disp->height;

			blit.dst.crop_y = 0;
			blit.dst_y = backbuffer_y_offset; /* expected 0 or disp.height */
	blit.dst_y = 0;
	blit.dst_w = disp->width;
	blit.dst_h = disp->height;
	blit.flags = 0;
	blit.fence_fd_in = -1;
	blit.fence_fd_out = -1;

	ret = ioctl(g2d_fd, G2D_IOC_BLIT, &blit);
	if (ret < 0) {
		perror("G2D_IOC_BLIT (bg → temp)");
		close(dmabuf_fd);
		return -1;
	}

	/* Step 2: ALPHA_BLEND ball → temp_buffer (compositing at position x,y) */
	blend.src.width = ball_size;
	blend.src.height = ball_size;
	blend.src.format = G2D_FMT_ARGB8888;
	blend.src.stride[0] = ball_size * 4;
	blend.src.dma_fd = ball_ion_fd;
	blend.src.crop_x = 0;
	blend.src.crop_y = 0;
	blend.src.crop_w = ball_size;
	blend.src.crop_h = ball_size;
	blend.src.alpha = 128;  /* Ball semi-transparent */
	blend.src.alpha_mode = G2D_PIXEL_ALPHA;  /* Use pixel alpha instead of global */

	blend.dst.width = disp->width;
	blend.dst.height = disp->height;
	blend.dst.format = G2D_FMT_XRGB8888;
	blend.dst.stride[0] = disp->width * 4;
	blend.dst.dma_fd = temp_ion_fd;
	blend.dst.crop_x = x;  /* Position ball at x,y in temp buffer */
	blend.dst.crop_y = y;
	blend.dst.crop_w = ball_size;
	blend.dst.crop_h = ball_size;
	blend.dst.alpha = 255;  /* Background in temp buffer is opaque */
	blend.dst.alpha_mode = G2D_GLOBAL_ALPHA;

	blend.fence_fd_in = -1;
	blend.fence_fd_out = -1;

	ret = ioctl(g2d_fd, G2D_IOC_ALPHA_BLEND, &blend);
	if (ret < 0) {
		perror("G2D_IOC_ALPHA_BLEND (ball → temp)");
		close(dmabuf_fd);
		return -1;
	}

	/* Step 3: BLIT temp_buffer → framebuffer (atomic copy to backbuffer) */
	blit.src.width = disp->width;
	blit.src.height = disp->height;
	blit.src.format = G2D_FMT_XRGB8888;
	blit.src.stride[0] = disp->width * 4;
	blit.src.dma_fd = temp_ion_fd;  /* Read from temp buffer */
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
	blit.dst.crop_y = backbuffer_y_offset;  /* Write to current backbuffer */
	blit.dst.crop_w = disp->width;
	blit.dst.crop_h = disp->height;

	blit.dst_x = 0;
	blit.dst_y = 0;
	blit.dst_w = disp->width;
	blit.dst_h = disp->height;
	blit.flags = 0;  /* Simple copy */
	blit.fence_fd_in = -1;
	blit.fence_fd_out = -1;

	ret = ioctl(g2d_fd, G2D_IOC_BLIT, &blit);
	if (ret < 0) {
		perror("G2D_IOC_BLIT (temp → framebuffer)");
		close(dmabuf_fd);
		return -1;
	}
	/* Wait for G2D blit to finish before flipping to avoid showing
	 * partially written frames. Use fence_fd_out if provided by the
	 * kernel driver, and fall back to no-wait if not supported.
	 */
	if (blit.fence_fd_out >= 0) {
		int sync_fd = blit.fence_fd_out;
		/* We already computed and printed backbuffer_y_offset earlier */
		if (ioctl(g2d_fd, G2D_IOC_SYNC, &sync_fd) < 0)
			perror("G2D_IOC_SYNC (wait blit temp->fb)");
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

	close(dmabuf_fd);
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
	int ball_radius = 40;
	uint32_t ball_color = 0xFFFFFFFF;  /* White in ARGB format - maximum visibility */
	uint8_t ball_alpha = 128;  /* Semi-transparent for final demo */
	int ball_size = ball_radius * 2;
	
	/* Background gradient colors (ARGB format - standard colors!) */
	uint32_t bg_top = 0xFF004080;     /* Medium-dark blue (A=FF, R=00, G=40, B=80) */
	uint32_t bg_bottom = 0xFF0080FF;  /* Bright blue (A=FF, R=00, G=80, B=FF) */
	
	/* FPS tracking */
	struct timespec frame_start, frame_end;
	long frame_time_us;
	int frame_count = 0;
	float fps = 0.0f;
	struct timespec fps_start;
	int ball_ion_fd = -1;
	int bg_ion_fd = -1;
	int temp_ion_fd = -1;  /* NEW: Temporary composition buffer */
	
	signal(SIGINT, sigint_handler);
	
	printf("==============================\n\n");
	
	ret = drm_display_init(&disp);
	if (ret < 0) {
		fprintf(stderr, "Failed to initialize DRM display\n");
		return 1;
	}
	
	/* Create ION buffer for ball sprite (80x80 ARGB8888) */
	struct g2d_alloc_buffer ball_alloc = {0};
	ball_alloc.size = ball_size * ball_size * 4;  /* ARGB8888 = 4 bytes per pixel */
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &ball_alloc);
	if (ret < 0) {
		fprintf(stderr, "Failed to allocate ION buffer for ball\n");
		perror("G2D_IOC_ALLOC_BUFFER (ball)");
		drm_display_cleanup(&disp);
		return 1;
	}
	ball_ion_fd = ball_alloc.dma_fd;
	printf("Ball ION buffer allocated: fd=%d size=%llu bytes (%dx%d ARGB8888)\n",
	       ball_ion_fd, ball_alloc.size, ball_size, ball_size);
	
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
	
	/* Fill ball buffer once with white color */
	struct g2d_fillrect fill_ball = {0};
	fill_ball.dst.width = ball_size;
	fill_ball.dst.height = ball_size;
	fill_ball.dst.format = G2D_FMT_ARGB8888;
	fill_ball.dst.stride[0] = ball_size * 4;
	fill_ball.dst.dma_fd = ball_ion_fd;
	fill_ball.dst.crop_x = 0;
	fill_ball.dst.crop_y = 0;
	fill_ball.dst.crop_w = ball_size;
	fill_ball.dst.crop_h = ball_size;
	fill_ball.dst_x = 0;
	fill_ball.dst_y = 0;
	fill_ball.dst_w = ball_size;
	fill_ball.dst_h = ball_size;
	fill_ball.color = 0xFFFFFFFF;  /* White with full alpha */
	fill_ball.color_format = G2D_FMT_ARGB8888;
	fill_ball.fence_fd_in = -1;
	fill_ball.fence_fd_out = -1;
	
	ret = ioctl(disp.g2d_fd, G2D_IOC_FILLRECT, &fill_ball);
	if (ret < 0) {
		perror("G2D_IOC_FILLRECT (ball)");
		close(temp_ion_fd);
		close(bg_ion_fd);
		close(ball_ion_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	printf("Ball buffer filled with white color\n");
	
	/* Fill background buffer once with gradient (two rects) */
	struct g2d_fillrect fill_bg_top = {0};
	fill_bg_top.dst.width = disp.width;
	fill_bg_top.dst.height = disp.height;
	fill_bg_top.dst.format = G2D_FMT_XRGB8888;
	fill_bg_top.dst.stride[0] = disp.width * 4;
	fill_bg_top.dst.dma_fd = bg_ion_fd;
	fill_bg_top.dst.crop_x = 0;
	fill_bg_top.dst.crop_y = 0;
	fill_bg_top.dst.crop_w = disp.width;
	fill_bg_top.dst.crop_h = disp.height / 2;
	fill_bg_top.dst_x = 0;
	fill_bg_top.dst_y = 0;
	fill_bg_top.dst_w = disp.width;
	fill_bg_top.dst_h = disp.height / 2;
	fill_bg_top.color = bg_top;
	fill_bg_top.color_format = G2D_FMT_ARGB8888;
	fill_bg_top.fence_fd_in = -1;
	fill_bg_top.fence_fd_out = -1;
	
	ret = ioctl(disp.g2d_fd, G2D_IOC_FILLRECT, &fill_bg_top);
	if (ret < 0) {
		perror("G2D_IOC_FILLRECT (bg top)");
		close(temp_ion_fd);
		close(bg_ion_fd);
		close(ball_ion_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	
	struct g2d_fillrect fill_bg_bottom = fill_bg_top;
	fill_bg_bottom.dst.crop_y = disp.height / 2;
	fill_bg_bottom.dst_y = disp.height / 2;
	fill_bg_bottom.color = bg_bottom;
	
	ret = ioctl(disp.g2d_fd, G2D_IOC_FILLRECT, &fill_bg_bottom);
	if (ret < 0) {
		perror("G2D_IOC_FILLRECT (bg bottom)");
		close(temp_ion_fd);
		close(bg_ion_fd);
		close(ball_ion_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	printf("Background buffer filled with gradient\n");

	/* Removed diagnostic startup blit that populated both fb pages. The
	 * production demo relies on normal composition + pageflip. If early
	 * frame black/stale pages are a concern, initialize the FB from the
	 * kernel side or orchestrate a proper atomic modeset rather than
	 * issuing best-effort asynchronous blits here. */

	
	/* Initialize ball position */
	ball_x = disp.width / 2.0f;
	ball_y = disp.height / 2.0f;
	
	printf("\nPress Ctrl+C to exit\n\n");
	clock_gettime(CLOCK_MONOTONIC, &fps_start);
	
	while (keep_running) {
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

			/* Diagnostic dumps removed in cleaned build. */

			/* Blit temp -> framebuffer pages (same as step 3 in normal flow) */
	     uint32_t backbuffer_offset_bytes = drm_get_backbuffer_offset(&disp);
	     int backbuffer_y_offset = backbuffer_offset_bytes / disp.pitch;
	     printf("[pattern] backbuffer_offset_bytes=%u backbuffer_y_offset(lines)=%d\n",
		     backbuffer_offset_bytes, backbuffer_y_offset);
			struct g2d_blit blit = {0};
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
			int fb_dmabuf = drm_export_dmabuf(&disp);
			printf("[pattern] exported fb dmabuf=%d\n", fb_dmabuf);
			if (fb_dmabuf < 0) {
				fprintf(stderr, "Failed to export framebuffer dmabuf\n");
				/* fallback: skip this frame but continue running */
			} else {
				/* No diagnostic dumps in production build. */
			}
	     blit.dst.dma_fd = fb_dmabuf;
	     blit.dst.crop_x = 0;
	     blit.dst.crop_y = backbuffer_y_offset; /* expected 0 or disp.height */
	     printf("[pattern] blit(dst.crop_y)=%d dst.stride=%u dst.height=%u\n",
		     blit.dst.crop_y, blit.dst.stride[0], blit.dst.height);
			blit.dst.crop_w = disp.width;
			blit.dst.crop_h = disp.height;

			blit.dst_x = 0;
			blit.dst_y = 0;
			blit.dst_w = disp.width;
			blit.dst_h = disp.height;
			blit.flags = 0;
			blit.fence_fd_in = -1;
			blit.fence_fd_out = -1;

			ret = ioctl(disp.g2d_fd, G2D_IOC_BLIT, &blit);
			if (ret < 0) {
				perror("G2D_IOC_BLIT (pattern -> framebuffer)");
				if (fb_dmabuf >= 0)
					close(fb_dmabuf);
			} else {
				/* Wait on fence if driver provided one, otherwise continue.
				 * Removing best-effort sleeps used only for debugging. */
				if (blit.fence_fd_out >= 0) {
					int sync_fd = blit.fence_fd_out;
					if (ioctl(disp.g2d_fd, G2D_IOC_SYNC, &sync_fd) < 0)
						perror("G2D_IOC_SYNC (wait pattern blit)");
					close(blit.fence_fd_out);
				}
				if (fb_dmabuf >= 0)
					close(fb_dmabuf);
			}

		} else {
			/* Composite FIRST: background ION + ball ION → temp → framebuffer
			 * Step 1 (inside function): BLIT bg → temp_buffer (clean copy)
			 * Step 2 (inside function): ALPHA_BLEND ball → temp_buffer (composition)
			 * Step 3 (inside function): BLIT temp → framebuffer (final copy)
			 */
			ret = g2d_blend_ball_ion(&disp, disp.g2d_fd, bg_ion_fd, ball_ion_fd, temp_ion_fd,
									 (int)(ball_x - ball_radius), 
									 (int)(ball_y - ball_radius),
									 ball_radius);
			if (ret < 0) {
				fprintf(stderr, "Failed to blend ball\n");
				break;
			}
		}
		if (ret < 0) {
			fprintf(stderr, "Failed to blend ball\n");
			break;
		}
		
		/* Flip page AFTER rendering (show the completed frame) */
		drm_flip_page(&disp);
		
		/* Update physics */
		ball_x += vel_x;
		ball_y += vel_y;
		
		/* Bounce off walls */
		if (ball_x - ball_radius < 0 || ball_x + ball_radius > disp.width) {
			vel_x = -vel_x;
			ball_x = (ball_x < disp.width / 2) ? ball_radius : disp.width - ball_radius;
		}
		
		if (ball_y - ball_radius < 0 || ball_y + ball_radius > disp.height) {
			vel_y = -vel_y;
			ball_y = (ball_y < disp.height / 2) ? ball_radius : disp.height - ball_radius;
		}
		
		/* FPS calculation */
		frame_count++;
		clock_gettime(CLOCK_MONOTONIC, &frame_end);
		
		if (frame_count % 60 == 0) {
			long elapsed_us = (frame_end.tv_sec - fps_start.tv_sec) * 1000000L +
			                 (frame_end.tv_nsec - fps_start.tv_nsec) / 1000L;
			fps = 60.0f / (elapsed_us / 1000000.0f);
			printf("\rFPS: %.1f | Ball: (%.0f, %.0f) | Alpha: %d   ", 
			       fps, ball_x, ball_y, ball_alpha);
			fflush(stdout);
			clock_gettime(CLOCK_MONOTONIC, &fps_start);
		}
		
		/* Frame timing (target ~60 FPS = 16.6ms) */
		frame_time_us = (frame_end.tv_sec - frame_start.tv_sec) * 1000000L +
		               (frame_end.tv_nsec - frame_start.tv_nsec) / 1000L;
		
		if (frame_time_us < 16666) {
			usleep(16666 - frame_time_us);
		}
	}
	
	printf("\n\nCleaning up...\n");
	
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
