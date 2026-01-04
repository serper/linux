# Manual del Desarrollador - Driver `sunxi-g2d`

Guía rápida para trabajar con la UAPI expuesta en `/dev/g2d`. La UAPI se define en `include/uapi/linux/sunxi_g2d.h` y la implementación de referencia está en `drivers/gpu/sunxi-g2d/sunxi-g2d-main.c`. El README complementario vive en `drivers/gpu/sunxi-g2d/README.md`.

## IOCTLs principales
- `G2D_IOC_GET_VERSION` (`struct g2d_version`): devuelve versión HW y driver.
- `G2D_IOC_ALLOC_BUFFER` (`struct g2d_alloc_buffer`): crea un DMA-BUF y retorna `dma_fd`. Flags: `G2D_ALLOC_F_CONTIGUOUS` (IOVA contigua) y/o `G2D_ALLOC_F_COHERENT` (backend `dma_alloc_coherent`).
- `G2D_IOC_CMD` (`struct g2d_cmd`): API unificada. `cmd_type` ∈ {`G2D_CMD_COPY`, `G2D_CMD_SCALE`, `G2D_CMD_BLEND`, `G2D_CMD_ROTATE`, `G2D_CMD_MASK`, `G2D_CMD_FILLRECT`}. Campos clave:
  - `src`, `dst`, `out` (`struct g2d_buf`): ancho/alto, formato (`enum g2d_pixel_format`), `stride[3]`, crop opcional, `dma_fd`, alpha/premul/color_space.
  - `dst_x`, `dst_y`, `dst_w`, `dst_h`: posición y tamaño destino (activa escalado si difiere del crop).
  - `flags`: rotaciones 90/180/270 y flips; no combinable con escalado.
  - `fence_fd_in`/`fence_fd_out`: sincronización explícita; el ioctl retorna inmediatamente.
  - Restricciones: COPY exige `crop == dst_w/h`; BLEND en 3 buffers requiere `out.dma_fd >= 0`; no se permite escala + rotación en un único comando.
- `G2D_IOC_TASK` (`struct g2d_task_req`): agrupa varios `g2d_cmd` en kernel.
  - `G2D_TASK_CREATE` → `task_id`.
  - `G2D_TASK_ADD` → añade step (usa `struct g2d_cmd`).
  - `G2D_TASK_RUN` → ejecuta y devuelve un único `fence_fd_out`.
  - `G2D_TASK_DEL` → destruye la tarea.
- `G2D_IOC_SET_CSC_ADJUST` / `G2D_IOC_GET_CSC_ADJUST`: brillo/contraste/saturación y modo CSC (601/709/2020).
- `G2D_IOC_SELFTEST_FENCE`: crea un fence que se señaliza tras `timeout_ms` (diagnóstico de sincronización).
- `G2D_IOC_SYNC`: espera bloqueante a un fence fd.
- `G2D_IOC_WRITE_BUFFER` / `G2D_IOC_READ_BUFFER`: copian entre userspace y un DMA-BUF (offset/size). Útiles para pruebas, no para el datapath crítico.

## Comando G2D_CMD_MASK (Operaciones ROP4)

El comando `G2D_CMD_MASK` implementa operaciones raster con máscara (ROP4), combinando hasta **4 buffers** (destino, fuente, patrón y máscara) mediante códigos ROP3 que se aplican selectivamente según el valor de la máscara.

### Conceptos Básicos
- **ROP4** = Operación raster de 4 operandos: Dst, Src, Pattern, Mask
- La **máscara** (mask) controla qué operación ROP3 se aplica a cada píxel:
  - Si `mask[x,y] == 1`: se aplica `rop_code_0` → combina Dst, Src, Pattern
  - Si `mask[x,y] == 0`: se aplica `rop_code_1` → combina Dst, Src, Pattern
- Los **códigos ROP3** son valores de 8 bits que definen operaciones lógicas entre 3 operandos (ver tabla ROP3 estándar de Windows GDI).

### Estructura de Buffers

