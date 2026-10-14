/*

  Cerberus - fs_ext.h

  Read-only ext2 / ext3 / ext4 driver for the USB-condom filesystem browser
  (implements FsReader from MultiFs.h). Lets Cerberus browse Linux-formatted
  USB media without a PC. Every on-disk length/offset is untrusted and is
  bounds-checked before use; every loop either advances or bails, so corrupt,
  truncated or maliciously crafted images fail cleanly rather than hang/crash.

  Coverage:
    - superblock probe/parse (magic 0xEF53 at volume byte offset 1024)
    - block sizes 1024/2048/4096 (larger is refused: keeps _blk bounded)
    - 32-bit and 64-bit (INCOMPAT_64BIT) group-descriptor / block addressing
    - both block-mapping forms:
        * classic i_block[] direct + single/double/triple indirect (ext2/3)
        * ext4 extent trees (magic 0xF30A, index nodes descended iteratively,
          depth <= 5); sparse holes and uninitialised extents read as zeros
    - linear directory scan of ext4_dir_entry_2 (works with dir_index/htree:
      the linear leaf blocks are always valid to walk)
    - path resolution (exact byte match), stat, windowed readFile
    - volume label from s_volume_name

  Limitations (by design, read-only forensic browsing):
    - journal is never replayed: on-disk state is shown as-is
    - inline-data inodes (EXT4_INLINE_DATA_FL) list/read as empty
    - encrypted inodes (EXT4_ENCRYPT_FL) refuse readFile
    - COMPRESSION incompat volumes are refused at mount (not used in practice)
    - "." and ".." are never listed and never matched, so paths cannot escape
      the volume root

  NOT REENTRANT: the scratch buffers are static class members shared by all
  instances. All FS work in Cerberus runs serialized on core1 (see
  usb_explorer.ino), so this is fine.

*/

#pragma once

#include <stdint.h>
#include <string.h>

#include "MultiFs.h"

// Cap on directory entries collected per listDir() call (see fs_ntfs.h for the
// same rationale: the Cerberus UI only ever asks for MSC_MAX_ENTRIES == 48).
#define MFS_EXT_ENUM_MAX 64

class ExtReader : public FsReader {
public:
  ExtReader() { _mounted = false; _dev = NULL; }
  ~ExtReader() {}

  bool begin(BlockDev& dev, uint32_t baseLba, uint32_t numSectors);
  void end() { _mounted = false; _dev = NULL; }
  const char* fsName() { return _fsName; }
  bool getLabel(char* out, int maxLen);
  int  listDir(const char* path, DirEntry* entries, int max);
  bool stat(const char* path, DirEntry& info);
  bool readFile(const char* path, uint64_t offset, void* buf, uint32_t len);

private:
  // ---- per-volume state -------------------------------------------------
  BlockDev* _dev;
  uint32_t  _base;            // volume start LBA on the device
  uint32_t  _num;             // volume slice length (sectors)
  uint64_t  _totalSectors;    // usable sectors per superblock (<= _num)
  uint32_t  _bs;              // block size (1024/2048/4096)
  uint32_t  _spb;             // sectors per block (_bs/512)
  uint32_t  _inodesPerGroup;
  uint32_t  _inodeSize;       // 128 / 256 / ...
  uint64_t  _gdTableBlock;    // first block of the group descriptor table
  uint32_t  _descSize;        // group descriptor size (32 or >=64)
  bool      _64bit;           // INCOMPAT_64BIT
  bool      _mounted;
  const char* _fsName;        // "ext2" / "ext3" / "ext4"
  char      _label[16 + 1];

  // ---- shared scratch (NOT reentrant, see banner) -----------------------
  static uint8_t  _blk[4096];                 // one block (or transient sector)
  static uint8_t  _ino[256];                  // one inode's leading fields
  static uint32_t _dirino[MFS_EXT_ENUM_MAX];  // child inode numbers for listDir

  // Bounds-checked read of `n` 512-sectors inside the volume slice.
  bool _read(uint64_t volSec, uint8_t* dst, uint32_t n) {
    if (!_dev || n == 0) return false;
    if (volSec + n > _totalSectors) return false;
    uint64_t lba = (uint64_t)_base + volSec;
    if (lba + n > 0x100000000ull) return false;       // device is 32-bit LBA
    return _dev->readSectors((uint32_t)lba, dst, n);
  }

  // Reads whole filesystem block `blk` into _blk. Returns false out of range.
  bool _readBlock(uint64_t blk) {
    if (blk == 0) { memset(_blk, 0, _bs); return true; }   // hole -> zeros
    uint64_t sec = blk * _spb;
    return _read(sec, _blk, _spb);
  }

