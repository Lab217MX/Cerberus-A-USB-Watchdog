/*

  Cerberus - hosttest/test_iso.cpp

  Host-side unit test for the read-only ISO9660/Joliet driver (../fs_iso.h),
  run against the images built by gen_iso.sh. Not part of the Arduino build.

    g++ -std=c++11 -Wall -Wextra -o test_iso test_iso.cpp
    ./test_iso img/iso_plain.iso  plain
    ./test_iso img/iso_joliet.iso joliet
    ./test_iso img/iso_notiso.img none      # must NOT mount

  Known content (see gen_iso.sh):
    /BIG2MB.BIN        2 MiB, byte i = i & 0xFF
    /SMALL.TXT         5000 bytes, byte i = i % 251
    /SUBDIR/NESTED.TXT "nested file content line 1\nline 2\n"
    plus (Joliet only) "hello world.txt", "LongMixedCaseName.txt".

*/

#include "harness.h"
#include "../fs_iso.h"

static const uint32_t BIG = 2u * 1024 * 1024;

static void check_pattern(IsoReader& fs, const char* path, uint64_t off,
                          uint32_t len, int mod) {
  uint8_t* buf = (uint8_t*)malloc(len);
  CHECK(buf != NULL);
  if (!buf) return;
  bool ok = fs.readFile(path, off, buf, len);
  CHECK(ok);
  if (ok) {
    bool good = true;
    for (uint32_t i = 0; i < len; i++) {
      uint8_t want = mod ? (uint8_t)((off + i) % mod) : (uint8_t)((off + i) & 0xFF);
      if (buf[i] != want) { good = false; break; }
    }
    if (!good) { printf("FAIL: %s content mismatch at off=%llu\n", path,
                        (unsigned long long)off); g_failures++; }
  }
  free(buf);
}

int main(int argc, char** argv) {
  if (argc < 3) { printf("usage: %s <iso> <plain|joliet|none>\n", argv[0]); return 2; }
  const char* mode = argv[2];
  FileBlockDev dev;
  if (!dev.open(argv[1])) return 2;

  IsoReader fs;
  bool mounted = fs.begin(dev, 0, dev.sectorCount());

  if (strcmp(mode, "none") == 0) {          // a non-ISO image must be rejected
    CHECK(!mounted);
    return report();
  }
  CHECK(mounted);
  if (!mounted) return report();

  bool joliet = (strcmp(mode, "joliet") == 0);
  CHECK(strcmp(fs.fsName(), joliet ? "Joliet" : "ISO9660") == 0);

  DirEntry e[64];
  int n = fs.listDir("/", e, 64);
  dump_listing("/", e, n);
  CHECK(find_entry(e, n, "BIG2MB.BIN") >= 0);
  CHECK(find_entry(e, n, "SMALL.TXT")  >= 0);
  int si = find_entry(e, n, "SUBDIR");
  CHECK(si >= 0);
  if (si >= 0) CHECK(e[si].dir);
  CHECK(find_entry(e, n, ".")  < 0);
  CHECK(find_entry(e, n, "..") < 0);
  if (joliet) {
    CHECK(find_entry(e, n, "hello world.txt")       >= 0);
    CHECK(find_entry(e, n, "LongMixedCaseName.txt") >= 0);
  }

  DirEntry info;
  CHECK(fs.stat("/BIG2MB.BIN", info)); CHECK_EQ_U64(info.size, BIG); CHECK(!info.dir);
  CHECK(fs.stat("/SMALL.TXT", info));  CHECK_EQ_U64(info.size, 5000);
  CHECK(fs.stat("/SUBDIR", info));     CHECK(info.dir);

  n = fs.listDir("/SUBDIR", e, 64);
  dump_listing("/SUBDIR", e, n);
  CHECK(find_entry(e, n, "NESTED.TXT") >= 0);
  CHECK(fs.stat("/SUBDIR/NESTED.TXT", info)); CHECK_EQ_U64(info.size, 34);

  check_pattern(fs, "/BIG2MB.BIN", 0, 4096, 0);
  check_pattern(fs, "/BIG2MB.BIN", 1000 * 1024 + 123, 8192, 0);
  check_pattern(fs, "/BIG2MB.BIN", BIG - 777, 777, 0);
  check_pattern(fs, "/SMALL.TXT", 0, 5000, 251);

  if (joliet) {
    char hb[32] = {0};
    CHECK(fs.readFile("/hello world.txt", 0, hb, 20));
    check_mem("hello world.txt", hb, "hello from cerberus\n", 20);
  }

  CHECK(fs.listDir("/nope", e, 64) < 0);
  CHECK(!fs.stat("/nope", info));

  return report();
}
