/*
 * Test program for sunxi-g2d fillrect RCQ path
 * Tests the new G2D_IOC_FILLRECT_RCQ ioctl
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <poll.h>

/* DMA heap ioctl definitions */
struct dma_heap_allocation_data {
	uint64_t len;
	uint32_t fd;
	uint32_t fd_flags;
	uint64_t heap_flags;
};

#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)

/* G2D UAPI - local copy matching kernel header */
enum g2d_pixel_format {
	G2D_FMT_ARGB8888 = 0,
	G2D_FMT_ABGR8888 = 1,
	G2D_FMT_RGBA8888 = 2,
	G2D_FMT_BGRA8888 = 3,
	G2D_FMT_XRGB8888 = 4,
	G2D_FMT_XBGR8888 = 5,
	G2D_FMT_RGBX8888 = 6,
	G2D_FMT_BGRX8888 = 7,
	G2D_FMT_RGB565 = 10,
};

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
	
	uint8_t alpha;
	uint8_t alpha_mode;
	uint16_t _pad;
};

struct g2d_fillrect {
	struct g2d_buf dst;
	
	uint32_t dst_x;
	uint32_t dst_y;
	uint32_t dst_w;
	uint32_t dst_h;
	
	uint32_t color;
	uint32_t color_format;
	
	int32_t fence_fd_in;
	int32_t fence_fd_out;
};

struct g2d_version {
	uint32_t hw_version;
	uint32_t driver_major;
	uint32_t driver_minor;
	uint32_t driver_patchlevel;
};

#define G2D_IOC_MAGIC 'G'
#define G2D_IOC_GET_VERSION    _IOR(G2D_IOC_MAGIC, 0, struct g2d_version)
#define G2D_IOC_FILLRECT       _IOWR(G2D_IOC_MAGIC, 2, struct g2d_fillrect)
#define G2D_IOC_FILLRECT_RCQ   _IOWR(G2D_IOC_MAGIC, 7, struct g2d_fillrect)
#define G2D_IOC_SYNC           _IOW(G2D_IOC_MAGIC, 3, int32_t)

#define BUFFER_WIDTH  256
#define BUFFER_HEIGHT 256
#define BUFFER_SIZE   (BUFFER_WIDTH * BUFFER_HEIGHT * 4)  // ARGB8888

static void print_buffer_sample(uint32_t *buf, const char *label)
{
	printf("%s: [0]=0x%08x [100]=0x%08x [1000]=0x%08x [10000]=0x%08x\n",
	       label, buf[0], buf[100], buf[1000], buf[10000]);
}

static int create_dmaheap_buffer(int *dma_fd, void **mapped_ptr)
{
	int heap_fd;
	struct dma_heap_allocation_data alloc = {0};
	
	/* Open system DMA heap */
	heap_fd = open("/dev/dma_heap/system", O_RDONLY);
	if (heap_fd < 0) {
		perror("Failed to open /dev/dma_heap/system");
		return -1;
	}

	/* Allocate buffer */
	alloc.len = BUFFER_SIZE;
	alloc.fd_flags = O_RDWR | O_CLOEXEC;
	alloc.heap_flags = 0;

	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
		perror("DMA_HEAP_IOCTL_ALLOC failed");
		close(heap_fd);
		return -1;
	}

	close(heap_fd);
	*dma_fd = alloc.fd;

	/* Map the buffer */
	*mapped_ptr = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED, alloc.fd, 0);
	if (*mapped_ptr == MAP_FAILED) {
		perror("mmap failed");
		close(alloc.fd);
		return -1;
	}

	printf("✓ Created DMA-BUF buffer: fd=%d size=%d mapped=%p\n",
	       alloc.fd, BUFFER_SIZE, *mapped_ptr);
	return 0;
}

static int wait_fence(int fence_fd, const char *label)
{
	struct pollfd pfd = {
		.fd = fence_fd,
		.events = POLLIN,
	};
	int ret;

	printf("  Waiting for fence fd=%d (%s)...\n", fence_fd, label);
	
	ret = poll(&pfd, 1, 5000);  // 5 second timeout
	if (ret < 0) {
		perror("poll failed");
		return -1;
	}
	if (ret == 0) {
		printf("  ❌ Fence timeout after 5 seconds!\n");
		return -1;
	}
	
	printf("  ✓ Fence signaled\n");
	return 0;
}

