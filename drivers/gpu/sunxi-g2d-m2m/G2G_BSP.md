# G2D RCQ BSP (Allwinner T113-S3)

Este documento resume la información clave extraída del driver RCQ (`v2`) del motor G2D incluido en `patches/sunxi_g2d`. La intención es facilitar la implementación de un driver propio sobre el SoC Allwinner T113-S3 centrándonos exclusivamente en la versión basada en *Register Command Queue* (RCQ).

---

## 1. Arquitectura y módulos

- El bloque G2D se divide en submódulos con offsets fijos respecto a la base mapeada (`para.io`). Las constantes están definidas en `g2d_rcq/g2d_mixer_type.h` y `g2d_regs_v2.h`.
  | Módulo        | Offset |
  |---------------|--------|
  | TOP           | `0x00000` |
  | MIXER GLB     | `0x00100` |
  | BLENDER (BLD) | `0x00400` |
  | Video Layer 0 | `0x00800` |
  | UI Layer 0/1/2| `0x01000`, `0x01800`, `0x02000` |
  | Write Back WB | `0x03000` |
  | VSU (scaler)  | `0x08000` |
  | ROT           | `0x28000` |
  | GSU           | `0x30000` |

- El *TOP* controla clocks, resets y toda la lógica RCQ (`g2d_rcq/g2d_top.c`, `g2d_rcq/g2d_top_type.h`).
- Cada submódulo expone una estructura de registros empaquetada (p.ej. `struct g2d_mixer_ovl_v_reg` para la capa de vídeo) con el layout de los registros tal y como los espera el hardware.

---

## 2. Registros TOP y control RCQ

| Registro TOP                      | Offset              | Descripción |
|----------------------------------|---------------------|-------------|
| `sclk_gate`, `hclk_gate`, `ahb_rst` | `0x00`, `0x04`, `0x08` | Puertas de reloj y reset de los bloques MIXER/ROT (`g2d_rcq/g2d_top_type.h:22-56`). |
| `sclk_div`                       | `0x0C`              | Divisores independientes para MIXER y ROT. |
| `version`                        | `0x10`              | Información de versión/IP. |
| `rcq_irq_ctl`                    | `0x20`              | Bits `task_end_irq_en` y `rcq_cfg_finish_irq_en`; hay un bit `rcq_sel` para escoger modo RCQ (`g2d_rcq/g2d_top_type.h:70`). |
| `rcq_status`                     | `0x24`              | `task_end_irq`, `cfg_finish_irq` y contador `frame_cnt`. |
| `rcq_ctrl`                       | `0x28`              | Bit `update` que arranca la carga de la cola. |
| `rcq_header_low_addr/high_addr`  | `0x2C` / `0x30`     | Dirección física (64 bit) del encabezado RCQ. |
| `rcq_header_len`                 | `0x34`              | Número de bytes del encabezado (múltiplo de 2). |

Funciones de ayuda (`g2d_rcq/g2d_top.c`):
- `g2d_top_set_base()` recibe `para.io` en `drv_g2d_init()` para mapear el `struct g2d_top_reg`.
- `g2d_top_rcq_irq_en()` habilita/deshabilita la generación de `task_end_irq`.
- `g2d_top_rcq_update_en()` activa el bit `update`.
- `g2d_top_set_rcq_head()` escribe la dirección física y longitud del encabezado.
- `g2d_top_rcq_task_irq_query()` limpia y devuelve `task_end_irq`.

---

## 3. Layout de la cola RCQ

Estructuras principales (`g2d_rcq/g2d_rcq.h`):

- `struct g2d_rcq_head` (encabezado por bloque):
  - `low_addr` + `dw0.bits.high_addr`: puntero físico 32+8 bits al snapshot de registros.
  - `dw0.bits.len`: longitud del bloque de registros (máx. 24 bits).
  - `dirty.bits.dirty`: flag que indica si el bloque debe aplicarse.
  - `dirty.bits.n_header_len`: longitud del siguiente encabezado RCQ dentro del mismo frame (permite encadenar frames).
  - `reg_offset`: offset de los registros respecto a la base G2D (lo usa el hardware para direccionar).

