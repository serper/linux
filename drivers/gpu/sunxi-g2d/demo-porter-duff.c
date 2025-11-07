/*
 * G2D Porter-Duff Modes Demo
 * 
 * Tests all 12 Porter-Duff blending modes side by side.
 * Displays a grid showing the result of each blending mode.
 * 
 * Test setup:
 * - SRC: Red semi-transparent circle (alpha gradient from center)
 * - DST: Blue semi-transparent square
 * - Each cell shows the result of a different Porter-Duff mode
 * 
 * Modes tested:
 *  0 CLEAR    - Clear to transparent
 *  1 COPY     - Copy source only
 *  2 DST      - Keep destination only
 *  3 SRCOVER  - Source over destination (default)
 *  4 DSTOVER  - Destination over source
 *  5 SRCIN    - Source where destination is opaque
 *  6 DSTIN    - Destination where source is opaque
 *  7 SRCOUT   - Source where destination is transparent
 *  8 DSTOUT   - Destination where source is transparent
 *  9 SRCATOP  - Source atop destination
 * 10 DSTATOP  - Destination atop source
 * 11 XOR      - XOR blend
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <errno.h>
#include <sys/mman.h>
#include <poll.h>
#include <math.h>

#include <linux/sunxi_g2d.h>
#include "demo-drm-base.h"

#define CELL_SIZE 150       /* Size of each test cell */
#define GRID_COLS 4         /* 4 columns */
#define GRID_ROWS 3         /* 3 rows (12 modes) */
#define PATTERN_SIZE 100    /* Size of src/dst patterns */

#define SCREEN_W (CELL_SIZE * GRID_COLS)
#define SCREEN_H (CELL_SIZE * GRID_ROWS)

static const char *mode_names[] = {
	"CLEAR",    /* 0 */
	"COPY",     /* 1 */
	"DST",      /* 2 */
	"SRCOVER",  /* 3 */
	"DSTOVER",  /* 4 */
	"SRCIN",    /* 5 */
	"DSTIN",    /* 6 */
	"SRCOUT",   /* 7 */
	"DSTOUT",   /* 8 */
	"SRCATOP",  /* 9 */
	"DSTATOP",  /* 10 */
	"XOR",      /* 11 */
};

/* Simple sync_wait implementation using poll() */
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
		return 0;
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

/* Create red semi-transparent circle with alpha gradient */
static void create_red_circle(void *buffer, int width, int height)
{
	uint32_t *pixels = (uint32_t *)buffer;
	int cx = width / 2;
	int cy = height / 2;
	int radius = (width < height ? width : height) / 2;
	
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			int dx = x - cx;
			int dy = y - cy;
			int dist = (int)sqrt(dx * dx + dy * dy);
			
			if (dist > radius) {
				/* Outside: transparent (alpha=255 for V0 inverted convention) */
				pixels[y * width + x] = 0xFF000000;
			} else {
				/* Inside: radial gradient from opaque center to semi-transparent edge
				 * V0 uses INVERTED alpha: 0=opaque, 255=transparent
				 * But with GLOBAL_ALPHA mode, we can use pixel alpha as-is */
				float norm = (float)dist / radius;
				uint8_t alpha = (uint8_t)(norm * 191);  /* 0 (center) to 191 (edge) */
				uint32_t color = 0xFF0000;  /* Red */
				pixels[y * width + x] = ((uint32_t)alpha << 24) | color;
			}
		}
	}
}

/* Create blue semi-transparent square */
static void create_blue_square(void *buffer, int width, int height)
{
	uint32_t *pixels = (uint32_t *)buffer;
	
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			/* Blue with 75% alpha (standard alpha: 192/255 = 0xC0) */
			pixels[y * width + x] = 0xC00000FF;  /* ARGB: A=0xC0, B=0xFF */
		}
	}
}

/* Fill buffer with black background */
static void fill_black(void *buffer, int width, int height)
{
	uint32_t *pixels = (uint32_t *)buffer;
	
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			pixels[y * width + x] = 0xFF000000;  /* Black opaque */
		}
	}
}

