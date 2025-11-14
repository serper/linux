// SPDX-License-Identifier: MIT
// Simple userspace test for /dev/g2d: BLIT I420/YV12 -> XRGB8888 and dump PPM
// - Allocates src/dst buffers from dma-heap (/dev/dma_heap/system)
// - Writes a synthetic YUV420 pattern in I420 or YV12 layout
// - Calls G2D_IOC_UNIFIED to convert to RGB
// - Writes result to PPM file for quick inspection

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <linux/dma-heap.h>
#include <linux/sunxi_g2d.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(EXIT_FAILURE);
}

static int open_or_die(const char *path, int flags)
{
    int fd = open(path, flags);
    if (fd < 0) die("open %s failed: %s", path, strerror(errno));
    return fd;
}

static int dma_heap_alloc_or_die(int heap_fd, size_t size)
{
    struct dma_heap_allocation_data data = {0};
    data.len = size;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    data.heap_flags = 0;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0)
        die("DMA_HEAP_IOCTL_ALLOC len=%zu failed: %s", size, strerror(errno));
    return (int)data.fd;
}

static void *mmap_or_die(int fd, size_t size)
{
    void *addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) die("mmap size=%zu failed: %s", size, strerror(errno));
    return addr;
}

static void write_ppm_xrgb8888(const char *path, const uint8_t *ptr, int w, int h, int stride)
{
    FILE *f = fopen(path, "wb");
    if (!f) die("fopen %s failed: %s", path, strerror(errno));
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = ptr + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            // XRGB8888: [B,G,R,X]? Our XRGB8888 implies 0xAARRGGBB? UAPI uses common convention
            // We assume little-endian XRGB8888, layout: [B, G, R, X]
            uint8_t b = row[x * 4 + 0];
            uint8_t g = row[x * 4 + 1];
            uint8_t r = row[x * 4 + 2];
            // Write RGB
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    }
    fclose(f);
}

