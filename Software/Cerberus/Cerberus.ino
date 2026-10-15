/*********************************************************************

  Cerberus
  
  written by Cesare Pizzi && Glitchboi
  This project extensively reuse code done by Adafruit and TinyUSB. 
  Please support them!

*********************************************************************/

/*********************************************************************
  Adafruit invests time and resources providing this open source code,
  please support Adafruit and open-source hardware by purchasing
  products from Adafruit!

  MIT license, check LICENSE for more information
  Copyright (c) 2019 Ha Thach for Adafruit Industries
  All text above, and the splash screen below must be included in
  any redistribution
*********************************************************************/

#include <pio_usb.h>
// SdFat (Adafruit Fork) MUST be included before Adafruit_TinyUSB.h: this is what
// makes arduino-cli link SdFat and makes TinyUSB expose its host MSC block device
// (Adafruit_USBH_MSC_BlockDevice), used by the USB content explorer (usb_explorer.ino).
#include "SdFat_Adafruit_Fork.h"
#include "Adafruit_TinyUSB.h"
#include <XxHash_arduino.h>
#include <pico/stdlib.h>
#include <hardware/clocks.h> 
#include <SPI.h>
#include <Wire.h>
#include <pico/util/queue.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>
#include <string.h>
#include <ctype.h>
#include "Petme8x8.h"    // Custom GFX fonts used by the on-device mass-storage menu
#include "Picopixel.h"

extern "C" {
  #include "tusb.h"
}
                            
//
// BADUSB detector section
//

/*
 * Requirements:
 * - [Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB) library
 * - 2 consecutive GPIOs: D+ is defined by HOST_PIN_DP (gpio2), D- = D+ +1 (gpio3)
 * - CPU Speed must be either 120 or 240 Mhz. Selected via "Menu -> CPU Speed"
 */

#define HOST_PIN_DP 0       // Pin used as D+ for host, D- = D+ + 1
#define LANGUAGE_ID 0x0409  // English

Adafruit_USBH_Host USBHost;

typedef struct {
  tusb_desc_device_t desc_device;
  uint16_t manufacturer[32];
  uint16_t product[48];
  uint16_t serial[16];
  bool mounted;
} dev_info_t;

// CFG_TUH_DEVICE_MAX is defined by tusb_config header
dev_info_t dev_info[CFG_TUH_DEVICE_MAX] = { 0 };


#define I2C_ADDRESS 0x3C  // 0X3C+SA0 - 0x3C or 0x3D
#define RST_PIN -1        // No external reset pin required by this OLED
#define OLED_WIDTH  128
#define OLED_HEIGHT 64    // 64 or 32 depending on the OLED

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, RST_PIN);

#include "GUI_Base.h"   // all on-device screens (drawn against `display`)

typedef struct {
  char text[64];
} DisplayMsg;

void cls(void);
void show_descriptor_page();
void exit_descriptor_view();
dev_info_t* get_first_mounted_device();
void reinit_display();
void showStatus(const char* message, uint8_t logoIndex);
void showHome(const char* line);     // idle home screen (GUI_Base guiMainMenu)
void redraw_resting();               // repaint the resting screen (home or last status)

// Home-screen state: when active, the resting screen is the redesigned idle
// home (guiMainMenu) showing `g_homeLine`; any transient alert via showStatus()
// clears it so the alert stays put until the next showHome().
char g_homeLine[20] = "Self-Test:ok";
bool g_homeActive   = false;
bool g_noDeviceActive = false;       // the "No device" screen is up (any key -> home)
volatile bool g_massActive = false;  // mass device attached: detection screen is resting
volatile bool g_hidMountReq = false; // HID attached (core1 -> core0 draws guiHidMount)
char          g_hidMountName[16] = {0};
volatile bool g_cdcMountReq = false; // CDC attached (core1 -> core0 draws guiCdcDevice)

// Define the dimension of RAM DISK. We have a "real" one (for which
// a real array is created) and a "fake" one, presented to the OS
#define DISK_BLOCK_NUM 0x150
#define FAKE_DISK_BLOCK_NUM 0x800
#define DISK_BLOCK_SIZE 0x200
#include "ramdisk.h"

Adafruit_USBD_MSC usb_msc;

// Front-panel navigation buttons (each wired to GND, using internal pull-ups).
//   Up     -> GP2    move the selection up / previous
//   Accept -> GP3    open the menu / enter the highlighted item
//   Bak    -> GP6    go back to the previous menu (or close the menu)
//   Down   -> GP7    move the selection down / next
#define BTN_UP     2
#define BTN_ACCEPT 3
#define BTN_BAK    6
#define BTN_DOWN   7

//
// USBKiller Globals
//
#define KILLER_PIN 8
#define NEOPIXEL_PIN 16
#define NEOPIXEL_COUNT 1

//
// USBvalve globals
//
#define VERSION "Cerberus - 1.1.0"
boolean readme = false;
boolean autorun = false;
boolean written = false;
boolean deleted = false;
boolean written_reported = false;
boolean deleted_reported = false;
boolean hid_sent = false;
boolean hid_reported = false;
boolean usbkiller = false;
boolean suspicious_device = false;
boolean suspicious_reported = false;
char suspicious_name[16] = {0};
bool descriptor_view_active = false;
uint8_t descriptor_page = 0;
uint hid_event_num = 0;
char current_gui_message[64] = "Cerberus Ready";

//
// HID Speed Detection - Detects automated typing (BadUSB)
//
#define HID_SPEED_WINDOW_MS 1000    // Window to measure speed (1 second)
#define HID_SPEED_THRESHOLD 40      // Keys per second threshold for automation
uint32_t hid_window_start = 0;
uint16_t hid_window_count = 0;
float hid_current_speed = 0;
boolean hid_speed_alert = false;
boolean hid_speed_reported = false;

//
// Serial Command Interface
//
#define SERIAL_BUFFER_SIZE 64
char serial_buffer[SERIAL_BUFFER_SIZE];
uint8_t serial_buffer_idx = 0;

//
// Verbose Mode - Reduce serial output when disabled
//
boolean verbose_mode = false;       // Default: OFF -- per-sector Read/Write LBA logging
                                    // floods the console (the PC polls the emulated
                                    // disk constantly). Enable with VERBOSE to debug I/O.
boolean show_hexdump = false;       // Default: hexdump off (reduces noise)
boolean show_hid_debug = false;     // Default: HID debug off (shows raw modifier bytes)

//
// Last Device Info (for forensics) - Stored in RAM, survives soft reset
//
typedef struct {
  uint16_t vid;
  uint16_t pid;
  uint8_t device_class;
  uint8_t device_subclass;
  char manufacturer[32];
  char product[48];
  char serial[16];
  uint32_t timestamp;
  uint16_t hid_events;
  float max_hid_speed;
  bool was_suspicious;
  char suspicious_reason[16];
} last_device_info_t;

last_device_info_t last_device __attribute__((section(".noinit")));  // Survives soft reset

void process_serial_command(const char* cmd);
void print_status();
void print_last_device();
void save_device_info(dev_info_t* dev, tusb_desc_device_t* desc);
void update_hid_speed();

Adafruit_NeoPixel statusPixel(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
queue_t display_queue;  // Thread-safe queue for cross-core OLED messages

//
// USB Neutralizer (partition-table wiper)
//
// Ported from the standalone USB-Neutralizer project by novamostra
// (https://github.com/novamostra/USB-Neutralizer).
//
// When a USB mass-storage device is attached to the HOST port and the user holds the "Bak" button, the
// first and last WIPE_LIMIT_LBA blocks are overwritten with zeros. That destroys
// the MBR / GPT primary header + GPT backup header and the FAT/boot area, so the
// drive can no longer be mounted until it is re-partitioned, WITHOUT touching any
// (potentially malicious) file payload in between.
//
// All tuh_msc_* calls must run on core1 (where the USB host stack lives). The
// buttons are read on core0, so the trigger is handed to core1 through the
// volatile flags below.
//
#define WIPE_LIMIT_LBA   34    // Blocks wiped at the start (and at the end) of the disk
#define WIPE_BLOCK_SIZE  512   // Logical block size we support (bytes)
#define WIPE_HOLD_MS     3000  // How long "Bak" must be held to launch a wipe

typedef struct {
  uint8_t  dev_addr;
  uint32_t current_lba;
  uint32_t block_count;
  uint8_t  phase;                     // 0 = wiping head, 1 = wiping tail
  bool     is_wiping;
  uint8_t  buffer[WIPE_BLOCK_SIZE];   // Always-zero source buffer
} WipeState;

WipeState wipe = { 0 };

// Cross-core signalling between the buttons (core0) and the host stack (core1)
volatile bool    msc_mounted      = false;  // A mass-storage device is attached
volatile uint8_t msc_dev_addr     = 0;      // Its host address
volatile bool    wipe_requested   = false;  // Core0 asks core1 to start a wipe
volatile bool    wipe_in_progress = false;  // Mirror of wipe.is_wiping, read by core0
volatile bool    wipe_done        = false;  // Core1 finished successfully
volatile bool    wipe_error       = false;  // Core1 aborted with an error
bool wipe_done_reported  = false;           // Core0 one-shot render guards
bool wipe_error_reported = false;

// Forward declarations (implemented in the host-devices section)
void start_wipe_process(uint8_t dev_addr);
void process_next_block();
bool wipe_write_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const* cb_data);
void service_wipe();

