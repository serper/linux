/*
 * demo-chromakey.c - Demostración de Chromakey (Green Screen) con G2D
 *
 * Compilar:
 *   gcc -o demo-chromakey demo-chromakey.c -O2 -Wall
 *
 * Ejecutar:
 *   ./demo-chromakey
 *
 * Descripción:
 *   Crea un "actor" (rectángulo rojo) sobre fondo verde (green screen)
 *   Compone el resultado sobre un fondo con patrón usando chromakey
 *   El verde debe desaparecer, mostrando solo el actor sobre el fondo
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
#include <linux/dma-buf.h>
#include <linux/sunxi_g2d.h>

#define WIDTH  800
#define HEIGHT 600
#define BPP    4  /* ARGB8888 = 4 bytes per pixel */

/* Colores */
#define COLOR_GREEN    0xFF00FF00  /* Verde puro (ARGB) */
#define COLOR_RED      0xFFFF0000  /* Rojo puro (ARGB) */
#define COLOR_BLUE     0xFF0000FF  /* Azul puro (ARGB) */
#define COLOR_YELLOW   0xFFFFFF00  /* Amarillo (ARGB) */
#define COLOR_MAGENTA  0xFFFF00FF  /* Magenta (ARGB) */
#define COLOR_CYAN     0xFF00FFFF  /* Cyan (ARGB) */

/* Allocate DMA buffer using dma-heap */
int alloc_dma_buffer(size_t size, int *dma_fd, void **mapped) {
    int heap_fd = open("/dev/dma_heap/reserved", O_RDWR);
    if (heap_fd < 0) {
        heap_fd = open("/dev/dma_heap/system", O_RDWR);
        if (heap_fd < 0) {
            perror("open dma-heap");
            return -1;
        }
    }

    struct dma_heap_allocation_data alloc_data = {
        .len = size,
        .fd_flags = O_RDWR | O_CLOEXEC,
        .heap_flags = 0,
    };

    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc_data) < 0) {
        perror("DMA_HEAP_IOCTL_ALLOC");
        close(heap_fd);
        return -1;
    }

    close(heap_fd);

    *dma_fd = alloc_data.fd;
    *mapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, *dma_fd, 0);
    if (*mapped == MAP_FAILED) {
        perror("mmap");
        close(*dma_fd);
        return -1;
    }

    return 0;
}

/* Fill rectangle with solid color */
void fill_rect(uint32_t *buf, int buf_w, int buf_h, 
               int x, int y, int w, int h, uint32_t color) {
    for (int yy = y; yy < y + h && yy < buf_h; yy++) {
        for (int xx = x; xx < x + w && xx < buf_w; xx++) {
            buf[yy * buf_w + xx] = color;
        }
    }
}

/* Create a pattern background (checkerboard) */
void create_pattern_background(uint32_t *buf, int width, int height) {
    int square_size = 50;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int bx = x / square_size;
            int by = y / square_size;
            uint32_t color = ((bx + by) % 2) ? COLOR_CYAN : COLOR_MAGENTA;
            buf[y * width + x] = color;
        }
    }
}

/* Create green screen foreground with red "actor" */
void create_greenscreen_foreground(uint32_t *buf, int width, int height) {
    /* Fill entire buffer with green */
    for (int i = 0; i < width * height; i++) {
        buf[i] = COLOR_GREEN;
    }
    
    /* Draw red "actor" rectangle in the center */
    int actor_w = 200;
    int actor_h = 300;
    int actor_x = (width - actor_w) / 2;
    int actor_y = (height - actor_h) / 2;
    fill_rect(buf, width, height, actor_x, actor_y, actor_w, actor_h, COLOR_RED);
    
    /* Add some yellow details to the actor */
    fill_rect(buf, width, height, actor_x + 50, actor_y + 50, 100, 50, COLOR_YELLOW);
}

