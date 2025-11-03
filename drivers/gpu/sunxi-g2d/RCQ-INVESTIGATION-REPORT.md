# Informe de Investigación: RCQ (Register Configuration Queue) en T113-S3 G2D

**Fecha**: 3 de noviembre de 2025  
**SoC**: Allwinner T113-S3  
**IP G2D**: Versión 0x0110 (RCQ v2)  
**Estado**: No funcional - Investigación suspendida

## Resumen Ejecutivo

Se investigó exhaustivamente la implementación de RCQ (Register Configuration Queue) para el acelerador G2D del T113-S3. A pesar de múltiples iteraciones y análisis profundo del BSP de Allwinner, **no se logró hacer funcionar el RCQ**. El modo legacy (escritura directa de registros) funciona perfectamente.

## Contexto Técnico

### ¿Qué es RCQ?

RCQ (Register Configuration Queue) es un mecanismo DMA que permite al G2D procesar listas de comandos de forma asíncrona:

1. **CPU prepara buffer**: Lista de comandos (headers + valores de registros)
2. **CPU dispara RCQ**: Escribe UPDATE=1 en RCQ_CTRL
3. **DMA procesa**: RCQ engine lee comandos y escribe registros
4. **MIXER ejecuta**: Motor gráfico procesa la operación
5. **IRQ notifica**: task_end_irq señala finalización

### Arquitectura del Buffer RCQ

```
RCQ Buffer Structure:
┌─────────────────────────────────────┐
│ Header 0 (16 bytes)                 │
│  - low_addr: dirección datos        │
│  - reg_size: tamaño en bytes        │
│  - reg_num: número de bloques       │
│  - reg_offset: offset del registro  │
├─────────────────────────────────────┤
│ Data Block 0 (N bytes)              │
│  - Valores de registros             │
├─────────────────────────────────────┤
│ Header 1...                         │
│ Data Block 1...                     │
└─────────────────────────────────────┘
```

## Cronología de la Investigación

### Fase 1: Implementación Inicial (v2.0.1 - v2.6.0)

**Objetivo**: Portar RCQ del BSP al driver mainline

**Acciones**:
- Análisis del código BSP en `patches/sunxi_g2d/BSP/g2d_rcq/`
- Implementación de `sunxi-g2d-rcq.c` con helpers para construir buffers
- Creación de ioctl `G2D_IOC_FILLRECT_RCQ` para testing

**Resultado**: RCQ procesa comandos (cfg_finish_irq se activa) pero MIXER nunca ejecuta

**Logs observados**:
```
RCQ_STATUS=0x00000104 (cfg_finish_irq=1, frame_cnt=1)
MIXER_CTL=0x00000000 (START=0)
MIXER_INT=0x00000000 (finish_irq=0)
```

**Conclusión Fase 1**: RCQ DMA funciona, pero falta trigger para MIXER

### Fase 2: Alineación con BSP Pattern (v2.8.0 - v2.8.2)

**Hipótesis**: Estamos usando un patrón incorrecto comparado con el BSP

**Descubrimiento clave del BSP** (`g2d_mixer.c:873-901`):
```c
#if G2D_MIXER_RCQ_USED == 1
    g2d_top_rcq_update_en(0);      // Clear UPDATE
    g2d_top_set_rcq_head(...);     // Set HEAD/LEN
    ... apply frames ...            // Build RCQ buffer
    g2d_top_rcq_irq_en(1);         // Enable task_end_irq
    g2d_top_rcq_update_en(1);      // Trigger RCQ execution
    // NO g2d_mixer_start() call!
#endif
```

**Patrón BSP identificado**: BSP NUNCA escribe MIXER_START cuando usa RCQ

**Cambios implementados**:
- **v2.8.0**: Eliminamos escritura manual de MIXER_START, solo UPDATE
- **v2.8.1**: Intentamos pre-armar MIXER_START antes de RCQ
- **v2.8.2**: Patrón de 2 fases: esperar cfg_finish → escribir MIXER_START → esperar finish

