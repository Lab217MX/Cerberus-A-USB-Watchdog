/*

  Cerberus - MultiFs.h

  Shared interface for the read-only, multi-format USB storage browser.

  This header is deliberately free of ANY Arduino / SdFat / TinyUSB dependency so the
  filesystem drivers that implement it (fs_ext.h, fs_ntfs.h, fs_iso.h) can be compiled
  unchanged both:
    - on the RP2040, backed by Adafruit_USBH_MSC_BlockDevice (see usb_explorer.ino), and
    - on a host PC, backed by a plain disk-image file (see hosttest/),
  which is how the drivers get unit-tested against real filesystem images.

  Every driver is READ-ONLY by construction: the BlockDev interface exposed to drivers
  has no write method at all. All data read from the disk is UNTRUSTED (this is a USB
  condom inspecting potentially hostile media): drivers must bounds-check every length
  and offset taken from on-disk structures before using it, and every loop must make
  forward progress or bail out.

  Disk model: 512-byte sectors (the logical block size the rest of Cerberus already
  assumes). A "volume" is a [baseLba, baseLba+numSectors) slice of the disk.

*/

#pragma once

#include <stdint.h>
#include <stddef.h>

// Longest entry name the UI can display; longer names are truncated.
#define MFS_NAME_LEN 48

struct DirEntry {
  char     name[MFS_NAME_LEN];
  bool     dir;
  uint64_t size;   // bytes (display only; >4GB files show truncated in the UI)
};

/*

  Synchronous 512-byte-sector block device. On the device this is driven by
  Adafruit_USBH_MSC_BlockDevice::readSectors() (which pumps the USB host task until the
  transfer completes); on the host by fread() of a disk image.

*/
class BlockDev {
public:
  virtual ~BlockDev() {}
  virtual uint32_t sectorCount() = 0;
  // Read `n` sectors (n*512 bytes) starting at LBA `lba`. Returns false on I/O error
  // or when the range exceeds sectorCount().
  virtual bool readSectors(uint32_t lba, uint8_t* dst, size_t n) = 0;
};

/*

  One mounted volume (any filesystem). begin() probes the volume slice and returns
  false if the filesystem signature is absent or the structures are unusable.

  Paths are always absolute inside the volume, starting with '/', e.g. "/dir/file".
  The volume root is "/". listDir() must skip "." and ".." entries.

*/
class FsReader {
public:
  virtual ~FsReader() {}
  virtual bool begin(BlockDev& dev, uint32_t baseLba, uint32_t numSectors) = 0;
  virtual void end() {}
  virtual const char* fsName() = 0;                       // e.g. "FAT32", "ext4", "NTFS"
  virtual bool getLabel(char* out, int maxLen) = 0;       // best effort; "" if none
  // Fills up to `max` entries of `path` (created on success). Returns entry count,
  // or < 0 on error (not found / not a directory / I/O).
  virtual int listDir(const char* path, DirEntry* entries, int max) = 0;
  // Resolves `path` to a directory or file. Returns false if not found.
  virtual bool stat(const char* path, DirEntry& info) = 0;
  // Reads up to `len` bytes of `path` (a file) at byte offset `offset`.
  // Returns false on any error (not found, is a directory, unsupported encoding...).
  virtual bool readFile(const char* path, uint64_t offset, void* buf, uint32_t len) = 0;
};

// ---------------------------------------------------------------------------
// Little helpers shared by all drivers (host and device use the same LE getters:
// drivers only ever run on the MCU for real, the host path is just for tests).
// ---------------------------------------------------------------------------

static inline uint16_t mfs_le16(const uint8_t* p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static inline uint32_t mfs_le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t mfs_le64(const uint8_t* p) {
  return (uint64_t)mfs_le32(p) | ((uint64_t)mfs_le32(p + 4) << 32);
}
static inline uint32_t mfs_be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline uint16_t mfs_be16(const uint8_t* p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

// Converts a UTF-16 (LE if `bigEndian`==false, BE otherwise) name to printable ASCII.
// Non-ASCII and control characters become '?'; result is NUL-terminated and truncated
// to outMax-1 bytes. Used by NTFS ($FILE_NAME) and Joliet (ISO9660 SVD).
static inline void mfs_utf16_to_ascii(const uint8_t* u16, uint32_t chars, bool bigEndian,
                                      char* out, int outMax) {
  int o = 0;
  for (uint32_t i = 0; i < chars && o < outMax - 1; i++) {
    uint16_t c = bigEndian ? mfs_be16(u16 + 2 * i) : mfs_le16(u16 + 2 * i);
    out[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
  }
  out[o] = '\0';
}

// Extracts the next '/'-separated component of an in-volume path into `comp`
// (truncated to compMax-1). Returns a pointer past the component, or NULL when the
// path is exhausted ("" and "/" both yield no more components). Skips leading '/'.
// So "/a/b" -> "a", then "b", then NULL.
static inline const char* mfs_next_component(const char* path, char* comp, int compMax) {
  while (*path == '/') path++;
  if (*path == '\0') return NULL;
  int o = 0;
  while (*path && *path != '/') {
    if (o < compMax - 1) comp[o++] = *path;
    path++;
  }
  comp[o] = '\0';
  return path;
}
