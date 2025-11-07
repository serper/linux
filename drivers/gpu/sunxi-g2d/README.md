# Allwinner G2D Driver - Guía para Desarrolladores

## Descripción General

El driver `sunxi-g2d` proporciona acceso hardware al acelerador gráfico 2D de Allwinner (G2D) en SoCs como el T113-S3. Este acelerador es capaz de realizar operaciones de:

- **Relleno de rectángulos** (fillrect) - Relleno sólido de regiones ✅
- **Copia de imágenes** (blit) - Copia 1:1 y con crop ✅
- **Escalado de imágenes** - Escalado con VSU (Video Scaler Unit) ✅
- **Rotación y transformaciones** - Rotación 90°/180°/270° y flip H/V ✅
- **Alpha blending** - Composición con 12 modos Porter-Duff ✅
- **3-buffer compositing** - Preserva buffer destino (src + dst → out) ✅

### Características Implementadas

✅ **Operaciones sincrónicas con IRQ** (no busy-wait)  
✅ **Soporte DMA-BUF** para zero-copy con otros subsistemas  
✅ **FILLRECT**: Relleno de rectángulos con color sólido  
✅ **BLIT UNIFICADO**: Copia, escalado, rotación y alpha blending en una sola operación  
✅ **ALPHA BLENDING**: 12 modos Porter-Duff (SRCOVER default, COPY, DST, XOR, etc.)  
✅ **3-BUFFER COMPOSITING**: Buffer destino preservado (src + dst → out)  
✅ **TRANSFORMACIONES**: Rotación (90°/180°/270°) y flip horizontal/vertical  
✅ **VSU (Video Scaler Unit)**: Escalado hardware con filtros bicúbicos  
✅ **Gestión automática de poder** (clocks, reset, MBUS)  
✅ **Formatos RGB completos**: ARGB8888, XRGB8888, RGB565, RGB888, ARGB4444, ARGB1555, y variantes  
✅ **Formatos YUV/Video**: NV12, NV21, I420, YV12, YUYV, UYVY, NV16, y más (conversión YUV→RGB acelerada)  
✅ **API UAPI estable** en `/dev/g2d`

---

## Requisitos del Sistema

### Kernel

- Linux 6.1+ con soporte DMA-BUF
- Configuración requerida:
  ```
  CONFIG_DMABUF_HEAPS=y
  CONFIG_DMABUF_HEAPS_SYSTEM=y
  CONFIG_DMABUF_HEAPS_CMA=y
  ```

### Hardware

- Allwinner T113-S3 (probado) o SoCs compatibles
- G2D IP version 1.1.x
- Memoria CMA configurada en el device tree

---

## Inicio Rápido

### 1. Verificar que el driver esté cargado

```bash
# Verificar que /dev/g2d existe
ls -l /dev/g2d

# Ver información del módulo
lsmod | grep sunxi_g2d

# Ver logs del driver
dmesg | grep g2d
```

### 2. Ejemplo Mínimo: Fillrect

```c
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/sunxi_g2d.h>
#include <linux/dma-heap.h>

int main(void)
{
    int g2d_fd, heap_fd, dmabuf_fd;
    void *mapped;
    struct dma_heap_allocation_data alloc = {0};
    struct g2d_fillrect fill = {0};

    /* 1. Abrir dispositivos */
    g2d_fd = open("/dev/g2d", O_RDWR);
    heap_fd = open("/dev/dma_heap/default_cma_region", O_RDWR);

    /* 2. Asignar buffer DMA-BUF desde CMA heap */
    alloc.len = 1024 * 600 * 4; // 1024x600 ARGB8888
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc);
    dmabuf_fd = alloc.fd;
    close(heap_fd);

    /* 3. Mapear buffer para acceso CPU */
    mapped = mmap(NULL, alloc.len, PROT_READ | PROT_WRITE, 
                  MAP_SHARED, dmabuf_fd, 0);
    memset(mapped, 0, alloc.len);

    /* 4. Configurar operación fillrect */
    fill.dst.width = 1024;
    fill.dst.height = 600;
    fill.dst.format = G2D_FMT_ARGB8888;
    fill.dst.stride[0] = 1024 * 4;
    fill.dst.dma_fd = dmabuf_fd;  // ¡Importante!
    fill.dst.crop_x = 0;
    fill.dst.crop_y = 0;
    fill.dst.crop_w = 1024;
    fill.dst.crop_h = 600;

    fill.dst_x = 100;
    fill.dst_y = 50;
    fill.dst_w = 800;
    fill.dst_h = 500;
    fill.color = 0xFFFF0000;  // Rojo ARGB
    fill.fence_fd_in = -1;
    fill.fence_fd_out = -1;

    /* 5. Ejecutar fillrect (bloqueante, retorna cuando termina) */
    ioctl(g2d_fd, G2D_IOC_FILLRECT, &fill);

    /* 6. Leer resultado desde buffer mapeado */
    uint32_t *pixels = (uint32_t *)mapped;
    printf("Pixel[0] = 0x%08X\n", pixels[0]);

    /* 7. Cleanup */
    munmap(mapped, alloc.len);
    close(dmabuf_fd);
    close(g2d_fd);
    return 0;
}
```

**Compilación:**
```bash
arm-linux-gnueabihf-gcc -o test test.c -static
```

### 3. Ejemplo Mínimo: BLIT (copiar imagen)

