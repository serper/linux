/*
 * test-rotation.c - Test rotation and flip operations
 * Tests 90°, 180°, 270° rotations and H/V flips
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
#define G2D_FMT_RGB888   8
#define G2D_FMT_RGB565   10

/* Blit flags */
#define G2D_BLIT_FLAG_ROTATE_90    (1 << 1)
#define G2D_BLIT_FLAG_ROTATE_180   (1 << 2)
#define G2D_BLIT_FLAG_ROTATE_270   (1 << 3)
#define G2D_BLIT_FLAG_FLIP_H       (1 << 4)
#define G2D_BLIT_FLAG_FLIP_V       (1 << 5)

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
};

struct g2d_blit {
	struct g2d_buf src;
	struct g2d_buf dst;
	uint32_t dst_x, dst_y, dst_w, dst_h;
	uint32_t flags;
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

/* Create a simple pattern: gradient with colored corner markers */
void create_test_pattern(void *buffer, uint32_t width, uint32_t height) {
	uint32_t *pixels = (uint32_t *)buffer;
	
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			uint32_t color;
			
			/* Corner markers (8x8 pixels each) */
			if (x < 8 && y < 8) {
				/* Top-left: RED */
				color = 0xFFFF0000;
			} else if (x >= width - 8 && y < 8) {
				/* Top-right: GREEN */
				color = 0xFF00FF00;
			} else if (x < 8 && y >= height - 8) {
				/* Bottom-left: BLUE */
				color = 0xFF0000FF;
			} else if (x >= width - 8 && y >= height - 8) {
				/* Bottom-right: YELLOW */
				color = 0xFFFFFF00;
			} else {
				/* Gradient: darker at top, brighter at bottom */
				uint8_t val = (y * 255) / height;
				color = 0xFF000000 | (val << 16) | (val << 8) | val;
			}
			
			pixels[y * width + x] = color;
		}
	}
}

/* Verify rotation result by checking corner markers */
int verify_rotation(void *buffer, uint32_t width, uint32_t height, const char *rotation_name,
                    uint32_t expected_tl, uint32_t expected_tr, 
                    uint32_t expected_bl, uint32_t expected_br) {
	uint32_t *pixels = (uint32_t *)buffer;
	
	/* Sample one pixel from each corner */
	uint32_t tl = pixels[4 * width + 4];  /* Top-left */
	uint32_t tr = pixels[4 * width + (width - 5)];  /* Top-right */
	uint32_t bl = pixels[(height - 5) * width + 4];  /* Bottom-left */
	uint32_t br = pixels[(height - 5) * width + (width - 5)];  /* Bottom-right */
	
	printf("  %s - Corners: TL=0x%08X TR=0x%08X BL=0x%08X BR=0x%08X\n",
	       rotation_name, tl, tr, bl, br);
	
	/* Check if corners match expected colors */
	if (tl == expected_tl && tr == expected_tr && 
	    bl == expected_bl && br == expected_br) {
		printf("  ✓ %s verification PASSED\n", rotation_name);
		return 1;
	} else {
		printf("  ✗ %s verification FAILED\n", rotation_name);
		printf("    Expected: TL=0x%08X TR=0x%08X BL=0x%08X BR=0x%08X\n",
		       expected_tl, expected_tr, expected_bl, expected_br);
		return 0;
	}
}