  // Loads inode `ino` (1-based) leading bytes into _ino. Uses _blk transiently.
  bool _openInode(uint32_t ino) {
    if (ino == 0 || _inodesPerGroup == 0) return false;
    uint32_t group = (ino - 1) / _inodesPerGroup;
    uint32_t idx   = (ino - 1) % _inodesPerGroup;

    // Group descriptor -> inode table block.
    uint64_t gdByte = (uint64_t)group * _descSize;
    uint64_t gdBlk  = _gdTableBlock + gdByte / _bs;
    uint32_t gdOff  = (uint32_t)(gdByte % _bs);
    if (gdOff + 8 > _bs) return false;
    if (!_readBlock(gdBlk)) return false;
    uint64_t itBlk = mfs_le32(_blk + gdOff + 8);
    if (_64bit && _descSize >= 64) itBlk |= (uint64_t)mfs_le32(_blk + gdOff + 40) << 32;

    // Inode within the table.
    uint64_t iByte = (uint64_t)idx * _inodeSize;
    uint64_t iSec  = itBlk * _spb + iByte / 512;
    uint32_t iOff  = (uint32_t)(iByte % 512);
    uint32_t want  = _inodeSize < sizeof(_ino) ? _inodeSize : (uint32_t)sizeof(_ino);
    uint32_t nsec  = (iOff + want + 511) / 512;
    if (nsec == 0 || nsec > (sizeof(_blk) / 512)) return false;
    if (!_read(iSec, _blk, nsec)) return false;
    memset(_ino, 0, sizeof(_ino));
    memcpy(_ino, _blk + iOff, want);
    return true;
  }

  static bool _isDirMode(uint16_t mode)  { return (mode & 0xF000) == 0x4000; }
  static bool _isRegMode(uint16_t mode)  { return (mode & 0xF000) == 0x8000; }

  // 64-bit size of the inode currently in _ino (regular files use i_size_high).
  uint64_t _inodeSize64() const {
    uint16_t mode = mfs_le16(_ino + 0);
    uint64_t sz = mfs_le32(_ino + 4);
    if (_isRegMode(mode)) sz |= (uint64_t)mfs_le32(_ino + 108) << 32;
    return sz;
  }

  // Maps logical block `lblk` of the inode in _ino to a physical block in
  // `pblk` (0 means a sparse hole -> zeros). Returns false on corruption.
  bool _bmap(uint32_t lblk, uint64_t& pblk) {
    uint32_t flags = mfs_le32(_ino + 32);
    if (flags & 0x10000000u) return false;              // inline data: no blocks
    if (flags & 0x00080000u) return _bmapExtent(lblk, pblk);
    return _bmapIndirect(lblk, pblk);
  }

  // ext4 extent tree. Descends index nodes iteratively (depth <= 5), reusing
  // _blk for each level: the chosen child pointer is copied out before the
  // child block overwrites the buffer.
  bool _bmapExtent(uint32_t lblk, uint64_t& pblk) {
    const uint8_t* node = _ino + 40;      // extent header lives in i_block[]
    uint32_t avail = 60;                  // bytes of the in-inode extent area
    for (int level = 0; level <= 5; level++) {
      if (mfs_le16(node + 0) != 0xF30A) return false;   // eh_magic
      uint16_t ent   = mfs_le16(node + 2);
      uint16_t depth = mfs_le16(node + 6);
      if ((uint32_t)12 + (uint32_t)ent * 12u > avail) return false;
      if (depth == 0) {
        for (uint16_t i = 0; i < ent; i++) {
          const uint8_t* e = node + 12 + i * 12;
          uint32_t eBlk = mfs_le32(e + 0);
          uint16_t eLen = mfs_le16(e + 4);
          if (eLen > 32768) eLen -= 32768;              // uninitialised extent
          if (eLen == 0) continue;
          if (lblk >= eBlk && lblk < (uint64_t)eBlk + eLen) {
            uint64_t start = mfs_le32(e + 8) | ((uint64_t)mfs_le16(e + 6) << 32);
            pblk = start + (lblk - eBlk);
            if (pblk * _spb + _spb > _totalSectors) return false;
            return true;
          }
        }
        pblk = 0;                                       // hole
        return true;
      }
      // Index node: pick the last entry whose ei_block <= lblk.
      uint64_t child = 0;
      bool have = false;
      for (uint16_t i = 0; i < ent; i++) {
        const uint8_t* e = node + 12 + i * 12;
        uint32_t iBlk = mfs_le32(e + 0);
        if (i == 0 || iBlk <= lblk) {
          child = mfs_le32(e + 4) | ((uint64_t)mfs_le16(e + 8) << 32);
          have = true;
          if (iBlk > lblk && i == 0) break;             // even first is past
        } else {
          break;
        }
      }
      if (!have || child == 0) { pblk = 0; return true; }
      if (!_readBlock(child)) return false;
      node = _blk;
      avail = _bs;
    }
    return false;                                       // tree too deep
  }

