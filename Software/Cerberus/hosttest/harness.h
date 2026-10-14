/*

  Cerberus - hosttest/harness.h

  Host-side test harness for the read-only filesystem drivers in the parent
  directory (fs_ext.h, fs_ntfs.h, fs_iso.h). Not part of the firmware build:
  the Arduino builder only compiles the sketch root and src/, so this folder
  is ignored by device builds.

  Usage:
    g++ -std=c++11 -Wall -Wextra -o test_ext test_ext.cpp
    ./test_ext img/ext2.img

*/

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../MultiFs.h"

// BlockDev over a raw disk image file (512-byte sectors).
class FileBlockDev : public BlockDev {
public:
  FileBlockDev() : _f(NULL), _sectors(0) {}
  ~FileBlockDev() { if (_f) fclose(_f); }
  bool open(const char* path) {
    _f = fopen(path, "rb");
    if (!_f) { printf("cannot open image '%s'\n", path); return false; }
    fseek(_f, 0, SEEK_END);
    long bytes = ftell(_f);
    if (bytes < 0 || (bytes % 512) != 0) {
      printf("image '%s' has odd size %ld\n", path, bytes);
      fclose(_f); _f = NULL;
      return false;
    }
    _sectors = bytes / 512;
    fseek(_f, 0, SEEK_SET);
    return true;
  }
  uint32_t sectorCount() { return (uint32_t)_sectors; }
  bool readSectors(uint32_t lba, uint8_t* dst, size_t n) {
    if (!_f) return false;
    if ((uint64_t)lba + n > (uint64_t)_sectors) return false;
    if (fseek(_f, (long)lba * 512L, SEEK_SET) != 0) return false;
    return fread(dst, 512, n, _f) == n;
  }
private:
  FILE* _f;
  long  _sectors;
};

static int g_failures = 0;

#define CHECK(cond)                                                           \
  do {                                                                        \
    if (!(cond)) {                                                            \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
      g_failures++;                                                           \
    }                                                                         \
  } while (0)

#define CHECK_EQ_U64(a, b)                                                    \
  do {                                                                        \
    unsigned long long _va = (unsigned long long)(a);                         \
    unsigned long long _vb = (unsigned long long)(b);                         \
    if (_va != _vb) {                                                         \
      printf("FAIL %s:%d: %s (=%llu) != %s (=%llu)\n", __FILE__, __LINE__,    \
             #a, _va, #b, _vb);                                               \
      g_failures++;                                                           \
    }                                                                         \
  } while (0)

static void check_mem(const char* what, const void* got, const void* want, size_t len) {
  if (memcmp(got, want, len) != 0) {
    printf("FAIL: %s content mismatch (first %zu bytes)\n", what, len);
    g_failures++;
  }
}

static void dump_listing(const char* path, DirEntry* e, int n) {
  printf("--- listing %s (%d) ---\n", path, n);
  for (int i = 0; i < n; i++)
    printf("  %-44s %c %llu\n", e[i].name, e[i].dir ? 'D' : 'f',
           (unsigned long long)e[i].size);
  printf("---\n");
}

// Finds an entry by exact name; returns index or -1.
static int find_entry(DirEntry* e, int n, const char* name) {
  for (int i = 0; i < n; i++)
    if (strcmp(e[i].name, name) == 0) return i;
  return -1;
}

static int report() {
  if (g_failures) printf("RESULT: FAILED (%d failures)\n", g_failures);
  else            printf("RESULT: ALL TESTS PASSED\n");
  return g_failures ? 1 : 0;
}
