/*********************************************************************

  Cerberus - USB content explorer

  Lets you inspect the files on a mass-storage device plugged into the
  HOST port BEFORE the drive is ever trusted on the real PC. It mounts
  the device *read-only* and lets you:

    - list directories and preview files over serial   (LS / CAT commands)
    - browse them on the OLED                           (hold OK / BROWSE)

  Multi-format, multi-volume: at mount time the device's partition table
  (MBR + GPT, or a whole-disk "superfloppy") is walked, and each slice is
  probed for NTFS, ext2/3/4 and ISO9660/Joliet (the read-only drivers in
  fs_ntfs.h / fs_ext.h / fs_iso.h, behind the MultiFs.h FsReader interface),
  falling back to SdFat for FAT12/16/32 + exFAT. Every supported volume is
  recorded in a small registry; the UI exposes them under a "/N" path (a single
  volume is entered transparently). Because the drivers share static scratch and
  are not reentrant, only ONE volume is mounted at a time -- switching volumes
  re-mounts. Every driver treats on-disk data as untrusted (this is a USB
  condom) and is strictly read-only: no write path reaches the inspected disk.

  Ported from the RP2350 Swiss-Knife project (usb_msc.cpp), adapted to
  Cerberus's Adafruit_SSD1306 display and 2-button controls.

  Concurrency model (identical to the rest of Cerberus): the TinyUSB host
  stack lives on core1. The block device drives its I/O by pumping
  tuh_task(), which is NOT safe to call from two cores at once, so ALL
  filesystem work (mount, list, read) runs on core1 inside mscService().
  The custom drivers are likewise single-instance / not reentrant, which is
  fine because only core1 ever touches them. The UI (core0) only *requests*
  an operation through the flags below and reads back the results.

*********************************************************************/

// These are already included (with guards) by Cerberus.ino, which is
// concatenated before this file; repeated here for clarity. SdFat must come
// before Adafruit_TinyUSB.h so the host MSC block device is exposed.
#include "SdFat_Adafruit_Fork.h"
#include "Adafruit_TinyUSB.h"

// Multi-format read-only drivers (see MultiFs.h). Included ONLY here so their
// header-defined statics (IsoReader::_buf, NtfsReader::_buf/_rec/_key,
// ExtReader::_blk/...) get a single definition. FAT/exFAT is still handled by
// SdFat below; NTFS, ext2/3/4 and ISO9660/Joliet are handled by these drivers,
// all behind the same UI.
#include "MultiFs.h"
#include "fs_ntfs.h"
#include "fs_ext.h"
#include "fs_iso.h"

// Raw USB disk (512-byte sectors). Two kinds of backend sit on top of it:
//   - SdFat's FsVolume   -> FAT12/16/32 + exFAT   (battle-tested, kept as-is)
//   - an FsReader driver  -> NTFS, ext2/3/4, ISO9660/Joliet
// A single UI (serial + OLED) talks to whichever volume is active.
static Adafruit_USBH_MSC_BlockDevice msc_blockdev;
static FsVolume fatfs;

// Adapts the TinyUSB host MSC block device to the driver-facing BlockDev
// interface. readSectors() already blocks (SdFat drives FAT reads through the
// very same call), so no extra host-task pumping is needed here.
class UsbBlockDev : public BlockDev {
public:
  uint32_t sectorCount() { return msc_blockdev.sectorCount(); }
  bool readSectors(uint32_t lba, uint8_t* dst, size_t n) {
    return msc_blockdev.readSectors(lba, dst, n);
  }
};
static UsbBlockDev g_bdev;

// A read-only SdFat block device that re-bases reads onto a partition slice, so
// SdFat can mount a FAT/exFAT volume at ANY offset (MBR entry, GPT entry, or a
// superfloppy at LBA 0). Writes are hard-refused: nothing this browser does may
// ever reach the inspected disk.
class OffsetBlockDevice : public FsBlockDeviceInterface {
public:
  void setSlice(uint32_t base, uint32_t num) { _base = base; _num = num; }
  bool isBusy() { return false; }
  bool readSector(uint32_t s, uint8_t* dst) { return readSectors(s, dst, 1); }
  bool readSectors(uint32_t s, uint8_t* dst, size_t ns) {
    if ((uint64_t)s + ns > _num) return false;
    return msc_blockdev.readSectors(_base + s, dst, ns);
  }
  uint32_t sectorCount() { return _num; }
  bool syncDevice() { return true; }
  bool writeSector(uint32_t, const uint8_t*) { return false; }
  bool writeSectors(uint32_t, const uint8_t*, size_t) { return false; }
private:
  uint32_t _base = 0, _num = 0;
};
static OffsetBlockDevice g_offsetDev;

// The custom-driver instances. Their scratch buffers are static (each driver is
// single-instance / NOT reentrant), so only ONE volume is ever actively mounted
// at a time -- switching volumes re-mounts (see activateVolume()). This is what
// keeps RAM bounded no matter how many volumes a disk holds.
static NtfsReader g_ntfs;
static ExtReader  g_ext;
static IsoReader  g_iso;

// Kind of backend serving the ACTIVE volume, and (for the FsReader kinds) the
// driver pointer.
enum { BK_NONE = 0, BK_SDFAT, BK_FSREADER };
static int       g_backend = BK_NONE;
static FsReader* g_fs      = NULL;      // valid only when g_backend == BK_FSREADER
static char      g_fsName[12]  = "";    // active volume's fs name, for the UI
static char      g_fsLabel[32] = "";    // active volume's label, "" if none

// Volume registry: filled by scanVolumes() at mount time. Holds metadata only
// (which filesystem lives on which slice); at most one volume is mounted at a
// time. Discovered in disk order: whole-disk first, else MBR/GPT partitions.
enum { FS_NONE = 0, FS_FAT, FS_NTFS, FS_EXT, FS_ISO };
#define MAX_VOLUMES 4
struct Volume {
  uint8_t  type;        // FS_*
  uint32_t base;        // partition start LBA
  uint32_t num;         // partition length (sectors)
  char     name[8];     // "FAT32" / "NTFS" / "ext4" / "ISO9660" ...
  char     label[32];   // best-effort volume label ("" if none)
};
static Volume g_vols[MAX_VOLUMES];
static int    g_nVols     = 0;
static int    g_activeVol = -1;         // index of the currently mounted volume

volatile bool g_mscMounted = false;   // at least one browsable volume is present
volatile bool g_mscPresent = false;   // a disk is attached (even if unmountable)

//
// core0 -> core1 request queue
//
enum { MSC_NONE = 0, MSC_LIST, MSC_READ, MSC_SCAN, MSC_INSPECT };
static volatile int  g_req      = MSC_NONE;
static volatile bool g_reqDone  = true;
static volatile bool g_reqOk    = false;
static char          g_reqPath[256];

// Kept modest on purpose: RAM is tight because Cerberus also holds the emulated
// RAM disk (~172 KB). Raise these only if you also shrink DISK_BLOCK_NUM.
// DirEntry (from MultiFs.h) is the common listing type shared by both backends,
// so there is no second per-driver buffer to copy through.
#define MSC_MAX_ENTRIES 48
static DirEntry      g_entries[MSC_MAX_ENTRIES];
static volatile int  g_nEntries = 0;
static char          g_fileHead[512];   // first bytes of a file, for preview
static volatile int  g_fileHeadLen = 0;

// ---------------------------------------------------------------------------
// File analyzer (the "Files" menu entry): a flat, recursive listing of every
// file on the device, each tagged with the type guessed from its magic bytes.
// Buffers are kept deliberately small -- RAM is dominated by the ~172 KB
// emulated RAM disk -- so the scan is capped and marks itself truncated when it
// runs out of room rather than growing.  Stored paths are full VFS paths so a
// file can be re-opened (to show its magic bytes) without a second tree walk.
// ---------------------------------------------------------------------------
#define SCAN_MAX       64    // most files the flat list holds
#define SCAN_PATH_LEN  112   // longest re-openable path we keep per file
#define DIRQ_MAX       16    // pending-directory queue depth (iterative walk)

struct ScanItem {
  char     path[SCAN_PATH_LEN];
  uint32_t size;             // display only (clamped to 32-bit)
  uint8_t  type;             // FileType
};
static ScanItem      g_scan[SCAN_MAX];
static volatile int  g_scanCount = 0;
static volatile bool g_scanTrunc = false;   // list / queue / path limit hit

// Directory-walk queue (BFS). Holds full VFS paths of directories still to visit.
static char g_dirq[DIRQ_MAX][SCAN_PATH_LEN];
static int  g_dirqHead = 0, g_dirqTail = 0, g_dirqCount = 0;

