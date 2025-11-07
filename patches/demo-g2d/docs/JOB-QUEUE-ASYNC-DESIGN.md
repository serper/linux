# Diseño de Job Queue Asíncrono para sunxi-g2d

## Objetivo
Implementar operaciones completamente asíncronas usando un job queue con workqueue del kernel, permitiendo que múltiples operaciones se encolen sin bloquear el proceso de usuario.

## Arquitectura Actual (Síncrona)
```
User IOCTL → Configurar HW → CMD_CTL_START → wait_event_timeout() → IRQ → Retornar
             ↑______________ BLOQUEA AQUÍ _______________↑
```

## Arquitectura Nueva (Asíncrona)
```
User IOCTL → Crear Job → Encolar → Retornar fence_fd (INMEDIATO)
                           ↓
                      Workqueue Worker
                           ↓
              Job Dequeued → Configurar HW → CMD_CTL_START
                                                  ↓
                                             IRQ Handler
                                                  ↓
                                      Signal fence → Cleanup → Next job
```

## Componentes

### 1. Struct sunxi_g2d_job (ya existe parcialmente)
```c
struct sunxi_g2d_job {
    struct list_head node;           // Para la cola
    struct dma_fence *fence;          // Fence para sincronización
    int fence_fd;                     // FD retornado a userspace
    struct sync_file *sync_file;      // sync_file wrapper
    enum g2d_job_type type;           // BLIT o FILLRECT
    
    // Datos de la operación
    union {
        struct g2d_blit blit;
        struct g2d_fillrect fillrect;
    } data;
    
    // DMA-BUF imports (para cleanup)
    struct dma_buf *src_dmabuf;
    struct dma_buf_attachment *src_attach;
    struct sg_table *src_sgt;
    
    struct dma_buf *dst_dmabuf;
    struct dma_buf_attachment *dst_attach;
    struct sg_table *dst_sgt;
    
    struct dma_buf *out_dmabuf;       // Para 3-buffer blit
    struct dma_buf_attachment *out_attach;
    struct sg_table *out_sgt;
};
```

### 2. sunxi_g2d_dev additions (algunos ya existen)
```c
struct sunxi_g2d_dev {
    // ... existing fields ...
    
    // Job queue
    spinlock_t job_lock;              // Protege job_queue y current_job
    struct list_head job_queue;       // Cola de jobs pendientes
    struct workqueue_struct *job_wq;  // Workqueue para jobs
    struct work_struct job_work;      // Work item para procesar jobs
    struct sunxi_g2d_job *current_job; // Job en ejecución
    
    // Para wait() síncrono (compatible con userspace antiguo)
    wait_queue_head_t irq_wait;
    atomic_t irq_done;
};
```

### 3. Flujo de Ejecución

#### IOCTL (proceso de usuario)
1. Validar parámetros
2. Importar DMA-BUFs
3. Crear job y allocar memoria
4. Crear fence y sync_file
5. Reservar FD para el fence
6. Guardar job en cola
7. **fd_install() AQUÍ (proceso context)**
8. Programar workqueue
9. Retornar fence_fd a userspace inmediatamente

#### Worker Thread
1. Tomar job de la cola (spin_lock)
2. Marcar como current_job
3. Activar hardware (power on si necesario)
4. Configurar registros según tipo de job
5. CMD_CTL_START
6. Retornar (NO wait)

#### IRQ Handler
1. Detectar completion (MIXER_IRQ/ROT_INT/etc)
2. dma_fence_signal(current_job->fence)
3. Programar cleanup workqueue
4. current_job = NULL
5. Programar siguiente job si hay

#### Cleanup Worker
1. Liberar DMA-BUFs importados
2. dma_fence_put()
3. kfree(job)

## Ventajas
- No bloquea userspace
- Permite pipelining de operaciones
- Compatible con DRM sync model
- Mejor uso de CPU (puede hacer otras cosas mientras G2D trabaja)

## Compatibilidad
- fence_fd es estándar de DRM
- Userspace puede hacer poll() o sync_file_wait()
- Múltiples jobs en vuelo
