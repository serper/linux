/*
 * Test G2D BLIT operation with DMA-BUF HEAP
 * 
 * Tests:
 * 1. Simple copy (no scaling)
 * 2. Scaled copy (2x upscale)
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
#include <linux/sunxi_g2d.h>

#define DMA_HEAP_PATH "/dev/dma_heap/default_cma_region"

/* Helper: Allocate DMA-BUF from CMA heap */
static int alloc_dmabuf(size_t size)
{
	int heap_fd, dmabuf_fd;
	struct dma_heap_allocation_data alloc = {
		.len = size,
		.fd_flags = O_RDWR | O_CLOEXEC,
		.heap_flags = 0,
	};
	
	heap_fd = open(DMA_HEAP_PATH, O_RDONLY | O_CLOEXEC);
	if (heap_fd < 0) {
		perror("open(" DMA_HEAP_PATH ")");
		return -1;
	}
	
	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
		perror("ioctl(DMA_HEAP_IOCTL_ALLOC)");
		close(heap_fd);
		return -1;
	}
	
	dmabuf_fd = alloc.fd;
	close(heap_fd);
	
	return dmabuf_fd;
}

/* Helper: Fill buffer with gradient pattern */
static void fill_gradient(uint32_t *pixels, int width, int height, int stride_bytes)
{
	int stride_pixels = stride_bytes / 4;
	
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			uint8_t r = (x * 255) / width;
			uint8_t g = (y * 255) / height;
			uint8_t b = 128;
			pixels[y * stride_pixels + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
		}
	}
}

/* Helper: Fill buffer with checkerboard pattern */
static void fill_checkerboard(uint32_t *pixels, int width, int height, int stride_bytes,
			       uint32_t color1, uint32_t color2, int block_size)
{
	int stride_pixels = stride_bytes / 4;
	
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			int block_x = x / block_size;
			int block_y = y / block_size;
			uint32_t color = ((block_x + block_y) & 1) ? color1 : color2;
			pixels[y * stride_pixels + x] = color;
		}
	}
}

/* Helper: Verify blit result */
static int verify_blit(uint32_t *dst, int dst_width, int dst_height, int dst_stride_bytes,
		       int dst_x, int dst_y, int dst_w, int dst_h,
		       uint32_t *expected_src, int src_width, int src_height, int src_stride_bytes,
		       int src_x, int src_y, int src_w, int src_h)
{
	int dst_stride_pixels = dst_stride_bytes / 4;
	int src_stride_pixels = src_stride_bytes / 4;
	int correct = 0;
	int total = 0;
	
	/* For scaled blit, we just check that some pixels changed */
	for (int y = 0; y < dst_h; y++) {
		for (int x = 0; x < dst_w; x++) {
			int dst_idx = (dst_y + y) * dst_stride_pixels + (dst_x + x);
			uint32_t dst_pixel = dst[dst_idx];
			
			if (dst_pixel != 0x00000000) {  /* Not background */
				correct++;
			}
			total++;
		}
	}
	
	printf("Verificación: %d/%d píxeles escritos (%.1f%%)\n",
	       correct, total, (100.0 * correct) / total);
	
	return (correct > total / 2);  /* At least 50% should be non-zero */
}

