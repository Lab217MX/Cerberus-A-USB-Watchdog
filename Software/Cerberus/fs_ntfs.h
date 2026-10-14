/*

  Cerberus - fs_ntfs.h

  Read-only NTFS driver for the USB-condom filesystem browser (implements
  FsReader from MultiFs.h). Lets Cerberus browse potentially hostile USB
  sticks without a PC, so EVERY on-disk length/offset is bounds-checked
  before use and every loop either advances or bails out: corrupt, truncated
  or maliciously crafted images must fail cleanly, never hang or crash.

  Coverage:
    - boot sector probe/parse (OEM "NTFS    ", 0xAA55, 512-byte sectors,
      power-of-two sectors/cluster <= 128)
    - MFT records + INDX records with full USA fixup validation
    - resident and non-resident attributes (named attributes skipped except
      $FILE_NAME; compressed/sparse/encrypted non-resident data refused)
    - runlist decoding (signed offsets, sparse LCN -1 runs zero-filled)
    - directories in both forms: resident $INDEX_ROOT and non-resident
      $INDEX_ALLOCATION INDX blocks (linear VCN walk; when both exist only
      the INDX blocks are enumerated, so root separator keys are skipped)
    - path resolution (case-insensitive, exact-case preferred), stat,
      readFile windowed reads, volume label ($VOLUME_NAME)

  Limitations (by design, read-only forensic browsing):
    - compressed / sparse-with-flags / encrypted attributes are refused
      (compression_unit != 0 or attr flags & 0xC000)
    - $ATTRIBUTE_LIST continuation attributes are not followed (files whose
      attributes span multiple MFT records are listed by their in-record
      data only)
    - hard-link parent resolution via ".." relies on the ".." index entry
      being present (real NTFS directories have it)

  NOT REENTRANT: the scratch buffers are static class members shared by all
  instances. One NtfsReader may be used at a time; nested use (e.g. calling
  readFile from inside a listDir-driven callback) would corrupt the shared
  record buffer. The device UI is single-threaded, so this is fine.

*/

#pragma once

#include <string.h>

#include "MultiFs.h"

// Cap on directory entries collected per listDir() call (caller may pass a
// bigger `max`; we fill at most this many). Sized so the packed ref/namespace
// key array below stays small on the RP2040 (RAM is tight; the Cerberus UI
// only ever requests MSC_MAX_ENTRIES == 48 entries). Raise for host tests that
// need to enumerate larger directories.
#define MFS_NTFS_ENUM_MAX 64

class NtfsReader : public FsReader {
public:
  NtfsReader() : _dev(NULL), _mounted(false) {}
  ~NtfsReader() {}

  bool begin(BlockDev& dev, uint32_t baseLba, uint32_t numSectors);
  void end() { _mounted = false; _dev = NULL; }
  const char* fsName() { return "NTFS"; }
  bool getLabel(char* out, int maxLen);
  int  listDir(const char* path, DirEntry* entries, int max);
  bool stat(const char* path, DirEntry& info);
  bool readFile(const char* path, uint64_t offset, void* buf, uint32_t len);

private:
  // ---- per-volume state -------------------------------------------------
  BlockDev* _dev;
  uint32_t  _base;          // volume start LBA on the underlying device
  uint32_t  _numSectors;    // volume slice length (sectors)
  uint64_t  _totalSectors;  // volume size in sectors per boot sector (<= _numSectors)
  uint64_t  _mftLcn;        // $MFT cluster
  uint8_t   _spc;           // sectors per cluster (power of two, <= 128)
  uint32_t  _clusterSize;   // _spc * 512
  uint32_t  _recordSize;    // MFT record bytes (512..4096, power of two)
  uint32_t  _indexSize;     // INDX record bytes (512..4096, power of two)
  bool      _mounted;

  // $MFT's own $DATA runlist, parsed once at begin(). $MFT is frequently
  // fragmented on real media, so record N may live anywhere; without this,
  // _openRec would read records beyond the first fragment from the wrong place.
  // Empty (_nMftRuns == 0) means "fall back to contiguous from _mftLcn".
  struct MftRun { uint64_t vcn, lcn, len; };   // in clusters
  MftRun    _mftRuns[16];
  int       _nMftRuns;

  // ---- shared scratch (NOT reentrant, see banner) ------------------------
  static uint8_t _buf[512];                          // single-sector transfer
  static uint8_t _rec[4096];                         // MFT / INDX record buffer
  static uint64_t _key[MFS_NTFS_ENUM_MAX];           // packed ref|namespace<<48

  // Directory index entry visitor. `nm`/`nl` are the UTF-16LE name inside the
  // entry; `ref` is the 48-bit child file reference.
  typedef void (*NtfsWalkCb)(void* ctx, uint64_t ref, uint8_t ns,
                             const uint8_t* nm, uint8_t nl);

