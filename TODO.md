# TODO — pantallas OLED

Todas las pantallas de mensajes/eventos ya tienen una vista propia en
`Software/Cerberus/GUI_Base.h` y están **cableadas** a sus eventos. El renderer
genérico heredado (`draw()` + `resources.h`) fue eliminado: `GUI_Base.h` es la
única fuente de pantallas.

## Pantallas (todas ✅)

### Principales
- [x] Home / self-test → `guiMainMenu`
- [x] Mass storage detectado → `guiMassDetect`
- [x] Menú de opciones (Info/Files/Wipe) → `guiMassMenu`
- [x] Descriptores USB → `guiDescPage`
- [x] Lista de archivos (analizador) → `guiFileList`
- [x] Visor hex / magic bytes → `guiHexView`
- [x] Confirmación de wipe → `guiWipe`
- [x] USB Killer → `guiBadUsb`
- [x] Sin dispositivo → `guiNoDevice`

### Actividad del disco emulado
- [x] README leído → `guiReadmeRead`
- [x] AUTORUN leído → `guiAutorunRead`
- [x] Escritura detectada → `guiWriting`
- [x] Borrado detectado → `guiDeleting`

### Dispositivos conectados
- [x] HID conectado (montaje) → `guiHidMount` (bandera core1→core0)
- [x] HID enviando datos / tecleo automático → `guiHidSendData`
- [x] Dispositivo sospechoso (VID/PID) → `guiHidMount`
- [x] CDC conectado → `guiCdcDevice` (bandera core1→core0)

### Wipe
- [x] Wipe en progreso → `guiWipingProgress`
- [x] Wipe completado → `guiWipingDone`
- [x] Wipe con error → `guiWipingError`

### Sistema
- [x] Self-test fallido → `guiBadSelftest`
- [x] Reset de contadores (RESET) → `guiGenericMsg("Reset OK")`
- [x] Reinicio (REBOOT) → `guiReboot`
- [x] Reinicio por BOOTSEL corto → `guiResetting`
- [x] Conteo de eventos HID (BOOTSEL largo) → `guiHidEvents(n)`
- [x] Mensajes genéricos de la cola del host → `guiGenericMsg(texto)`

## Limpieza

- [x] **`resources.h` eliminado.** Se quitó el renderer viejo `draw()` y el
  sistema de logos (`LOGO_ASSETS` / `image_Lock_bits` / `image_Quest_bits`). Sus
  usos se reemplazaron por pantallas de `GUI_Base.h`: splash de boot →
  `guiMainMenu("Booting...")`, fallback de `redraw_resting()` → `guiGenericMsg`,
  y `showStatus()` quedó como wrapper fino sobre `guiGenericMsg`.

## Notas de diseño

- Los callbacks de core1 (`tuh_hid_mount_cb`, `tuh_cdc_mount_cb`,
  `tuh_msc_mount_cb`) **no dibujan**: solo levantan una bandera y core0 pinta en
  `loop()`, para evitar la carrera de I2C entre cores.
- Varias pantallas de estado muestran "By: Lab217" vía `guiByline()`.