// ---------------------------------------------------------------------------
// Deep inspection (MSC_INSPECT): look *inside* a file without decompressing --
// RAM is far too tight for a DEFLATE window, and a signature pass is immune to
// zip bombs (we never inflate). Two checks:
//   1. ZIP central directory walk -> names of embedded entries, flag dangerous
//      ones (the "exe hidden in a zip" case). Robust to streamed/data-descriptor
//      zips because the central directory always carries correct sizes.
//   2. Embedded-signature sweep -> a distinctive magic found at offset > 0
//      (the "payload appended after a valid header" / polyglot case).
// All walks are bounded, require forward progress, and never allocate from an
// attacker-controlled size field.
// ---------------------------------------------------------------------------
#define ZIP_MAX_ENTRIES   128          // central-dir entries we enumerate
#define ZIP_MAX_ITERS     512          // hard loop cap (bomb guard)
#define EOCD_SCAN_MAX     66000u       // bytes of tail scanned for the EOCD record
#define SWEEP_MAX_BYTES   (1u << 20)   // 1 MB cap for the embedded-signature sweep
#define SWEEP_BULK_MAX    (128u * 1024) // in bulk SCAN, only sweep files up to this

struct InspectResult {
  bool     zipParsed;        // central directory was located & walked
  int      zipEntries;       // entries enumerated
  bool     zipTrunc;         // hit entry/iteration cap or a structure violation
  bool     zipExec;          // an entry name looks executable/script
  char     zipExecName[40];  // first such entry (for display)
  bool     sweepDone;        // deep sweep actually ran
  bool     sweepTrunc;       // sweep stopped at SWEEP_MAX_BYTES (file not fully covered)
  bool     embedded;         // a distinctive magic was found at offset > 0
  uint32_t embeddedOff;      // where (absolute byte offset)
  char     embeddedWhat[12]; // short label of what was found
};
static InspectResult  g_insp;
static volatile bool  g_inspectDeep = false;   // run the (slower) sweep too

//--------------------------------------------------------------------+
// Mount / unmount (called from the MSC callbacks in Cerberus.ino, core1)
//--------------------------------------------------------------------+
// Probes one [base, num) slice for a supported filesystem and, on success,
// appends it to the registry (metadata + label) and immediately unmounts the
// driver so the shared buffers are free for the next probe. Probe order is
// NTFS -> ext -> ISO -> FAT; the signatures are disjoint so order only sets the
// tie-break that never actually occurs. Returns true if a volume was recorded.
static bool probeAndRegister(uint32_t base, uint32_t num) {
  if (g_nVols >= MAX_VOLUMES || num == 0) return false;

  Volume v;
  v.base = base;
  v.num = num;
  v.name[0] = '\0';
  v.label[0] = '\0';
  v.type = FS_NONE;

  FsReader* r = NULL;
  if      (g_ntfs.begin(g_bdev, base, num)) { v.type = FS_NTFS; r = &g_ntfs; }
  else if (g_ext.begin(g_bdev, base, num))  { v.type = FS_EXT;  r = &g_ext;  }
  else if (g_iso.begin(g_bdev, base, num))  { v.type = FS_ISO;  r = &g_iso;  }
  else {
    // FAT/exFAT: mount the slice through the offset shim, read the type, unmount.
    g_offsetDev.setSlice(base, num);
    if (fatfs.begin(&g_offsetDev, true, 0)) {
      v.type = FS_FAT;
      int ft = fatfs.fatType();
      const char* nm = (ft == 64) ? "exFAT" : (ft == 32) ? "FAT32"
                     : (ft == 16) ? "FAT16" : (ft == 12) ? "FAT12" : "FAT";
      strncpy(v.name, nm, sizeof(v.name) - 1);
      v.name[sizeof(v.name) - 1] = '\0';
      fatfs.end();
    }
  }
  if (r) {
    strncpy(v.name, r->fsName(), sizeof(v.name) - 1);
    v.name[sizeof(v.name) - 1] = '\0';
    if (!r->getLabel(v.label, sizeof(v.label))) v.label[0] = '\0';
    r->end();
  }
  if (v.type == FS_NONE) return false;

  g_vols[g_nVols++] = v;
  SerialTinyUSB.printf("[MSC] vol %d: %-7s%s%s base=%lu size=%luMB\r\n",
                       g_nVols, v.name, v.label[0] ? " " : "", v.label,
                       (unsigned long)base, (unsigned long)(num / 2048UL));
  return true;
}

// Enumerates GPT partitions (protective-MBR case) and registers each. `sec1`
// holds LBA 1 (the GPT header). Bounded to a fixed number of scanned entry
// sectors so a hostile header can never spin us.
static void scanGpt(const uint8_t* sec1, uint32_t sc) {
  if (memcmp(sec1, "EFI PART", 8) != 0) return;
  uint32_t nParts  = mfs_le32(sec1 + 80);
  uint32_t entSize = mfs_le32(sec1 + 84);
  uint64_t entLba  = mfs_le64(sec1 + 72);
  if (entSize < 128 || entSize > 512 || (512 % entSize) != 0) return;
  if (nParts == 0 || nParts > 512) nParts = 512;            // clamp
  uint32_t perSec = 512 / entSize;

  uint8_t eb[512];
  uint32_t seen = 0;
  for (uint32_t s = 0; seen < nParts && s < 64 && g_nVols < MAX_VOLUMES; s++) {
    if (entLba + s >= sc) break;
    if (!msc_blockdev.readSectors((uint32_t)entLba + s, eb, 1)) break;
    for (uint32_t k = 0; k < perSec && seen < nParts && g_nVols < MAX_VOLUMES;
         k++, seen++) {
      const uint8_t* e = eb + k * entSize;
      bool used = false;                                    // non-zero type GUID
      for (int b = 0; b < 16; b++) if (e[b]) { used = true; break; }
      if (!used) continue;
      uint64_t first = mfs_le64(e + 32);
      uint64_t last  = mfs_le64(e + 40);
      if (last < first || first >= sc) continue;
      if (last >= sc) last = sc - 1;
      probeAndRegister((uint32_t)first, (uint32_t)(last - first + 1));
    }
  }
}

// Fills the volume registry for the attached device. A disk is either a single
// whole-disk ("superfloppy" / ISO) volume OR partitioned -- so we try the whole
// disk first and, only if nothing mounts there, parse the partition table. This
// also sidesteps the FAT-superfloppy vs MBR ambiguity (both end in 0x55AA).
static bool scanVolumes() {
  g_nVols = 0;
  g_activeVol = -1;
  g_backend = BK_NONE;
  g_fs = NULL;

  uint32_t sc = msc_blockdev.sectorCount();
  if (sc == 0) return false;

  if (probeAndRegister(0, sc)) return true;                 // whole-disk volume

  uint8_t sec0[512];
  if (msc_blockdev.readSectors(0, sec0, 1) &&
      sec0[510] == 0x55 && sec0[511] == 0xAA) {
    bool gpt = false;
    for (int i = 0; i < 4; i++)
      if (sec0[446 + 16 * i + 4] == 0xEE) { gpt = true; break; }
    if (gpt) {
      uint8_t sec1[512];
      if (msc_blockdev.readSectors(1, sec1, 1)) scanGpt(sec1, sc);
    } else {
      for (int i = 0; i < 4 && g_nVols < MAX_VOLUMES; i++) {   // 4 MBR primaries
        const uint8_t* e = sec0 + 446 + 16 * i;
        uint8_t type = e[4];
        if (type == 0 || type == 0x05 || type == 0x0F) continue;  // empty/extended
        uint32_t base = mfs_le32(e + 8);
        uint32_t num  = mfs_le32(e + 12);
        if (base == 0 || num == 0 || base >= sc) continue;
        if ((uint64_t)base + num > sc) num = sc - base;
        probeAndRegister(base, num);
      }
    }
  }
  return g_nVols > 0;
}