  bool    _read(uint64_t volSec, uint8_t* dst, uint32_t n);
  bool    _loadMftRuns();                            // parse $MFT $DATA runlist
  bool    _mftByteToSector(uint64_t byteOff, uint64_t& sector);
  bool    _openRec(uint64_t recNo);                  // FILE record -> _rec
  bool    _fixup(uint8_t* buf, uint32_t size);       // USA fixup + validate
  uint32_t _findAttr(uint32_t type, int wantResident);// 0 = not found
  uint32_t _findIndexAlloc();                        // non-resident $I30 0xA0
  uint32_t _findFileNameAttr();                      // best-namespace $FILE_NAME
  bool    _nrRefused(uint32_t off);                  // compressed/encrypted/sparse
  bool    _runSeek(uint32_t attrOff, uint64_t vcn,
                   uint64_t& lcn, uint64_t& v0, uint64_t& len);
  bool    _walkDir(uint64_t dirRef, void* ctx, NtfsWalkCb cb);
  static void _walkNode(const uint8_t* buf, uint32_t es, uint32_t ee,
                        void* ctx, NtfsWalkCb cb);
  bool    _resolve(const char* path, uint64_t& ref);
  bool    _copyBytes(uint64_t volByteOff, uint8_t* dst, uint64_t n);
  void    _fillInfo(DirEntry& info);                 // from FILE record in _rec
  static int _nsRank(uint8_t ns);
  static char _lower(char c);

  static void _searchCb(void* ctx, uint64_t ref, uint8_t ns,
                        const uint8_t* nm, uint8_t nl);
  static void _collectCb(void* ctx, uint64_t ref, uint8_t ns,
                         const uint8_t* nm, uint8_t nl);
};

// ---------------------------------------------------------------------------
// Little helpers
// ---------------------------------------------------------------------------

// Out-of-class definitions of the shared scratch buffers (see the NOT-REENTRANT
// banner above). Placed here in the header (like IsoReader::_buf in fs_iso.h)
// because this header is #included exactly once, from usb_explorer.ino.
uint8_t  NtfsReader::_buf[512];
uint8_t  NtfsReader::_rec[4096];
uint64_t NtfsReader::_key[MFS_NTFS_ENUM_MAX];

inline char NtfsReader::_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

// Display preference across duplicate $FILE_NAME namespaces: Win32 long (1),
// then Win32+DOS (3), then DOS (2) / POSIX (0) as first-available.
inline int NtfsReader::_nsRank(uint8_t ns) {
  if (ns == 1) return 3;
  if (ns == 3) return 2;
  return 1;
}

inline bool NtfsReader::_read(uint64_t volSec, uint8_t* dst, uint32_t n) {
  if (!_dev || n == 0) return false;
  if (volSec + n > _totalSectors) return false;          // outside volume
  uint64_t lba = (uint64_t)_base + volSec;
  if (lba > 0xFFFFFFFFull) return false;                 // device address space
  return _dev->readSectors((uint32_t)lba, dst, n);
}

// Applies the Update Sequence Array fixup in place. The buffer holds one
// record (MFT or INDX) of `size` bytes, split into size/512 sectors whose
// trailing u16 must carry the check value; they are replaced with the per-
// sector values from the USA. Any mismatch means a corrupt record.
inline bool NtfsReader::_fixup(uint8_t* buf, uint32_t size) {
  uint16_t uo = mfs_le16(buf + 4);
  uint16_t uc = mfs_le16(buf + 6);
  if (uc < 2) return false;
  if ((uint32_t)(uc - 1) * 512u != size) return false;   // sector count mismatch
  if ((uint32_t)uo + 2u * uc > size) return false;       // USA outside record
  uint16_t chk = mfs_le16(buf + uo);
  for (uint16_t i = 1; i < uc; i++) {
    uint8_t* tail = buf + (uint32_t)i * 512u - 2;
    if (mfs_le16(tail) != chk) return false;             // torn/corrupt record
    uint16_t rep = mfs_le16(buf + uo + 2u * i);
    tail[0] = (uint8_t)rep;
    tail[1] = (uint8_t)(rep >> 8);
  }
  return true;
}

