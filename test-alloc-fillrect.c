/*
 * Test G2D ALLOC_BUFFER + fillrect
 * El driver asigna el buffer y lo exporta como DMA-BUF
 * 
 * Compila: arm-linux-gnueabihf-gcc -o test-alloc-fillrect test-alloc-fillrect.c -static -I./include/uapi
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <linux/sunxi_g2d.h>

int main(void)
{
	int g2d_fd = -1;
	struct g2d_version ver = {0};
	struct g2d_alloc_buffer alloc = {0};
	struct g2d_fillrect req = {0};
	void *mapped = NULL;
	uint32_t *pixels;
	const uint32_t width = 800;
	const uint32_t height = 600;
	const uint32_t bpp = 4;
	const size_t size = width * height * bpp;
	int ret;

	printf("=== Test G2D ALLOC_BUFFER + FILLRECT ===\n\n");

	/* Abrir G2D */
	g2d_fd = open("/dev/g2d", O_RDWR);
	if (g2d_fd < 0) {
		perror("open(/dev/g2d)");
		return 1;
	}

	printf("✓ G2D abierto: fd=%d\n", g2d_fd);

	/* Verificar versión */
	ret = ioctl(g2d_fd, G2D_IOC_GET_VERSION, &ver);
	if (ret < 0) {
		perror("G2D_IOC_GET_VERSION");
		close(g2d_fd);
		return 1;
	}

	printf("✓ Versión: HW=0x%08X, Driver=%u.%u.%u\n\n",
	       ver.hw_version, ver.driver_major, ver.driver_minor, ver.driver_patchlevel);

	/* Asignar buffer desde el driver */
	alloc.size = size;
	alloc.flags = 0;

	printf("Pidiendo buffer de %zu bytes al driver...\n", size);
	ret = ioctl(g2d_fd, G2D_IOC_ALLOC_BUFFER, &alloc);
	if (ret < 0) {
		perror("G2D_IOC_ALLOC_BUFFER");
		close(g2d_fd);
		return 1;
	}

	printf("✓ Buffer asignado: dma_fd=%d\n", alloc.dma_fd);

	/* Mapear para acceso CPU */
	mapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, alloc.dma_fd, 0);
	if (mapped == MAP_FAILED) {
		perror("mmap");
		close(alloc.dma_fd);
		close(g2d_fd);
		return 1;
	}

	printf("✓ Buffer mapeado en: %p\n", mapped);

	/* Limpiar buffer */
	memset(mapped, 0x00, size);
	pixels = (uint32_t *)mapped;

	printf("Estado inicial: [0]=0x%08X [10]=0x%08X\n\n", pixels[0], pixels[10]);

	/* Ejecutar fillrect */
	req.dst.width = width;
	req.dst.height = height;
	req.dst.format = G2D_FMT_ARGB8888;
	req.dst.stride[0] = width * bpp;
	req.dst.dma_fd = alloc.dma_fd;  /* ¡Usamos el fd que nos dio el driver! */
	req.dst.crop_x = 0;
	req.dst.crop_y = 0;
	req.dst.crop_w = width;
	req.dst.crop_h = height;

	req.dst_x = 100;
	req.dst_y = 50;
	req.dst_w = 600;
	req.dst_h = 500;
	req.color = 0xFF00FFFF; /* Cian */
	req.fence_fd_in = -1;
	req.fence_fd_out = -1;

	printf("Ejecutando fillrect: rectángulo CIAN 600x500...\n");
	ret = ioctl(g2d_fd, G2D_IOC_FILLRECT, &req);
	if (ret < 0) {
		perror("G2D_IOC_FILLRECT");
		munmap(mapped, size);
		close(alloc.dma_fd);
		close(g2d_fd);
		return 1;
	}

	printf("✓ Fillrect completado\n");

	/* Verificar resultado */
	printf("Estado final: [0]=0x%08X [10]=0x%08X [1000]=0x%08X\n", 
	       pixels[0], pixels[10], pixels[1000]);

	int correct = 0;
	int total = (width * height);
	for (int i = 0; i < total; i++) {
		if (pixels[i] == 0xFF00FFFF)
			correct++;
	}

	printf("\nResultado: %d/%d píxeles correctos (%.1f%%)\n",
	       correct, total, (correct * 100.0) / total);

	if (correct > (total / 2)) {
		printf("✓ ¡TEST EXITOSO!\n");
	} else {
		printf("✗ Test falló\n");
	}

	/* Cleanup */
	munmap(mapped, size);
	close(alloc.dma_fd);
	close(g2d_fd);

	return 0;
}