// Ensures volume `idx` is the one currently mounted, (re)mounting if needed.
// Because the drivers share static buffers, switching volumes tears the old
// mount down first. Also refreshes g_fsName/g_fsLabel for the UI.
static bool activateVolume(int idx) {
  if (idx < 0 || idx >= g_nVols) return false;
  if (g_activeVol == idx && g_backend != BK_NONE) return true;

  if (g_backend == BK_FSREADER && g_fs) g_fs->end();
  else if (g_backend == BK_SDFAT)       fatfs.end();
  g_backend = BK_NONE;
  g_fs = NULL;
  g_activeVol = -1;

  Volume& v = g_vols[idx];
  bool ok = false;
  switch (v.type) {
    case FS_NTFS: ok = g_ntfs.begin(g_bdev, v.base, v.num); if (ok) g_fs = &g_ntfs; break;
    case FS_EXT:  ok = g_ext.begin(g_bdev, v.base, v.num);  if (ok) g_fs = &g_ext;  break;
    case FS_ISO:  ok = g_iso.begin(g_bdev, v.base, v.num);  if (ok) g_fs = &g_iso;  break;
    case FS_FAT:  g_offsetDev.setSlice(v.base, v.num);
                  ok = fatfs.begin(&g_offsetDev, true, 0); break;
    default: return false;
  }
  if (!ok) { g_fs = NULL; return false; }

  g_backend = (v.type == FS_FAT) ? BK_SDFAT : BK_FSREADER;
  g_activeVol = idx;
  strncpy(g_fsName, v.name, sizeof(g_fsName) - 1);
  g_fsName[sizeof(g_fsName) - 1] = '\0';
  strncpy(g_fsLabel, v.label, sizeof(g_fsLabel) - 1);
  g_fsLabel[sizeof(g_fsLabel) - 1] = '\0';
  return true;
}

void explorer_mount(uint8_t dev_addr) {
  SerialTinyUSB.printf("[MSC] mount dev=%u\r\n", dev_addr);
  bool b = msc_blockdev.begin(dev_addr);
  msc_blockdev.setActiveLUN(0);
  g_mscPresent = true;
  g_fsName[0] = '\0';
  g_fsLabel[0] = '\0';

  uint32_t sc = msc_blockdev.sectorCount();
  SerialTinyUSB.printf("[MSC] blockdev.begin=%d  sectors=%lu (%lu MB)\r\n",
                       (int)b, (unsigned long)sc, (unsigned long)(sc / 2048UL));

  bool m = scanVolumes();
  if (m) SerialTinyUSB.printf("[MSC] %d volume(s) found\r\n", g_nVols);
  else   SerialTinyUSB.printf("[MSC] NOT mounted (unsupported FS; "
                              "FAT/exFAT/NTFS/ext/ISO9660 supported)\r\n");
  g_mscMounted = m;
}

void explorer_umount(uint8_t dev_addr) {
  (void)dev_addr;
  SerialTinyUSB.printf("[MSC] umount dev=%u\r\n", dev_addr);
  g_mscMounted = false;
  g_mscPresent = false;
  if (g_backend == BK_FSREADER && g_fs) g_fs->end();
  else if (g_backend == BK_SDFAT)       fatfs.end();
  g_backend = BK_NONE;
  g_fs = NULL;
  g_nVols = 0;
  g_activeVol = -1;
  g_fsName[0] = '\0';
  g_fsLabel[0] = '\0';
  msc_blockdev.end();
}

//--------------------------------------------------------------------+
// Filesystem operations (run on core1 via mscService)
//--------------------------------------------------------------------+
// Splits a VFS path into (volume index, in-volume subpath). The path grammar
// depends on how many volumes the disk has, so the single-stick experience is
// unchanged:
//   - 1 volume:   the whole path is in-volume ("/", "/dir/file") -> vol 0.
//   - >1 volumes: "/"      -> the volume list          (returns 0)
//                 "/N"     -> root of volume N (1-based) (returns 1, sub="/")
//                 "/N/sub" -> inside volume N            (returns 1)
// Returns 1 for an in-volume path (fills volIdx + sub), 0 for the volume list,
// or -1 for an invalid path / no volumes.
static int vfsParse(const char* path, int& volIdx, char* sub, size_t subsz) {
  if (g_nVols <= 0) return -1;
  if (g_nVols == 1) {
    volIdx = 0;
    const char* p = (path && path[0]) ? path : "/";
    strncpy(sub, p, subsz - 1);
    sub[subsz - 1] = '\0';
    return 1;
  }
  const char* p = path ? path : "/";
  while (*p == '/') p++;
  if (*p == '\0') return 0;                       // "/" -> volume list
  int n = 0;
  bool any = false;
  while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; any = true;
                                   if (n > 9999) return -1; }
  if (!any) return -1;
  int idx = n - 1;
  if (idx < 0 || idx >= g_nVols) return -1;
  volIdx = idx;
  if (*p == '\0')      { strncpy(sub, "/", subsz - 1); sub[subsz - 1] = '\0'; }
  else if (*p == '/')  { strncpy(sub, p, subsz - 1);   sub[subsz - 1] = '\0'; }
  else                 return -1;                 // e.g. "/12abc"
  return 1;
}

// Fills g_entries with one synthetic row per volume (used at the multi-volume
// root). Rows are directories named "N:TYPE label" so the existing browser can
// render them; SELECT is special-cased to enter volume N (see the OLED code).
static void listVolumes() {
  int n = g_nVols < MSC_MAX_ENTRIES ? g_nVols : MSC_MAX_ENTRIES;
  for (int i = 0; i < n; i++) {
    snprintf(g_entries[i].name, MFS_NAME_LEN, "%d:%s%s%s", i + 1, g_vols[i].name,
             g_vols[i].label[0] ? " " : "", g_vols[i].label);
    g_entries[i].dir  = true;
    g_entries[i].size = (uint64_t)g_vols[i].num * 512ull;
  }
  g_nEntries = n;
  g_reqOk = true;
}

static void doList(const char* path) {
  g_nEntries = 0;
  int volIdx = -1;
  char sub[256];
  int r = vfsParse(path, volIdx, sub, sizeof(sub));
  if (r < 0) { g_reqOk = false; return; }
  if (r == 0) { listVolumes(); return; }           // multi-volume root
  if (!activateVolume(volIdx)) { g_reqOk = false; return; }

  if (g_backend == BK_FSREADER && g_fs) {
    // Drivers fill DirEntry directly into g_entries (no extra copy). listDir
    // may report a total larger than MSC_MAX_ENTRIES; we only keep the first
    // MSC_MAX_ENTRIES it wrote.
    int n = g_fs->listDir(sub, g_entries, MSC_MAX_ENTRIES);
    if (n < 0) { g_reqOk = false; return; }
    if (n > MSC_MAX_ENTRIES) n = MSC_MAX_ENTRIES;
    g_nEntries = n;
    g_reqOk = true;
    return;
  }

  // SdFat (FAT/exFAT).
  FsFile dir = fatfs.open(sub, O_RDONLY);
  if (!dir) { g_reqOk = false; return; }
  while (g_nEntries < MSC_MAX_ENTRIES) {
    FsFile f = dir.openNextFile(O_RDONLY);
    if (!f) break;
    f.getName(g_entries[g_nEntries].name, sizeof(g_entries[0].name));
    g_entries[g_nEntries].dir  = f.isDir();
    g_entries[g_nEntries].size = (uint64_t)f.fileSize();
    g_nEntries++;
    f.close();
  }
  dir.close();
  g_reqOk = true;
}

// Case-insensitive equality for short ASCII tokens (extensions).
static bool iequals(const char* a, const char* b) {
  for (; *a && *b; a++, b++)
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
  return *a == *b;
}

// Reads up to `want` bytes at byte offset `off` from the file at VFS `path` into
// `out`. Returns bytes read (>=0), or -1 on error / not-a-file. Shared by doRead
// (preview), the per-file magic probe, and the deep inspector's chunked reads.
static int readFileAt(const char* path, uint64_t off, uint8_t* out, uint32_t want) {
  int volIdx = -1;
  char sub[256];
  int r = vfsParse(path, volIdx, sub, sizeof(sub));
  if (r != 1) return -1;                              // not an in-volume file path
  if (!activateVolume(volIdx)) return -1;

  if (g_backend == BK_FSREADER && g_fs) {
    DirEntry info;
    uint32_t avail = want;
    if (g_fs->stat(sub, info)) {
      if (info.dir) return -1;
      if (off >= info.size) return 0;
      if (info.size - off < avail) avail = (uint32_t)(info.size - off);
    }
    if (avail == 0) return 0;
    if (!g_fs->readFile(sub, off, out, avail)) return -1;
    return (int)avail;
  }

  // SdFat (FAT/exFAT).
  FsFile f = fatfs.open(sub, O_RDONLY);
  if (!f) return -1;
  if (f.isDir()) { f.close(); return -1; }
  if (off && !f.seekSet(off)) { f.close(); return 0; }
  int n = f.read(out, want);
  f.close();
  return n < 0 ? 0 : n;
}

static inline int readFileHead(const char* path, uint8_t* out, uint32_t want) {
  return readFileAt(path, 0, out, want);
}

