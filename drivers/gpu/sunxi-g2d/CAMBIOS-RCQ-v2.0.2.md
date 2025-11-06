# Correcciones RCQ v2.0.2 - Cambio de IRQ cfg_finish

## Fecha: 1 de noviembre de 2025
## Problema Identificado

Del análisis de los logs del hardware T113:

```
PRIMERA OPERACIÓN (exitosa en configuración):
  RCQ setup: CMD_CTL=0x00010001 ✅
  RCQ regs written: HEAD_LOW=0x05c4b000 ✅
  RCQ status after UPDATE: STATUS=0x00000104
    → bit 2 (cfg_finish_irq) = 1 ✅
    → bit 8-15 (frame_cnt) = 1 ✅
    → bit 0 (task_end_irq) = 0 ❌
  Result: TIMEOUT (no IRQ recibida)
  → sunxi_g2d_hw_disable() apaga el hardware

SEGUNDA OPERACIÓN (falla por hardware apagado):
  RCQ setup: CMD_CTL=0x00000000 ❌
  RCQ regs readback zero ❌
  Todos los registros = 0x00000000
  → Hardware completamente apagado
```

**Causa raíz:**
- El T113 G2D activa `cfg_finish_irq` (bit 2) en lugar de `task_end_irq` (bit 0)
- El driver esperaba `task_end_irq` → nunca llegaba la IRQ
- `sunxi_g2d_hw_disable()` apagaba el hardware tras timeout
- Operaciones siguientes fallaban por hardware apagado

## Cambios Implementados

### 1. Cambiar IRQ de task_end a cfg_finish

**Archivo:** `drivers/gpu/sunxi-g2d/sunxi-g2d-rcq.c`  
**Función:** `sunxi_g2d_rcq_start()`

```c
// ANTES (v2.0.1):
irq_ctl.bits.task_end_irq_en = 1;  /* Enable task end IRQ */

// DESPUÉS (v2.0.2):
irq_ctl.bits.rcq_cfg_finish_irq_en = 1;  /* Enable config finish IRQ */
irq_ctl.bits.task_end_irq_en = 0;        /* Disable task end (not used by T113) */
```

**Logs actualizados:**
```c
pr_info("RCQ started: IRQ_CTL=0x%08x (cfg_finish_irq_en=%d)\n",
        irq_ctl.dwval, irq_ctl.bits.rcq_cfg_finish_irq_en);
```

### 2. Actualizar handler de IRQ

**Archivo:** `drivers/gpu/sunxi-g2d/sunxi-g2d-main.c`  
**Función:** `sunxi_g2d_irq()`

```c
// NUEVO: Check cfg_finish primero (T113 usa este)
if (g2d_top->rcq_status.bits.cfg_finish_irq) {
    g2d_top->rcq_status.bits.cfg_finish_irq = 1;  /* W1C */
    dev_info(g2d->dev, "RCQ IRQ received (cfg_finish): status=0x%08x\n", 
             g2d_top->rcq_status.dwval);
    atomic_set(&g2d->irq_done, 1);
    wake_up(&g2d->irq_wait);
    handled = true;
}

// FALLBACK: Check task_end por si acaso (otros SoCs)
if (g2d_top->rcq_status.bits.task_end_irq) {
    // ... (mismo código)
}
```

### 3. Eliminar hw_disable() tras operaciones RCQ

**Archivos:** `sunxi_g2d_do_fillrect_rcq()` y `sunxi_g2d_do_blit_alpha_rcq()`

```c
// ANTES (v2.0.1):
if (timeout == 0) {
    dev_err(g2d->dev, "Fillrect RCQ timeout: RCQ_STATUS=0x%08X\n", rcq_status);
    sunxi_g2d_hw_disable(g2d);  ❌
    return -ETIMEDOUT;
}
dev_info(g2d->dev, "FILLRECT_RCQ completed via RCQ IRQ\n");
sunxi_g2d_hw_disable(g2d);  ❌
return 0;

// DESPUÉS (v2.0.2):
if (timeout == 0) {
    dev_err(g2d->dev, "Fillrect RCQ timeout: RCQ_STATUS=0x%08X\n", rcq_status);
    /* DON'T disable hardware - keep it running for next operation */
    return -ETIMEDOUT;
}
dev_info(g2d->dev, "FILLRECT_RCQ completed via RCQ IRQ\n");
/* DON'T disable hardware after success - keep it running for next operation */
return 0;
```

**Razón:** El hardware debe permanecer encendido entre operaciones RCQ. Solo se apaga en:
- `sunxi_g2d_close()` cuando se cierra el device
- Errores de inicialización/setup (antes de la primera operación)

