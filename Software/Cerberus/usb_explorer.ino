/*********************************************************************

  Cerberus - USB content explorer

  Lets you inspect the files on a mass-storage device plugged into the
  HOST port BEFORE the drive is ever trusted on the real PC. It mounts
  the device's FAT16/FAT32/exFAT filesystem *read-only* and lets you:

    - list directories and preview files over serial   (LS / CAT commands)
    - browse them on the OLED                           (hold OK / BROWSE)

  Ported from the RP2350 Swiss-Knife project (usb_msc.cpp), adapted to
  Cerberus's Adafruit_SSD1306 display and 2-button controls.

  Concurrency model (identical to the rest of Cerberus): the TinyUSB host
  stack lives on core1. SdFat's block device drives its I/O by pumping
  tuh_task(), which is NOT safe to call from two cores at once, so ALL
  filesystem work (mount, list, read) runs on core1 inside mscService().
  The UI (core0) only *requests* an operation through the flags below and
  reads back the results.

*********************************************************************/

// These are already included (with guards) by Cerberus.ino, which is
// concatenated before this file; repeated here for clarity. SdFat must come
// before Adafruit_TinyUSB.h so the host MSC block device is exposed.
#include "SdFat_Adafruit_Fork.h"
#include "Adafruit_TinyUSB.h"

// Raw USB disk (512-byte sectors) and the universal FAT/exFAT volume on top.
// FsVolume (not FatVolume) is what gives us exFAT support, common on large
// modern flash drives.
static Adafruit_USBH_MSC_BlockDevice msc_blockdev;
static FsVolume fatfs;

volatile bool g_mscMounted = false;   // FAT/exFAT volume mounted and browsable
volatile bool g_mscPresent = false;   // a disk is attached (even if unmountable)

//
// core0 -> core1 request queue
//
enum { MSC_NONE = 0, MSC_LIST, MSC_READ };
static volatile int  g_req      = MSC_NONE;
static volatile bool g_reqDone  = true;
static volatile bool g_reqOk    = false;
static char          g_reqPath[256];

// Kept modest on purpose: RAM is tight because Cerberus also holds the emulated
// RAM disk (~172 KB). Raise these only if you also shrink DISK_BLOCK_NUM.
#define MSC_MAX_ENTRIES 48
#define MSC_NAME_LEN    48
struct MscEntry {
  char     name[MSC_NAME_LEN];
  bool     dir;
  uint32_t size;
};
static MscEntry      g_entries[MSC_MAX_ENTRIES];
static volatile int  g_nEntries = 0;
static char          g_fileHead[512];   // first bytes of a file, for preview
static volatile int  g_fileHeadLen = 0;

//--------------------------------------------------------------------+
// Mount / unmount (called from the MSC callbacks in Cerberus.ino, core1)
//--------------------------------------------------------------------+
void explorer_mount(uint8_t dev_addr) {
  SerialTinyUSB.printf("[MSC] mount dev=%u\r\n", dev_addr);
  bool b = msc_blockdev.begin(dev_addr);
  msc_blockdev.setActiveLUN(0);
  g_mscPresent = true;

  uint32_t sc = msc_blockdev.sectorCount();
  SerialTinyUSB.printf("[MSC] blockdev.begin=%d  sectors=%lu (%lu MB)\r\n",
                       (int)b, (unsigned long)sc, (unsigned long)(sc / 2048UL));

  // Try the first MBR partition, then fall back to a "superfloppy" volume
  // (filesystem laid directly at LBA 0, no partition table).
  bool m = fatfs.begin(&msc_blockdev, true, 1);
  if (!m) {
    m = fatfs.begin(&msc_blockdev, true, 0);
  }
  if (m) SerialTinyUSB.printf("[MSC] mounted, fatType=%d\r\n", fatfs.fatType());
  else   SerialTinyUSB.printf("[MSC] NOT mounted (unsupported FS? try FAT32/exFAT)\r\n");
  g_mscMounted = m;
}

void explorer_umount(uint8_t dev_addr) {
  (void)dev_addr;
  SerialTinyUSB.printf("[MSC] umount dev=%u\r\n", dev_addr);
  g_mscMounted = false;
  g_mscPresent = false;
  fatfs.end();
  msc_blockdev.end();
}

//--------------------------------------------------------------------+
// Filesystem operations (run on core1 via mscService)
//--------------------------------------------------------------------+
static void doList(const char* path) {
  g_nEntries = 0;
  FsFile dir = fatfs.open((path && path[0]) ? path : "/", O_RDONLY);
  if (!dir) { g_reqOk = false; return; }
  while (g_nEntries < MSC_MAX_ENTRIES) {
    FsFile f = dir.openNextFile(O_RDONLY);
    if (!f) break;
    f.getName(g_entries[g_nEntries].name, sizeof(g_entries[0].name));
    g_entries[g_nEntries].dir  = f.isDir();
    g_entries[g_nEntries].size = (uint32_t)f.fileSize();   // truncated to 32 bits
    g_nEntries++;
    f.close();
  }
  dir.close();
  g_reqOk = true;
}