// File size in bytes, or -1 on error / not-a-file.
static int64_t fileSizeOf(const char* path) {
  int volIdx = -1;
  char sub[256];
  if (vfsParse(path, volIdx, sub, sizeof(sub)) != 1) return -1;
  if (!activateVolume(volIdx)) return -1;
  if (g_backend == BK_FSREADER && g_fs) {
    DirEntry info;
    if (!g_fs->stat(sub, info) || info.dir) return -1;
    return (int64_t)info.size;
  }
  FsFile f = fatfs.open(sub, O_RDONLY);
  if (!f) return -1;
  if (f.isDir()) { f.close(); return -1; }
  uint64_t s = f.fileSize();
  f.close();
  return (int64_t)s;
}

static void doRead(const char* path) {
  memset(g_fileHead, 0, sizeof(g_fileHead));          // no stale bytes past EOF
  int n = readFileHead(path, (uint8_t*)g_fileHead, sizeof(g_fileHead));
  if (n < 0) { g_fileHeadLen = 0; g_reqOk = false; return; }
  g_fileHeadLen = n;
  g_reqOk = true;
}

//--------------------------------------------------------------------+
// Magic-byte file typing + recursive flat scan (the "Files" analyzer)
//--------------------------------------------------------------------+
// A compact signature table. We only identify the file type and keep the raw
// magic bytes for display; no per-format metadata parsing (kept intentionally
// small and fast for the MCU). Order matters only where prefixes overlap.
enum FileType {
  FT_EMPTY = 0, FT_PNG, FT_JPEG, FT_GIF, FT_BMP, FT_WEBP, FT_ICO,
  FT_PDF, FT_ZIP, FT_GZIP, FT_RAR, FT_7Z, FT_XZ, FT_BZ2,
  FT_ELF, FT_PE, FT_MACHO, FT_CLASS, FT_WASM,
  FT_MP3, FT_MP4, FT_WAV, FT_AVI, FT_OGG, FT_FLAC,
  FT_SQLITE, FT_SCRIPT, FT_TEXT, FT_UNKNOWN, FT_COUNT
};

struct TypeLabel { const char* shortn; const char* longn; };
static const TypeLabel TYPE_LABELS[FT_COUNT] = {
  { "empty",  "(empty file)"        },
  { "PNG",    "PNG image"           },
  { "JPEG",   "JPEG image"          },
  { "GIF",    "GIF image"           },
  { "BMP",    "BMP image"           },
  { "WEBP",   "WebP image"          },
  { "ICO",    "icon (ICO)"          },
  { "PDF",    "PDF document"        },
  { "ZIP",    "ZIP / OOXML / jar"   },
  { "GZIP",   "gzip archive"        },
  { "RAR",    "RAR archive"         },
  { "7Z",     "7-Zip archive"       },
  { "XZ",     "xz archive"          },
  { "BZ2",    "bzip2 archive"       },
  { "ELF",    "ELF executable"      },
  { "EXE",    "DOS/PE executable"   },
  { "MACHO",  "Mach-O binary"       },
  { "CLASS",  "Java class"          },
  { "WASM",   "WebAssembly"         },
  { "MP3",    "MP3 audio"           },
  { "MP4",    "MP4 / MOV / ISO-BMFF"},
  { "WAV",    "WAV audio"           },
  { "AVI",    "AVI video"           },
  { "OGG",    "Ogg media"           },
  { "FLAC",   "FLAC audio"          },
  { "SQLITE", "SQLite database"     },
  { "SH",     "script (#!)"         },
  { "TEXT",   "text"                },
  { "?",      "unknown / binary"    },
};

// Classifies a file from the first `n` bytes of its contents (and total size).
static int detect_type(const uint8_t* b, int n, uint64_t size) {
  if (size == 0 || n == 0) return FT_EMPTY;
  #define HAS(k) (n >= (int)(k))
  if (HAS(8)  && !memcmp(b, "\x89PNG\r\n\x1a\n", 8))                 return FT_PNG;
  if (HAS(3)  && b[0]==0xFF && b[1]==0xD8 && b[2]==0xFF)             return FT_JPEG;
  if (HAS(6)  && (!memcmp(b,"GIF87a",6) || !memcmp(b,"GIF89a",6)))   return FT_GIF;
  if (HAS(12) && !memcmp(b,"RIFF",4) && !memcmp(b+8,"WEBP",4))       return FT_WEBP;
  if (HAS(12) && !memcmp(b,"RIFF",4) && !memcmp(b+8,"WAVE",4))       return FT_WAV;
  if (HAS(12) && !memcmp(b,"RIFF",4) && !memcmp(b+8,"AVI ",4))       return FT_AVI;
  if (HAS(2)  && b[0]=='B' && b[1]=='M')                            return FT_BMP;
  if (HAS(4)  && b[0]==0 && b[1]==0 && b[2]==1 && b[3]==0)          return FT_ICO;
  if (HAS(5)  && !memcmp(b,"%PDF-",5))                              return FT_PDF;
  if (HAS(4)  && b[0]=='P' && b[1]=='K' && (b[2]==3||b[2]==5||b[2]==7)) return FT_ZIP;
  if (HAS(2)  && b[0]==0x1F && b[1]==0x8B)                          return FT_GZIP;
  if (HAS(7)  && !memcmp(b,"Rar!\x1a\x07",6))                       return FT_RAR;
  if (HAS(6)  && !memcmp(b,"7z\xbc\xaf\x27\x1c",6))                 return FT_7Z;
  if (HAS(6)  && !memcmp(b,"\xfd""7zXZ\x00",6))                     return FT_XZ;
  if (HAS(3)  && !memcmp(b,"BZh",3))                               return FT_BZ2;
  if (HAS(4)  && !memcmp(b,"\x7f""ELF",4))                          return FT_ELF;
  if (HAS(4)  && mfs_be32(b)==0xCAFEBABE)                           return FT_CLASS;
  if (HAS(4)  && (mfs_be32(b)==0xFEEDFACE || mfs_be32(b)==0xFEEDFACF ||
                  mfs_le32(b)==0xFEEDFACE || mfs_le32(b)==0xFEEDFACF)) return FT_MACHO;
  if (HAS(4)  && !memcmp(b,"\x00""asm",4))                          return FT_WASM;
  if (HAS(2)  && b[0]=='M' && b[1]=='Z')                           return FT_PE;
  if (HAS(12) && !memcmp(b+4,"ftyp",4))                            return FT_MP4;
  if (HAS(4)  && !memcmp(b,"OggS",4))                              return FT_OGG;
  if (HAS(4)  && !memcmp(b,"fLaC",4))                              return FT_FLAC;
  if (HAS(3)  && !memcmp(b,"ID3",3))                               return FT_MP3;
  if (HAS(2)  && b[0]==0xFF && (b[1]&0xE0)==0xE0)                   return FT_MP3;
  if (HAS(16) && !memcmp(b,"SQLite format 3\x00",16))              return FT_SQLITE;
  if (HAS(2)  && b[0]=='#' && b[1]=='!')                           return FT_SCRIPT;
  #undef HAS
  // Printable-ASCII heuristic (checked last, before giving up).
  for (int i = 0; i < n; i++) {
    uint8_t c = b[i];
    if (c=='\t' || c=='\n' || c=='\r' || c=='\f') continue;
    if (c >= 0x20 && c <= 0x7E) continue;
    return FT_UNKNOWN;
  }
  return FT_TEXT;
}

// --- directory-walk queue -------------------------------------------------
static void dirqReset() { g_dirqHead = g_dirqTail = g_dirqCount = 0; }
static bool dirqPush(const char* path) {
  if (g_dirqCount >= DIRQ_MAX) { g_scanTrunc = true; return false; }
  if (strlen(path) >= SCAN_PATH_LEN) { g_scanTrunc = true; return false; }
  strcpy(g_dirq[g_dirqTail], path);
  g_dirqTail = (g_dirqTail + 1) % DIRQ_MAX;
  g_dirqCount++;
  return true;
}
static bool dirqPop(char* out) {
  if (g_dirqCount == 0) return false;
  strcpy(out, g_dirq[g_dirqHead]);
  g_dirqHead = (g_dirqHead + 1) % DIRQ_MAX;
  g_dirqCount--;
  return true;
}

