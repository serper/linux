# Cambios RCQ v2.0.3 - MBUS Priority Quirk

## 📋 Resumen

**Versión**: v2.0.3-mbus-quirk  
**Fecha**: 1 noviembre 2025  
**Objetivo**: Solucionar timeout de RCQ agregando quirk de prioridad MBUS para master G2D

## 🔍 Diagnóstico

### Problema Raíz

Tras implementar v2.0.2 (cfg_finish_irq), el hardware sigue mostrando:

- ✅ RCQ se ejecuta (`STATUS=0x00000104` = cfg_finish_irq activo)
- ✅ G2D responde (lee/escribe registros correctamente)  
- ❌ **IRQ nunca se dispara** (`/proc/interrupts` count=0)
- ❌ **RCQ timeout** (100ms sin completar)

### Causa: Acceso DMA Bloqueado

El problema **NO** es la secuencia RCQ sino el **acceso MBUS**:

1. RCQ configura registros correctamente → `cfg_finish_irq` se activa
2. G2D intenta leer framebuffer via DMA a través de MBUS
3. **MBUS bloquea/retrasa peticiones** del master G2D (master 3)
4. G2D nunca completa operación → IRQ task_end/mixer nunca se dispara
5. Timeout tras 100ms

## 🎯 Solución Implementada

### Quirk MBUS para T113-S3

Configurar **directamente** el registro MBUS de prioridad durante `sunxi_g2d_hw_enable()`:

```c
#define MBUS_BASE       0x03102000
#define MBUS_MAST_CFG0  0x0010  /* Master 0-3 priority config */
#define G2D_MBUS_MASTER 3       /* G2D es master 3 per T113 manual */

/* Configurar prioridad de G2D (master 3) */
cfg = readl(MBUS_BASE + MBUS_MAST_CFG0);
cfg &= ~(0xFF << 24);           /* Clear master 3 bits */
cfg |= (0xC0 << 24);            /* Set priority = 0xC0 (high) */
writel(cfg, MBUS_BASE + MBUS_MAST_CFG0);
```

### MBUS_MAST_CFG0 Register Layout

```
Offset: 0x0010 desde base MBUS (0x03102000)

Bits [31:24]: Master 3 priority  ← G2D
Bits [23:16]: Master 2 priority
Bits [15:8]:  Master 1 priority
Bits [7:0]:   Master 0 priority

Priority values:
  0x00 = Lowest priority (DMA blocked indefinitely)
  0x80 = Medium priority
  0xC0 = High priority (chosen for G2D)
  0xFF = Highest priority (may starve other masters)
```

### Integración en Driver

Nueva función agregada a `sunxi-g2d-main.c`:

```c
static int sunxi_g2d_setup_mbus_priority(struct sunxi_g2d_dev *g2d)
{
    /* Solo para T113-S3 */
    if (!of_device_is_compatible(..., "allwinner,sun8i-t113-g2d"))
        return 0;
    
    /* ioremap MBUS registers */
    mbus = ioremap(MBUS_BASE, SZ_4K);
    
    /* Set G2D master 3 priority to 0xC0 */
    cfg = readl(mbus + MBUS_MAST_CFG0);
    cfg &= ~(0xFF << 24);
    cfg |= (0xC0 << 24);
    writel(cfg, mbus + MBUS_MAST_CFG0);
    
    dev_info(g2d->dev, "MBUS_MAST_CFG0: before=0x%08x after=0x%08x\n", ...);
    
    iounmap(mbus);
    return 0;
}
```

Llamada desde `sunxi_g2d_hw_enable()` en **Step 4.5** (tras configurar bandwidth MBUS).

## 📊 Cambios en Código

### Archivos Modificados

1. **drivers/gpu/sunxi-g2d/sunxi-g2d-main.c**
   - Nueva función: `sunxi_g2d_setup_mbus_priority()` (líneas ~370-415)
   - Llamada en `sunxi_g2d_hw_enable()` (Step 4.5)
   - Logging: `"MBUS_MAST_CFG0: before=0x... after=0x... (master3_prio=192)"`

### Archivos sin Cambios (mantienen v2.0.2)

- **drivers/gpu/sunxi-g2d/sunxi-g2d-rcq.c**: cfg_finish_irq habilitado
- **drivers/gpu/sunxi-g2d/sunxi-g2d-rcq.h**: Firma de funciones
- **IRQ handler en main.c**: Sigue chequeando cfg_finish_irq primero

## 🧪 Procedimiento de Prueba

### En T113 Device (spuc@10.42.0.1)