```c
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/sunxi_g2d.h>
#include <linux/dma-heap.h>

int main(void)
{
    int g2d_fd, heap_fd, src_fd, dst_fd;
    void *src_map, *dst_map;
    struct dma_heap_allocation_data alloc = {0};
    struct g2d_blit blit = {0};

    /* 1. Abrir dispositivos */
    g2d_fd = open("/dev/g2d", O_RDWR);
    heap_fd = open("/dev/dma_heap/default_cma_region", O_RDWR);

    /* 2. Asignar buffer de origen (320x240) */
    alloc.len = 320 * 240 * 4;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc);
    src_fd = alloc.fd;
    src_map = mmap(NULL, alloc.len, PROT_READ | PROT_WRITE,
                   MAP_SHARED, src_fd, 0);
    
    /* Llenar origen con color azul */
    uint32_t *src_pixels = (uint32_t *)src_map;
    for (int i = 0; i < 320 * 240; i++)
        src_pixels[i] = 0xFF0000FF;  // Azul

    /* 3. Asignar buffer de destino (640x480) */
    alloc.len = 640 * 480 * 4;
    ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc);
    dst_fd = alloc.fd;
    dst_map = mmap(NULL, alloc.len, PROT_READ | PROT_WRITE,
                   MAP_SHARED, dst_fd, 0);
    memset(dst_map, 0, alloc.len);  // Negro
    close(heap_fd);

    /* 4. Configurar operación BLIT */
    blit.src.width = 320;
    blit.src.height = 240;
    blit.src.format = G2D_FMT_ARGB8888;
    blit.src.stride[0] = 320 * 4;
    blit.src.dma_fd = src_fd;
    blit.src.crop_w = 320;  // Copiar todo el origen
    blit.src.crop_h = 240;

    blit.dst.width = 640;
    blit.dst.height = 480;
    blit.dst.format = G2D_FMT_ARGB8888;
    blit.dst.stride[0] = 640 * 4;
    blit.dst.dma_fd = dst_fd;
    blit.dst_x = 160;  // Centrado
    blit.dst_y = 120;
    blit.dst_w = 320;  // Sin escalado (1:1)
    blit.dst_h = 240;
    blit.fence_fd_in = -1;

    /* 5. Ejecutar BLIT */
    if (ioctl(g2d_fd, G2D_IOC_BLIT, &blit) < 0) {
        perror("G2D_IOC_BLIT");
        return 1;
    }

    /* 6. Verificar resultado */
    uint32_t *dst_pixels = (uint32_t *)dst_map;
    printf("Pixel destino[0,0] = 0x%08X (negro)\n", dst_pixels[0]);
    printf("Pixel destino[160,120] = 0x%08X (azul)\n", 
           dst_pixels[120 * 640 + 160]);

    /* 7. Cleanup */
    munmap(src_map, 320 * 240 * 4);
    munmap(dst_map, 640 * 480 * 4);
    close(src_fd);
    close(dst_fd);
    close(g2d_fd);
    return 0;
}
```

---

## API Reference

### Abrir el Dispositivo

```c
int fd = open("/dev/g2d", O_RDWR);
```

### IOCTLs Disponibles

#### `G2D_IOC_GET_VERSION` - Obtener versión

```c
struct g2d_version ver;
ioctl(fd, G2D_IOC_GET_VERSION, &ver);

printf("HW version: 0x%08X\n", ver.hw_version);
printf("Driver: %u.%u.%u\n", ver.driver_major, 
       ver.driver_minor, ver.driver_patchlevel);
```

#### `G2D_IOC_FILLRECT` - Rellenar rectángulo

```c
struct g2d_fillrect fill = {
    .dst = {
        .width = 1920,
        .height = 1080,
        .format = G2D_FMT_ARGB8888,
        .stride[0] = 1920 * 4,
        .dma_fd = dmabuf_fd,  // DMA-BUF fd (ver "Gestión de Buffers")
        .crop_x = 0,
        .crop_y = 0,
        .crop_w = 1920,
        .crop_h = 1080,
    },
    .dst_x = 100,      // Posición X del rectángulo
    .dst_y = 100,      // Posición Y del rectángulo
    .dst_w = 1720,     // Ancho del rectángulo
    .dst_h = 880,      // Alto del rectángulo
    .color = 0xFF00FF00,  // Verde ARGB
    .fence_fd_in = -1,    // Futuro: sync fence
    .fence_fd_out = -1,   // Futuro: retorna fence
};

int ret = ioctl(fd, G2D_IOC_FILLRECT, &fill);
if (ret < 0) {
    perror("G2D_IOC_FILLRECT");
}
```

**Notas:**
- La operación es **bloqueante** por defecto
- Retorna cuando el hardware termina (vía IRQ)
- El buffer debe ser accesible vía DMA

#### `G2D_IOC_BLIT` - Copiar/escalar imagen

Copia una región de una imagen origen a un buffer destino con soporte completo para escalado, rotación y flip.

```c
struct g2d_blit blit = {
    .src = {
        .width = 640,           // Ancho total del buffer origen
        .height = 480,          // Alto total del buffer origen
        .format = G2D_FMT_ARGB8888,
        .stride[0] = 640 * 4,   // Pitch en bytes
        .dma_fd = src_dmabuf_fd,
        .crop_x = 100,          // Región a copiar (opcional)
        .crop_y = 50,
        .crop_w = 320,          // 0 = ancho completo
        .crop_h = 240,          // 0 = alto completo
    },
    .dst = {
        .width = 1920,          // Ancho total del buffer destino
        .height = 1080,         // Alto total del buffer destino
        .format = G2D_FMT_ARGB8888,
        .stride[0] = 1920 * 4,
        .dma_fd = dst_dmabuf_fd,
    },
    .dst_x = 400,               // Posición de destino
    .dst_y = 300,
    .dst_w = 640,               // Puede diferir de crop_w (escalado)
    .dst_h = 480,               // Puede diferir de crop_h (escalado)
    .flags = G2D_BLT_ROTATE_90 | G2D_BLT_FLIP_HORIZONTAL,  // Opcional
    .fence_fd_in = -1,
    .fence_fd_out = -1,
};

int ret = ioctl(fd, G2D_IOC_BLIT, &blit);
if (ret < 0) {
    perror("G2D_IOC_BLIT");
}
```

**Notas importantes:**
- ✅ **Copia 1:1**: `dst_w == crop_w` y `dst_h == crop_h`
- ✅ **Escalado**: Hasta 8x upscale/downscale con VSU (filtros bicúbicos)
- ✅ **Crop de origen**: Especificar `crop_x`, `crop_y`, `crop_w`, `crop_h`
- ✅ **Rotación**: `G2D_BLT_ROTATE_90/180/270` (se aplica después del escalado)
- ✅ **Flip**: `G2D_BLT_FLIP_HORIZONTAL/VERTICAL` (combinable con rotación)
- ✅ **Múltiples operaciones**: Llamar ioctl varias veces consecutivas

**Casos de uso:**
- Copiar imágenes completas entre buffers
- Copiar regiones (tiles) de una imagen grande
- Escalar thumbnails o previews (ej: 1920x1080 → 320x180)
- Rotar imágenes de cámara (portrait ↔ landscape)
- Crear efectos espejo (flip horizontal/vertical)
- Componer UI juntando sprites/iconos en un framebuffer

**Ejemplo: Escalar y rotar región de 200x150 a 400x300**
```c
blit.src.width = 640;
blit.src.height = 480;
blit.src.crop_x = 220;   // Centro: (640-200)/2
blit.src.crop_y = 165;   // Centro: (480-150)/2
blit.src.crop_w = 200;
blit.src.crop_h = 150;

blit.dst_x = 50;
blit.dst_y = 50;
blit.dst_w = 400;  // Escalado 2x
blit.dst_h = 300;
blit.flags = G2D_BLT_ROTATE_90;  // Rotar 90° después de escalar
```

