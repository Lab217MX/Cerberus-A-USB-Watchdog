# TODO — pantallas OLED pendientes

El rediseño de la GUI (`Software/Cerberus/GUI_Base.h`) ya cubre las pantallas
principales. Estos **mensajes/eventos todavía usan el renderer genérico**
`draw()` / `showStatus(texto, logo)` (título "Cerberus" + icono + texto centrado)
y **les falta una pantalla propia** en `GUI_Base.h`. Diseñarlas queda pendiente.

## Pantallas ya hechas (referencia)

- Home / self-test → `guiMainMenu`
- Mass storage detectado → `guiMassDetect`
- Menú de opciones (Info/Files/Wipe) → `guiMassMenu`
- Descriptores USB → `guiDescPage`
- Lista de archivos (analizador) → `guiFileList`
- Visor hex / magic bytes → `guiHexView`
- Confirmación de wipe → `guiWipe`
- USB Killer → `guiBadUsb`
- HID sospechoso / tecleo automático → `guiHidDetect`
- Sin dispositivo → `guiNoDevice`

## Pendientes de diseñar

### Actividad del disco emulado (lecturas/escrituras del host)
- [ ] **README leído** — `"README (R)"`
- [ ] **AUTORUN leído** — `"AUTORUN (R)"`
- [ ] **Escritura detectada** — `"WRITING"`
- [ ] **Borrado detectado** — `"DELETING"`

### Dispositivos conectados
- [ ] **HID conectado** (aviso de montaje, distinto de la alerta de amenaza) — `"HID Device"`
- [ ] **HID enviando datos** — `"HID Sending data"`
- [ ] **CDC conectado** — `"CDC Device"`

### Wipe (progreso / resultado)
- [ ] **Wipe en progreso** — `"WIPING DISK"`
- [ ] **Wipe completado** — `"WIPE DONE"`
- [ ] **Wipe con error** — `"WIPE ERROR"`

### Sistema
- [ ] **Self-test fallido** — `"Selftest: KO"` + `"Stopping..."`
- [ ] **Reset de contadores (cmd RESET)** — `"Reset OK"`
- [ ] **Reinicio (cmd REBOOT)** — `"REBOOT"`
- [ ] **Reinicio por BOOTSEL corto** — `"RESETTING"`
- [ ] **Conteo de eventos HID (BOOTSEL largo)** — `"HID Evt# N"` (texto dinámico)
- [ ] **Mensajes genéricos de la cola de callbacks del host** — `showStatus(msg.text)` (texto variable)

## Nota

Mientras existan estos mensajes en `draw()`/`showStatus`, **`resources.h` sigue
siendo necesario** (provee `LOGO_USB_CONNECTED` / `LOGO_ALERT_SYMBOL` /
`LOGO_EVIL_SYMBOL`, `LOGO_ASSETS` y los bitmaps `image_Lock_bits` /
`image_Quest_bits`). Al migrar todo lo anterior a `GUI_Base.h` (idealmente con un
`guiStatus(texto, icono)` genérico que reemplace a `draw()`), se podrá eliminar
`resources.h`. El único símbolo ya muerto hoy es `image_arrow_curved_left_up_bits`.