// Joins directory `dir` and child `name` into a VFS path. Returns false (and
// leaves `out` untouched) if the result would not fit -- the caller then skips
// that subtree/file rather than storing a truncated, un-openable path.
static bool joinPath(const char* dir, const char* name, char* out) {
  bool root = (dir[0]=='/' && dir[1]=='\0');
  int need = (root ? 1 : (int)strlen(dir) + 1) + (int)strlen(name) + 1;
  if (need > SCAN_PATH_LEN) return false;
  if (root) snprintf(out, SCAN_PATH_LEN, "/%s", name);
  else      snprintf(out, SCAN_PATH_LEN, "%s/%s", dir, name);
  return true;
}

// Records one file in the flat list: probe its magic bytes and classify it.
static void scanRecordFile(const char* vpath, uint64_t size) {
  if (g_scanCount >= SCAN_MAX) { g_scanTrunc = true; return; }
  uint8_t hdr[64];
  int hn = readFileHead(vpath, hdr, sizeof(hdr));
  if (hn < 0) hn = 0;
  ScanItem& it = g_scan[g_scanCount];
  strcpy(it.path, vpath);                             // length checked by joinPath
  it.size = (size > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32_t)size;
  it.type = (uint8_t)detect_type(hdr, hn, size);
  g_scanCount++;
}

// Breadth-first walk of every volume, filling g_scan with files. Iterative (a
// global queue, not recursion) so it stays within core1's small stack. Reuses
// doList() per directory: its g_entries result is fully consumed -- subdirs
// queued, files recorded -- before the next doList() overwrites it.
static void doScan() {
  g_scanCount = 0;
  g_scanTrunc = false;
  dirqReset();
  if (g_nVols <= 0) { g_reqOk = false; return; }

  if (g_nVols == 1) {
    dirqPush("/");
  } else {
    for (int v = 1; v <= g_nVols; v++) { char p[8]; snprintf(p, sizeof(p), "/%d", v); dirqPush(p); }
  }

  char dir[SCAN_PATH_LEN];
  char child[SCAN_PATH_LEN];
  while (g_scanCount < SCAN_MAX && dirqPop(dir)) {
    doList(dir);                                      // fills g_entries/g_nEntries
    if (!g_reqOk) continue;
    int n = g_nEntries;
    for (int i = 0; i < n && g_scanCount < SCAN_MAX; i++) {
      if (!joinPath(dir, g_entries[i].name, child)) { g_scanTrunc = true; continue; }
      if (g_entries[i].dir) dirqPush(child);
      else                  scanRecordFile(child, g_entries[i].size);
    }
  }
  if (g_dirqCount > 0) g_scanTrunc = true;            // more directories than we drained
  g_reqOk = true;
}

// True if `ext` (no dot, any case) is a Windows/Unix executable or script type
// commonly abused to smuggle code inside an archive.
static bool dangerousExt(const char* e) {
  static const char* const BAD[] = {
    "exe","dll","scr","com","pif","cpl","sys","drv","ocx","msi","bat","cmd",
    "js","jse","vbs","vbe","wsf","wsh","hta","ps1","psm1","lnk","scf","reg",
    "jar","apk","sh","bash","py","pl","rb","elf","so","dylib","bin","out","app", nullptr
  };
  for (int i = 0; BAD[i]; i++) if (iequals(e, BAD[i])) return true;
  return false;
}

// Parses the ZIP central directory of `path` (size `fsz`) and records the names
// of embedded entries, flagging the first that looks executable/scripted. Uses
// the central directory (not the local headers) so streamed / data-descriptor
// zips are handled correctly. Bounded + forward-progress guarded (bomb-safe).
static void zipInspect(const char* path, uint64_t fsz) {
  if (fsz < 22) return;                               // too small for an EOCD

  // 1) Locate the End Of Central Directory ("PK\x05\x06") in the file tail.
  uint64_t scanStart = (fsz > EOCD_SCAN_MAX) ? (fsz - EOCD_SCAN_MAX) : 0;
  int64_t  eocd = -1;
  uint8_t  prev[3] = {0,0,0};                         // carry across chunk boundary
  bool     havePrev = false;
  static uint8_t buf[256];                            // static: core1 stack is only 2 KB
  for (uint64_t off = scanStart; off < fsz; ) {
    int r = readFileAt(path, off, buf, sizeof(buf));
    if (r < 4) break;
    // check boundary (last 3 of prev chunk + first of this)
    if (havePrev && prev[0]=='P' && prev[1]=='K' && prev[2]==0x05 && buf[0]==0x06)
      eocd = (int64_t)off - 1;
    for (int i = 0; i + 4 <= r; i++)
      if (buf[i]=='P' && buf[i+1]=='K' && buf[i+2]==0x05 && buf[i+3]==0x06)
        eocd = (int64_t)off + i;                      // keep the LAST match
    prev[0]=buf[r-3]; prev[1]=buf[r-2]; prev[2]=buf[r-1]; havePrev = (r>=3);
    off += r;
  }
  if (eocd < 0) return;                               // no central directory found

  uint8_t rec[22];
  if (readFileAt(path, (uint64_t)eocd, rec, 22) < 22) return;
  uint16_t total  = mfs_le16(rec + 10);
  uint32_t cdOff  = mfs_le32(rec + 16);
  g_insp.zipParsed = true;

  // 2) Walk the central directory ("PK\x01\x02" records).
  uint64_t off = cdOff;
  int iters = 0;
  int limit = (total < ZIP_MAX_ENTRIES) ? total : ZIP_MAX_ENTRIES;
  while (g_insp.zipEntries < limit && iters++ < ZIP_MAX_ITERS && off + 46 <= fsz) {
    uint8_t h[46];
    if (readFileAt(path, off, h, 46) < 46) break;
    if (!(h[0]=='P' && h[1]=='K' && h[2]==0x01 && h[3]==0x02)) break;
    uint16_t nlen = mfs_le16(h + 28);
    uint16_t elen = mfs_le16(h + 30);
    uint16_t clen = mfs_le16(h + 32);

    char nm[40];
    int take = (nlen < (int)sizeof(nm) - 1) ? nlen : (int)sizeof(nm) - 1;
    int got = (take > 0) ? readFileAt(path, off + 46, (uint8_t*)nm, take) : 0;
    if (got < 0) got = 0;
    for (int i = 0; i < got; i++) if (nm[i] < 0x20 || nm[i] > 0x7E) nm[i] = '?';
    nm[got] = '\0';

    g_insp.zipEntries++;
    const char* base = strrchr(nm, '/'); base = base ? base + 1 : nm;
    const char* dot  = strrchr(base, '.');
    if (dot && dangerousExt(dot + 1) && !g_insp.zipExec) {
      g_insp.zipExec = true;
      strncpy(g_insp.zipExecName, nm, sizeof(g_insp.zipExecName) - 1);
      g_insp.zipExecName[sizeof(g_insp.zipExecName) - 1] = '\0';
    }

    uint64_t next = off + 46 + nlen + elen + clen;
    if (next <= off) { g_insp.zipTrunc = true; break; }   // forward-progress guard
    off = next;
  }
  if (iters >= ZIP_MAX_ITERS || g_insp.zipEntries >= limit)
    g_insp.zipTrunc = true;
}

// Sweeps `path` for a distinctive container/executable magic appearing at any
// offset > 0 (appended payloads / polyglots). Only long, low-false-positive
// signatures are used. Reads sequential chunks with a small carry so a magic
// straddling a boundary is still caught. Bounded to SWEEP_MAX_BYTES.
static void sweepInspect(const char* path, uint64_t fsz) {
  struct Sig { const char* label; const uint8_t* pat; int len; };
  static const uint8_t P_ZIP[]={'P','K',3,4}, P_ELF[]={0x7f,'E','L','F'},
    P_PNG[]={0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a}, P_PDF[]={'%','P','D','F','-'},
    P_RAR[]={'R','a','r','!',0x1a,0x07}, P_7Z[]={'7','z',0xbc,0xaf,0x27,0x1c},
    P_GZ[]={0x1f,0x8b,0x08}, P_CLS[]={0xca,0xfe,0xba,0xbe};
  static const Sig SIGS[] = {
    {"ZIP",P_ZIP,4},{"ELF",P_ELF,4},{"PNG",P_PNG,8},{"PDF",P_PDF,5},
    {"RAR",P_RAR,6},{"7Z",P_7Z,6},{"GZIP",P_GZ,3},{"CLASS",P_CLS,4},
  };
  const int nSigs = (int)(sizeof(SIGS)/sizeof(SIGS[0]));
  const int CARRY = 8;                                // >= longest signature

  g_insp.sweepDone = true;
  uint64_t cap = (fsz < SWEEP_MAX_BYTES) ? fsz : SWEEP_MAX_BYTES;
  if (fsz > SWEEP_MAX_BYTES) g_insp.sweepTrunc = true;

  static uint8_t win[512 + 8];                        // static: core1 stack is only 2 KB
  int     carry = 0;                                  // bytes of previous tail in win[0..]
  uint64_t base = 0;                                  // absolute offset of win[0]
  for (uint64_t off = 0; off < cap; ) {
    int r = readFileAt(path, off, win + carry, 512);
    if (r <= 0) break;
    int have = carry + r;
    for (int i = 0; i + 1 <= have; i++) {
      uint64_t abs = base + i;
      if (abs == 0) continue;                         // offset 0 is the file's own type
      for (int s = 0; s < nSigs; s++) {
        if (i + SIGS[s].len > have) continue;
        if (!memcmp(win + i, SIGS[s].pat, SIGS[s].len)) {
          g_insp.embedded   = true;
          g_insp.embeddedOff = (uint32_t)abs;
          strncpy(g_insp.embeddedWhat, SIGS[s].label, sizeof(g_insp.embeddedWhat) - 1);
          g_insp.embeddedWhat[sizeof(g_insp.embeddedWhat) - 1] = '\0';
          return;                                     // first hit is enough
        }
      }
    }
    // carry the last CARRY bytes so a boundary-straddling magic is not missed
    carry = (have >= CARRY) ? CARRY : have;
    memmove(win, win + have - carry, carry);
    base = off + r - carry;
    off += r;
  }
}