**Ejemplo: Thumbnail con flip**
```c
// Crear thumbnail 160x90 con flip horizontal
blit.src.crop_w = 1920;
blit.src.crop_h = 1080;
blit.dst_w = 160;
blit.dst_h = 90;
blit.flags = G2D_BLT_FLIP_HORIZONTAL;
```

---

## Gestión de Buffers

### Método Recomendado: DMA-BUF Heaps

El driver **NO asigna buffers** directamente. En su lugar, el desarrollador debe:

1. **Asignar buffer desde DMA-BUF heap** (CMA recomendado)
2. **Pasar el fd** al driver vía `struct g2d_buf`
3. El driver importa el DMA-BUF automáticamente

#### Heaps disponibles en T113-S3

```bash
ls -la /dev/dma_heap/
# default_cma_region  <- Recomendado para G2D
# system              <- Memoria del sistema
# reserved            <- Memoria reservada
```

#### Ejemplo completo de asignación

```c
#include <linux/dma-heap.h>

int create_g2d_buffer(size_t size, void **cpu_ptr)
{
    int heap_fd, dmabuf_fd;
    void *mapped;
    struct dma_heap_allocation_data alloc = {0};

    /* Abrir heap CMA */
    heap_fd = open("/dev/dma_heap/default_cma_region", O_RDWR | O_CLOEXEC);
    if (heap_fd < 0) {
        perror("open dma_heap");
        return -1;
    }

    /* Asignar buffer */
    alloc.len = size;
    alloc.fd = 0;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    alloc.heap_flags = 0;

    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
        perror("DMA_HEAP_IOCTL_ALLOC");
        close(heap_fd);
        return -1;
    }

    dmabuf_fd = alloc.fd;
    close(heap_fd);  // Ya no necesitamos el heap_fd

    /* Mapear para acceso CPU */
    mapped = mmap(NULL, size, PROT_READ | PROT_WRITE, 
                  MAP_SHARED, dmabuf_fd, 0);
    if (mapped == MAP_FAILED) {
        perror("mmap");
        close(dmabuf_fd);
        return -1;
    }

    *cpu_ptr = mapped;
    return dmabuf_fd;  // Este fd se pasa a G2D
}

/* Uso */
void *buf;
int dmabuf_fd = create_g2d_buffer(1920 * 1080 * 4, &buf);

struct g2d_fillrect fill = {
    .dst.dma_fd = dmabuf_fd,  // ¡Aquí!
    /* ... resto de la configuración ... */
};

ioctl(g2d_fd, G2D_IOC_FILLRECT, &fill);

/* Leer resultado desde 'buf' */
uint32_t *pixels = (uint32_t *)buf;

/* Cleanup */
munmap(buf, 1920 * 1080 * 4);
close(dmabuf_fd);
```

---

## Formatos de Píxeles Soportados

El driver soporta una amplia gama de formatos RGB y YUV para procesamiento de gráficos y video.

### Formatos RGB

| Formato | Enum | Descripción | Bytes/píxel |
|---------|------|-------------|-------------|
| **Formatos 32-bit** |
| ARGB8888 | `G2D_FMT_ARGB8888` | 8 bits alpha + RGB | 4 |
| ABGR8888 | `G2D_FMT_ABGR8888` | 8 bits alpha + BGR | 4 |
| RGBA8888 | `G2D_FMT_RGBA8888` | RGB + 8 bits alpha | 4 |
| BGRA8888 | `G2D_FMT_BGRA8888` | BGR + 8 bits alpha | 4 |
| XRGB8888 | `G2D_FMT_XRGB8888` | Sin alpha (X=ignorado) + RGB | 4 |
| XBGR8888 | `G2D_FMT_XBGR8888` | Sin alpha + BGR | 4 |
| RGBX8888 | `G2D_FMT_RGBX8888` | RGB + X ignorado | 4 |
| BGRX8888 | `G2D_FMT_BGRX8888` | BGR + X ignorado | 4 |
| **Formatos 24-bit** |
| RGB888 | `G2D_FMT_RGB888` | 8 bits RGB (packed) | 3 |
| BGR888 | `G2D_FMT_BGR888` | 8 bits BGR (packed) | 3 |
| **Formatos 16-bit** |
| RGB565 | `G2D_FMT_RGB565` | 5-6-5 bits RGB | 2 |
| BGR565 | `G2D_FMT_BGR565` | 5-6-5 bits BGR | 2 |
| ARGB4444 | `G2D_FMT_ARGB4444` | 4-4-4-4 bits ARGB | 2 |
| ABGR4444 | `G2D_FMT_ABGR4444` | 4-4-4-4 bits ABGR | 2 |
| RGBA4444 | `G2D_FMT_RGBA4444` | 4-4-4-4 bits RGBA | 2 |
| BGRA4444 | `G2D_FMT_BGRA4444` | 4-4-4-4 bits BGRA | 2 |
| ARGB1555 | `G2D_FMT_ARGB1555` | 1 bit alpha + 5-5-5 RGB | 2 |
| ABGR1555 | `G2D_FMT_ABGR1555` | 1 bit alpha + 5-5-5 BGR | 2 |
| RGBA5551 | `G2D_FMT_RGBA5551` | 5-5-5 RGB + 1 bit alpha | 2 |
| BGRA5551 | `G2D_FMT_BGRA5551` | 5-5-5 BGR + 1 bit alpha | 2 |

### Formatos YUV (Video)

Los formatos YUV son críticos para procesamiento de video. Todos están soportados por el hardware G2D.

**⚠️ IMPORTANTE**: Los formatos YUV solo pueden usarse en la **capa V0 (source/video layer)**. La capa UI2 (destination) debe usar formatos RGB.