// Parses $MFT's own $DATA runlist (MFT record 0, which always begins at
// _mftLcn) into _mftRuns, so _openRec can locate any record even when $MFT is
// fragmented. Best-effort: on any problem it leaves _nMftRuns == 0 and callers
// fall back to the contiguous assumption.
inline bool NtfsReader::_loadMftRuns() {
  _nMftRuns = 0;
  uint32_t rs = _recordSize >> 9;
  uint64_t sec = _mftLcn * (uint64_t)_spc;               // record 0 starts here
  if (!_read(sec, _rec, rs)) return false;
  if (mfs_le32(_rec) != 0x454C4946u) return false;       // "FILE"
  if (!_fixup(_rec, _recordSize)) return false;
  uint32_t a = _findAttr(0x80, 1);                       // non-resident $DATA
  if (!a) return false;

  uint32_t al  = mfs_le32(_rec + a + 4);
  uint32_t pos = mfs_le16(_rec + a + 32);
  uint64_t curVcn = mfs_le64(_rec + a + 16);             // low_vcn (0 for $MFT)
  int64_t  curLcn = 0;
  uint64_t clusters = _totalSectors / _spc;
  while (pos < al && _nMftRuns < 16) {
    uint8_t h = _rec[a + pos]; pos++;
    if (h == 0) break;                                   // runlist terminator
    uint8_t lb = h & 0x0F, ob = h >> 4;
    if (lb < 1 || lb > 8 || ob < 1 || ob > 8) return false;   // $MFT is never sparse
    if (pos + 1u * lb + ob > al) return false;
    uint64_t rlen = 0;
    for (uint8_t i = 0; i < lb; i++) rlen |= (uint64_t)_rec[a + pos + i] << (8 * i);
    pos += lb;
    int64_t delta = 0;
    for (uint8_t i = 0; i < ob; i++) {
      uint8_t b = _rec[a + pos + i];
      if (i == ob - 1 && (b & 0x80)) delta |= -((int64_t)1 << (8 * ob - 1));
      delta |= (int64_t)b << (8 * i);
    }
    pos += ob;
    if (rlen == 0) return false;
    curLcn += delta;
    if (curLcn < 0 || (uint64_t)curLcn + rlen > clusters) return false;
    _mftRuns[_nMftRuns].vcn = curVcn;
    _mftRuns[_nMftRuns].lcn = (uint64_t)curLcn;
    _mftRuns[_nMftRuns].len = rlen;
    _nMftRuns++;
    curVcn += rlen;
  }
  return _nMftRuns > 0;
}

// Maps a byte offset within $MFT's $DATA to a device volume sector via the
// runlist parsed at begin(). Falls back to the contiguous-from-_mftLcn layout
// when the runlist is unavailable (_nMftRuns == 0).
inline bool NtfsReader::_mftByteToSector(uint64_t byteOff, uint64_t& sector) {
  if (_nMftRuns <= 0) { sector = _mftLcn * (uint64_t)_spc + byteOff / 512; return true; }
  uint64_t vcn = byteOff / _clusterSize;
  uint32_t within = (uint32_t)(byteOff % _clusterSize);
  for (int i = 0; i < _nMftRuns; i++) {
    if (vcn >= _mftRuns[i].vcn && vcn < _mftRuns[i].vcn + _mftRuns[i].len) {
      uint64_t lcn = _mftRuns[i].lcn + (vcn - _mftRuns[i].vcn);
      sector = lcn * (uint64_t)_spc + within / 512;
      return true;
    }
  }
  return false;                                          // vcn past end of $MFT
}

inline bool NtfsReader::_openRec(uint64_t recNo) {
  uint32_t rs = _recordSize >> 9;                        // sectors per record
  uint64_t base = recNo * (uint64_t)_recordSize;
  for (uint32_t i = 0; i < rs; i++) {                    // map each 512 sector
    uint64_t sec;
    if (!_mftByteToSector(base + (uint64_t)i * 512, sec)) return false;
    if (!_read(sec, _rec + i * 512, 1)) return false;
  }
  if (mfs_le32(_rec) != 0x454C4946u) return false;       // "FILE"
  return _fixup(_rec, _recordSize);
}

// Finds an unnamed attribute of `type` in the FILE record currently in _rec.
// wantResident: 0 = resident only, 1 = non-resident only, -1 = either.
// Per-attribute bounds are validated here; the returned offset (> 0) is safe
// to read header fields from, and resident values are guaranteed to lie
// inside the attribute. Returns 0 when absent or unusable.
inline uint32_t NtfsReader::_findAttr(uint32_t type, int wantResident) {
  uint32_t used = mfs_le32(_rec + 24);
  if (used > _recordSize) used = _recordSize;            // lenient clamp
  uint32_t off = mfs_le16(_rec + 20);
  while (off + 16 <= used) {
    uint32_t at = mfs_le32(_rec + off);
    if (at == 0 || at == 0xFFFFFFFFu) break;             // END
    uint32_t al = mfs_le32(_rec + off + 4);
    if (al < 16 || (uint64_t)off + al > used) break;     // corrupt attribute list
    if (at == type && _rec[off + 9] == 0) {              // unnamed only
      bool nr = _rec[off + 8] != 0;
      if (wantResident < 0 || (wantResident == 1) == nr) {
        if (!nr) {
          uint32_t vl = mfs_le32(_rec + off + 16);
          uint32_t vo = mfs_le16(_rec + off + 20);
          if ((uint64_t)vo + vl <= al) return off;
        } else {
          uint32_t ro = mfs_le16(_rec + off + 32);
          if (ro < al) return off;
        }
      }
    }
    off += al;                                           // al >= 16: progress
  }
  return 0;
}

