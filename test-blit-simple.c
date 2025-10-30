/*
 * Test G2D BLIT - Casos básicos sin escalado
 * 
 * Tests:
 * 1. Copy completo (toda la imagen fuente)
 * 2. Copy parcial con crop (región de la fuente)
 * 3. Copy a diferentes posiciones del destino
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

static void fill_solid(uint32_t *pixels, int width, int height, int stride_bytes, uint32_t color)
{
	int stride_pixels = stride_bytes / 4;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			pixels[y * stride_pixels + x] = color;
		}
	}
}

int main(void)
{
	int g2d_fd, src_fd, dst_fd;
	void *src_map, *dst_map;
	struct g2d_blit blit;
	int ret = 0;
	
	printf("=== Test G2D BLIT (sin escalado) ===\n");
	
	g2d_fd = open("/dev/g2d", O_RDWR);
	if (g2d_fd < 0) {
		perror("open(/dev/g2d)");
		return 1;
	}
	
	/* Buffers: 640x480 ARGB8888 */
	const int width = 640;
	const int height = 480;
	const int stride = width * 4;
	size_t size = stride * height;
	
	/* Test 1: Copy completo - azul a rojo */
	printf("\n=== Test 1: Copy completo 640x480 ===\n");
	
	src_fd = alloc_dmabuf(size);
	dst_fd = alloc_dmabuf(size);
	if (src_fd < 0 || dst_fd < 0) {
		ret = 1;
		goto cleanup;
	}
	
	src_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, src_fd, 0);
	dst_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dst_fd, 0);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
		perror("mmap");
		ret = 1;
		goto cleanup;
	}
	
	/* Source: azul */
	fill_solid((uint32_t *)src_map, width, height, stride, 0xFF0000FF);
	/* Destination: negro */
	fill_solid((uint32_t *)dst_map, width, height, stride, 0xFF000000);
	
	memset(&blit, 0, sizeof(blit));
	blit.src.width = width;
	blit.src.height = height;
	blit.src.format = G2D_FMT_ARGB8888;
	blit.src.stride[0] = stride;
	blit.src.dma_fd = src_fd;
	blit.src.crop_w = width;
	blit.src.crop_h = height;
	
	blit.dst.width = width;
	blit.dst.height = height;
	blit.dst.format = G2D_FMT_ARGB8888;
	blit.dst.stride[0] = stride;
	blit.dst.dma_fd = dst_fd;
	blit.dst_x = 0;
	blit.dst_y = 0;
	blit.dst_w = width;
	blit.dst_h = height;
	blit.fence_fd_in = -1;
	
	printf("Copiando 640x480 azul...\n");
	if (ioctl(g2d_fd, G2D_IOC_BLIT, &blit) < 0) {
		perror("ioctl(G2D_IOC_BLIT)");
		ret = 1;
		goto cleanup_maps;
	}
	
	/* Verificar que dst es azul */
	uint32_t *dst_pixels = (uint32_t *)dst_map;
	int correct = 0;
	for (int i = 0; i < width * height; i++) {
		if (dst_pixels[i] == 0xFF0000FF)
			correct++;
	}
	printf("✓ Test 1: %d/%d píxeles correctos (%.1f%%)\n",
	       correct, width * height, (100.0 * correct) / (width * height));
	
	munmap(src_map, size);
	munmap(dst_map, size);
	close(src_fd);
	close(dst_fd);
	
	/* Test 2: Copy parcial con crop - 200x150 del centro */
	printf("\n=== Test 2: Copy parcial con crop 200x150 ===\n");
	
	src_fd = alloc_dmabuf(size);
	dst_fd = alloc_dmabuf(size);
	if (src_fd < 0 || dst_fd < 0) {
		ret = 1;
		goto cleanup;
	}
	
	src_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, src_fd, 0);
	dst_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dst_fd, 0);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
		perror("mmap");
		ret = 1;
		goto cleanup;
	}
	
	/* Source: verde */
	fill_solid((uint32_t *)src_map, width, height, stride, 0xFF00FF00);
	/* Destination: negro */
	fill_solid((uint32_t *)dst_map, width, height, stride, 0xFF000000);
	
	memset(&blit, 0, sizeof(blit));
	blit.src.width = width;
	blit.src.height = height;
	blit.src.format = G2D_FMT_ARGB8888;
	blit.src.stride[0] = stride;
	blit.src.dma_fd = src_fd;
	blit.src.crop_x = 220;  /* Crop del centro */
	blit.src.crop_y = 165;
	blit.src.crop_w = 200;
	blit.src.crop_h = 150;
	
	blit.dst.width = width;
	blit.dst.height = height;
	blit.dst.format = G2D_FMT_ARGB8888;
	blit.dst.stride[0] = stride;
	blit.dst.dma_fd = dst_fd;
	blit.dst_x = 50;  /* Destino offset */
	blit.dst_y = 50;
	blit.dst_w = 200;  /* Sin escalado */
	blit.dst_h = 150;
	blit.fence_fd_in = -1;
	
	printf("Copiando región 200x150 del centro a (50,50)...\n");
	if (ioctl(g2d_fd, G2D_IOC_BLIT, &blit) < 0) {
		perror("ioctl(G2D_IOC_BLIT)");
		ret = 1;
		goto cleanup_maps;
	}
	
	/* Verificar región copiada (verde) y resto (negro) */
	dst_pixels = (uint32_t *)dst_map;
	int green_count = 0;
	int black_count = 0;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			uint32_t pixel = dst_pixels[y * width + x];
			if (x >= 50 && x < 250 && y >= 50 && y < 200) {
				/* Región copiada - debe ser verde */
				if (pixel == 0xFF00FF00)
					green_count++;
			} else {
				/* Resto - debe ser negro */
				if (pixel == 0xFF000000)
					black_count++;
			}
		}
	}
	int expected_green = 200 * 150;
	int expected_black = (width * height) - expected_green;
	printf("✓ Test 2: Verde=%d/%d (%.1f%%), Negro=%d/%d (%.1f%%)\n",
	       green_count, expected_green, (100.0 * green_count) / expected_green,
	       black_count, expected_black, (100.0 * black_count) / expected_black);
	
	munmap(src_map, size);
	munmap(dst_map, size);
	close(src_fd);
	close(dst_fd);
	
	/* Test 3: Múltiples copias a diferentes posiciones */
	printf("\n=== Test 3: Múltiples copias 100x100 ===\n");
	
	src_fd = alloc_dmabuf(size);
	dst_fd = alloc_dmabuf(size);
	if (src_fd < 0 || dst_fd < 0) {
		ret = 1;
		goto cleanup;
	}
	
	src_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, src_fd, 0);
	dst_map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dst_fd, 0);
	if (src_map == MAP_FAILED || dst_map == MAP_FAILED) {
		perror("mmap");
		ret = 1;
		goto cleanup;
	}
	
	/* Source: rojo */
	fill_solid((uint32_t *)src_map, width, height, stride, 0xFFFF0000);
	/* Destination: negro */
	fill_solid((uint32_t *)dst_map, width, height, stride, 0xFF000000);
	
	/* Copy a 4 esquinas */
	int positions[][2] = {
		{0, 0},           /* Esquina superior izquierda */
		{540, 0},         /* Esquina superior derecha */
		{0, 380},         /* Esquina inferior izquierda */
		{540, 380}        /* Esquina inferior derecha */
	};
	
	for (int i = 0; i < 4; i++) {
		memset(&blit, 0, sizeof(blit));
		blit.src.width = width;
		blit.src.height = height;
		blit.src.format = G2D_FMT_ARGB8888;
		blit.src.stride[0] = stride;
		blit.src.dma_fd = src_fd;
		blit.src.crop_w = 100;
		blit.src.crop_h = 100;
		
		blit.dst.width = width;
		blit.dst.height = height;
		blit.dst.format = G2D_FMT_ARGB8888;
		blit.dst.stride[0] = stride;
		blit.dst.dma_fd = dst_fd;
		blit.dst_x = positions[i][0];
		blit.dst_y = positions[i][1];
		blit.dst_w = 100;
		blit.dst_h = 100;
		blit.fence_fd_in = -1;
		
		printf("  Copiando cuadrado %d a (%d,%d)...\n", i+1, positions[i][0], positions[i][1]);
		if (ioctl(g2d_fd, G2D_IOC_BLIT, &blit) < 0) {
			perror("ioctl(G2D_IOC_BLIT)");
			ret = 1;
			goto cleanup_maps;
		}
	}
	
	/* Verificar que las 4 regiones son rojas */
	dst_pixels = (uint32_t *)dst_map;
	int red_count = 0;
	for (int i = 0; i < 4; i++) {
		int x0 = positions[i][0];
		int y0 = positions[i][1];
		for (int y = y0; y < y0 + 100; y++) {
			for (int x = x0; x < x0 + 100; x++) {
				if (dst_pixels[y * width + x] == 0xFFFF0000)
					red_count++;
			}
		}
	}
	int expected_red = 4 * 100 * 100;
	printf("✓ Test 3: %d/%d píxeles rojos (%.1f%%)\n",
	       red_count, expected_red, (100.0 * red_count) / expected_red);
	
cleanup_maps:
	munmap(src_map, size);
	munmap(dst_map, size);
cleanup:
	close(src_fd);
	close(dst_fd);
	close(g2d_fd);
	
	printf("\n=== Tests completados (ret=%d) ===\n", ret);
	return ret;
}