static void doInspect(const char* path) {
  memset(&g_insp, 0, sizeof(g_insp));
  int64_t fsz = fileSizeOf(path);
  if (fsz < 0) { g_reqOk = false; return; }

  uint8_t head[4];
  int hn = readFileAt(path, 0, head, 4);
  if (hn >= 4 && head[0]=='P' && head[1]=='K' && head[2]==3 && head[3]==4)
    zipInspect(path, (uint64_t)fsz);

  if (g_inspectDeep)
    sweepInspect(path, (uint64_t)fsz);

  g_reqOk = true;
}

// Invoked from loop1() on core1, after USBHost.task().
void mscService() {
  if (g_reqDone || g_req == MSC_NONE) return;
  __sync_synchronize();
  if (!g_mscMounted)          g_reqOk = false;
  else if (g_req == MSC_LIST) doList(g_reqPath);
  else if (g_req == MSC_READ) doRead(g_reqPath);
  else if (g_req == MSC_SCAN) doScan();
  else if (g_req == MSC_INSPECT) doInspect(g_reqPath);
  __sync_synchronize();
  g_req = MSC_NONE;
  g_reqDone = true;
}

//--------------------------------------------------------------------+
// Request an operation from core0 and wait for core1 to finish it.
//--------------------------------------------------------------------+
static bool mscDo(int r, const char* path, uint32_t timeoutMs) {
  strncpy(g_reqPath, path, sizeof(g_reqPath) - 1);
  g_reqPath[sizeof(g_reqPath) - 1] = 0;
  g_reqOk   = false;
  g_reqDone = false;
  __sync_synchronize();
  g_req = r;
  uint32_t t = to_ms_since_boot(get_absolute_time());
  while (!g_reqDone) {
    delay(2);   // yields so the TinyUSB device stack keeps running
    if (to_ms_since_boot(get_absolute_time()) - t > timeoutMs) return false;   // timeout
  }
  __sync_synchronize();
  return g_reqOk;
}

//====================================================================
// Serial frontend: LS / CAT
//====================================================================
static void human_size(uint64_t s, char* out, size_t n) {
  if      (s < 1024ULL)                 snprintf(out, n, "%luB", (unsigned long)s);
  else if (s < 1024ULL * 1024)          snprintf(out, n, "%luK", (unsigned long)(s / 1024ULL));
  else if (s < 1024ULL * 1024 * 1024)   snprintf(out, n, "%luM", (unsigned long)(s / (1024ULL * 1024)));
  else                                  snprintf(out, n, "%luG", (unsigned long)(s / (1024ULL * 1024 * 1024)));
}