// Locates the non-resident $INDEX_ALLOCATION (type 0xA0) for a directory.
// Directory index attributes are named "$I30", so (unlike the $DATA finder)
// this does NOT require an empty name. Returns the attribute offset or 0.
inline uint32_t NtfsReader::_findIndexAlloc() {
  uint32_t used = mfs_le32(_rec + 24);
  if (used > _recordSize) used = _recordSize;
  uint32_t off = mfs_le16(_rec + 20);
  while (off + 16 <= used) {
    uint32_t at = mfs_le32(_rec + off);
    if (at == 0 || at == 0xFFFFFFFFu) break;
    uint32_t al = mfs_le32(_rec + off + 4);
    if (al < 16 || (uint64_t)off + al > used) break;
    if (at == 0xA0 && _rec[off + 8] != 0) {              // non-resident
      if (mfs_le16(_rec + off + 32) < al) return off;
    }
    off += al;
  }
  return 0;
}

// Best-namespace $FILE_NAME attribute (named instances allowed by spec).
// The resident value is guaranteed >= 66 bytes (real_size@48 readable).
inline uint32_t NtfsReader::_findFileNameAttr() {
  uint32_t used = mfs_le32(_rec + 24);
  if (used > _recordSize) used = _recordSize;
  uint32_t off = mfs_le16(_rec + 20);
  uint32_t best = 0;
  int bestRank = -1;
  while (off + 16 <= used) {
    uint32_t at = mfs_le32(_rec + off);
    if (at == 0 || at == 0xFFFFFFFFu) break;
    uint32_t al = mfs_le32(_rec + off + 4);
    if (al < 16 || (uint64_t)off + al > used) break;
    if (at == 0x30 && _rec[off + 8] == 0) {              // resident
      uint32_t vl = mfs_le32(_rec + off + 16);
      uint32_t vo = mfs_le16(_rec + off + 20);
      if ((uint64_t)vo + vl <= al && vl >= 66) {
        int rank = _nsRank(_rec[off + vo + 65]);
        if (rank > bestRank) { bestRank = rank; best = off; }
      }
    }
    off += al;
  }
  return best;
}

// Non-resident attributes we cannot read: compressed (compression_unit != 0)
// or flagged sparse/encrypted (attr flags & 0xC000).
inline bool NtfsReader::_nrRefused(uint32_t off) {
  return mfs_le16(_rec + off + 34) != 0 || (mfs_le16(_rec + off + 12) & 0xC000) != 0;
}

// Locates the run covering `vcn` in the non-resident attribute at _rec+attrOff.
// The runlist is re-parsed from the record on every call (no caching: the
// record buffer is clobbered by INDX block reads, and it keeps the driver
// stateless against hostile runlists). Every run must satisfy vcn monotonic
// growth and vcn+len-1 <= high_vcn+1, and its LCN must stay inside the
// volume; anything else fails cleanly.
inline bool NtfsReader::_runSeek(uint32_t attrOff, uint64_t vcn,
                                 uint64_t& lcn, uint64_t& v0, uint64_t& len) {
  uint32_t al = mfs_le32(_rec + attrOff + 4);
  uint32_t pos = mfs_le16(_rec + attrOff + 32);
  uint64_t curVcn = mfs_le64(_rec + attrOff + 16);       // low_vcn
  uint64_t hi = mfs_le64(_rec + attrOff + 24);           // high_vcn
  int64_t curLcn = 0;
  while (pos < al) {
    uint8_t h = _rec[attrOff + pos];
    pos++;
    if (h == 0) return false;                            // terminator: not found
    uint8_t lb = h & 0x0F;
    uint8_t ob = h >> 4;
    if (lb < 1 || lb > 8 || ob > 8) return false;
    if (pos + 1u + lb + ob > al) return false;           // runlist outside attr
    uint64_t rlen = 0;
    for (uint8_t i = 0; i < lb; i++)
      rlen |= (uint64_t)_rec[attrOff + pos + i] << (8 * i);
    pos += lb;
    int64_t delta = 0;
    for (uint8_t i = 0; i < ob; i++) {
      uint8_t b = _rec[attrOff + pos + i];
      if (i == ob - 1 && (b & 0x80))                     // sign-extend from MSB
        delta |= -((int64_t)1 << (8 * ob - 1));
      delta |= (int64_t)b << (8 * i);
    }
    pos += ob;
    if (rlen == 0) return false;
    curLcn += delta;
    if (curLcn < -1) return false;                       // invalid LCN
    if (curLcn > 0 && (uint64_t)curLcn > _totalSectors) return false;
    if (rlen > _totalSectors) return false;              // run cannot fit volume
    if (curVcn + rlen - 1 > hi + 1) return false;
    if (vcn >= curVcn && vcn < curVcn + rlen) {
      lcn = (uint64_t)curLcn;
      v0 = curVcn;
      len = rlen;
      return true;
    }
    curVcn += rlen;                                      // strictly grows
  }
  return false;
}

