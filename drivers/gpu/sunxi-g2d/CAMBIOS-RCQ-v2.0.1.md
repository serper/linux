# Cambios en Driver sunxi-g2d v2.0.1 - Correcciones RCQ

## Fecha: 1 de noviembre de 2025
## Autor: Sergio Perez

---

## RESUMEN DE CORRECCIONES

Basándose en el análisis exhaustivo del driver BSP de Allwinner y el informe de investigación T113-G2D-MAINLINE-INVESTIGATION-REPORT.md, se han implementado las siguientes correcciones críticas para alinear el driver con el comportamiento exacto del BSP.

---

## CAMBIOS IMPLEMENTADOS

### 1. ✅ Eliminación de MIXER_CTL.start manual en modo RCQ

**Archivo:** `sunxi-g2d-main.c`  
**Función:** `sunxi_g2d_do_blit_alpha_rcq()`  
**Línea:** ~1572

**ANTES:**
```c
/* Clear and enable MIXER interrupts */
g2d_write(g2d, G2D_MIXER_INT, 0x00000000);
udelay(1);
g2d_write(g2d, G2D_MIXER_INT, 0x00000011);

/* Setup RCQ hardware and start execution */
sunxi_g2d_rcq_setup_hw(g2d->base, &g2d->rcq);
sunxi_g2d_rcq_start(g2d->base);

dev_info(g2d->dev, "RCQ and MIXER started for alpha blending\n");

/* After RCQ updates registers, trigger MIXER processing */
udelay(10);  /* Give RCQ time to write registers */
g2d_write(g2d, G2D_MIXER_CTL, BIT(31));  /* START bit */  ⚠️ PROBLEMA
wmb();
```

**DESPUÉS:**
```c
/* Reset IRQ flag */
atomic_set(&g2d->irq_done, 0);

/* Setup RCQ hardware and start execution (BSP pattern: no MIXER_CTL in RCQ mode) */
sunxi_g2d_rcq_setup_hw(g2d->base, &g2d->rcq);
sunxi_g2d_rcq_start(g2d->base);

dev_info(g2d->dev, "RCQ started for alpha blending (waiting for task_end IRQ)\n");
```

**Razón:** El BSP **nunca** escribe MIXER_CTL.start cuando usa RCQ. El trigger es únicamente `RCQ_CTRL.update`. La escritura manual de MIXER_CTL podía causar conflictos con el mecanismo RCQ.

---

### 2. ✅ Ajuste de secuencia de inicio RCQ según BSP

**Archivo:** `sunxi-g2d-rcq.c`  
**Función:** `sunxi_g2d_rcq_start()`

**ANTES:**
```c
/* Enable RCQ mode and task end interrupt */
irq_ctl.dwval = readl(base + G2D_RCQ_IRQ_CTL);
irq_ctl.bits.rcq_sel = 1;          /* Enable RCQ mode */  ⚠️ BSP NO lo hace
irq_ctl.bits.task_end_irq_en = 1;
writel(irq_ctl.dwval, base + G2D_RCQ_IRQ_CTL);
```

**DESPUÉS:**
```c
/* Enable task end interrupt ONLY - BSP pattern (NO rcq_sel) */
irq_ctl.dwval = readl(base + G2D_RCQ_IRQ_CTL);
irq_ctl.bits.task_end_irq_en = 1;  /* Enable task end IRQ */
/* BSP does NOT set rcq_sel - leave it as is */
writel(irq_ctl.dwval, base + G2D_RCQ_IRQ_CTL);
```

**Razón:** El análisis del BSP (`g2d_rcq/g2d_top.c:115`) muestra que **solo** configura `task_end_irq_en`. No toca el bit `rcq_sel`. Este bit podría ser configurado por hardware o por el bootloader.

---

### 3. ✅ Verificación de G2D_CMD_CTL

**Archivo:** `sunxi-g2d-main.c`  
**Función:** `sunxi_g2d_hw_enable()`  
**Línea:** 419-420

**Estado:** Ya estaba implementado correctamente desde versión anterior:
```c
/* Configure CMD_CTL for DRAM command control (needed for RCQ DMA) */
g2d_write(g2d, G2D_CMD_CTL, 0x00010001);  /* Enable CORE0 and RT_WB */
wmb();
```

**Verificación agregada en RCQ setup:**
```c
pr_info("RCQ setup: CMD_CTL=0x%08x (should be 0x00010001 for DMA)\n",
        readl(base + G2D_CMD_CTL));
```

---

### 4. ✅ Mejora de logging para depuración

**Archivo:** `sunxi-g2d-rcq.c`  
**Funciones:** `sunxi_g2d_rcq_setup_hw()`, `sunxi_g2d_rcq_start()`

**Agregados:**
- Log de estado inicial de todos los registros RCQ
- Verificación de CMD_CTL durante setup
- Logs más descriptivos del estado post-UPDATE
- Eliminación de delay innecesario (udelay(100))

---