```c
struct g2d_cmd mask_cmd = {
    .cmd_type = G2D_CMD_MASK,
    
    // Buffer DESTINO (V0) - Lectura del estado inicial
    .dst = {
        .dma_fd = dst_fd,
        .width = 64, .height = 64,
        .format = G2D_FMT_ARGB8888,
        .stride = { 256 },
        // crop opcional
    },
    
    // Buffer FUENTE (UI0) - Source
    .src = {
        .dma_fd = src_fd,
        .width = 64, .height = 64,
        .format = G2D_FMT_ARGB8888,
        .stride = { 256 },
    },
    
    // Buffer SALIDA (WB) - Resultado final
    .out = {
        .dma_fd = out_fd,
        .width = 64, .height = 64,
        .format = G2D_FMT_ARGB8888,
        .stride = { 256 },
    },
    
    // Parámetros ROP4
    .params.mask = {
        .mask_fd = mask_fd,        // DMA-BUF de máscara (formato A8)
        .mask_crop_x = 0,
        .mask_crop_y = 0,
        .mask_crop_w = 64,
        .mask_crop_h = 64,
        .mask_pitch = 64,          // Stride de máscara (bytes)
        .mask_alpha = 255,         // Alpha global de máscara (0-255)
        .mask_alpha_mode = 0,      // 0=global, otros reservados
        
        .ptn_fd = ptn_fd,          // OPCIONAL: DMA-BUF de patrón
        .ptn_crop_x = 0,
        .ptn_crop_y = 0,
        .ptn_crop_w = 64,
        .ptn_crop_h = 64,
        .ptn_pitch = 256,
        .ptn_format = G2D_FMT_ARGB8888,
        
        .rop_code_0 = 0xCC,        // ROP3 cuando mask==1 (ej: SRCCOPY)
        .rop_code_1 = 0xAA,        // ROP3 cuando mask==0 (ej: NOP/Dst)
    },
    
    .dst_w = 64, .dst_h = 64,
    .fence_fd_in = -1,
    .fence_fd_out = -1,
};
```

### Buffers Requeridos vs Opcionales

| Buffer | Campo | Obligatorio | Notas |
|--------|-------|-------------|-------|
| **Destino** | `dst.dma_fd` | **SÍ** | Lee estado inicial (V0 layer) |
| **Fuente** | `src.dma_fd` | **SÍ** | Source para ROP (UI0 layer) |
| **Salida** | `out.dma_fd` | **SÍ** | Resultado final (WB writeback) |
| **Máscara** | `params.mask.mask_fd` | **SÍ** | Control de selección ROP - Usa solo canal alpha (formato ARGB8888 en el HW) |
| **Patrón** | `params.mask.ptn_fd` | **OPCIONAL** | Depende del código ROP3 (UI1 layer) |

#### ¿Cuándo es necesario el patrón?

El buffer de patrón (`ptn_fd`) solo es necesario si los códigos ROP3 utilizados **referencian el operando P (Pattern)**:

- **NO necesita patrón** (`ptn_fd = -1`): Códigos ROP3 que solo usan D (Dst) y S (Src)
  - `0xCC` = `SRCCOPY` (S)
  - `0xAA` = `NOP` (D)
  - `0xEE` = `SRCPAINT` (D | S)
  - `0x88` = `SRCAND` (D & S)
  - `0x66` = `SRCINVERT` (D ^ S)
  - `0x00` = `BLACKNESS` (0)
  - `0xFF` = `WHITENESS` (1)

- **SÍ necesita patrón** (`ptn_fd >= 0`): Códigos que incluyen P
  - `0xF0` = `PATCOPY` (P)
  - `0xFB` = `PATPAINT` (D | (P | ~S))
  - `0xA0` = `PATAND` (D & P)
  - `0x5A` = `PATINVERT` (D ^ P)
  - Y cualquier combinación que use P en la tabla de verdad

**Si `ptn_fd = -1`**: El driver construye automáticamente un buffer patrón dummy (UI1), pero los bits P en el ROP3 leerán valores indefinidos. Por ello, **solo usa `ptn_fd = -1` si sabes que tus códigos ROP3 no usan el operando P**.

### Códigos ROP3 Comunes

```c
// Códigos que NO necesitan Pattern (ptn_fd = -1 es seguro)
#define ROP3_SRCCOPY    0xCC  // Result = Src
#define ROP3_DSTCOPY    0xAA  // Result = Dst (NOP)
#define ROP3_SRCPAINT   0xEE  // Result = Dst | Src
#define ROP3_SRCAND     0x88  // Result = Dst & Src
#define ROP3_SRCINVERT  0x66  // Result = Dst ^ Src
#define ROP3_NOTSRC     0x33  // Result = ~Src
#define ROP3_BLACKNESS  0x00  // Result = 0 (negro)
#define ROP3_WHITENESS  0xFF  // Result = 1 (blanco)

// Códigos que SÍ necesitan Pattern (ptn_fd >= 0 obligatorio)
#define ROP3_PATCOPY    0xF0  // Result = Pattern
#define ROP3_PATPAINT   0xFB  // Result = Dst | (Pattern | ~Src)
#define ROP3_PATAND     0xA0  // Result = Dst & Pattern
#define ROP3_PATINVERT  0x5A  // Result = Dst ^ Pattern
```

