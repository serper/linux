/*
 * test-alpha-simple.c - Simple test for alpha blending pipeline
 * Just tries to copy a solid color through the BLD path
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

#define G2D_IOC_MAGIC 'G'
#define G2D_IOC_BLIT _IOWR(G2D_IOC_MAGIC, 1, struct g2d_blit)

#define G2D_FMT_ARGB8888 0
#define G2D_BLIT_FLAG_ALPHA_BLEND (1 << 0)

struct g2d_buf {
	uint32_t width, height, format;
	uint32_t stride[3];
	int32_t dma_fd;
	uint64_t paddr[3];
	uint32_t crop_x, crop_y, crop_w, crop_h;
};

struct g2d_blit {
	struct g2d_buf src, dst;
	uint32_t dst_x, dst_y, dst_w, dst_h;
	uint32_t flags;
	uint32_t global_alpha;
	int32_t fence_fd_in, fence_fd_out;
};

int alloc_dma_buffer(int heap_fd, uint32_t size, int *dma_fd) {
	struct dma_heap_allocation_data alloc = {
		.len = size, .fd = 0,
		.fd_flags = O_RDWR | O_CLOEXEC, .heap_flags = 0,
	};
	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
		perror("DMA alloc failed");
		return -1;
	}
	*dma_fd = alloc.fd;
	return 0;
}

int main(void) {
	int g2d_fd, heap_fd, src_fd = -1, dst_fd = -1;
	void *src_map = NULL, *dst_map = NULL;
	const uint32_t width = 64, height = 64;
	const uint32_t size = width * height * 4;
	const uint32_t test_color = 0xFFFF0000;  /* Red */
	
	printf("Simple Alpha Blending Pipeline Test\n");
	printf("====================================\n");
	
	g2d_fd = open("/dev/g2d", O_RDWR);
	heap_fd = open("/dev/dma_heap/default_cma_region", O_RDONLY);
	if (g2d_fd < 0 || heap_fd < 0) {
		perror("Device open failed");
		return 1;
	}
	
	if (alloc_dma_buffer(heap_fd, size, &src_fd) < 0 ||
	    alloc_dma_buffer(heap_fd, size, &dst_fd) < 0) {
		goto cleanup;
	}
	
	src_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, src_fd, 0);
	dst_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dst_fd, 0);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
		perror("mmap failed");
		goto cleanup;
	}
	
	/* Fill source with red */
	for (uint32_t i = 0; i < width * height; i++) {
		((uint32_t *)src_map)[i] = test_color;
	}
	
	/* Fill destination with green (background) */
	for (uint32_t i = 0; i < width * height; i++) {
		((uint32_t *)dst_map)[i] = 0xFF00FF00;
	}
	
	printf("Source: 0x%08X (red)\n", test_color);
	printf("Dest before: 0x%08X (green)\n", ((uint32_t *)dst_map)[0]);
	
	/* Try alpha blend with alpha=255 (should give pure source color) */
	struct g2d_blit blit = {
		.src = {width, height, G2D_FMT_ARGB8888, {0,0,0}, src_fd, {0,0,0}, 0, 0, width, height},
		.dst = {width, height, G2D_FMT_ARGB8888, {0,0,0}, dst_fd, {0,0,0}, 0, 0, 0, 0},
		.dst_x = 0, .dst_y = 0, .dst_w = width, .dst_h = height,
		.flags = G2D_BLIT_FLAG_ALPHA_BLEND,
		.global_alpha = 255,
		.fence_fd_in = -1, .fence_fd_out = -1,
	};
	
	if (ioctl(g2d_fd, G2D_IOC_BLIT, &blit) < 0) {
		perror("BLIT failed");
		goto cleanup;
	}
	
	printf("Dest after: 0x%08X\n", ((uint32_t *)dst_map)[0]);
	
	if (((uint32_t *)dst_map)[0] == test_color) {
		printf("✓ SUCCESS: Got expected color!\n");
	} else if (((uint32_t *)dst_map)[0] == 0xFF00FF00) {
		printf("✗ FAILED: Destination unchanged (blend didn't run)\n");
	} else if (((uint32_t *)dst_map)[0] == 0x00000000) {
		printf("✗ FAILED: Got black (writeback issue?)\n");
	} else {
		printf("✗ FAILED: Got unexpected color\n");
	}
	
cleanup:
	if (src_map && src_map != MAP_FAILED) munmap(src_map, size);
	if (dst_map && dst_map != MAP_FAILED) munmap(dst_map, size);
	if (src_fd >= 0) close(src_fd);
	if (dst_fd >= 0) close(dst_fd);
	close(heap_fd);
	close(g2d_fd);
	return 0;
}