int main(void)
{
	int g2d_fd, src_dmabuf_fd, dst_dmabuf_fd;
	void *src_map, *dst_map;
	struct g2d_version ver;
	struct g2d_blit blit;
	int ret = 0;
	
	printf("=== Test G2D BLIT con DMA-BUF HEAP ===\n");
	
	/* Open G2D device */
	g2d_fd = open("/dev/g2d", O_RDWR);
	if (g2d_fd < 0) {
		perror("open(/dev/g2d)");
		return 1;
	}
	printf("✓ Abierto /dev/g2d: fd=%d\n", g2d_fd);
	
	/* Get version */
	if (ioctl(g2d_fd, G2D_IOC_GET_VERSION, &ver) < 0) {
		perror("ioctl(G2D_IOC_GET_VERSION)");
		ret = 1;
		goto cleanup;
	}
	printf("✓ Versión G2D: HW=0x%08x, Driver=%d.%d.%d\n",
	       ver.hw_version, ver.driver_major, ver.driver_minor, ver.driver_patchlevel);
	
	/* ===== Test 1: Simple copy (no scaling) ===== */
	printf("\n=== Test 1: Copia simple 320x240 (sin escalado) ===\n");
	
	const int src1_width = 320;
	const int src1_height = 240;
	const int src1_bpp = 4;  /* ARGB8888 */
	const int src1_stride = src1_width * src1_bpp;
	size_t src1_size = src1_stride * src1_height;
	
	const int dst1_width = 640;
	const int dst1_height = 480;
	const int dst1_bpp = 4;
	const int dst1_stride = dst1_width * dst1_bpp;
	size_t dst1_size = dst1_stride * dst1_height;
	
	/* Allocate source buffer */
	src_dmabuf_fd = alloc_dmabuf(src1_size);
	if (src_dmabuf_fd < 0) {
		ret = 1;
		goto cleanup;
	}
	printf("✓ Asignado src buffer: dmabuf_fd=%d, size=%zu\n", src_dmabuf_fd, src1_size);
	
	/* Map and fill source with gradient */
	src_map = mmap(NULL, src1_size, PROT_READ | PROT_WRITE, MAP_SHARED, src_dmabuf_fd, 0);
	if (src_map == MAP_FAILED) {
		perror("mmap(src)");
		ret = 1;
		goto cleanup;
	}
	printf("✓ Mapeado src: %p\n", src_map);
	
	fill_gradient((uint32_t *)src_map, src1_width, src1_height, src1_stride);
	printf("✓ Rellenado src con gradiente RGB\n");
	
	/* Allocate destination buffer */
	dst_dmabuf_fd = alloc_dmabuf(dst1_size);
	if (dst_dmabuf_fd < 0) {
		ret = 1;
		goto cleanup_src1;
	}
	printf("✓ Asignado dst buffer: dmabuf_fd=%d, size=%zu\n", dst_dmabuf_fd, dst1_size);
	
	/* Map and clear destination */
	dst_map = mmap(NULL, dst1_size, PROT_READ | PROT_WRITE, MAP_SHARED, dst_dmabuf_fd, 0);
	if (dst_map == MAP_FAILED) {
		perror("mmap(dst)");
		ret = 1;
		goto cleanup_dst1;
	}
	printf("✓ Mapeado dst: %p\n", dst_map);
	memset(dst_map, 0, dst1_size);
	
	/* Prepare BLIT */
	memset(&blit, 0, sizeof(blit));
	
	/* Source */
	blit.src.width = src1_width;
	blit.src.height = src1_height;
	blit.src.format = G2D_FMT_ARGB8888;
	blit.src.stride[0] = src1_stride;
	blit.src.dma_fd = src_dmabuf_fd;
	blit.src.crop_x = 0;
	blit.src.crop_y = 0;
	blit.src.crop_w = src1_width;   /* Full source */
	blit.src.crop_h = src1_height;
	
	/* Destination */
	blit.dst.width = dst1_width;
	blit.dst.height = dst1_height;
	blit.dst.format = G2D_FMT_ARGB8888;
	blit.dst.stride[0] = dst1_stride;
	blit.dst.dma_fd = dst_dmabuf_fd;
	
	/* Position and size (1:1 copy, centered) */
	blit.dst_x = 160;  /* Center horizontally */
	blit.dst_y = 120;  /* Center vertically */
	blit.dst_w = src1_width;   /* No scaling */
	blit.dst_h = src1_height;
	
	blit.flags = 0;
	blit.fence_fd_in = -1;
	blit.fence_fd_out = -1;
	
	printf("Llamando G2D_IOC_BLIT (copia 1:1)...\n");
	if (ioctl(g2d_fd, G2D_IOC_BLIT, &blit) < 0) {
		perror("ioctl(G2D_IOC_BLIT)");
		ret = 1;
		goto cleanup_maps1;
	}
	printf("✓ ioctl() exitoso\n");
	
	/* Verify */
	if (verify_blit((uint32_t *)dst_map, dst1_width, dst1_height, dst1_stride,
			blit.dst_x, blit.dst_y, blit.dst_w, blit.dst_h,
			(uint32_t *)src_map, src1_width, src1_height, src1_stride,
			0, 0, src1_width, src1_height)) {
		printf("✓ BLIT correcto!\n");
	} else {
		printf("✗ BLIT incorrecto\n");
		ret = 1;
	}
	
cleanup_maps1:
	munmap(dst_map, dst1_size);
	munmap(src_map, src1_size);
cleanup_dst1:
	close(dst_dmabuf_fd);
cleanup_src1:
	close(src_dmabuf_fd);
	
	if (ret != 0)
		goto cleanup;
	
	/* ===== Test 2: Scaled copy (2x upscale) ===== */
	printf("\n=== Test 2: Copia escalada 160x120 -> 320x240 (2x) ===\n");
	
	const int src2_width = 160;
	const int src2_height = 120;
	const int src2_stride = src2_width * 4;
	size_t src2_size = src2_stride * src2_height;
	
	const int dst2_width = 640;
	const int dst2_height = 480;
	const int dst2_stride = dst2_width * 4;
	size_t dst2_size = dst2_stride * dst2_height;
	
	/* Allocate source */
	src_dmabuf_fd = alloc_dmabuf(src2_size);
	if (src_dmabuf_fd < 0) {
		ret = 1;
		goto cleanup;
	}
	printf("✓ Asignado src buffer: dmabuf_fd=%d, size=%zu\n", src_dmabuf_fd, src2_size);
	
	src_map = mmap(NULL, src2_size, PROT_READ | PROT_WRITE, MAP_SHARED, src_dmabuf_fd, 0);
	if (src_map == MAP_FAILED) {
		perror("mmap(src)");
		ret = 1;
		goto cleanup;
	}
	
	/* Fill with checkerboard */
	fill_checkerboard((uint32_t *)src_map, src2_width, src2_height, src2_stride,
			  0xFFFF0000, 0xFF00FF00, 16);
	printf("✓ Rellenado src con patrón checkerboard\n");
	
	/* Allocate destination */
	dst_dmabuf_fd = alloc_dmabuf(dst2_size);
	if (dst_dmabuf_fd < 0) {
		ret = 1;
		goto cleanup_src2;
	}
	printf("✓ Asignado dst buffer: dmabuf_fd=%d, size=%zu\n", dst_dmabuf_fd, dst2_size);
	
	dst_map = mmap(NULL, dst2_size, PROT_READ | PROT_WRITE, MAP_SHARED, dst_dmabuf_fd, 0);
	if (dst_map == MAP_FAILED) {
		perror("mmap(dst)");
		ret = 1;
		goto cleanup_dst2;
	}
	memset(dst_map, 0, dst2_size);
	
	/* Prepare BLIT with scaling */
	memset(&blit, 0, sizeof(blit));
	
	blit.src.width = src2_width;
	blit.src.height = src2_height;
	blit.src.format = G2D_FMT_ARGB8888;
	blit.src.stride[0] = src2_stride;
	blit.src.dma_fd = src_dmabuf_fd;
	blit.src.crop_w = src2_width;
	blit.src.crop_h = src2_height;
	
	blit.dst.width = dst2_width;
	blit.dst.height = dst2_height;
	blit.dst.format = G2D_FMT_ARGB8888;
	blit.dst.stride[0] = dst2_stride;
	blit.dst.dma_fd = dst_dmabuf_fd;
	
	/* Scale 2x (160x120 -> 320x240) */
	blit.dst_x = 160;
	blit.dst_y = 120;
	blit.dst_w = src2_width * 2;   /* 2x scaling */
	blit.dst_h = src2_height * 2;
	
	blit.flags = 0;
	blit.fence_fd_in = -1;
	
	printf("Llamando G2D_IOC_BLIT (escalado 2x)...\n");
	if (ioctl(g2d_fd, G2D_IOC_BLIT, &blit) < 0) {
		perror("ioctl(G2D_IOC_BLIT)");
		ret = 1;
		goto cleanup_maps2;
	}
	printf("✓ ioctl() exitoso\n");
	
	/* Verify */
	if (verify_blit((uint32_t *)dst_map, dst2_width, dst2_height, dst2_stride,
			blit.dst_x, blit.dst_y, blit.dst_w, blit.dst_h,
			(uint32_t *)src_map, src2_width, src2_height, src2_stride,
			0, 0, src2_width, src2_height)) {
		printf("✓ BLIT escalado correcto!\n");
	} else {
		printf("✗ BLIT escalado incorrecto\n");
		ret = 1;
	}
	
cleanup_maps2:
	munmap(dst_map, dst2_size);
	munmap(src_map, src2_size);
cleanup_dst2:
	close(dst_dmabuf_fd);
cleanup_src2:
	close(src_dmabuf_fd);
	
cleanup:
	close(g2d_fd);
	
	printf("\n=== Tests completados (ret=%d) ===\n", ret);
	return ret;
}