### Restricciones Importantes

1. **Formato de Máscara**: Debe usar `G2D_FMT_ARGB8888` (aunque solo se usa el canal alpha)
   - El hardware G2D no soporta formato A8 puro, por ello se usa ARGB8888
   - Solo el canal alpha (bits 24-31) es utilizado; los canales RGB se ignoran
   - Valor alpha 0x00 = máscara inactiva → usa `rop_code_1`
   - Valor alpha 0xFF = máscara activa → usa `rop_code_0`
   - Valores intermedios (0x01-0xFE) se tratarán como 0xFF (activos)

2. **Alpha de Máscara**: `mask_alpha` debe ser **255** para operaciones ROP4 correctas
   - Otros valores pueden causar resultados incorrectos en el hardware

3. **Dimensiones**: Todos los buffers deben tener las mismas dimensiones de crop
   - `src_crop == dst_crop == mask_crop == ptn_crop` (si ptn está presente)

4. **Formatos**: Dst, Src, Pattern y Out deben usar formatos RGB compatibles
   - Recomendado: `G2D_FMT_ARGB8888` para todos (excepto máscara que es A8)
   - No mezclar RGB con YUV en ROP4

5. **Limitación Hardware - Primer Píxel**: 
   - El píxel en posición [0,0] siempre se escribe como `0x00000000` (negro transparente)
   - Esta es una limitación conocida del hardware G2D v0x0110
   - Representa solo el 0.02% de los píxeles en operaciones típicas (1/4096)
   - Ver `ROP4-FIRST-PIXEL-ANALYSIS.md` para detalles completos

6. **Limitación Hardware - Formato de Máscara**:
   - El hardware G2D no soporta formato A8 puro de 8 bits
   - Por lo tanto, la máscara debe usar `G2D_FMT_ARGB8888` (32 bits por píxel)
   - Esto consume 4x más memoria que lo ideal (16KB en lugar de 4KB para 64x64)
   - Solo el canal alpha es interpretado por el hardware; los canales RGB se ignoran
   - Asegúrate de llenar correctamente el canal alpha:
     - Para píxeles activos (máscara ON): `alpha = 0xFF` (cualquier valor >= 0x80 es activo)
     - Para píxeles inactivos (máscara OFF): `alpha = 0x00` (cualquier valor < 0x80 es inactivo)
   - Usa `0xFF000000` (negro opaco) para máscara activa y `0x00000000` (transparente) para inactiva

7. **Sin Escalado**: ROP4 no soporta escalado, `dst_w` y `dst_h` deben coincidir con el crop

### Ejemplo Completo: Máscara con Pattern