- Alineaciones:
  - Cada bloque de registros se alinea a 32 bytes: `G2D_RCQ_BYTE_ALIGN()`.
  - El número de encabezados por frame se alinea a 2: `G2D_RCQ_HEADER_ALIGN()`.

- `struct g2d_reg_block` relaciona cada snapshot:
  - `phy_addr` / `vir_addr`: memoria DMA coherente donde se escriben los registros.
  - `reg_addr`: offset HW a programar.
  - `rcq_hd`: puntero al encabezado asociado.

- `struct g2d_rcq_mem_info` guarda el pool de memoria RCQ:
  - `rcq_reg_mem_size`: tamaño total (encabezados + snapshots).
  - `alloc_num_per_frame`: encabezados reservados por frame.
  - `block_num_per_frame`: bloques reales por frame (uno por submódulo activo).
  - `reg_blk`: array con todos los `g2d_reg_block` del task.

Asignación de memoria (`g2d_rcq/g2d_rcq.c`):
1. `g2d_top_mem_pool_alloc()` reserva un bloque DMA con `g2d_malloc()` (alineado a 4 KB).
2. `g2d_top_reg_memory_alloc()` entrega trozos alineados a 32 bytes para cada registro lógico, devolviendo tanto dirección virtual como física.
3. `g2d_top_mem_pool_free()` libera el bloque al destruir la tarea.

---

## 4. Flujo de inicialización y uso

1. **Probe** (`g2d_rcq/g2d.c:980+`):
   - Mapea registros (`of_iomap`), obtiene IRQ y clocks.
   - Llama a `drv_g2d_init()` → `g2d_top_set_base()` y `g2d_rot_set_base()` (si procede).

2. **Apertura del dispositivo** (`g2d_open()`):
   - Protegido por `para.mutex`.
   - Habilita clocks (`g2d_clock_enable`) y hace `g2d_bsp_open()`:
     - `g2d_bsp_open()` (TOP directo, `g2d_rcq/g2d_top.c:49`) levanta `sclk_gate`, `hclk_gate` y `ahb_rst`.

3. **Preparación de una tarea** (`create_mixer_task()` → `g2d_mixer_mem_setup()` en `g2d_rcq/g2d_mixer.c:437+`):
   - Calcula `block_num_per_frame` sumando los submódulos activos (video, UI, blender, scaler, wb).
   - Reserva `alloc_num_per_frame` encabezados alineados.
   - Obtiene memoria RCQ con `g2d_top_mem_pool_alloc()`.
   - Asigna cada submódulo:
     - `ovl_v_rcq_setup()` usa `base + G2D_V0`.
     - `ovl_u_rcq_setup()` crea tres bloques en `base + G2D_UI{0,1,2}`.
     - `bld_rcq_setup()` mapea `base + G2D_BLD`.
     - `scal->rcq_setup()` usa `base + G2D_VSU`.
     - `wb_rcq_setup()` usa `base + G2D_WB`.
   - Cada `rcq_setup` llena el `g2d_reg_block` con el offset HW (`reg_addr`) y el snapshot DMA reservado (`vir_addr`, `phy_addr`).
   - Se calcula `rcq_header_len` = `alloc_num_per_frame * sizeof(struct g2d_rcq_head)`.
   - Para cada frame se rellena la lista de encabezados:
     - `rcq_hd->reg_offset` = `reg_addr - base`.
     - `n_header_len` apunta al encabezado del frame siguiente (0 en el último).

