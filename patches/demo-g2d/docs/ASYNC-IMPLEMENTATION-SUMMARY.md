# Implementación de Job Queue Asíncrono - Resumen

## Versión
Driver sunxi-g2d v2.9.17

## Objetivo
Convertir el driver de G2D de operación síncrona (bloqueante) a asíncrona (no-bloqueante) usando una cola de jobs con workqueue del kernel.

## Arquitectura

### ANTES (Síncrona - v2.9.16)
```
User IOCTL → Import DMA-BUF → Configure HW → CMD_START → wait_event_timeout() → IRQ → Return
              ↑_________________________ BLOQUEA AQUÍ _________________________↑
```

**Problemas:**
- Un job a la vez (no pipelining)
- Proceso de usuario bloqueado hasta completion
- Desperdicio de CPU (idle esperando HW)

### DESPUÉS (Asíncrona - v2.9.17)
```
User IOCTL → Create Job → Enqueue → Return fence_fd (INMEDIATO, NO BLOQUEA)
                            ↓
                    Workqueue Worker (sunxi_g2d_job_worker)
                            ↓
               Job Dequeued → Configure HW → CMD_START
                                ↓
                           IRQ Handler
                                ↓
                    Signal fence → Cleanup → Schedule Next Job
```

**Ventajas:**
- No bloquea userspace (retorna fence_fd inmediatamente)
- Permite múltiples jobs en cola (pipelining)
- CPU libre para hacer otras cosas mientras G2D trabaja
- Compatible con DRM sync model (sync_file/dma_fence)

## Cambios Implementados

### 1. Estructuras de Datos

#### struct sunxi_g2d_dev (línea ~286)
```c
/* Nuevos campos */
atomic64_t jobs_submitted;    // Contador de jobs enviados
spinlock_t job_lock;           // Ya existía, protege job_queue
struct list_head job_queue;    // Ya existía, cola de jobs pendientes
struct workqueue_struct *job_wq; // Ya existía, workqueue
struct work_struct job_work;   // Inicializado ahora para worker
struct sunxi_g2d_job *current_job; // Ya existía, job en ejecución
```

#### struct sunxi_g2d_job (línea ~370)
```c
struct sunxi_g2d_job {
    /* Core fields */
    struct list_head node;
    struct dma_fence *fence;
    int fence_fd;
    struct sync_file *sync_file;
    struct work_struct cleanup_work;
    enum g2d_job_type type;  // FILLRECT, BLIT, etc.
    
    /* Operation parameters */
    union {
        struct g2d_job_fillrect_data fillrect;
        struct g2d_job_blit_data blit;  // TODO
    } data;
    
    /* DMA addresses (mapped from DMA-BUFs) */
    dma_addr_t src_dma;
    dma_addr_t dst_dma;
    dma_addr_t out_dma;
    
    /* DMA-BUF references (for cleanup) */
    struct dma_buf *dst_dmabuf;
    struct dma_buf_attachment *dst_attach;
    struct sg_table *dst_sgt;
    /* src/out similar */
};
```

### 2. Job Worker (línea ~885)
```c
static void sunxi_g2d_job_worker(struct work_struct *work)
```

**Responsabilidades:**
1. Dequeue job de `job_queue` (bajo spinlock)
2. Set `current_job`
3. Ejecutar operación según `job->type`:
   - `G2D_JOB_FILLRECT`: llama `sunxi_g2d_do_fillrect()`
   - `G2D_JOB_BLIT`: TODO (no implementado aún)
4. Retornar INMEDIATAMENTE sin wait
5. HW triggereará IRQ cuando complete

**Nota:** NO espera completion, el IRQ handler se encarga de eso.

### 3. IRQ Handler (línea ~1054)
```c
static irqreturn_t sunxi_g2d_irq(int irq, void *data)
```

**Modificación en línea ~1075:**
```c
/* After signaling fence and scheduling cleanup */
queue_work(g2d->job_wq, &g2d->job_work);
```

Esto hace que después de completar un job, automáticamente se procese el siguiente en la cola.

### 4. IOCTL FILLRECT (línea ~4686)
```c
static long sunxi_g2d_ioctl_fillrect(struct sunxi_g2d_dev *g2d, unsigned long arg)
```

**Cambios principales:**

#### Antes (sync):
```c
ret = sunxi_g2d_do_fillrect(...);  // BLOQUEABA aquí
/* Crear fence DESPUÉS */
```

#### Ahora (async):
```c
/* 1. Crear job */
job = kzalloc(...);

/* 2. Crear fence */
job->fence = sunxi_g2d_fence_create(g2d);

/* 3. Reservar FD */
out_fd = get_unused_fd_flags(O_CLOEXEC);

/* 4. Crear sync_file */
job->sync_file = sync_file_create(job->fence);

/* 5. CRITICAL: Instalar FD en process context (NO en IRQ) */
fd_install(out_fd, job->sync_file->file);
job->sync_file = NULL;  // fd table owns ref now

/* 6. Guardar parámetros en job */
job->type = G2D_JOB_FILLRECT;
job->dst_dma = dma_addr;
job->data.fillrect.width = width;
/* ... resto de parámetros ... */

/* 7. Transferir DMA-BUF ownership al job */
job->dst_dmabuf = dmabuf;
job->dst_attach = attach;
job->dst_sgt = sgt;

/* 8. Encolar job */
spin_lock_irqsave(&g2d->job_lock, flags);
list_add_tail(&job->node, &g2d->job_queue);
spin_unlock_irqrestore(&g2d->job_lock, flags);

/* 9. Programar worker */
queue_work(g2d->job_wq, &g2d->job_work);

/* 10. Return fence_fd to userspace */
fill.fence_fd_out = job->fence_fd;

/* 11. RETURN IMMEDIATELY - NO WAIT */
return 0;  // Job is async, cleanup will happen later
```

