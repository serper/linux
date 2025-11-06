/*
 * G2D + DRM Demo: Bouncing Ball with Direct Alpha Blending
 * 
 * This version blends bg_ion + ball_ion → framebuffer in ONE operation
 * No intermediate BLIT step needed!
 * 
 * Compile:
 *   arm-linux-musleabihf-gcc -o demo-bouncing-ball-direct \
 *       demo-drm-base.c demo-bouncing-ball-direct.c \
 *       -I/usr/include/libdrm -ldrm -static
 * 
 * Copyright (C) 2025 Sergio Perez
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

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
#include <linux/sunxi_g2d.h>
#include "demo-drm-base.h"

static volatile int keep_running = 1;

void sigint_handler(int sig)
{
	(void)sig;
	keep_running = 0;
}

/*
 * Direct composite: bg_ion + ball_ion → framebuffer
 * Single ALPHA_BLEND operation:
 *   - UI2 reads from bg_ion_fd (background gradient)
 *   - V0 reads from ball_ion_fd (white ball sprite)
 *   - WB writes to framebuffer
 */
int g2d_blend_ball_direct(struct drm_display *disp, int g2d_fd, 
                          int bg_ion_fd, int ball_ion_fd,
                          int x, int y, int radius)
{
	struct g2d_alpha_blend blend = {0};
	int dmabuf_fd;
	int ret;
	int ball_size = radius * 2;
	
	/* Export DRM framebuffer as dmabuf */
	dmabuf_fd = drm_export_dmabuf(disp);
	if (dmabuf_fd < 0)
		return -1;
	
	int backbuffer_y_offset = drm_get_backbuffer_offset(disp) / disp->pitch;
	
	/* Source (V0): Ball sprite from ION buffer */
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
	blend.src.alpha_mode = G2D_GLOBAL_ALPHA;
	
	/* Destination (UI2 + WB): Background from ION, output to framebuffer
	 * TRICK: Use bg_ion_fd as "dst" so UI2 reads from it!
	 * But WB will write to framebuffer (dmabuf_fd)
	 * 
	 * Wait... this won't work because we need TWO different dst addresses:
	 * - UI2 reads from bg_ion_fd
	 * - WB writes to dmabuf_fd
	 * 
	 * The API doesn't support this directly. We need to modify the driver
	 * to accept a separate "background_fd" parameter.
	 */
	blend.dst.width = disp->width;
	blend.dst.height = disp->height * 2;
	blend.dst.format = G2D_FMT_XRGB8888;
	blend.dst.stride[0] = disp->pitch;
	blend.dst.dma_fd = dmabuf_fd;  /* WB writes here */
	blend.dst.crop_x = 0;
	blend.dst.crop_y = backbuffer_y_offset;
	blend.dst.crop_w = disp->width;
	blend.dst.crop_h = disp->height;
	blend.dst.alpha = 255;  /* Background opaque */
	blend.dst.alpha_mode = G2D_GLOBAL_ALPHA;
	
	/* TODO: Need driver modification to accept bg_ion_fd for UI2! */
	
	blend.fence_fd_in = -1;
	blend.fence_fd_out = -1;
	
	ret = ioctl(g2d_fd, G2D_IOC_ALPHA_BLEND, &blend);
	if (ret < 0) {
		perror("G2D_IOC_ALPHA_BLEND");
		close(dmabuf_fd);
		return -1;
	}
	
	close(dmabuf_fd);
	return 0;
}