int main(void) {
	int g2d_fd, heap_fd;
	int src_fd = -1, dst_fd = -1;
	void *src_map = NULL, *dst_map = NULL;
	const uint32_t width = 64;
	const uint32_t height = 64;
	const uint32_t size = width * height * 4;  /* ARGB8888 */
	int passed = 0, failed = 0;
	
	printf("G2D Rotation/Flip Test\n");
	printf("======================\n");
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
	    alloc_dma_buffer(heap_fd, size, &dst_fd) < 0) {
		goto cleanup;
	}
	
	/* Map buffers */
	src_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, src_fd, 0);
	dst_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dst_fd, 0);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
		perror("mmap failed");
		goto cleanup;
	}
	
	/* Create test pattern */
	create_test_pattern(src_map, width, height);
	printf("Created test pattern with colored corner markers:\n");
	printf("  Top-left: RED, Top-right: GREEN, Bottom-left: BLUE, Bottom-right: YELLOW\n\n");
	
	/* Test 1: 90° rotation */
	printf("Test 1: 90° clockwise rotation\n");
	memset(dst_map, 0, size);
	
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
			.dma_fd = dst_fd,
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
		.flags = G2D_BLIT_FLAG_ROTATE_90,
		.fence_fd_in = -1,
		.fence_fd_out = -1,
	};
	
	if (ioctl(g2d_fd, G2D_IOC_BLIT, &blit) < 0) {
		perror("  BLIT 90° failed");
		failed++;
	} else {
		/* After 90° CW: TL→TR, TR→BR, BR→BL, BL→TL */
		if (verify_rotation(dst_map, width, height, "90° CW",
		                    0xFF0000FF, 0xFFFF0000,  /* TL=BLUE, TR=RED */
		                    0xFFFFFF00, 0xFF00FF00)) /* BL=YELLOW, BR=GREEN */
			passed++;
		else
			failed++;
	}
	
	/* Test 2: 180° rotation */
	printf("\nTest 2: 180° rotation\n");
	memset(dst_map, 0, size);
	blit.flags = G2D_BLIT_FLAG_ROTATE_180;
	
	if (ioctl(g2d_fd, G2D_IOC_BLIT, &blit) < 0) {
		perror("  BLIT 180° failed");
		failed++;
	} else {
		/* After 180°: TL→BR, TR→BL, BR→TL, BL→TR */
		if (verify_rotation(dst_map, width, height, "180°",
		                    0xFFFFFF00, 0xFF0000FF,  /* TL=YELLOW, TR=BLUE */
		                    0xFF00FF00, 0xFFFF0000)) /* BL=GREEN, BR=RED */
			passed++;
		else
			failed++;
	}
	
	/* Test 3: 270° rotation */
	printf("\nTest 3: 270° clockwise rotation\n");
	memset(dst_map, 0, size);
	blit.flags = G2D_BLIT_FLAG_ROTATE_270;
	
	if (ioctl(g2d_fd, G2D_IOC_BLIT, &blit) < 0) {
		perror("  BLIT 270° failed");
		failed++;
	} else {
		/* After 270° CW: TL→BL, BL→BR, BR→TR, TR→TL */
		if (verify_rotation(dst_map, width, height, "270° CW",
		                    0xFF00FF00, 0xFFFFFF00,  /* TL=GREEN, TR=YELLOW */
		                    0xFFFF0000, 0xFF0000FF)) /* BL=RED, BR=BLUE */
			passed++;
		else
			failed++;
	}
	
	/* Test 4: Horizontal flip */
	printf("\nTest 4: Horizontal flip\n");
	memset(dst_map, 0, size);
	blit.flags = G2D_BLIT_FLAG_FLIP_H;
	
	if (ioctl(g2d_fd, G2D_IOC_BLIT, &blit) < 0) {
		perror("  BLIT H-flip failed");
		failed++;
	} else {
		/* After H-flip: TL→TR, TR→TL, BL→BR, BR→BL */
		if (verify_rotation(dst_map, width, height, "H-flip",
		                    0xFF00FF00, 0xFFFF0000,  /* TL=GREEN, TR=RED */
		                    0xFFFFFF00, 0xFF0000FF)) /* BL=YELLOW, BR=BLUE */
			passed++;
		else
			failed++;
	}
	
	/* Test 5: Vertical flip */
	printf("\nTest 5: Vertical flip\n");
	memset(dst_map, 0, size);
	blit.flags = G2D_BLIT_FLAG_FLIP_V;
	
	if (ioctl(g2d_fd, G2D_IOC_BLIT, &blit) < 0) {
		perror("  BLIT V-flip failed");
		failed++;
	} else {
		/* After V-flip: TL→BL, BL→TL, TR→BR, BR→TR */
		if (verify_rotation(dst_map, width, height, "V-flip",
		                    0xFF0000FF, 0xFFFFFF00,  /* TL=BLUE, TR=YELLOW */
		                    0xFFFF0000, 0xFF00FF00)) /* BL=RED, BR=GREEN */
			passed++;
		else
			failed++;
	}
	
	/* Test 6: Combined rotation + flip (should work if supported) */
	printf("\nTest 6: 90° + H-flip\n");
	memset(dst_map, 0, size);
	blit.flags = G2D_BLIT_FLAG_ROTATE_90 | G2D_BLIT_FLAG_FLIP_H;
	
	if (ioctl(g2d_fd, G2D_IOC_BLIT, &blit) < 0) {
		printf("  Combined rotation+flip not supported (expected)\n");
	} else {
		printf("  Combined rotation+flip completed (checking result...)\n");
		/* Just report what we got - hardware behavior may vary */
		uint32_t *pixels = (uint32_t *)dst_map;
		uint32_t tl = pixels[4 * width + 4];
		uint32_t tr = pixels[4 * width + (width - 5)];
		uint32_t bl = pixels[(height - 5) * width + 4];
		uint32_t br = pixels[(height - 5) * width + (width - 5)];
		printf("  Result corners: TL=0x%08X TR=0x%08X BL=0x%08X BR=0x%08X\n",
		       tl, tr, bl, br);
	}
	
	/* Test 7: Error test - rotation + scaling (should fail) */
	printf("\nTest 7: Rotation + scaling (should fail)\n");
	blit.flags = G2D_BLIT_FLAG_ROTATE_90;
	blit.dst_w = width * 2;  /* Try to scale */
	blit.dst_h = height * 2;
	
	if (ioctl(g2d_fd, G2D_IOC_BLIT, &blit) < 0) {
		printf("  ✓ Correctly rejected rotation + scaling combination\n");
		passed++;
	} else {
		printf("  ✗ FAILED: Should have rejected rotation + scaling\n");
		failed++;
	}
	
	/* Summary */
	printf("\n======================\n");
	printf("Test Results: %d passed, %d failed\n", passed, failed);
	
cleanup:
	if (src_map && src_map != MAP_FAILED) munmap(src_map, size);
	if (dst_map && dst_map != MAP_FAILED) munmap(dst_map, size);
	if (src_fd >= 0) close(src_fd);
	if (dst_fd >= 0) close(dst_fd);
	close(heap_fd);
	close(g2d_fd);
	
	return (failed == 0) ? 0 : 1;
}