```c
#include <fcntl.h>
#include <linux/sunxi_g2d.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>

int main(void) {
    int g2d = open("/dev/g2d", O_RDWR);
    
    // Crear buffers (64x64 ARGB8888 = 256 bytes/línea = 16KB total)
    struct g2d_alloc_buffer dst_buf = { .size = 16384 };
    struct g2d_alloc_buffer src_buf = { .size = 16384 };
    struct g2d_alloc_buffer out_buf = { .size = 16384 };
    struct g2d_alloc_buffer ptn_buf = { .size = 16384 };
    struct g2d_alloc_buffer mask_buf = { .size = 16384 }; // 64x64 ARGB8888 = 16KB
    
    ioctl(g2d, G2D_IOC_ALLOC_BUFFER, &dst_buf);
    ioctl(g2d, G2D_IOC_ALLOC_BUFFER, &src_buf);
    ioctl(g2d, G2D_IOC_ALLOC_BUFFER, &out_buf);
    ioctl(g2d, G2D_IOC_ALLOC_BUFFER, &ptn_buf);
    ioctl(g2d, G2D_IOC_ALLOC_BUFFER, &mask_buf);
    
    // Inicializar buffers con colores
    // Dst = Azul, Src = Rojo, Pattern = Verde
    uint32_t blue_pixel = 0xFF0000FF;
    uint32_t red_pixel = 0xFFFF0000;
    uint32_t green_pixel = 0xFF00FF00;
    
    for (int i = 0; i < 64*64; i++) {
        struct g2d_buffer_rw dst_wr = {
            .dma_fd = dst_buf.dma_fd,
            .offset = i * 4,
            .size = 4,
            .user_addr = (unsigned long)&blue_pixel,
        };
        ioctl(g2d, G2D_IOC_WRITE_BUFFER, &dst_wr);
        
        struct g2d_buffer_rw src_wr = {
            .dma_fd = src_buf.dma_fd,
            .offset = i * 4,
            .size = 4,
            .user_addr = (unsigned long)&red_pixel,
        };
        ioctl(g2d, G2D_IOC_WRITE_BUFFER, &src_wr);
        
        struct g2d_buffer_rw ptn_wr = {
            .dma_fd = ptn_buf.dma_fd,
            .offset = i * 4,
            .size = 4,
            .user_addr = (unsigned long)&green_pixel,
        };
        ioctl(g2d, G2D_IOC_WRITE_BUFFER, &ptn_wr);
    }
    
    // Crear máscara: mitad superior activa (alpha=0xFF), mitad inferior inactiva (alpha=0x00)
    // Nota: se usa ARGB8888 aunque solo importa el canal alpha
    for (int y = 0; y < 64; y++) {
        uint32_t mask_value = (y < 32) ? 0xFF000000 : 0x00000000; // Alpha en bits 24-31
        for (int x = 0; x < 64; x++) {
            struct g2d_buffer_rw mask_wr = {
                .dma_fd = mask_buf.dma_fd,
                .offset = (y * 64 + x) * 4,  // ARGB8888 = 4 bytes/píxel
                .size = 4,
                .user_addr = (unsigned long)&mask_value,
            };
            ioctl(g2d, G2D_IOC_WRITE_BUFFER, &mask_wr);
        }
    }
    
    // Ejecutar ROP4:
    // - Máscara activa (arriba): rop_code_0 = 0xF0 (PATCOPY) → Verde
    // - Máscara inactiva (abajo): rop_code_1 = 0xCC (SRCCOPY) → Rojo
    struct g2d_cmd mask_cmd = {
        .cmd_type = G2D_CMD_MASK,
        .dst = {
            .dma_fd = dst_buf.dma_fd,
            .width = 64, .height = 64,
            .format = G2D_FMT_ARGB8888,
            .stride = { 256 },
        },
        .src = {
            .dma_fd = src_buf.dma_fd,
            .width = 64, .height = 64,
            .format = G2D_FMT_ARGB8888,
            .stride = { 256 },
        },
        .out = {
            .dma_fd = out_buf.dma_fd,
            .width = 64, .height = 64,
            .format = G2D_FMT_ARGB8888,
            .stride = { 256 },
        },
        .params.mask = {
            .mask_fd = mask_buf.dma_fd,
            .mask_pitch = 64,
            .mask_crop_w = 64,
            .mask_crop_h = 64,
            .mask_alpha = 255,
            .mask_alpha_mode = 0,
            
            .ptn_fd = ptn_buf.dma_fd,  // Pattern necesario para PATCOPY
            .ptn_pitch = 256,
            .ptn_crop_w = 64,
            .ptn_crop_h = 64,
            .ptn_format = G2D_FMT_ARGB8888,
            
            .rop_code_0 = 0xF0,  // PATCOPY: donde mask==1
            .rop_code_1 = 0xCC,  // SRCCOPY: donde mask==0
        },
        .dst_w = 64, .dst_h = 64,
        .fence_fd_in = -1,
        .fence_fd_out = -1,
    };
    
    ioctl(g2d, G2D_IOC_CMD, &mask_cmd);
    
    // Esperar a que termine
    struct pollfd pfd = { .fd = mask_cmd.fence_fd_out, .events = POLLIN };
    poll(&pfd, 1, -1);
    close(mask_cmd.fence_fd_out);
    
    // Resultado esperado en out_buf:
    // - Píxeles Y=0..31: Verde (0xFF00FF00) - PATCOPY aplicado
    // - Píxeles Y=32..63: Rojo (0xFFFF0000) - SRCCOPY aplicado
    // - Píxel [0,0]: Negro (0x00000000) - limitación hardware
    
    // Limpiar
    close(dst_buf.dma_fd);
    close(src_buf.dma_fd);
    close(out_buf.dma_fd);
    close(ptn_buf.dma_fd);
    close(mask_buf.dma_fd);
    close(g2d);
    
    return 0;
}
```