4. **Aplicación de la tarea** (`g2d_mixer_apply()`):
   - Deshabilita `update` e IRQ antes de escribir (`g2d_top_rcq_update_en(0)`, `g2d_top_rcq_irq_en(0)`).
   - Llama a `g2d_top_set_rcq_head()` con la dirección física del primer encabezado y `rcq_header_len`.
   - Aplica cada operación (`frame[i].apply`) sobre las estructuras en memoria (los writes van al snapshot virtual, no al HW).
   - Activa de nuevo `task_end_irq` y `update`.
   - Espera a `g2d_wait_cmd_finish()` con timeout proporcional al número de frames (`WAIT_CMD_TIME_MS * frame_cnt`).

5. **Interrupción** (`g2d_handle_irq()`):
   - Atiende `task_end_irq` consultando `g2d_top_rcq_task_irq_query()`.
   - Si se completó, resetea el mixer (`g2d_top_mixer_reset()`, que pulsa el bit `mixer_ahb_rst`) y despierta la cola de espera.

6. **Cierre** (`g2d_release()`):
   - Cuando `user_cnt` llega a 0, se deshabilitan clocks y se llama a `g2d_bsp_close()` (limpia gates/resets).
   - El `scan_order` vuelve a `G2D_SM_TDLR` por defecto.

---

## 5. Submódulos y bloques de registros

### 5.1 Capa de vídeo (`g2d_rcq/g2d_ovl_v.c`)
- Bloque único en `G2D_V0`.
- Registros principales dentro de `struct g2d_mixer_ovl_v_reg`:
  - `ovl_attr`: formato `lay_fbfmt`, modo alpha, global alpha, flags de premultiplied y fill color.
  - `ovl_mem`: ancho/alto menos 1.
  - `ovl_mem_pitch{0,1,2}`, `ovl_mem_low_addr{0,1,2}`, `ovl_mem_high_addr` (Y/C planes).
  - Ventana (`ovl_winsize`) y down-samplers horizontales/verticales (para conversión YUV a 4:4:4).
- `g2d_vlayer_set()` calcula pitches según formato (`g2d_byte_cal`) y rellena direcciones físicas de cada plano.
- `g2d_ovl_v_calc_coarse()` programa coeficientes de down-sampling para formatos YUV420/422/411.

### 5.2 Capas UI (`g2d_rcq/g2d_ovl_u.c`)
- Tres bloques independientes (`G2D_UI0`, `G2D_UI1`, `G2D_UI2`).
- Cada bloque reutiliza `struct g2d_mixer_ovl_u_reg`.
- `g2d_uilayer_set()` fija formato, alpha, pitches y dirección base (solo un plano).
- `g2d_ovl_u_fc_set()` habilita fill color cuando `bbuff == 0`.

### 5.3 Blender (`g2d_rcq/g2d_bld.c`)
- Registros en `struct g2d_mixer_bld_reg` (mezcla, keying, ROP, CSC).
- Funciones clave:
  - `bld_fc_set()` programa fill color por canal.
  - `bld_porter_duff()` escribe el modo de fusión (campos en `bld_ctrl`).
  - `bld_csc_reg_set()` carga matrices CSC RGB↔YUV según gamut (`709`, `601`, `2020`).

### 5.4 Scaler (`g2d_rcq/g2d_scal.c`)
- Base `G2D_VSU`.
- Registros agrupados en `struct g2d_mixer_video_scaler_reg`.
- Controla tamaño de salida (`vs_out_size`), pasos/coeficientes horizontales y verticales (`vs_y_*`, `vs_c_*`).
- `g2d_vsu_para_set()` selecciona tablas de coeficientes y programa `vs_ctrl`.

### 5.5 Write Back (`g2d_rcq/g2d_wb.c`)
- Base `G2D_WB`.
- `g2d_wb_set()` calcula pitches y direcciones físicas para salida (hasta 3 planos).
- `wb_attr.bits.fmt` guarda el formato del destino.

---

## 6. Gestión de memoria de imágenes

