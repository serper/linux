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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <linux/sunxi_g2d.h>

/* Declare functions from demo-drm-base.c */
struct drm_display;
int drm_display_init(struct drm_display *disp);
void drm_display_cleanup(struct drm_display *disp);
void *drm_get_backbuffer(struct drm_display *disp);
uint32_t drm_get_backbuffer_offset(struct drm_display *disp);
int drm_flip_page(struct drm_display *disp);
int drm_export_dmabuf(struct drm_display *disp);
int g2d_fillrect_display(struct drm_display *disp, int x, int y, int w, int h, uint32_t color);
int g2d_clear_backbuffer(struct drm_display *disp, uint32_t color);

/* Provided by demo-drm-base.c */
extern struct drm_display {
	int drm_fd;
	int g2d_fd;
	uint32_t conn_id;
	uint32_t crtc_id;
	uint32_t fb_id;
	void *mode_placeholder[20]; /* drmModeModeInfo */
	void *saved_crtc;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t size;
	void *map;
	uint32_t handle;
	int current_page;
};

static volatile int keep_running = 1;

void sigint_handler(int sig)
{
	(void)sig;
	keep_running = 0;
}

/*
 * Alpha blend a ball onto background using G2D
 */
int g2d_blend_ball(struct drm_display *disp, int x, int y, int radius, 
                   uint32_t ball_color, uint8_t alpha)
{
	struct g2d_alpha_blend blend = {0};
	int dmabuf_fd;
	int ret;
	int ball_size = radius * 2;
	
	/* Export DRM buffer as DMA-BUF */
	dmabuf_fd = drm_export_dmabuf(disp);
	if (dmabuf_fd < 0)
		return -1;
	
	/* Background (destination) = current backbuffer */
	blend.dst.width = disp->width;
	blend.dst.height = disp->height * 2;
	blend.dst.format = G2D_FMT_XRGB8888;
	blend.dst.stride[0] = disp->pitch;
	blend.dst.dma_fd = dmabuf_fd;
	blend.dst.crop_x = x;
	blend.dst.crop_y = y + (drm_get_backbuffer_offset(disp) / disp->pitch);
	blend.dst.crop_w = ball_size;
	blend.dst.crop_h = ball_size;
	
	/* Foreground (source) = same buffer, use different region as temp */
	/* We'll use a small region at bottom of page 1 as scratch space for the ball */
	int scratch_y = disp->height * 2 - ball_size - 10;
	
	/* First, draw the ball into scratch area */
	ret = g2d_fillrect_display(disp, 0, scratch_y - disp->height, 
	                           ball_size, ball_size, ball_color);
	if (ret < 0) {
		close(dmabuf_fd);
		return -1;
	}
	
	/* Now blend the scratch ball onto the backbuffer */
	blend.src.width = disp->width;
	blend.src.height = disp->height * 2;
	blend.src.format = G2D_FMT_XRGB8888;
	blend.src.stride[0] = disp->pitch;
	blend.src.dma_fd = dmabuf_fd;
	blend.src.crop_x = 0;
	blend.src.crop_y = scratch_y;
	blend.src.crop_w = ball_size;
	blend.src.crop_h = ball_size;
	
	blend.global_alpha = alpha;
	blend.fence_fd_in = -1;
	
	ret = ioctl(disp->g2d_fd, G2D_IOC_ALPHA_BLEND, &blend);
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
	
	printf("G2D + DRM Bouncing Ball Demo\n");
	printf("sizeof(struct g2d_buf) = %zu\n", sizeof(struct g2d_buf));
	printf("sizeof(struct g2d_fillrect) = %zu\n", sizeof(struct g2d_fillrect));
	
	/* Ball physics */
	float ball_x, ball_y;
	float vel_x = 3.5f;
	float vel_y = 2.8f;
	int ball_radius = 40;
	uint32_t ball_color = 0xFFFF0000;  /* Red */
	uint8_t ball_alpha = 220;  /* 86% opaque */
	
	/* Background gradient colors */
	uint32_t bg_top = 0xFF001020;     /* Dark blue */
	uint32_t bg_bottom = 0xFF002040;  /* Slightly lighter blue */
	
	/* FPS tracking */
	struct timespec frame_start, frame_end;
	long frame_time_us;
	int frame_count = 0;
	float fps = 0.0f;
	struct timespec fps_start;
	
	signal(SIGINT, sigint_handler);
	
	printf("==============================\n\n");
	
	ret = drm_display_init(&disp);
	if (ret < 0) {
		fprintf(stderr, "Failed to initialize DRM display\n");
		return 1;
	}
	
	/* Initialize ball position */
	ball_x = disp.width / 2.0f;
	ball_y = disp.height / 2.0f;
	
	printf("\nPress Ctrl+C to exit\n\n");
	clock_gettime(CLOCK_MONOTONIC, &fps_start);
	
	while (keep_running) {
		clock_gettime(CLOCK_MONOTONIC, &frame_start);
		
		/* Clear backbuffer with gradient (simulate with two rects) */
		g2d_fillrect_display(&disp, 0, 0, disp.width, disp.height / 2, bg_top);
		g2d_fillrect_display(&disp, 0, disp.height / 2, disp.width, disp.height / 2, bg_bottom);
		
		/* Draw ball with alpha blending */
		ret = g2d_blend_ball(&disp, (int)(ball_x - ball_radius), 
		                    (int)(ball_y - ball_radius),
		                    ball_radius, ball_color, ball_alpha);
		if (ret < 0) {
			fprintf(stderr, "Failed to draw ball\n");
			break;
		}
		
		/* Flip page (tear-free display) */
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
			printf("\rFPS: %.1f | Ball: (%.0f, %.0f) | Alpha: %d", 
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
	drm_display_cleanup(&disp);
	
	printf("Total frames: %d\n", frame_count);
	printf("Demo exited cleanly\n");
	
	return 0;
}
