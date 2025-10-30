/*
 * test-alpha.c - Test alpha blending operations
 * Creates two solid color layers and blends them with different alpha values
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>

/* G2D IOCTL definitions */
#define G2D_IOC_MAGIC 'G'
#define G2D_IOC_BLIT     _IOWR(G2D_IOC_MAGIC, 1, struct g2d_blit)
#define G2D_IOC_FILLRECT _IOWR(G2D_IOC_MAGIC, 2, struct g2d_fillrect)

/* Format definitions */
#define G2D_FMT_ARGB8888 0
#define G2D_FMT_XRGB8888 4

/* Blit flags */
#define G2D_BLIT_FLAG_ALPHA_BLEND  (1 << 0)

struct g2d_buf {
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint32_t stride[3];
	
	int32_t dma_fd;
	uint64_t paddr[3];
	
	uint32_t crop_x;
	uint32_t crop_y;
	uint32_t crop_w;
	uint32_t crop_h;
};

struct g2d_fillrect {
	struct g2d_buf dst;
	uint32_t dst_x, dst_y, dst_w, dst_h;
	uint32_t color;
	int32_t fence_fd_in;
	int32_t fence_fd_out;
};

struct g2d_blit {
	struct g2d_buf src;
	struct g2d_buf dst;
	uint32_t dst_x, dst_y, dst_w, dst_h;
	uint32_t flags;
	uint32_t global_alpha;  /* 0-255 */
	int32_t fence_fd_in;
	int32_t fence_fd_out;
};

/* Allocate DMA buffer */
int alloc_dma_buffer(int heap_fd, uint32_t size, int *dma_fd) {
	struct dma_heap_allocation_data alloc = {
		.len = size,
		.fd = 0,
		.fd_flags = O_RDWR | O_CLOEXEC,
		.heap_flags = 0,
	};
	
	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
		perror("DMA heap allocation failed");
		return -1;
	}
	
	*dma_fd = alloc.fd;
	return 0;
}

/* Sample color from center of buffer */
uint32_t sample_center(void *buffer, uint32_t width, uint32_t height) {
	uint32_t *pixels = (uint32_t *)buffer;
	return pixels[(height / 2) * width + (width / 2)];
}

/* Calculate expected blend result (simplified, pixel alpha ignored) */
uint32_t calculate_expected(uint32_t fg_color, uint32_t bg_color, uint32_t alpha) {
	uint8_t fg_r = (fg_color >> 16) & 0xFF;
	uint8_t fg_g = (fg_color >> 8) & 0xFF;
	uint8_t fg_b = fg_color & 0xFF;
	
	uint8_t bg_r = (bg_color >> 16) & 0xFF;
	uint8_t bg_g = (bg_color >> 8) & 0xFF;
	uint8_t bg_b = bg_color & 0xFF;
	
	/* dst = src*alpha + dst*(1-alpha) */
	uint8_t out_r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
	uint8_t out_g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
	uint8_t out_b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;
	
	return 0xFF000000 | (out_r << 16) | (out_g << 8) | out_b;
}

/* Check if colors are close (allow tolerance for rounding) */
int colors_match(uint32_t got, uint32_t expected, int tolerance) {
	int dr = abs((int)((got >> 16) & 0xFF) - (int)((expected >> 16) & 0xFF));
	int dg = abs((int)((got >> 8) & 0xFF) - (int)((expected >> 8) & 0xFF));
	int db = abs((int)(got & 0xFF) - (int)(expected & 0xFF));
	
	return (dr <= tolerance && dg <= tolerance && db <= tolerance);
}