## CAMBIOS EN EL FLUJO DE EJECUCIÓN RCQ

### Patrón BSP (Ahora Implementado):

```
1. sunxi_g2d_rcq_setup_hw():
   - Deshabilita IRQ y UPDATE
   - Limpia flags de estado
   - Escribe HEAD_LOW, HEAD_HIGH, HEAD_LEN
   - NO toca rcq_sel

2. sunxi_g2d_rcq_start():
   - Limpia flags de estado (W1C)
   - Habilita task_end_irq_en SOLAMENTE
   - Escribe CTRL.en=1 + CTRL.update=1
   - NO escribe MIXER_CTL

3. Hardware RCQ:
   - Fetches headers via DMA
   - Escribe registros a offset+0x28000
   - Ejecuta MIXER internamente
   - Genera IRQ cuando termina
```

### Diferencias con Patrón Anterior:

| Aspecto | Antes | Ahora (BSP) |
|---------|-------|-------------|
| **rcq_sel** | Se configuraba = 1 | NO se toca |
| **MIXER_CTL** | Se escribía manualmente | NO se escribe |
| **MIXER_INT** | Se configuraba | NO en modo RCQ |
| **Delay** | udelay(100) | Eliminado |

---

## EXPERIMENTACIÓN: Bit RCQ_CTRL.EN

**Estado actual:** El bit `RCQ_CTRL.EN` (bit 4) se configura a 1, basándose en:

```c
ctrl.bits.en = 1;       /* Enable RCQ (T113 v2 - experimental) */
ctrl.bits.update = 1;   /* Trigger RCQ execution */
```

**Nota:** Este bit **no está documentado públicamente**. El BSP no lo menciona explícitamente en `g2d_top_rcq_update_en()`, que solo toca el bit `update`. Sin embargo, algunas versiones del BSP parecen usarlo.

**Próximos pasos de experimentación:**
1. Probar con `en=1` (actual)
2. Si falla, probar con `en=0` (solo UPDATE)
3. Capturar logs de ambos casos

---

## ARCHIVOS MODIFICADOS

```
drivers/gpu/sunxi-g2d/
├── sunxi-g2d-main.c     - Eliminación MIXER_CTL manual en RCQ
├── sunxi-g2d-rcq.c      - Ajuste secuencia inicio RCQ
└── CAMBIOS-RCQ-v2.0.1.md  - Este documento
```

---

## TESTING RECOMENDADO

### 1. Compilar driver:
```bash
cd /home/sergio/source/t113/kernel-t113/drivers/gpu/sunxi-g2d
make clean
make
```

### 2. Cargar módulo:
```bash
sudo rmmod sunxi_g2d 2>/dev/null
sudo insmod sunxi-g2d-main.ko
dmesg | tail -50
```

### 3. Ejecutar test fillrect RCQ:
```bash
cd /home/sergio/source/t113/kernel-t113
sudo ./test-dmaheap-fillrect
```

### 4. Analizar logs:
```bash
dmesg | grep -E "RCQ|G2D|CMD_CTL|IRQ"
```

**Buscar en logs:**
- ✅ "CMD_CTL=0x00010001" (debe aparecer)
- ✅ "RCQ started" (sin MIXER_CTL después)
- ⚠️ "RCQ status after UPDATE" - verificar si STATUS cambia de 0x00000000
- ⚠️ "RCQ IRQ received" o timeout

---

## POSIBLES RESULTADOS

### Caso 1: ÉXITO (Esperado)
```
[   XX.XXX] RCQ setup: CMD_CTL=0x00010001 (should be 0x00010001 for DMA)
[   XX.XXX] RCQ started: IRQ_CTL=0x00000010 (task_end_irq_en=1)
[   XX.XXX] RCQ status after UPDATE: CTRL=0x00000010 STATUS=0x00000001
[   XX.XXX] RCQ IRQ received: status=0x00000001
[   XX.XXX] FILLRECT_RCQ completed via RCQ IRQ
```

### Caso 2: Fallo RCQ_CTRL.EN
Si STATUS sigue en 0x00000000, probar variante sin EN bit:

**Modificar en `sunxi-g2d-rcq.c:276`:**
```c
ctrl.bits.en = 0;       /* Disable EN bit - try UPDATE only */
ctrl.bits.update = 1;
```

### Caso 3: Fallo persistente
- Verificar que headers RCQ se construyen correctamente (dump hex)
- Comparar con BSP kernel en hardware real
- Solicitar ayuda en linux-sunxi mailing list con logs completos

---

## REFERENCIAS

- **Informe de investigación:** `T113-G2D-MAINLINE-INVESTIGATION-REPORT.md`
- **BSP Source:** `patches/sunxi_g2d/BSP/g2d_rcq/`
- **Funciones clave BSP:**
  - `g2d_top_rcq_irq_en()` - Solo task_end_irq_en
  - `g2d_top_rcq_update_en()` - Solo update bit
  - `g2d_mixer_apply()` - Patrón completo de ejecución