**CRÍTICO:** NO hacemos cleanup de DMA-BUFs en el IOCTL (excepto si hay error antes de encolar). Los buffers ahora son propiedad del job y se limpiarán en `cleanup_work`.

### 5. Cleanup Worker (línea ~403)
```c
static void sunxi_g2d_job_cleanup_workfn(struct work_struct *work)
```

**Añadido:**
```c
/* Release imported DMA-BUFs */
if (job->dst_sgt && job->dst_attach)
    dma_buf_unmap_attachment(job->dst_attach, job->dst_sgt, DMA_FROM_DEVICE);
if (job->dst_attach && job->dst_dmabuf)
    dma_buf_detach(job->dst_dmabuf, job->dst_attach);
if (job->dst_dmabuf)
    dma_buf_put(job->dst_dmabuf);
/* src/out similar */
```

Se llama desde IRQ handler después de completar el job.

### 6. Inicialización en Probe (línea ~5774)
```c
/* Initialize job worker */
INIT_WORK(&g2d->job_work, sunxi_g2d_job_worker);
dev_info(&pdev->dev, "Job worker initialized for async operations\n");
```

## Estado de Implementación

### ✅ Completado
- [x] Estructura de datos (job queue, worker, locks)
- [x] Job worker function
- [x] IRQ handler enhancement (chain next job)
- [x] FILLRECT IOCTL asíncrono
- [x] Cleanup de DMA-BUFs en workqueue
- [x] Fence creation/signaling
- [x] Compilación exitosa

### 🔄 Pendiente
- [ ] Async BLIT IOCTL (similar a FILLRECT)
- [ ] Async SCALE IOCTL
- [ ] Async ROTATE IOCTL
- [ ] Testing con demos en `patches/demo-g2d/demos/`
- [ ] Verificar fence synchronization
- [ ] Medir performance (vs sync mode)

## Testing

### Demos Disponibles
```bash
cd patches/demo-g2d/demos/

# Test básico de fillrect async
./demo-bouncing-ball   # Debería funcionar con async mode

# Test de 3-buffer (chromakey/porter-duff)
./demo-3buffer-test    # Necesita async BLIT

# Test de rotation
./demo-rotate-test     # Necesita async ROTATE
```

### Validación
```bash
# Cargar driver
insmod drivers/gpu/sunxi-g2d/sunxi-g2d.ko

# Check dmesg para "Job worker initialized"
dmesg | grep -i "job worker"

# Ejecutar demo
./demo-bouncing-ball

# Verificar ejecución async en dmesg
dmesg | tail -100 | grep -i "enqueued\|fence_fd\|job_worker"
```

### Debugging
```bash
# Habilitar debug messages
insmod sunxi-g2d.ko g2d_debug=1

# Ver flujo de jobs
dmesg -w | grep -i "fillrect\|job\|fence"
```

## Compatibilidad

### Userspace
- **fence_fd_out**: Los demos existentes ya soportan fences (aunque no los usen)
- **Retrocompatibilidad**: Userspace que ignore fence_fd seguirá funcionando (job se ejecuta igual)
- **Sync explícito**: Userspace puede hacer `poll()` o `sync_wait()` en fence_fd

### Kernel APIs
- **DMA fence**: Estándar del kernel, compatible con DRM
- **sync_file**: Compatible con Android sync framework
- **DMA-BUF**: Sin cambios en API

## Próximos Pasos

1. **Testing básico**: Ejecutar `demo-bouncing-ball` para validar FILLRECT async
2. **Port BLIT async**: Aplicar mismo patrón a IOCTL de BLIT
3. **Port SCALE/ROTATE async**: Resto de operaciones
4. **Performance testing**: Medir latencia y throughput vs sync mode
5. **Stress testing**: Múltiples jobs concurrentes
6. **Fence wait testing**: Validar userspace puede esperar en fences

## Notas Técnicas

### Critical: FD Installation
**Debe hacerse en process context** (IOCTL), NO en IRQ context:
```c
/* GOOD (IOCTL context) */
fd_install(out_fd, sync_file->file);

/* BAD (IRQ context) */
// NEVER touch process fd table from IRQ!
```

### DMA-BUF Lifetime
1. **Import**: IOCTL
2. **Transfer**: a job (IOCTL)
3. **Use**: worker thread (HW access)
4. **Release**: cleanup_work (after IRQ)

### Job Queue Ordering
- FIFO: `list_add_tail()` + `list_first_entry()`
- Single worker thread: garantiza orden de ejecución
- No parallelism: G2D HW solo puede ejecutar un job a la vez

## Referencias
- Design doc: `patches/demo-g2d/docs/JOB-QUEUE-ASYNC-DESIGN.md`
- DMA fence API: `include/linux/dma-fence.h`
- sync_file API: `include/linux/sync_file.h`
- Workqueue API: `include/linux/workqueue.h`
