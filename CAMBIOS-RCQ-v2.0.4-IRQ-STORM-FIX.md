# Cambios RCQ v2.0.4 - IRQ Storm Fix

## 📋 Resumen

**Versión**: v2.0.4-irq-storm-fix  
**Fecha**: 1 noviembre 2025  
**Objetivo**: Solucionar IRQ storm causado por cfg_finish_irq que no se deshabilitaba

## ✅ Éxito de v2.0.3

**CONFIRMADO**: El quirk MBUS funciona, IRQ **SÍ se dispara** ahora.

Logs del usuario:
```
[10789.286344] RCQ IRQ received (cfg_finish): status=0x00000104
[10789.286449] RCQ IRQ received (cfg_finish): status=0x00000104
... (40+ IRQs en 216ms)
```

**Problema MBUS RESUELTO** ✅

## 🔴 Nuevo Problema: IRQ Storm

### Síntomas

- **40+ interrupciones en 216ms** (10789.286 → 10789.624)
- IRQs mezcladas con logs del kernel (preemption)
- **Sistema se cuelga** por saturación de interrupciones
- `/proc/interrupts` muestra miles de IRQs

### Causa Raíz: Race Condition en IRQ Handler

```c
/* Handler v2.0.3 (INCORRECTO) */
if (g2d_top->rcq_status.bits.cfg_finish_irq) {
    g2d_top->rcq_status.bits.cfg_finish_irq = 1;  // Clear bit
    wake_up(&g2d->irq_wait);                     // Signal userspace
}
// ← IRQ sigue habilitado en RCQ_IRQ_CTL
// ← Siguiente operación RCQ setea cfg_finish_irq otra vez
// ← IRQ se dispara inmediatamente de nuevo = LOOP INFINITO
```

**Problema**: Entre el clear del STATUS y el return del handler, si hay otra operación RCQ pendiente (o la misma se relanza), `cfg_finish_irq` vuelve a activarse → IRQ infinito.

## 🔧 Solución v2.0.4

### Deshabilitar IRQ tras recibir

**Cambio en IRQ handler** (`sunxi_g2d_irq()`):

```c
/* Handler v2.0.4 (CORRECTO) */
if (g2d_top->rcq_status.bits.cfg_finish_irq) {
    union g2d_rcq_irq_ctl irq_ctl;
    
    /* CRITICAL: Disable IRQ FIRST to prevent storm */
    irq_ctl.dwval = g2d_read(g2d, G2D_RCQ_IRQ_CTL);
    irq_ctl.bits.rcq_cfg_finish_irq_en = 0;  // ← Disable IRQ
    g2d_write(g2d, G2D_RCQ_IRQ_CTL, irq_ctl.dwval);
    wmb();
    
    /* Clear RCQ config finish interrupt (W1C) */
    g2d_top->rcq_status.bits.cfg_finish_irq = 1;
    wmb();
    
    dev_info(g2d->dev, "RCQ IRQ received (cfg_finish): status=0x%08x (IRQ disabled)\n", 
             g2d_top->rcq_status.dwval);
    
    /* Signal completion */
    atomic_set(&g2d->irq_done, 1);
    wake_up(&g2d->irq_wait);
    handled = true;
}
```

**Flujo correcto**:
1. **IRQ llega** → Handler ejecuta
2. **Deshabilitar rcq_cfg_finish_irq_en** inmediatamente
3. **Clear cfg_finish_irq** flag (W1C)
4. **Wake up** proceso esperando
5. **Return** del handler
6. Proceso continúa, lanza siguiente RCQ
7. `sunxi_g2d_rcq_start()` **re-habilita** rcq_cfg_finish_irq_en
8. Ciclo se repite correctamente (1 IRQ por operación)

### Fix MBUS Logging

**Cambio en `sunxi_g2d_setup_mbus_priority()`**:

Removido check de `of_device_is_compatible()` que impedía logging:

```c
/* Antes (v2.0.3) */
if (!of_device_is_compatible(g2d->dev->of_node, "allwinner,sun8i-t113-g2d"))
    return 0;  // ← Nunca se ejecutaba, faltaba en DTS

/* Ahora (v2.0.4) */
dev_info(g2d->dev, "Configuring MBUS priority for G2D master...\n");
// ← Siempre se ejecuta
```

**Ahora veremos**:
```
[  ] sunxi-g2d: Configuring MBUS priority for G2D master...
[  ] sunxi-g2d: MBUS_MAST_CFG0: before=0xXXXXXXXX after=0xC0XXXXXX (master3_prio=192)
```

## 📊 Cambios en Código

### Archivos Modificados

1. **drivers/gpu/sunxi-g2d/sunxi-g2d-main.c**
   
   a) `sunxi_g2d_irq()` (líneas ~607-625):
   ```c
   + union g2d_rcq_irq_ctl irq_ctl;
   + 
   + /* Disable IRQ first to prevent storm */
   + irq_ctl.dwval = g2d_read(g2d, G2D_RCQ_IRQ_CTL);
   + irq_ctl.bits.rcq_cfg_finish_irq_en = 0;
   + g2d_write(g2d, G2D_RCQ_IRQ_CTL, irq_ctl.dwval);
   + wmb();
   + 
     /* Clear RCQ config finish interrupt (W1C) */
     g2d_top->rcq_status.bits.cfg_finish_irq = 1;
   + wmb();
   ```
   
   b) `sunxi_g2d_setup_mbus_priority()` (líneas ~395-398):
   ```c
   - if (!of_device_is_compatible(...))
   -     return 0;
   + dev_info(g2d->dev, "Configuring MBUS priority for G2D master...\n");
   ```

### Archivos sin Cambios