| Formato | Enum | Descripción | Subsampling | Notas |
|---------|------|-------------|-------------|-------|
| **Formatos Interleaved (Packed) 4:2:2** |
| YVYU | `G2D_FMT_YUV422_I_YVYU` | Y-V-Y-U entrelazado | 4:2:2 | V antes de U |
| YUYV | `G2D_FMT_YUV422_I_YUYV` | Y-U-Y-V entrelazado | 4:2:2 | U antes de V |
| UYVY | `G2D_FMT_YUV422_I_UYVY` | U-Y-V-Y entrelazado | 4:2:2 | U primero |
| VYUY | `G2D_FMT_YUV422_I_VYUY` | V-Y-U-Y entrelazado | 4:2:2 | V primero |
| **Formatos Semi-Planar 4:2:2 (NV16)** |
| YUV422_SP_UVUV | `G2D_FMT_YUV422_SP_UVUV` | Y plane + UV interleaved | 4:2:2 | NV16 |
| YUV422_SP_VUVU | `G2D_FMT_YUV422_SP_VUVU` | Y plane + VU interleaved | 4:2:2 | NV61 |
| **Formatos Planar 4:2:2** |
| YUV422_P | `G2D_FMT_YUV422_P` | Y, U, V planes separados | 4:2:2 | I422/YV16 |
| **Formatos Semi-Planar 4:2:0 (NV12/NV21)** |
| YUV420_SP_UVUV | `G2D_FMT_YUV420_SP_UVUV` | Y plane + UV interleaved | 4:2:0 | **NV12** ⭐ |
| YUV420_SP_VUVU | `G2D_FMT_YUV420_SP_VUVU` | Y plane + VU interleaved | 4:2:0 | **NV21** ⭐ |
| **Formatos Planar 4:2:0 (I420/YV12)** |
| YUV420_P | `G2D_FMT_YUV420_P` | Y, U, V planes separados | 4:2:0 | **I420/YV12** ⭐ |
| **Formatos Semi-Planar 4:1:1** |
| YUV411_SP_UVUV | `G2D_FMT_YUV411_SP_UVUV` | Y plane + UV interleaved | 4:1:1 | Raro |
| YUV411_SP_VUVU | `G2D_FMT_YUV411_SP_VUVU` | Y plane + VU interleaved | 4:1:1 | Raro |
| **Formatos Planar 4:1:1** |
| YUV411_P | `G2D_FMT_YUV411_P` | Y, U, V planes separados | 4:1:1 | Raro |
| **Monocromo** |
| 8BPP_MONO | `G2D_FMT_8BPP_MONO` | Grayscale 8-bit | Mono | Y solamente |

⭐ **Formatos más comunes para video**:
- **NV12** (`YUV420_SP_UVUV`): Usado por FFmpeg, GStreamer, V4L2, cámaras
- **NV21** (`YUV420_SP_VUVU`): Usado por Android Camera API
- **I420/YV12** (`YUV420_P`): Formato planar estándar

### Uso de Formatos YUV

**Ejemplo: Escalar y convertir NV12 (video) a ARGB8888 (framebuffer)**

```c
struct g2d_blit blit = {
    .src = {
        .width = 1920,
        .height = 1080,
        .format = G2D_FMT_YUV420_SP_UVUV,  /* NV12 desde cámara/decoder */
        .stride[0] = 1920,     /* Y plane stride */
        .stride[1] = 1920,     /* UV plane stride */
        .dma_fd = video_dmabuf_fd,
        .crop_x = 0,
        .crop_y = 0,
        .crop_w = 1920,
        .crop_h = 1080,
    },
    .dst = {
        .width = 800,
        .height = 480,
        .format = G2D_FMT_ARGB8888,  /* Framebuffer RGB */
        .stride[0] = 800 * 4,
        .dma_fd = fb_dmabuf_fd,
    },
    .dst_x = 0,
    .dst_y = 0,
    .dst_w = 800,   /* Downscale 1920→800 */
    .dst_h = 480,   /* Downscale 1080→480 */
};

ioctl(g2d_fd, G2D_IOC_BLIT, &blit);  /* YUV→RGB + scale en una operación */
```

**Ventajas del soporte YUV:**
- ✅ Conversión YUV→RGB acelerada por hardware
- ✅ Escalado y conversión en un solo paso (sin buffer intermedio)
- ✅ Procesamiento eficiente de video (decoders producen YUV)
- ✅ Ahorro de memoria vs RGB (4:2:0 usa 1.5 bytes/píxel vs 4 bytes/píxel)

**Notas técnicas:**
- Los formatos YUV requieren `stride[0]` (Y plane) y opcionalmente `stride[1]` (UV/U plane)
- Para formatos planar (I420), se requiere `stride[2]` (V plane)
- El hardware G2D convierte automáticamente YUV→RGB al escribir en buffer destino RGB
- El color space usado es **BT.601** (estándar SD video)

---

## Alpha Blending y Composición (BLIT Unificado)

El driver G2D implementa alpha blending a través de la operación **BLIT unificada**, que combina copia, escalado, rotación y blending en una sola llamada.

### Pipeline Unificado

La operación `G2D_IOC_BLIT` detecta automáticamente qué módulos hardware activar:

```
BLIT → Detección automática → ROT (rotación) | VSU (escalado) | BLD (blending)
```

- **ROT**: Se activa si `flags & (ROTATE_90|ROTATE_180|ROTATE_270|FLIP_H|FLIP_V)`
- **VSU**: Se activa si `dst_w != crop_w` o `dst_h != crop_h` (escalado)
- **BLD**: Se activa si hay alpha blending (ver detección abajo)

### Detección Automática de Alpha Blending

El blending se activa automáticamente cuando:
1. `out.dma_fd >= 0` (operación de 3 buffers)
2. `src.alpha_mode == G2D_GLOBAL_ALPHA` o `G2D_MIXER_ALPHA`
3. `dst.alpha_mode == G2D_GLOBAL_ALPHA` o `G2D_MIXER_ALPHA`
4. `src.alpha_mode == G2D_PIXEL_ALPHA` y formato tiene alpha (ARGB8888)

### Modos de Alpha

```c
enum g2d_alpha_mode {
    G2D_PIXEL_ALPHA = 0,   /* Usa alpha de cada píxel */
    G2D_GLOBAL_ALPHA = 1,  /* Usa valor global .alpha */
    G2D_MIXER_ALPHA = 2,   /* Multiplica pixel × global */
};
```

### Premultiplicación de Alpha

El campo `premul_mode` en `struct g2d_buf` controla cómo se interpreta el canal alpha:

```c
enum g2d_premul_mode {
    G2D_PREMUL_NONE = 0,   /* Non-premultiplied (straight alpha) */
    G2D_PREMUL_ALPHA = 1,  /* Premultiplied alpha (color *= alpha) */
};
```

**Non-premultiplied alpha (G2D_PREMUL_NONE)** - Por defecto para ARGB estándar:
- Los componentes RGB son independientes del alpha
- Ejemplo: `0x80FF0000` = rojo semi-transparente (A=128, R=255, G=0, B=0)
- Usado por la mayoría de formatos de imagen (PNG, OpenGL por defecto)

**Premultiplied alpha (G2D_PREMUL_ALPHA)** - Optimizado para composición:
- Los componentes RGB ya están multiplicados por alpha
- Ejemplo: `0x80800000` = rojo semi-transparente (A=128, R=128, G=0, B=0)
- Más eficiente para múltiples operaciones de blending
- Requerido por algunos formatos (Cairo, Direct2D)

