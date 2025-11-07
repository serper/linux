/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * G2D Chromakey (Green Screen) Demo
 * 
 * Demonstrates hardware chromakey/color keying for green screen effects:
 * 1. Creates a rainbow gradient background
 * 2. Creates foreground with white objects on green background
 * 3. Composites foreground over background using chromakey (green becomes transparent)
 * 
 * Copyright (C) 2025 Sergio Perez
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <sys/ioctl.h>
#include <linux/sunxi_g2d.h>
#include "demo-drm-base.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Chromakey range for green (0x00RRGGBB) */
#define GREEN_MIN 0x00C000  /* Darker green */
#define GREEN_MAX 0x00FF40  /* Brighter green with tolerance */

/* HSV to RGB conversion helper */
static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b)
{
	float c = v * s;
	float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
	float m = v - c;
	float r1, g1, b1;

	if (h < 60) {
		r1 = c; g1 = x; b1 = 0;
	} else if (h < 120) {
		r1 = x; g1 = c; b1 = 0;
	} else if (h < 180) {
		r1 = 0; g1 = c; b1 = x;
	} else if (h < 240) {
		r1 = 0; g1 = x; b1 = c;
	} else if (h < 300) {
		r1 = x; g1 = 0; b1 = c;
	} else {
		r1 = c; g1 = 0; b1 = x;
	}

	*r = (uint8_t)((r1 + m) * 255);
	*g = (uint8_t)((g1 + m) * 255);
	*b = (uint8_t)((b1 + m) * 255);
}

/* Create rainbow gradient background (Red->Yellow->Green->Cyan->Blue->Magenta) */
static void create_gradient_background(uint32_t *buffer, int width, int height)
{
	for (int y = 0; y < height; y++) {
		float hue = (float)y / (float)height * 360.0f;  /* 0-360 degrees */
		uint8_t r, g, b;
		hsv_to_rgb(hue, 1.0f, 1.0f, &r, &g, &b);
		
		uint32_t color = 0xFF000000 | (r << 16) | (g << 8) | b;  /* ARGB8888 */
		
		for (int x = 0; x < width; x++) {
			buffer[y * width + x] = color;
		}
	}
}

/* Create foreground with white objects on green background */
static void create_foreground_greenscreen(uint32_t *buffer, int width, int height)
{
	/* Fill with pure green (0xFF00FF00 in ARGB8888) */
	for (int i = 0; i < width * height; i++) {
		buffer[i] = 0xFF00FF00;  /* Green background */
	}
	
	/* Draw white circle in center */
	int center_x = width / 2;
	int center_y = height / 2;
	int radius = height / 3;
	
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			int dx = x - center_x;
			int dy = y - center_y;
			if (dx * dx + dy * dy < radius * radius) {
				buffer[y * width + x] = 0xFFFFFFFF;  /* White */
			}
		}
	}
	
	/* Draw white squares in corners (60x60 each) */
	int sq_size = 60;
	
	/* Top-left square */
	for (int y = 0; y < sq_size; y++) {
		for (int x = 0; x < sq_size; x++) {
			buffer[y * width + x] = 0xFFFFFFFF;
		}
	}
	
	/* Top-right square */
	for (int y = 0; y < sq_size; y++) {
		for (int x = width - sq_size; x < width; x++) {
			buffer[y * width + x] = 0xFFFFFFFF;
		}
	}
	
	/* Bottom-left square */
	for (int y = height - sq_size; y < height; y++) {
		for (int x = 0; x < sq_size; x++) {
			buffer[y * width + x] = 0xFFFFFFFF;
		}
	}
	
	/* Bottom-right square */
	for (int y = height - sq_size; y < height; y++) {
		for (int x = width - sq_size; x < width; x++) {
			buffer[y * width + x] = 0xFFFFFFFF;
		}
	}
}

