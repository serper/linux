/*
 * G2D Rotation Test Demo
 * Tests all rotation angles: 0°, 90°, 180°, 270°
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/sunxi_g2d.h>
#include <signal.h>
#include <time.h>
#include "demo-drm-base.h"

static volatile int keep_running = 1;

static void signal_handler(int sig)
{
	(void)sig;
	keep_running = 0;
}

/* Allocate ION buffer via DMA-HEAP */
static int alloc_ion_buffer(size_t size)
{
	int heap_fd = open("/dev/dma_heap/default_cma_region", O_RDWR);
	if (heap_fd < 0) {
		perror("Failed to open DMA-HEAP");
		return -1;
	}

	struct dma_heap_allocation_data heap_data = {
		.len = size,
		.fd_flags = O_RDWR | O_CLOEXEC,
		.heap_flags = 0,
	};

	int ret = ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &heap_data);
	close(heap_fd);

	if (ret < 0) {
		perror("DMA_HEAP_IOCTL_ALLOC failed");
		return -1;
	}

	return heap_data.fd;
}

/* Write buffer using G2D_IOC_WRITE_BUFFER */
static int g2d_write_buffer(int g2d_fd, int dma_fd, void *data, size_t size, size_t offset)
{
	struct g2d_buffer_rw buf_rw = {
		.dma_fd = dma_fd,
		.offset = offset,
		.size = size,
		.user_ptr = (__u64)(uintptr_t)data,
	};

	if (ioctl(g2d_fd, G2D_IOC_WRITE_BUFFER, &buf_rw) < 0) {
		perror("G2D_IOC_WRITE_BUFFER");
		return -1;
	}

	return 0;
}

/* Create a directional pattern to make rotation visible:
 * - Red arrow pointing RIGHT
 * - Different colors in each corner for easy identification
 */
static int create_directional_pattern(int g2d_fd, int ion_fd, int width, int height)
{
	uint32_t *pattern = malloc(width * height * 4);
	if (!pattern) {
		perror("malloc pattern");
		return -1;
	}

	/* Clear to black with full alpha */
	memset(pattern, 0, width * height * 4);

	/* Draw colored corners for orientation reference:
	 * Top-Left: RED, Top-Right: GREEN, Bottom-Left: BLUE, Bottom-Right: YELLOW
	 */
	int corner_size = width / 4;
	
	/* Top-Left corner: RED */
	for (int y = 0; y < corner_size; y++) {
		for (int x = 0; x < corner_size; x++) {
			pattern[y * width + x] = 0xFFFF0000; /* ARGB: Red */
		}
	}
	
	/* Top-Right corner: GREEN */
	for (int y = 0; y < corner_size; y++) {
		for (int x = width - corner_size; x < width; x++) {
			pattern[y * width + x] = 0xFF00FF00; /* ARGB: Green */
		}
	}
	
	/* Bottom-Left corner: BLUE */
	for (int y = height - corner_size; y < height; y++) {
		for (int x = 0; x < corner_size; x++) {
			pattern[y * width + x] = 0xFF0000FF; /* ARGB: Blue */
		}
	}
	
	/* Bottom-Right corner: YELLOW */
	for (int y = height - corner_size; y < height; y++) {
		for (int x = width - corner_size; x < width; x++) {
			pattern[y * width + x] = 0xFFFFFF00; /* ARGB: Yellow */
		}
	}
	
	/* Draw a big arrow pointing RIGHT (to verify rotation direction) */
	int arrow_y = height / 2;
	int arrow_head_size = width / 4;
	
	/* Arrow shaft (horizontal line in the middle) */
	for (int x = corner_size; x < width - arrow_head_size; x++) {
		for (int dy = -5; dy <= 5; dy++) {
			int y = arrow_y + dy;
			if (y >= 0 && y < height)
				pattern[y * width + x] = 0xFFFFFFFF; /* White */
		}
	}
	
	/* Arrow head (triangle pointing right) */
	int tip_x = width - corner_size;
	for (int i = 0; i < arrow_head_size; i++) {
		int x = tip_x - i;
		for (int dy = -i; dy <= i; dy++) {
			int y = arrow_y + dy;
			if (x >= 0 && x < width && y >= 0 && y < height)
				pattern[y * width + x] = 0xFFFFFFFF; /* White */
		}
	}

	int ret = g2d_write_buffer(g2d_fd, ion_fd, pattern, width * height * 4, 0);
	free(pattern);
	
	return ret;
}