int main(int argc, char *argv[]) {
    int g2d_fd, foreground_fd, background_fd, output_fd;
    void *foreground_map, *background_map, *output_map;
    size_t buf_size = WIDTH * HEIGHT * BPP;
    int use_chromakey = 1;  /* Default: enable chromakey */

    printf("=== G2D Chromakey Demo ===\n");
    printf("Resolution: %dx%d ARGB8888\n", WIDTH, HEIGHT);

    if (argc > 1 && strcmp(argv[1], "--no-chromakey") == 0) {
        use_chromakey = 0;
        printf("Mode: WITHOUT chromakey (reference)\n");
    } else {
        printf("Mode: WITH chromakey (green screen)\n");
        printf("Usage: %s [--no-chromakey]\n", argv[0]);
    }

    /* Open G2D device */
    g2d_fd = open("/dev/sunxi_g2d", O_RDWR);
    if (g2d_fd < 0) {
        perror("open /dev/sunxi_g2d");
        return 1;
    }

    /* Allocate buffers */
    printf("\nAllocating DMA buffers...\n");
    if (alloc_dma_buffer(buf_size, &foreground_fd, &foreground_map) < 0) {
        fprintf(stderr, "Failed to allocate foreground buffer\n");
        return 1;
    }
    if (alloc_dma_buffer(buf_size, &background_fd, &background_map) < 0) {
        fprintf(stderr, "Failed to allocate background buffer\n");
        return 1;
    }
    if (alloc_dma_buffer(buf_size, &output_fd, &output_map) < 0) {
        fprintf(stderr, "Failed to allocate output buffer\n");
        return 1;
    }

    /* Create test images */
    printf("Creating test images...\n");
    printf("  - Foreground: Green screen with red actor\n");
    printf("  - Background: Cyan/Magenta checkerboard pattern\n");
    
    create_greenscreen_foreground((uint32_t *)foreground_map, WIDTH, HEIGHT);
    create_pattern_background((uint32_t *)background_map, WIDTH, HEIGHT);
    memset(output_map, 0, buf_size);  /* Clear output */

    /* Sync buffers */
    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE };
    ioctl(foreground_fd, DMA_BUF_IOCTL_SYNC, &sync);
    ioctl(background_fd, DMA_BUF_IOCTL_SYNC, &sync);
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    ioctl(foreground_fd, DMA_BUF_IOCTL_SYNC, &sync);
    ioctl(background_fd, DMA_BUF_IOCTL_SYNC, &sync);

    /* Perform G2D blit with chromakey */
    printf("\nPerforming G2D blit...\n");
    if (use_chromakey) {
        printf("  - Chromakey ENABLED\n");
        printf("  - Mode: Inside range = transparent\n");
        printf("  - Color range: 0x00C000 - 0x00FF80 (green)\n");
    } else {
        printf("  - Chromakey DISABLED (normal alpha blend)\n");
    }

    struct g2d_blit blit = {
        .src = {
            .width = WIDTH,
            .height = HEIGHT,
            .format = G2D_FMT_ARGB8888,
            .dma_fd = foreground_fd,
            .crop_w = WIDTH,
            .crop_h = HEIGHT,
        },
        .dst = {
            .width = WIDTH,
            .height = HEIGHT,
            .format = G2D_FMT_ARGB8888,
            .dma_fd = background_fd,
        },
        .dst_x = 0,
        .dst_y = 0,
        .dst_w = WIDTH,
        .dst_h = HEIGHT,
        
        .out = {
            .width = WIDTH,
            .height = HEIGHT,
            .format = G2D_FMT_ARGB8888,
            .dma_fd = output_fd,
        },
        
        /* Chromakey configuration */
        .color_key_enable = use_chromakey ? 1 : 0,
        .color_key_mode = 0,          /* 0 = inside range transparent */
        .color_key_min = 0x00C000,    /* Dark green (RGB) */
        .color_key_max = 0x00FF80,    /* Bright green (RGB) */
        
        .bld_mode = G2D_BLD_SRCOVER,  /* Source over */
        .flags = 0,
    };

    if (ioctl(g2d_fd, G2D_IOC_BLIT, &blit) < 0) {
        perror("G2D_IOC_BLIT");
        return 1;
    }

    /* Sync output buffer */
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
    ioctl(output_fd, DMA_BUF_IOCTL_SYNC, &sync);

    /* Save output as PPM */
    char filename[64];
    snprintf(filename, sizeof(filename), "chromakey_%s.ppm", 
             use_chromakey ? "enabled" : "disabled");
    
    printf("\nSaving output to %s...\n", filename);
    FILE *fp = fopen(filename, "wb");
    if (fp) {
        fprintf(fp, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
        uint32_t *pixels = (uint32_t *)output_map;
        for (int i = 0; i < WIDTH * HEIGHT; i++) {
            uint32_t pixel = pixels[i];
            uint8_t rgb[3] = {
                (pixel >> 16) & 0xFF,  /* R */
                (pixel >> 8) & 0xFF,   /* G */
                (pixel >> 0) & 0xFF    /* B */
            };
            fwrite(rgb, 1, 3, fp);
        }
        fclose(fp);
        printf("Output saved successfully!\n");
    }

    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ioctl(output_fd, DMA_BUF_IOCTL_SYNC, &sync);

    /* Verify results */
    printf("\nVerifying results...\n");
    uint32_t *out = (uint32_t *)output_map;
    int green_pixels = 0;
    int pattern_pixels = 0;
    int red_pixels = 0;
    
    for (int i = 0; i < WIDTH * HEIGHT; i++) {
        uint32_t p = out[i] & 0x00FFFFFF;  /* Ignore alpha */
        if ((p & 0x00FF00) > 0x80 && (p & 0xFF00FF) < 0x40) {
            green_pixels++;  /* Mostly green */
        } else if (p == (COLOR_CYAN & 0x00FFFFFF) || p == (COLOR_MAGENTA & 0x00FFFFFF)) {
            pattern_pixels++;  /* Background pattern visible */
        } else if ((p & 0xFF0000) > 0x80) {
            red_pixels++;  /* Red actor */
        }
    }
    
    printf("  Green pixels: %d (%.1f%%)\n", green_pixels, 
           100.0 * green_pixels / (WIDTH * HEIGHT));
    printf("  Pattern pixels: %d (%.1f%%)\n", pattern_pixels, 
           100.0 * pattern_pixels / (WIDTH * HEIGHT));
    printf("  Red pixels: %d (%.1f%%)\n", red_pixels, 
           100.0 * red_pixels / (WIDTH * HEIGHT));
    
    if (use_chromakey) {
        printf("\n✅ Expected: Green pixels ~0%%, Pattern pixels >50%%, Red pixels ~7.5%%\n");
        if (green_pixels < (WIDTH * HEIGHT) / 100) {  /* Less than 1% green */
            printf("✅ SUCCESS: Chromakey working! Green screen removed.\n");
        } else {
            printf("❌ WARNING: Chromakey may not be working correctly (too much green visible)\n");
        }
    } else {
        printf("\n📝 Reference mode: Green should be fully visible\n");
    }

    /* Cleanup */
    munmap(foreground_map, buf_size);
    munmap(background_map, buf_size);
    munmap(output_map, buf_size);
    close(foreground_fd);
    close(background_fd);
    close(output_fd);
    close(g2d_fd);

    printf("\n=== Demo Complete ===\n");
    printf("View output: convert %s %s.png && xdg-open %s.png\n", 
           filename, filename, filename);
    
    return 0;
}
