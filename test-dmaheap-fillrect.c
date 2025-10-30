/*
 * Test G2D fillrect usando DMA-BUF HEAP (más estable que udmabuf)
 * Compila: arm-linux-gnueabihf-gcc -o test-dmaheap-fillrect test-dmaheap-fillrect.c -static -I./include/uapi
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
#include <linux/dma-heap.h>
#include <linux/sunxi_g2d.h>

/*
 * Crea un buffer usando DMA-BUF HEAP CMA
 * Retorna: dmabuf_fd, o -1 en error
 */
static int create_dmaheap_buffer(size_t size, void **cpu_ptr)
{
	int heap_fd = -1;
	int dmabuf_fd = -1;
	void *mapped = NULL;
	struct dma_heap_allocation_data alloc_data = {0};

	/* Abrir el heap CMA */
	heap_fd = open("/dev/dma_heap/default_cma_region", O_RDWR | O_CLOEXEC);
	if (heap_fd < 0) {
		perror("open(/dev/dma_heap/default_cma_region)");
		return -1;
	}

	/* Asignar memoria del heap CMA */
	alloc_data.len = size;
	alloc_data.fd = 0;
	alloc_data.fd_flags = O_RDWR | O_CLOEXEC;
	alloc_data.heap_flags = 0;

	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc_data) < 0) {
		perror("DMA_HEAP_IOCTL_ALLOC");
		close(heap_fd);
		return -1;
	}

	dmabuf_fd = alloc_data.fd;
	close(heap_fd); /* Ya no necesitamos el heap_fd */

	printf("✓ Asignado buffer DMA-BUF HEAP CMA: dmabuf_fd=%d, size=%zu\n",
	       dmabuf_fd, size);

	/* Mapear para acceso CPU */
	mapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf_fd, 0);
	if (mapped == MAP_FAILED) {
		perror("mmap(dmabuf_fd)");
		close(dmabuf_fd);
		return -1;
	}

	/* Inicializar a cero (patrón conocido) */
	memset(mapped, 0x00, size);
	printf("✓ Buffer mapeado en CPU: %p, limpiado a 0x00\n", mapped);

	*cpu_ptr = mapped;
	return dmabuf_fd;
}

/*
 * Verifica si el buffer fue modificado por G2D
 */
static void verify_buffer(void *buf, size_t size, uint32_t expected_color)
{
	uint32_t *pixels = (uint32_t *)buf;
	size_t num_pixels = size / sizeof(uint32_t);
	int modified = 0;
	int correct = 0;

	for (size_t i = 0; i < num_pixels; i++) {
		if (pixels[i] != 0x00000000) {
			modified++;
			if (pixels[i] == expected_color) {
				correct++;
			}
		}
	}

	printf("Verificación: %d/%zu píxeles modificados, %d correctos (esperado: 0x%08X)\n",
	       modified, num_pixels, correct, expected_color);

	if (correct > 0) {
		printf("✓ G2D escribió datos correctamente!\n");
	} else if (modified > 0) {
		printf("⚠ G2D escribió, pero color incorrecto (primeros 4: 0x%08X 0x%08X 0x%08X 0x%08X)\n",
		       pixels[0], pixels[1], pixels[2], pixels[3]);
	} else {
		printf("✗ Buffer sin modificar (primeros 4: 0x%08X 0x%08X 0x%08X 0x%08X)\n",
		       pixels[0], pixels[1], pixels[2], pixels[3]);
	}
}

/*
 * Test básico: Rectángulo rojo en ARGB8888
 */