//
// USB content explorer (implemented in usb_explorer.ino)
//
// Lets you inspect the files on a mass-storage device plugged into the HOST
// port BEFORE it ever reaches the real PC: mount it read-only, list directories
// and preview file contents (text/hex), either over serial (LS/CAT) or on the
// OLED (hold OK while a drive is mounted). Supports FAT12/16/32 + exFAT, NTFS,
// ext2/3/4 and ISO9660/Joliet across MBR/GPT partitions (see usb_explorer.ino).
//
void explorer_mount(uint8_t dev_addr);    // called from tuh_msc_mount_cb   (core1)
void explorer_umount(uint8_t dev_addr);   // called from tuh_msc_umount_cb  (core1)
void mscService();                        // called from loop1()            (core1)
void explorer_ls(const char* path);       // serial LS command              (core0)
void explorer_cat(const char* path);      // serial CAT command             (core0)
void explorer_scan();                     // serial SCAN command (recursive)  (core0)
void explorer_inspect(const char* path);  // serial INSPECT command (deep)    (core0)
void explorer_analyze();                  // modal OLED file analyzer         (core0)
void explorer_menu();                     // modal 4-button on-device menu    (core0)
extern volatile bool g_mscMounted;        // a browsable volume is mounted
extern volatile bool g_mscPresent;        // a disk is attached (mountable or not)

//
// Anti-Detection settings.
//
// Set USB IDs strings and numbers, to avoid possible detections.
// Remember that you can cusotmize FAKE_DISK_BLOCK_NUM as well
// for the same reason. Also DISK_LABEL in ramdisk.h can be changed.
//
// You can see here for inspiration: https://the-sz.com/products/usbid/
//
// Example:
// 0x0951 0x16D5    VENDORID_STR: Kingston   PRODUCTID_STR: DataTraveler
//
#define USB_VENDORID 0x0951
#define USB_PRODUCTID 0x16D5
#define USB_DESCRIPTOR "DataTraveler"
#define USB_MANUF "Kingston"
#define USB_SERIAL "123456789A"           // This override the Pi Pico default. Disabled by default. \
                                          // See "setSerialDescriptor" in setup() if needed
#define USB_VENDORID_STR "Kingston"       // Up to 8 chars
#define USB_PRODUCTID_STR "DataTraveler"  // Up to 16 chars
#define USB_VERSION_STR "1.0"             // Up to 4 chars

//
// Suspicious VID/PID Database
// Known BadUSB, HID attack tools, and devices commonly used for USB attacks
// Stored in flash to save RAM
//
typedef struct {
  uint16_t vid;
  uint16_t pid;        // 0x0000 = match any PID for this VID
  const char* name;
} suspicious_device_t;

const suspicious_device_t SUSPICIOUS_DEVICES[] PROGMEM = {
  // Hak5 Rubber Ducky (often spoofs Apple)
  {0x05AC, 0x0000, "Apple(Spoof?)"},

  // DigiSpark / ATtiny85 USB devices
  {0x16C0, 0x0477, "DigiSpark"},
  {0x16C0, 0x05DF, "USBasp/Digi"},
  {0x16C0, 0x27DB, "HID Digi"},
  {0x16C0, 0x27DA, "CDC Digi"},

  // Teensy boards (commonly used for BadUSB)
  {0x16C0, 0x0486, "Teensy HID"},
  {0x16C0, 0x0487, "Teensy MIDI"},
  {0x16C0, 0x0488, "Teensy Serial"},
  {0x16C0, 0x04D0, "Teensy40 HID"},
  {0x16C0, 0x04D1, "Teensy40 Ser"},

  // Arduino Leonardo/Micro (HID capable)
  {0x2341, 0x8036, "Arduino Leo"},
  {0x2341, 0x8037, "Arduino Micro"},
  {0x2341, 0x0036, "Arduino Leo BL"},
  {0x2341, 0x0037, "Arduino Micro BL"},

  // SparkFun Pro Micro
  {0x1B4F, 0x9205, "ProMicro 5V"},
  {0x1B4F, 0x9206, "ProMicro 3.3V"},
  {0x1B4F, 0x9207, "LilyPad USB"},

  // Malduino / CJMCU BadUSB
  {0x1B4F, 0x0000, "Malduino?"},

  // Flipper Zero
  {0x0483, 0x5740, "Flipper Zero"},

  // O.MG Cable (various spoofed VIDs)
  {0x046D, 0xC52B, "Logitech(OMG?)"},

  // Generic CH340/CH341 (common in cheap clones)
  {0x1A86, 0x7523, "CH340"},
  {0x1A86, 0x5523, "CH341"},

  // USBarmory
  {0x0525, 0xA4A6, "USBarmory"},

  // LAN Turtle / Packet Squirrel
  {0x0525, 0xA4A7, "LAN Turtle?"},

  // Raspberry Pi Pico (could be BadUSB)
  {0x2E8A, 0x0005, "RPi Pico HID"},

  // ESP32-S2/S3 USB
  {0x303A, 0x0002, "ESP32-S2"},
  {0x303A, 0x1001, "ESP32-S3"},

  // ATEN UC-232A (used in some attacks)
  {0x0557, 0x2008, "ATEN Serial"},

  // End marker
  {0x0000, 0x0000, NULL}
};

#define SUSPICIOUS_COUNT (sizeof(SUSPICIOUS_DEVICES) / sizeof(SUSPICIOUS_DEVICES[0]) - 1)

const char* check_suspicious_device(uint16_t vid, uint16_t pid);

#define BLOCK_AUTORUN 102       // Block where Autorun.inf file is saved
#define BLOCK_README 100        // Block where README.txt file is saved
#define MAX_DUMP_BYTES 16       // Used by the dump of the debug facility: do not increase this too much
#define BYTES_TO_HASH 512 * 2   // Number of bytes of the RAM disk used to check consistency
#define BYTES_TO_HASH_OFFSET 7  // Starting sector to check for consistency (FAT_DIRECTORY is 7)

// Burned hash to check consistency
uint valid_hash = 2362816530;

// Main USB killer detector. Raises a software flag when a hostile voltage discharge is detected.
void detectUSBKiller() {
  if (digitalRead(KILLER_PIN) == LOW) {
    usbkiller = true;
  }
}

// Core 0 setup: configures TinyUSB device mode, RAM disk emulation, I2C display, and status LED.
void setup() {
  // Init queue used to pass text from host callbacks (core1) to OLED (core0)
  queue_init(&display_queue, sizeof(DisplayMsg), 16);

  TinyUSBDevice.setID(USB_VENDORID, USB_PRODUCTID);
  TinyUSBDevice.setProductDescriptor(USB_DESCRIPTOR);
  TinyUSBDevice.setManufacturerDescriptor(USB_MANUF);
  TinyUSBDevice.setSerialDescriptor(USB_SERIAL);

#if defined(ARDUINO_ARCH_MBED) && defined(ARDUINO_ARCH_RP2040)
  // Manual begin() is required on core without built-in support for TinyUSB such as
  // - mbed rp2040
  TinyUSB_Device_Init(0);
#endif

  // Set disk vendor id, product id and revision with string up to 8, 16, 4 characters respectively
  usb_msc.setID(USB_VENDORID_STR, USB_PRODUCTID_STR, USB_VERSION_STR);

  // Set disk size (using the "fake" size)
  usb_msc.setCapacity(FAKE_DISK_BLOCK_NUM, DISK_BLOCK_SIZE);

  usb_msc.setReadWriteCallback(msc_read_callback, msc_write_callback, msc_flush_callback);

  // Set Lun ready (RAM disk is always ready)
  usb_msc.setUnitReady(true);

  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_ACCEPT, INPUT_PULLUP);
  pinMode(BTN_BAK, INPUT_PULLUP);

  // USB killer detection wiring and interrupt setup
  pinMode(KILLER_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(KILLER_PIN), detectUSBKiller, FALLING);

  // Check consistency of RAM FS
  // Add 11 bytes to skip the DISK_LABEL from the hashing
  // The startup of the USB has been moved before initialization of the 
  // screen because sometimes it inserts some delay preventing
  // proper initialization of the mass device
  uint computed_hash;
  computed_hash = XXH32(msc_disk[BYTES_TO_HASH_OFFSET] + 11, BYTES_TO_HASH, 0);
  if (computed_hash == valid_hash) {
      usb_msc.begin();
  }

  Wire.setSDA(4);
  Wire.setSCL(5);
  display.begin(SSD1306_SWITCHCAPVCC, I2C_ADDRESS);

  cls();
  guiMainMenu("Booting...");
  statusPixel.begin();
  statusPixel.setBrightness(50);
  setNeoColor(0, 0, 255);  // Blue as the initial neutral status

  if (computed_hash == valid_hash) {
    showHome("Self-Test:ok");
  } else {
    guiBadSelftest();
    SerialTinyUSB.println("Selftest: KO -- stopping");
    while (1) {
      delay(1000);
    }
  }

}

// Core 1 setup: brings up TinyUSB host stack and PIO USB bridge for BADUSB monitoring.
void setup1() {
  set_sys_clock_khz(240000, true);
  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  pio_cfg.pin_dp = HOST_PIN_DP;
  USBHost.configure_pio_usb(1, &pio_cfg);

  // Note: For rp2040 pico-pio-usb, calling USBHost.begin() on core1 will have most of the
  // host bit-banging processing works done in core1
  USBHost.begin(1);
}