// Walks the raw index entries of one node buffer. [es, ee) is the entries
// byte range (absolute offsets into buf). Stops at the END entry, on a
// corrupt entry, or when the used region is exhausted. Branch entries
// (flags & 0x01) are reported like normal entries.
inline void NtfsReader::_walkNode(const uint8_t* buf, uint32_t es, uint32_t ee,
                                  void* ctx, NtfsWalkCb cb) {
  uint32_t e = es;
  while (e + 82 <= ee) {                                 // fixed part: 16+66
    uint16_t elen = mfs_le16(buf + e + 8);
    if (elen < 82 || (uint32_t)e + elen > ee) return;    // corrupt node
    if (buf[e + 11] & 0x02) return;                      // END entry
    uint64_t ref = mfs_le64(buf + e) & 0x0000FFFFFFFFFFFFull;
    uint8_t nl = buf[e + 16 + 64];
    uint8_t ns = buf[e + 16 + 65];
    uint32_t maxName = ((uint32_t)elen - 82) / 2;        // clamp name to entry
    if ((uint32_t)nl > maxName) nl = (uint8_t)maxName;
    cb(ctx, ref, ns, buf + e + 16 + 66, nl);
    e += elen;                                           // elen >= 82: progress
  }
}

// Enumerates a directory's index entries and reports each to `cb`.
// Handles both directory forms: resident $INDEX_ROOT only, or $INDEX_ALLOCATION
// INDX blocks (VCN walk). When both exist only the INDX blocks are enumerated;
// the root separator keys may duplicate names already present in the leaves
// and are skipped, as spec'd. Returns false on I/O or structural corruption.
inline bool NtfsReader::_walkDir(uint64_t dirRef, void* ctx, NtfsWalkCb cb) {
  if (!_openRec(dirRef)) return false;
  if (!(mfs_le16(_rec + 22) & 0x0002)) return false;     // not a directory

  // Pass A: locate $INDEX_ROOT (resident) and $INDEX_ALLOCATION (non-resident).
  uint32_t rootOff = 0;
  bool hasRoot = false, hasAlloc = false;
  {
    uint32_t used = mfs_le32(_rec + 24);
    if (used > _recordSize) used = _recordSize;
    uint32_t off = mfs_le16(_rec + 20);
    if (off + 16 > used) return false;
    while (off + 16 <= used) {
      uint32_t at = mfs_le32(_rec + off);
      if (at == 0 || at == 0xFFFFFFFFu) break;
      uint32_t al = mfs_le32(_rec + off + 4);
      if (al < 16 || (uint64_t)off + al > used) return false;
      // The directory index attributes ($INDEX_ROOT / $INDEX_ALLOCATION) are
      // always NAMED "$I30", so -- unlike $DATA -- the name is not required to
      // be empty here. A directory has exactly one of each, so first-wins.
      if (at == 0x90 && !hasRoot && _rec[off + 8] == 0) {
        uint32_t vl = mfs_le32(_rec + off + 16);
        uint32_t vo = mfs_le16(_rec + off + 20);
        if ((uint64_t)vo + vl <= al) { hasRoot = true; rootOff = off; }
      } else if (at == 0xA0 && !hasAlloc && _rec[off + 8] != 0) {
        if (mfs_le16(_rec + off + 32) < al) hasAlloc = true;
      }
      off += al;
    }
  }

  if (hasAlloc) {
    uint32_t allocOff = _findIndexAlloc();
    if (!allocOff || _nrRefused(allocOff)) return false;
    uint64_t real = mfs_le64(_rec + allocOff + 48);
    uint64_t blocks = real / _indexSize;
    for (uint64_t v = 0; v < blocks; v++) {
      // The record buffer is clobbered by each INDX read: re-open the
      // directory record and re-locate the attribute for every block.
      if (!_openRec(dirRef)) return false;
      allocOff = _findIndexAlloc();
      if (!allocOff || _nrRefused(allocOff)) return false;
      uint64_t lcn, v0, rl;
      if (!_runSeek(allocOff, v, lcn, v0, rl)) return false;
      if (lcn == 0xFFFFFFFFFFFFFFFFull) return false;    // sparse index block
      uint64_t sec = (lcn + (v - v0)) * (uint64_t)_spc;  // cluster -> sector
      if (sec + (_indexSize >> 9) > _totalSectors) return false;
      if (!_read(sec, _rec, _indexSize >> 9)) return false;
      if (mfs_le32(_rec) != 0x58444E49u) return false;   // "INDX"
      if (!_fixup(_rec, _indexSize)) return false;
      uint32_t es = mfs_le32(_rec + 24);
      uint32_t usz = mfs_le32(_rec + 28);
      if (es > usz) return false;                        // entries outside used
      uint64_t eStart = 24ull + es;
      uint64_t eEnd = 24ull + usz;
      if (eEnd > _indexSize || eStart > eEnd) return false;
      _walkNode(_rec, (uint32_t)eStart, (uint32_t)eEnd, ctx, cb);
    }
    return true;
  }

  if (!hasRoot) return false;

  // Resident $INDEX_ROOT: value = 16-byte root header + node header + entries.
  {
    uint32_t vl = mfs_le32(_rec + rootOff + 16);
    uint32_t vo = mfs_le16(_rec + rootOff + 20);
    const uint8_t* rv = _rec + rootOff + vo;
    if (mfs_le32(rv) != 0x30u) return false;             // must index $FILE_NAME
    if (vl < 16 + 16) return false;                      // root hdr + node hdr
    uint32_t base = rootOff + vo + 16;                   // node header
    uint32_t es = mfs_le32(rv + 16);
    uint32_t usz = mfs_le32(rv + 20);
    if (usz + 16 > vl) return false;
    if (es > usz) return false;
    uint64_t eStart = (uint64_t)base + es;
    uint64_t eEnd = (uint64_t)base + usz;
    uint64_t vEnd = (uint64_t)rootOff + vo + vl;
    if (eEnd > vEnd || eStart > eEnd) return false;
    _walkNode(_rec, (uint32_t)eStart, (uint32_t)eEnd, ctx, cb);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Path resolution
// ---------------------------------------------------------------------------

struct NtfsSearchCtx {
  const char* comp;
  uint64_t    ref;
  bool        found;
};

inline void NtfsReader::_searchCb(void* ctx_, uint64_t ref, uint8_t ns,
                                  const uint8_t* nm, uint8_t nl) {
  (void)ns;
  NtfsSearchCtx* c = (NtfsSearchCtx*)ctx_;
  if (ref == 0 || c->found) return;
  uint32_t clen = (uint32_t)strlen(c->comp);
  if ((uint32_t)nl != clen) return;
  bool ci = true, exact = true;
  for (uint32_t i = 0; i < clen; i++) {
    uint16_t ch = mfs_le16(nm + 2 * i);
    char b = (ch < 0x80) ? (char)ch : '?';
    if (c->comp[i] != b) exact = false;
    if (_lower(c->comp[i]) != _lower(b)) { ci = false; break; }
  }
  if (!ci) return;
  c->ref = ref;
  c->found = true;                                       // first ci match wins;
}                                                         // exact match is first

inline bool NtfsReader::_resolve(const char* path, uint64_t& ref) {
  ref = 5;                                               // root directory record
  const char* p = path;
  char comp[MFS_NAME_LEN];
  while ((p = mfs_next_component(p, comp, sizeof(comp))) != NULL) {
    if (comp[0] == '.' && comp[1] == '\0') continue;     // "." -> same dir
    NtfsSearchCtx ctx;
    ctx.comp = comp;
    ctx.ref = 0;
    ctx.found = false;
    if (!_walkDir(ref, &ctx, _searchCb) || !ctx.found) return false;
    ref = ctx.ref;
  }
  return true;
}

// ---------------------------------------------------------------------------
// FsReader interface
// ---------------------------------------------------------------------------

inline bool NtfsReader::begin(BlockDev& dev, uint32_t baseLba, uint32_t numSectors) {
  _dev = &dev;
  _base = baseLba;
  _numSectors = numSectors;
  _mounted = false;
  _totalSectors = 0;

  if (numSectors == 0) return false;
  if (!_dev->readSectors(_base, _buf, 1)) return false;  // boot sector

  if (memcmp(_buf + 3, "NTFS    ", 8) != 0) return false;
  if (mfs_le16(_buf + 510) != 0xAA55) return false;
  if (mfs_le16(_buf + 0x0B) != 512) return false;        // BlockDev is 512-based

  uint8_t spc = _buf[0x0D];
  if (spc == 0 || (spc & (spc - 1)) != 0 || spc > 128) return false;

  uint64_t total = mfs_le64(_buf + 0x28);
  if (total == 0 || total > numSectors) return false;

  uint64_t mftLcn = mfs_le64(_buf + 0x30);
  if (mftLcn >= total) return false;
  if (mftLcn * spc >= total) return false;

  // MFT record / index record size: i8, negative n -> 1 << -n bytes,
  // positive -> value * spc * 512. Must be 512..4096 power of two so the
  // shared record buffer can hold them.
  uint32_t rec = 0, idx = 0;
  {
    int8_t v = (int8_t)_buf[0x40];
    if (v < 0) {
      uint8_t sh = (uint8_t)(-(v + 1)) + 1;              // shift = -v, -128 safe
      if (sh <= 12) rec = 1u << sh;
    } else if (v > 0) {
      rec = (uint32_t)v * spc * 512u;
    }
    v = (int8_t)_buf[0x44];
    if (v < 0) {
      uint8_t sh = (uint8_t)(-(v + 1)) + 1;
      if (sh <= 12) idx = 1u << sh;
    } else if (v > 0) {
      idx = (uint32_t)v * spc * 512u;
    }
  }
  if (rec < 512 || rec > 4096 || (rec & (rec - 1)) != 0) return false;
  if (idx < 512 || idx > 4096 || (idx & (idx - 1)) != 0) return false;

  _totalSectors = total;
  _mftLcn = mftLcn;
  _spc = spc;
  _clusterSize = (uint32_t)spc * 512u;
  _recordSize = rec;
  _indexSize = idx;

  // Parse $MFT's runlist so fragmented $MFTs are handled. Best-effort: on
  // failure _nMftRuns stays 0 and _openRec falls back to a contiguous $MFT.
  _loadMftRuns();

  _mounted = true;
  return true;
}

inline bool NtfsReader::getLabel(char* out, int maxLen) {
  if (out && maxLen > 0) out[0] = '\0';
  if (!_mounted || !out || maxLen <= 0) return false;
  if (!_openRec(3)) return false;                        // $Volume
  uint32_t a = _findAttr(0x60, 0);                       // $VOLUME_NAME, resident
  if (!a) return false;
  uint32_t vl = mfs_le32(_rec + a + 16);
  uint32_t vo = mfs_le16(_rec + a + 20);
  mfs_utf16_to_ascii(_rec + a + vo, vl / 2, false, out, maxLen);
  return true;
}

struct NtfsCollectCtx {
  DirEntry* entries;
  uint64_t* keys;
  int       max;
  int       n;
};

inline void NtfsReader::_collectCb(void* ctx_, uint64_t ref, uint8_t ns,
                                   const uint8_t* nm, uint8_t nl) {
  NtfsCollectCtx* c = (NtfsCollectCtx*)ctx_;
  if (ref == 0) return;
  char ascii[MFS_NAME_LEN];
  mfs_utf16_to_ascii(nm, nl, false, ascii, sizeof(ascii));
  if (ascii[0] == '.' && (ascii[1] == '\0' || (ascii[1] == '.' && ascii[2] == '\0')))
    return;                                              // skip "." and ".."
  // A file with long+short names appears once per namespace: keep the best
  // (Win32 long, then Win32+DOS, then DOS/POSIX first-available).
  for (int i = 0; i < c->n; i++) {
    if ((c->keys[i] & 0x0000FFFFFFFFFFFFull) == ref) {
      if (_nsRank(ns) > _nsRank((uint8_t)(c->keys[i] >> 48))) {
        c->keys[i] = ref | ((uint64_t)ns << 48);
        mfs_utf16_to_ascii(nm, nl, false, c->entries[i].name, MFS_NAME_LEN);
      }
      return;
    }
  }
  if (c->n >= c->max) return;                            // caller's array full
  c->keys[c->n] = ref | ((uint64_t)ns << 48);
  mfs_utf16_to_ascii(nm, nl, false, c->entries[c->n].name, MFS_NAME_LEN);
  c->n++;
}

inline int NtfsReader::listDir(const char* path, DirEntry* entries, int max) {
  if (!_mounted) return -1;
  if (!entries || max < 0) return -1;
  if (max == 0) return 0;

  uint64_t dref;
  if (!_resolve(path, dref)) return -1;
  if (!_openRec(dref)) return -1;
  if (!(mfs_le16(_rec + 22) & 0x0002)) return -1;        // not a directory

  NtfsCollectCtx ctx;
  ctx.entries = entries;
  ctx.keys = _key;
  ctx.max = max > MFS_NTFS_ENUM_MAX ? MFS_NTFS_ENUM_MAX : max;
  ctx.n = 0;
  if (!_walkDir(dref, &ctx, _collectCb)) return -1;

  // Second pass: open each child record for the authoritative directory flag
  // and size. Children that fail to open (corrupt/stale references) are
  // dropped from the listing.
  int n = ctx.n;
  for (int i = 0; i < n; i++) {
    uint64_t ref = ctx.keys[i] & 0x0000FFFFFFFFFFFFull;
    if (!_openRec(ref)) {
      memmove(&entries[i], &entries[i + 1], (size_t)(n - i - 1) * sizeof(DirEntry));
      memmove(&ctx.keys[i], &ctx.keys[i + 1], (size_t)(n - i - 1) * sizeof(uint64_t));
      n--;
      i--;
      continue;
    }
    _fillInfo(entries[i]);
  }
  return n;
}

// Fills `info` from the FILE record currently in _rec (name untouched).
inline void NtfsReader::_fillInfo(DirEntry& info) {
  info.dir = (mfs_le16(_rec + 22) & 0x0002) != 0;
  uint64_t sz = 0;
  uint32_t a = _findAttr(0x80, -1);                      // unnamed $DATA
  bool have = false, bad = false;
  if (a) {
    if (_rec[a + 8] == 0) {
      sz = mfs_le32(_rec + a + 16);                      // resident value_len
      have = true;
    } else if (!_nrRefused(a)) {
      sz = mfs_le64(_rec + a + 48);                      // real_size
      have = true;
    } else {
      bad = true;                                        // compressed/encrypted
    }
  }
  if (!have || bad) {                                    // fallback
    uint32_t f = _findFileNameAttr();
    if (f) sz = mfs_le64(_rec + f + mfs_le16(_rec + f + 20) + 48);
  }
  info.size = sz;
}

inline bool NtfsReader::stat(const char* path, DirEntry& info) {
  if (!_mounted) return false;
  uint64_t ref;
  if (!_resolve(path, ref)) return false;
  if (!_openRec(ref)) return false;

  // Name = last path component.
  info.name[0] = '\0';
  {
    const char* p = path;
    char comp[MFS_NAME_LEN];
    while ((p = mfs_next_component(p, comp, sizeof(comp))) != NULL)
      memcpy(info.name, comp, sizeof(comp));
  }
  _fillInfo(info);
  return true;
}

inline bool NtfsReader::readFile(const char* path, uint64_t offset, void* buf,
                                 uint32_t len) {
  if (!_mounted) return false;
  if (len && !buf) return false;
  uint64_t ref;
  if (!_resolve(path, ref)) return false;
  if (!_openRec(ref)) return false;
  if (mfs_le16(_rec + 22) & 0x0002) return false;        // directory

  uint32_t a = _findAttr(0x80, -1);                      // unnamed $DATA only
  if (!a) return false;

  if (_rec[a + 8] == 0) {                                // resident
    uint32_t vl = mfs_le32(_rec + a + 16);
    uint32_t vo = mfs_le16(_rec + a + 20);
    if (offset >= vl || len == 0) return true;           // EOF: nothing to copy
    uint32_t n = vl - (uint32_t)offset;
    if (n > len) n = len;
    memcpy(buf, _rec + a + vo + (uint32_t)offset, n);
    return true;
  }

  // Non-resident: windowed run walk.
  if (_nrRefused(a)) return false;                       // compressed/encrypted
  uint64_t real = mfs_le64(_rec + a + 48);
  if (len == 0 || offset >= real) return true;
  uint64_t end = real;
  uint64_t avail = end - offset;
  if (avail > len) end = offset + len;

  uint8_t* d = (uint8_t*)buf;
  uint64_t cs = _clusterSize;
  uint64_t vcn = offset / cs;
  while (vcn * cs < end) {
    uint64_t lcn, v0, rl;
    if (!_runSeek(a, vcn, lcn, v0, rl)) return false;
    uint64_t rb0 = v0 * cs;                              // run byte range
    uint64_t rb1 = rb0 + rl * cs;
    uint64_t s = offset > rb0 ? offset : rb0;
    uint64_t t = end < rb1 ? end : rb1;
    if (s < t) {
      if (lcn == 0xFFFFFFFFFFFFFFFFull) {                // sparse run: zeros
        memset(d + (s - offset), 0, (size_t)(t - s));
      } else {
        uint64_t abs = lcn * cs + (s - rb0);
        if (!_copyBytes(abs, d + (s - offset), t - s)) return false;
      }
    }
    if (rl == 0) return false;                           // paranoia: progress
    vcn = v0 + rl;                                       // hop past this run
  }
  return true;
}

// Copies n bytes from a volume byte offset, one sector at a time through the
// shared sector buffer (works for any alignment/length, bounds-checked).
inline bool NtfsReader::_copyBytes(uint64_t volByteOff, uint8_t* dst, uint64_t n) {
  while (n) {
    uint64_t sec = volByteOff >> 9;
    uint32_t within = (uint32_t)(volByteOff & 511u);
    uint32_t chunk = 512u - within;
    if ((uint64_t)chunk > n) chunk = (uint32_t)n;
    if (!_read(sec, _buf, 1)) return false;
    memcpy(dst, _buf + within, chunk);
    dst += chunk;
    volByteOff += chunk;
    n -= chunk;
  }
  return true;
}
