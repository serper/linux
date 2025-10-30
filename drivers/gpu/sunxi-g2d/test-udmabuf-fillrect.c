/*
 * Test program for sunxi-g2d fillrect using UDMABUF
 * Allocates buffers via udmabuf and tests hardware fillrect
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

/* UDMABUF ioctls */
struct udmabuf_create {
	uint32_t memfd;
	uint32_t flags;
	uint64_t offset;
	uint64_t size;
};

#define UDMABUF_FLAGS_CLOEXEC 0x01
#define UDMABUF_CREATE _IOW('u', 0x42, struct udmabuf_create)

/* memfd sealing */
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#endif
#ifndef F_SEAL_SHRINK
#define F_SEAL_SHRINK 0x0002
#endif

/* G2D UAPI */
enum g2d_pixel_format {
	G2D_FMT_ARGB8888 = 0,
	G2D_FMT_XRGB8888 = 4,
	G2D_FMT_RGB565 = 10,
};

/* G2D buffer description - must match kernel UAPI */
struct g2d_buf {
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint32_t stride[3];
	
	int32_t dma_fd;      /* DMA-BUF file descriptor, or -1 */
	uint64_t paddr[3];   /* Physical addresses */
	
	uint32_t crop_x;
	uint32_t crop_y;
	uint32_t crop_w;
	uint32_t crop_h;
};

struct g2d_fillrect {
	struct g2d_buf dst;
	
	uint32_t dst_x;
	uint32_t dst_y;
	uint32_t dst_w;
	uint32_t dst_h;
	
	uint32_t color;
	
	int32_t fence_fd_in;
	int32_t fence_fd_out;
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
#define BUFFER_SIZE   (BUFFER_WIDTH * BUFFER_HEIGHT * 4)

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

static void print_buffer_sample(uint32_t *buf, const char *label)
{
	printf("%s: [0]=0x%08x [10]=0x%08x [100]=0x%08x [1000]=0x%08x\n",
	       label, buf[0], buf[10], buf[100], buf[1000]);
}

static int create_udmabuf_buffer(int *dma_fd, int *memfd_out, void **mapped_ptr)
{
	int memfd, udmabuf_fd, dmabuf_fd;
	struct udmabuf_create create;
	
	/* Create memfd for backing storage */
	memfd = memfd_create("g2d-buffer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (memfd < 0) {
		perror("memfd_create failed");
		return -1;
	}

	/* Add sealing to prevent shrinking */
	if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) {
		perror("fcntl F_ADD_SEALS failed");
		close(memfd);
		return -1;
	}

	/* Set size */
	if (ftruncate(memfd, BUFFER_SIZE) < 0) {
		perror("ftruncate failed");
		close(memfd);
		return -1;
	}

	/* Map the memfd for CPU access */
	*mapped_ptr = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED, memfd, 0);
	if (*mapped_ptr == MAP_FAILED) {
		perror("mmap failed");
		close(memfd);
		return -1;
	}

	/* Open udmabuf device */
	udmabuf_fd = open("/dev/udmabuf", O_RDWR);
	if (udmabuf_fd < 0) {
		perror("Failed to open /dev/udmabuf");
		munmap(*mapped_ptr, BUFFER_SIZE);
		close(memfd);
		return -1;
	}

	/* Create udmabuf from memfd - ioctl returns the dmabuf fd */
	memset(&create, 0, sizeof(create));
	create.memfd = memfd;
	create.flags = UDMABUF_FLAGS_CLOEXEC;
	create.offset = 0;
	create.size = BUFFER_SIZE;

	dmabuf_fd = ioctl(udmabuf_fd, UDMABUF_CREATE, &create);
	if (dmabuf_fd < 0) {
		perror("UDMABUF_CREATE failed");
		close(udmabuf_fd);
		munmap(*mapped_ptr, BUFFER_SIZE);
		close(memfd);
		return -1;
	}

	close(udmabuf_fd);  /* No longer needed */
	/* Keep memfd open - caller must close it */

	*dma_fd = dmabuf_fd;
	*memfd_out = memfd;
	printf("✓ Created udmabuf: dmabuf_fd=%d memfd=%d size=%d mapped=%p\n",
	       dmabuf_fd, memfd, BUFFER_SIZE, *mapped_ptr);
	return 0;
}

