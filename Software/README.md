# Software

Firmware y herramientas de software para el proyecto Cerberus.

---

## Estructura de archivos

```
Software/
├── Cerberus/
│   ├── Cerberus.ino    # Firmware principal
│   ├── ramdisk.h       # Imagen FAT del RAM Disk virtual
│   └── resources.h     # Iconos y recursos gráficos para OLED
├── cerberus_listener.py # Aplicación companion (GUI)
└── README.md
```

---

## Firmware (Cerberus/)

### Cerberus.ino

Firmware principal que implementa:

- **Emulación MSC**: Disco RAM virtual que detecta lecturas de README/AUTORUN y escrituras sospechosas
- **Neutralizador de particiones**: Borrado manual de las tablas MBR/GPT del USB montado en el puerto host (portado desde [USB-Neutralizer](https://github.com/novamostra/USB-Neutralizer)). Se dispara manteniendo pulsado el botón "Bak" ~3 s o con el comando serial `WIPE`; nunca de forma automática al montar
- **Host USB (PIO-USB)**: Monitoreo de dispositivos HID, Mass Storage y CDC
- **Detección de amenazas**: USB Killer, dispositivos sospechosos (base de datos VID/PID), tecleo automatizado (BadUSB)
- **Interfaz OLED**: GUI con iconos, estados y vista de descriptores USB
- **LED NeoPixel**: Indicador visual de estado por colores
- **Comandos serial**: Interface de texto para debugging y forense

### ramdisk.h

Imagen FAT predefinida con sectores fijos para README.txt y AUTORUN.INF. Permite detectar cuando el host intenta leer estos archivos.

### resources.h

Recursos gráficos (bitmaps) para la pantalla OLED: iconos de estado, alertas y logos.

---

## Pinout GPIO (RP2040)

| GPIO   | Función      | Notas                                                     |
| ------ | ------------ | --------------------------------------------------------- |
| GPIO0  | USB Host D+  | PIO-USB (HOST_PIN_DP)                                     |
| GPIO1  | USB Host D-  | PIO-USB (D+ + 1)                                          |
| GPIO3  | BTN_RST      | "Bak": corto = navegar descriptores; mantener ~3 s con USB montado = borrar tablas MBR/GPT (INPUT_PULLUP) |
| GPIO4  | I2C SDA      | Pantalla OLED SSD1306                                     |
| GPIO5  | I2C SCL      | Pantalla OLED SSD1306                                     |
| GPIO6  | BTN_OK       | Botón para salir de vista descriptores (INPUT_PULLUP)     |
| GPIO8  | KILLER_PIN   | Detección USB Killer (INPUT_PULLUP, interrupción FALLING) |
| GPIO16 | NEOPIXEL_PIN | LED RGB de estado (WS2812)                                |

---

## Dependencias (Arduino IDE)

| Librería                 | Versión mínima |
| ------------------------ | -------------- |
| Adafruit TinyUSB Library | >= 3.6.0       |
| Pico-PIO-USB             | >= 0.7.2       |
| Adafruit_GFX             | -              |
| Adafruit_SSD1306         | >= 2.5.14      |
| XxHash_arduino           | -              |
| Adafruit_NeoPixel        | -              |

**Board**: Raspberry Pi RP2040 (4.5.4)
**USB Stack**: Adafruit TinyUSB
**CPU Speed**: 133 MHz (se ajusta a 240 MHz en Core 1 para PIO-USB)

---

## Companion App (cerberus_listener.py)

![Preview de la app](companionapp.png)
Aplicación GUI en Python/Tkinter para monitoreo en tiempo real desde PC.

### Características

- **Conexión serial**: Auto-detección del dispositivo, reconexión automática
- **Log con filtros**: Colores por severidad, búsqueda, filtrado por categoría
- **Analizador de payloads**: Detecta patrones de ataque (GUI+R, powershell, etc.)
- **Modo Red Team**: Exportación de keystrokes a DuckyScript

### Requisitos

```bash
pip install pyserial
```

> `tkinter` viene incluido en instalaciones estándar de Python

### Uso

```bash
python Software/cerberus_listener.py
```

Si no auto-detecta el puerto, selecciónalo manualmente en el combo y pulsa "Conectar".

---

## Uso rápido

1. Abre `Software/Cerberus/Cerberus.ino` en Arduino IDE
2. Configura: `Tools → USB Stack → Adafruit TinyUSB`
3. Sube el firmware a la Pico
4. Conecta al PC y observa mensajes en la OLED y por `SerialTinyUSB`

---

## Créditos

- Emulación MSC y detección de amenazas basadas en [USBvalve](https://github.com/cecio/USBvalve) de [Cecio](https://github.com/cecio)
- Neutralizador de particiones (borrado de tablas MBR/GPT) portado desde [USB-Neutralizer](https://github.com/novamostra/USB-Neutralizer) de [novamostra](https://github.com/novamostra)