- **sunxi-g2d-rcq.c**: Mantiene habilitación de cfg_finish_irq_en en `sunxi_g2d_rcq_start()`
- **sunxi-g2d-rcq.h**: Sin cambios

## 🧪 Procedimiento de Prueba

### En T113 (spuc@10.42.0.1)

```bash
# 1. Cargar v2.0.4
sudo rmmod sunxi-g2d || true
sudo insmod /home/spuc/sunxi-g2d-v2.0.4-irq-storm-fix.ko rcq_enable_bit=1
sudo dmesg -C

# 2. Ejecutar test
./demo-bouncing-ball &
sleep 5
pkill demo-bouncing-ball

# 3. Capturar logs
dmesg > /home/spuc/dmesg-v2.0.4.txt
cat /proc/interrupts | grep g2d
```

### Logs Esperados (ÉXITO)

```
[  ] sunxi-g2d: === G2D hardware enable ===
[  ] sunxi-g2d: Configuring MBUS priority for G2D master...
[  ] sunxi-g2d: MBUS_MAST_CFG0: before=0x00000000 after=0xC0000000 (master3_prio=192)
                                                   ^^
                                          Master 3 configurado correctamente

[  ] sunxi-g2d: FILLRECT_RCQ: 800x240 color=0xff004080
[  ] RCQ started: IRQ_CTL=0x00000040 (cfg_finish_irq_en=1)

[  ] sunxi-g2d: RCQ IRQ received (cfg_finish): status=0x00000104 (IRQ disabled)
                                                                  ^^^^^^^^^^^^^
                                                    IRQ deshabilitado en handler
[  ] sunxi-g2d: FILLRECT_RCQ completed via RCQ IRQ

[  ] sunxi-g2d: FILLRECT_RCQ: 800x240 color=0xff0080ff  ← Segunda operación
[  ] RCQ started: IRQ_CTL=0x00000040 (cfg_finish_irq_en=1)  ← Re-habilitado
[  ] sunxi-g2d: RCQ IRQ received (cfg_finish): status=0x00000104 (IRQ disabled)
[  ] sunxi-g2d: FILLRECT_RCQ completed via RCQ IRQ

/proc/interrupts:
253:        120          0    GICv2 121 Level     5410000.g2d
         ^^^^^^
    ~1 IRQ por operación (no storm!)
```

**Características de éxito**:
- ✅ Log "IRQ disabled" tras cada IRQ
- ✅ 1-2 IRQs por operación RCQ (no 40+)
- ✅ Sistema NO se cuelga
- ✅ `/proc/interrupts` count proporcional a operaciones (~100-200 para 60 frames)

### Logs de Fallo (si persiste storm)

```
[  ] RCQ IRQ received (cfg_finish): status=0x00000104 (IRQ disabled)
[  ] RCQ IRQ received (cfg_finish): status=0x00000104 (IRQ disabled)
[  ] RCQ IRQ received (cfg_finish): status=0x00000104 (IRQ disabled)
... (40+ veces)

/proc/interrupts:
253:      5000          0    GICv2 121 Level     5410000.g2d
         ^^^^^^
    IRQ count explosivo
```

**Si falla**: Puede que el clear del STATUS no funcione (W1C mal implementado en HW) o hay otro source de IRQ activo.

## 🔍 Debugging Adicional

### Si IRQ storm persiste

1. **Verificar que IRQ_CTL se deshabilitó**:
   ```c
   dev_info(g2d->dev, "RCQ_IRQ_CTL after disable: 0x%08x\n",
            g2d_read(g2d, G2D_RCQ_IRQ_CTL));
   // Debe ser 0x00000000 (todos los IRQ off)
   ```

2. **Verificar que STATUS se limpia**:
   ```c
   dev_info(g2d->dev, "RCQ_STATUS after clear: 0x%08x\n",
            g2d_read(g2d, G2D_RCQ_STATUS));
   // Debe ser 0x00000004 (cfg_finish_irq=0, solo bits de estado)
   ```

3. **Probar con task_end_irq en lugar de cfg_finish_irq**:
   - Modificar `sunxi_g2d_rcq_start()` para habilitar `task_end_irq_en`
   - Ver si ese IRQ tiene mejor comportamiento

4. **Usar MIXER_IRQ como alternativa**:
   - Deshabilitar completamente RCQ IRQ
   - Confiar solo en MIXER finish IRQ (más lento pero estable)

## 📚 Comparación de Versiones

| Versión | MBUS Quirk | IRQ Handler | Resultado |
|---------|------------|-------------|-----------|
| v2.0.2  | ❌ No      | cfg_finish check | ❌ Timeout (IRQ nunca llega) |
| v2.0.3  | ✅ Sí      | cfg_finish check | ⚠️ IRQ storm (loop infinito) |
| v2.0.4  | ✅ Sí      | cfg_finish + disable | ✅ Esperamos 1 IRQ/op |

## 🎯 Próximos Pasos

### Si v2.0.4 FUNCIONA
1. ✅ **Limpiar código**: Remover logs excesivos (dejar solo errores)
2. ✅ **Medir performance**: FPS de `demo-bouncing-ball`, latencia de operaciones
3. ✅ **Stress test**: Ejecutar por minutos, verificar estabilidad
4. ✅ **Preparar para upstream**: Clean up, split patches, submit RFC

### Si v2.0.4 FALLA
- Cambiar a `task_end_irq` en lugar de `cfg_finish_irq`
- Implementar polling en lugar de IRQ (último recurso)
- Revisar si hay errores de DMA (IOMMU faults, page faults)

---

**Archivo compilado**: `sunxi-g2d-v2.0.4-irq-storm-fix.ko` (49KB)  
**Estado**: Listo para prueba - esperamos operación estable sin cuelgues