static int test_fillrect(int g2d_fd, int dma_fd, uint32_t *buf,
			 uint32_t x, uint32_t y, uint32_t w, uint32_t h,
			 uint32_t color)
{
	struct g2d_fillrect fillrect = {
		.dst = {
			.width = BUFFER_WIDTH,
			.height = BUFFER_HEIGHT,
			.format = G2D_FMT_ARGB8888,
			.stride = {0, 0, 0},
			.dma_fd = dma_fd,
			.paddr = {0, 0, 0},
			.crop_x = 0,
			.crop_y = 0,
			.crop_w = 0,
			.crop_h = 0,
		},
		.dst_x = x,
		.dst_y = y,
		.dst_w = w,
		.dst_h = h,
		.color = color,
		.fence_fd_in = -1,
		.fence_fd_out = -1,
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
	int g2d_fd, dma_fd = -1, memfd = -1;
	void *mapped_ptr = NULL;
	uint32_t *buf;
	struct g2d_capability cap;
	int ret = 0;

	printf("=== sunxi-g2d fillrect UDMABUF test ===\n\n");

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

	/* Create UDMABUF buffer */
	if (create_udmabuf_buffer(&dma_fd, &memfd, &mapped_ptr) < 0) {
		ret = 1;
		goto cleanup;
	}
	buf = (uint32_t *)mapped_ptr;

	/* Test 1: Fill entire buffer with red */
	printf("\n--- Test 1: Full buffer fill (red) ---\n");
	memset(buf, 0, BUFFER_SIZE);
	if (test_fillrect(g2d_fd, dma_fd, buf, 0, 0,
			  BUFFER_WIDTH, BUFFER_HEIGHT, 0xFFFF0000) < 0) {
		ret = 1;
		goto cleanup;
	}

	/* Test 2: Fill a rectangle in the middle with green */
	printf("\n--- Test 2: Middle rectangle (green) ---\n");
	if (test_fillrect(g2d_fd, dma_fd, buf, 32, 32, 64, 64, 0xFF00FF00) < 0) {
		ret = 1;
		goto cleanup;
	}

	/* Test 3: Fill top-left corner with blue */
	printf("\n--- Test 3: Top-left corner (blue) ---\n");
	if (test_fillrect(g2d_fd, dma_fd, buf, 0, 0, 16, 16, 0xFF0000FF) < 0) {
		ret = 1;
		goto cleanup;
	}

	/* Test 4: Fill bottom-right corner with yellow */
	printf("\n--- Test 4: Bottom-right corner (yellow) ---\n");
	if (test_fillrect(g2d_fd, dma_fd, buf,
			  BUFFER_WIDTH - 16, BUFFER_HEIGHT - 16,
			  16, 16, 0xFFFFFF00) < 0) {
		ret = 1;
		goto cleanup;
	}

	/* Test 5: Small 1x1 pixel */
	printf("\n--- Test 5: Single pixel (white) ---\n");
	memset(buf, 0, BUFFER_SIZE);
	if (test_fillrect(g2d_fd, dma_fd, buf, 64, 64, 1, 1, 0xFFFFFFFF) < 0) {
		ret = 1;
		goto cleanup;
	}

	/* Test 6: Wide rectangle */
	printf("\n--- Test 6: Wide rectangle (magenta) ---\n");
	memset(buf, 0, BUFFER_SIZE);
	if (test_fillrect(g2d_fd, dma_fd, buf, 0, 60, BUFFER_WIDTH, 8, 0xFFFF00FF) < 0) {
		ret = 1;
		goto cleanup;
	}

	printf("\n🎉 All fillrect tests passed!\n");
	printf("Hardware acceleration is working perfectly!\n");

cleanup:
	if (mapped_ptr && mapped_ptr != MAP_FAILED)
		munmap(mapped_ptr, BUFFER_SIZE);
	if (dma_fd >= 0)
		close(dma_fd);
	if (memfd >= 0)
		close(memfd);
	if (g2d_fd >= 0)
		close(g2d_fd);

	return ret;
}
