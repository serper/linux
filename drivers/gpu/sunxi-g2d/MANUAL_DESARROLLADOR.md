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
  - BLEND 3-buffers no escala; si necesitas escala + blend, haz SCALE y luego BLEND.
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
