/*

  Cerberus - fs_iso.h

  Read-only ISO9660 (+ Joliet) filesystem driver implementing FsReader
  (see MultiFs.h). Lets Cerberus inspect USB optical/virtual drives and
  ISO-on-USB images without a PC. All on-disk data is untrusted and every
  length/offset is bounds-checked before use; every loop advances or bails,
  so corrupt or truncated images fail cleanly instead of hanging/crashing.

  Design notes / limitations:

    - NOT reentrant: a single static 4096-byte scratch buffer (_buf) is
      shared by all instances and operations. All FS work in Cerberus runs
      serialized on core1 (see usb_explorer.ino), so this is fine.
    - No Arduino includes, no STL, no malloc/new, no exceptions, no printf:
      the driver is silent and compiles both on the RP2040 and on a host PC.
    - The ISO logical block size is taken from the volume descriptor and
      must be 512/1024/2048/4096 (a multiple of 512); anything else is
      rejected. begin() probes for logical block 16 at 512-sector baseLba+64
      (2048-byte blocks); only a baseLba==0 volume is additionally probed at
      the 512/1024/4096 offsets, and a partitioned volume (baseLba != 0)
      whose probe fails is rejected outright.
    - Volume descriptors are scanned from logical block 16, at most 32,
      stopping at the terminator (type 255) or a bad "CD001" signature.
      A supplementary VD whose escape sequences @88 hold "%/@", "%/C" or
      "%/E" is a Joliet SVD and is preferred over the PVD.
    - Multi-extent files (records with flag 0x80) accumulate up to 8
      (lba,length) pairs. A file needing more than that lists with the size
      accumulated so far but readFile() refuses it.
    - Records with non-zero interleave file-unit or gap size are skipped
      (interleaved media are not supported).
    - "." and ".." are never listed and never matched during path lookup,
      so paths cannot escape the volume root.
    - Name matching is exact first, then ASCII case-insensitive (ISO9660
      primary-tree names are upper-case by spec; Joliet names are matched
      as stored). PVD names have any ";version" suffix stripped (cut at
      the first ';'); Joliet names are UTF-16BE via mfs_utf16_to_ascii()
      and are left untouched.

*/

#pragma once

#include <stdint.h>
#include <string.h>

#include "MultiFs.h"

class IsoReader : public FsReader {
public:
  IsoReader() { _reset(); }

  // Probes [baseLba, baseLba+numSectors) for an ISO9660 volume. Returns
  // false if no usable Primary/Supplementary volume descriptor is found.
  bool begin(BlockDev& dev, uint32_t baseLba, uint32_t numSectors);

  void end() { _reset(); }

  // "Joliet" when a Joliet SVD was chosen, else "ISO9660".
  const char* fsName() { return _joliet ? "Joliet" : "ISO9660"; }

  // Volume id of the chosen descriptor: d-characters (trailing spaces
  // trimmed) for the PVD, UTF-16BE for Joliet. "" if none / not mounted.
  bool getLabel(char* out, int maxLen);

  // Fills up to `max` entries of `path` (returns total count, which may
  // exceed `max`), or < 0 on error (not found / not a directory / I/O).
  int  listDir(const char* path, DirEntry* entries, int max);

  // Resolves `path` to a directory or file. Returns false if not found.
  bool stat(const char* path, DirEntry& info);

  // Reads up to `len` bytes of `path` (a file) at byte offset `offset`,
  // following up to 8 multi-extents. Returns false on any error (not
  // found, is a directory, too many extents, short I/O...).
  bool readFile(const char* path, uint64_t offset, void* buf, uint32_t len);

  // One logical block of scratch, shared by all instances (NOT reentrant).
  static uint8_t _buf[4096];

private:
  struct Extent { uint32_t lba; uint32_t len; };   // logical blocks / bytes

  // A resolved directory record; a multi-extent file chains several
  // records with the same name into `ext` (max 8, then `tooMany`).
  struct File {
    uint8_t   flags;   // flags of the first record (0x02 = directory)
    uint64_t  size;    // total data length accumulated over all extents
    Extent    ext[8];
    int       nExt;
    bool      tooMany; // >8 extents: listing shows size, readFile refuses
  };

  // Sequential directory-record iterator over the concatenated extents.
  struct Walk {
    File     dir;      // directory being walked
    uint32_t next;     // next byte offset in the concatenated dir data
    uint32_t cExt;     // extent holding the cached block
    uint32_t cBlk;     // logical block (within extent) cached in _buf
    bool     live;     // cache valid
  };

