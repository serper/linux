# Allwinner G2D Driver - Guía para Desarrolladores

## Descripción General

El driver `sunxi-g2d` proporciona acceso hardware al acelerador gráfico 2D de Allwinner (G2D) en SoCs como el T113-S3. Este acelerador es capaz de realizar operaciones de:

- **Relleno de rectángulos** (fillrect) - Relleno sólido de regiones ✅
- **Copia de imágenes** (blit) - Copia 1:1 y con crop ✅
- **Escalado de imágenes** - TODO: Requiere VSU (Video Scaler Unit)
- **Rotación y transformaciones** - TODO: Futuro
- **Alpha blending** - TODO: Futuro

### Características Implementadas

✅ **Operaciones sincrónicas con IRQ** (no busy-wait)  
✅ **Soporte DMA-BUF** para zero-copy con otros subsistemas  
✅ **FILLRECT**: Relleno de rectángulos con color sólido  
✅ **BLIT**: Copia de imágenes (1:1 y con crop de origen)  
✅ **Gestión automática de poder** (clocks, reset, MBUS)  
✅ **Múltiples formatos de píxeles** (ARGB8888, XRGB8888, RGB565, etc.)  
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

#### `G2D_IOC_BLIT` - Copiar imagen

Copia una región de una imagen origen a un buffer destino. Soporta crop (recorte de origen) pero no escalado por el momento.

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
    .dst_w = 320,               // Debe coincidir con crop_w (sin escalado)
    .dst_h = 240,               // Debe coincidir con crop_h (sin escalado)
    .flags = 0,                 // Rotación/flip/alpha no soportados aún
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
- ✅ **Crop de origen**: Especificar `crop_x`, `crop_y`, `crop_w`, `crop_h`
- ✅ **Múltiples copias**: Llamar ioctl varias veces consecutivas
- ❌ **Escalado**: Requiere VSU (no implementado) - si `dst_w != crop_w` dará timeout
- ❌ **Rotación/flip**: No implementado (flags serán rechazados con -ENOSYS)
- ❌ **Alpha blending**: No implementado

**Casos de uso:**
- Copiar imágenes completas entre buffers
- Copiar regiones (tiles) de una imagen grande
- Componer UI juntando sprites/iconos en un framebuffer
- Preparar texturas para renderizado

**Ejemplo: Copiar región de 200x150 del centro de src a (50,50) en dst**
```c
blit.src.width = 640;
blit.src.height = 480;
blit.src.crop_x = 220;   // Centro: (640-200)/2
blit.src.crop_y = 165;   // Centro: (480-150)/2
blit.src.crop_w = 200;
blit.src.crop_h = 150;

blit.dst_x = 50;
blit.dst_y = 50;
blit.dst_w = 200;  // Mismo tamaño (sin escalado)
blit.dst_h = 150;
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

| Formato | Enum | Descripción | Bytes/píxel |
|---------|------|-------------|-------------|
| ARGB8888 | `G2D_FMT_ARGB8888` | 8 bits alpha, RGB | 4 |
| XRGB8888 | `G2D_FMT_XRGB8888` | Sin alpha (X=ignorado) | 4 |
| RGBA8888 | `G2D_FMT_RGBA8888` | RGB + 8 bits alpha | 4 |
| RGB565 | `G2D_FMT_RGB565` | 5-6-5 bits RGB | 2 |
| RGB888 | `G2D_FMT_RGB888` | 8 bits RGB (packed) | 3 |
| ARGB1555 | `G2D_FMT_ARGB1555` | 1 bit alpha, 5-5-5 RGB | 2 |

**Nota sobre YUV:** Los formatos YUV están definidos en el UAPI pero no están probados aún.

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

| Operación | Tamaño | G2D | CPU (memset) | Speedup |
|-----------|--------|-----|--------------|---------|
| Fillrect | 1920x1080 ARGB | ~0.5 ms | ~2.5 ms | 5x |
| Fillrect | 800x600 ARGB | ~0.3 ms | ~1.0 ms | 3x |
| Fillrect | 320x240 ARGB | ~0.1 ms | ~0.15 ms | 1.5x |

**Conclusión:** G2D es más eficiente para buffers >500 KB.

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
- [x] DMA-BUF import
- [x] Gestión de poder automática
- [x] Múltiples formatos de píxeles (ARGB8888, XRGB8888, RGB565)
- [x] UAPI estable

### En Desarrollo 🚧

- [ ] **BLIT con escalado** - Requiere configurar VSU (Video Scaler Unit)
- [ ] **Sync fences** - Para DRM/Wayland
- [ ] **Buffer allocation desde driver** - Simplificar API (DMA-BUF export mmap pendiente)

### Futuro 📋

- [ ] Rotación (90°, 180°, 270°) - Hardware soportado, falta implementación
- [ ] Flip horizontal/vertical
- [ ] Alpha blending avanzado (porter-duff)
- [ ] Color space conversion (RGB ↔ YUV)
- [ ] Operaciones asíncronas con job queue
- [ ] Soporte para Porter-Duff compositing completo

---

## Ejemplos Completos

Ver el código de prueba en el repositorio:
- `test-dmaheap-fillrect.c` - Fillrect con 2 tests (rojo y verde)
- `test-blit-simple.c` - BLIT con 3 tests (copia completa, crop, múltiples posiciones)
- `test-blit.c` - BLIT con escalado (requiere VSU - no funcional aún)

## Licencia

GPL-2.0-or-later

## Autor

Sergio Perez  
Copyright (C) 2025

## Referencias

- [DMA-BUF Documentation](https://www.kernel.org/doc/html/latest/driver-api/dma-buf.html)
- [DMA-BUF Heaps](https://lwn.net/Articles/780691/)
- Allwinner G2D IP Manual v1.1.0