**Resultados**:
- v2.8.0: `MIXER_CTL=0x00000000` (nunca arranca)
- v2.8.1: `MIXER_CTL=0x80000000` (START stuck, no auto-clear, no ejecución)
- v2.8.2: `cfg_finish_irq` OK instantáneo, pero MIXER timeout

**Conclusión Fase 2**: El patrón de ejecución parece correcto, problema en configuración de registros

### Fase 3: Configuración de Registros (v2.8.3 - v2.8.4)

**Hipótesis**: Faltan registros críticos o configuración incorrecta

**Investigaciones**:

1. **scan_order en MIXER_CTL**:
   - BSP inicializa `scan_order = G2D_SM_TDLR` (probable=0)
   - Añadimos: `mixer_ctrl.bits.scan_order = 0`
   - Resultado: Sin cambio, timeout

2. **task_end_irq_en**:
   - BSP habilita `task_end_irq_en=1` ANTES de UPDATE
   - Descubrimos que `sunxi_g2d_rcq_start()` lo estaba deshabilitando
   - Añadimos parámetro `enable_irq` para preservar configuración
   - Resultado: IRQ habilitada correctamente (`RCQ_IRQ_CTL=0x00000010`) pero task_end_irq NUNCA dispara

**Análisis de registros RCQ generados**:
```
V0_ATTCTL = 0xff000111  (alpha=0xFF, fillcolor_en=1, EN=1)
V0_MBSIZE = 0x00ff00ff  (256x256)
V0_PITCH0 = 0x00000400  (1024 bytes)
BLD_EN_CTL = 0x00000100 (p0_en=1)
WB_ATT = 0x00000001     (EN=1)
```

**Comparación con BSP**:
- BSP tiene 7 bloques de registros (OVL_V×1, OVL_U×3, SCAL×1, BLD×1, WB×1)
- Nosotros tenemos 10 headers pero faltan OVL_U (3 blocks) y SCAL (1 block)
- No está claro si estos son necesarios para fillrect simple

**Conclusión Fase 3**: Configuración parece razonable pero falta algo que bloquea MIXER

### Fase 4: Configuración MBUS (Contexto)

**Cambio previo**: MBUS master ID cambiado de 3 → 9 basado en Tina Linux BSP

**Verificación**:
```bash
xxd /sys/firmware/devicetree/base/soc/g2d@5410000/interconnects
# Output: 0x00000027 0x00000009
# phandle=0x27 (MBUS), master_id=0x09 ✓
```

**Estado**: MBUS configurado correctamente, no es el problema

## Síntomas Consistentes

En todas las pruebas, el comportamiento fue idéntico:

```
✅ RCQ DMA funciona:
   - cfg_finish_irq se activa (instantáneo)
   - frame_cnt incrementa (0→1)
   - RCQ_STATUS=0x00000104

❌ MIXER no ejecuta:
   - task_end_irq NUNCA dispara
   - MIXER_CTL=0x00000000 o 0x80000000 (stuck)
   - MIXER_INT=0x00000000
   - finish_irq NUNCA se activa
   - Timeout después de 100ms
```

## Hipótesis sobre Causa Raíz

### Hipótesis 1: Registros Faltantes (Más Probable)
El BSP usa abstracción modular (`g2d_ovl_v`, `g2d_ovl_u`, `g2d_scal`, etc.) que oculta configuración crítica. Posibles candidatos:

- **OVL_U blocks**: 3 bloques de overlay UI que BSP siempre configura
- **SCAL block**: 1 bloque de scaler que BSP incluye
- **Registros globales MIXER**: Posible inicialización especial no documentada

### Hipótesis 2: Secuencia de Inicialización
El T113 puede requerir secuencia específica:
- Inicialización de módulos en orden particular
- Configuración global de MIXER antes del primer RCQ
- Estados de hardware que legacy mode configura implícitamente

### Hipótesis 3: Protección Hardware
El T113-S3 puede tener protección que impide:
- RCQ escribir en MIXER_CTL (observado: writes ignorados)
- MIXER ejecutar sin algún enable global
- Operaciones RCQ sin configuración previa específica