**Diferencia clave**: Para el mismo color semi-transparente:
- Non-premult: `ARGB(0.5, 1.0, 0.0, 0.0)` → `0x80FF0000`
- Premult: `ARGB(0.5, 0.5, 0.0, 0.0)` → `0x80800000`

**Cuándo usar cada modo**:
- `G2D_PREMUL_NONE`: Datos de PNG, JPEG con alpha, buffers de aplicación estándar
- `G2D_PREMUL_ALPHA`: Renderizado Cairo, formatos de video con premult, pipelines de composición múltiple

**Nota**: El modo debe coincidir con el formato real de los datos. Usar el modo incorrecto producirá colores incorrectos en los bordes semi-transparentes.

### Modos Porter-Duff

El campo `bld_mode` permite seleccionar el modo de composición:

```c
enum g2d_bld_mode {
    G2D_BLD_CLEAR = 0,     /* Clear: 0 */
    G2D_BLD_COPY = 1,      /* Copy: Src */
    G2D_BLD_DST = 2,       /* Keep: Dst */
    G2D_BLD_SRCOVER = 3,   /* Source Over: Src + Dst×(1-As) [DEFAULT] */
    G2D_BLD_DSTOVER = 4,   /* Dst Over: Dst + Src×(1-Ad) */
    G2D_BLD_SRCIN = 5,     /* Src In: Src×Ad */
    G2D_BLD_DSTIN = 6,     /* Dst In: Dst×As */
    G2D_BLD_SRCOUT = 7,    /* Src Out: Src×(1-Ad) */
    G2D_BLD_DSTOUT = 8,    /* Dst Out: Dst×(1-As) */
    G2D_BLD_SRCATOP = 9,   /* Src Atop: Src×Ad + Dst×(1-As) */
    G2D_BLD_DSTATOP = 10,  /* Dst Atop: Dst×As + Src×(1-Ad) */
    G2D_BLD_XOR = 11,      /* XOR: Src×(1-Ad) + Dst×(1-As) */
};
```

### Operación de 2 Buffers (In-Place)

Mezcla SRC y DST escribiendo el resultado en DST:

```c
struct g2d_blit blit = {
    .src = {
        .width = 200,
        .height = 200,
        .format = G2D_FMT_ARGB8888,
        .dma_fd = foreground_fd,
        .crop_w = 200,
        .crop_h = 200,
        .alpha = 128,                 /* 50% transparencia */
        .alpha_mode = G2D_GLOBAL_ALPHA,
        .premul_mode = G2D_PREMUL_NONE,  /* Datos estándar ARGB */
    },
    .dst = {
        .width = 800,
        .height = 600,
        .format = G2D_FMT_ARGB8888,
        .dma_fd = background_fd,
        .alpha = 128,                 /* 50% opacidad del fondo */
        .alpha_mode = G2D_GLOBAL_ALPHA,
        .premul_mode = G2D_PREMUL_NONE,  /* Datos estándar ARGB */
    },
    .dst_x = 300,
    .dst_y = 200,
    .dst_w = 200,
    .dst_h = 200,
    
    .out.dma_fd = -1,                 /* In-place: resultado en DST */
    .bld_mode = G2D_BLD_SRCOVER,      /* Source Over (default) */
    .flags = 0,
};

ioctl(g2d_fd, G2D_IOC_BLIT, &blit);
```

**Resultado:** `background_fd` contiene la composición (se modifica)

### Operación de 3 Buffers (Preserva Destino)

**✨ NUEVA FUNCIONALIDAD:** Buffer destino preservado intacto

Mezcla SRC y DST escribiendo el resultado en OUT (buffer separado):

```c
struct g2d_blit blit = {
    .src = {
        .width = 200,
        .height = 200,
        .format = G2D_FMT_ARGB8888,
        .dma_fd = ball_fd,            /* Pelota con alpha */
        .crop_w = 200,
        .crop_h = 200,
        .alpha = 128,
        .alpha_mode = G2D_GLOBAL_ALPHA,
        .premul_mode = G2D_PREMUL_NONE,  /* Datos estándar */
    },
    .dst = {
        .width = 800,
        .height = 600,
        .format = G2D_FMT_ARGB8888,
        .dma_fd = background_fd,      /* Fondo - NO SE MODIFICA */
        .alpha = 128,
        .alpha_mode = G2D_GLOBAL_ALPHA,
        .premul_mode = G2D_PREMUL_NONE,  /* Datos estándar */
    },
    .dst_x = 300,
    .dst_y = 200,
    .dst_w = 200,
    .dst_h = 200,
    
    .out = {
        .dma_fd = framebuffer_fd,     /* Buffer de salida separado */
        .width = 800,
        .height = 600,
        .format = G2D_FMT_ARGB8888,
    },
    .bld_mode = G2D_BLD_SRCOVER,
    .flags = 0,
};

ioctl(g2d_fd, G2D_IOC_BLIT, &blit);
```

**Resultado:** 
- `background_fd` permanece **intacto** ✅
- `framebuffer_fd` contiene la composición ✅

**Casos de uso:**
- **Animación de sprites**: Reutilizar mismo fondo para cada frame
- **Picture-in-picture**: Preservar video de fondo mientras se superpone UI
- **Compositing multi-capa**: Componer varias capas sin destruir las originales
- **Double/triple buffering**: Alternar buffers de salida sin recargar el fondo

### Comportamiento del Alpha en Capas Hardware

**⚠️ IMPORTANTE:** Las capas V0 y UI2 usan convenciones de alpha diferentes:

#### V0 Layer (Source/Video) - Alpha INVERTIDO
```c
alpha = 255  →  Transparente (invisible)
alpha = 128  →  50% transparencia
alpha = 0    →  Opaco (sólido)
```

#### UI2 Layer (Destination/UI) - Alpha ESTÁNDAR
```c
alpha = 255  →  Opaco (bloquea source)
alpha = 128  →  50% transparencia (mezcla con source)
alpha = 0    →  Transparente (solo se ve source)
```

**Con GLOBAL_ALPHA mode:** El hardware maneja la conversión automáticamente
- Especificas `alpha=128` en ambos y funciona correctamente
- Para PIXEL_ALPHA mode con V0, los píxeles deben usar alpha invertido

### Ejemplo: Animación con 3 Buffers

