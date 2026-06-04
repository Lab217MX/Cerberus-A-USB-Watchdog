# Cerberus

![Alpha V4 del proyecto](1.jpg)

<p align="center">
  <strong>Desarrollado por Lab217</strong><br>
  Ninguna USB entra sin permiso. NINGUNA
</p>

![Estado](https://img.shields.io/badge/status-En_desarollo-gree)
![License](https://img.shields.io/badge/license-GNU_AGPLv3-blue)

---

## Descripción

Este proyecto es un dispositivo compacto diseñado para proteger computadoras de ataques por USB, detectando lecturas o escrituras automáticas que podrían indicar malware, y diferenciando dispositivos maliciosos como Rubber Ducky o USB Killer. A diferencia de otras soluciones que solo cortan las líneas de datos, este dispositivo muestra en tiempo real la actividad USB, brindando visibilidad sobre lo que realmente ocurre. Nace de la necesidad de entender y prevenir ataques físicos de forma proactiva, aportando seguridad con transparencia y control para el usuario.

Enfocado a expertos en ciberseguridad (SOC, blue teamers y red teamers). Cerberus extiende USBvalve para pruebas de USB, debugging de payloads BadUSB y analisis forense basico ante sospecha de infeccion. Muestra actividad de almacenamiento/HID en OLED y por serie para su uso en laboratorio.

---

## Instalación

### Prerequisitos

- Instalar ArduinoIDE
- `Adafruit TinyUSB Library` version `>=3.6.0`
- `Pico-PIO-USB` version `>=0.7.2`
- Boards `Raspberry Pi RP2040 (4.5.4)` con CPU Speed at `133MHz` y Tools=>USB Stack en `Adafruit TinyUSB`
- `Adafruit_SSD1306` OLED library version `>=2.5.14`
- Raspberry Pi Pico 1 o 2 (u otra devboars basada en RP2040)
- Pantalla I2C OLED de 128x64 o 128x32 (SSD1306)

### Pasos

---

#### Usando la version precompilada

Dentro del github ve a releases y busca el archivo .uf2 mas reciente, descargalo

Para flashear la imagen debes:

- Conecta la Raspberry Pi Pico con un cable USB, manten el boton _BOOTSEL/BOOT_ presionado.
- Suelta el boton.
- Veremos un nuevo dispositivo en el sistema, llamado `RPI-RP2` (En linux probablemente tendras que hacerlo manualmente).
- Copia el archivo creado `.uf2` en el folder, dependiendo de la pantalla OLED.
- Espera unos segundos hasta que desaparezca el dispositivo

#### Compila tu version

Obten el repo de forma local

```bash
git clone https://github.com/Glitchboi-sudo/Cerberus-A-USB-Watchdog.git
```

Con Arduino IDE

- Abre `Software/Cerberus/Cerberus.ino`.
- Conecta la Raspberry Pi Pico con un cable USB
- Selecciona la Pico en Arduino
- Tools -> USBStack -> Adafruit TinyUSB
- Click en `Upload`

---

## Uso

### Inicio

1. Conecta la Pico por USB (puerto nativo del RP2040)
2. La OLED mostrará "Selftest: OK" y el NeoPixel quedará en azul si todo funciona correctamente
3. Conecta dispositivos USB sospechosos al puerto host para analizarlos

### Indicadores en pantalla y LED

| Evento                        | Mensaje OLED       | Color LED |
| ----------------------------- | ------------------ | --------- |
| Listo/Normal                  | "Cerberus Ready"   | Azul      |
| Lectura de README             | "README (R)"       | Azul      |
| Lectura de AUTORUN            | "AUTORUN (R)"      | Azul      |
| Escritura detectada           | "WRITING"          | Azul      |
| Borrado detectado             | "DELETING"         | Azul      |
| Dispositivo HID conectado     | "HID Device"       | Rojo      |
| HID enviando datos            | "HID Sending data" | Rojo      |
| Dispositivo sospechoso        | "SUSP: [nombre]"   | Naranja   |
| Tecleo automatizado (BadUSB)  | "AUTO [X] k/s"     | Magenta   |
| USB Killer detectado          | "USB Killer"       | Rojo      |
| Dispositivo de almacenamiento | "Mass Device"      | Verde     |

### Botones físicos

- **BTN_RST**: Navega por las páginas de descriptores USB del dispositivo conectado
- **BTN_OK**: Sale de la vista de descriptores / refresca la pantalla
- **BOOTSEL**:
  - Pulsación corta: Reinicia el dispositivo
  - Pulsación >2s: Muestra el conteo de eventos HID

### Comandos por puerto serie

Conecta vía serial (SerialTinyUSB) y escribe estos comandos:

| Comando    | Descripción                                   |
| ---------- | --------------------------------------------- |
| `HELP`     | Muestra lista de comandos                     |
| `STATUS`   | Estado actual del dispositivo                 |
| `LAST`     | Info forense del último dispositivo conectado |
| `RESET`    | Reinicia contadores                           |
| `REBOOT`   | Reinicia el dispositivo                       |
| `VERBOSE`  | Activa/desactiva modo detallado               |
| `HEXDUMP`  | Activa/desactiva volcado hexadecimal          |
| `HIDDEBUG` | Activa/desactiva debug HID (bytes crudos)     |
| `CLEAR`    | Borra info del último dispositivo             |

---

## Hardware

La carpeta `Hardware/` contiene los diseños de PCB del proyecto.

### Esquemático

![Esquemático de Cerberus](CerberusSchemmatics.png)

### CerberusZero.epro

PCB completa del proyecto Cerberus. Incluye el circuito integrado con el RP2040, conectores USB, pantalla OLED y todos los componentes necesarios.

- **Formato**: Proyecto de EasyEDA Pro
- **Cómo abrir**: Importar en EasyEDA Pro desde `Archivo → Abrir`

### ADUM3160 USB Isolator

Módulo de aislamiento galvánico USB basado en el chip ADUM3160. Proporciona **protección contra ataques USB-Killer** al aislar eléctricamente el host del dispositivo, bloqueando picos de alto voltaje.

- **Uso**: Complemento opcional para añadir una capa extra de seguridad física
- **Velocidad**: USB 2.0 Full Speed (12 Mbps)
- **Más información**: Ver `Hardware/ADUM3160 USB Isolator/README.md`

---

## Arquitectura del Software

### Firmware (Cerberus.ino)

El firmware utiliza ambos cores del RP2040:

- **Core 0**: Maneja la interfaz de usuario (OLED, LED NeoPixel, botones) y emula un dispositivo de almacenamiento USB para detectar actividad sospechosa del host.
- **Core 1**: Ejecuta el stack USB Host via PIO para monitorear dispositivos conectados (HID, Mass Storage, CDC).

**Modulos principales:**

- **Deteccion de amenazas**: USB Killer (via interrupcion hardware), dispositivos sospechosos (base de datos VID/PID), y escritura automatizada BadUSB (analisis de velocidad HID).
- **Emulacion MSC**: Disco RAM virtual que detecta lecturas de README/AUTORUN y escrituras maliciosas.
- **Procesamiento HID**: Decodificacion de reportes de teclado/mouse con soporte para modificadores y teclas especiales.
- **Interfaz OLED**: GUI con iconos, estados y vista de descriptores USB.
- **Comandos serial**: Interface de texto para debugging y forense (HELP, STATUS, LAST, HEXDUMP, etc.).

### Companion App (cerberus_listener.py)

Aplicacion GUI en Python/Tkinter para monitoreo en tiempo real:

- **Conexion serial**: Auto-deteccion del dispositivo, reconexion automatica.
- **Log con filtros**: Colores por severidad, busqueda, filtrado por categoria.
- **Analizador de payloads**: Detecta patrones de ataque (GUI+R, powershell, etc.) y calcula velocidad de tecleo.
- **Modo Red Team**: Exportacion de keystrokes capturados a DuckyScript, comandos rapidos, filtro para bug CTRL del Flipper Zero.

---

## Contribuir

Este proyecto no solo es un repositorio: es un espacio abierto para aprender, experimentar y construir juntos. **Buscamos activamente contribuciones**, ya sea en la parte técnica o incluso en la documentación.

- **En hardware:** Si detectas oportunidades para mejorar la eficiencia (por ejemplo, usando otros chips, optimizando el consumo de energía o cambiando componentes por alternativas más confiables), ¡tus sugerencias son bienvenidas!
- **En software:** Desde corrección de bugs, optimización de rendimiento, hasta mejoras en la legibilidad del código o documentación; todo aporte, grande o pequeño, suma muchísimo.
  No necesitas ser experto para ayudar: si crees que algo puede explicarse mejor, que el código puede ser más claro, o que hay una forma más elegante de hacer algo, **cuéntanos o abre un Pull Request**.

---

## Créditos

- Proyecto basado en [USBvalve](https://github.com/cecio/USBvalve) hecho por _[Cecio](https://github.com/cecio)_
- Proteccion Galvanica basada en [USB Isolator ](https://github.com/wagiminator/ADuM3160-USB-Isolator) hecho por _[wagiminator](https://github.com/wagiminator)_
- Modificado / Creado por:
  - [Erik Alcantara](https://www.linkedin.com/in/erik-alc%C3%A1ntara-covarrubias-29a97628a/)

---

## Patrocinador

<p align="center">
  <a href="https://www.pcbway.com/">
    <img src="https://www.pcbway.com/project/img/images/frompcbway-1220.png" alt="PCBWay Logo" width="300">
  </a>
</p>

<p align="center">
  <strong>Este proyecto fue posible gracias al apoyo de <a href="https://www.pcbway.com/">PCBWay</a></strong>
</p>

Agradecemos a **PCBWay** por patrocinar este proyecto. Su servicio de fabricación de PCBs de alta calidad nos permitió llevar Cerberus del prototipo a la realidad. Si buscas fabricar tus propias PCBs con excelente calidad, precios competitivos y envío rápido, te recomendamos visitar [PCBWay](https://www.pcbway.com/).