static void doRead(const char* path) {
  FsFile f = fatfs.open(path, O_RDONLY);
  if (!f) { g_fileHeadLen = 0; g_reqOk = false; return; }
  int n = f.read(g_fileHead, sizeof(g_fileHead));
  if (n < 0) n = 0;
  g_fileHeadLen = n;
  f.close();
  g_reqOk = true;
}

// Invoked from loop1() on core1, after USBHost.task().
void mscService() {
  if (g_reqDone || g_req == MSC_NONE) return;
  __sync_synchronize();
  if (!g_mscMounted)          g_reqOk = false;
  else if (g_req == MSC_LIST) doList(g_reqPath);
  else if (g_req == MSC_READ) doRead(g_reqPath);
  __sync_synchronize();
  g_req = MSC_NONE;
  g_reqDone = true;
}

//--------------------------------------------------------------------+
// Request an operation from core0 and wait for core1 to finish it.
//--------------------------------------------------------------------+
static bool mscDo(int r, const char* path) {
  strncpy(g_reqPath, path, sizeof(g_reqPath) - 1);
  g_reqPath[sizeof(g_reqPath) - 1] = 0;
  g_reqOk   = false;
  g_reqDone = false;
  __sync_synchronize();
  g_req = r;
  uint32_t t = to_ms_since_boot(get_absolute_time());
  while (!g_reqDone) {
    delay(2);   // yields so the TinyUSB device stack keeps running
    if (to_ms_since_boot(get_absolute_time()) - t > 8000) return false;   // timeout
  }
  __sync_synchronize();
  return g_reqOk;
}

//====================================================================
// Serial frontend: LS / CAT
//====================================================================
static void human_size(uint32_t s, char* out, size_t n) {
  if      (s < 1024UL)          snprintf(out, n, "%luB", (unsigned long)s);
  else if (s < 1024UL * 1024)   snprintf(out, n, "%luK", (unsigned long)(s / 1024UL));
  else                          snprintf(out, n, "%luM", (unsigned long)(s / (1024UL * 1024UL)));
}

void explorer_ls(const char* path) {
  if (!g_mscMounted) {
    if (g_mscPresent) SerialTinyUSB.println("[!] Disk attached but filesystem not supported (use FAT32/exFAT)");
    else              SerialTinyUSB.println("[!] No mass-storage device mounted");
    return;
  }
  if (!mscDo(MSC_LIST, path)) {
    SerialTinyUSB.printf("[!] Cannot list '%s'\r\n", path);
    return;
  }
  SerialTinyUSB.printf("\r\n=== LS %s (%d entries) ===\r\n", path, g_nEntries);
  for (int i = 0; i < g_nEntries; i++) {
    if (g_entries[i].dir) {
      SerialTinyUSB.printf("  <DIR>  %s\r\n", g_entries[i].name);
    } else {
      char sz[10];
      human_size(g_entries[i].size, sz, sizeof(sz));
      SerialTinyUSB.printf("  %6s %s\r\n", sz, g_entries[i].name);
    }
  }
  SerialTinyUSB.println("========================\r\n");
}

void explorer_cat(const char* path) {
  if (!g_mscMounted) {
    SerialTinyUSB.println("[!] No mass-storage device mounted");
    return;
  }
  if (!mscDo(MSC_READ, path)) {
    SerialTinyUSB.printf("[!] Cannot read '%s'\r\n", path);
    return;
  }
  int len = g_fileHeadLen;
  SerialTinyUSB.printf("\r\n=== CAT %s (first %d bytes) ===\r\n", path, len);

  // Text view (non-printable bytes shown as '.')
  for (int i = 0; i < len; i++) {
    char c = g_fileHead[i];
    if (c == '\r') continue;
    if (c == '\n' || (c >= 0x20 && c < 0x7F)) SerialTinyUSB.write(c);
    else SerialTinyUSB.write('.');
  }
  SerialTinyUSB.println();

  // Hex view (reuses the existing hexDump helper from Cerberus.ino)
  SerialTinyUSB.println("--- hex ---");
  hexDump((unsigned char*)g_fileHead, len);
  SerialTinyUSB.println("=============================\r\n");
}