  // One raw directory record as parsed from disk.
  struct Rec {
    uint32_t lba;      // extent location (logical blocks)
    uint32_t len;      // data length (bytes)
    uint8_t  flags;    // 0x02 directory, 0x80 multi-extent continuation
  };

  BlockDev* _dev;
  uint32_t  _base;     // volume start (512-byte sectors)
  uint32_t  _num;      // volume length (512-byte sectors)
  uint16_t  _bs;       // ISO logical block size (0 when not mounted)
  uint8_t   _shift;    // _bs / 512
  bool      _joliet;   // chosen VD is a Joliet SVD
  uint32_t  _vdSec;    // 512-sector of the chosen volume descriptor
  char      _label[32];
  File      _root;     // cached root directory record

  void _reset() {
    _dev = NULL; _base = 0; _num = 0; _bs = 0; _shift = 0;
    _joliet = false; _vdSec = 0; _label[0] = '\0';
    memset(&_root, 0, sizeof(_root));
    _root.flags = 0x02;
  }

  // Bounds-checked read of `n` 512-sectors inside the volume slice.
  bool _volRead(uint64_t lba512, uint8_t* dst, uint32_t n) {
    if (!_dev || n == 0) return false;
    if (lba512 < (uint64_t)_base || lba512 + n > (uint64_t)_base + _num)
      return false;
    return _dev->readSectors((uint32_t)lba512, dst, n);
  }

  static bool _validBs(uint16_t bs) {
    return bs >= 512 && bs <= 4096 && (bs & 0x1FF) == 0;
  }

  // True when the SVD escape sequence slots @88 hold a Joliet designator.
  static bool _jolietEscapes(const uint8_t* vd) {
    for (int slot = 0; slot < 3; slot++) {
      const uint8_t* e = vd + 88 + 3 * slot;
      if (e[0] == '%' && e[1] == '/' &&
          (e[2] == '@' || e[2] == 'C' || e[2] == 'E'))
        return true;
    }
    return false;
  }

  // Validates the root directory record @156 of a candidate VD sector.
  static bool _validRoot(const uint8_t* vd) {
    uint8_t rl = vd[156];
    if (rl < 34 || 156 + (uint32_t)rl > 512) return false;
    if (vd[156 + 26] != 0 || vd[156 + 27] != 0) return false;   // interleave
    if (vd[156 + 25] & 0x80) return false;                      // multi-ext root
    return true;
  }

  // ASCII case-insensitive compare ('a'..'z' fold only).
  static bool _striEq(const char* a, const char* b) {
    while (*a && *b) {
      char ca = *a, cb = *b;
      if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
      if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
      if (ca != cb) return false;
      a++; b++;
    }
    return *a == *b;
  }

  // Converts a raw ISO9660 file identifier to the display/compare form.
  // Returns false for "." and ".." (callers skip those records).
  bool _convName(const uint8_t* id, uint8_t idLen, char* out, int outMax) const {
    if (idLen == 1 && (id[0] == 0 || id[0] == 1)) return false;
    if (_joliet) {
      mfs_utf16_to_ascii(id, (uint32_t)idLen / 2, true, out, outMax);
    } else {
      int n = (int)idLen;
      if (n > outMax - 1) n = outMax - 1;
      for (int i = 0; i < n; i++) out[i] = (char)id[i];
      out[n] = '\0';
      // strip a ";version" suffix (everything from the first ';')
      for (int i = 0; out[i]; i++) {
        if (out[i] == ';') { out[i] = '\0'; break; }
      }
    }
    return true;
  }

  // Initializes a directory-record walk over `dir`.
  static void _walkInit(Walk& w, const File& dir) {
    w.dir = dir;
    w.next = 0;
    w.cExt = 0;
    w.cBlk = 0xFFFFFFFFUL;
    w.live = false;
  }

  // Loads logical block `blkIdx` of extent `e` into _buf.
  bool _loadBlock(const Extent& e, uint32_t blkIdx) {
    uint64_t sec = (uint64_t)_base + ((uint64_t)e.lba + blkIdx) * _shift;
    return _volRead(sec, _buf, _shift);
  }