### Ejemplo Sin Pattern (Solo Src y Dst)

```c
// ROP4 usando solo Source y Destination (sin Pattern)
struct g2d_cmd mask_simple = {
    .cmd_type = G2D_CMD_MASK,
    .dst = { .dma_fd = dst_fd, .width = 64, .height = 64,
             .format = G2D_FMT_ARGB8888, .stride = { 256 } },
    .src = { .dma_fd = src_fd, .width = 64, .height = 64,
             .format = G2D_FMT_ARGB8888, .stride = { 256 } },
    .out = { .dma_fd = out_fd, .width = 64, .height = 64,
             .format = G2D_FMT_ARGB8888, .stride = { 256 } },
    .params.mask = {
        .mask_fd = mask_fd,
        .mask_pitch = 64,
        .mask_crop_w = 64, .mask_crop_h = 64,
        .mask_alpha = 255,
        
        .ptn_fd = -1,  // NO usar pattern
        
        // ROP3 que solo usan Dst y Src (no Pattern)
        .rop_code_0 = 0xCC,  // SRCCOPY: Result = Src
        .rop_code_1 = 0xAA,  // NOP: Result = Dst
    },
    .dst_w = 64, .dst_h = 64,
    .fence_fd_in = -1, .fence_fd_out = -1,
};
// Resultado: Copia Src donde mask==1, mantiene Dst donde mask==0
```

### Casos de Uso Típicos

1. **Alpha Masking**: Componer sprites con máscaras irregulares
   ```c
   rop_code_0 = 0xCC;  // SRCCOPY: tomar Source
   rop_code_1 = 0xAA;  // NOP: mantener Destination
   ptn_fd = -1;         // No necesita pattern
   ```

2. **Pattern Fill con Máscara**: Rellenar áreas con patrón
   ```c
   rop_code_0 = 0xF0;  // PATCOPY: usar Pattern
   rop_code_1 = 0xAA;  // NOP: mantener Destination
   ptn_fd = pattern_fd; // Pattern obligatorio
   ```

3. **Operaciones Lógicas Selectivas**: AND/OR/XOR con máscara
   ```c
   rop_code_0 = 0x88;  // SRCAND: Dst & Src
   rop_code_1 = 0xAA;  // NOP: mantener Destination
   ptn_fd = -1;         // No necesita pattern
   ```

4. **Borrado Selectivo**: Poner a negro áreas enmascaradas
   ```c
   rop_code_0 = 0x00;  // BLACKNESS: negro
   rop_code_1 = 0xAA;  // NOP: mantener Destination
   ptn_fd = -1;         // No necesita pattern
   ```

### Depuración

Si los resultados ROP4 son incorrectos:
1. Verificar `mask_alpha = 255` (otros valores causan problemas)
2. Verificar que todos los crops tienen las mismas dimensiones
3. Si usas Pattern, asegúrate de que `ptn_fd >= 0` y el formato es correcto
4. El formato de máscara debe ser exactamente `G2D_FMT_ARGB8888` (aunque solo usa el canal alpha)
5. Verificar que el canal alpha de la máscara está correctamente inicializado (0xFF para activo, 0x00 para inactivo)
6. Recordar que el píxel [0,0] siempre será negro (limitación hardware)
7. Activar logs de debugging si es necesario:
   ```bash
   sudo bash -c 'echo "file drivers/gpu/sunxi-g2d/*.c format MASK +p" > /sys/kernel/debug/dynamic_debug/control'
   ```

### Referencia Adicional

- Tabla completa de códigos ROP3: Ver documentación de Windows GDI ROP3
- Detalles de implementación: [drivers/gpu/sunxi-g2d/sunxi-g2d-main.c](sunxi-g2d-main.c) función `sunxi_g2d_setup_mask_rcq()`
- Limitación primer píxel: [ROP4-FIRST-PIXEL-ANALYSIS.md](../../ROP4-FIRST-PIXEL-ANALYSIS.md)
- Guía de debugging: [DYNAMIC-DEBUG-GUIDE.md](../../DYNAMIC-DEBUG-GUIDE.md)

