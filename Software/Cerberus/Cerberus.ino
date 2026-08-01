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
#include "resources.h"

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

typedef struct {
  char text[64];
} DisplayMsg;

void cls(void);
void show_descriptor_page();
void exit_descriptor_view();
dev_info_t* get_first_mounted_device();
void reinit_display();
void showStatus(const char* message, uint8_t logoIndex);
void draw(const char* text, uint8_t logoIndex);

// Define the dimension of RAM DISK. We have a "real" one (for which
// a real array is created) and a "fake" one, presented to the OS
#define DISK_BLOCK_NUM 0x150
#define FAKE_DISK_BLOCK_NUM 0x800
#define DISK_BLOCK_SIZE 0x200
#include "ramdisk.h"

Adafruit_USBD_MSC usb_msc;

// Manual reset button wired GP3 to GND
#define BTN_RST 3

// Manual OK button wired GP6 to GND
#define BTN_OK 6

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
uint8_t current_logo_index = LOGO_USB_CONNECTED;

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
boolean verbose_mode = true;        // Default: verbose on
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

  pinMode(BTN_RST, INPUT_PULLUP);
  pinMode(BTN_OK, INPUT_PULLUP);

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
  draw(current_gui_message, current_logo_index);
  statusPixel.begin();
  statusPixel.setBrightness(50);
  setNeoColor(0, 0, 255);  // Blue as the initial neutral status

  if (computed_hash == valid_hash) {
    showStatus("Selftest: OK", LOGO_USB_CONNECTED);
  } else {
    showStatus("Selftest: KO", LOGO_ALERT_SYMBOL);
    showStatus("Stopping...", LOGO_ALERT_SYMBOL);
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
    if (descriptor_view_active) {
      SerialTinyUSB.println(msg.text);
    } else {
      showStatus(msg.text, LOGO_USB_CONNECTED);
    }
  }

  //
  // "Bak" button (BTN_RST):
  //   - short press -> browse the USB descriptor pages (original behaviour)
  //   - hold >= WIPE_HOLD_MS while a mass-storage device is mounted -> wipe it
  //
  static bool     rst_prev = true;
  static uint32_t bak_press_start = 0;
  static bool     bak_wipe_fired = false;   // Ensures a single wipe per hold
  static bool     bak_countdown = false;    // A wipe countdown is on screen
  static uint32_t bak_last_hint = 0;        // Throttles the countdown redraws
  bool rst_now = digitalRead(BTN_RST);
  if (rst_now != rst_prev) {
    SerialTinyUSB.printf("BTN_RST %s\n", rst_now == LOW ? "PRESSED -> descriptors" : "released");
    rst_prev = rst_now;
    if (rst_now == LOW) {
      bak_press_start = to_ms_since_boot(get_absolute_time());
      bak_wipe_fired = false;
      bak_countdown = false;
      bak_last_hint = 0;
      if (descriptor_view_active == false) {
        descriptor_view_active = true;
        descriptor_page = 0;
      } else {
        descriptor_page++;
      }
      show_descriptor_page();
    } else {
      bak_press_start = 0;
      // Released in time while a wipe countdown was showing: cancel and restore
      if (bak_countdown && !bak_wipe_fired && !wipe_in_progress) {
        bak_countdown = false;
        descriptor_view_active = false;
        descriptor_page = 0;
        draw(current_gui_message, current_logo_index);
      }
    }
  }

  // Sustained hold detection: only arms while a mass-storage device is mounted.
  if (rst_now == LOW && bak_press_start != 0 && !bak_wipe_fired &&
      msc_mounted && !wipe_in_progress) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t held = now - bak_press_start;
    if (held >= WIPE_HOLD_MS) {
      // Fire the wipe: core1 (host stack) picks up wipe_requested in loop1()
      bak_wipe_fired = true;
      bak_countdown = false;
      descriptor_view_active = false;
      descriptor_page = 0;
      wipe_done = false;
      wipe_error = false;
      wipe_done_reported = false;
      wipe_error_reported = false;
      wipe_requested = true;
      showStatus("WIPING DISK", LOGO_ALERT_SYMBOL);
      setNeoColor(255, 165, 0);
    } else if (held >= 700 && (now - bak_last_hint) >= 300) {
      // Warn with a countdown so an accidental hold can be released in time.
      // Use draw() (not showStatus) so the cached normal status is preserved.
      bak_last_hint = now;
      bak_countdown = true;
      descriptor_view_active = false;
      descriptor_page = 0;
      uint32_t remain = (WIPE_HOLD_MS - held + 999) / 1000;
      char hint[24];
      snprintf(hint, sizeof(hint), "Hold: WIPE %lus", (unsigned long)remain);
      draw(hint, LOGO_ALERT_SYMBOL);
    }
  }

  static bool ok_prev = true;
  bool ok_now = digitalRead(BTN_OK);
  if (ok_now != ok_prev) {
    ok_prev = ok_now;
    if (ok_now == LOW) {
      if (descriptor_view_active) {
        SerialTinyUSB.println("BTN_OK pressed -> exit descriptor view");
        exit_descriptor_view();
      } else {
        // Force a redraw of the main screen if it was cleared unexpectedly
        SerialTinyUSB.println("BTN_OK pressed -> refresh display");
        reinit_display();
      }
    }
  }

  if (descriptor_view_active) {
    return;
  }

  if (usbkiller == true) {
    showStatus("USB Killer", LOGO_EVIL_SYMBOL);
    usbkiller = false;
    setNeoColor(255, 0, 0);
  }

  if (readme == true) {
    showStatus("README (R)", LOGO_USB_CONNECTED);
    readme = false;
    setNeoColor(0, 0, 255);
  }

  if (autorun == true) {
    showStatus("AUTORUN (R)", LOGO_USB_CONNECTED);
    autorun = false;
    setNeoColor(0, 0, 255);
  }

  if (deleted == true && deleted_reported == false) {
    showStatus("DELETING", LOGO_USB_CONNECTED);
    deleted = false;
    deleted_reported = true;
    setNeoColor(0, 0, 255);
  }

  if (written == true && written_reported == false) {
    showStatus("WRITING", LOGO_USB_CONNECTED);
    written = false;
    written_reported = true;
    setNeoColor(0, 0, 255);
  }

  if (hid_sent == true && hid_reported == false) {
    showStatus("HID Sending data", LOGO_ALERT_SYMBOL);
    hid_sent = false;
    hid_reported = true;
    setNeoColor(255, 0, 0);
  }

  if (suspicious_device == true && suspicious_reported == false) {
    char alert_msg[24];
    snprintf(alert_msg, sizeof(alert_msg), "SUSP: %s", suspicious_name);
    showStatus(alert_msg, LOGO_EVIL_SYMBOL);
    suspicious_device = false;
    suspicious_reported = true;
    setNeoColor(255, 165, 0);
  }

  if (hid_speed_alert == true && hid_speed_reported == false) {
    char speed_msg[24];
    snprintf(speed_msg, sizeof(speed_msg), "AUTO %.0f k/s", hid_current_speed);
    showStatus(speed_msg, LOGO_EVIL_SYMBOL);
    SerialTinyUSB.printf("[!!!] AUTOMATED TYPING: %.1f keys/sec\r\n", hid_current_speed);
    hid_speed_reported = true;
    setNeoColor(255, 0, 255);
    // Update last device info
    if (hid_current_speed > last_device.max_hid_speed) {
      last_device.max_hid_speed = hid_current_speed;
    }
  }

  // Partition-wipe outcome (flags are set by core1 in wipe_write_complete_cb)
  if (wipe_done == true && wipe_done_reported == false) {
    showStatus("WIPE DONE", LOGO_USB_CONNECTED);
    wipe_done_reported = true;
    setNeoColor(0, 255, 0);
  }

  if (wipe_error == true && wipe_error_reported == false) {
    showStatus("WIPE ERROR", LOGO_EVIL_SYMBOL);
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
      // Print the number of HID events detected so far
      char outstr[22];
      snprintf(outstr, sizeof(outstr), "HID Evt# %d", hid_event_num);
      showStatus(outstr, LOGO_USB_CONNECTED);
    } else {
      showStatus("RESETTING", LOGO_USB_CONNECTED);
      swreset();
    }
  }
}