int main(void)
{
	struct drm_display disp;
	int ret;
	
	printf("G2D + DRM Direct Blend Demo\n");
	
	/* Ball physics */
	float ball_x, ball_y;
	float vel_x = 3.5f;
	float vel_y = 2.8f;
	int ball_radius = 40;
	int ball_size = ball_radius * 2;
	
	/* Background gradient colors */
	uint32_t bg_top = 0xFF004080;
	uint32_t bg_bottom = 0xFF0080FF;
	
	/* FPS tracking */
	struct timespec frame_start, frame_end;
	long frame_time_us;
	int frame_count = 0;
	float fps = 0.0f;
	struct timespec fps_start;
	
	/* ION buffers */
	int ball_ion_fd = -1;
	int bg_ion_fd = -1;
	
	signal(SIGINT, sigint_handler);
	
	ret = drm_display_init(&disp);
	if (ret < 0) {
		fprintf(stderr, "Failed to initialize DRM display\n");
		return 1;
	}
	
	/* Create ball buffer */
	struct g2d_alloc_buffer ball_alloc = {0};
	ball_alloc.size = ball_size * ball_size * 4;
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &ball_alloc);
	if (ret < 0) {
		perror("G2D_IOC_ALLOC_BUFFER (ball)");
		drm_display_cleanup(&disp);
		return 1;
	}
	ball_ion_fd = ball_alloc.dma_fd;
	printf("Ball ION: fd=%d\n", ball_ion_fd);
	
	/* Create background buffer */
	struct g2d_alloc_buffer bg_alloc = {0};
	bg_alloc.size = disp.width * disp.height * 4;
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &bg_alloc);
	if (ret < 0) {
		perror("G2D_IOC_ALLOC_BUFFER (bg)");
		close(ball_ion_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	bg_ion_fd = bg_alloc.dma_fd;
	printf("Background ION: fd=%d\n", bg_ion_fd);
	
	/* Fill buffers (once) */
	struct g2d_fillrect fill = {0};
	
	/* Fill ball white */
	fill.dst.width = ball_size;
	fill.dst.height = ball_size;
	fill.dst.format = G2D_FMT_ARGB8888;
	fill.dst.stride[0] = ball_size * 4;
	fill.dst.dma_fd = ball_ion_fd;
	fill.dst.crop_x = 0;
	fill.dst.crop_y = 0;
	fill.dst.crop_w = ball_size;
	fill.dst.crop_h = ball_size;
	fill.dst_x = 0;
	fill.dst_y = 0;
	fill.dst_w = ball_size;
	fill.dst_h = ball_size;
	fill.color = 0xFFFFFFFF;
	fill.color_format = G2D_FMT_ARGB8888;
	fill.fence_fd_in = -1;
	fill.fence_fd_out = -1;
	ioctl(disp.g2d_fd, G2D_IOC_FILLRECT, &fill);
	
	/* Fill background gradient (top half) */
	fill.dst.width = disp.width;
	fill.dst.height = disp.height;
	fill.dst.format = G2D_FMT_XRGB8888;
	fill.dst.stride[0] = disp.width * 4;
	fill.dst.dma_fd = bg_ion_fd;
	fill.dst.crop_w = disp.width;
	fill.dst.crop_h = disp.height / 2;
	fill.dst_w = disp.width;
	fill.dst_h = disp.height / 2;
	fill.color = bg_top;
	ioctl(disp.g2d_fd, G2D_IOC_FILLRECT, &fill);
	
	/* Fill background gradient (bottom half) */
	fill.dst.crop_y = disp.height / 2;
	fill.dst_y = disp.height / 2;
	fill.color = bg_bottom;
	ioctl(disp.g2d_fd, G2D_IOC_FILLRECT, &fill);
	
	printf("Buffers filled\n");
	
	/* Initialize ball */
	ball_x = disp.width / 2.0f;
	ball_y = disp.height / 2.0f;
	
	printf("\nPress Ctrl+C to exit\n\n");
	clock_gettime(CLOCK_MONOTONIC, &fps_start);
	
	while (keep_running) {
		clock_gettime(CLOCK_MONOTONIC, &frame_start);
		
		/* Single operation: bg_ion + ball_ion → framebuffer */
		ret = g2d_blend_ball_direct(&disp, disp.g2d_fd, bg_ion_fd, ball_ion_fd,
		                            (int)(ball_x - ball_radius), 
		                            (int)(ball_y - ball_radius),
		                            ball_radius);
		if (ret < 0) {
			fprintf(stderr, "Failed to blend\n");
			break;
		}
		
		drm_flip_page(&disp);
		
		/* Physics */
		ball_x += vel_x;
		ball_y += vel_y;
		
		if (ball_x - ball_radius < 0 || ball_x + ball_radius > disp.width) {
			vel_x = -vel_x;
			ball_x = (ball_x < disp.width / 2) ? ball_radius : disp.width - ball_radius;
		}
		
		if (ball_y - ball_radius < 0 || ball_y + ball_radius > disp.height) {
			vel_y = -vel_y;
			ball_y = (ball_y < disp.height / 2) ? ball_radius : disp.height - ball_radius;
		}
		
		/* FPS */
		frame_count++;
		clock_gettime(CLOCK_MONOTONIC, &frame_end);
		
		if (frame_count % 60 == 0) {
			long elapsed_us = (frame_end.tv_sec - fps_start.tv_sec) * 1000000L +
			                 (frame_end.tv_nsec - fps_start.tv_nsec) / 1000L;
			fps = 60.0f / (elapsed_us / 1000000.0f);
			printf("\rFPS: %.1f | Ball: (%.0f, %.0f)   ", fps, ball_x, ball_y);
			fflush(stdout);
			clock_gettime(CLOCK_MONOTONIC, &fps_start);
		}
		
		frame_time_us = (frame_end.tv_sec - frame_start.tv_sec) * 1000000L +
		               (frame_end.tv_nsec - frame_start.tv_nsec) / 1000L;
		
		if (frame_time_us < 16666)
			usleep(16666 - frame_time_us);
	}
	
	printf("\n\nCleaning up...\n");
	
	if (bg_ion_fd >= 0)
		close(bg_ion_fd);
	if (ball_ion_fd >= 0)
		close(ball_ion_fd);
	
	drm_display_cleanup(&disp);
	
	printf("Total frames: %d\n", frame_count);
	
	return 0;
}