  // Classic 12 direct + single/double/triple indirect mapping (ext2/3).
  bool _bmapIndirect(uint32_t lblk, uint64_t& pblk) {
    uint32_t ppb = _bs / 4;                             // pointers per block
    if (lblk < 12) { pblk = mfs_le32(_ino + 40 + 4 * lblk); return true; }
    lblk -= 12;
    if (lblk < ppb) return _indirect1(mfs_le32(_ino + 40 + 4 * 12), lblk, pblk);
    lblk -= ppb;
    if (lblk < (uint64_t)ppb * ppb) {
      uint32_t l1 = mfs_le32(_ino + 40 + 4 * 13);
      if (l1 == 0) { pblk = 0; return true; }
      if (!_readBlock(l1)) return false;
      uint32_t next = mfs_le32(_blk + 4 * (lblk / ppb));
      return _indirect1(next, lblk % ppb, pblk);
    }
    lblk -= (uint64_t)ppb * ppb;
    if (lblk < (uint64_t)ppb * ppb * ppb) {
      uint32_t l2 = mfs_le32(_ino + 40 + 4 * 14);
      if (l2 == 0) { pblk = 0; return true; }
      if (!_readBlock(l2)) return false;
      uint32_t mid = mfs_le32(_blk + 4 * (lblk / ((uint64_t)ppb * ppb)));
      if (mid == 0) { pblk = 0; return true; }
      if (!_readBlock(mid)) return false;
      uint32_t next = mfs_le32(_blk + 4 * ((lblk / ppb) % ppb));
      return _indirect1(next, lblk % ppb, pblk);
    }
    return false;                                       // beyond triple indirect
  }

  // Resolves one entry of a single-indirect block `iblk` at slot `slot`.
  bool _indirect1(uint32_t iblk, uint32_t slot, uint64_t& pblk) {
    if (iblk == 0) { pblk = 0; return true; }
    if (!_readBlock(iblk)) return false;
    pblk = mfs_le32(_blk + 4 * slot);
    return true;
  }

  // Resolves an absolute in-volume path to an inode number (0 on failure).
  // On success _ino holds that inode. Root is inode 2.
  uint32_t _resolve(const char* path) {
    if (!path || path[0] != '/') return 0;
    uint32_t ino = 2;
    if (!_openInode(ino)) return 0;
    char comp[MFS_NAME_LEN];
    const char* p = path;
    while ((p = mfs_next_component(p, comp, sizeof(comp))) != NULL) {
      if (comp[0] == '.' && comp[1] == '\0') continue;
      if (!_isDirMode(mfs_le16(_ino + 0))) return 0;    // component under a file
      uint32_t child = _findInDir(comp);
      if (child == 0) return 0;
      ino = child;
      if (!_openInode(ino)) return 0;
    }
    return ino;
  }

  // Scans the directory whose inode is in _ino for `name`; returns its inode
  // number or 0. Preserves _ino across the scan (which reuses _blk).
  uint32_t _findInDir(const char* name) {
    uint64_t size = mfs_le32(_ino + 4);                 // dir size (32-bit)
    uint32_t nlen = (uint32_t)strlen(name);
    if (nlen == 0 || nlen > 255) return 0;
    uint64_t nblocks = (size + _bs - 1) / _bs;
    uint8_t savedIno[256];
    memcpy(savedIno, _ino, sizeof(savedIno));           // _bmap/_readBlock clobber _blk only
    uint32_t found = 0;
    for (uint64_t lb = 0; lb < nblocks && found == 0; lb++) {
      uint64_t pblk;
      memcpy(_ino, savedIno, sizeof(_ino));
      if (!_bmap((uint32_t)lb, pblk)) break;
      if (pblk == 0) continue;
      if (!_readBlock(pblk)) break;
      uint32_t off = 0;
      while (off + 8 <= _bs) {
        uint32_t eIno = mfs_le32(_blk + off);
        uint16_t rlen = mfs_le16(_blk + off + 4);
        uint8_t  nl   = _blk[off + 6];
        if (rlen < 8 || (off + rlen) > _bs) break;      // corrupt: stop block
        if (eIno != 0 && nl == nlen && (uint32_t)8 + nl <= rlen) {
          if (memcmp(_blk + off + 8, name, nl) == 0) { found = eIno; break; }
        }
        off += rlen;                                    // rlen >= 8: progress
      }
    }
    memcpy(_ino, savedIno, sizeof(_ino));
    return found;
  }