// Main Core0 loop: managing display
void loop() {
  // Greet a newly-opened serial terminal (rising edge of DTR / CDC connect).
  // Polled here instead of via tud_cdc_line_state_cb so we don't interfere with
  // the 1200bps-touch auto-reset used for flashing.
  static bool cdc_prev = false;
  bool cdc_now = (bool)SerialTinyUSB;
  if (cdc_now && !cdc_prev) {
    delay(60);                 // let the host finish opening the port
    print_banner();
  }
  cdc_prev = cdc_now;

  // Process serial commands
  while (SerialTinyUSB.available()) {
    char c = SerialTinyUSB.read();
    if (c == '\n' || c == '\r') {
      if (serial_buffer_idx > 0) {
        serial_buffer[serial_buffer_idx] = '\0';
        process_serial_command(serial_buffer);
        serial_buffer_idx = 0;
      }
    } else if (serial_buffer_idx < SERIAL_BUFFER_SIZE - 1) {
      serial_buffer[serial_buffer_idx++] = c;
    }
  }

  // Drain any text queued from host callbacks (running on core1)
  DisplayMsg msg;
  while (queue_try_remove(&display_queue, &msg)) {
    SerialTinyUSB.println(msg.text);
    if (!descriptor_view_active) {
      g_homeActive = false;
      guiGenericMsg(msg.text);
    }
  }

  // Mass-storage attach/detach is flagged by the core1 MSC callbacks; paint the
  // resulting screen here on core0 (never from core1 -- it would race on I2C).
  static bool mass_prev = false;
  if (g_massActive != mass_prev) {
    mass_prev = g_massActive;
    if (g_massActive) { if (!descriptor_view_active) guiMassDetect(); }  // detection screen
    else              showHome(g_homeLine);                              // device gone -> home
  }

  //
  // Front-panel buttons (edge-triggered on press). Resting-screen dependent:
  //   - "No device" screen up  -> ANY key returns to home.
  //   - Mass DETECTION screen  -> Ok/Up/Down open the interactive menu; Bak -> home.
  //   - Home screen            -> Accept with a device opens the menu, otherwise
  //                               shows the "No device" screen.
  // explorer_menu() is a blocking modal that owns all four buttons while open.
  //
  static bool accept_prev = true, up_prev = true, down_prev = true, bak_prev = true;
  bool accept_now = digitalRead(BTN_ACCEPT);
  bool up_now     = digitalRead(BTN_UP);
  bool down_now   = digitalRead(BTN_DOWN);
  bool bak_now    = digitalRead(BTN_BAK);
  bool accept_press = (accept_now != accept_prev) && (accept_now == LOW); accept_prev = accept_now;
  bool up_press     = (up_now     != up_prev)     && (up_now     == LOW); up_prev     = up_now;
  bool down_press   = (down_now   != down_prev)   && (down_now   == LOW); down_prev   = down_now;
  bool bak_press    = (bak_now    != bak_prev)    && (bak_now    == LOW); bak_prev    = bak_now;
  bool any_press    = accept_press || up_press || down_press || bak_press;
  bool devicePresent = (get_first_mounted_device() != NULL || g_mscPresent);

  if (g_noDeviceActive) {
    if (any_press) {                       // any key dismisses -> home
      g_noDeviceActive = false;
      showHome(g_homeLine);
      setNeoColor(0, 0, 255);
    }
  } else if (g_massActive) {
    // Mass DETECTION screen: any button EXCEPT Bak (disabled here) opens the
    // interactive submenu. Bak does nothing; the screen clears on disconnect.
    if (accept_press || up_press || down_press) {
      explorer_menu();                     // interactive Info/Files/Wipe (highlighted)
      if (devicePresent) guiMassDetect();  // back to the detection screen
      else { g_massActive = false; showHome(g_homeLine); }
    }
  } else if (accept_press) {
    if (devicePresent) {
      explorer_menu();
      redraw_resting();
    } else {
      SerialTinyUSB.println("BTN_ACCEPT pressed -> no USB device");
      g_homeActive = false;
      g_noDeviceActive = true;
      guiNoDevice();
    }
  }

  if (descriptor_view_active) {
    return;
  }

  if (usbkiller == true) {
    g_homeActive = false;                 // persistent alert screen
    guiBadUsb();
    SerialTinyUSB.println("USB Killer");
    usbkiller = false;
    setNeoColor(255, 0, 0);
  }

  if (readme == true) {
    guiReadmeRead();
    SerialTinyUSB.println("README (R)");
    readme = false;
    setNeoColor(0, 0, 255);
  }

  if (autorun == true) {
    guiAutorunRead();
    SerialTinyUSB.println("AUTORUN (R)");
    autorun = false;
    setNeoColor(0, 0, 255);
  }

  if (deleted == true && deleted_reported == false) {
    guiDeleting();
    SerialTinyUSB.println("DELETING");
    deleted = false;
    deleted_reported = true;
    setNeoColor(0, 0, 255);
  }

  if (written == true && written_reported == false) {
    guiWriting();
    SerialTinyUSB.println("WRITING");
    written = false;
    written_reported = true;
    setNeoColor(0, 0, 255);
  }

  if (hid_sent == true && hid_reported == false) {
    guiHidSendData(suspicious_name[0] ? suspicious_name : "unknown", hid_current_speed);
    SerialTinyUSB.println("HID Sending data");
    hid_sent = false;
    hid_reported = true;
    setNeoColor(255, 0, 0);
  }

  if (suspicious_device == true && suspicious_reported == false) {
    g_homeActive = false;
    guiHidMount(suspicious_name);
    SerialTinyUSB.printf("[!] Suspicious HID: %s\r\n", suspicious_name);
    suspicious_device = false;
    suspicious_reported = true;
    setNeoColor(255, 165, 0);
  }

  if (hid_speed_alert == true && hid_speed_reported == false) {
    g_homeActive = false;
    guiHidSendData(suspicious_name[0] ? suspicious_name : "unknown", hid_current_speed);
    SerialTinyUSB.printf("[!!!] AUTOMATED TYPING: %.1f keys/sec\r\n", hid_current_speed);
    hid_speed_reported = true;
    setNeoColor(255, 0, 255);
    // Update last device info
    if (hid_current_speed > last_device.max_hid_speed) {
      last_device.max_hid_speed = hid_current_speed;
    }
  }

  // HID / CDC attach notices: the callbacks run on core1 and only raise a flag;
  // draw here on core0 so the OLED (I2C) is never touched from two cores at once.
  if (g_hidMountReq) {
    g_hidMountReq = false;
    g_homeActive = false;
    guiHidMount(g_hidMountName);
    setNeoColor(255, 0, 0);
  }
  if (g_cdcMountReq) {
    g_cdcMountReq = false;
    g_homeActive = false;
    guiCdcDevice("");
    setNeoColor(0, 255, 0);
  }

  // Partition-wipe outcome (flags are set by core1 in wipe_write_complete_cb)
  if (wipe_done == true && wipe_done_reported == false) {
    guiWipingDone();
    SerialTinyUSB.println("WIPE DONE");
    wipe_done_reported = true;
    setNeoColor(0, 255, 0);
  }

  if (wipe_error == true && wipe_error_reported == false) {
    guiWipingError();
    SerialTinyUSB.println("WIPE ERROR");
    wipe_error_reported = true;
    setNeoColor(255, 0, 0);
  }

  if (BOOTSEL) {
    uint32_t press_start = to_ms_since_boot(get_absolute_time());
    while (BOOTSEL) {
      sleep_ms(10);
    }
    uint32_t press_end = to_ms_since_boot(get_absolute_time());
    uint32_t press_duration = press_end - press_start;

    if (press_duration > 2000) {              // Press duration > 2sec
      // Show the number of HID events detected so far
      guiHidEvents(hid_event_num);
      SerialTinyUSB.printf("HID Evt# %d\r\n", hid_event_num);
    } else {
      guiResetting();
      SerialTinyUSB.println("RESETTING");
      swreset();
    }
  }
}


// Main Core1 loop: managing USB Host
void loop1() {
  USBHost.task();
  service_wipe();  // Starts a wipe on the host core when core0 requested one
  mscService();    // Serves file-explorer requests (list/read) issued by core0
}

// Callback invoked when received READ10 command.
// Copy disk's data to buffer (up to bufsize) and
// return number of copied bytes (must be multiple of block size).
// This happens only for the "real" size of disk
int32_t msc_read_callback(uint32_t lba, void* buffer, uint32_t bufsize) {

  // Check for README.TXT
  if (lba == BLOCK_README) {
    readme = true;
  }

  // Check for AUTORUN.INF
  if (lba == BLOCK_AUTORUN) {
    autorun = true;
  }

  // We are declaring a bigger size than what is actually allocated, so
  // this is protecting our memory integrity
  if (lba < DISK_BLOCK_NUM - 1) {
    uint8_t const* addr = msc_disk[lba];
    memcpy(buffer, addr, bufsize);
  }

  if (verbose_mode) {
    SerialTinyUSB.print("Read LBA: ");
    SerialTinyUSB.print(lba);
    SerialTinyUSB.print("   Size: ");
    SerialTinyUSB.println(bufsize);
    if (show_hexdump && lba < DISK_BLOCK_NUM - 1) {
      hexDump(msc_disk[lba], MAX_DUMP_BYTES);
    }
    SerialTinyUSB.flush();
  }

  return bufsize;
}

// Callback invoked when received WRITE10 command.
// Process data in buffer to disk's storage and
// return number of written bytes (must be multiple of block size).
// This happens only for the "real" size of disk
int32_t msc_write_callback(uint32_t lba, uint8_t* buffer, uint32_t bufsize) {

  // Check for file deletion at Block 7
  // The first char of filename is replaced with 0xE5, we are going
  // to check for it
  if (lba == 7) {
    if (buffer[32] == 0xE5 || buffer[64] == 0xE5 || buffer[160] == 0xE5) {
      deleted = true;
    }
  }

  // This check for writing of space. The LBA > 10 is set to avoid some
  // false positives, in particular on Windows Systems
  if (lba > 10) {
    written = true;
  }

  // We are declaring a bigger size than what is actually allocated, so
  // this is protecting our memory integrity
  if (lba < DISK_BLOCK_NUM - 1) {
    uint8_t* addr = msc_disk[lba];
    memcpy(addr, buffer, bufsize);
  }

  if (verbose_mode) {
    SerialTinyUSB.print("Write LBA: ");
    SerialTinyUSB.print(lba);
    SerialTinyUSB.print("   Size: ");
    SerialTinyUSB.println(bufsize);
    if (show_hexdump && lba < DISK_BLOCK_NUM - 1) {
      hexDump(msc_disk[lba], MAX_DUMP_BYTES);
    }
    SerialTinyUSB.flush();
  }

  return bufsize;
}