### Hipótesis 4: Documentación Incompleta
El manual del usuario del G2D IP 0x0110 puede estar incompleto:
- Registros no documentados requeridos para RCQ v2
- Diferencias específicas del T113 vs otros SoCs con mismo IP
- Quirks de implementación de Allwinner

## Intentos de Compilar BSP

Se intentó compilar el módulo BSP original para capturar traces:

**Obstáculos**:
1. Headers UAPI faltantes (`linux/g2d_driver.h`)
2. APIs de kernel 4.9 incompatibles con 6.1
3. Dependencias de ftrace específicas de Tina

**Copiado**: `g2d_driver.h` desde Tina Linux → kernel mainline

**Estado**: Compilación parcial, pero requiere portado extenso de APIs

## Código Implementado

### Archivos Creados/Modificados

1. **`drivers/gpu/sunxi-g2d/sunxi-g2d-rcq.c`** (nuevo):
   - Gestión de buffer RCQ en memoria DMA
   - Helpers para añadir bloques de comandos
   - Setup y control de hardware RCQ
   - ~360 líneas

2. **`drivers/gpu/sunxi-g2d/sunxi-g2d-rcq.h`** (nuevo):
   - Estructuras y constantes RCQ
   - API pública para otros módulos

3. **`drivers/gpu/sunxi-g2d/sunxi-g2d-main.c`**:
   - Ioctl `G2D_IOC_FILLRECT_RCQ` (líneas 1124-1430)
   - Construcción de buffer con 10 bloques de registros
   - Múltiples iteraciones de patrones de ejecución (v2.8.0 - v2.8.4)

4. **`test-fillrect-rcq.c`** (userspace):
   - Test standalone para RCQ path
   - Usa DMA-BUF heap para memoria
   - Verificación de resultados

### Registros Configurados en RCQ

Buffer generado (10 headers, 160 bytes totales):

| Offset | Registro | Contenido | Propósito |
|--------|----------|-----------|-----------|
| 0x0002810c | FILLCOLOR0 | 0xFF000000 | Color de relleno global |
| 0x00028800 | V0_ATTCTL | 0xFF000111 | Video layer 0 attributes |
| 0x00028400 | BLD_EN_CTL | 0x00000100 | Blender pipe enable |
| 0x00028420 | BLD_CH_ISIZE0 | size | Blender pipe 0 size |
| 0x00028430 | BLD_CH_OFFSET0 | 0x00000000 | Blender pipe 0 offset |
| 0x00028440 | BLD_SIZE | size | Blender output size |
| 0x00028480 | BLD_CTL | 0x000000CC | Blender control |
| 0x00028460 | BLD_OUT_COLOR | 0x00000002 | Output color mode |
| 0x0002b000 | WB_ATT | 0x00000001 | Writeback attributes |
| 0x00028104 | MIXER_INT | 0x00000010 | Interrupt enable |

## Comparación: Legacy vs RCQ

### Modo Legacy (FUNCIONA ✅)

```c
// 1. Reset MIXER
g2d_write(G2D_AHB_RESET, 0x0);
g2d_write(G2D_AHB_RESET, 0x3);

// 2. Configurar V0, BLD, WB
g2d_write(V0_ATTCTL, ...);
g2d_write(BLD_EN_CTL, ...);
g2d_write(WB_ATT, ...);
// ... más registros ...

// 3. Iniciar
g2d_write(MIXER_CTL, START=1);

// 4. Esperar IRQ
wait_for_completion(&mixer_irq);
```

**Tiempo**: ~100-500 µs  
**IRQs**: MIXER finish_irq  
**Resultado**: ✅ Funciona perfectamente

### Modo RCQ (NO FUNCIONA ❌)