static int test_fillrect_rcq(int g2d_fd, int dma_fd, uint32_t *buf,
			     uint32_t x, uint32_t y, uint32_t w, uint32_t h,
			     uint32_t color)
{
	struct g2d_fillrect fill = {0};
	int ret;

	printf("\n=== Testing FILLRECT_RCQ: pos(%u,%u) size(%ux%u) color=0x%08x ===\n",
	       x, y, w, h, color);

	/* Setup fillrect request */
	fill.dst.width = BUFFER_WIDTH;
	fill.dst.height = BUFFER_HEIGHT;
	fill.dst.format = G2D_FMT_ARGB8888;
	fill.dst.stride[0] = BUFFER_WIDTH * 4;
	fill.dst.dma_fd = dma_fd;
	
	fill.dst_x = x;
	fill.dst_y = y;
	fill.dst_w = w;
	fill.dst_h = h;
	
	fill.color = color;
	fill.color_format = G2D_FMT_ARGB8888;
	
	fill.fence_fd_in = -1;
	fill.fence_fd_out = -1;

	print_buffer_sample(buf, "Before");

	/* Call RCQ fillrect ioctl */
	ret = ioctl(g2d_fd, G2D_IOC_FILLRECT_RCQ, &fill);
	if (ret < 0) {
		perror("G2D_IOC_FILLRECT_RCQ failed");
		return -1;
	}

	printf("  ✓ FILLRECT_RCQ ioctl returned, fence_fd_out=%d\n", fill.fence_fd_out);

	/* Wait for fence if one was returned */
	if (fill.fence_fd_out >= 0) {
		if (wait_fence(fill.fence_fd_out, "fillrect completion") < 0) {
			close(fill.fence_fd_out);
			return -1;
		}
		close(fill.fence_fd_out);
	} else {
		printf("  ⚠ No fence returned (fence_fd_out=%d)\n", fill.fence_fd_out);
	}

	print_buffer_sample(buf, "After ");

	/* Verify the fill (check first pixel of filled region) */
	uint32_t offset = y * BUFFER_WIDTH + x;
	if (buf[offset] != color) {
		printf("  ❌ FAILED: Expected 0x%08x at [%u,%u], got 0x%08x\n",
		       color, x, y, buf[offset]);
		return -1;
	}

	/* Check a pixel in the middle of the filled region */
	if (w > 2 && h > 2) {
		offset = (y + h/2) * BUFFER_WIDTH + (x + w/2);
		if (buf[offset] != color) {
			printf("  ❌ FAILED: Expected 0x%08x at center, got 0x%08x\n",
			       color, buf[offset]);
			return -1;
		}
	}

	/* Check last pixel */
	offset = (y + h - 1) * BUFFER_WIDTH + (x + w - 1);
	if (buf[offset] != color) {
		printf("  ❌ FAILED: Expected 0x%08x at last pixel, got 0x%08x\n",
		       color, buf[offset]);
		return -1;
	}

	printf("  ✅ Fillrect RCQ verified successfully!\n");
	return 0;
}

int main(int argc, char *argv[])
{
	int g2d_fd, dma_fd = -1;
	void *mapped_ptr = NULL;
	uint32_t *buf;
	struct g2d_version ver;
	int ret = 0;
	int num_tests = 3;

	printf("=== sunxi-g2d FILLRECT_RCQ test ===\n\n");

	/* Open G2D device */
	g2d_fd = open("/dev/g2d", O_RDWR);
	if (g2d_fd < 0) {
		perror("Failed to open /dev/g2d");
		return 1;
	}
	printf("✓ Opened /dev/g2d\n");

	/* Get version */
	if (ioctl(g2d_fd, G2D_IOC_GET_VERSION, &ver) == 0) {
		printf("✓ G2D version: hw=0x%08x driver=%u.%u.%u\n",
		       ver.hw_version, ver.driver_major, ver.driver_minor,
		       ver.driver_patchlevel);
	}

	/* Create DMA-BUF buffer */
	if (create_dmaheap_buffer(&dma_fd, &mapped_ptr) < 0) {
		close(g2d_fd);
		return 1;
	}

	buf = (uint32_t *)mapped_ptr;
	
	/* Clear buffer to black */
	memset(buf, 0, BUFFER_SIZE);
	printf("✓ Buffer cleared to black\n");

	/* Allow fewer tests via command line */
	if (argc > 1) {
		num_tests = atoi(argv[1]);
		if (num_tests < 1) num_tests = 1;
		if (num_tests > 10) num_tests = 10;
	}

	printf("\n📋 Running %d fillrect RCQ test(s)...\n", num_tests);

	/* Test 1: Fill entire buffer with red */
	if (test_fillrect_rcq(g2d_fd, dma_fd, buf, 
			      0, 0, BUFFER_WIDTH, BUFFER_HEIGHT,
			      0xFFFF0000) < 0) {
		ret = 1;
		goto cleanup;
	}

	if (num_tests < 2) goto cleanup;

	/* Test 2: Fill smaller rectangle with green */
	if (test_fillrect_rcq(g2d_fd, dma_fd, buf,
			      64, 64, 128, 128,
			      0xFF00FF00) < 0) {
		ret = 1;
		goto cleanup;
	}

	if (num_tests < 3) goto cleanup;

	/* Test 3: Fill small rectangle with blue */
	if (test_fillrect_rcq(g2d_fd, dma_fd, buf,
			      96, 96, 64, 64,
			      0xFF0000FF) < 0) {
		ret = 1;
		goto cleanup;
	}

	printf("\n🎉 All %d tests PASSED!\n", num_tests);

cleanup:
	if (mapped_ptr)
		munmap(mapped_ptr, BUFFER_SIZE);
	if (dma_fd >= 0)
		close(dma_fd);
	close(g2d_fd);

	return ret;
}