  // Finds the extent of `f` holding global byte offset `g`, fills
  // `local` with the offset inside that extent. Returns false past end.
  static bool _locate(const File& f, uint32_t g, uint32_t& extIdx, uint32_t& local) {
    uint32_t cum = 0;
    for (int i = 0; i < f.nExt; i++) {
      if (g < cum + f.ext[i].len) {
        extIdx = (uint32_t)i;
        local = g - cum;
        return true;
      }
      cum += f.ext[i].len;
    }
    return false;
  }

  // Returns the next usable record of a directory walk. Records that are
  // dot entries, interleaved, or unusable are skipped. Returns false at
  // end of data or on corruption/short I/O (walk is then dead).
  bool _walkNext(Walk& w, Rec& rec, char* name, int nameMax) {
    while (w.next < w.dir.size) {
      uint32_t extIdx, local;
      if (!_locate(w.dir, w.next, extIdx, local)) return false;
      const Extent& e = w.dir.ext[extIdx];
      uint32_t blkIdx = local / _bs;          // logical block within extent
      uint32_t pos = local % _bs;             // byte offset within block
      uint32_t blockBase = local - pos;       // extent-local offset of block
      uint32_t valid = e.len - blockBase;     // dir bytes present in block
      if (valid > _bs) valid = _bs;
      if (pos >= valid) {                     // partial tail: pad to boundary
        w.next = w.next - local + blockBase + _bs;
        continue;
      }
      if (!w.live || w.cExt != extIdx || w.cBlk != blkIdx) {
        if (!_loadBlock(e, blkIdx)) return false;
        w.cExt = extIdx; w.cBlk = blkIdx; w.live = true;
      }
      uint8_t rl = _buf[pos];
      if (rl == 0) {                          // padding to next block boundary
        w.next = w.next - local + blockBase + _bs;
        continue;
      }
      if (rl < 33 || pos + (uint32_t)rl > valid) return false;   // corrupt
      uint8_t nl = _buf[pos + 32];
      if (33 + (uint32_t)nl > rl) return false;                  // corrupt
      if (_buf[pos + 26] != 0 || _buf[pos + 27] != 0) {          // interleave
        w.next += rl;
        continue;
      }
      if (!_convName(_buf + pos + 33, nl, name, nameMax)) {      // "." / ".."
        w.next += rl;
        continue;
      }
      rec.lba = mfs_be32(_buf + pos + 6);
      rec.len = mfs_be32(_buf + pos + 14);
      rec.flags = _buf[pos + 25];
      w.next += rl;
      return true;
    }
    return false;
  }

  // Finds `comp` inside directory `dir` (exact match, then ASCII
  // case-insensitive). On success fills `out` with the accumulated record.
  bool _findInDir(const File& dir, const char* comp, File& out) {
    for (int pass = 0; pass < 2; pass++) {
      Walk w;
      Rec rec;
      char name[MFS_NAME_LEN];
      File acc;
      bool matched = false;
      _walkInit(w, dir);
      while (_walkNext(w, rec, name, sizeof(name))) {
        bool m = (pass == 0) ? (strcmp(name, comp) == 0) : _striEq(name, comp);
        if (!m) continue;
        if (!matched) {
          memset(&acc, 0, sizeof(acc));
          acc.flags = rec.flags;
          matched = true;
        }
        if (acc.nExt < 8) {
          acc.ext[acc.nExt].lba = rec.lba;
          acc.ext[acc.nExt].len = rec.len;
          acc.nExt++;
        } else {
          acc.tooMany = true;
        }
        acc.size += rec.len;
        if (!(rec.flags & 0x80)) { out = acc; return true; }
      }
      if (matched) { out = acc; return true; }   // chain open at end of dir
    }
    return false;
  }

  // Resolves an absolute in-volume path to a File. The root is "/".
  bool _resolve(const char* path, File& out) {
    if (!_bs || !path || path[0] != '/') return false;
    File cur = _root;
    const char* q = mfs_next_component(path, _comp, sizeof(_comp));
    if (!q) { out = cur; return true; }          // "/" -> root
    for (;;) {
      if (!(cur.flags & 0x02)) return false;     // component under a file
      File nxt;
      if (!_findInDir(cur, _comp, nxt)) return false;
      cur = nxt;
      q = mfs_next_component(q, _comp, sizeof(_comp));
      if (!q) { out = cur; return true; }
    }
  }

  // Component buffer reused by path resolution (NOT reentrant, like _buf).
  char _comp[MFS_NAME_LEN];