  // Fills `de` (except name) from the inode currently in _ino.
  void _fillInfo(DirEntry& de) {
    uint16_t mode = mfs_le16(_ino + 0);
    de.dir  = _isDirMode(mode);
    de.size = de.dir ? mfs_le32(_ino + 4) : _inodeSize64();
  }
};

uint8_t  ExtReader::_blk[4096];
uint8_t  ExtReader::_ino[256];
uint32_t ExtReader::_dirino[MFS_EXT_ENUM_MAX];

inline bool ExtReader::begin(BlockDev& dev, uint32_t baseLba, uint32_t numSectors) {
  _dev = &dev;
  _base = baseLba;
  _num = numSectors;
  _mounted = false;
  _totalSectors = numSectors;                           // provisional for _read
  if (numSectors < 4) return false;

  // Superblock: 1024 bytes at volume byte offset 1024 == sectors [2,3].
  if (!_read(2, _blk, 2)) return false;
  const uint8_t* sb = _blk;
  if (mfs_le16(sb + 56) != 0xEF53) return false;        // s_magic

  uint32_t logbs = mfs_le32(sb + 24);
  if (logbs > 2) return false;                          // bs > 4096 refused
  _bs  = 1024u << logbs;
  _spb = _bs / 512;

  uint32_t incompat = mfs_le32(sb + 96);
  if (incompat & 0x0001u) return false;                 // COMPRESSION: refuse
  _64bit = (incompat & 0x0080u) != 0;

  uint32_t rev = mfs_le32(sb + 76);
  _inodeSize = (rev == 0) ? 128 : mfs_le16(sb + 88);
  if (_inodeSize < 128 || _inodeSize > _bs || (_bs % _inodeSize) != 0) return false;

  _inodesPerGroup = mfs_le32(sb + 40);
  if (_inodesPerGroup == 0) return false;

  _descSize = _64bit ? mfs_le16(sb + 254) : 32;
  if (_descSize < 32) _descSize = 32;
  if (_64bit && _descSize < 64) _descSize = 64;
  if (_descSize > _bs) return false;

  uint32_t firstData = mfs_le32(sb + 20);               // 1 if bs==1024 else 0
  _gdTableBlock = (uint64_t)firstData + 1;

  uint64_t blocks = mfs_le32(sb + 4);
  if (_64bit) blocks |= (uint64_t)mfs_le32(sb + 0x150) << 32;
  uint64_t secs = blocks * _spb;
  _totalSectors = (secs < numSectors) ? secs : numSectors;
  if (_totalSectors < 4) return false;

  // ext2 vs ext3 vs ext4: HAS_JOURNAL (compat 0x4) => ext3+, EXTENTS/64BIT
  // (incompat 0x40/0x80) => ext4. Cosmetic (drives fsName()).
  uint32_t compat = mfs_le32(sb + 92);
  if (incompat & (0x0040u | 0x0080u)) _fsName = "ext4";
  else if (compat & 0x0004u)          _fsName = "ext3";
  else                                _fsName = "ext2";

  // Volume label (s_volume_name @120, 16 bytes, NUL/space padded).
  memcpy(_label, sb + 120, 16);
  _label[16] = '\0';
  for (int i = 15; i >= 0 && (_label[i] == ' ' || _label[i] == '\0'); i--) _label[i] = '\0';
  for (int i = 0; _label[i]; i++)
    if ((uint8_t)_label[i] < 0x20 || (uint8_t)_label[i] >= 0x7F) _label[i] = '?';

  // Root must be a directory.
  if (!_openInode(2) || !_isDirMode(mfs_le16(_ino + 0))) return false;
  _mounted = true;
  return true;
}

inline bool ExtReader::getLabel(char* out, int maxLen) {
  if (!out || maxLen <= 0) return false;
  if (!_mounted) { out[0] = '\0'; return false; }
  strncpy(out, _label, (size_t)maxLen - 1);
  out[maxLen - 1] = '\0';
  return true;
}