int main(void)
{
	struct drm_display disp = {0};
	int ret;

	signal(SIGINT, signal_handler);

	printf("G2D Rotation Test Demo\n");
	printf("======================\n\n");

	/* Initialize DRM display */
	ret = drm_display_init(&disp);
	if (ret < 0) {
		fprintf(stderr, "Failed to initialize DRM display\n");
		return 1;
	}

	printf("Display: %dx%d\n", disp.width, disp.height);

	/* Pattern size: 200x200 square */
	int pattern_size = 200;
	int pattern_buffer_size = pattern_size * pattern_size * 4; /* ARGB8888 */

	/* Allocate pattern buffer */
	int pattern_ion_fd = alloc_ion_buffer(pattern_buffer_size);
	if (pattern_ion_fd < 0) {
		fprintf(stderr, "Failed to allocate pattern buffer\n");
		drm_display_cleanup(&disp);
		return 1;
	}
	printf("Pattern buffer allocated: fd=%d size=%d bytes (%dx%d ARGB8888)\n",
	       pattern_ion_fd, pattern_buffer_size, pattern_size, pattern_size);

	/* Create directional pattern */
	ret = create_directional_pattern(disp.g2d_fd, pattern_ion_fd, pattern_size, pattern_size);
	if (ret < 0) {
		fprintf(stderr, "Failed to create pattern\n");
		close(pattern_ion_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	printf("Directional pattern created (arrow pointing RIGHT, colored corners)\n");

	/* Export framebuffer as dmabuf (using page 0) */
	int fb_dmabuf = drm_export_page_dmabuf(&disp, 0);
	if (fb_dmabuf < 0) {
		fprintf(stderr, "Failed to export framebuffer dmabuf\n");
		close(pattern_ion_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	printf("Framebuffer dmabuf exported: fd=%d\n", fb_dmabuf);

	/* Fill background with dark gray */
	struct g2d_fillrect fill = {0};
	fill.dst.width = disp.width;
	fill.dst.height = disp.height;
	fill.dst.format = G2D_FMT_XRGB8888;
	fill.dst.stride[0] = disp.pitch;
	fill.dst.dma_fd = fb_dmabuf;
	fill.dst_x = 0;
	fill.dst_y = 0;
	fill.dst_w = disp.width;
	fill.dst_h = disp.height;
	fill.color = 0xFF303030; /* Dark gray */
	fill.color_format = G2D_FMT_ARGB8888;
	fill.fence_fd_in = -1;
	fill.fence_fd_out = -1;

	ret = ioctl(disp.g2d_fd, G2D_IOC_FILLRECT, &fill);
	if (ret < 0) {
		perror("G2D_IOC_FILLRECT (background)");
	} else if (fill.fence_fd_out >= 0) {
		close(fill.fence_fd_out);
	}

	printf("\nTesting rotations:\n");
	printf("  Top-Left:     0° (original - arrow RIGHT)\n");
	printf("  Top-Right:   90° (arrow UP)\n");
	printf("  Bottom-Left: 180° (arrow LEFT)\n");
	printf("  Bottom-Right: 270° (arrow DOWN)\n\n");

	/* Position and spacing */
	int spacing = 50;
	int x_positions[4] = {
		spacing,                          /* Top-Left: 0° */
		disp.width / 2 + spacing,        /* Top-Right: 90° */
		spacing,                          /* Bottom-Left: 180° */
		disp.width / 2 + spacing         /* Bottom-Right: 270° */
	};
	int y_positions[4] = {
		spacing,                          /* Top-Left: 0° */
		spacing,                          /* Top-Right: 90° */
		disp.height - pattern_size - spacing,  /* Bottom-Left: 180° - FIX */
		disp.height - pattern_size - spacing   /* Bottom-Right: 270° - FIX */
	};
	uint32_t rotation_flags[4] = {
		0,                               /* 0° */
		G2D_BLIT_FLAG_ROTATE_90,        /* 90° */
		G2D_BLIT_FLAG_ROTATE_180,       /* 180° */
		G2D_BLIT_FLAG_ROTATE_270        /* 270° */
	};
	const char *rotation_names[4] = {"0°", "90°", "180°", "270°"};

	/* Blit pattern with each rotation */
	for (int i = 0; i < 4; i++) {
		struct g2d_blit blit = {0};
		
		/* Source: directional pattern */
		blit.src.width = pattern_size;
		blit.src.height = pattern_size;
		blit.src.format = G2D_FMT_ARGB8888;
		blit.src.stride[0] = pattern_size * 4;
		blit.src.dma_fd = pattern_ion_fd;
		blit.src.crop_x = 0;
		blit.src.crop_y = 0;
		blit.src.crop_w = pattern_size;
		blit.src.crop_h = pattern_size;
		blit.src.alpha = 255;
		blit.src.alpha_mode = G2D_PIXEL_ALPHA;
		
		/* Destination: framebuffer */
		blit.dst.width = disp.width;
		blit.dst.height = disp.height;
		blit.dst.format = G2D_FMT_XRGB8888;
		blit.dst.stride[0] = disp.pitch;
		blit.dst.dma_fd = fb_dmabuf;
		
		blit.dst_x = x_positions[i];
		blit.dst_y = y_positions[i];
		blit.dst_w = pattern_size;
		blit.dst_h = pattern_size;
		
		blit.flags = rotation_flags[i];
		blit.fence_fd_in = -1;
		blit.fence_fd_out = -1;
		blit.out.dma_fd = -1; /* 2-buffer mode */
		
		printf("Blitting with rotation %s at position (%d, %d)...\n",
		       rotation_names[i], x_positions[i], y_positions[i]);
		
		ret = ioctl(disp.g2d_fd, G2D_IOC_BLIT, &blit);
		if (ret < 0) {
			perror("G2D_IOC_BLIT");
			fprintf(stderr, "Failed rotation %s\n", rotation_names[i]);
		} else {
			if (blit.fence_fd_out >= 0)
				close(blit.fence_fd_out);
			printf("  ✓ Rotation %s completed successfully\n", rotation_names[i]);
		}
	}

	printf("\nAll rotations rendered. Check the display:\n");
	printf("- Top-Left corner should show RED in top-left\n");
	printf("- After 90° rotation, RED should be in top-right\n");
	printf("- After 180° rotation, RED should be in bottom-right\n");
	printf("- After 270° rotation, RED should be in bottom-left\n");
	printf("\nPress Ctrl+C to exit...\n");

	/* Wait for user interrupt */
	while (keep_running) {
		sleep(1);
	}

	printf("\nCleaning up...\n");
	close(fb_dmabuf);
	close(pattern_ion_fd);
	drm_display_cleanup(&disp);
	
	printf("Demo exited cleanly\n");
	return 0;
}