## Buenas prácticas DMA-BUF
- **Asignación**:
  - Scanout/framebuffer: usa buffers contiguos (DRM dumb o `G2D_ALLOC_F_CONTIGUOUS`); T113 no tolera `sg_table` lineal con múltiples entradas.
  - Trabajo intermedio (copy/scale/blend): `G2D_IOC_ALLOC_BUFFER` sin flags prefiere system heap si es contiguo; cae a CMA cuando hace falta. Calcula tamaño como `stride * height` (considera 3 planos en YUV).
  - Acceso frecuente CPU ↔ HW: considera `G2D_ALLOC_F_COHERENT` para reducir flushing de caché.
- **Stride y planos**:
  - Rellena `stride[0..2]` y `crop` acorde al formato; para YUV planar/semi-planar alinea según submuestreo (equivalente a `sunxi_g2d_get_yuv_plane_info()`).
  - Mantén `crop_w/h` dentro de `width/height`; el driver valida y puede rechazar.
- **Ciclo de vida**:
  - Reutiliza DMA-BUF y evita adjuntos repetidos; el driver adjunta/mappea por job y libera al completar.
  - Si escribes desde CPU antes de enviar a G2D, sincroniza (`dma_buf_begin/end_cpu_access`) o usa `G2D_IOC_WRITE_BUFFER` (ya sincroniza). Tras operaciones G2D, sincroniza antes de leer desde CPU o usa `G2D_IOC_READ_BUFFER`.
  - Cierra siempre los `dma_fd` exportados y los `fence_fd` recibidos para evitar fugas.
- **Fences y orden**:
  - Encadena jobs pasando `fence_fd_out` previo en `fence_fd_in` cuando comparten buffers.
  - Para pipelines largos, usa `G2D_IOC_TASK` y espera solo un fence final.
- **Límites y validaciones**:
  - Dimensiones soportadas: 2–2048 px por eje.
  - Rotación (90/180/270 y flips) no se mezcla con escalado en el mismo comando.

## Ejemplo mínimo: FILLRECT → COPY al framebuffer
```c
#include <fcntl.h>
#include <linux/sunxi_g2d.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void) {
    int g2d = open("/dev/g2d", O_RDWR);
    struct g2d_alloc_buffer buf = { .size = 800*480*4,
                                    .flags = G2D_ALLOC_F_CONTIGUOUS };
    ioctl(g2d, G2D_IOC_ALLOC_BUFFER, &buf);
    int fb_fd = /* dma_fd del framebuffer DRM */ -1;

    struct g2d_cmd fill = {
        .cmd_type = G2D_CMD_FILLRECT,
        .dst = { .width = 800, .height = 480, .format = G2D_FMT_ARGB8888,
                 .stride = { 800*4 }, .dma_fd = buf.dma_fd },
        .dst_w = 800, .dst_h = 480,
        .params.fillrect.color = 0xff202040,
        .fence_fd_in = -1, .fence_fd_out = -1,
    };
    ioctl(g2d, G2D_IOC_CMD, &fill);
    poll(&(struct pollfd){ .fd = fill.fence_fd_out, .events = POLLIN }, 1, -1);
    close(fill.fence_fd_out);

    struct g2d_cmd copy = {
        .cmd_type = G2D_CMD_COPY,
        .src = fill.dst,
        .dst = { .width = 800, .height = 480, .format = G2D_FMT_ARGB8888,
                 .stride = { 800*4 }, .dma_fd = fb_fd },
        .dst_w = 800, .dst_h = 480,
        .fence_fd_in = -1, .fence_fd_out = -1,
    };
    ioctl(g2d, G2D_IOC_CMD, &copy);
    poll(&(struct pollfd){ .fd = copy.fence_fd_out, .events = POLLIN }, 1, -1);
    close(copy.fence_fd_out);

    close(buf.dma_fd);
    close(g2d);
    return 0;
}
```
Puntos clave: se usa contiguidad para framebuffer, cada comando retorna un fence, `crop_w/h` por defecto se igualan a `width/height` cuando son cero, se cierran todos los fds.

## Ejemplo pipeline con TASK (scale → blend 3 buffers)
1) `G2D_TASK_CREATE` → recibe `task_id`.
2) `G2D_TASK_ADD` con `cmd_type=G2D_CMD_SCALE` (src bola → temp escalado).
3) `G2D_TASK_ADD` con `cmd_type=G2D_CMD_BLEND` (`src=temp`, `dst=background`, `out=output`) y `bld_mode=G2D_BLD_SRCOVER`.
4) `G2D_TASK_RUN` → devuelve `fence_fd_out`; espera/cierra.
5) `G2D_TASK_DEL`.