// Callback invoked when WRITE10 command is completed (status received and accepted by host).
// used to flush any pending cache.
void msc_flush_callback(void) {
}

void scrollUp(uint8_t pixels) {
  // Read the current content of the display, shift it up by 'pixels' rows
  display.startscrollright(0x00, 0x07); // Dummy values to initiate scroll
  display.stopscroll(); // Immediately stop to manually shift pixels in memory
  for (int i = 0; i < display.height() - pixels; i++) {
    for (int j = 0; j < display.width(); j++) {
      uint8_t color = display.getPixel(j, i + pixels);
      display.drawPixel(j, i, color);
    }
  }

  // Clear the freed space after scrolling
  display.fillRect(0, display.height() - pixels, display.width(), pixels, SSD1306_BLACK);
  display.display();
}

void checkAndScroll() {
  // Assumes text height of 8 pixels, but check for 16 because newline is not used
  if ((display.getCursorY() + 16) > display.height()) {
    scrollUp(8);
    display.setCursor(0, display.getCursorY() - 8);
  }
}

// Queue-safe enqueue to avoid cross-core I2C usage
void enqueue_display(const char *str) {
  DisplayMsg msg;
  strncpy(msg.text, str, sizeof(msg.text) - 1);
  msg.text[sizeof(msg.text) - 1] = '\0';
  // If queue is full we drop the message silently to avoid blocking
  queue_try_add(&display_queue, &msg);
}

void printout(const char *str)
{
  checkAndScroll();
  display.print(str);
  display.display();

  SerialTinyUSB.println(str);
}

// Printed once whenever a serial terminal connects (see the DTR edge in loop()).
void print_banner() {
  SerialTinyUSB.println("\r\n======================================");
  SerialTinyUSB.printf(  "  %s\r\n", VERSION);
  SerialTinyUSB.println(  "  USB firewall / inspector");
  SerialTinyUSB.println(  "  Type HELP for commands");
  SerialTinyUSB.println("======================================\r\n");
}

// Updates the cached GUI message/icon and mirrors the same text over serial.
// Legacy generic-status entry point, now a thin wrapper over the GUI_Base
// generic-message screen (the old draw()/logo renderer is gone). Kept so any
// remaining/future caller still works; `logoIndex` is no longer used.
void showStatus(const char* str, uint8_t logoIndex) {
  (void)logoIndex;
  g_homeActive = false;                 // a transient status replaces the home screen
  g_noDeviceActive = false;             // ...and dismisses the "No device" screen
  const char* sanitized = str;
  while (*sanitized == '\n') {
    sanitized++;
  }
  strncpy(current_gui_message, sanitized, sizeof(current_gui_message) - 1);
  current_gui_message[sizeof(current_gui_message) - 1] = '\0';

  if (!descriptor_view_active) {
    guiGenericMsg(current_gui_message);
  }
  SerialTinyUSB.println(str);
}

// Shows the redesigned idle home screen and remembers it as the resting screen.
void showHome(const char* line) {
  strncpy(g_homeLine, line, sizeof(g_homeLine) - 1);
  g_homeLine[sizeof(g_homeLine) - 1] = '\0';
  g_homeActive = true;
  g_noDeviceActive = false;
  if (!descriptor_view_active) guiMainMenu(g_homeLine);
  SerialTinyUSB.println(line);
}

// Repaints the current resting screen: the mass-storage DETECTION screen (the
// options shown with no row highlighted) if a device is attached, else the home
// screen, else the last status message.
void redraw_resting() {
  if (g_massActive && (get_first_mounted_device() != NULL || g_mscPresent)) guiMassDetect();
  else if (g_homeActive) guiMainMenu(g_homeLine);
  else                   guiGenericMsg(current_gui_message);
}

dev_info_t* get_first_mounted_device() {
  for (int i = 0; i < CFG_TUH_DEVICE_MAX; i++) {
    if (dev_info[i].mounted) {
      return &dev_info[i];
    }
  }
  return NULL;
}

// Prints the currently selected descriptor page to both OLED and serial consoles.
void show_descriptor_page() {
  dev_info_t* dev = get_first_mounted_device();
  if (dev == NULL) {
    SerialTinyUSB.println("\r\n[!] No devices");
    const char* f[1] = { "(no device)" };
    guiDescPage("Desc [-/-]", f, 1);
    return;
  }

  tusb_desc_device_t *desc = &dev->desc_device;
  uint8_t page = descriptor_page % 3;

  // OLED: a short "Key:Val" per grid cell. Serial: the full, verbose lines.
  char f0[22], f1[22], f2[22], f3[22], f4[22], f5[22], f6[22];
  const char* fields[7] = {0};
  const char* title;
  int n = 0;

  if (page == 0) {
    title = "Desc [1/3]";
    snprintf(f0, sizeof(f0), "VID:%04X", desc->idVendor);
    snprintf(f1, sizeof(f1), "PID:%04X", desc->idProduct);
    snprintf(f2, sizeof(f2), "USB:%04X", desc->bcdUSB);
    snprintf(f3, sizeof(f3), "Dev:%04X", desc->bcdDevice);
    snprintf(f4, sizeof(f4), "Cls:%u", desc->bDeviceClass);
    snprintf(f5, sizeof(f5), "Sub:%u", desc->bDeviceSubClass);
    snprintf(f6, sizeof(f6), "Pro:%u", desc->bDeviceProtocol);
    fields[0]=f0; fields[1]=f1; fields[2]=f2; fields[3]=f3; fields[4]=f4; fields[5]=f5; fields[6]=f6; n=7;
    SerialTinyUSB.println("\r\n[Desc 1/3]");
    SerialTinyUSB.printf("VID:%04X PID:%04X\r\n", desc->idVendor, desc->idProduct);
    SerialTinyUSB.printf("USB:%04X Dev:%04X\r\n", desc->bcdUSB, desc->bcdDevice);
    SerialTinyUSB.printf("Cls:%u Sub:%u Proto:%u\r\n", desc->bDeviceClass, desc->bDeviceSubClass, desc->bDeviceProtocol);
  } else if (page == 1) {
    title = "Desc [2/3]";
    snprintf(f0, sizeof(f0), "Pkt0:%u", desc->bMaxPacketSize0);
    snprintf(f1, sizeof(f1), "Cfg:%u", desc->bNumConfigurations);
    snprintf(f2, sizeof(f2), "ManID:%u", desc->iManufacturer);
    snprintf(f3, sizeof(f3), "PrdID:%u", desc->iProduct);
    snprintf(f4, sizeof(f4), "SerID:%u", desc->iSerialNumber);
    fields[0]=f0; fields[1]=f1; fields[2]=f2; fields[3]=f3; fields[4]=f4; n=5;
    SerialTinyUSB.println("\r\n[Desc 2/3]");
    SerialTinyUSB.printf("Pkt0:%u Config:%u\r\n", desc->bMaxPacketSize0, desc->bNumConfigurations);
    SerialTinyUSB.printf("Manu ID:%u Prod ID:%u\r\n", desc->iManufacturer, desc->iProduct);
    SerialTinyUSB.printf("Serial ID:%u\r\n", desc->iSerialNumber);
  } else {
    title = "Desc [3/3]";
    // Long strings: use the left-column slots (0,2,4) full-width; leave 1,3 empty.
    snprintf(f0, sizeof(f0), "Mn:%.18s", (char *)dev->manufacturer);
    snprintf(f2, sizeof(f2), "Pd:%.18s", (char *)dev->product);
    snprintf(f4, sizeof(f4), "Sn:%.18s", (char *)dev->serial);
    fields[0]=f0; fields[1]=NULL; fields[2]=f2; fields[3]=NULL; fields[4]=f4; n=5;
    SerialTinyUSB.println("\r\n[Desc 3/3]");
    SerialTinyUSB.printf("Manu: %s\r\n", (char *)dev->manufacturer);
    SerialTinyUSB.printf("Prod: %s\r\n", (char *)dev->product);
    SerialTinyUSB.printf("Serial: %s\r\n", (char *)dev->serial);
  }

  guiDescPage(title, fields, n);
}

// Returns to the normal GUI once the descriptor view button is released.
void exit_descriptor_view() {
  descriptor_view_active = false;
  descriptor_page = 0;
  cls();
  redraw_resting();
}

// Clear display
void cls(void) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.display();
}

void reinit_display() {
  // Retry OLED initialization and redraw the cached header
  Wire.setSDA(4);
  Wire.setSCL(5);
  if (!display.begin(SSD1306_SWITCHCAPVCC, I2C_ADDRESS)) {
    SerialTinyUSB.println("OLED reinit failed");
    return;
  }
  cls();
  redraw_resting();
}

void hexDump(unsigned char* data, size_t size) {
  char asciitab[17];
  size_t i, j;
  asciitab[16] = '\0';

  for (i = 0; i < size; ++i) {

    SerialTinyUSB.print(data[i] >> 4, HEX);
    SerialTinyUSB.print(data[i] & 0x0F, HEX);

    if ((data)[i] >= ' ' && (data)[i] <= '~') {
      asciitab[i % 16] = (data)[i];
    } else {
      asciitab[i % 16] = '.';
    }
    if ((i + 1) % 8 == 0 || i + 1 == size) {
      SerialTinyUSB.print(" ");
      if ((i + 1) % 16 == 0) {
        SerialTinyUSB.println(asciitab);
      } else if (i + 1 == size) {
        asciitab[(i + 1) % 16] = '\0';
        if ((i + 1) % 16 <= 8) {
          SerialTinyUSB.print(" ");
        }
        for (j = (i + 1) % 16; j < 16; ++j) {
          SerialTinyUSB.print("   ");
        }
        SerialTinyUSB.print("|  ");
        SerialTinyUSB.println(asciitab);
      }
    }
  }
  SerialTinyUSB.println();
}