  // Parses the chosen volume descriptor sector (in _buf) into state.
  bool _applyVD();

  // Fills DirEntry `de` from a resolved file and `name`.
  static void _fillEntry(DirEntry& de, const char* name, const File& f) {
    strncpy(de.name, name, sizeof(de.name) - 1);
    de.name[sizeof(de.name) - 1] = '\0';
    de.dir = (f.flags & 0x02) != 0;
    de.size = f.size;
  }
};

uint8_t IsoReader::_buf[4096];

bool IsoReader::begin(BlockDev& dev, uint32_t baseLba, uint32_t numSectors) {
  _reset();
  _dev = &dev;
  _base = baseLba;
  _num = numSectors;

  // Locate logical block 16 (the first volume descriptor): 512-sector
  // baseLba+64 when the block size is 2048; a whole-disk volume
  // (baseLba == 0) is additionally probed at the 512/1024/4096 offsets.
  const uint32_t cand[4] = { 64, 16, 32, 128 };
  bool found = false;
  for (int i = 0; i < 4; i++) {
    if (i > 0 && baseLba != 0) break;
    uint64_t s = (uint64_t)baseLba + cand[i];
    if (s + 1 > (uint64_t)baseLba + numSectors) continue;
    if (!_volRead(s, _buf, 1)) continue;
    if (_buf[1] != 'C' || _buf[2] != 'D' || _buf[3] != '0' ||
        _buf[4] != '0' || _buf[5] != '1')
      continue;
    uint16_t bs = mfs_be16(_buf + 130);
    if (!_validBs(bs)) continue;
    _bs = bs;
    _shift = (uint8_t)(bs / 512);
    found = true;
    break;
  }
  if (!found) { _reset(); return false; }
  if (baseLba % _shift != 0) { _reset(); return false; }

  // Scan volume descriptors from logical block 16 (max 32, one per block).
  bool havePvd = false, haveJol = false;
  uint32_t pvdSec = 0, jolSec = 0;
  for (uint32_t i = 0; i < 32; i++) {
    uint64_t s = (uint64_t)baseLba + (16 + i) * (uint64_t)_shift;
    if (s + 1 > (uint64_t)baseLba + numSectors) break;
    if (!_volRead(s, _buf, 1)) break;
    if (_buf[1] != 'C' || _buf[2] != 'D' || _buf[3] != '0' ||
        _buf[4] != '0' || _buf[5] != '1')
      break;                                     // unrecognised: stop
    uint8_t t = _buf[0];
    if (t == 255) break;                         // terminator
    if ((t == 1 || t == 2) &&
        mfs_be16(_buf + 130) == _bs && _validRoot(_buf)) {
      if (t == 1 && !havePvd) { havePvd = true; pvdSec = (uint32_t)s; }
      if (t == 2 && !haveJol && _jolietEscapes(_buf)) {
        haveJol = true;
        jolSec = (uint32_t)s;
      }
    }
    if (havePvd && haveJol) break;
  }
  if (haveJol)      { _vdSec = jolSec; _joliet = true; }
  else if (havePvd) { _vdSec = pvdSec; _joliet = false; }
  else              { _reset(); return false; }

  if (!_volRead(_vdSec, _buf, 1)) { _reset(); return false; }
  if (!_applyVD()) { _reset(); return false; }
  return true;
}

bool IsoReader::_applyVD() {
  // Volume id @40: 32 bytes. PVD: d-characters padded with spaces.
  // Joliet: 16 UTF-16BE chars (padded with spaces by xorriso, possibly
  // NULs elsewhere) -> convert, then trim trailing padding.
  const uint8_t* v = _buf + 40;
  char tmp[32];
  if (_joliet) {
    mfs_utf16_to_ascii(v, 16, true, tmp, sizeof(tmp));
    int l = (int)strlen(tmp);
    while (l > 0 && (tmp[l - 1] == ' ' || tmp[l - 1] == '?')) tmp[--l] = '\0';
  } else {
    int l = 0;
    for (int i = 0; i < 32 && l < (int)sizeof(tmp) - 1; i++) {
      if (v[i] == 0) break;
      tmp[l++] = (char)v[i];
    }
    while (l > 0 && tmp[l - 1] == ' ') tmp[--l] = '\0';
  }
  strncpy(_label, tmp, sizeof(_label) - 1);
  _label[sizeof(_label) - 1] = '\0';

  // Root directory record @156.
  const uint8_t* r = _buf + 156;
  memset(&_root, 0, sizeof(_root));
  _root.flags = r[25];
  _root.ext[0].lba = mfs_be32(r + 6);
  _root.ext[0].len = mfs_be32(r + 14);
  _root.nExt = 1;
  _root.size = _root.ext[0].len;
  return (_root.flags & 0x02) != 0;              // root must be a directory
}