// Main Core1 loop: managing USB Host
void loop1() {
  USBHost.task();
  service_wipe();  // Starts a wipe on the host core when core0 requested one
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

// Updates the cached GUI message/icon and mirrors the same text over serial.
void showStatus(const char* str, uint8_t logoIndex) {
  const char* sanitized = str;
  while (*sanitized == '\n') {
    sanitized++;
  }

  if (LOGO_COUNT > 0) {
    current_logo_index = logoIndex % LOGO_COUNT;
  } else {
    current_logo_index = 0;
  }
  strncpy(current_gui_message, sanitized, sizeof(current_gui_message) - 1);
  current_gui_message[sizeof(current_gui_message) - 1] = '\0';

  if (!descriptor_view_active) {
    draw(current_gui_message, current_logo_index);
  }
  SerialTinyUSB.println(str);
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
  cls();
  dev_info_t* dev = get_first_mounted_device();
  if (dev == NULL) {
    printout("\n[!] No devices");
    return;
  }

  tusb_desc_device_t *desc = &dev->desc_device;
  char buf[48];
  uint8_t page = descriptor_page % 3;

  if (page == 0) {
    printout("\n[Desc 1/3]");
    snprintf(buf, sizeof(buf), "\nVID:%04X PID:%04X", desc->idVendor, desc->idProduct);
    printout(buf);
    snprintf(buf, sizeof(buf), "\nUSB:%04X Dev:%04X", desc->bcdUSB, desc->bcdDevice);
    printout(buf);
    snprintf(buf, sizeof(buf), "\nCls:%u Sub:%u Proto:%u", desc->bDeviceClass, desc->bDeviceSubClass, desc->bDeviceProtocol);
    printout(buf);
  } else if (page == 1) {
    printout("\n[Desc 2/3]");
    snprintf(buf, sizeof(buf), "\nPkt0:%u Config:%u", desc->bMaxPacketSize0, desc->bNumConfigurations);
    printout(buf);
    snprintf(buf, sizeof(buf), "\nManu ID:%u Prod ID:%u", desc->iManufacturer, desc->iProduct);
    printout(buf);
    snprintf(buf, sizeof(buf), "\nSerial ID:%u", desc->iSerialNumber);
    printout(buf);
  } else {
    printout("\n[Desc 3/3]");
    snprintf(buf, sizeof(buf), "\nManu: %s", (char *)dev->manufacturer);
    printout(buf);
    snprintf(buf, sizeof(buf), "\nProd: %s", (char *)dev->product);
    printout(buf);
    snprintf(buf, sizeof(buf), "\nSerial: %s", (char *)dev->serial);
    printout(buf);
  }
}

// Returns to the normal GUI once the descriptor view button is released.
void exit_descriptor_view() {
  descriptor_view_active = false;
  descriptor_page = 0;
  cls();
  draw(current_gui_message, current_logo_index);
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
  draw(current_gui_message, current_logo_index);
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
  SerialTinyUSB.printf("  iManufacturer       %u\r", desc->iManufacturer);
  if (desc->iManufacturer &&
      XFER_RESULT_SUCCESS ==
        tuh_descriptor_get_manufacturer_string_sync(daddr,
                                                    LANGUAGE_ID,
                                                    dev->manufacturer,
                                                    sizeof(dev->manufacturer))) {

    utf16_to_utf8(dev->manufacturer, sizeof(dev->manufacturer));
    SerialTinyUSB.printf(" --- %s\r", (char *) dev->manufacturer);
  }
  SerialTinyUSB.printf("\n");

  // -------- Product String --------
  SerialTinyUSB.printf("  iProduct            %u\r", desc->iProduct);
  if (desc->iProduct &&
      XFER_RESULT_SUCCESS ==
        tuh_descriptor_get_product_string_sync(daddr,
                                               LANGUAGE_ID,
                                               dev->product,
                                               sizeof(dev->product))) {

    utf16_to_utf8(dev->product, sizeof(dev->product));
    SerialTinyUSB.printf(" --- %s\r", (char *) dev->product);
  }
  SerialTinyUSB.printf("\n");
  // -------- Serial Number String --------
  SerialTinyUSB.printf("  iSerialNumber       %u\r", desc->iSerialNumber);
  if (desc->iSerialNumber &&
      XFER_RESULT_SUCCESS ==
        tuh_descriptor_get_serial_string_sync(daddr,
                                              LANGUAGE_ID,
                                              dev->serial,
                                              sizeof(dev->serial))) {

    utf16_to_utf8(dev->serial, sizeof(dev->serial));
    SerialTinyUSB.printf(" --- %s\r", (char *) dev->serial);
  }
  SerialTinyUSB.printf("\n");
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

  showStatus("HID Device", LOGO_ALERT_SYMBOL);
  setNeoColor(255, 0, 0);

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
            SerialTinyUSB.print("\n");
          }
        } else if (is_printable) {
          SerialTinyUSB.printf("%c", ch);
        } else if (ch == '\r') {
          SerialTinyUSB.print("<ENTER>\n");
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
  showStatus("Mass Device", LOGO_USB_CONNECTED);
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
  showStatus("CDC Device", LOGO_USB_CONNECTED);
  setNeoColor(0, 255, 0);
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
    showStatus("Reset OK", LOGO_USB_CONNECTED);
  }
  else if (strcmp(upper, "REBOOT") == 0) {
    SerialTinyUSB.println("[+] Rebooting...");
    showStatus("REBOOT", LOGO_USB_CONNECTED);
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
      showStatus("WIPING DISK", LOGO_ALERT_SYMBOL);
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
void draw(const char* text, uint8_t logoIndex) {
    // Clear the entire framebuffer to avoid leftover pixels from earlier frames
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextWrap(false);

    // Header
    display.setCursor(0, 0);
    display.println("Cerberus");
    display.drawBitmap(51, 0, image_Lock_bits, 7, 8, SSD1306_WHITE);

    // Footer / soft-keys
    display.setCursor(12, 54);
    display.println("OK");
    display.drawBitmap(98, 55, image_arrow_curved_left_up_bits, 8, 5, SSD1306_WHITE);
    display.setCursor(109, 54);
    display.println("Bak");
    display.drawBitmap(2, 54, image_Quest_bits, 7, 8, SSD1306_WHITE);

    // Main icon
    const LogoAsset& asset = LOGO_ASSETS[(LOGO_COUNT > 0) ? (logoIndex % LOGO_COUNT) : 0];
    display.drawBitmap(50, 16, asset.bits, asset.width, asset.height, SSD1306_WHITE);

    // Center the label horizontally on y = 36
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    int16_t x = (int16_t)((OLED_WIDTH - (int16_t)w) / 2);
    if (x < 0) x = 0;
    display.setCursor(x, 36);
    display.println(text);
    display.display();
}