// Reset the Pico
void swreset() {
  watchdog_enable(1500, 1);
  while (1)
    ;
}

//
// Device info gathering 
//
static void print_device_descriptor(tuh_xfer_t *xfer);
void utf16_to_utf8(uint16_t *temp_buf, size_t buf_len);

// Invocado cuando cualquier dispositivo se monta (configurado)
void tuh_mount_cb(uint8_t daddr)
{
  SerialTinyUSB.printf("Device attached, address = %d\r\n", daddr);

  dev_info_t *dev = &dev_info[daddr - 1];
  dev->mounted = true;

  // Pide el Device Descriptor, cuando llegue se llama a print_device_descriptor()
  tuh_descriptor_get_device(daddr,
                            &dev->desc_device,
                            sizeof(tusb_desc_device_t),
                            print_device_descriptor,
                            0);
}

/// Invocado cuando el dispositivo se desmonta (reset/unplug)
void tuh_umount_cb(uint8_t daddr)
{
  SerialTinyUSB.printf("Device removed, address = %d\r\n", daddr);

  dev_info_t *dev = &dev_info[daddr - 1];
  dev->mounted = false;

  suspicious_device = false;
  suspicious_reported = false;
  suspicious_name[0] = '\0';
}

static void print_device_descriptor(tuh_xfer_t *xfer)
{
  if (XFER_RESULT_SUCCESS != xfer->result) {
    SerialTinyUSB.printf("Failed to get device descriptor\r\n");
    return;
  }

  uint8_t const daddr = xfer->daddr;
  dev_info_t *dev = &dev_info[daddr - 1];
  tusb_desc_device_t *desc = &dev->desc_device;

  save_device_info(dev, desc);

  const char* suspicious_match = check_suspicious_device(desc->idVendor, desc->idProduct);
  if (suspicious_match != NULL) {
    suspicious_device = true;
    suspicious_reported = false;
    strncpy(suspicious_name, suspicious_match, sizeof(suspicious_name) - 1);
    suspicious_name[sizeof(suspicious_name) - 1] = '\0';
    SerialTinyUSB.printf("\r\n[!!!] SUSPICIOUS DEVICE: %s\r\n", suspicious_match);

    last_device.was_suspicious = true;
    strncpy(last_device.suspicious_reason, suspicious_match, sizeof(last_device.suspicious_reason) - 1);
  }

  // Show a short summary on the OLED/serial right away
  char summary[48];
  SerialTinyUSB.printf("\r\nDevice %u: ID %04x:%04x\r\n",
                       daddr, desc->idVendor, desc->idProduct);

  SerialTinyUSB.printf("Device Descriptor:\r\n");
  SerialTinyUSB.printf("  bLength             %u\r\n"     , desc->bLength);
  SerialTinyUSB.printf("  bDescriptorType     %u\r\n"     , desc->bDescriptorType);
  SerialTinyUSB.printf("  bcdUSB              %04x\r\n"   , desc->bcdUSB);
  SerialTinyUSB.printf("  bDeviceClass        %u\r\n"     , desc->bDeviceClass);
  SerialTinyUSB.printf("  bDeviceSubClass     %u\r\n"     , desc->bDeviceSubClass);
  SerialTinyUSB.printf("  bDeviceProtocol     %u\r\n"     , desc->bDeviceProtocol);
  SerialTinyUSB.printf("  bMaxPacketSize0     %u\r\n"     , desc->bMaxPacketSize0);
  SerialTinyUSB.printf("  idVendor            0x%04x\r\n" , desc->idVendor);
  SerialTinyUSB.printf("  idProduct           0x%04x\r\n" , desc->idProduct);
  SerialTinyUSB.printf("  bcdDevice           %04x\r\n"   , desc->bcdDevice);
  // -------- Manufacturer String --------
  SerialTinyUSB.printf("  iManufacturer       %u", desc->iManufacturer);
  if (desc->iManufacturer &&
      XFER_RESULT_SUCCESS ==
        tuh_descriptor_get_manufacturer_string_sync(daddr,
                                                    LANGUAGE_ID,
                                                    dev->manufacturer,
                                                    sizeof(dev->manufacturer))) {

    utf16_to_utf8(dev->manufacturer, sizeof(dev->manufacturer));
    SerialTinyUSB.printf(" --- %s", (char *) dev->manufacturer);
  }
  SerialTinyUSB.printf("\r\n");

  // -------- Product String --------
  SerialTinyUSB.printf("  iProduct            %u", desc->iProduct);
  if (desc->iProduct &&
      XFER_RESULT_SUCCESS ==
        tuh_descriptor_get_product_string_sync(daddr,
                                               LANGUAGE_ID,
                                               dev->product,
                                               sizeof(dev->product))) {

    utf16_to_utf8(dev->product, sizeof(dev->product));
    SerialTinyUSB.printf(" --- %s", (char *) dev->product);
  }
  SerialTinyUSB.printf("\r\n");
  // -------- Serial Number String --------
  SerialTinyUSB.printf("  iSerialNumber       %u", desc->iSerialNumber);
  if (desc->iSerialNumber &&
      XFER_RESULT_SUCCESS ==
        tuh_descriptor_get_serial_string_sync(daddr,
                                              LANGUAGE_ID,
                                              dev->serial,
                                              sizeof(dev->serial))) {

    utf16_to_utf8(dev->serial, sizeof(dev->serial));
    SerialTinyUSB.printf(" --- %s", (char *) dev->serial);
  }
  SerialTinyUSB.printf("\r\n");
  SerialTinyUSB.printf("  bNumConfigurations  %u\r\n", desc->bNumConfigurations);
  SerialTinyUSB.printf("======================================\r\n");
}

//--------------------------------------------------------------------+
// String Descriptor Helper
//--------------------------------------------------------------------+

static void _convert_utf16le_to_utf8(const uint16_t *utf16, size_t utf16_len, uint8_t *utf8, size_t utf8_len) {
  // TODO: Check for runover.
  (void) utf8_len;
  // Get the UTF-16 length out of the data itself.

  for (size_t i = 0; i < utf16_len; i++) {
    uint16_t chr = utf16[i];
    if (chr < 0x80) {
      *utf8++ = chr & 0xff;
    } else if (chr < 0x800) {
      *utf8++ = (uint8_t) (0xC0 | (chr >> 6 & 0x1F));
      *utf8++ = (uint8_t) (0x80 | (chr >> 0 & 0x3F));
    } else {
      // TODO: Verify surrogate.
      *utf8++ = (uint8_t) (0xE0 | (chr >> 12 & 0x0F));
      *utf8++ = (uint8_t) (0x80 | (chr >> 6 & 0x3F));
      *utf8++ = (uint8_t) (0x80 | (chr >> 0 & 0x3F));
    }
    // TODO: Handle UTF-16 code points that take two entries.
  }
}

// Count how many bytes a utf-16-le encoded string will take in utf-8.
static int _count_utf8_bytes(const uint16_t *buf, size_t len) {
  size_t total_bytes = 0;
  for (size_t i = 0; i < len; i++) {
    uint16_t chr = buf[i];
    if (chr < 0x80) {
      total_bytes += 1;
    } else if (chr < 0x800) {
      total_bytes += 2;
    } else {
      total_bytes += 3;
    }
    // TODO: Handle UTF-16 code points that take two entries.
  }
  return total_bytes;
}

void utf16_to_utf8(uint16_t *temp_buf, size_t buf_len) {
  size_t utf16_len = ((temp_buf[0] & 0xff) - 2) / sizeof(uint16_t);
  size_t utf8_len = _count_utf8_bytes(temp_buf + 1, utf16_len);

  _convert_utf16le_to_utf8(temp_buf + 1, utf16_len, (uint8_t *) temp_buf, buf_len);
  ((uint8_t *) temp_buf)[utf8_len] = '\0';
}

//
// BADUSB detector section
//

static uint8_t const keycode2ascii[128][2] = { HID_KEYCODE_TO_ASCII };

// Invoked when device with hid interface is mounted
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len) {

  uint16_t vid, pid;
  const char* protocol_str[] = { "None", "Keyboard", "Mouse" };

  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  tuh_vid_pid_get(dev_addr, &vid, &pid);

  // Defer the OLED draw to core0 (I2C is core0-only): flag it and stash the name.
  strncpy(g_hidMountName, protocol_str[itf_protocol], sizeof(g_hidMountName) - 1);
  g_hidMountName[sizeof(g_hidMountName) - 1] = '\0';
  g_hidMountReq = true;

  SerialTinyUSB.printf("HID device address = %d, instance = %d mounted\r\n", dev_addr, instance);
  SerialTinyUSB.printf("VID = %04x, PID = %04x\r\n", vid, pid);
  SerialTinyUSB.printf("HID Interface Protocol = %s\r\n", protocol_str[itf_protocol]);

  if (!tuh_hid_receive_report(dev_addr, instance)) {
    SerialTinyUSB.printf("Error: cannot request to receive report\r\n");
  }
}

// Invoked when device with hid interface is un-mounted
void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
  SerialTinyUSB.printf("HID device address = %d, instance = %d unmounted\r\n", dev_addr, instance);

  hid_sent = false;
  hid_reported = false;
  hid_event_num = 0;
}