bool IsoReader::getLabel(char* out, int maxLen) {
  if (!out || maxLen <= 0) return false;
  if (!_bs) { out[0] = '\0'; return false; }
  strncpy(out, _label, (size_t)maxLen - 1);
  out[maxLen - 1] = '\0';
  return true;
}

int IsoReader::listDir(const char* path, DirEntry* entries, int max) {
  if (!_bs || max < 0 || (max > 0 && !entries)) return -1;
  File dir;
  if (!_resolve(path, dir)) return -1;
  if (!(dir.flags & 0x02)) return -1;

  int n = 0;
  bool pending = false;          // accumulating a multi-extent chain
  DirEntry pend;
  Walk w;
  Rec rec;
  char name[MFS_NAME_LEN];
  _walkInit(w, dir);
  while (_walkNext(w, rec, name, sizeof(name))) {
    if (rec.flags & 0x80) {      // continuation follows: accumulate
      if (!pending) {
        memset(&pend, 0, sizeof(pend));
        _fillEntry(pend, name, dir);   // placeholder size, dir flag below
        pend.dir = (rec.flags & 0x02) != 0;
        pend.size = 0;
        pending = true;
      }
      pend.size += rec.len;
      continue;
    }
    if (pending) {
      pend.size += rec.len;
      if (n < max) entries[n] = pend;
      n++;
      pending = false;
      continue;
    }
    if (n < max) {
      DirEntry de;
      File f;                       // single-extent view of this record
      memset(&f, 0, sizeof(f));
      f.flags = rec.flags;
      f.ext[0].lba = rec.lba;
      f.ext[0].len = rec.len;
      f.nExt = 1;
      f.size = rec.len;
      _fillEntry(de, name, f);
      entries[n] = de;
    }
    n++;
  }
  if (pending) {                 // chain never terminated: report partial
    if (n < max) entries[n] = pend;
    n++;
  }
  return n;
}

bool IsoReader::stat(const char* path, DirEntry& info) {
  if (!_bs || !path) return false;
  File f;
  if (!_resolve(path, f)) return false;
  // display name: last path component (trailing slashes ignored)
  const char* base = path;
  for (const char* p = path; *p; p++) {
    if (*p != '/') base = p + 1;
  }
  char name[MFS_NAME_LEN];
  strncpy(name, base, sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  _fillEntry(info, name, f);
  return true;
}

bool IsoReader::readFile(const char* path, uint64_t offset, void* buf,
                         uint32_t len) {
  if (!_bs || !path) return false;
  File f;
  if (!_resolve(path, f)) return false;
  if ((f.flags & 0x02) || f.tooMany) return false;
  if (len == 0) return true;
  if (!buf || offset >= f.size) return false;
  if (offset + len > f.size) len = (uint32_t)(f.size - offset);

  uint8_t* out = (uint8_t*)buf;
  uint32_t remaining = len;
  uint64_t pos = offset;
  while (remaining > 0) {
    uint32_t extIdx, local;
    if (!_locate(f, (uint32_t)pos, extIdx, local)) return false;
    const Extent& e = f.ext[extIdx];
    uint32_t chunk = e.len - local;
    if (chunk > remaining) chunk = remaining;
    if (chunk > 3584) chunk = 3584;      // fits _buf with any 512 alignment
    uint32_t blkOff = local % _bs;       // offset within the logical block
    uint32_t soff = blkOff & 511;        // offset within the first 512-sector
    uint64_t sec = (uint64_t)_base +
                   ((uint64_t)e.lba + local / _bs) * _shift + (blkOff >> 9);
    uint32_t nsec = (soff + chunk + 511) / 512;
    if (nsec == 0 || nsec * 512 > sizeof(_buf)) return false;
    if (!_volRead(sec, _buf, nsec)) return false;
    memcpy(out, _buf + soff, chunk);
    out += chunk;
    pos += chunk;
    remaining -= chunk;
  }
  return true;
}