//====================================================================
// OLED frontend: file browser (core0)
//====================================================================
//
// 2-button navigation. The whole browser only asks nav_poll() for one of four
// actions, so this is the single place to remap controls.
//
//   NOW (2 buttons):  short OK  -> NEXT (move selection / scroll)
//                     hold  OK  -> SELECT (enter dir / open file / toggle view)
//                     Bak       -> BACK  (up a level / exit)
//
//   TODO(4 buttons):  wire dedicated UP / DOWN / SELECT / BACK and return the
//                     matching NavAction here; the rest of the browser is unchanged.
//
// NavAction values are returned as int (not the enum type) so the Arduino
// auto-generated prototype for nav_poll() doesn't reference a type declared
// further down in the combined sketch.
enum { NAV_IDLE = 0, NAV_NEXT, NAV_SELECT, NAV_BACK };
#define NAV_SELECT_HOLD_MS 500

static int nav_poll() {
  static bool     ok_prev  = true;   // true == HIGH == released (pull-ups)
  static bool     bak_prev = true;
  static uint32_t ok_start = 0;
  static bool     ok_fired = false;
  int act = NAV_IDLE;

  bool ok_now  = digitalRead(BTN_OK);
  bool bak_now = digitalRead(BTN_RST);

  if (ok_now != ok_prev) {
    ok_prev = ok_now;
    if (ok_now == LOW) {                 // pressed
      ok_start = to_ms_since_boot(get_absolute_time());
      ok_fired = false;
    } else {                             // released
      if (!ok_fired) act = NAV_NEXT;     // short press -> NEXT
      ok_start = 0;
    }
  } else if (ok_now == LOW && ok_start != 0 && !ok_fired) {
    if (to_ms_since_boot(get_absolute_time()) - ok_start >= NAV_SELECT_HOLD_MS) {
      ok_fired = true;                   // hold -> SELECT (once)
      act = NAV_SELECT;
    }
  }

  if (bak_now != bak_prev) {
    bak_prev = bak_now;
    if (bak_now == LOW && act == NAV_IDLE) act = NAV_BACK;
  }
  return act;
}

// Block until both buttons are released (so an entry-hold isn't misread).
static void nav_wait_release() {
  while (digitalRead(BTN_OK) == LOW || digitalRead(BTN_RST) == LOW) delay(5);
  delay(20);
  nav_poll();   // absorb the resulting edges
}

// Draws an inverted title bar with a (truncated) label.
static void draw_titlebar(const char* label) {
  display.fillRect(0, 0, OLED_WIDTH, 10, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(1, 1);
  display.print(label);
  display.setTextColor(SSD1306_WHITE);
}

// File preview: first 512 bytes, toggle text <-> hex with a hold, scroll with taps.
static void explorer_view_file(const char* fpath, const char* name) {
  setNeoColor(255, 165, 0);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 28);
  display.print("Reading...");
  display.display();

  bool ok = mscDo(MSC_READ, fpath);
  nav_wait_release();
  if (!ok) {
    display.clearDisplay();
    display.setCursor(0, 28);
    display.print("Read failed");
    display.display();
    delay(1000);
    return;
  }

  bool hexMode = false;
  int  scroll  = 0;
  const int VIS = 5;

  for (;;) {
    int act = nav_poll();
    if (act == NAV_BACK)   return;
    if (act == NAV_SELECT) { hexMode = !hexMode; scroll = 0; }
    if (act == NAV_NEXT)   scroll++;
    if (scroll < 0) scroll = 0;

    int len = g_fileHeadLen;
    setNeoColor(0, 25, 0);

    display.clearDisplay();
    char title[24];
    snprintf(title, sizeof(title), "%.20s", name);
    draw_titlebar(title);

    if (hexMode) {
      const int PERLINE = 6;   // 6 bytes/line fits "OFF: xx xx.." in 21 cols
      int totalLines = (len + PERLINE - 1) / PERLINE;
      if (totalLines > 0 && scroll > totalLines - 1) scroll = totalLines - 1;
      for (int r = 0; r < VIS; r++) {
        int off = (scroll + r) * PERLINE;
        if (off >= len) break;
        char line[24];
        int p = snprintf(line, sizeof(line), "%03X:", off);
        for (int i = 0; i < PERLINE && off + i < len; i++)
          p += snprintf(line + p, sizeof(line) - p, "%02X ", (uint8_t)g_fileHead[off + i]);
        display.setCursor(0, 12 + r * 8);
        display.print(line);
      }
    } else {
      // wrap to 21 columns, drop non-printables to '.'
      const int W = 21;
      static char lines[64][W + 1];
      int nl = 0, col = 0;
      lines[0][0] = 0;
      for (int i = 0; i < len; i++) {
        char c = g_fileHead[i];
        if (c == '\n') { lines[nl][col] = 0; if (nl < 63) nl++; col = 0; lines[nl][0] = 0; continue; }
        if (c == '\r') continue;
        if (c < 32 || c > 126) c = '.';
        if (col >= W) { lines[nl][col] = 0; if (nl < 63) nl++; col = 0; }
        lines[nl][col++] = c;
        lines[nl][col] = 0;
      }
      int nLines = nl + 1;
      if (scroll > nLines - 1) scroll = nLines - 1;
      for (int r = 0; r < VIS && scroll + r < nLines; r++) {
        display.setCursor(0, 12 + r * 8);
        display.print(lines[scroll + r]);
      }
    }

    char foot[24];
    snprintf(foot, sizeof(foot), "%dB %s OK:scr hold", len, hexMode ? "HEX" : "TXT");
    display.setCursor(0, 56);
    display.print(foot);
    display.display();
    delay(30);
  }
}

