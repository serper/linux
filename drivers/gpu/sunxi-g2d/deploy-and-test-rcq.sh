#!/bin/bash
# Script para compilar, desplegar y ejecutar test-fillrect-rcq
# Ejecutar desde el directorio drivers/gpu/sunxi-g2d/

set -e

BOARD_HOST="spuc@10.42.0.1"
KERNEL_OUT="/home/sergio/build/kernel-t113-out"
KERNEL_SRC="/home/sergio/source/t113/kernel-t113"

echo "=== Compilar y probar FILLRECT_RCQ en T113-S3 ==="
echo ""

# Paso 1: Compilar test userspace
echo "📦 Compilando test-fillrect-rcq..."
make -f Makefile.tests clean
make -f Makefile.tests test-fillrect-rcq
echo "✓ Test compilado"
echo ""

# Paso 2: Verificar si módulo kernel está actualizado
echo "⚙️  Verificando módulo kernel..."
if [ ! -f "$KERNEL_OUT/drivers/gpu/sunxi-g2d/sunxi-g2d.ko" ]; then
    echo "⚠️  Módulo no encontrado en $KERNEL_OUT"
    echo "   Compilando módulo kernel..."
    cd "$KERNEL_SRC"
    make O="$KERNEL_OUT" ARCH=arm CROSS_COMPILE=arm-linux-musleabihf- -j$(nproc) M=drivers/gpu/sunxi-g2d modules
    cd -
    echo "✓ Módulo compilado"
else
    echo "✓ Módulo encontrado (si hiciste cambios, recompila manualmente)"
fi
echo ""

# Paso 3: Copiar a la board
echo "📤 Copiando archivos a la board..."
scp "$KERNEL_OUT/drivers/gpu/sunxi-g2d/sunxi-g2d.ko" "$BOARD_HOST:/tmp/" || {
    echo "❌ Error copiando módulo"
    exit 1
}
scp test-fillrect-rcq run-test-rcq.sh "$BOARD_HOST:/home/spuc/" || {
    echo "❌ Error copiando test"
    exit 1
}
echo "✓ Archivos copiados"
echo ""

# Paso 4: Ejecutar en la board
echo "🚀 Ejecutando test en la board..."
echo ""
ssh "$BOARD_HOST" << 'ENDSSH'
    set -e
    
    # Recargar módulo
    echo "  Recargando módulo sunxi-g2d..."
    sudo rmmod sunxi-g2d 2>/dev/null || true
    sleep 1
    sudo insmod /tmp/sunxi-g2d.ko rcq_enable_bit=1
    
    echo "  ✓ Módulo cargado"
    echo ""
    
    # Verificar módulo
    if ! lsmod | grep -q sunxi_g2d; then
        echo "  ❌ Módulo no se cargó correctamente"
        exit 1
    fi
    
    # Limpiar dmesg para captura limpia
    sudo dmesg -C
    
    # Ejecutar test
    echo "  Ejecutando test-fillrect-rcq (3 operaciones)..."
    echo ""
    
    cd /home/spuc
    chmod +x test-fillrect-rcq run-test-rcq.sh
    
    sudo ./test-fillrect-rcq 3
    TEST_RET=$?
    
    echo ""
    echo "=== dmesg reciente ==="
    dmesg | tail -n 80 | grep -E 'FILLRECT_RCQ|RCQ_CTRL|RCQ_STATUS|fillrect_rcq|fence_create|irq:|installing fd|about to signal' --color=always || echo "  (sin matches en dmesg)"
    
    echo ""
    if [ $TEST_RET -eq 0 ]; then
        echo "✅ Test completado exitosamente"
    else
        echo "❌ Test falló con código: $TEST_RET"
    fi
    
    exit $TEST_RET
ENDSSH

echo ""
echo "=== Resumen ==="
if [ $? -eq 0 ]; then
    echo "✅ Todo OK - RCQ fillrect funcionando"
else
    echo "❌ Falló - revisar logs arriba"
    echo ""
    echo "Para más diagnóstico, conectar a la board y ejecutar:"
    echo "  ssh $BOARD_HOST"
    echo "  dmesg | grep -E 'RCQ|g2d' | tail -n 100"
fi
