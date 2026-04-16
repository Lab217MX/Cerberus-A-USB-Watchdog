# CerberusZero

PCB principal del proyecto Cerberus USB Watchdog.

## Archivos

### CerberusZero_V1.epro

Proyecto editable del PCB.

- **Formato:** EasyEDA Pro (`.epro`)
- **Cómo abrir:** En EasyEDA Pro, ir a `Archivo → Abrir` y seleccionar el archivo

### CerberusZero.zip

Archivos Gerber listos para fabricación.

- **Contenido:** Gerbers, drill files, y demás archivos necesarios para producción
- **Uso:** Subir directamente al fabricante de PCBs

### PickAndPlace_CerberusZero.csv

Archivo de coordenadas para ensamblaje SMT automatizado.

- **Formato:** CSV con posiciones X/Y, rotación y designadores de componentes
- **Uso:** Requerido si el fabricante ensambla los componentes SMT

## Fabricación con PCBWay

1. Ir a [pcbway.com](https://www.pcbway.com/)
2. Seleccionar "Quote Now" o "PCB Instant Quote"
3. Subir el archivo `CerberusZero.zip`
4. Configurar las opciones del PCB según necesidad
5. Si requieres ensamblaje SMT, subir también el archivo `PickAndPlace_CerberusZero.csv` junto con el BOM