- `g2d_set_image_addr()` (en `g2d_rcq/g2d_mixer.c`) prepara `dmabuf_item` a partir de los `fd` de usuario usando DMA-BUF (`g2d_rcq/g2d.c:144+`).
- Pitches y offsets siempre se recalculan antes de escribir los snapshots (los valores se almacenan en memoria RCQ y sólo se aplican al disparar).
- Si `en_split_mem` está activo (memoria lineal para animaciones), la función `g2d_split_mem()` ajusta direcciones para cada frame.

---

## 7. Secuencia típica para un driver RCQ

1. Mapear `para.io` y llamar a `g2d_top_set_base(base)`.
2. Al habilitar el motor:
   - `g2d_bsp_open()` → habilitar clocks y reset.
   - Configurar `scan_order` si es necesario (`g2d_mixer_scan_order_fun()` escribe `mixer_ctrl.bits.scan_order` a través de la estructura mapeada).
3. Preparar memoria RCQ:
   - Llamar a `g2d_top_mem_pool_alloc()` con el número de frames planificado.
   - Reservar snapshots por submódulo mediante sus `rcq_setup`.
4. Llenar snapshots:
   - Usar las funciones `g2d_vlayer_set()`, `g2d_uilayer_set()`, `bld_*`, `g2d_wb_set()`, etc., que actúan sobre la memoria virtual del snapshot.
   - Marcar los bloques como sucios (`set_block_dirty`, que a su vez pone `rcq_hd->dirty = 1`).
5. Disparar:
   - `g2d_top_rcq_update_en(0)` → `g2d_top_set_rcq_head()` → `g2d_top_rcq_irq_en(1)` → `g2d_top_rcq_update_en(1)`.
6. Esperar a `task_end_irq`, limpiar y, si se pretende reusar la misma cola, volver a escribir los snapshots antes del siguiente `update`.

---

## 8. Notas adicionales

- **Alineación DMA**: la reserva RCQ usa `dma_alloc_coherent`; en plataformas ARM64 se rellenan los bits altos de dirección (`rcq_hd->dw0.bits.high_addr`). En 32 bit se mantiene en cero (`g2d_rcq/g2d_mixer.c:469`).
- **Timeout y reset de emergencia**: si `g2d_wait_cmd_finish()` expira, se ejecuta `g2d_bsp_reset()` que relanza los resets del TOP.
- **IRQ compartido**: la ISR también atiende al bloque ROT si estaba habilitado (`g2d_handle_irq()`).
- **sysfs debug**: escribir `2` en `attr/debug` activa un volcado de encabezados RCQ y snapshots en `/tmp` (`g2d_mixer_rcq_debug()`).
- **Clocks**: el driver espera tres clocks (`g2d`, `bus`, `mbus_g2d`) y un reset software; deben declararse en el device tree.

---

## 9. Referencias rápidas

- Base y registros TOP: `g2d_rcq/g2d_top_type.h`.
- Control RCQ: `g2d_rcq/g2d_top.c`, `g2d_rcq/g2d_mixer.c` (funciones `g2d_top_rcq_*` y `g2d_mixer_apply`).
- Layout snapshots: `g2d_rcq/g2d_rcq.h`, `g2d_rcq/g2d_rcq.c`.
- Submódulos:
  - Vídeo: `g2d_rcq/g2d_ovl_v.c`.
  - UI: `g2d_rcq/g2d_ovl_u.c`.
  - Blender: `g2d_rcq/g2d_bld.c`.
  - Scaler: `g2d_rcq/g2d_scal.c`.
  - Writeback: `g2d_rcq/g2d_wb.c`.
- Flujo de tarea: `g2d_rcq/g2d_mixer.c`.
- Gestión global / ioctl: `g2d_rcq/g2d.c`.

Con esta información se dispone de un mapa completo de offsets, estructuras y secuencias necesarias para implementar o portar un driver G2D basado en RCQ para el Allwinner T113-S3.