// Invoked when received report from device
void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {

  static bool kbd_printed = false;
  static bool mouse_printed = false;

  // Used in main loop to write output to OLED
  hid_sent = true;

  uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

  switch (itf_protocol) {
    case HID_ITF_PROTOCOL_KEYBOARD:
      if (kbd_printed == false) {
        SerialTinyUSB.println("HID received keyboard report");
        kbd_printed = true;
        mouse_printed = false;
      }
      process_kbd_report((hid_keyboard_report_t const*)report);
      hid_event_num++;
      break;

    case HID_ITF_PROTOCOL_MOUSE:
      if (kbd_printed == false) {
        SerialTinyUSB.println("HID receive mouse report");
        mouse_printed = true;
        kbd_printed = false;
      }
      process_mouse_report((hid_mouse_report_t const*)report);
      hid_event_num++;
      break;

    default:
      // Generic report: for the time being we use kbd for this as well
      process_kbd_report((hid_keyboard_report_t const*)report);
      hid_event_num++;
      break;
  }

  if (!tuh_hid_receive_report(dev_addr, instance)) {
    SerialTinyUSB.println("Error: cannot request to receive report");
  }
}

static inline bool find_key_in_report(hid_keyboard_report_t const* report, uint8_t keycode) {
  for (uint8_t i = 0; i < 6; i++) {
    if (report->keycode[i] == keycode) return true;
  }

  return false;
}