inline int ExtReader::listDir(const char* path, DirEntry* entries, int max) {
  if (!_mounted || !entries || max < 0) return -1;
  if (max == 0) return 0;
  if (max > MFS_EXT_ENUM_MAX) max = MFS_EXT_ENUM_MAX;

  uint32_t dino = _resolve(path);
  if (dino == 0) return -1;
  if (!_isDirMode(mfs_le16(_ino + 0))) return -1;

  // Pass A: collect names + child inode numbers (walking dir blocks in _blk).
  uint64_t size = mfs_le32(_ino + 4);
  uint64_t nblocks = (size + _bs - 1) / _bs;
  uint8_t savedIno[256];
  memcpy(savedIno, _ino, sizeof(savedIno));
  int n = 0;
  for (uint64_t lb = 0; lb < nblocks && n < max; lb++) {
    uint64_t pblk;
    memcpy(_ino, savedIno, sizeof(_ino));
    if (!_bmap((uint32_t)lb, pblk)) return -1;
    if (pblk == 0) continue;
    if (!_readBlock(pblk)) return -1;
    uint32_t off = 0;
    while (off + 8 <= _bs && n < max) {
      uint32_t eIno = mfs_le32(_blk + off);
      uint16_t rlen = mfs_le16(_blk + off + 4);
      uint8_t  nl   = _blk[off + 6];
      if (rlen < 8 || (off + rlen) > _bs) break;
      if (eIno != 0 && nl > 0 && (uint32_t)8 + nl <= rlen) {
        bool dot = (nl == 1 && _blk[off + 8] == '.') ||
                   (nl == 2 && _blk[off + 8] == '.' && _blk[off + 9] == '.');
        if (!dot) {
          int cn = nl < MFS_NAME_LEN - 1 ? nl : MFS_NAME_LEN - 1;
          for (int i = 0; i < cn; i++) {
            uint8_t c = _blk[off + 8 + i];
            entries[n].name[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
          }
          entries[n].name[cn] = '\0';
          _dirino[n] = eIno;
          n++;
        }
      }
      off += rlen;
    }
  }

  // Pass B: open each child inode for the authoritative dir flag and size.
  // Children that fail to open (corrupt/stale) are dropped from the listing.
  int out = 0;
  for (int i = 0; i < n; i++) {
    if (!_openInode(_dirino[i])) continue;
    if (out != i) memcpy(entries[out].name, entries[i].name, MFS_NAME_LEN);
    _fillInfo(entries[out]);
    out++;
  }
  return out;
}

inline bool ExtReader::stat(const char* path, DirEntry& info) {
  if (!_mounted || !path) return false;
  uint32_t ino = _resolve(path);
  if (ino == 0) return false;
  info.name[0] = '\0';
  {
    const char* p = path;
    char comp[MFS_NAME_LEN];
    while ((p = mfs_next_component(p, comp, sizeof(comp))) != NULL) {
      strncpy(info.name, comp, sizeof(info.name) - 1);
      info.name[sizeof(info.name) - 1] = '\0';
    }
  }
  _fillInfo(info);
  return true;
}

inline bool ExtReader::readFile(const char* path, uint64_t offset, void* buf,
                                uint32_t len) {
  if (!_mounted || !path) return false;
  if (len && !buf) return false;
  uint32_t ino = _resolve(path);
  if (ino == 0) return false;
  uint16_t mode = mfs_le16(_ino + 0);
  if (_isDirMode(mode)) return false;
  uint32_t flags = mfs_le32(_ino + 32);
  if (flags & 0x00000800u) return false;                // EXT4_ENCRYPT_FL
  if (flags & 0x10000000u) return false;                // inline data

  uint64_t fsize = _inodeSize64();
  if (len == 0 || offset >= fsize) return true;
  if (offset + len > fsize) len = (uint32_t)(fsize - offset);

  uint8_t savedIno[256];
  memcpy(savedIno, _ino, sizeof(savedIno));
  uint8_t* out = (uint8_t*)buf;
  uint64_t pos = offset;
  uint32_t remaining = len;
  while (remaining > 0) {
    uint32_t lblk   = (uint32_t)(pos / _bs);
    uint32_t within = (uint32_t)(pos % _bs);
    uint32_t chunk  = _bs - within;
    if (chunk > remaining) chunk = remaining;
    uint64_t pblk;
    memcpy(_ino, savedIno, sizeof(_ino));
    if (!_bmap(lblk, pblk)) return false;
    if (pblk == 0) {
      memset(out, 0, chunk);                            // sparse hole
    } else {
      if (!_readBlock(pblk)) return false;              // fills _blk[0.._bs)
      memcpy(out, _blk + within, chunk);
    }
    out += chunk;
    pos += chunk;
    remaining -= chunk;
  }
  return true;
}