```c
// 1. Reset MIXER
g2d_write(G2D_AHB_RESET, 0x0);
g2d_write(G2D_AHB_RESET, 0x3);

// 2. Construir buffer RCQ
sunxi_g2d_rcq_add_block(V0_ATTCTL, ...);
sunxi_g2d_rcq_add_block(BLD_EN_CTL, ...);
// ... 10 bloques ...

// 3. Setup RCQ hardware
g2d_write(RCQ_HEAD_LOW, buffer_phys);
g2d_write(RCQ_HEAD_LEN, buffer_size);

// 4. Habilitar IRQ y disparar
g2d_write(RCQ_IRQ_CTL, task_end_irq_en=1);
g2d_write(RCQ_CTRL, UPDATE=1);

// 5. Esperar IRQ
wait_event_timeout(task_end_irq);  // ⏱️ TIMEOUT!
```

**Tiempo**: ❌ Timeout 100ms  
**IRQs observadas**: cfg_finish_irq (no task_end_irq)  
**Resultado**: ❌ MIXER nunca ejecuta

## Evidencia de Hardware Funcional

El RCQ DMA engine SÍ funciona:

1. **cfg_finish_irq se activa**: RCQ procesó los headers
2. **frame_cnt incrementa**: DMA leyó el buffer completo
3. **No hay errores de bus**: Dirección física correcta, acceso exitoso

El problema está en el paso siguiente: **MIXER no ejecuta después de RCQ**

## Recomendaciones Futuras

### Opción 1: Soporte Vendor (Recomendado)
- Contactar Allwinner para documentación técnica del G2D IP 0x0110
- Preguntar en foros linux-sunxi sobre T113 RCQ
- Comparar con implementaciones de R329/D1 (mismo IP)

### Opción 2: Reverse Engineering
- Arrancar Tina Linux con BSP kernel
- Usar ftrace/debugfs para capturar secuencia RCQ
- Comparar traces con nuestra implementación
- Analizar diferencias en registros escritos

### Opción 3: Análisis Hardware
- Usar analizador lógico en bus MBUS
- Capturar transacciones DMA durante RCQ
- Verificar si MIXER recibe configuración correcta

### Opción 4: Enfoque Pragmático (Actual)
- Mantener modo legacy funcional
- Documentar limitaciones RCQ
- Marcar RCQ como "experimental/no soportado"
- Revisitar cuando haya más información disponible

## Archivos de Referencia

### BSP Analizados
```
patches/sunxi_g2d/BSP/g2d_rcq/
├── g2d.c              # Driver principal RCQ
├── g2d_mixer.c        # Patrón de ejecución (líneas 873-901)
├── g2d_top.c          # Control RCQ hardware
├── g2d_ovl_v.c        # Video overlay
├── g2d_ovl_u.c        # UI overlay (×3 blocks)
├── g2d_scal.c         # Scaler
├── g2d_bld.c          # Blender
└── g2d_wb.c           # Writeback
```

### Nuestro Código
```
drivers/gpu/sunxi-g2d/
├── sunxi-g2d-main.c          # Ioctl RCQ, v2.8.0-v2.8.4
├── sunxi-g2d-rcq.c           # Gestión buffer RCQ
├── sunxi-g2d-rcq.h           # API RCQ
├── sunxi-g2d-structs.h       # Estructuras de registros
└── RCQ-INVESTIGATION-REPORT.md  # Este documento

test-fillrect-rcq.c            # Test userspace
```

## Conclusión

A pesar de una investigación exhaustiva y múltiples iteraciones:

1. ✅ **RCQ DMA funciona**: Procesa comandos correctamente
2. ✅ **Configuración correcta**: MBUS, interrupts, buffer format
3. ❌ **MIXER no ejecuta**: Falta algo crítico para trigger
4. ❓ **Causa desconocida**: Hardware protection, init sequence, o registros faltantes

**Decisión**: Usar modo legacy (funcional) y dejar RCQ para investigación futura con más recursos/documentación.

---

**Lecciones Aprendidas**:
- El BSP usa abstracción que oculta detalles críticos
- Documentación pública del G2D IP es incompleta
- Algunas funcionalidades requieren soporte vendor
- El modo legacy es suficiente para uso productivo

**Tiempo invertido**: ~6-8 horas de investigación y desarrollo

**Estado final**: RCQ deshabilitado, código comentado, modo legacy activo