```bash
# 1. Cargar módulo v2.0.3
sudo rmmod sunxi-g2d || true
sudo insmod /home/spuc/sunxi-g2d-v2.0.3-mbus-quirk.ko rcq_enable_bit=1
sudo dmesg -C

# 2. Ejecutar test
./demo-bouncing-ball &
sleep 3
pkill demo-bouncing-ball

# 3. Capturar logs
dmesg > /home/spuc/dmesg-v2.0.3.txt
cat /proc/interrupts | grep g2d
```

### Logs Esperados (ÉXITO)

```
[ ] sunxi-g2d: G2D hardware enable (fillrect v1.0.0 sequence)
[ ] sunxi-g2d: Clocks: mod=300000000 bus=200000000 mbus=396000000
[ ] sunxi-g2d: MBUS_MAST_CFG0: before=0x00000000 after=0xC0000000 (master3_prio=192)
                                                   ^^
                                          Master 3 = 0xC0 (high priority)

[ ] sunxi-g2d: FILLRECT_RCQ: 800x240 color=0xff004080
[ ] RCQ started: IRQ_CTL=0x00000040 (cfg_finish_irq_en=1)
                               ^^
                      Bit 6 (cfg_finish_irq_en) enabled

[ ] sunxi-g2d: RCQ IRQ received (cfg_finish): status=0x00000104
                                                           ^^^
                                              cfg_finish_irq activo (bit 2)

[ ] sunxi-g2d: FILLRECT_RCQ completed via RCQ IRQ (100ms)

/proc/interrupts:
253:         42          0    GICv2 121 Level     5410000.g2d
         ^^^^^^^
    IRQ count > 0 (no timeout!)
```

### Logs de Fallo (si persiste)

```
[ ] sunxi-g2d: MBUS_MAST_CFG0: before=0xXXXXXXXX after=0xC0XXXXXX
[ ] RCQ started: IRQ_CTL=0x00000040 (cfg_finish_irq_en=1)
[ ] RCQ status after UPDATE: STATUS=0x00000104  ← cfg_finish activo
[ ] sunxi-g2d: Fillrect RCQ timeout: RCQ_STATUS=0x00000104
                                                          ^^
                                          RCQ completó pero IRQ no llegó

/proc/interrupts:
253:          0          0    GICv2 121 Level     5410000.g2d
         ^^^^^^^
    IRQ count = 0 (nunca se disparó)
```

**Si falla**: Problema puede ser routing de IRQ o timing (no prioridad MBUS).

## 🔧 Debugging Adicional

### Si IRQ sigue sin dispararse

1. **Verificar que MBUS_MAST_CFG0 se escribió**:
   ```c
   dev_info(g2d->dev, "MBUS master3 prio: %d\n", (cfg_after >> 24) & 0xFF);
   // Debe ser 192 (0xC0)
   ```

2. **Probar diferentes prioridades**:
   ```c
   cfg |= (0x80 << 24);   // Media
   cfg |= (0xF0 << 24);   // Muy alta
   cfg |= (0xFF << 24);   // Máxima (puede causar starvation)
   ```

3. **Verificar otros masters MBUS**:
   - Leer `MBUS_MAST_CFG0` antes del quirk
   - Ver si otros masters tienen prioridad muy alta que bloquea G2D

4. **Verificar IRQ routing en GIC**:
   - IRQ 253 = GIC SPI 121 (offset +32)
   - Verificar que esté habilitado en `/proc/interrupts`

## 📚 Referencias Técnicas

- **T113-S3 User Manual**: Tabla 3-16 (MBUS Master IDs)
- **MBUS Controller**: Base 0x03102000, config offset 0x0010
- **DTS**: `arch/arm/boot/dts/allwinner/sun8i-t113-spuc.dtsi`
  ```dts
  g2d: g2d@5410000 {
      interconnects = <&mbus 3>;  /* Master 3 */
  };
  ```
- **drivers/bus/da8xx-mstpri.c**: Ejemplo de configuración de bus priority
- **drivers/soc/sunxi/sunxi_mbus.c**: Quirks MBUS para otros SoCs Allwinner

## 🎯 Próximos Pasos

### Si v2.0.3 FUNCIONA
1. ✅ Refinar prioridad si es necesario (probar 0x80, 0xA0, 0xC0)
2. ✅ Agregar T113 a `drivers/soc/sunxi/sunxi_mbus.c` (medio plazo)
3. ✅ Implementar driver interconnect completo para D1/T113 (largo plazo)
4. ✅ Upstream patches

### Si v2.0.3 FALLA
- Investigar routing de IRQ 253 (GIC configuration)
- Verificar timing de RCQ (puede necesitar delays)
- Revisar si hay otros registros MBUS necesarios (QoS, bandwidth limits)
- Considerar usar MIXER_IRQ en lugar de RCQ_IRQ como fallback

---

**Archivo compilado**: `sunxi-g2d-v2.0.3-mbus-quirk.ko` (49KB)  
**Estado**: Listo para prueba en hardware T113-S3