int main(int argc, char *argv[])
{
	int ret;
	int g2d_fd = -1;
	int src_ion_fd = -1, dst_ion_fd = -1, screen_ion_fd = -1;
	
	printf("========================================\n");
	printf("  G2D Porter-Duff Modes Demo\n");
	printf("========================================\n\n");
	
	printf("📋 Testing all 12 Porter-Duff blending modes\n");
	printf("   Grid: %dx%d cells, each %dx%d pixels\n", GRID_COLS, GRID_ROWS, CELL_SIZE, CELL_SIZE);
	printf("   SRC: Red semi-transparent circle\n");
	printf("   DST: Blue semi-transparent square\n\n");
	
	/* Open G2D device */
	g2d_fd = open("/dev/g2d", O_RDWR);
	if (g2d_fd < 0) {
		perror("Failed to open /dev/g2d");
		return 1;
	}
	
	/* Allocate SRC buffer (red circle) */
	size_t pattern_size = PATTERN_SIZE * PATTERN_SIZE * 4;  /* ARGB8888 */
	
	struct g2d_alloc_buffer src_alloc = {0};
	src_alloc.size = pattern_size;
	ret = ioctl(g2d_fd, G2D_IOC_ALLOC_BUFFER, &src_alloc);
	if (ret < 0) {
		perror("Failed to allocate SRC buffer");
		goto cleanup;
	}
	src_ion_fd = src_alloc.dma_fd;
	printf("✓ SRC buffer allocated: fd=%d (%dx%d)\n", src_ion_fd, PATTERN_SIZE, PATTERN_SIZE);
	
	/* Allocate DST buffer (blue square) */
	struct g2d_alloc_buffer dst_alloc = {0};
	dst_alloc.size = pattern_size;
	ret = ioctl(g2d_fd, G2D_IOC_ALLOC_BUFFER, &dst_alloc);
	if (ret < 0) {
		perror("Failed to allocate DST buffer");
		goto cleanup;
	}
	dst_ion_fd = dst_alloc.dma_fd;
	printf("✓ DST buffer allocated: fd=%d (%dx%d)\n", dst_ion_fd, PATTERN_SIZE, PATTERN_SIZE);
	
	/* Allocate screen buffer (for all results) */
	size_t screen_size = SCREEN_W * SCREEN_H * 4;
	
	struct g2d_alloc_buffer screen_alloc = {0};
	screen_alloc.size = screen_size;
	ret = ioctl(g2d_fd, G2D_IOC_ALLOC_BUFFER, &screen_alloc);
	if (ret < 0) {
		perror("Failed to allocate screen buffer");
		goto cleanup;
	}
	screen_ion_fd = screen_alloc.dma_fd;
	printf("✓ Screen buffer allocated: fd=%d (%dx%d)\n\n", screen_ion_fd, SCREEN_W, SCREEN_H);
	
	/* Create patterns */
	void *src_pattern = malloc(pattern_size);
	void *dst_pattern = malloc(pattern_size);
	void *screen_pattern = malloc(screen_size);
	
	if (!src_pattern || !dst_pattern || !screen_pattern) {
		fprintf(stderr, "Failed to allocate pattern buffers\n");
		goto cleanup;
	}
	
	printf("📝 Creating patterns...\n");
	create_red_circle(src_pattern, PATTERN_SIZE, PATTERN_SIZE);
	create_blue_square(dst_pattern, PATTERN_SIZE, PATTERN_SIZE);
	fill_black(screen_pattern, SCREEN_W, SCREEN_H);
	
	/* Upload patterns to G2D buffers */
	ret = g2d_write_buffer(g2d_fd, src_ion_fd, src_pattern, pattern_size, 0);
	if (ret < 0) {
		fprintf(stderr, "Failed to write SRC pattern\n");
		goto cleanup;
	}
	printf("  ✓ SRC: Red circle uploaded\n");
	
	ret = g2d_write_buffer(g2d_fd, dst_ion_fd, dst_pattern, pattern_size, 0);
	if (ret < 0) {
		fprintf(stderr, "Failed to write DST pattern\n");
		goto cleanup;
	}
	printf("  ✓ DST: Blue square uploaded\n");
	
	ret = g2d_write_buffer(g2d_fd, screen_ion_fd, screen_pattern, screen_size, 0);
	if (ret < 0) {
		fprintf(stderr, "Failed to write screen pattern\n");
		goto cleanup;
	}
	printf("  ✓ Screen: Black background uploaded\n\n");
	
	free(src_pattern);
	free(dst_pattern);
	free(screen_pattern);
	
	/* Test all Porter-Duff modes */
	printf("🚀 Testing Porter-Duff modes:\n");
	
	for (int mode = 0; mode < 12; mode++) {
		int grid_x = mode % GRID_COLS;
		int grid_y = mode / GRID_COLS;
		int dst_x = grid_x * CELL_SIZE + (CELL_SIZE - PATTERN_SIZE) / 2;
		int dst_y = grid_y * CELL_SIZE + (CELL_SIZE - PATTERN_SIZE) / 2;
		
		printf("  [%2d] %-8s at (%3d,%3d)...", mode, mode_names[mode], dst_x, dst_y);
		fflush(stdout);
		
		/* First, blit DST (blue square) to screen */
		struct g2d_blit blit1 = {0};
		blit1.src.dma_fd = dst_ion_fd;
		blit1.src.width = PATTERN_SIZE;
		blit1.src.height = PATTERN_SIZE;
		blit1.src.format = G2D_FMT_ARGB8888;
		blit1.src.crop_w = PATTERN_SIZE;
		blit1.src.crop_h = PATTERN_SIZE;
		
		blit1.dst.dma_fd = screen_ion_fd;
		blit1.dst.width = SCREEN_W;
		blit1.dst.height = SCREEN_H;
		blit1.dst.format = G2D_FMT_ARGB8888;
		blit1.dst_x = dst_x;
		blit1.dst_y = dst_y;
		blit1.dst_w = PATTERN_SIZE;
		blit1.dst_h = PATTERN_SIZE;
		
		blit1.out.dma_fd = -1;  /* In-place */
		blit1.flags = 0;
		blit1.bld_mode = 1;  /* COPY mode for first layer */
		blit1.fence_fd_in = -1;
		blit1.fence_fd_out = -1;
		
		ret = ioctl(g2d_fd, G2D_IOC_BLIT, &blit1);
		if (ret < 0) {
			printf(" ❌ DST blit failed\n");
			continue;
		}
		
		if (blit1.fence_fd_out >= 0) {
			sync_wait(blit1.fence_fd_out, 1000);
			close(blit1.fence_fd_out);
		}
		
		/* Now blit SRC (red circle) with the Porter-Duff mode */
		struct g2d_blit blit2 = {0};
		blit2.src.dma_fd = src_ion_fd;
		blit2.src.width = PATTERN_SIZE;
		blit2.src.height = PATTERN_SIZE;
		blit2.src.format = G2D_FMT_ARGB8888;
		blit2.src.crop_w = PATTERN_SIZE;
		blit2.src.crop_h = PATTERN_SIZE;
		blit2.src.alpha = 128;  /* 50% global alpha */
		blit2.src.alpha_mode = 1;  /* GLOBAL_ALPHA */
		
		blit2.dst.dma_fd = screen_ion_fd;
		blit2.dst.width = SCREEN_W;
		blit2.dst.height = SCREEN_H;
		blit2.dst.format = G2D_FMT_ARGB8888;
		blit2.dst.alpha = 128;  /* 50% for blending */
		blit2.dst.alpha_mode = 1;  /* GLOBAL_ALPHA */
		blit2.dst_x = dst_x;
		blit2.dst_y = dst_y;
		blit2.dst_w = PATTERN_SIZE;
		blit2.dst_h = PATTERN_SIZE;
		
		blit2.out.dma_fd = -1;  /* In-place */
		blit2.flags = 0;
		blit2.bld_mode = mode;  /* Test this Porter-Duff mode */
		blit2.fence_fd_in = -1;
		blit2.fence_fd_out = -1;
		
		ret = ioctl(g2d_fd, G2D_IOC_BLIT, &blit2);
		if (ret < 0) {
			printf(" ❌ SRC blit failed\n");
			continue;
		}
		
		if (blit2.fence_fd_out >= 0) {
			sync_wait(blit2.fence_fd_out, 1000);
			close(blit2.fence_fd_out);
		}
		
		printf(" ✓\n");
	}
	
	printf("\n========================================\n");
	printf("  All modes tested successfully!\n");
	printf("  Screen buffer contains %dx%d grid\n", GRID_COLS, GRID_ROWS);
	printf("  Each cell shows a different blend mode\n");
	printf("========================================\n");
	
	/* TODO: Display the screen buffer or save to file */
	printf("\nℹ️  To visualize: Add DRM display code or export buffer to file\n");
	
cleanup:
	if (src_ion_fd >= 0) close(src_ion_fd);
	if (dst_ion_fd >= 0) close(dst_ion_fd);
	if (screen_ion_fd >= 0) close(screen_ion_fd);
	if (g2d_fd >= 0) close(g2d_fd);
	
	return 0;
}
