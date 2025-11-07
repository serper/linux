Demos y pruebas para el driver sunxi-g2d

Contenido:
- demos/: ejecutables y fuentes de demostración (demo-*-test, demo-bouncing-ball, etc.)
- tests/: scripts y utilidades para ejecutar pruebas (test-3buffer.sh)
- docs/: documentos y notas relacionadas con RCQ y pruebas

Uso rápido
1. Compila el módulo del driver desde la raíz del árbol del kernel:

   make drivers/gpu/sunxi-g2d/sunxi-g2d.ko

2. Inserta el módulo (con permisos apropiados):

   sudo insmod drivers/gpu/sunxi-g2d/sunxi-g2d.ko

3. Ejecuta un demo (ejemplo):

   # ejecutable local
   patches/demo-g2d/demos/demo-3buffer-test

   # o compilar y ejecutar la fuente en el host si es necesario
   gcc -O2 -o demo demo-bouncing-ball.c && ./demo

Notas
- Estos demo y tests se han movido fuera del árbol principal del driver a
  `patches/demo-g2d` para mantener el directorio del driver limpio.
- Los binarios están incluidos como referencia; si prefieres solo las fuentes,
  indícalo y los dejaré fuera del repositorio.

Licencia y autores
- Mantener la misma licencia que el proyecto principal.
