/*
 * G2D Scale + Rotate Test Demo
 * Tests 2-pass operation: scale then rotate
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/dma-heap.h>
#include <linux/sunxi_g2d.h>
#include <signal.h>
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

/* Create a small directional pattern to test scaling + rotation */
static int create_test_pattern(int g2d_fd, int ion_fd, int width, int height)
{
	uint32_t *pattern = malloc(width * height * 4);
	if (!pattern) {
		perror("malloc pattern");
		return -1;
	}

	/* Create a simple gradient with corner markers */
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			uint8_t r = (x * 255) / width;
			uint8_t g = (y * 255) / height;
			uint8_t b = 128;
			
			/* Mark corners */
			if ((x < 5 && y < 5) || (x >= width - 5 && y < 5) ||
			    (x < 5 && y >= height - 5) || (x >= width - 5 && y >= height - 5)) {
				/* White corners */
				pattern[y * width + x] = 0xFFFFFFFF;
			} else {
				pattern[y * width + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
			}
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

	printf("G2D Scale + Rotate Test Demo\n");
	printf("=============================\n\n");

	/* Initialize DRM display */
	ret = drm_display_init(&disp);
	if (ret < 0) {
		fprintf(stderr, "Failed to initialize DRM display\n");
		return 1;
	}

	printf("Display: %dx%d\n", disp.width, disp.height);

	/* Small source pattern: 64x64 */
	int src_size = 64;
	int src_buffer_size = src_size * src_size * 4;

	/* Allocate source buffer */
	int src_ion_fd = alloc_ion_buffer(src_buffer_size);
	if (src_ion_fd < 0) {
		fprintf(stderr, "Failed to allocate source buffer\n");
		drm_display_cleanup(&disp);
		return 1;
	}
	printf("Source buffer allocated: fd=%d size=%d bytes (%dx%d ARGB8888)\n",
	       src_ion_fd, src_buffer_size, src_size, src_size);

	/* Create test pattern */
	ret = create_test_pattern(disp.g2d_fd, src_ion_fd, src_size, src_size);
	if (ret < 0) {
		fprintf(stderr, "Failed to create pattern\n");
		close(src_ion_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	printf("Test pattern created: 64x64 gradient with white corners\n");

	/* Export framebuffer as dmabuf */
	int fb_dmabuf = drm_export_page_dmabuf(&disp, 0);
	if (fb_dmabuf < 0) {
		fprintf(stderr, "Failed to export framebuffer dmabuf\n");
		close(src_ion_fd);
		drm_display_cleanup(&disp);
		return 1;
	}
	printf("Framebuffer dmabuf exported: fd=%d\n", fb_dmabuf);

	/* Fill background with black */
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
	fill.color = 0xFF000000;
	fill.color_format = G2D_FMT_ARGB8888;
	fill.fence_fd_in = -1;
	fill.fence_fd_out = -1;

	ret = ioctl(disp.g2d_fd, G2D_IOC_FILLRECT, &fill);
	if (ret < 0) {
		perror("G2D_IOC_FILLRECT");
	} else if (fill.fence_fd_out >= 0) {
		close(fill.fence_fd_out);
	}

	printf("\nTesting scale + rotation combinations:\n");
	printf("  Scale 64x64 -> 200x200 with different rotations\n\n");

	/* Test configurations  */
	struct {
		int x, y;
		uint32_t flags;
		const char *desc;
	} tests[] = {
		{50, 50, 0, "Scale only (no rotation)"},
		{300, 50, G2D_BLIT_FLAG_ROTATE_90, "Scale + 90° rotation"},
		{50, 230, G2D_BLIT_FLAG_ROTATE_180, "Scale + 180° rotation"},  /* Adjusted Y to fit 200px */
		{300, 230, G2D_BLIT_FLAG_ROTATE_270, "Scale + 270° rotation"}, /* Adjusted Y to fit 200px */
	};

	for (int i = 0; i < 4; i++) {
		struct g2d_blit blit = {0};
		
		/* Source: small 64x64 pattern */
		blit.src.width = src_size;
		blit.src.height = src_size;
		blit.src.format = G2D_FMT_XRGB8888;  /* Changed from ARGB to XRGB to disable alpha */
		blit.src.stride[0] = src_size * 4;
		blit.src.dma_fd = src_ion_fd;
		blit.src.crop_x = 0;
		blit.src.crop_y = 0;
		blit.src.crop_w = src_size;
		blit.src.crop_h = src_size;
		blit.src.alpha = 0;  /* Changed from 255 to 0 */
		blit.src.alpha_mode = 0;  /* Disable alpha completely */
		
		/* Destination: framebuffer with scaling */
		blit.dst.width = disp.width;
		blit.dst.height = disp.height;
		blit.dst.format = G2D_FMT_XRGB8888;
		blit.dst.stride[0] = disp.pitch;
		blit.dst.dma_fd = fb_dmabuf;
		
		blit.dst_x = tests[i].x;
		blit.dst_y = tests[i].y;
		blit.dst_w = 200;  /* Scale up from 64 to 200 */
		blit.dst_h = 200;
		
		blit.flags = tests[i].flags;
		blit.fence_fd_in = -1;
		blit.fence_fd_out = -1;
		blit.out.dma_fd = -1;
		
		printf("%s at (%d, %d)...\n", tests[i].desc, tests[i].x, tests[i].y);
		
		ret = ioctl(disp.g2d_fd, G2D_IOC_BLIT, &blit);
		if (ret < 0) {
			perror("  G2D_IOC_BLIT");
			fprintf(stderr, "  ✗ Failed: %s\n", tests[i].desc);
		} else {
			if (blit.fence_fd_out >= 0)
				close(blit.fence_fd_out);
			printf("  ✓ Success: %s\n", tests[i].desc);
		}
	}

	printf("\nAll tests completed. Check the display!\n");
	printf("You should see 4 scaled (200x200) patterns:\n");
	printf("- Top-left: normal orientation\n");
	printf("- Top-right: rotated 90°\n");
	printf("- Bottom-left: rotated 180°\n");
	printf("- Bottom-right: rotated 270°\n");
	printf("\nPress Ctrl+C to exit...\n");

	while (keep_running) {
		sleep(1);
	}

	printf("\nCleaning up...\n");
	close(fb_dmabuf);
	close(src_ion_fd);
	drm_display_cleanup(&disp);
	
	printf("Demo exited cleanly\n");
	return 0;
}