// Main browser loop. Blocks until the user backs out of the root directory.
void explorer_oled_browse() {
  nav_wait_release();

  char path[256] = "/";
  int  sel = 0, top = 0;
  bool need = true;
  const int VIS = 5;

  for (;;) {
    int act = nav_poll();

    // Back: at root leave the browser, otherwise go up one level.
    if (act == NAV_BACK) {
      if (strcmp(path, "/") == 0) { setNeoColor(0, 0, 255); return; }
      char* s = strrchr(path, '/');
      if (s) { if (s == path) path[1] = 0; else *s = 0; }
      sel = 0; top = 0; need = true;
      continue;
    }

    // No browsable volume: show a hint and keep waiting (Bak still exits).
    if (!g_mscMounted) {
      setNeoColor(0, 0, 40);
      display.clearDisplay();
      draw_titlebar("USB STORAGE");
      display.setCursor(0, 20);
      if (g_mscPresent) {
        display.print("FS not supported");
        display.setCursor(0, 32);
        display.print("Use FAT32/exFAT");
      } else {
        display.print("Insert a USB drive");
        display.setCursor(0, 32);
        display.print("in the HOST port");
      }
      display.setCursor(0, 56);
      display.print("Bak: exit");
      display.display();
      need = true;
      delay(60);
      continue;
    }

    // (Re)list the current directory.
    if (need) {
      setNeoColor(255, 165, 0);
      display.clearDisplay();
      draw_titlebar("USB STORAGE");
      display.setCursor(0, 28);
      display.print("Reading...");
      display.display();
      bool ok = mscDo(MSC_LIST, path);
      need = false;
      nav_wait_release();
      if (!ok) {
        display.clearDisplay();
        display.setCursor(0, 28);
        display.print("Read error");
        display.display();
        delay(1000);
      }
    }

    int n = g_nEntries;

    if (act == NAV_NEXT && n > 0) {
      sel++;
      if (sel >= n) sel = 0;
    }
    if (sel < top)        top = sel;
    if (sel >= top + VIS) top = sel - VIS + 1;

    if (act == NAV_SELECT && n > 0) {
      char np[256];
      if (strcmp(path, "/") == 0) snprintf(np, sizeof(np), "/%s", g_entries[sel].name);
      else                        snprintf(np, sizeof(np), "%s/%s", path, g_entries[sel].name);
      if (g_entries[sel].dir) {
        strncpy(path, np, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
        sel = 0; top = 0; need = true;
      } else {
        explorer_view_file(np, g_entries[sel].name);
        nav_wait_release();
      }
      continue;
    }

    setNeoColor(0, 40, 0);

    // Draw the directory listing.
    display.clearDisplay();
    char title[24];
    snprintf(title, sizeof(title), "DIR %.16s", path);
    draw_titlebar(title);

    if (n == 0) {
      display.setCursor(2, 28);
      display.print("(empty)");
    }
    for (int i = 0; i < VIS && (top + i) < n; i++) {
      int idx = top + i;
      int y   = 12 + i * 10;
      if (idx == sel) {
        display.fillRect(0, y - 1, OLED_WIDTH, 10, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
      }
      char row[24];
      if (g_entries[idx].dir) {
        snprintf(row, sizeof(row), "[%.18s]", g_entries[idx].name);
      } else {
        char sz[10];
        human_size(g_entries[idx].size, sz, sizeof(sz));
        snprintf(row, sizeof(row), "%-14.14s %5s", g_entries[idx].name, sz);
      }
      display.setCursor(2, y);
      display.print(row);
      display.setTextColor(SSD1306_WHITE);
    }

    // Scrollbar.
    if (n > VIS) {
      int barH = 50 * VIS / n; if (barH < 4) barH = 4;
      int barY = 12 + (50 - barH) * top / (n - VIS);
      display.drawFastVLine(126, 12, 50, SSD1306_WHITE);
      display.fillRect(125, barY, 3, barH, SSD1306_WHITE);
    }

    display.display();
    delay(20);
  }
}