```c
/* Setup inicial */
int background_fd = load_image("background.png");
int ball_fd = create_ball_sprite(110, 110);
int front_fb = create_framebuffer(800, 600);
int back_fb = create_framebuffer(800, 600);
bool use_front = true;

/* Loop de animación */
for (int frame = 0; frame < 1000; frame++) {
    int x = compute_ball_x(frame);
    int y = compute_ball_y(frame);
    int out_fd = use_front ? front_fb : back_fb;
    
    /* Componer: ball + background → output */
    struct g2d_blit blit = {
        .src.dma_fd = ball_fd,
        .src.alpha = 128,
        .src.alpha_mode = G2D_GLOBAL_ALPHA,
        
        .dst.dma_fd = background_fd,  /* ¡NO se modifica! */
        .dst.alpha = 128,
        .dst.alpha_mode = G2D_GLOBAL_ALPHA,
        
        .dst_x = x,
        .dst_y = y,
        .dst_w = 110,
        .dst_h = 110,
        
        .out.dma_fd = out_fd,         /* Alterna entre buffers */
        .out.width = 800,
        .out.height = 600,
        .out.format = G2D_FMT_ARGB8888,
        
        .bld_mode = G2D_BLD_SRCOVER,
    };
    
    ioctl(g2d_fd, G2D_IOC_BLIT, &blit);
    
    /* Mostrar resultado */
    display_buffer(out_fd);
    use_front = !use_front;
}
```

**Ventajas vs 2-buffer:**
- ✅ No recargar background cada frame (ahorra ~0.8ms)
- ✅ Background puede ser imagen estática grande
- ✅ Permite double/triple buffering sin copias CPU
- ✅ FPS: ~56 FPS vs ~30 FPS con 2-buffer

### Ejemplo: Probar Todos los Modos Porter-Duff

```c
const char *mode_names[] = {
    "CLEAR", "COPY", "DST", "SRCOVER", "DSTOVER",
    "SRCIN", "DSTIN", "SRCOUT", "DSTOUT",
    "SRCATOP", "DSTATOP", "XOR"
};

for (int mode = 0; mode < 12; mode++) {
    /* Colocar DST (cuadrado azul) */
    blit1.src.dma_fd = blue_square_fd;
    blit1.dst.dma_fd = screen_fd;
    blit1.bld_mode = G2D_BLD_COPY;
    ioctl(g2d_fd, G2D_IOC_BLIT, &blit1);
    
    /* Aplicar SRC (círculo rojo) con modo Porter-Duff */
    blit2.src.dma_fd = red_circle_fd;
    blit2.src.alpha = 128;
    blit2.src.alpha_mode = G2D_GLOBAL_ALPHA;
    
    blit2.dst.dma_fd = screen_fd;
    blit2.dst.alpha = 128;
    blit2.dst.alpha_mode = G2D_GLOBAL_ALPHA;
    
    blit2.bld_mode = mode;  /* Probar cada modo */
    ioctl(g2d_fd, G2D_IOC_BLIT, &blit2);
    
    printf("Mode %d: %s\n", mode, mode_names[mode]);
}
```

### Hardware Utilizado

**Blending Pipeline:**
- **V0 (Video layer)**: Lee buffer SOURCE (foreground)
- **UI2 (UI layer)**: Lee buffer DESTINATION (background)
- **BLD (Blender)**: Combina según modo Porter-Duff
- **WB (Writeback)**: Escribe a OUTPUT (dst o out según caso)

**Configuración BLD:**
```c
bld.bld_en_ctrl = 0x00000300;  /* Habilita PIPE0 (V0) y PIPE1 (UI2) */
bld.premulti_ctrl.p0_alpha_mode = 1;  /* UI2 premultiplicación */
bld.premulti_ctrl.p1_alpha_mode = 1;  /* V0 premultiplicación */
bld.bld_ctrl = sunxi_g2d_get_bld_mode(bld_mode);  /* Selecciona Porter-Duff */
```

**Valores BLD_CTL por modo:**

| Modo | BLD_CTL | Fórmula |
|------|---------|---------|
| CLEAR | 0x00000000 | 0 |
| COPY | 0x00010001 | Src |
| DST | 0x01000100 | Dst |
| **SRCOVER** | **0x03010301** | Src + Dst×(1-As) |
| DSTOVER | 0x01030103 | Dst + Src×(1-Ad) |
| SRCIN | 0x00020002 | Src×Ad |
| DSTIN | 0x02000200 | Dst×As |
| SRCOUT | 0x00030003 | Src×(1-Ad) |
| DSTOUT | 0x03000300 | Dst×(1-As) |
| SRCATOP | 0x03020302 | Src×Ad + Dst×(1-As) |
| DSTATOP | 0x02030203 | Dst×As + Src×(1-Ad) |
| XOR | 0x03030303 | Src×(1-Ad) + Dst×(1-As) |

### Casos de Uso Prácticos

**SRCOVER (default)** - Composición normal
```c
// Logo semitransparente sobre imagen
blit.bld_mode = G2D_BLD_SRCOVER;
blit.src.alpha = 180;  // Logo 70% opaco
```

**XOR** - Efectos de inversión
```c
// Efecto "revelar" entre dos imágenes
blit.bld_mode = G2D_BLD_XOR;
```

**SRCIN** - Máscaras
```c
// Mostrar SRC solo donde DST es opaco
blit.bld_mode = G2D_BLD_SRCIN;
```

**SRCATOP** - Picture-in-picture
```c
// Video sobre fondo, recortado por forma del fondo
blit.bld_mode = G2D_BLD_SRCATOP;
```

### Chromakey (Color Keying)

**✨ NUEVA FUNCIONALIDAD:** Transparencia basada en color (green screen/blue screen)

El chromakey permite hacer transparente un rango de colores RGB de la imagen fuente, ideal para efectos de pantalla verde/azul en video.

#### Configuración

```c
struct g2d_blit blit = {
    .src = { /* Video con fondo verde */ },
    .dst = { /* Imagen de fondo */ },
    
    /* Chromakey fields */
    .color_key_enable = 1,              /* Activar chromakey */
    .color_key_mode = 0,                /* 0=inside transparent, 1=outside transparent */
    .color_key_min = 0x00C000,          /* Verde oscuro (RGB mínimo) */
    .color_key_max = 0x00FF80,          /* Verde claro (RGB máximo) */
    
    .bld_mode = G2D_BLD_SRCOVER,
};

ioctl(g2d_fd, G2D_IOC_BLIT, &blit);
```

#### Formato de Color

Los valores `color_key_min` y `color_key_max` usan formato **0xRRGGBB**:

```c
/* Ejemplos de rangos de color */

// Green screen (pantalla verde)
.color_key_min = 0x00C000,  // R=0x00, G=0xC0, B=0x00
.color_key_max = 0x00FF80,  // R=0x00, G=0xFF, B=0x80

// Blue screen (pantalla azul)
.color_key_min = 0x0000C0,  // R=0x00, G=0x00, B=0xC0
.color_key_max = 0x0080FF,  // R=0x00, G=0x80, B=0xFF

// Magenta screen
.color_key_min = 0xC000C0,  // R=0xC0, G=0x00, B=0xC0
.color_key_max = 0xFF00FF,  // R=0xFF, G=0x00, B=0xFF
```

