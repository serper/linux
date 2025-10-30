/*
 * Minimal test for sunxi-g2d fillrect with UDMABUF
 * Just tests that the ioctl doesn't crash
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

#define UDMABUF_FLAGS_CLOEXEC 0x01
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#endif
#ifndef F_SEAL_SHRINK
#define F_SEAL_SHRINK 0x0002
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

struct udmabuf_create {
	uint32_t memfd;
	uint32_t flags;
	uint64_t offset;
	uint64_t size;
};

#define UDMABUF_CREATE _IOW('u', 0x42, struct udmabuf_create)

enum g2d_pixel_format {
	G2D_FMT_ARGB8888 = 0,
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

#define G2D_IOC_MAGIC 'G'
#define G2D_IOCTL_FILLRECT _IOWR(G2D_IOC_MAGIC, 2, struct g2d_fillrect)

int main(void)
{
	int g2d_fd, memfd, udmabuf_fd, dmabuf_fd;
	struct udmabuf_create create;
	struct g2d_fillrect fillrect;
	const size_t size = 128 * 128 * 4;

	printf("Minimal G2D fillrect test\n");

	/* Create memfd */
	memfd = memfd_create("g2d", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (memfd < 0) {
		perror("memfd_create");
		return 1;
	}

	fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK);
	ftruncate(memfd, size);

	/* Create udmabuf */
	udmabuf_fd = open("/dev/udmabuf", O_RDWR);
	if (udmabuf_fd < 0) {
		perror("open udmabuf");
		close(memfd);
		return 1;
	}

	memset(&create, 0, sizeof(create));
	create.memfd = memfd;
	create.flags = 0;
	create.offset = 0;
	create.size = size;

	dmabuf_fd = ioctl(udmabuf_fd, UDMABUF_CREATE, &create);
	close(udmabuf_fd);
	
	if (dmabuf_fd < 0) {
		perror("UDMABUF_CREATE");
		close(memfd);
		return 1;
	}
	printf("Created dmabuf_fd=%d\n", dmabuf_fd);

	/* Open G2D */
	g2d_fd = open("/dev/g2d", O_RDWR);
	if (g2d_fd < 0) {
		perror("open g2d");
		close(dmabuf_fd);
		close(memfd);
		return 1;
	}

	/* Call fillrect */
	memset(&fillrect, 0, sizeof(fillrect));
	fillrect.dst.width = 128;
	fillrect.dst.height = 128;
	fillrect.dst.format = G2D_FMT_ARGB8888;
	fillrect.dst.dma_fd = dmabuf_fd;
	fillrect.dst_x = 0;
	fillrect.dst_y = 0;
	fillrect.dst_w = 128;
	fillrect.dst_h = 128;
	fillrect.color = 0xFFFF0000;
	fillrect.fence_fd_in = -1;

	printf("Calling G2D_IOCTL_FILLRECT...\n");
	if (ioctl(g2d_fd, G2D_IOCTL_FILLRECT, &fillrect) < 0) {
		perror("G2D_IOCTL_FILLRECT");
		close(g2d_fd);
		close(dmabuf_fd);
		close(memfd);
		return 1;
	}

	printf("✅ Success! ioctl returned without error\n");

	close(g2d_fd);
	close(dmabuf_fd);
	close(memfd);
	return 0;
}