static void process_kbd_report(hid_keyboard_report_t const* report) {
  // Previous report to check key released
  static hid_keyboard_report_t prev_report = { 0, 0, { 0 } };

  // -----------------------------------------------------------------------
  // Spurious-CTRL detection
  //
  // Some devices (Flipper Zero BadKB, certain RP2040 firmwares) set the CTRL
  // modifier bit on *every* HID report, including ordinary STRING characters.
  // We count consecutive keypresses (reports that actually have keycodes) that
  // carry CTRL in the modifier.  Once the run reaches CTRL_SPURIOUS_THRESHOLD
  // we enter suppress-mode and omit the "CTRL+" prefix from serial output.
  // Suppress-mode is cleared as soon as any keypress arrives WITHOUT CTRL.
  //
  // Reports with no active keycodes (key-release reports) are ignored so
  // they don't artificially reset the counter between rapid keystrokes.
  // -----------------------------------------------------------------------
  #define CTRL_SPURIOUS_THRESHOLD 3
  static uint8_t  ctrl_seq_count   = 0;
  static bool     ctrl_is_spurious = false;

  bool report_has_ctrl = (report->modifier &
      (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) != 0;

  // Check if this report actually contains any pressed key
  bool report_has_key = false;
  for (uint8_t i = 0; i < 6; i++) {
    if (report->keycode[i]) { report_has_key = true; break; }
  }

  if (report_has_key) {
    if (report_has_ctrl) {
      if (ctrl_seq_count < 255) ctrl_seq_count++;
      if (!ctrl_is_spurious && ctrl_seq_count >= CTRL_SPURIOUS_THRESHOLD) {
        ctrl_is_spurious = true;
        SerialTinyUSB.print(" [CTRL-filtered]");
      }
    } else {
      // A real keypress without CTRL — device is sending clean data again
      if (ctrl_is_spurious) {
        ctrl_is_spurious = false;
        if (show_hid_debug) {
          SerialTinyUSB.print(" [CTRL-filter-off]");
        }
      }
      ctrl_seq_count = 0;
    }
  }
  // (reports with no keycodes leave the counter unchanged)

  if (show_hid_debug && report->modifier != 0) {
    SerialTinyUSB.printf("[mod=0x%02X]", report->modifier);
  }

  for (uint8_t i = 0; i < 6; i++) {
    if (report->keycode[i]) {
      if (find_key_in_report(&prev_report, report->keycode[i])) {
        // Exist in previous report means the current key is holding
      } else {
        // Not existed in previous report means the current key is pressed
        uint8_t keycode = report->keycode[i];

        if (show_hid_debug) {
          SerialTinyUSB.printf("[key=0x%02X]", keycode);
        }

        update_hid_speed();
        last_device.hid_events++;

        // Check for modifiers - use explicit masks to avoid false positives
        bool const is_shift = (report->modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;
        uint8_t ch = keycode2ascii[keycode][is_shift ? 1 : 0];

        uint8_t mod = report->modifier;

        // Mask out the modifier bit if this keycode IS a modifier key being pressed
        // (to avoid "CTRL+<CTRL>" when just pressing CTRL)
        if (keycode == HID_KEY_CONTROL_LEFT || keycode == HID_KEY_CONTROL_RIGHT) {
          mod &= ~(KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL);
        }
        if (keycode == HID_KEY_GUI_LEFT || keycode == HID_KEY_GUI_RIGHT) {
          mod &= ~(KEYBOARD_MODIFIER_LEFTGUI | KEYBOARD_MODIFIER_RIGHTGUI);
        }
        if (keycode == HID_KEY_ALT_LEFT || keycode == HID_KEY_ALT_RIGHT) {
          mod &= ~(KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT);
        }

        bool const is_ctrl = (mod & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) != 0;
        bool const is_gui  = (mod & (KEYBOARD_MODIFIER_LEFTGUI  | KEYBOARD_MODIFIER_RIGHTGUI))  != 0;
        bool const is_alt  = (mod & (KEYBOARD_MODIFIER_LEFTALT  | KEYBOARD_MODIFIER_RIGHTALT))  != 0;

        // Resolve the key to a display marker WITHOUT printing yet.
        // This fixes the output-order bug: previously is_special_key() printed the key
        // as a side effect before the modifier prefix was emitted, resulting in output
        // like "<ENTER>\nCTRL+" instead of "CTRL+<ENTER>\n".
        const char* marker = get_special_key_marker(keycode);
        bool const is_special  = (marker != NULL);
        bool const is_printable = (!is_special) && (ch >= 0x20 && ch < 0x7F);
        bool const is_ctrl_char = (!is_special) && (!is_printable) &&
                                   (ch == '\r' || ch == '\t' || ch == '\b' || ch == 0x1B);

        bool const has_output = is_special || is_printable || is_ctrl_char;
        if (!has_output) continue;

        // 1. Print modifier prefix FIRST (always before the key).
        //    Suppress CTRL+ when the spurious-CTRL detector is active.
        bool const print_ctrl = is_ctrl && !ctrl_is_spurious;
        if (print_ctrl || is_gui || is_alt) {
          if (print_ctrl) SerialTinyUSB.print("CTRL+");
          if (is_gui)     SerialTinyUSB.print("GUI+");
          if (is_alt)     SerialTinyUSB.print("ALT+");
        }

        // 2. Print the key / marker AFTER the modifiers
        if (is_special) {
          SerialTinyUSB.print(marker);
          // Newline after ENTER so the next keystroke starts on a fresh line
          if (keycode == HID_KEY_ENTER || keycode == HID_KEY_KEYPAD_ENTER) {
            SerialTinyUSB.print("\r\n");
          }
        } else if (is_printable) {
          SerialTinyUSB.printf("%c", ch);
        } else if (ch == '\r') {
          SerialTinyUSB.print("<ENTER>\r\n");
        } else if (ch == '\t') {
          SerialTinyUSB.print("<TAB>");
        } else if (ch == '\b' || ch == 0x08) {
          SerialTinyUSB.print("<BACKSPACE>");
        } else if (ch == 0x1B) {
          SerialTinyUSB.print("<ESC>");
        }

        fflush(stdout);
      }
    }
  }

  prev_report = *report;
}

// Returns the display marker for a special key, or NULL if it is not a special key.
// Does NOT print anything — the caller is responsible for printing modifiers first,
// then this marker, so the output order is always correct.
static const char* get_special_key_marker(uint8_t code) {
  switch (code) {
    // Arrow keys
    case HID_KEY_ARROW_RIGHT: return "<ARROWRIGHT>";
    case HID_KEY_ARROW_LEFT:  return "<ARROWLEFT>";
    case HID_KEY_ARROW_DOWN:  return "<ARROWDOWN>";
    case HID_KEY_ARROW_UP:    return "<ARROWUP>";

    // Navigation
    case HID_KEY_HOME:      return "<HOME>";
    case HID_KEY_END:       return "<END>";
    case HID_KEY_PAGE_UP:   return "<PAGEUP>";
    case HID_KEY_PAGE_DOWN: return "<PAGEDOWN>";
    case HID_KEY_INSERT:    return "<INSERT>";
    case HID_KEY_DELETE:    return "<DEL>";

    // Function keys
    case HID_KEY_F1:  return "<F1>";
    case HID_KEY_F2:  return "<F2>";
    case HID_KEY_F3:  return "<F3>";
    case HID_KEY_F4:  return "<F4>";
    case HID_KEY_F5:  return "<F5>";
    case HID_KEY_F6:  return "<F6>";
    case HID_KEY_F7:  return "<F7>";
    case HID_KEY_F8:  return "<F8>";
    case HID_KEY_F9:  return "<F9>";
    case HID_KEY_F10: return "<F10>";
    case HID_KEY_F11: return "<F11>";
    case HID_KEY_F12: return "<F12>";

    // Keypad — return the digit/operator string directly so the caller can print
    // modifier prefix before it (e.g. CTRL+1 would have been broken before this fix)
    case HID_KEY_KEYPAD_1:        return "1";
    case HID_KEY_KEYPAD_2:        return "2";
    case HID_KEY_KEYPAD_3:        return "3";
    case HID_KEY_KEYPAD_4:        return "4";
    case HID_KEY_KEYPAD_5:        return "5";
    case HID_KEY_KEYPAD_6:        return "6";
    case HID_KEY_KEYPAD_7:        return "7";
    case HID_KEY_KEYPAD_8:        return "8";
    case HID_KEY_KEYPAD_9:        return "9";
    case HID_KEY_KEYPAD_0:        return "0";
    case HID_KEY_KEYPAD_DECIMAL:  return ".";
    case HID_KEY_KEYPAD_DIVIDE:   return "/";
    case HID_KEY_KEYPAD_MULTIPLY: return "*";
    case HID_KEY_KEYPAD_SUBTRACT: return "-";
    case HID_KEY_KEYPAD_ADD:      return "+";
    case HID_KEY_KEYPAD_ENTER:    return "<ENTER>";

    // System keys
    case HID_KEY_PRINT_SCREEN: return "<PRNT>";
    case HID_KEY_SCROLL_LOCK:  return "<SCRLL>";
    case HID_KEY_PAUSE:        return "<PAUSE>";
    case HID_KEY_NUM_LOCK:     return "<NUMLOCK>";
    case HID_KEY_CAPS_LOCK:    return "<CAPSLOCK>";
    case HID_KEY_ESCAPE:       return "<ESC>";

    // Modifier keys (when pressed alone, i.e. no other key in the report)
    case HID_KEY_CONTROL_LEFT:
    case HID_KEY_CONTROL_RIGHT: return "<CTRL>";
    case HID_KEY_ALT_LEFT:
    case HID_KEY_ALT_RIGHT:     return "<ALT>";
    case HID_KEY_GUI_LEFT:
    case HID_KEY_GUI_RIGHT:     return "<WIN>";
    case HID_KEY_SHIFT_LEFT:
    case HID_KEY_SHIFT_RIGHT:   return "<SHIFT>";
    case HID_KEY_APPLICATION:   return "<MENU>";

    // Space, Enter, Tab, Backspace
    case HID_KEY_SPACE:     return " ";
    case HID_KEY_ENTER:     return "<ENTER>";
    case HID_KEY_TAB:       return "<TAB>";
    case HID_KEY_BACKSPACE: return "<BACKSPACE>";

    default:
      return NULL;  // Not a special key
  }
}

static void process_mouse_report(hid_mouse_report_t const* report) {
  static hid_mouse_report_t prev_report = { 0 };

  uint8_t button_changed_mask = report->buttons ^ prev_report.buttons;
  if (button_changed_mask & report->buttons) {
    SerialTinyUSB.printf("MOUSE: %c%c%c ",
                         report->buttons & MOUSE_BUTTON_LEFT ? 'L' : '-',
                         report->buttons & MOUSE_BUTTON_MIDDLE ? 'M' : '-',
                         report->buttons & MOUSE_BUTTON_RIGHT ? 'R' : '-');
  }

  cursor_movement(report->x, report->y, report->wheel);
}

// Logs relative mouse movement for debugging.
void cursor_movement(int8_t x, int8_t y, int8_t wheel) {
  SerialTinyUSB.printf("(%d %d %d)\r\n", x, y, wheel);
}

// Sets the RGB status pixel to the requested color.
void setNeoColor(uint8_t r, uint8_t g, uint8_t b) {
  statusPixel.setPixelColor(0, statusPixel.Color(r, g, b));
  statusPixel.show();
}


//
// OTHER Host devices detection section
//

// Invoked when a device with MassStorage interface is mounted
void tuh_msc_mount_cb(uint8_t dev_addr) {
  // Flag the mass-storage DETECTION state. Do NOT draw here: this runs on core1,
  // and the OLED (I2C) is driven by core0 -- a cross-core draw races on the Wire
  // bus. core0 sees g_massActive change in loop() and paints the screen.
  g_homeActive = false;
  g_noDeviceActive = false;
  g_massActive = true;
  setNeoColor(0, 255, 0);
  SerialTinyUSB.printf("Mass Device attached, address = %d\r\n", dev_addr);

  // Remember it so a "Bak" hold (or the WIPE serial command) can wipe its tables.
  // We deliberately DO NOT wipe automatically on mount: the user must ask for it.
  msc_dev_addr        = dev_addr;
  msc_mounted         = true;
  wipe.is_wiping      = false;
  wipe_in_progress    = false;
  wipe_requested      = false;
  wipe_done           = false;
  wipe_error          = false;
  wipe_done_reported  = false;
  wipe_error_reported = false;

  // Mount its filesystem (read-only) so its contents can be inspected before
  // the drive is trusted on the real PC. Safe to do the blocking sync reads
  // here: this callback already runs on core1 inside USBHost.task().
  explorer_mount(dev_addr);
}

// Invoked when a device with MassStorage interface is unmounted
void tuh_msc_umount_cb(uint8_t dev_addr) {
  SerialTinyUSB.printf("Mass Device unmounted, address = %d\r\n", dev_addr);
  if (dev_addr == msc_dev_addr) {
    msc_mounted      = false;
    wipe.is_wiping   = false;
    wipe_in_progress = false;
    wipe_requested   = false;
  }
  explorer_umount(dev_addr);
  g_massActive = false;                 // device gone; core0 repaints home (see loop)
}

//
// Partition-table wiper (runs entirely on core1 / host stack)
//

// Picks up a wipe request from core0 and kicks off the write chain.
void service_wipe() {
  if (!wipe_requested) {
    return;
  }
  wipe_requested = false;

  if (wipe.is_wiping) {
    return;  // Already running
  }
  if (!msc_mounted || !tuh_msc_mounted(msc_dev_addr)) {
    SerialTinyUSB.println("[WIPE] Aborted: no mass-storage device mounted");
    return;
  }

  // Our zero buffer is WIPE_BLOCK_SIZE bytes; refuse anything else to stay safe
  uint32_t bsize = tuh_msc_get_block_size(msc_dev_addr, 0);
  if (bsize != WIPE_BLOCK_SIZE) {
    SerialTinyUSB.printf("[WIPE] Unsupported block size %lu, aborting\r\n", (unsigned long)bsize);
    wipe_error = true;
    return;
  }

  memset(wipe.buffer, 0, sizeof(wipe.buffer));
  wipe.dev_addr    = msc_dev_addr;
  wipe.block_count = tuh_msc_get_block_count(msc_dev_addr, 0);
  wipe.current_lba = 0;
  wipe.phase       = 0;
  wipe.is_wiping   = true;
  wipe_in_progress = true;
  wipe_done        = false;
  wipe_error       = false;

  SerialTinyUSB.printf("[WIPE] Starting on addr %u (%lu blocks)\r\n",
                       wipe.dev_addr, (unsigned long)wipe.block_count);
  start_wipe_process(wipe.dev_addr);
}

void start_wipe_process(uint8_t dev_addr) {
  (void)dev_addr;
  SerialTinyUSB.printf("[WIPE] Zeroing LBA 0..%d\r\n", WIPE_LIMIT_LBA);
  process_next_block();
}

// Issues a single-block zero write; the completion callback chains the next one.
void process_next_block() {
  if (!tuh_msc_mounted(wipe.dev_addr)) {
    SerialTinyUSB.println("[WIPE] Device vanished mid-wipe");
    wipe.is_wiping   = false;
    wipe_in_progress = false;
    wipe_error       = true;
    return;
  }

  bool ok = tuh_msc_write10(wipe.dev_addr, 0, wipe.buffer,
                            wipe.current_lba, 1, wipe_write_complete_cb, 0);
  if (!ok) {
    SerialTinyUSB.printf("[WIPE] write10 failed at LBA %lu\r\n", (unsigned long)wipe.current_lba);
    wipe.is_wiping   = false;
    wipe_in_progress = false;
    wipe_error       = true;
  }
}

// Invoked by the host stack when a WRITE10 completes. Advances the state machine:
// phase 0 zeroes the head of the disk, then jumps to zero the tail (GPT backup).
bool wipe_write_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const* cb_data) {
  (void)dev_addr;

  if (cb_data->csw->status != 0) {
    SerialTinyUSB.printf("[WIPE] Write error at LBA %lu (status %d)\r\n",
                         (unsigned long)wipe.current_lba, cb_data->csw->status);
    wipe.is_wiping   = false;
    wipe_in_progress = false;
    wipe_error       = true;
    return true;
  }

  wipe.current_lba++;

  if (wipe.phase == 0) {
    if (wipe.current_lba >= WIPE_LIMIT_LBA) {
      if (wipe.block_count > (uint32_t)WIPE_LIMIT_LBA * 2) {
        // Jump to the tail to erase the GPT backup header + partition array
        wipe.phase       = 1;
        wipe.current_lba = wipe.block_count - WIPE_LIMIT_LBA;
        process_next_block();
      } else {
        wipe.is_wiping   = false;
        wipe_in_progress = false;
        wipe_done        = true;
        SerialTinyUSB.printf("[WIPE] Done, wiped %d blocks\r\n", WIPE_LIMIT_LBA);
      }
    } else {
      process_next_block();
    }
  } else {  // phase 1: tail of the disk
    if (wipe.current_lba >= wipe.block_count) {
      wipe.is_wiping   = false;
      wipe_in_progress = false;
      wipe_done        = true;
      SerialTinyUSB.printf("[WIPE] Done, wiped first and last %d blocks\r\n", WIPE_LIMIT_LBA);
    } else {
      process_next_block();
    }
  }

  return true;
}

// Invoked when a device with CDC (Communication Device Class) interface is mounted
void tuh_cdc_mount_cb(uint8_t idx) {
  // Defer the OLED draw to core0 (I2C is core0-only): just flag it here.
  g_cdcMountReq = true;
  SerialTinyUSB.printf("CDC Device attached, idx = %d\r\n", idx);
}

// Invoked when a device with CDC (Communication Device Class) interface is unmounted
void tuh_cdc_umount_cb(uint8_t idx) {
  SerialTinyUSB.printf("CDC Device unmounted, idx = %d\r\n", idx);
}


//
// Serial Command Processing
//
void process_serial_command(const char* cmd) {
  char upper[SERIAL_BUFFER_SIZE];
  for (int i = 0; cmd[i] && i < SERIAL_BUFFER_SIZE - 1; i++) {
    upper[i] = toupper(cmd[i]);
    upper[i + 1] = '\0';
  }

  // Commands that take an argument (a filesystem path) need the argument in its
  // ORIGINAL case, so we can't use the fully-uppercased buffer for them. Split
  // off the first word as the (case-insensitive) keyword and keep a pointer to
  // the original-case remainder as the argument.
  char keyword[16] = {0};
  const char* arg = cmd;
  while (*arg == ' ') arg++;
  {
    int k = 0;
    while (arg[k] && arg[k] != ' ' && k < (int)sizeof(keyword) - 1) {
      keyword[k] = toupper(arg[k]);
      k++;
    }
    keyword[k] = '\0';
    arg += k;
    while (*arg == ' ') arg++;   // arg now points at the (possibly empty) argument
  }

  if (strcmp(keyword, "LS") == 0 || strcmp(keyword, "DIR") == 0) {
    explorer_ls(arg[0] ? arg : "/");
    return;
  }
  else if (strcmp(keyword, "CAT") == 0 || strcmp(keyword, "TYPE") == 0) {
    if (!arg[0]) {
      SerialTinyUSB.println("[!] Usage: CAT <path>");
    } else {
      explorer_cat(arg);
    }
    return;
  }
  else if (strcmp(keyword, "SCAN") == 0) {
    explorer_scan();
    return;
  }
  else if (strcmp(keyword, "INSPECT") == 0) {
    if (!arg[0]) SerialTinyUSB.println("[!] Usage: INSPECT <path>");
    else         explorer_inspect(arg);
    return;
  }
  else if (strcmp(keyword, "BROWSE") == 0) {
    explorer_analyze();
    return;
  }

  if (strcmp(upper, "HELP") == 0 || strcmp(upper, "?") == 0) {
    SerialTinyUSB.println("\r\n=== CERBERUS COMMANDS ===");
    SerialTinyUSB.println("HELP     - Show this help");
    SerialTinyUSB.println("STATUS   - Show current status");
    SerialTinyUSB.println("LAST     - Show last device info (forensics)");
    SerialTinyUSB.println("RESET    - Reset counters");
    SerialTinyUSB.println("REBOOT   - Reboot device");
    SerialTinyUSB.println("VERBOSE  - Toggle verbose mode");
    SerialTinyUSB.println("HEXDUMP  - Toggle hexdump output");
    SerialTinyUSB.println("HIDDEBUG - Toggle HID raw debug");
    SerialTinyUSB.println("CLEAR    - Clear last device info");
    SerialTinyUSB.println("WIPE     - Destroy partition tables of mounted USB");
    SerialTinyUSB.println("LS [path]- List dir on USB (LS / = volume list; LS /N = volume N)");
    SerialTinyUSB.println("CAT path - Preview a file (text + hex) on the USB");
    SerialTinyUSB.println("SCAN     - Recursively list files + type + archive/polyglot checks");
    SerialTinyUSB.println("INSPECT p- Deep look inside one file (zip entries + 1MB sig sweep)");
    SerialTinyUSB.println("BROWSE   - Open the file analyzer on the OLED");
    SerialTinyUSB.println("=========================\r\n");
  }
  else if (strcmp(upper, "STATUS") == 0) {
    print_status();
  }
  else if (strcmp(upper, "LAST") == 0) {
    print_last_device();
  }
  else if (strcmp(upper, "RESET") == 0) {
    hid_event_num = 0;
    hid_window_count = 0;
    hid_current_speed = 0;
    hid_speed_alert = false;
    hid_speed_reported = false;
    written_reported = false;
    deleted_reported = false;
    hid_reported = false;
    suspicious_reported = false;
    SerialTinyUSB.println("[+] Counters reset");
    guiGenericMsg("Reset OK");
  }
  else if (strcmp(upper, "REBOOT") == 0) {
    SerialTinyUSB.println("[+] Rebooting...");
    guiReboot();
    delay(500);
    swreset();
  }
  else if (strcmp(upper, "VERBOSE") == 0) {
    verbose_mode = !verbose_mode;
    SerialTinyUSB.printf("[+] Verbose mode: %s\r\n", verbose_mode ? "ON" : "OFF");
  }
  else if (strcmp(upper, "HEXDUMP") == 0) {
    show_hexdump = !show_hexdump;
    SerialTinyUSB.printf("[+] Hexdump: %s\r\n", show_hexdump ? "ON" : "OFF");
  }
  else if (strcmp(upper, "HIDDEBUG") == 0) {
    show_hid_debug = !show_hid_debug;
    SerialTinyUSB.printf("[+] HID Debug: %s\r\n", show_hid_debug ? "ON" : "OFF");
    if (show_hid_debug) {
      SerialTinyUSB.println("    Format: [mod=XX key=YY] where mod is modifier byte");
    }
  }
  else if (strcmp(upper, "CLEAR") == 0) {
    memset(&last_device, 0, sizeof(last_device));
    SerialTinyUSB.println("[+] Last device info cleared");
  }
  else if (strcmp(upper, "WIPE") == 0) {
    if (!msc_mounted) {
      SerialTinyUSB.println("[!] No mass-storage device mounted");
    } else if (wipe_in_progress) {
      SerialTinyUSB.println("[!] Wipe already in progress");
    } else {
      SerialTinyUSB.println("[+] Wipe requested: destroying partition tables");
      wipe_done = false;
      wipe_error = false;
      wipe_done_reported = false;
      wipe_error_reported = false;
      wipe_requested = true;
      guiWipingProgress();
      setNeoColor(255, 165, 0);
    }
  }
  else if (strlen(upper) > 0) {
    SerialTinyUSB.printf("[!] Unknown command: %s (type HELP)\r\n", cmd);
  }
}

void print_status() {
  SerialTinyUSB.println("\r\n=== CERBERUS STATUS ===");
  SerialTinyUSB.printf("Version: %s\r\n", VERSION);
  SerialTinyUSB.printf("Uptime: %lu ms\r\n", to_ms_since_boot(get_absolute_time()));
  SerialTinyUSB.printf("HID Events: %u\r\n", hid_event_num);
  SerialTinyUSB.printf("HID Speed: %.1f keys/sec\r\n", hid_current_speed);
  SerialTinyUSB.printf("Speed Alert: %s\r\n", hid_speed_alert ? "YES" : "no");
  SerialTinyUSB.printf("Verbose: %s\r\n", verbose_mode ? "ON" : "OFF");
  SerialTinyUSB.printf("Hexdump: %s\r\n", show_hexdump ? "ON" : "OFF");

  dev_info_t* dev = get_first_mounted_device();
  if (dev != NULL) {
    SerialTinyUSB.printf("Connected: VID=%04X PID=%04X\r\n",
                         dev->desc_device.idVendor, dev->desc_device.idProduct);
  } else {
    SerialTinyUSB.println("Connected: none");
  }
  SerialTinyUSB.println("=======================\r\n");
}

void print_last_device() {
  SerialTinyUSB.println("\r\n=== LAST DEVICE (FORENSICS) ===");
  if (last_device.vid == 0 && last_device.pid == 0) {
    SerialTinyUSB.println("No device recorded");
  } else {
    SerialTinyUSB.printf("VID:PID      : %04X:%04X\r\n", last_device.vid, last_device.pid);
    SerialTinyUSB.printf("Class        : %u/%u\r\n", last_device.device_class, last_device.device_subclass);
    SerialTinyUSB.printf("Manufacturer : %s\r\n", last_device.manufacturer);
    SerialTinyUSB.printf("Product      : %s\r\n", last_device.product);
    SerialTinyUSB.printf("Serial       : %s\r\n", last_device.serial);
    SerialTinyUSB.printf("HID Events   : %u\r\n", last_device.hid_events);
    SerialTinyUSB.printf("Max Speed    : %.1f keys/sec\r\n", last_device.max_hid_speed);
    SerialTinyUSB.printf("Suspicious   : %s\r\n", last_device.was_suspicious ? "YES" : "no");
    if (last_device.was_suspicious) {
      SerialTinyUSB.printf("Reason       : %s\r\n", last_device.suspicious_reason);
    }
    SerialTinyUSB.printf("Timestamp    : %lu ms after boot\r\n", last_device.timestamp);
  }
  SerialTinyUSB.println("===============================\r\n");
}

void save_device_info(dev_info_t* dev, tusb_desc_device_t* desc) {
  last_device.vid = desc->idVendor;
  last_device.pid = desc->idProduct;
  last_device.device_class = desc->bDeviceClass;
  last_device.device_subclass = desc->bDeviceSubClass;
  strncpy(last_device.manufacturer, (char*)dev->manufacturer, sizeof(last_device.manufacturer) - 1);
  strncpy(last_device.product, (char*)dev->product, sizeof(last_device.product) - 1);
  strncpy(last_device.serial, (char*)dev->serial, sizeof(last_device.serial) - 1);
  last_device.timestamp = to_ms_since_boot(get_absolute_time());
  last_device.hid_events = 0;
  last_device.max_hid_speed = 0;
  last_device.was_suspicious = false;
  last_device.suspicious_reason[0] = '\0';
}

void update_hid_speed() {
  uint32_t now = to_ms_since_boot(get_absolute_time());

  if (now - hid_window_start > HID_SPEED_WINDOW_MS) {
    if (hid_window_count > 0) {
      hid_current_speed = (float)hid_window_count * 1000.0f / (float)HID_SPEED_WINDOW_MS;

      if (hid_current_speed > HID_SPEED_THRESHOLD && !hid_speed_alert) {
        hid_speed_alert = true;
        hid_speed_reported = false;
      }
    }
    hid_window_start = now;
    hid_window_count = 0;
  }

  hid_window_count++;
}

//
// Suspicious device detection
//
const char* check_suspicious_device(uint16_t vid, uint16_t pid) {
  for (size_t i = 0; i < SUSPICIOUS_COUNT; i++) {
    // RP2040 has memory-mapped flash, direct access works
    const suspicious_device_t* dev = &SUSPICIOUS_DEVICES[i];

    if (dev->vid == vid) {
      // If PID is 0x0000, match any PID for this VID
      if (dev->pid == 0x0000 || dev->pid == pid) {
        return dev->name;
      }
    }
  }
  return NULL;
}

// GUI renderer: draws a static frame plus the selected icon and status text.