#### Modos de Operación

**Modo 0 - Inside Transparent** (típico green screen):
```c
.color_key_mode = 0;
/* Si color está EN el rango [min, max] → TRANSPARENTE
   Si color está FUERA del rango → OPACO */
```

**Modo 1 - Outside Transparent** (keying inverso):
```c
.color_key_mode = 1;
/* Si color está FUERA del rango → TRANSPARENTE
   Si color está EN el rango [min, max] → OPACO */
```

#### Hardware

El chromakey opera en el módulo **BLD (Blender)** sobre la capa **V0 (source)**:

```c
/* Registros BLD configurados por el driver */
BLD_KEY_CTL:  key0_en=1, key0_match_dir=mode
BLD_KEY_CON:  key0b_match=1, key0g_match=1, key0y_match=1  /* Match RGB */
BLD_KEY_MIN:  min_r, min_g, min_b  /* Extraídos de color_key_min */
BLD_KEY_MAX:  max_r, max_g, max_b  /* Extraídos de color_key_max */
```

- **Matching**: Compara cada canal RGB por separado (8 bits/canal)
- **Range check**: `(R >= min_r && R <= max_r) && (G >= min_g && G <= max_g) && (B >= min_b && B <= max_b)`
- **Integración**: Se aplica **ANTES** del Porter-Duff blending

#### Ejemplo Completo: Green Screen

```c
#include <sys/ioctl.h>
#include <fcntl.h>
#include <linux/sunxi_g2d.h>
#include <linux/dma-heap.h>

int main() {
    int g2d_fd = open("/dev/sunxi_g2d", O_RDWR);
    int video_fd = load_video_frame("greenscreen_video.yuv");
    int background_fd = load_image("beach.jpg");
    int output_fd = create_framebuffer(1920, 1080, G2D_FMT_ARGB8888);
    
    struct g2d_blit blit = {
        .src = {
            .width = 1920,
            .height = 1080,
            .format = G2D_FMT_NV12,         /* YUV 4:2:0 video */
            .dma_fd = video_fd,
            .crop_w = 1920,
            .crop_h = 1080,
        },
        .dst = {
            .width = 1920,
            .height = 1080,
            .format = G2D_FMT_RGB888,       /* Background JPEG */
            .dma_fd = background_fd,
        },
        .dst_x = 0,
        .dst_y = 0,
        .dst_w = 1920,
        .dst_h = 1080,
        
        .out = {
            .width = 1920,
            .height = 1080,
            .format = G2D_FMT_ARGB8888,
            .dma_fd = output_fd,
        },
        
        /* Chromakey: Verde → Transparente */
        .color_key_enable = 1,
        .color_key_mode = 0,                /* Inside = transparent */
        .color_key_min = 0x00B000,          /* Verde oscuro */
        .color_key_max = 0x40FF60,          /* Verde claro + tolerancia */
        
        .bld_mode = G2D_BLD_SRCOVER,        /* Source over background */
        .flags = 0,
    };
    
    /* Procesar frame con chromakey */
    if (ioctl(g2d_fd, G2D_IOC_BLIT, &blit) < 0) {
        perror("G2D_IOC_BLIT");
        return -1;
    }
    
    /* output_fd contiene: video sin fondo verde + background */
    display_buffer(output_fd);
    
    close(g2d_fd);
    return 0;
}
```

#### Resultado

**Antes:**
```
[ Video con actor delante de pantalla verde ]
```

**Después:**
```
[ Actor compuesto sobre imagen de playa ]
```

**Performance:** ~1.5ms para 1080p NV12→RGB chromakey + blend (vs ~35ms en CPU)

#### Consejos para Green Screen

1. **Iluminación uniforme**: Evita sombras en la pantalla verde
2. **Rango amplio**: Incluye tolerancia para variaciones de color
   ```c
   .color_key_min = 0x00A000;  // Más oscuro que el verde puro
   .color_key_max = 0x50FF70;  // Incluye variaciones de tono
   ```

3. **Evitar spill**: Verde reflejado en el sujeto → Ajustar rango
4. **YUV source**: Mejor calidad con formatos YUV (NV12, NV21) que RGB
5. **Porter-Duff**: Usar **SRCOVER** para composición natural

#### Casos de Uso

- **Video conferencing**: Cambio de fondo en tiempo real
- **Live streaming**: Efectos de pantalla verde para streamers
- **Video editing**: Composición de múltiples capas de video
- **AR/VR**: Integración de video real con fondos virtuales
- **Broadcasting**: Efectos de clima/noticias con chromakey

---

## Consideraciones de Performance

### ✅ Buenas Prácticas

1. **Usar CMA heap** (`default_cma_region`) para buffers grandes
2. **Reutilizar buffers** en lugar de asignar/liberar constantemente
3. **Alinear stride** a múltiplos de 64 bytes si es posible
4. **Evitar `memcpy`** desde/hacia buffers G2D (usar DMA-BUF compartido)

### ❌ Anti-patrones

1. ❌ Asignar buffer del sistema y copiar con `memcpy` → Usar DMA-BUF
2. ❌ Polling manual del estado → El driver usa IRQ (automático)
3. ❌ Buffers no alineados → Puede reducir performance
4. ❌ Operaciones de <64x64 píxeles → Mejor hacerlo en CPU

### Benchmarks (T113-S3 @1.0 GHz)

| Operación | Tamaño | G2D | CPU equiv. | Speedup |
|-----------|--------|-----|------------|---------|
| Fillrect | 1920x1080 ARGB | ~0.5 ms | ~2.5 ms (memset) | 5x |
| Fillrect | 800x600 ARGB | ~0.3 ms | ~1.0 ms (memset) | 3x |
| Blit 1:1 | 1920x1080 ARGB | ~0.8 ms | ~3.5 ms (memcpy) | 4x |
| Blit upscale 2x | 640x480→1280x960 | ~1.2 ms | ~15 ms (bilinear SW) | 12x |
| Blit downscale 4x | 1920x1080→480x270 | ~0.6 ms | ~8 ms (bilinear SW) | 13x |
| Rotation 90° | 1920x1080 ARGB | ~1.0 ms | ~12 ms (CPU) | 12x |
| Alpha blend | 128x128 ARGB | ~0.15 ms | ~0.5 ms (CPU) | 3x |

**Conclusión:** G2D es más eficiente para buffers >500 KB. Escalado y rotación tienen mayor speedup (10-15x).

