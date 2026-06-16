# Cerberus
<p align="center">
  <img src="1.png" alt="Póster del proyecto" width="550">
</p>

<p align="center">
  <strong>Desarrollado por Lab217</strong><br>
  Ninguna USB entra sin permiso. NINGUNA
</p>

<p align="center">
  <sub>Patrocinado con orgullo por</sub><br>
  <a href="https://www.pcbway.com/"><img src="https://www.pcbway.com/project/img/images/frompcbway-1220.png" alt="PCBWay" width="150"></a>
</p>

![Estado](https://img.shields.io/badge/status-En_desarrollo-green)
![License](https://img.shields.io/badge/license-GNU_AGPLv3-blue)

**🌐 [English](README.md) · [Español](README.es.md) · [Português](README.pt.md)**

---

## ¿Qué es Cerberus?

**Cerberus es un dispositivo de bolsillo donde conectas cualquier USB sospechoso para ver, en tiempo real, qué intenta hacer en una computadora — antes de arriesgar la tuya.**

En lugar de confiar a ciegas en una memoria que encontraste o que te prestaron, la conectas primero a Cerberus: el dispositivo se hace pasar por una PC normal, observa cómo se comporta el USB y te avisa al instante si intenta teclear solo (BadUSB / Rubber Ducky), leer o escribir archivos sin permiso, o freír el puerto con un pico de voltaje (USB Killer). Todo se muestra en una pantalla OLED, un LED de colores y por puerto serie.

### Características principales

- **Detecta BadUSB / Rubber Ducky** — identifica el tecleo automático imposible para un humano.
- **Detecta USB Killer** — alerta de picos de alto voltaje (y los bloquea con el aislador opcional).
- **Vigila lecturas y escrituras** — avisa si el USB intenta leer `AUTORUN`/`README` o escribir/borrar archivos por su cuenta.
- **Base de datos de dispositivos sospechosos** — reconoce hardware de ataque conocido por su VID/PID.
- **Visibilidad total** — pantalla OLED, LED NeoPixel y consola serie con todo lo que ocurre.
- **Forense y red team** — captura keystrokes, exporta a DuckyScript y guarda info del último dispositivo.

> Pensado para profesionales de ciberseguridad (SOC, blue team y red team) como banco de pruebas USB, depuración de payloads BadUSB y triaje forense básico. Construido sobre [USBvalve](https://github.com/cecio/USBvalve).

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

La idea es simple: **Cerberus se conecta entre tu computadora y el USB que quieres inspeccionar**, y actúa como señuelo para que el dispositivo sospechoso muestre su comportamiento real.

### Paso a paso

1. **Alimenta Cerberus.** Conéctalo a tu computadora con un cable USB (puerto nativo del RP2040). Al encender, la OLED mostrará "Selftest: OK" y el LED quedará en **azul**: listo para trabajar.
2. **Conecta el USB sospechoso** al puerto host de Cerberus (no a tu computadora).
3. **Observa la reacción en tiempo real.** Cerberus se hace pasar por una PC y deja que el dispositivo actúe. La pantalla, el LED y la consola serie te dirán de inmediato qué intenta hacer.
4. **Interpreta el veredicto** con la tabla de indicadores de abajo: azul = normal, rojo/naranja/magenta = sospechoso.
5. **(Opcional) Profundiza** con los botones físicos para ver los descriptores USB, o con los comandos serie para análisis forense y captura de keystrokes.

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

## ¿Cómo detecta las amenazas?

Cerberus no se limita a cortar las líneas de datos: **se hace pasar por una computadora real** para que el dispositivo sospechoso revele sus intenciones, y vigila cuatro frentes a la vez.

- **Tecleo automático (BadUSB / Rubber Ducky).** Cerberus mide la velocidad de pulsaciones del teclado emulado. Ningún humano teclea decenas de teclas por segundo, así que cuando supera ese umbral lo marca como ataque automatizado y muestra la velocidad detectada.

- **Lecturas y escrituras maliciosas.** Cerberus expone un pequeño disco virtual con archivos cebo (`README`, `AUTORUN`). Si un USB —o el malware en él— intenta leer esos archivos automáticamente, o escribir/borrar contenido sin tu intervención, queda al descubierto al instante.

- **Dispositivos de ataque conocidos.** Al conectarse, Cerberus lee los identificadores del dispositivo (VID/PID) y los compara contra una base de datos de hardware de ataque conocido, avisando si reconoce uno.

- **USB Killer.** Un circuito de hardware vigila el voltaje del puerto; ante un pico de alto voltaje dispara una alerta inmediata. Con el aislador galvánico opcional, además se **bloquea físicamente** la descarga para que nunca llegue a tu computadora.

Cada detección se traduce al momento en un mensaje en la OLED, un color de LED y una línea por el puerto serie, dándote un veredicto claro sin necesidad de arriesgar un equipo real.

---

## Hardware

La carpeta `Hardware/` contiene los diseños de PCB del proyecto.

### Esquemático

📄 [Esquemático de Cerberus (PDF)](SCH_Schematic1_2026-06-15.pdf)

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
