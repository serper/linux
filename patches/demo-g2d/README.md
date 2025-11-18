Demos y pruebas para el driver sunxi-g2d

Contenido:
- demos/: ejecutables y fuentes de demostración (demo-*-test, demo-bouncing-ball, etc.)
- tests/: scripts y utilidades para ejecutar pruebas (test-3buffer.sh)
- docs/: documentos y notas relacionadas con RCQ y pruebas

Uso rápido
1. Compila el módulo del driver desde la raíz del árbol del kernel:

   make drivers/gpu/sunxi-g2d/sunxi-g2d.ko

2. Inserta el módulo (con permisos apropiados):

   sudo insmod drivers/gpu/sunxi-g2d/sunxi-g2d.ko

3. Ejecuta un demo (ejemplo):

   # ejecutable local
   patches/demo-g2d/demos/demo-3buffer-test

   # o compilar y ejecutar la fuente en el host si es necesario
   gcc -O2 -o demo demo-bouncing-ball.c && ./demo

Notas
- Estos demo y tests se han movido fuera del árbol principal del driver a
  `patches/demo-g2d` para mantener el directorio del driver limpio.
- Los binarios están incluidos como referencia; si prefieres solo las fuentes,
  indícalo y los dejaré fuera del repositorio.

Licencia y autores
- Mantener la misma licencia que el proyecto principal.

## Demo G2D + DRM (T113) – Política de memoria y uso

El motor de display (DE/sun4i-drm) en T113 no usa IOMMU para scanout; los framebuffers deben ser físicamente contiguos (CMA) y lineales.

### Reglas prácticas

- Framebuffer para scanout (DRM/DE):
   - Crear como dumb buffer con `drmModeCreateDumb` (CMA contiguo).
   - Exportar por PRIME a G2D para el `CMD_COPY` final o composición directa.
   - No usar `dma-heap` system ni buffers “coherentes/IOMMU” para scanout: pueden no ser contiguos físicamente y el DE mostrará basura.

- Buffers de trabajo (solo G2D):
   - Usar páginas del sistema (sg) o backend coherente/IOMMU del driver G2D (no consumen CMA).
   - Pueden no ser mapeables a userland; usa `G2D_IOC_WRITE_BUFFER`/`READ_BUFFER`.

- G2D COPY a framebuffer:
   - El destino debe ser contiguo (nents=1) o el driver devuelve `-EOPNOTSUPP`.
   - Con dumb buffer el COPY es hardware; no hace falta memcpy de CPU.

### Defaults del driver

- `g2d_alloc_mode = 2` por defecto (coherente/IOMMU) para buffers internos de G2D.
- `g2d_alloc_use_system_heap = 1` por defecto (si `g2d_alloc_mode==0`, usa heap del sistema).
- Estos buffers NO son para scanout; son para trabajo interno del G2D.

### Comportamiento del demo

- Por defecto, el demo crea el framebuffer como **dumb buffer** (DRM).
- Variables de entorno para experimentar:
   - `FB_ALLOC_MODE=dumb` (por defecto): usa `drmModeCreateDumb`.
   - `FB_ALLOC_MODE=heap`: intenta `/dev/dma_heap/system` (no recomendado para scanout).
   - `FB_ALLOC_MODE=g2d`: pide el framebuffer al driver G2D (no recomendado para scanout).

> Nota: los modos `heap` y `g2d` solo para pruebas. Si el destino no es contiguo, G2D rechazará el COPY con `-EOPNOTSUPP` y el DE mostrará memoria corrupta.

### Cómo ejecutar

```sh
# Recomendado (scanout correcto):
./demo-bouncing-ball

# Modos experimentales (no recomendado para pantalla):
FB_ALLOC_MODE=heap ./demo-bouncing-ball
FB_ALLOC_MODE=g2d  ./demo-bouncing-ball
```

### FAQ

- ¿Puedo usar buffers coherentes/IOMMU o `dma-heap` para enviar al DRM?
   - No en T113: el DE no usa IOMMU y necesita contigüidad física (CMA). Usa dumb buffer.
- ¿Tengo que copiar por CPU al DRM?
   - No. Exporta el dumb buffer y usa `G2D_CMD_COPY` (hardware) como último paso.
- ¿Por qué a veces `mmap` del framebuffer falla?
   - Algunos exportadores no permiten `mmap`. El demo limpia via `G2D_IOC_FILLRECT` si `mmap` falla. Para dumb buffer hay `DRM_IOCTL_MODE_MAP_DUMB`.