int main(void)
{
	int ret;
	struct drm_display disp = {0};
	int bg_dmabuf_fd = -1, fg_dmabuf_fd = -1, fb_dmabuf_fd = -1;
	
	printf("=== G2D Chromakey Demo (Green Screen) ===\n\n");
	
	/* Initialize DRM */
	ret = drm_display_init(&disp);
	if (ret < 0) {
		fprintf(stderr, "Failed to initialize DRM display\n");
		return 1;
	}
	
	printf("Display: %ux%u\n\n", disp.width, disp.height);
	
	/* Export framebuffer as DMA-BUF */
	fb_dmabuf_fd = drm_export_dmabuf(&disp);
	if (fb_dmabuf_fd < 0) {
		fprintf(stderr, "Failed to export framebuffer as DMA-BUF\n");
		drm_display_cleanup(&disp);
		return 1;
	}
	printf("Framebuffer DMA-BUF: fd=%d\n", fb_dmabuf_fd);
	
	/* Allocate background buffer */
	struct g2d_alloc_buffer bg_alloc = {0};
	bg_alloc.size = disp.width * disp.height * 4;  /* ARGB8888 */
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &bg_alloc);
	if (ret < 0) {
		fprintf(stderr, "Failed to allocate background buffer\n");
		perror("G2D_IOC_ALLOC_BUFFER (background)");
		drm_display_cleanup(&disp);
		return 1;
	}
	bg_dmabuf_fd = bg_alloc.dma_fd;
	printf("Background buffer: fd=%d, size=%llu bytes\n", bg_dmabuf_fd, bg_alloc.size);
	
	/* Allocate foreground buffer */
	struct g2d_alloc_buffer fg_alloc = {0};
	fg_alloc.size = disp.width * disp.height * 4;  /* ARGB8888 */
	ret = ioctl(disp.g2d_fd, G2D_IOC_ALLOC_BUFFER, &fg_alloc);
	if (ret < 0) {
		fprintf(stderr, "Failed to allocate foreground buffer\n");
		perror("G2D_IOC_ALLOC_BUFFER (foreground)");
		close(bg_dmabuf_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	fg_dmabuf_fd = fg_alloc.dma_fd;
	printf("Foreground buffer: fd=%d, size=%llu bytes\n\n", fg_dmabuf_fd, fg_alloc.size);
	
	/* Allocate local buffers for pattern creation */
	uint32_t *bg_pattern = malloc(disp.width * disp.height * 4);
	uint32_t *fg_pattern = malloc(disp.width * disp.height * 4);
	if (!bg_pattern || !fg_pattern) {
		fprintf(stderr, "Failed to allocate pattern buffers\n");
		close(fg_dmabuf_fd);
		close(bg_dmabuf_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	
	printf("Creating test patterns...\n");
	create_gradient_background(bg_pattern, disp.width, disp.height);
	create_foreground_greenscreen(fg_pattern, disp.width, disp.height);
	
	/* Write patterns to DMA-BUF buffers via G2D */
	printf("Writing patterns to buffers...\n");
	
	struct g2d_buffer_rw bg_write = {
		.dma_fd = bg_dmabuf_fd,
		.offset = 0,
		.size = disp.width * disp.height * 4,
		.user_ptr = (__u64)(uintptr_t)bg_pattern,
	};
	ret = ioctl(disp.g2d_fd, G2D_IOC_WRITE_BUFFER, &bg_write);
	if (ret < 0) {
		fprintf(stderr, "Failed to write background pattern\n");
		perror("G2D_IOC_WRITE_BUFFER");
		goto cleanup;
	}
	
	struct g2d_buffer_rw fg_write = {
		.dma_fd = fg_dmabuf_fd,
		.offset = 0,
		.size = disp.width * disp.height * 4,
		.user_ptr = (__u64)(uintptr_t)fg_pattern,
	};
	ret = ioctl(disp.g2d_fd, G2D_IOC_WRITE_BUFFER, &fg_write);
	if (ret < 0) {
		fprintf(stderr, "Failed to write foreground pattern\n");
		perror("G2D_IOC_WRITE_BUFFER");
		goto cleanup;
	}
	
	/* Free local pattern buffers */
	free(bg_pattern);
	free(fg_pattern);
	bg_pattern = NULL;
	fg_pattern = NULL;
	
	printf("\nStep 1: Copying background gradient to framebuffer...\n");
	
	/* Copy background to framebuffer */
	struct g2d_blit bg_blit = {
		.src = {
			.dma_fd = bg_dmabuf_fd,
			.width = disp.width,
			.height = disp.height,
			.format = G2D_FMT_ARGB8888,
		},
		.dst = {
			.dma_fd = fb_dmabuf_fd,
			.width = disp.width,
			.height = disp.height,
			.format = G2D_FMT_XRGB8888,
		},
		.out = { .dma_fd = -1 },  /* In-place operation */
		.dst_x = 0,
		.dst_y = 0,
		.dst_w = disp.width,
		.dst_h = disp.height,
		.bld_mode = G2D_BLD_COPY,
		.fence_fd_in = -1,
		.fence_fd_out = -1,
	};
	
	ret = ioctl(disp.g2d_fd, G2D_IOC_BLIT, &bg_blit);
	if (ret < 0) {
		fprintf(stderr, "Failed to blit background\n");
		perror("G2D_IOC_BLIT (background)");
		goto cleanup;
	}
	
	printf("Step 2: Compositing foreground with chromakey (green -> transparent)...\n");
	
	/* Composite foreground with chromakey */
	struct g2d_blit fg_blit = {
		.src = {
			.dma_fd = fg_dmabuf_fd,
			.width = disp.width,
			.height = disp.height,
			.format = G2D_FMT_ARGB8888,
		},
		.dst = {
			.dma_fd = fb_dmabuf_fd,
			.width = disp.width,
			.height = disp.height,
			.format = G2D_FMT_XRGB8888,
		},
		.out = { .dma_fd = -1 },  /* In-place operation */
		.dst_x = 0,
		.dst_y = 0,
		.dst_w = disp.width,
		.dst_h = disp.height,
		.bld_mode = G2D_BLD_SRCOVER,
		
		/* Chromakey configuration */
		.color_key_enable = 1,
		.color_key_mode = 0,  /* 0 = inside range transparent */
		.color_key_min = GREEN_MIN,
		.color_key_max = GREEN_MAX,
		
		.fence_fd_in = -1,
		.fence_fd_out = -1,
	};
	
	ret = ioctl(disp.g2d_fd, G2D_IOC_BLIT, &fg_blit);
	if (ret < 0) {
		fprintf(stderr, "Failed to blit foreground with chromakey\n");
		perror("G2D_IOC_BLIT (chromakey)");
		goto cleanup;
	}
	
	printf("\nChromakey demo complete!\n");
	printf("You should see white circle and squares on a rainbow background.\n");
	printf("Green areas are transparent (showing rainbow through).\n");
	printf("\nPress Ctrl+C to exit...\n");
	
	/* Keep display active */
	while (1) {
		sleep(1);
	}
	
cleanup:
	if (bg_pattern) free(bg_pattern);
	if (fg_pattern) free(fg_pattern);
	if (fg_dmabuf_fd >= 0) close(fg_dmabuf_fd);
	if (bg_dmabuf_fd >= 0) close(bg_dmabuf_fd);
	if (fb_dmabuf_fd >= 0) close(fb_dmabuf_fd);
	drm_display_cleanup(&disp);
	
	return ret < 0 ? 1 : 0;
}
