# Cerberus
<p align="center">
  <img src="1.png" alt="Pôster do projeto" width="550">
</p>

<p align="center">
  <strong>Desenvolvido pela Lab217</strong><br>
  Nenhum USB entra sem permissão. NENHUM
</p>

<p align="center">
  <sub>Orgulhosamente patrocinado por</sub><br>
  <a href="https://www.pcbway.com/"><img src="https://www.pcbway.com/project/img/images/frompcbway-1220.png" alt="PCBWay" width="150"></a>
</p>

![Status](https://img.shields.io/badge/status-Em_desenvolvimento-green)
![License](https://img.shields.io/badge/license-GNU_AGPLv3-blue)

**🌐 [English](README.md) · [Español](README.es.md) · [Português](README.pt.md)**

---

## O que é o Cerberus?

**O Cerberus é um dispositivo de bolso onde você conecta qualquer USB suspeito para ver, em tempo real, o que ele tenta fazer em um computador — antes de arriscar o seu.**

Em vez de confiar cegamente em um pendrive que você encontrou ou que lhe emprestaram, você o conecta primeiro ao Cerberus: o dispositivo se passa por um PC normal, observa como o USB se comporta e avisa na hora se ele tentar digitar sozinho (BadUSB / Rubber Ducky), ler ou gravar arquivos sem permissão, ou queimar a porta com um pico de tensão (USB Killer). Tudo é exibido em uma tela OLED, um LED colorido e pela porta serial.

### Principais recursos

- **Detecta BadUSB / Rubber Ducky** — identifica a digitação automática impossível para um humano.
- **Detecta USB Killer** — alerta sobre picos de alta tensão (e os bloqueia com o isolador opcional).
- **Monitora leituras e gravações** — avisa se o USB tentar ler `AUTORUN`/`README` ou gravar/apagar arquivos por conta própria.
- **Banco de dados de dispositivos suspeitos** — reconhece hardware de ataque conhecido pelo seu VID/PID.
- **Visibilidade total** — tela OLED, LED NeoPixel e console serial mostrando tudo o que acontece.
- **Forense e red team** — captura keystrokes, exporta para DuckyScript e guarda informações do último dispositivo.

> Pensado para profissionais de cibersegurança (SOC, blue team e red team) como bancada de testes de USB, depuração de payloads BadUSB e triagem forense básica. Construído sobre o [USBvalve](https://github.com/cecio/USBvalve).

---

## Instalação

### Pré-requisitos

- Instalar o Arduino IDE
- `Adafruit TinyUSB Library` versão `>=3.6.0`
- `Pico-PIO-USB` versão `>=0.7.2`
- Boards `Raspberry Pi RP2040 (4.5.4)` com CPU Speed em `133MHz` e Tools => USB Stack em `Adafruit TinyUSB`
- `Adafruit_SSD1306` biblioteca OLED versão `>=2.5.14`
- Raspberry Pi Pico 1 ou 2 (ou outra placa de desenvolvimento baseada em RP2040)
- Tela I2C OLED de 128x64 ou 128x32 (SSD1306)

### Passos

---

#### Usando a versão pré-compilada

No GitHub, vá em Releases e procure o arquivo `.uf2` mais recente, depois baixe-o.

Para gravar a imagem:

- Conecte a Raspberry Pi Pico com um cabo USB segurando o botão _BOOTSEL/BOOT_.
- Solte o botão.
- Um novo dispositivo chamado `RPI-RP2` aparecerá no sistema (no Linux você provavelmente terá que montá-lo manualmente).
- Copie o arquivo `.uf2` para a pasta, dependendo da sua tela OLED.
- Aguarde alguns segundos até o dispositivo desaparecer.

#### Compile sua própria versão

Obtenha o repositório localmente:

```bash
git clone https://github.com/Glitchboi-sudo/Cerberus-A-USB-Watchdog.git
```

Com o Arduino IDE:

- Abra `Software/Cerberus/Cerberus.ino`.
- Conecte a Raspberry Pi Pico com um cabo USB.
- Selecione a Pico no Arduino.
- Tools -> USB Stack -> Adafruit TinyUSB
- Clique em `Upload`.

---

## Uso

A ideia é simples: **o Cerberus fica entre o seu computador e o USB que você quer inspecionar**, atuando como isca para que o dispositivo suspeito mostre seu comportamento real.

### Passo a passo

1. **Ligue o Cerberus.** Conecte-o ao seu computador com um cabo USB (a porta nativa do RP2040). Ao ligar, o OLED mostra "Selftest: OK" e o LED fica **azul**: pronto para uso.
2. **Conecte o USB suspeito** na porta host do Cerberus (não no seu computador).
3. **Observe a reação em tempo real.** O Cerberus se passa por um PC e deixa o dispositivo agir. A tela, o LED e o console serial dizem imediatamente o que ele está tentando fazer.
4. **Interprete o veredito** usando a tabela de indicadores abaixo: azul = normal, vermelho/laranja/magenta = suspeito.
5. **(Opcional) Aprofunde** com os botões físicos para ver os descritores USB, ou com os comandos seriais para análise forense e captura de keystrokes.

### Indicadores na tela e no LED

| Evento                       | Mensagem OLED      | Cor do LED |
| ---------------------------- | ------------------ | ---------- |
| Pronto/Normal                | "Cerberus Ready"   | Azul       |
| Leitura de README            | "README (R)"       | Azul       |
| Leitura de AUTORUN           | "AUTORUN (R)"      | Azul       |
| Gravação detectada           | "WRITING"          | Azul       |
| Exclusão detectada           | "DELETING"         | Azul       |
| Dispositivo HID conectado    | "HID Device"       | Vermelho   |
| HID enviando dados           | "HID Sending data" | Vermelho   |
| Dispositivo suspeito         | "SUSP: [nome]"     | Laranja    |
| Digitação automatizada (BadUSB) | "AUTO [X] k/s"  | Magenta    |
| USB Killer detectado         | "USB Killer"       | Vermelho   |
| Dispositivo de armazenamento | "Mass Device"      | Verde      |

### Botões físicos

- **BTN_RST**: Navega pelas páginas de descritores USB do dispositivo conectado
- **BTN_OK**: Sai da visualização de descritores / atualiza a tela
- **BOOTSEL**:
  - Pressão curta: Reinicia o dispositivo
  - Pressão >2s: Mostra a contagem de eventos HID

### Comandos pela porta serial

Conecte via serial (SerialTinyUSB) e digite estes comandos:

| Comando    | Descrição                                       |
| ---------- | ----------------------------------------------- |
| `HELP`     | Mostra a lista de comandos                       |
| `STATUS`   | Estado atual do dispositivo                       |
| `LAST`     | Info forense do último dispositivo conectado     |
| `RESET`    | Reinicia os contadores                            |
| `REBOOT`   | Reinicia o dispositivo                            |
| `VERBOSE`  | Ativa/desativa o modo detalhado                   |
| `HEXDUMP`  | Ativa/desativa o dump hexadecimal                 |
| `HIDDEBUG` | Ativa/desativa o debug HID (bytes crus)           |
| `CLEAR`    | Apaga a info do último dispositivo                |

---

## Como ele detecta as ameaças?

O Cerberus não se limita a cortar as linhas de dados: **ele se passa por um computador real** para que o dispositivo suspeito revele suas intenções, e vigia quatro frentes ao mesmo tempo.

- **Digitação automática (BadUSB / Rubber Ducky).** O Cerberus mede a velocidade de teclas do teclado emulado. Nenhum humano digita dezenas de teclas por segundo, então quando ultrapassa esse limite ele marca como ataque automatizado e mostra a velocidade detectada.

- **Leituras e gravações maliciosas.** O Cerberus expõe um pequeno disco virtual com arquivos isca (`README`, `AUTORUN`). Se um USB — ou o malware nele — tentar ler esses arquivos automaticamente, ou gravar/apagar conteúdo sem a sua ação, fica exposto na hora.

- **Dispositivos de ataque conhecidos.** Ao conectar, o Cerberus lê os identificadores do dispositivo (VID/PID) e os compara com um banco de dados de hardware de ataque conhecido, avisando se reconhecer algum.

- **USB Killer.** Um circuito de hardware vigia a tensão da porta; diante de um pico de alta tensão dispara um alerta imediato. Com o isolador galvânico opcional, a descarga ainda é **bloqueada fisicamente** para que nunca chegue ao seu computador.

Cada detecção é traduzida na hora em uma mensagem no OLED, uma cor de LED e uma linha pela porta serial, dando a você um veredito claro sem precisar arriscar uma máquina real.

---

## Hardware

A pasta `Hardware/` contém os projetos de PCB do projeto.

### Esquemático

📄 [Esquemático do Cerberus (PDF)](SCH_Schematic1_2026-06-15.pdf)

### CerberusZero.epro

A PCB completa do projeto Cerberus. Inclui o circuito integrado com o RP2040, conectores USB, tela OLED e todos os componentes necessários.

- **Formato**: Projeto do EasyEDA Pro
- **Como abrir**: Importar no EasyEDA Pro em `Arquivo → Abrir`

### ADUM3160 USB Isolator

Módulo de isolamento galvânico USB baseado no chip ADUM3160. Fornece **proteção contra ataques USB-Killer** ao isolar eletricamente o host do dispositivo, bloqueando picos de alta tensão.

- **Uso**: Complemento opcional para adicionar uma camada extra de segurança física
- **Velocidade**: USB 2.0 Full Speed (12 Mbps)
- **Mais informações**: Veja `Hardware/ADUM3160 USB Isolator/README.md`

---

## Arquitetura do Software

### Firmware (Cerberus.ino)

O firmware usa os dois cores do RP2040:

- **Core 0**: Cuida da interface de usuário (OLED, LED NeoPixel, botões) e emula um dispositivo de armazenamento USB para detectar atividade suspeita do host.
- **Core 1**: Executa a stack USB Host via PIO para monitorar dispositivos conectados (HID, Mass Storage, CDC).

**Módulos principais:**

- **Detecção de ameaças**: USB Killer (via interrupção de hardware), dispositivos suspeitos (banco de dados VID/PID) e gravação automatizada BadUSB (análise de velocidade HID).
- **Emulação MSC**: Disco RAM virtual que detecta leituras de README/AUTORUN e gravações maliciosas.
- **Processamento HID**: Decodificação de relatórios de teclado/mouse com suporte a modificadores e teclas especiais.
- **Interface OLED**: GUI com ícones, estados e visualização de descritores USB.
- **Comandos seriais**: Interface de texto para depuração e forense (HELP, STATUS, LAST, HEXDUMP, etc.).

### Companion App (cerberus_listener.py)

Aplicativo GUI em Python/Tkinter para monitoramento em tempo real:

- **Conexão serial**: Detecção automática do dispositivo, reconexão automática.
- **Log com filtros**: Cores por severidade, busca, filtragem por categoria.
- **Analisador de payloads**: Detecta padrões de ataque (GUI+R, powershell, etc.) e calcula a velocidade de digitação.
- **Modo Red Team**: Exportação de keystrokes capturados para DuckyScript, comandos rápidos, filtro para o bug CTRL do Flipper Zero.

---

## Contribuir

Este projeto não é apenas um repositório: é um espaço aberto para aprender, experimentar e construir juntos. **Buscamos ativamente contribuições**, seja na parte técnica ou até na documentação.

- **Em hardware:** Se você notar oportunidades de melhorar a eficiência (por exemplo, usando outros chips, otimizando o consumo de energia ou trocando componentes por alternativas mais confiáveis), suas sugestões são bem-vindas!
- **Em software:** Desde correção de bugs e otimização de desempenho até melhorias na legibilidade do código ou na documentação; toda contribuição, grande ou pequena, soma muito.
  Você não precisa ser especialista para ajudar: se acha que algo pode ser explicado melhor, que o código pode ficar mais claro, ou que há uma forma mais elegante de fazer algo, **conte para nós ou abra um Pull Request**.

---

## Créditos

- Projeto baseado no [USBvalve](https://github.com/cecio/USBvalve) feito por _[Cecio](https://github.com/cecio)_
- Proteção galvânica baseada no [USB Isolator](https://github.com/wagiminator/ADuM3160-USB-Isolator) feito por _[wagiminator](https://github.com/wagiminator)_
- Modificado / Criado por:
  - [Erik Alcantara](https://www.linkedin.com/in/erik-alc%C3%A1ntara-covarrubias-29a97628a/)

---

## Patrocinador

<p align="center">
  <a href="https://www.pcbway.com/">
    <img src="https://www.pcbway.com/project/img/images/frompcbway-1220.png" alt="PCBWay Logo" width="300">
  </a>
</p>

<p align="center">
  <strong>Este projeto foi possível graças ao apoio da <a href="https://www.pcbway.com/">PCBWay</a></strong>
</p>

Agradecemos à **PCBWay** por patrocinar este projeto. Seu serviço de fabricação de PCBs de alta qualidade nos permitiu levar o Cerberus do protótipo à realidade. Se você procura fabricar suas próprias PCBs com excelente qualidade, preços competitivos e envio rápido, recomendamos visitar a [PCBWay](https://www.pcbway.com/).
