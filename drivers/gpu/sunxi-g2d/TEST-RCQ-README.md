# Test FILLRECT_RCQ - Instrucciones

Este directorio contiene un test para probar la nueva ruta RCQ de `fillrect` en el driver sunxi-g2d.

## Archivos creados

- `test-fillrect-rcq.c` - Programa de test que invoca `G2D_IOC_FILLRECT_RCQ`
- `Makefile.tests` - Makefile para compilar tests de usuario
- `run-test-rcq.sh` - Script de ayuda para ejecutar el test en la board

## Compilación

### Módulo kernel con nuevo ioctl

El módulo ya fue parcheado para incluir `G2D_IOC_FILLRECT_RCQ` (número 7). Compilar:

```bash
cd /home/sergio/source/t113/kernel-t113
make O=/home/sergio/build/kernel-t113-out ARCH=arm CROSS_COMPILE=arm-linux-musleabihf- -j$(nproc) modules
```

### Test userspace

```bash
cd drivers/gpu/sunxi-g2d
make -f Makefile.tests test-fillrect-rcq
```

## Despliegue a la board

```bash
# Copiar módulo
scp /home/sergio/build/kernel-t113-out/drivers/gpu/sunxi-g2d/sunxi-g2d.ko spuc@10.42.0.1:/tmp/

# Copiar test y script
cd drivers/gpu/sunxi-g2d
scp test-fillrect-rcq run-test-rcq.sh spuc@10.42.0.1:/home/spuc/
```

## Ejecución en la board

```bash
# Conectar a la board
ssh spuc@10.42.0.1

# Cargar módulo con RCQ habilitado
sudo rmmod sunxi-g2d || true
sudo insmod /tmp/sunxi-g2d.ko rcq_enable_bit=1

# Verificar carga
dmesg | tail -n 20

# Ejecutar test
chmod +x run-test-rcq.sh
./run-test-rcq.sh

# O ejecutar directamente (3 tests por defecto)
sudo ./test-fillrect-rcq

# Ejecutar con diferente número de tests
sudo ./test-fillrect-rcq 5
```

## Qué hace el test

El test `test-fillrect-rcq`:

1. Abre `/dev/g2d`
2. Obtiene versión del driver
3. Crea buffer DMA-BUF usando `/dev/dma_heap/system`
4. Ejecuta 3 operaciones fillrect usando `G2D_IOC_FILLRECT_RCQ`:
   - Rellena buffer completo (256x256) con rojo
   - Rellena rectángulo 128x128 con verde
   - Rellena rectángulo 64x64 con azul
5. Para cada operación:
   - Espera el fence retornado usando `poll()`
   - Verifica que los píxeles se rellenaron correctamente
6. Reporta éxito/fallo

## Qué buscar en dmesg

Después de ejecutar el test, revisar dmesg para:

```bash
dmesg | grep -E 'FILLRECT_RCQ|RCQ|fillrect_rcq|fence_create|irq:|installing fd'
```

Esperado:
- Mensajes `FILLRECT_RCQ ioctl:` con dimensiones y color
- Mensajes `fillrect_rcq: installing fd=...` con fence info
- Mensajes `RCQ` mostrando actividad del hardware
- Mensajes `irq:` cuando las operaciones completan
- Mensajes `fence signalled` confirmando que los fences se señalizan

## Diagnóstico

Si el test falla:

1. **Timeout en fence**: El fence no se señaliza
   - Revisar dmesg: ¿aparecen IRQs del G2D?
   - Revisar `/proc/interrupts | grep g2d`
   - Verificar RCQ_CTRL y RCQ_STATUS en dmesg

2. **Verificación de píxeles falla**: El hardware no ejecutó la operación
   - Revisar dmesg para errores de RCQ
   - Verificar que el buffer DMA-BUF se mapeó correctamente

3. **IOCTL falla**: Error al invocar `G2D_IOC_FILLRECT_RCQ`
   - Verificar que el módulo se compiló con el nuevo ioctl
   - Revisar dmesg para mensajes de error del driver

## Comparación con ruta legacy

Para comparar con la ruta legacy (sin RCQ):

```bash
# Ejecutar test con fillrect normal
sudo ./test-fillrect-simple
```

Esto permite confirmar si el problema es específico de RCQ o afecta ambas rutas.

## Próximos pasos si funciona

Si el test pasa:
1. Migrar otros ioctls (blit, alpha_blend) a usar RCQ
2. Actualizar demo para usar `G2D_IOC_FILLRECT_RCQ` opcionalmente
3. Medir rendimiento RCQ vs direct writes

## Próximos pasos si falla

Si el test falla:
1. Implementar watchdog para capturar estado RCQ cuando fence no se señaliza
2. Revisar configuración MBUS master (actualmente 9, antes era 3)
3. Ejecutar test-rcq-v2.0.1.sh para verificar RCQ básico
4. Considerar revertir cambio MBUS temporalmente para aislar problema