## Pruebas en Dispositivo

### Comandos en el dispositivo T113:

```bash
# SSH al dispositivo
ssh spuc@10.42.0.1

# Cargar módulo corregido
sudo rmmod sunxi-g2d || true
sudo insmod /home/spuc/sunxi-g2d-v2.0.2-rcq-fix.ko rcq_enable_bit=1

# Limpiar dmesg
sudo dmesg -C

# Ejecutar demo
./demo-bouncing-ball &
sleep 3
pkill demo-bouncing-ball

# Recopilar logs
dmesg > /home/spuc/dmesg-v2.0.2.txt
cat /proc/interrupts | grep g2d > /home/spuc/interrupts-v2.0.2.txt

# Desde host, copiar logs:
exit
scp spuc@10.42.0.1:/home/spuc/dmesg-v2.0.2.txt /tmp/
scp spuc@10.42.0.1:/home/spuc/interrupts-v2.0.2.txt /tmp/
```

### Logs Esperados (si funciona correctamente):

```
✅ PRIMERA OPERACIÓN:
[ X.XXX] RCQ setup: CMD_CTL=0x00010001 (should be 0x00010001 for DMA)
[ X.XXX] RCQ regs written: HEAD_LOW=0x05c4b000 HEAD_HIGH=0x00000000 HEAD_LEN=0x0000000a
[ X.XXX] RCQ writing CTRL=0x00000011 (en=1 update=1 use_en_bit=1)
[ X.XXX] RCQ started: IRQ_CTL=0xXXXXXXXX (cfg_finish_irq_en=1)
[ X.XXX] RCQ status after UPDATE: CTRL=0x00000000 STATUS=0x00000104
[ X.XXX] RCQ IRQ received (cfg_finish): status=0x00000104 ✅✅✅
[ X.XXX] FILLRECT_RCQ completed via RCQ IRQ ✅

✅ SEGUNDA OPERACIÓN (hardware sigue encendido):
[ X.XXX] RCQ setup: CMD_CTL=0x00010001 ✅ (NO es 0x00000000)
[ X.XXX] RCQ regs written: HEAD_LOW=0x05c4b000 ✅ (NO es 0x00000000)
[ X.XXX] RCQ IRQ received (cfg_finish): status=0x00000104 ✅
[ X.XXX] FILLRECT_RCQ completed via RCQ IRQ ✅

✅ /proc/interrupts:
253:         XX         XX    GICv2 121 Level     5410000.g2d
              ^^         ^^
              Contadores incrementados (NO cero)
```

### Si aún falla (logs de diagnóstico):

Si sigue habiendo timeout, buscar en dmesg:
- `STATUS=0x00000104` pero sin mensaje "RCQ IRQ received" → IRQ no llegó al handler
- `STATUS=0x00000000` → RCQ no ejecutó (problema de configuración)
- Otros valores de STATUS → decodificar bits para diagnosticar

## Cambios Técnicos Detallados

### RCQ_STATUS register (T113 behavior):
```
Bit 0: task_end_irq    - NO se activa en T113 ❌
Bit 2: cfg_finish_irq  - SÍ se activa en T113 ✅
Bit 8-15: frame_cnt    - Cuenta frames procesados (1, 2, 3...)
```

### Secuencia correcta para T113:
1. `sunxi_g2d_hw_enable()` - configurar clocks, reset, CMD_CTL=0x00010001
2. `sunxi_g2d_rcq_setup_hw()` - escribir HEAD_LOW/HIGH/LEN
3. `sunxi_g2d_rcq_start()` - habilitar cfg_finish_irq, escribir UPDATE=1
4. **ESPERAR IRQ cfg_finish** (bit 2)
5. Operación completada
6. **NO** llamar `hw_disable()` - hardware permanece encendido
7. Siguiente operación puede proceder inmediatamente

## Archivos Modificados

- `drivers/gpu/sunxi-g2d/sunxi-g2d-rcq.c` (líneas ~285-300)
- `drivers/gpu/sunxi-g2d/sunxi-g2d-main.c`:
  - IRQ handler (líneas ~538-560)
  - `sunxi_g2d_do_fillrect_rcq()` (líneas ~1010-1030)
  - `sunxi_g2d_do_blit_alpha_rcq()` (líneas ~1580-1610)

## Versión

**Driver:** sunxi-g2d v2.0.2  
**Compilado:** 1 de noviembre de 2025  
**Archivo:** `sunxi-g2d-v2.0.2-rcq-fix.ko`

---

## Estado

⏳ **PENDIENTE DE PRUEBAS EN HARDWARE T113**