static int test_fillrect_red(int g2d_fd)
{
	const uint32_t width = 1024;
	const uint32_t height = 600;
	const uint32_t bpp = 4; /* ARGB8888 */
	const size_t size = width * height * bpp;
	void *cpu_ptr = NULL;
	int dmabuf_fd = -1;
	struct g2d_fillrect req = {0};
	int ret;

	printf("\n=== Test 1: Rectángulo ROJO 200x150 en ARGB8888 ===\n");

	/* Crear buffer DMA-BUF HEAP */
	dmabuf_fd = create_dmaheap_buffer(size, &cpu_ptr);
	if (dmabuf_fd < 0) {
		return -1;
	}

	/* Configurar buffer destino */
	req.dst.width = width;
	req.dst.height = height;
	req.dst.format = G2D_FMT_ARGB8888;
	req.dst.stride[0] = width * bpp;
	req.dst.dma_fd = dmabuf_fd;
	req.dst.crop_x = 0;
	req.dst.crop_y = 0;
	req.dst.crop_w = width;
	req.dst.crop_h = height;

	/* Rectángulo rojo en el centro */
	req.dst_x = 100;
	req.dst_y = 50;
	req.dst_w = 200;
	req.dst_h = 150;
	req.color = 0xFFFF0000; /* ARGB: Rojo opaco */
	req.fence_fd_in = -1;
	req.fence_fd_out = -1;

	printf("Llamando G2D_IOC_FILLRECT...\n");
	ret = ioctl(g2d_fd, G2D_IOC_FILLRECT, &req);
	if (ret < 0) {
		perror("G2D_IOC_FILLRECT");
		munmap(cpu_ptr, size);
		close(dmabuf_fd);
		return -1;
	}

	printf("✓ ioctl() exitoso\n");

	/* Verificar resultado */
	verify_buffer(cpu_ptr, size, 0xFFFF0000);

	/* Cleanup */
	munmap(cpu_ptr, size);
	close(dmabuf_fd);
	return 0;
}

/*
 * Test 2: Rectángulo verde en XRGB8888
 */
static int test_fillrect_green(int g2d_fd)
{
	const uint32_t width = 800;
	const uint32_t height = 480;
	const uint32_t bpp = 4;
	const size_t size = width * height * bpp;
	void *cpu_ptr = NULL;
	int dmabuf_fd = -1;
	struct g2d_fillrect req = {0};
	int ret;

	printf("\n=== Test 2: Rectángulo VERDE 640x400 en XRGB8888 ===\n");

	dmabuf_fd = create_dmaheap_buffer(size, &cpu_ptr);
	if (dmabuf_fd < 0) {
		return -1;
	}

	req.dst.width = width;
	req.dst.height = height;
	req.dst.format = G2D_FMT_XRGB8888;
	req.dst.stride[0] = width * bpp;
	req.dst.dma_fd = dmabuf_fd;
	req.dst.crop_x = 0;
	req.dst.crop_y = 0;
	req.dst.crop_w = width;
	req.dst.crop_h = height;

	/* Rectángulo verde casi completo */
	req.dst_x = 80;
	req.dst_y = 40;
	req.dst_w = 640;
	req.dst_h = 400;
	req.color = 0xFF00FF00; /* Verde opaco */
	req.fence_fd_in = -1;
	req.fence_fd_out = -1;

	ret = ioctl(g2d_fd, G2D_IOC_FILLRECT, &req);
	if (ret < 0) {
		perror("G2D_IOC_FILLRECT");
		munmap(cpu_ptr, size);
		close(dmabuf_fd);
		return -1;
	}

	printf("✓ ioctl() exitoso\n");
	verify_buffer(cpu_ptr, size, 0xFF00FF00);

	munmap(cpu_ptr, size);
	close(dmabuf_fd);
	return 0;
}

int main(void)
{
	int g2d_fd = -1;
	struct g2d_version ver = {0};
	int ret;

	printf("=== Test G2D con DMA-BUF HEAP CMA ===\n");

	/* Abrir dispositivo G2D */
	g2d_fd = open("/dev/g2d", O_RDWR);
	if (g2d_fd < 0) {
		perror("open(/dev/g2d)");
		return 1;
	}

	printf("✓ Abierto /dev/g2d: fd=%d\n", g2d_fd);

	/* Verificar versión */
	ret = ioctl(g2d_fd, G2D_IOC_GET_VERSION, &ver);
	if (ret < 0) {
		perror("G2D_IOC_GET_VERSION");
		close(g2d_fd);
		return 1;
	}

	printf("✓ Versión G2D: HW=0x%08X, Driver=%u.%u.%u\n",
	       ver.hw_version, ver.driver_major, ver.driver_minor, ver.driver_patchlevel);

	/* Ejecutar tests */
	if (test_fillrect_red(g2d_fd) < 0) {
		printf("✗ Test 1 falló\n");
	}

	if (test_fillrect_green(g2d_fd) < 0) {
		printf("✗ Test 2 falló\n");
	}

	close(g2d_fd);
	printf("\n=== Tests completados ===\n");
	return 0;
}