---

## PRÓXIMOS PASOS SI FUNCIONA

1. ✅ Verificar fillrect RCQ
2. ✅ Verificar alpha blending RCQ
3. ✅ Agregar modo RCQ a todas las operaciones
4. ✅ Optimizar: reutilizar buffer RCQ entre operaciones
5. ✅ Implementar MIXER direct mode como fallback
6. ✅ Agregar soporte para múltiples frames en RCQ
7. ✅ Documentar hallazgos para comunidad linux-sunxi

---

## CAMBIOS EXPERIMENTALES ADICIONALES (POST-COMPILACIÓN)

### 5. ✅ Parámetro de módulo para bit RCQ_CTRL.EN (experimental)

**Archivo:** `sunxi-g2d-main.c`  
**Líneas:** 51-57

**Agregados:**
```c
static bool rcq_enable_bit = true;
module_param(rcq_enable_bit, bool, 0644);
MODULE_PARM_DESC(rcq_enable_bit, 
    "Set RCQ_CTRL.EN bit during RCQ start (default=1, try 0 if RCQ fails)");

static bool rcq_use_mixer_irq = false;
module_param(rcq_use_mixer_irq, bool, 0644);
MODULE_PARM_DESC(rcq_use_mixer_irq,
    "Use MIXER IRQ instead of RCQ IRQ (experimental, default=0)");
```

**Modificación de firma:**
`sunxi_g2d_rcq.h` y `sunxi_g2d_rcq.c`:
```c
void sunxi_g2d_rcq_start(void __iomem *base, bool use_en_bit);
```

**Call sites actualizados:**
- `sunxi_g2d_do_fillrect_rcq()` - línea 995
- `sunxi_g2d_do_blit_alpha_rcq()` - línea 1571

**Razón:** El bit RCQ_CTRL.EN (bit 4) no está documentado en el datasheet. Algunas versiones del BSP pueden usarlo, otras no. Este parámetro permite probar ambas configuraciones:

```bash
# Con EN bit (configuración por defecto - T113 v2 podría necesitarlo)
insmod sunxi-g2d.ko

# Sin EN bit (patrón estricto del BSP analizado)
insmod sunxi-g2d.ko rcq_enable_bit=0
```

**Estado de implementación:**
- ✅ Header actualizado con nuevo parámetro
- ✅ Implementación en sunxi-g2d-rcq.c actualizada
- ✅ Dos call sites actualizados para pasar parámetro
- ✅ Compilación exitosa (sunxi-g2d.ko generado)

---

## COMPILACIÓN

**Comando usado:**
```bash
make O=/home/sergio/build/kernel-t113-out ARCH=arm \
     CROSS_COMPILE=arm-linux-musleabihf- M=drivers/gpu/sunxi-g2d
```

**Resultado:**
```
✅ sunxi-g2d.ko compilado exitosamente
⚠️ Advertencias: funciones no usadas (versiones sin RCQ - OK)
```

**Salida del módulo:**
```
/home/sergio/build/kernel-t113-out/drivers/gpu/sunxi-g2d/sunxi-g2d.ko
```

---

## PRUEBAS PENDIENTES

### Prueba 1: Con EN bit (configuración por defecto)
```bash
insmod sunxi-g2d.ko
./test-rcq-v2.0.1.sh
dmesg | grep -i "rcq\|g2d\|cmd_ctl"
```

### Prueba 2: Sin EN bit (si falla la anterior)
```bash
rmmod sunxi-g2d
insmod sunxi-g2d.ko rcq_enable_bit=0
./test-rcq-v2.0.1.sh
dmesg | grep -i "rcq\|g2d\|cmd_ctl"
```

### Logs esperados:
```
[  X.XXX] sunxi-g2d: CMD_CTL=0x00010001 ✅
[  X.XXX] RCQ setup: CMD_CTL=0x00010001 (should be 0x00010001 for DMA) ✅
[  X.XXX] RCQ writing CTRL=0xXXXXXXXX (en=X update=1 use_en_bit=X)
[  X.XXX] RCQ started: IRQ_CTL=0xXXXXXXXX (task_end_irq_en=1)
[  X.XXX] RCQ status after UPDATE: CTRL=0xXXXXXXXX STATUS=0xXXXXXXXX
[  X.XXX] RCQ IRQ received: STATUS=0xXXXXXXXX ✅ (SI FUNCIONA)
```

Si aún falla, verificar en dmesg:
- ¿STATUS cambia después de UPDATE? (debería != 0x00000000)
- ¿Llega alguna IRQ (MIXER, ROT, o RCQ)?

---

## CONTACTO

**Autor:** Sergio Perez (serper)  
**Repo:** https://github.com/serper/linux (branch: sunxi-g2d-m2m)  
**Fecha:** 1 de noviembre de 2025  
**Versión:** v2.0.1

---

**Estado:** ✅ COMPILADO - ⚠️ PENDIENTE DE PRUEBAS EN HARDWARE
