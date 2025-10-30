/*
 * Test program for sunxi-g2d fillrect operation from userspace
 * Uses CMA allocation via dmabuf export from driver
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include "../../include/uapi/linux/sunxi_g2d.h"

#define BUFFER_WIDTH  128
#define BUFFER_HEIGHT 128
#define BUFFER_SIZE   (BUFFER_WIDTH * BUFFER_HEIGHT * 4)  // ARGB8888

static void print_buffer_sample(uint32_t *buf, const char *label)
{
	printf("%s: [0]=0x%08x [10]=0x%08x [100]=0x%08x [1000]=0x%08x\n",
	       label, buf[0], buf[10], buf[100], buf[1000]);
}

static int alloc_cma_buffer(int *dma_fd, void **mapped_ptr)
{
	int heap_fd;
	struct dma_heap_allocation_data alloc = {
		.len = BUFFER_SIZE,
		.fd_flags = O_RDWR | O_CLOEXEC,
	};

	// Try to open CMA heap
	heap_fd = open("/dev/dma_heap/linux,cma", O_RDONLY | O_CLOEXEC);
	if (heap_fd < 0) {
		// Fallback to system heap
		heap_fd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
		if (heap_fd < 0) {
			perror("Failed to open dma_heap");
			return -1;
		}
	}

	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
		perror("DMA_HEAP_IOCTL_ALLOC failed");
		close(heap_fd);
		return -1;
	}

	close(heap_fd);
	*dma_fd = alloc.fd;

	// Map the buffer
	*mapped_ptr = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED, *dma_fd, 0);
	if (*mapped_ptr == MAP_FAILED) {
		perror("mmap failed");
		close(*dma_fd);
		return -1;
	}

	printf("✓ Allocated CMA buffer: fd=%d size=%d mapped=%p\n",
	       *dma_fd, BUFFER_SIZE, *mapped_ptr);
	return 0;
}

static int test_fillrect(int g2d_fd, int dma_fd, uint32_t *buf,
			 uint32_t x, uint32_t y, uint32_t w, uint32_t h,
			 uint32_t color)
{
	struct g2d_fillrect fillrect = {
		.dst = {
			.fd = dma_fd,
			.width = BUFFER_WIDTH,
			.height = BUFFER_HEIGHT,
			.format = G2D_FORMAT_ARGB8888,
		},
		.dst_rect = {
			.x = x,
			.y = y,
			.w = w,
			.h = h,
		},
		.color = color,
		.acquire_fence_fd = -1,
		.release_fence_fd = -1,
	};

	printf("\n=== Testing fillrect: pos(%u,%u) size(%ux%u) color=0x%08x ===\n",
	       x, y, w, h, color);

	print_buffer_sample(buf, "Before");

	if (ioctl(g2d_fd, G2D_IOCTL_FILLRECT, &fillrect) < 0) {
		perror("G2D_IOCTL_FILLRECT failed");
		return -1;
	}

	print_buffer_sample(buf, "After ");

	// Verify the fill (check first pixel of filled region)
	uint32_t offset = y * BUFFER_WIDTH + x;
	if (buf[offset] != color) {
		printf("❌ FAILED: Expected 0x%08x at [%u,%u], got 0x%08x\n",
		       color, x, y, buf[offset]);
		return -1;
	}

	// Check a pixel in the middle of the filled region
	if (w > 2 && h > 2) {
		offset = (y + h/2) * BUFFER_WIDTH + (x + w/2);
		if (buf[offset] != color) {
			printf("❌ FAILED: Expected 0x%08x at center, got 0x%08x\n",
			       color, buf[offset]);
			return -1;
		}
	}

	printf("✅ Fillrect verified successfully!\n");
	return 0;
}

int main(void)
{
	int g2d_fd, dma_fd = -1;
	void *mapped_ptr = NULL;
	uint32_t *buf;
	struct g2d_capability cap;
	int ret = 0;

	printf("=== sunxi-g2d fillrect userspace test ===\n\n");

	// Open G2D device
	g2d_fd = open("/dev/g2d", O_RDWR);
	if (g2d_fd < 0) {
		perror("Failed to open /dev/g2d");
		return 1;
	}
	printf("✓ Opened /dev/g2d\n");

	// Get driver version
	if (ioctl(g2d_fd, G2D_IOCTL_GET_VERSION, &cap) == 0) {
		printf("✓ Driver version: %u.%u.%u, HW version: 0x%08x\n",
		       cap.driver_major, cap.driver_minor, cap.driver_patch,
		       cap.hw_version);
	}

	// Allocate CMA buffer
	if (alloc_cma_buffer(&dma_fd, &mapped_ptr) < 0) {
		ret = 1;
		goto cleanup;
	}
	buf = (uint32_t *)mapped_ptr;

	// Test 1: Fill entire buffer with red
	memset(buf, 0, BUFFER_SIZE);
	if (test_fillrect(g2d_fd, dma_fd, buf, 0, 0,
			  BUFFER_WIDTH, BUFFER_HEIGHT, 0xFFFF0000) < 0) {
		ret = 1;
		goto cleanup;
	}

	// Test 2: Fill a rectangle in the middle with green
	if (test_fillrect(g2d_fd, dma_fd, buf, 32, 32, 64, 64, 0xFF00FF00) < 0) {
		ret = 1;
		goto cleanup;
	}

	// Test 3: Fill top-left corner with blue
	if (test_fillrect(g2d_fd, dma_fd, buf, 0, 0, 16, 16, 0xFF0000FF) < 0) {
		ret = 1;
		goto cleanup;
	}

	// Test 4: Fill bottom-right corner with yellow
	if (test_fillrect(g2d_fd, dma_fd, buf,
			  BUFFER_WIDTH - 16, BUFFER_HEIGHT - 16,
			  16, 16, 0xFFFFFF00) < 0) {
		ret = 1;
		goto cleanup;
	}

	printf("\n🎉 All fillrect tests passed!\n");

cleanup:
	if (mapped_ptr && mapped_ptr != MAP_FAILED)
		munmap(mapped_ptr, BUFFER_SIZE);
	if (dma_fd >= 0)
		close(dma_fd);
	if (g2d_fd >= 0)
		close(g2d_fd);

	return ret;
}