static void gen_yuv420_color_bars(uint8_t *dst, int w, int h, bool yv12)
{
    // Simple vertical color bars. Compute Y, U, V per bar in BT.601 full range.
    // Bars: White, Yellow, Cyan, Green, Magenta, Red, Blue, Black.
    const struct { uint8_t r,g,b; } bars[] = {
        {255,255,255},{255,255,0},{0,255,255},{0,255,0},{255,0,255},{255,0,0},{0,0,255},{0,0,0}
    };
    int nbar = (int)ARRAY_SIZE(bars);

    int y_stride = w;
    int c_stride = (w + 1) / 2; // ceil(w/2)
    int y_size = y_stride * h;
    int c_w = (w + 1) / 2;
    int c_h = (h + 1) / 2;
    int c_size = c_stride * c_h;

    uint8_t *Y = dst;
    uint8_t *U = yv12 ? dst + y_size + c_size /* U after V */ : dst + y_size;
    uint8_t *V = yv12 ? dst + y_size /* V first */ : dst + y_size + c_size;

    // Fill Y plane per-pixel, subsample U/V per 2x2 block as average of block RGB->UV
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int bar = (x * nbar) / w; if (bar < 0) bar = 0; if (bar >= nbar) bar = nbar - 1;
            uint8_t r = bars[bar].r, g = bars[bar].g, b = bars[bar].b;
            // BT.601 full range approx
            int yy = (  77*r + 150*g +  29*b) / 256;         // ~Y  (0..255)
            Y[y * y_stride + x] = (uint8_t)yy;
        }
    }
    // U/V 2x2
    for (int by = 0; by < h; by += 2) {
        for (int bx = 0; bx < w; bx += 2) {
            int rsum=0, gsum=0, bsum=0, cnt=0;
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    int x = bx + dx, y = by + dy;
                    if (x >= w || y >= h) continue;
                    int bar = (x * nbar) / w; if (bar < 0) bar = 0; if (bar >= nbar) bar = nbar - 1;
                    rsum += bars[bar].r; gsum += bars[bar].g; bsum += bars[bar].b; cnt++;
                }
            }
            int r = rsum / cnt, g = gsum / cnt, b = bsum / cnt;
            // BT.601 full range UV approx centered at 128
            int uu = (-43*r - 85*g + 128*b) / 256 + 128;
            int vv = (128*r - 107*g - 21*b) / 256 + 128;
            if (uu < 0) uu = 0; if (uu > 255) uu = 255;
            if (vv < 0) vv = 0; if (vv > 255) vv = 255;
            int cx = bx / 2, cy = by / 2;
            U[cy * c_stride + cx] = (uint8_t)uu;
            V[cy * c_stride + cx] = (uint8_t)vv;
        }
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [--format i420|yv12] [--width W] [--height H] [--out file.ppm] [--bt709]\n\n"
        "Creates a YUV420 test pattern (I420 or YV12), converts to XRGB8888 via /dev/g2d, and writes a PPM.\n"
        "Requires: /dev/g2d and /dev/dma_heap/system.\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *fmt_str = "i420";
    int width = 320, height = 240;
    const char *out_path = "out.ppm";
    bool bt709 = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--format") && i+1 < argc) { fmt_str = argv[++i]; }
        else if (!strcmp(argv[i], "--width") && i+1 < argc) { width = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--height") && i+1 < argc) { height = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--out") && i+1 < argc) { out_path = argv[++i]; }
        else if (!strcmp(argv[i], "--bt709")) { bt709 = true; }
        else { usage(argv[0]); return EXIT_FAILURE; }
    }

    bool yv12 = false;
    uint32_t src_fmt = G2D_FMT_YUV420_P;
    if (!strcmp(fmt_str, "yv12")) { yv12 = true; src_fmt = G2D_FMT_YUV420_P_VU; }
    else if (!strcmp(fmt_str, "i420")) { yv12 = false; src_fmt = G2D_FMT_YUV420_P; }
    else { usage(argv[0]); return EXIT_FAILURE; }

    if (width <= 0 || height <= 0) die("invalid size %dx%d", width, height);

    // Compute sizes/strides for planar 420
    int y_stride = width;
    int c_stride = (width + 1) / 2;
    int y_size = y_stride * height;
    int c_h = (height + 1) / 2;
    int c_size = c_stride * c_h;
    size_t src_size = (size_t)y_size + 2u * (size_t)c_size;

    int dst_stride = width * 4; // XRGB8888
    size_t dst_size = (size_t)dst_stride * (size_t)height;

    // Open devices
    int g2d_fd = open_or_die("/dev/g2d", O_RDWR);
    int heap_fd = open_or_die("/dev/dma_heap/system", O_RDWR);

    // Allocate src/dst dma-bufs
    int src_dma = dma_heap_alloc_or_die(heap_fd, src_size);
    int dst_dma = dma_heap_alloc_or_die(heap_fd, dst_size);

    // mmap and initialize patterns
    uint8_t *src_ptr = mmap_or_die(src_dma, src_size);
    uint8_t *dst_ptr = mmap_or_die(dst_dma, dst_size);

    memset(dst_ptr, 0, dst_size);
    gen_yuv420_color_bars(src_ptr, width, height, yv12);

    // Prepare blit
    struct g2d_blit blit;
    memset(&blit, 0, sizeof(blit));

    blit.src.width = width;
    blit.src.height = height;
    blit.src.format = src_fmt;
    blit.src.stride[0] = y_stride;
    blit.src.stride[1] = c_stride;
    blit.src.stride[2] = c_stride;
    blit.src.crop_x = 0;
    blit.src.crop_y = 0;
    blit.src.crop_w = width;
    blit.src.crop_h = height;
    blit.src.dma_fd = src_dma;
    blit.src.alpha = 255;
    /* Source is YUV (no per-pixel alpha). Use GLOBAL_ALPHA to supply an
     * explicit global alpha value. PIXEL_ALPHA requires a format with an
     * alpha channel (eg. ARGB) and may be rejected by the driver for
     * non-alpha formats. */
    blit.src.alpha_mode = G2D_GLOBAL_ALPHA;
    blit.src.premul_mode = G2D_PREMUL_NONE;
    blit.src.color_space = bt709 ? G2D_COLOR_SPACE_BT709 : G2D_COLOR_SPACE_BT601;

    blit.dst.width = width;
    blit.dst.height = height;
    blit.dst.format = G2D_FMT_XRGB8888;
    blit.dst.stride[0] = dst_stride;
    blit.dst.crop_x = 0;
    blit.dst.crop_y = 0;
    blit.dst.crop_w = width;
    blit.dst.crop_h = height;
    blit.dst.dma_fd = dst_dma;

    blit.out.dma_fd = -1; // in-place into dst

    blit.dst_x = 0;
    blit.dst_y = 0;
    blit.dst_w = width;
    blit.dst_h = height;

    blit.flags = 0;
    blit.bld_mode = G2D_BLD_COPY; // SRCCOPY path

    if (ioctl(g2d_fd, G2D_IOC_UNIFIED, &blit) < 0) {
        die("G2D_IOC_UNIFIED failed: %s", strerror(errno));
    }

    // Optionally wait on fence if returned
    if (blit.fence_fd_out >= 0) {
        // Simple blocking read of fence via poll/select could be used; as a simplification, just sleep a bit
        usleep(5 * 1000);
        close(blit.fence_fd_out);
    }

    // Dump PPM
    write_ppm_xrgb8888(out_path, dst_ptr, width, height, dst_stride);
    fprintf(stderr, "Wrote %s (%dx%d) from %s\n", out_path, width, height, yv12 ? "YV12" : "I420");

    // Cleanup
    munmap(src_ptr, src_size);
    munmap(dst_ptr, dst_size);
    close(src_dma);
    close(dst_dma);
    close(heap_fd);
    close(g2d_fd);
    return 0;
}
