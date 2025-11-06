#!/bin/bash
# Script de prueba para correcciones RCQ v2.0.1
# Autor: Sergio Perez
# Fecha: 1 noviembre 2025

set -e

DRIVER_DIR="/home/sergio/source/t113/kernel-t113/drivers/gpu/sunxi-g2d"
TEST_DIR="/home/sergio/source/t113/kernel-t113"

echo "==================================================================="
echo "  sunxi-g2d RCQ v2.0.1 - Script de Pruebas"
echo "==================================================================="
echo ""

# Función para logs con timestamp
log() {
    echo "[$(date +'%H:%M:%S')] $1"
}

# Verificar que estamos en el directorio correcto
if [ ! -d "$DRIVER_DIR" ]; then
    echo "ERROR: Directorio del driver no encontrado: $DRIVER_DIR"
    exit 1
fi

# Paso 1: Compilar driver
log "Paso 1: Compilando driver..."
cd "$DRIVER_DIR"
make clean > /dev/null 2>&1
if make; then
    log "✅ Compilación exitosa"
else
    log "❌ Error en compilación"
    exit 1
fi

# Paso 2: Descargar módulo anterior si existe
log "Paso 2: Descargando módulo anterior..."
if lsmod | grep -q sunxi_g2d; then
    sudo rmmod sunxi_g2d
    log "✅ Módulo anterior descargado"
else
    log "ℹ️  Módulo no estaba cargado"
fi

# Paso 3: Limpiar dmesg
log "Paso 3: Limpiando buffer dmesg..."
sudo dmesg -C

# Paso 4: Cargar nuevo módulo
log "Paso 4: Cargando módulo sunxi-g2d..."
if sudo insmod sunxi-g2d-main.ko; then
    log "✅ Módulo cargado"
else
    log "❌ Error al cargar módulo"
    exit 1
fi

# Dar tiempo al sistema para inicializar
sleep 1

# Paso 5: Verificar inicialización
log "Paso 5: Verificando inicialización del driver..."
echo ""
echo "--- Logs de inicialización ---"
dmesg | grep -E "G2D|g2d" | tail -20
echo "--- Fin logs inicialización ---"
echo ""

# Verificar CMD_CTL
if dmesg | grep -q "CMD_CTL=0x00010001"; then
    log "✅ CMD_CTL configurado correctamente"
else
    log "⚠️  CMD_CTL no encontrado en logs"
fi

# Verificar device
if [ -c /dev/g2d ]; then
    log "✅ Dispositivo /dev/g2d creado"
    ls -l /dev/g2d
else
    log "❌ Dispositivo /dev/g2d NO creado"
    exit 1
fi

# Paso 6: Ejecutar test fillrect
log "Paso 6: Ejecutando test fillrect..."
echo ""

cd "$TEST_DIR"

if [ -x ./test-dmaheap-fillrect ]; then
    log "Ejecutando test-dmaheap-fillrect..."
    if sudo ./test-dmaheap-fillrect; then
        log "✅ Test completado (verificar resultado)"
    else
        log "⚠️  Test terminó con error"
    fi
else
    log "⚠️  test-dmaheap-fillrect no encontrado o no ejecutable"
fi

# Paso 7: Analizar logs RCQ
log "Paso 7: Analizando logs RCQ..."
echo ""
echo "==================================================================="
echo "  LOGS RCQ DETALLADOS"
echo "==================================================================="
dmesg | grep -E "RCQ|CMD_CTL" --color=never
echo "==================================================================="
echo ""

# Paso 8: Verificar estado de IRQ
log "Paso 8: Verificando estado de interrupciones..."
if dmesg | grep -q "RCQ IRQ received"; then
    log "✅✅✅ ¡IRQ RCQ RECIBIDO! - Hardware respondió"
    echo ""
    echo "==================================================================="
    echo "  ¡¡¡ ÉXITO !!! RCQ FUNCIONANDO"
    echo "==================================================================="
elif dmesg | grep -q "RCQ timeout"; then
    log "❌ Timeout de RCQ - hardware no respondió"
    echo ""
    echo "Verificar en logs:"
    echo "  - RCQ status after UPDATE: ¿cambió de 0x00000000?"
    echo "  - RCQ CTRL: ¿bits en=1 update=1?"
    echo ""
    echo "Próximo paso: experimentar con en=0 (solo UPDATE)"
else
    log "ℹ️  No se ejecutó operación RCQ aún"
fi

# Paso 9: Resumen
echo ""
log "Paso 9: Resumen de resultados"
echo "==================================================================="
echo "Verificar manualmente:"
echo "  1. ¿CMD_CTL = 0x00010001? $(dmesg | grep -q 'CMD_CTL=0x00010001' && echo '✅' || echo '❌')"
echo "  2. ¿RCQ setup exitoso? $(dmesg | grep -q 'RCQ setup:' && echo '✅' || echo '❌')"
echo "  3. ¿RCQ started? $(dmesg | grep -q 'RCQ started:' && echo '✅' || echo '❌')"
echo "  4. ¿IRQ recibido? $(dmesg | grep -q 'RCQ IRQ received' && echo '✅' || echo '❌')"
echo "  5. ¿MIXER_CTL manual? $(dmesg | grep -q 'After RCQ.*MIXER' && echo '❌ (presente)' || echo '✅ (eliminado)')"
echo "==================================================================="
echo ""
log "Script completado. Revisar logs arriba para diagnóstico."
echo ""