---

## Integración con Otros Subsistemas

### DRM/KMS (Display)

El G2D puede compartir buffers con el display controller:

```c
/* 1. Asignar buffer desde DRM */
struct drm_mode_create_dumb create = {
    .width = 1920,
    .height = 1080,
    .bpp = 32,
};
ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);

/* 2. Exportar como DMA-BUF */
struct drm_prime_handle prime = {
    .handle = create.handle,
    .flags = DRM_CLOEXEC | DRM_RDWR,
};
ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);

/* 3. Usar en G2D */
struct g2d_fillrect fill = {
    .dst.dma_fd = prime.fd,  // fd exportado desde DRM
    /* ... */
};
ioctl(g2d_fd, G2D_IOC_FILLRECT, &fill);

/* 4. Mostrar en pantalla */
drmModeSetCrtc(drm_fd, ...);
```

### V4L2 (Cámara/Video)

```c
/* Exportar buffer V4L2 como DMA-BUF */
struct v4l2_exportbuffer expbuf = {
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .index = 0,
    .flags = O_RDWR,
};
ioctl(v4l2_fd, VIDIOC_EXPBUF, &expbuf);

/* Usar como origen en blit (futuro) */
struct g2d_blit blit = {
    .src.dma_fd = expbuf.fd,
    /* ... */
};
```

---

## Troubleshooting

### Error: "No such device" al abrir /dev/g2d

**Causa:** El módulo no está cargado.

```bash
# Verificar
lsmod | grep sunxi_g2d

# Cargar manualmente
sudo modprobe sunxi-g2d

# O insertar directamente
sudo insmod sunxi-g2d.ko
```

### Error: "Invalid argument" en ioctl

**Causas posibles:**
1. `dma_fd` inválido o cerrado
2. Formato de píxel no soportado
3. Dimensiones fuera de rango (min: 8x8, max: 2048x2048)
4. `stride` menor que `width * bpp`

**Debug:**
```bash
# Ver logs del driver
dmesg | grep -i g2d | tail -20
```

### Error: "Cannot allocate memory" en dma_heap

**Causa:** CMA agotada o no configurada.

```bash
# Ver CMA disponible
cat /proc/meminfo | grep Cma

# Aumentar CMA en kernel cmdline
cma=128M
```

### Performance pobre

1. Verificar que NO estés usando `system` heap:
   ```c
   // ❌ Malo
   open("/dev/dma_heap/system", ...)
   
   // ✅ Bueno
   open("/dev/dma_heap/default_cma_region", ...)
   ```

2. Verificar clocks del G2D:
   ```bash
   cat /sys/kernel/debug/clk/g2d/clk_rate
   # Debe ser ~300 MHz
   ```

3. Verificar que IRQ esté funcionando:
   ```bash
   cat /proc/interrupts | grep g2d
   # El contador debe incrementar con cada operación
   ```

---

## Roadmap

### Implementado ✅

- [x] Fillrect con IRQ
- [x] BLIT (copia 1:1 y con crop)
- [x] **BLIT con escalado** - VSU configurado con filtros bicúbicos (hasta 8x)
- [x] **Rotación** (90°, 180°, 270°) - ROT module completo
- [x] **Flip** horizontal/vertical - Combinable con rotación
- [x] **Alpha blending** - 12 modos Porter-Duff (SRCOVER, COPY, XOR, etc.)
- [x] **3-buffer compositing** - Buffer destino preservado (src + dst → out)
- [x] **Pipeline unificado** - BLIT detecta automáticamente qué módulos activar
- [x] **Alpha modes** - GLOBAL_ALPHA, PIXEL_ALPHA, MIXER_ALPHA
- [x] DMA-BUF import
- [x] Gestión de poder automática
- [x] **Formatos RGB completos** - Todos los formatos RGB (32/24/16-bit, variantes ARGB/ABGR/RGBA/BGRA)
- [x] **Formatos YUV/Video** - NV12, NV21, I420, YUYV, UYVY, etc. con conversión YUV→RGB acelerada
- [x] **Chromakey (Color Keying)** - Transparencia por color con rango configurable (pantalla verde/azul)
- [x] UAPI estable

### En Desarrollo 🚧

- [ ] **Sync fences** - Para sincronización con DRM/Wayland (fence_fd_out en UAPI)
- [ ] **Demos YUV** - Ejemplos de procesamiento de video NV12→RGB
- [ ] **Demo Chromakey** - Ejemplo de pantalla verde con video

### Futuro 📋

- [ ] Operaciones asíncronas con job queue
- [ ] Premultiplicación de alpha (ya en hardware, falta exposición en UAPI)
- [ ] Buffer allocation desde driver (simplificar API, opcional)
- [ ] Soporte BT.709 color space (actualmente solo BT.601)

---

## Demos Visuales

El repositorio incluye demos interactivos para probar todas las funcionalidades:

### Demos Básicos
- `demo-bouncing-ball` - Animación de pelota con alpha blending y escalado
  - Usa 3-buffer compositing (56 FPS)
  - Demuestra GLOBAL_ALPHA y V0 alpha invertido
  - Pattern con gradiente radial
  
- `demo-3buffer-test` - Verifica operación de 3 buffers
  - SRC (rojo) + DST (azul) → OUT (morado)
  - Confirma que DST no se modifica
  - Prueba GLOBAL_ALPHA mode

- `demo-porter-duff` - Grid visual de los 12 modos Porter-Duff
  - 4×3 celdas, cada una con un modo diferente
  - SRC: círculo rojo con gradiente
  - DST: cuadrado azul semi-transparente
  - Permite comparar visualmente todos los modos

### Demos de Rotación/Escalado
- `demo-rotate-test` - Prueba rotación 90°/180°/270° y flip H/V
- `demo-scale-rotate` - Combina escalado con rotación (2-pass si es necesario)

### Tests Unitarios
- `test-fillrect.c` - Relleno de rectángulos
- `test-blit-simple.c` - Copia básica 1:1
- `test-blit-scale.c` - Escalado con VSU
- `test-rotation.c` - Todas las rotaciones
- `test-alpha.c` - Alpha blending (legacy, usar demo-porter-duff)

## Licencia

GPL-2.0-or-later

## Autor

Sergio Perez  
Copyright (C) 2025

## Referencias

- [DMA-BUF Documentation](https://www.kernel.org/doc/html/latest/driver-api/dma-buf.html)
- [DMA-BUF Heaps](https://lwn.net/Articles/780691/)
- [Porter-Duff Compositing](https://en.wikipedia.org/wiki/Alpha_compositing)
- Allwinner G2D IP Manual v1.1.0