void explorer_ls(const char* path) {
  if (!g_mscMounted) {
    if (g_mscPresent) SerialTinyUSB.println("[!] Disk attached but filesystem not supported (FAT/exFAT/NTFS/ext/ISO9660)");
    else              SerialTinyUSB.println("[!] No mass-storage device mounted");
    return;
  }
  if (!mscDo(MSC_LIST, path, 8000)) {
    SerialTinyUSB.printf("[!] Cannot list '%s'\r\n", path);
    return;
  }
  bool volList = (g_nVols > 1 &&
                  (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')));
  if (volList) {
    SerialTinyUSB.printf("\r\n=== Volumes (%d) ===  [LS /N to open]\r\n", g_nEntries);
  } else {
    SerialTinyUSB.printf("\r\n=== LS %s [%s%s%s] (%d entries) ===\r\n", path,
                         g_fsName, g_fsLabel[0] ? " " : "", g_fsLabel, g_nEntries);
  }
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
  if (!mscDo(MSC_READ, path, 8000)) {
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

// Returns the extension of a path (no dot), "" if none.
static const char* path_ext(const char* path) {
  const char* base = strrchr(path, '/'); base = base ? base + 1 : path;
  const char* dot  = strrchr(base, '.');
  return (dot && dot != base) ? dot + 1 : "";
}

// Serial "SCAN": recursively walk the whole device and print every file tagged
// with the type guessed from its magic bytes, its size, and its first bytes.
// Flags files whose true (magic) type is executable code but whose extension
// pretends otherwise -- the classic "malware disguised as a photo/doc" case.
void explorer_scan() {
  if (!g_mscMounted) {
    if (g_mscPresent) SerialTinyUSB.println("[!] Disk attached but filesystem not supported (FAT/exFAT/NTFS/ext/ISO9660)");
    else              SerialTinyUSB.println("[!] No mass-storage device mounted");
    return;
  }

  SerialTinyUSB.println("\r\n=== SCAN (recursive magic-byte analysis) ===");
  if (!mscDo(MSC_SCAN, "/", 30000)) {
    SerialTinyUSB.println("[!] Scan failed / timed out");
    return;
  }
  int n = g_scanCount;
  SerialTinyUSB.printf("Found %d file(s)%s\r\n\r\n", n,
                       g_scanTrunc ? "  (TRUNCATED: list/queue limit hit)" : "");

  int flagged = 0;
  for (int i = 0; i < n; i++) {
    uint8_t t = g_scan[i].type; if (t >= FT_COUNT) t = FT_UNKNOWN;
    char sz[10]; human_size(g_scan[i].size, sz, sizeof(sz));

    // Re-read the head so we can print the actual magic bytes.
    char hex[3 * 16 + 1] = { 0 };
    char asc[16 + 1]     = { 0 };
    int shown = 0;
    if (mscDo(MSC_READ, g_scan[i].path, 8000)) {
      int m = g_fileHeadLen; if (m > 16) m = 16;
      int p = 0;
      for (int b = 0; b < m; b++) {
        p += snprintf(hex + p, sizeof(hex) - p, "%02X ", (uint8_t)g_fileHead[b]);
        char c = g_fileHead[b];
        asc[b] = (c >= 0x20 && c < 0x7F) ? c : '.';
      }
      asc[m] = 0;
      shown  = m;
    }

    SerialTinyUSB.printf("%-20s %6s  %s\r\n", TYPE_LABELS[t].longn, sz, g_scan[i].path);
    if (shown > 0)
      SerialTinyUSB.printf("    magic: %-48s|%s|\r\n", hex, asc);

    // Disguise check: real type is executable code, extension says otherwise.
    bool execType = (t == FT_ELF || t == FT_PE || t == FT_MACHO ||
                     t == FT_CLASS || t == FT_WASM);
    const char* e = path_ext(g_scan[i].path);
    bool execExt = (e[0] == '\0' || iequals(e,"exe") || iequals(e,"dll") ||
                    iequals(e,"elf") || iequals(e,"so")  || iequals(e,"bin") ||
                    iequals(e,"com") || iequals(e,"class")|| iequals(e,"wasm")||
                    iequals(e,"o")   || iequals(e,"dylib")|| iequals(e,"out") ||
                    iequals(e,"sys") || iequals(e,"ko"));
    if (execType && !execExt) {
      SerialTinyUSB.printf("    [!] WARNING: executable code posing as '.%s' -- possible disguise\r\n", e);
      flagged++;
    }

    // Deep look inside: ZIP central directory (cheap, any size) + embedded-
    // signature sweep (only small files here, to keep the bulk scan fast).
    g_inspectDeep = (g_scan[i].size <= SWEEP_BULK_MAX);
    if (mscDo(MSC_INSPECT, g_scan[i].path, 15000)) {
      if (g_insp.zipParsed) {
        SerialTinyUSB.printf("    zip: %d entr%s%s\r\n", g_insp.zipEntries,
                             g_insp.zipEntries == 1 ? "y" : "ies",
                             g_insp.zipTrunc ? " (truncated)" : "");
        if (g_insp.zipExec) {
          SerialTinyUSB.printf("    [!] ARCHIVE carries executable/script: %s\r\n", g_insp.zipExecName);
          flagged++;
        }
      }
      if (g_insp.embedded) {
        SerialTinyUSB.printf("    [!] EMBEDDED %s signature at offset 0x%lX "
                             "(possible polyglot / appended payload)\r\n",
                             g_insp.embeddedWhat, (unsigned long)g_insp.embeddedOff);
        flagged++;
      }
    }
  }

  SerialTinyUSB.printf("\r\n%d file(s), %d suspicious finding(s).\r\n", n, flagged);
  SerialTinyUSB.println("(Deep sweep in bulk covers files <=128KB; use INSPECT <path> for a");
  SerialTinyUSB.println(" full 1MB sweep. Nothing is decompressed, so it is zip-bomb-safe.)");
  SerialTinyUSB.println("=============================================\r\n");
}

// Serial "INSPECT <path>": full deep look at ONE file -- ZIP entry listing and a
// 1 MB embedded-signature sweep. Heavier than SCAN's per-file pass, run on demand.
void explorer_inspect(const char* path) {
  if (!g_mscMounted) { SerialTinyUSB.println("[!] No mass-storage device mounted"); return; }

  SerialTinyUSB.printf("\r\n=== INSPECT %s ===\r\n", path);
  if (!mscDo(MSC_READ, path, 8000)) { SerialTinyUSB.printf("[!] Cannot read '%s'\r\n", path); return; }
  int t = detect_type((uint8_t*)g_fileHead, g_fileHeadLen, (uint64_t)g_fileHeadLen);
  if (t < 0 || t >= FT_COUNT) t = FT_UNKNOWN;
  SerialTinyUSB.printf("Type (magic): %s\r\n", TYPE_LABELS[t].longn);

  g_inspectDeep = true;
  if (!mscDo(MSC_INSPECT, path, 30000)) { SerialTinyUSB.println("[!] Inspect failed / timed out"); return; }

  if (g_insp.zipParsed) {
    SerialTinyUSB.printf("ZIP: %d entr%s%s\r\n", g_insp.zipEntries,
                         g_insp.zipEntries == 1 ? "y" : "ies",
                         g_insp.zipTrunc ? " (truncated/limit hit)" : "");
    if (g_insp.zipExec)
      SerialTinyUSB.printf("  [!] carries executable/script entry: %s\r\n", g_insp.zipExecName);
    else
      SerialTinyUSB.println("  no executable/script entry name detected");
  }
  if (g_insp.sweepDone) {
    if (g_insp.embedded)
      SerialTinyUSB.printf("  [!] EMBEDDED %s signature at offset 0x%lX (polyglot / appended payload)\r\n",
                           g_insp.embeddedWhat, (unsigned long)g_insp.embeddedOff);
    else
      SerialTinyUSB.printf("  no embedded signature found%s\r\n",
                           g_insp.sweepTrunc ? " (only first 1MB swept)" : "");
  }
  SerialTinyUSB.println("=============================\r\n");
}

//====================================================================
// OLED frontend: file browser (core0)
//====================================================================
//
// 4-button navigation. The whole browser/menu only asks nav_poll() for one of
// five actions, so this is the single place that touches the physical buttons.
//
//   Up     -> PREV    (move selection up / scroll up / previous menu item)
//   Down   -> NEXT    (move selection down / scroll down / next menu item)
//   Accept -> SELECT  (enter dir / open file / toggle view / choose item)
//   Bak    -> BACK    (up a level / cancel / return to the previous menu)
//
// Each button is edge-triggered on press (active-low, internal pull-ups). Only
// one action is returned per call; if several edges land in the same poll the
// order below (PREV, NEXT, SELECT, BACK) decides which one wins.
//
// NavAction values are returned as int (not the enum type) so the Arduino
// auto-generated prototype for nav_poll() doesn't reference a type declared
// further down in the combined sketch.
enum { NAV_IDLE = 0, NAV_PREV, NAV_NEXT, NAV_SELECT, NAV_BACK };

static int nav_poll() {
  static bool up_prev  = true;   // true == HIGH == released (pull-ups)
  static bool dn_prev  = true;
  static bool ok_prev  = true;
  static bool bak_prev = true;
  int act = NAV_IDLE;

  bool up_now  = digitalRead(BTN_UP);
  bool dn_now  = digitalRead(BTN_DOWN);
  bool ok_now  = digitalRead(BTN_ACCEPT);
  bool bak_now = digitalRead(BTN_BAK);

  if (up_now != up_prev)   { up_prev  = up_now;  if (up_now  == LOW)                    act = NAV_PREV;   }
  if (dn_now != dn_prev)   { dn_prev  = dn_now;  if (dn_now  == LOW && act == NAV_IDLE) act = NAV_NEXT;   }
  if (ok_now != ok_prev)   { ok_prev  = ok_now;  if (ok_now  == LOW && act == NAV_IDLE) act = NAV_SELECT; }
  if (bak_now != bak_prev) { bak_prev = bak_now; if (bak_now == LOW && act == NAV_IDLE) act = NAV_BACK;   }

  return act;
}

// Block until every button is released (so a press isn't read twice across the
// modal boundary between, e.g., the menu and the browser it launches).
static void nav_wait_release() {
  while (digitalRead(BTN_UP)     == LOW || digitalRead(BTN_DOWN) == LOW ||
         digitalRead(BTN_ACCEPT) == LOW || digitalRead(BTN_BAK)  == LOW) delay(5);
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

// File analyzer detail view: shows the type guessed from the magic bytes, the
// size, and a scrollable hex / ASCII dump of the first bytes. Accept toggles
// hex<->ASCII, Up/Dn scroll, Bak returns to the list.
static void analyze_view(const char* path, const char* name, uint32_t size, uint8_t type) {
  if (type >= FT_COUNT) type = FT_UNKNOWN;

  setNeoColor(255, 165, 0);
  display.clearDisplay();
  draw_titlebar("ANALYZE");
  display.setCursor(0, 28);
  display.print("Reading...");
  display.display();

  bool ok = mscDo(MSC_READ, path, 8000);
  nav_wait_release();
  if (!ok) {
    display.clearDisplay();
    draw_titlebar("ANALYZE");
    display.setCursor(0, 28);
    display.print("Read failed");
    display.display();
    delay(1000);
    return;
  }

  // Deep look inside the file (zip entries + embedded-signature sweep). This
  // clobbers g_fileHead, so capture a short warning then refill the head buffer
  // for the hex/ASCII dump below.
  char warn[22] = { 0 };
  g_inspectDeep = true;
  if (mscDo(MSC_INSPECT, path, 20000)) {
    if (g_insp.zipExec)
      snprintf(warn, sizeof(warn), "! zip:%.14s", g_insp.zipExecName);
    else if (g_insp.embedded)
      snprintf(warn, sizeof(warn), "! embed %s @%lX", g_insp.embeddedWhat,
               (unsigned long)g_insp.embeddedOff);
  }
  mscDo(MSC_READ, path, 8000);                 // refill g_fileHead for the dump
  nav_wait_release();

  int len = g_fileHeadLen;
  bool hexMode = true;
  int  scroll  = 0;
  const int PERLINE = 6;
  const int VIS     = 3;                        // guiHexView shows up to 3 rows
  const char* typeLong = TYPE_LABELS[type].longn;

  for (;;) {
    int act = nav_poll();
    if (act == NAV_BACK)   { setNeoColor(0, 0, 255); return; }
    if (act == NAV_SELECT) hexMode = !hexMode;  // toggle hex <-> ASCII
    if (act == NAV_NEXT)   scroll++;
    if (act == NAV_PREV)   scroll--;
    if (scroll < 0) scroll = 0;
    int totalLines = (len + PERLINE - 1) / PERLINE;
    if (totalLines > 0 && scroll > totalLines - 1) scroll = totalLines - 1;

    setNeoColor(0, 25, 0);

    // Info line: the threat warning takes priority; otherwise size + dump mode.
    char info[22];
    if (warn[0]) {
      snprintf(info, sizeof(info), "%s", warn);
    } else {
      char sz[10]; human_size(size, sz, sizeof(sz));
      snprintf(info, sizeof(info), "%s %s", sz, hexMode ? "HEX" : "ASCII");
    }

    // Up to 3 dump rows from the current scroll position.
    char rowbuf[VIS][24];
    const char* rows[VIS];
    for (int r = 0; r < VIS; r++) {
      int off = (scroll + r) * PERLINE;
      if (off >= len) { rows[r] = NULL; continue; }
      int p = snprintf(rowbuf[r], sizeof(rowbuf[r]), "%03X:", off);
      if (hexMode) {
        for (int i = 0; i < PERLINE && off + i < len; i++)
          p += snprintf(rowbuf[r] + p, sizeof(rowbuf[r]) - p, " %02X", (uint8_t)g_fileHead[off + i]);
      } else {
        if (p < (int)sizeof(rowbuf[r]) - 1) rowbuf[r][p++] = ' ';
        for (int i = 0; i < PERLINE && off + i < len && p < (int)sizeof(rowbuf[r]) - 1; i++) {
          char c = g_fileHead[off + i];
          rowbuf[r][p++] = (c >= 32 && c <= 126) ? c : '.';
        }
        rowbuf[r][p] = 0;
      }
      rows[r] = rowbuf[r];
    }
    if (len == 0) { snprintf(rowbuf[0], sizeof(rowbuf[0]), "(empty file)"); rows[0] = rowbuf[0]; }

    guiHexView(name, typeLong, info, rows, VIS);
    delay(30);
  }
}

// Flat file analyzer. On entry it recursively scans every volume (filling
// g_scan with one row per file, each tagged by its magic bytes) and presents a
// single flat, scrollable list -- no directory tree. Accept opens the per-file
// magic-byte / hex view; Bak leaves the analyzer.
// Row-text callbacks for guiFileList: file name (basename) and its magic type.
static const char* scanRowName(int idx, void* /*ctx*/) {
  const char* p = g_scan[idx].path;
  const char* nm = strrchr(p, '/');
  return nm ? nm + 1 : p;
}
static const char* scanRowType(int idx, void* /*ctx*/) {
  uint8_t t = g_scan[idx].type; if (t >= FT_COUNT) t = FT_UNKNOWN;
  return TYPE_LABELS[t].shortn;
}

void explorer_analyze() {
  nav_wait_release();

  // Recursive scan (longer timeout than a single read: it walks the whole tree).
  setNeoColor(255, 165, 0);
  display.clearDisplay();
  draw_titlebar("ANALYZER");
  display.setCursor(0, 28);
  display.print("Scanning...");
  display.display();
  bool ok = mscDo(MSC_SCAN, "/", 30000);
  (void)ok;
  nav_wait_release();

  int sel = 0, top = 0;
  const int VIS = GUI_FILELIST_VIS;   // matches guiFileList's visible rows

  for (;;) {
    int act = nav_poll();
    if (act == NAV_BACK) { setNeoColor(0, 0, 255); return; }

    // No browsable volume: show a hint (Bak still exits).
    if (!g_mscMounted) {
      setNeoColor(0, 0, 40);
      display.clearDisplay();
      draw_titlebar("USB STORAGE");
      display.setCursor(0, 20);
      if (g_mscPresent) {
        display.print("FS not supported");
        display.setCursor(0, 32);
        display.print("FAT/exFAT/NTFS/ext/ISO");
      } else {
        display.print("Insert a USB drive");
        display.setCursor(0, 32);
        display.print("in the HOST port");
      }
      display.setCursor(0, 56);
      display.print("Bak: exit");
      display.display();
      delay(60);
      continue;
    }

    int n = g_scanCount;
    if (act == NAV_NEXT && n > 0) { sel++; if (sel >= n) sel = 0; }
    if (act == NAV_PREV && n > 0) { sel--; if (sel < 0)  sel = n - 1; }
    if (sel < top)        top = sel;
    if (sel >= top + VIS) top = sel - VIS + 1;

    if (act == NAV_SELECT && n > 0) {
      const char* p  = g_scan[sel].path;
      const char* nm = strrchr(p, '/');
      nm = nm ? nm + 1 : p;
      analyze_view(p, nm, g_scan[sel].size, g_scan[sel].type);
      nav_wait_release();
      continue;
    }

    setNeoColor(0, 40, 0);
    guiFileList(n, top, sel, scanRowName, scanRowType, NULL);
    delay(20);
  }
}

//====================================================================
// OLED frontend: on-device menu (core0)
//====================================================================
//
// Opened from loop() when Accept is pressed while a USB device is connected.
// It is a blocking modal (same model as explorer_analyze): it owns the four
// buttons until the user leaves it. Labels are kept short so they fit the OLED.
//
//   Info   -> USB device properties (descriptor pages)
//   Files  -> magic-byte file analyzer (explorer_analyze)
//   Wipe   -> destroy the partition tables (GPT/MBR) of the mounted storage
//
// The menu is a list of boxed rows (Info/Files/Wipe), each with a status icon,
// over the Lab217 logo. Up/Down step through the rows (wrapping) and the
// selected one is drawn as a filled (inverted) box. Accept (Ok) enters the row;
// Bak closes the menu and returns to the status screen.
//
#define MENU_N 3   // Info, Files, Wipe (navigation wrap count)

// Draws the mass-storage options menu with item `sel` highlighted. The layout
// and artwork now live in GUI_Base.h (guiMassMenu); this is just the hook the
// menu loop calls.
static void menu_draw(int sel) {
  guiMassMenu(sel);
}

// "Info": pages through the connected device's USB descriptor (reuses the
// existing renderer). Up/Down change page, Bak returns to the menu.
static void menu_info_view() {
  descriptor_view_active = true;
  int page = 0;
  bool need = true;
  for (;;) {
    int act = nav_poll();
    if (act == NAV_BACK) break;
    if (act == NAV_NEXT) { page = (page + 1) % 3; need = true; }
    if (act == NAV_PREV) { page = (page + 2) % 3; need = true; }
    if (need) {
      descriptor_page = page;
      show_descriptor_page();
      need = false;
    }
    delay(20);
  }
  descriptor_view_active = false;
  descriptor_page = 0;
}

// "Wipe": asks for confirmation before arming the partition-table wipe. Returns
// true if the wipe was requested (so the caller leaves the menu and lets loop()
// report WIPE DONE / WIPE ERROR). Returns false if the user cancelled.
static bool menu_wipe_confirm() {
  // Nothing to wipe if no mass-storage device is mounted.
  if (!msc_mounted) {
    display.clearDisplay();
    draw_titlebar("WIPE");
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 24);
    display.print("No USB storage");
    display.setCursor(0, 34);
    display.print("mounted.");
    display.setCursor(0, 56);
    display.print("Bak: back");
    display.display();
    for (;;) {
      int act = nav_poll();
      if (act == NAV_BACK || act == NAV_SELECT) return false;
      delay(20);
    }
  }

  // New UX (per design): the screen itself is the confirmation -- Ok wipes
  // (destructive), Bak cancels. No on-screen Yes/No toggle.
  guiWipe();
  for (;;) {
    int act = nav_poll();
    if (act == NAV_BACK) return false;
    if (act == NAV_SELECT) {
      // Arm the wipe: core1 (host stack) picks up wipe_requested in loop1().
      wipe_done = false;
      wipe_error = false;
      wipe_done_reported = false;
      wipe_error_reported = false;
      wipe_requested = true;
      return true;
    }
    delay(20);
  }
}

// Top-level menu. Blocks until the user backs out (or a wipe is launched).
void explorer_menu() {
  nav_wait_release();
  int sel = 0;
  bool need = true;

  for (;;) {
    int act = nav_poll();

    if (act == NAV_PREV) { sel = (sel - 1 + MENU_N) % MENU_N; need = true; }
    if (act == NAV_NEXT) { sel = (sel + 1) % MENU_N; need = true; }
    if (act == NAV_BACK) break;                       // Bak at top level -> close

    if (act == NAV_SELECT) {
      switch (sel) {
        case 0:                                       // Info
          menu_info_view();
          break;
        case 1:                                       // Files
          explorer_analyze();
          break;
        case 2:                                       // Wipe
          if (menu_wipe_confirm()) {
            // Wipe armed: leave the menu so loop() can render its progress.
            showStatus("WIPING DISK", LOGO_ALERT_SYMBOL);
            setNeoColor(255, 165, 0);
            nav_wait_release();
            return;
          }
          break;
      }
      nav_wait_release();
      need = true;
    }

    if (need) { menu_draw(sel); need = false; }
    delay(20);
  }
}