int main(void) {
	int g2d_fd, heap_fd;
	int src_fd = -1, dst_fd = -1, result_fd = -1;
	void *src_map = NULL, *dst_map = NULL, *result_map = NULL;
	const uint32_t width = 128;
	const uint32_t height = 128;
	const uint32_t size = width * height * 4;  /* ARGB8888 */
	int passed = 0, failed = 0;
	
	printf("G2D Alpha Blending Test\n");
	printf("========================\n");
	printf("Image size: %ux%u (ARGB8888)\n\n", width, height);
	
	/* Open devices */
	g2d_fd = open("/dev/g2d", O_RDWR);
	if (g2d_fd < 0) {
		perror("Failed to open /dev/g2d");
		return 1;
	}
	
	heap_fd = open("/dev/dma_heap/default_cma_region", O_RDONLY);
	if (heap_fd < 0) {
		perror("Failed to open DMA heap");
		close(g2d_fd);
		return 1;
	}
	
	/* Allocate buffers */
	if (alloc_dma_buffer(heap_fd, size, &src_fd) < 0 ||
	    alloc_dma_buffer(heap_fd, size, &dst_fd) < 0 ||
	    alloc_dma_buffer(heap_fd, size, &result_fd) < 0) {
		goto cleanup;
	}
	
	/* Map buffers */
	src_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, src_fd, 0);
	dst_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dst_fd, 0);
	result_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, result_fd, 0);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED || result_map == MAP_FAILED) {
		perror("mmap failed");
		goto cleanup;
	}
	
	/* Test colors */
	struct {
		const char *name;
		uint32_t fg_color;  /* Foreground (source) */
		uint32_t bg_color;  /* Background (destination) */
		uint32_t alpha;
	} tests[] = {
		{"Red on Blue (50%)", 0xFFFF0000, 0xFF0000FF, 128},
		{"Green on Red (25%)", 0xFF00FF00, 0xFFFF0000, 64},
		{"White on Black (75%)", 0xFFFFFFFF, 0xFF000000, 192},
		{"Yellow on Cyan (100%)", 0xFFFFFF00, 0xFF00FFFF, 255},
		{"Orange on Purple (0%)", 0xFFFF8000, 0xFF8000FF, 0},
	};
	
	for (int i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
		printf("Test %d: %s\n", i + 1, tests[i].name);
		printf("  FG=0x%08X BG=0x%08X alpha=%u\n",
		       tests[i].fg_color, tests[i].bg_color, tests[i].alpha);
		
		/* Fill source buffer with foreground color */
		memset(src_map, 0, size);
		for (uint32_t j = 0; j < width * height; j++) {
			((uint32_t *)src_map)[j] = tests[i].fg_color;
		}
		
		/* Fill destination buffer with background color */
		memset(dst_map, 0, size);
		for (uint32_t j = 0; j < width * height; j++) {
			((uint32_t *)dst_map)[j] = tests[i].bg_color;
		}
		
		/* Copy dst to result (we'll blend src into result, keeping dst pristine) */
		memcpy(result_map, dst_map, size);
		
		/* Perform alpha blend: src blended over result */
		struct g2d_blit blit = {
			.src = {
				.width = width,
				.height = height,
				.format = G2D_FMT_ARGB8888,
				.stride = {0, 0, 0},
				.dma_fd = src_fd,
				.paddr = {0, 0, 0},
				.crop_x = 0,
				.crop_y = 0,
				.crop_w = width,
				.crop_h = height,
			},
			.dst = {
				.width = width,
				.height = height,
				.format = G2D_FMT_ARGB8888,
				.stride = {0, 0, 0},
				.dma_fd = result_fd,
				.paddr = {0, 0, 0},
				.crop_x = 0,
				.crop_y = 0,
				.crop_w = 0,
				.crop_h = 0,
			},
			.dst_x = 0,
			.dst_y = 0,
			.dst_w = width,
			.dst_h = height,
			.flags = G2D_BLIT_FLAG_ALPHA_BLEND,
			.global_alpha = tests[i].alpha,
			.fence_fd_in = -1,
			.fence_fd_out = -1,
		};
		
		if (ioctl(g2d_fd, G2D_IOC_BLIT, &blit) < 0) {
			perror("  BLIT failed");
			printf("  ✗ FAILED\n\n");
			failed++;
			continue;
		}
		
		/* Sample result */
		uint32_t result_color = sample_center(result_map, width, height);
		uint32_t expected = calculate_expected(tests[i].fg_color,
							tests[i].bg_color,
							tests[i].alpha);
		
		printf("  Result:   0x%08X\n", result_color);
		printf("  Expected: 0x%08X\n", expected);
		
		if (colors_match(result_color, expected, 2)) {
			printf("  ✓ PASSED\n\n");
			passed++;
		} else {
			printf("  ✗ FAILED (color mismatch)\n\n");
			failed++;
		}
	}
	
	/* Summary */
	printf("========================\n");
	printf("Test Results: %d passed, %d failed\n", passed, failed);
	
cleanup:
	if (src_map && src_map != MAP_FAILED) munmap(src_map, size);
	if (dst_map && dst_map != MAP_FAILED) munmap(dst_map, size);
	if (result_map && result_map != MAP_FAILED) munmap(result_map, size);
	if (src_fd >= 0) close(src_fd);
	if (dst_fd >= 0) close(dst_fd);
	if (result_fd >= 0) close(result_fd);
	close(heap_fd);
	close(g2d_fd);
	
	return (failed == 0) ? 0 : 1;
}
