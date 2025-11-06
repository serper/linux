#!/bin/bash
# Script para probar fillrect con RCQ en la board
# Ejecutar en la board T113-S3

set -e

echo "=== Test FILLRECT_RCQ en T113-S3 ==="
echo ""

# Verificar que el módulo está cargado
if ! lsmod | grep -q sunxi_g2d; then
    echo "❌ Módulo sunxi-g2d no está cargado"
    echo "   Ejecuta: sudo insmod /tmp/sunxi-g2d.ko rcq_enable_bit=1"
    exit 1
fi

echo "✓ Módulo sunxi-g2d cargado"

# Verificar dispositivo G2D
if [ ! -c /dev/g2d ]; then
    echo "❌ /dev/g2d no existe"
    exit 1
fi

echo "✓ /dev/g2d existe"

# Verificar dma_heap
if [ ! -d /dev/dma_heap ]; then
    echo "❌ /dev/dma_heap no existe"
    exit 1
fi

echo "✓ /dev/dma_heap disponible"

# Ejecutar test
echo ""
echo "🚀 Ejecutando test-fillrect-rcq..."
echo ""

# Limpiar dmesg previo (opcional)
if [ "$1" = "--clear-dmesg" ]; then
    sudo dmesg -C
    echo "✓ dmesg limpiado"
fi

# Ejecutar test (3 operaciones por defecto, o pasar número como argumento)
NUM_TESTS="${1:-3}"

sudo ./test-fillrect-rcq "$NUM_TESTS"
TEST_RET=$?

echo ""
echo "=== dmesg reciente (últimas 100 líneas con filtro RCQ/fillrect) ==="
dmesg | tail -n 100 | grep -E 'FILLRECT_RCQ|RCQ|fillrect_rcq|fence_create|irq:|installing fd' || true

echo ""
if [ $TEST_RET -eq 0 ]; then
    echo "✅ Test completado exitosamente"
else
    echo "❌ Test falló con código: $TEST_RET"
fi

exit $TEST_RET
