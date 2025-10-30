/*
 * Simple test program for sunxi-g2d fillrect from userspace
 * Uses memfd + dma-buf import
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

/* memfd_create flags */
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

/* Local copy of UAPI structs */
enum g2d_pixel_format {
	G2D_FORMAT_ARGB8888 = 0,
	G2D_FORMAT_XRGB8888 = 4,
	G2D_FORMAT_RGB565 = 10,
};

struct g2d_buffer {
	int fd;
	uint32_t width;
	uint32_t height;
	uint32_t format;
};

struct g2d_rect {
	uint32_t x;
	uint32_t y;
	uint32_t w;
	uint32_t h;
};

struct g2d_fillrect {
	struct g2d_buffer dst;
	struct g2d_rect dst_rect;
	uint32_t color;
	int acquire_fence_fd;
	int release_fence_fd;
};

struct g2d_capability {
	uint32_t hw_version;
	uint32_t driver_major;
	uint32_t driver_minor;
	uint32_t driver_patch;
};

#define G2D_IOC_MAGIC 'G'
#define G2D_IOCTL_GET_VERSION  _IOR(G2D_IOC_MAGIC, 0, struct g2d_capability)
#define G2D_IOCTL_FILLRECT     _IOWR(G2D_IOC_MAGIC, 2, struct g2d_fillrect)

#define BUFFER_WIDTH  128
#define BUFFER_HEIGHT 128
#define BUFFER_SIZE   (BUFFER_WIDTH * BUFFER_HEIGHT * 4)  // ARGB8888

static void print_buffer_sample(uint32_t *buf, const char *label)
{
	printf("%s: [0]=0x%08x [10]=0x%08x [100]=0x%08x [1000]=0x%08x\n",
	       label, buf[0], buf[10], buf[100], buf[1000]);
}

static int create_memfd_buffer(int *dma_fd, void **mapped_ptr)
{
	int fd;
	
	/* Create anonymous file descriptor */
	fd = memfd_create("g2d-buffer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0) {
		perror("memfd_create failed");
		return -1;
	}

	/* Set size */
	if (ftruncate(fd, BUFFER_SIZE) < 0) {
		perror("ftruncate failed");
		close(fd);
		return -1;
	}

	/* Map the buffer */
	*mapped_ptr = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd, 0);
	if (*mapped_ptr == MAP_FAILED) {
		perror("mmap failed");
		close(fd);
		return -1;
	}

	*dma_fd = fd;
	printf("✓ Created memfd buffer: fd=%d size=%d mapped=%p\n",
	       fd, BUFFER_SIZE, *mapped_ptr);
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

	/* Verify the fill (check first pixel of filled region) */
	uint32_t offset = y * BUFFER_WIDTH + x;
	if (buf[offset] != color) {
		printf("❌ FAILED: Expected 0x%08x at [%u,%u], got 0x%08x\n",
		       color, x, y, buf[offset]);
		return -1;
	}

	/* Check a pixel in the middle of the filled region */
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

	/* Open G2D device */
	g2d_fd = open("/dev/g2d", O_RDWR);
	if (g2d_fd < 0) {
		perror("Failed to open /dev/g2d");
		return 1;
	}
	printf("✓ Opened /dev/g2d\n");

	/* Get driver version */
	if (ioctl(g2d_fd, G2D_IOCTL_GET_VERSION, &cap) == 0) {
		printf("✓ Driver version: %u.%u.%u, HW version: 0x%08x\n",
		       cap.driver_major, cap.driver_minor, cap.driver_patch,
		       cap.hw_version);
	}

	/* Create memfd buffer */
	if (create_memfd_buffer(&dma_fd, &mapped_ptr) < 0) {
		ret = 1;
		goto cleanup;
	}
	buf = (uint32_t *)mapped_ptr;

	/* Test 1: Fill entire buffer with red */
	memset(buf, 0, BUFFER_SIZE);
	if (test_fillrect(g2d_fd, dma_fd, buf, 0, 0,
			  BUFFER_WIDTH, BUFFER_HEIGHT, 0xFFFF0000) < 0) {
		ret = 1;
		goto cleanup;
	}

	/* Test 2: Fill a rectangle in the middle with green */
	if (test_fillrect(g2d_fd, dma_fd, buf, 32, 32, 64, 64, 0xFF00FF00) < 0) {
		ret = 1;
		goto cleanup;
	}

	/* Test 3: Fill top-left corner with blue */
	if (test_fillrect(g2d_fd, dma_fd, buf, 0, 0, 16, 16, 0xFF0000FF) < 0) {
		ret = 1;
		goto cleanup;
	}

	/* Test 4: Fill bottom-right corner with yellow */
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
