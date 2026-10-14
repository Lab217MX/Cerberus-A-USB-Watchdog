/*

  Cerberus - hosttest/test_ext.cpp

  Host-side unit test for the read-only ext2/3/4 driver (../fs_ext.h). Runs the
  same driver code that ships in the firmware against a raw ext disk image built
  by gen_ext.sh. Not part of the Arduino build.

    g++ -std=c++11 -Wall -Wextra -o test_ext test_ext.cpp
    ./test_ext work/ext4.img CERBEXT4
    ./test_ext work/ext2.img CERBEXT2

  The images embed known-content files (reproduced below):
    /BIG2MB.BIN     2 MiB, byte i = i & 0xFF   (extents on ext4, indirect on ext2)
    /SMALL.TXT      5000 bytes, byte i = i % 251
    /hello.txt      "hello from cerberus\n"
    /sub/NESTED.TXT "nested line 1\nline 2\n"

*/

#include "harness.h"
#include "../fs_ext.h"

static const uint32_t BIG = 2u * 1024 * 1024;

static void check_pattern(ExtReader& fs, const char* path, uint64_t off,
                          uint32_t len, int mod /*0 => &0xFF, else %mod*/) {
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
  if (argc < 3) { printf("usage: %s <ext.img> <label>\n", argv[0]); return 2; }
  FileBlockDev dev;
  if (!dev.open(argv[1])) return 2;

  ExtReader fs;
  CHECK(fs.begin(dev, 0, dev.sectorCount()));
  printf("mounted %s\n", fs.fsName());

  char label[32];
  CHECK(fs.getLabel(label, sizeof(label)));
  printf("label='%s'\n", label);
  CHECK(strcmp(label, argv[2]) == 0);

  // Root listing: our four entries plus lost+found (created by mke2fs).
  DirEntry e[64];
  int n = fs.listDir("/", e, 64);
  dump_listing("/", e, n);
  CHECK(n >= 4);
  CHECK(find_entry(e, n, "BIG2MB.BIN") >= 0);
  CHECK(find_entry(e, n, "SMALL.TXT")  >= 0);
  CHECK(find_entry(e, n, "hello.txt")  >= 0);
  int si = find_entry(e, n, "sub");
  CHECK(si >= 0);
  if (si >= 0) CHECK(e[si].dir);

  // "." and ".." must never appear.
  CHECK(find_entry(e, n, ".")  < 0);
  CHECK(find_entry(e, n, "..") < 0);

  // Sizes via stat.
  DirEntry info;
  CHECK(fs.stat("/BIG2MB.BIN", info)); CHECK_EQ_U64(info.size, BIG);
  CHECK(!info.dir);
  CHECK(fs.stat("/SMALL.TXT", info));  CHECK_EQ_U64(info.size, 5000);
  CHECK(fs.stat("/hello.txt", info));  CHECK_EQ_U64(info.size, 20);
  CHECK(fs.stat("/sub", info));        CHECK(info.dir);

  // Nested directory.
  n = fs.listDir("/sub", e, 64);
  dump_listing("/sub", e, n);
  CHECK(find_entry(e, n, "NESTED.TXT") >= 0);
  CHECK(fs.stat("/sub/NESTED.TXT", info)); CHECK_EQ_U64(info.size, 21);

  // Content: head, an interior window, and the final bytes of the big file
  // (forces block mapping deep into the extent tree / indirect blocks).
  check_pattern(fs, "/BIG2MB.BIN", 0, 4096, 0);
  check_pattern(fs, "/BIG2MB.BIN", 1000 * 1024 + 123, 8192, 0);
  check_pattern(fs, "/BIG2MB.BIN", BIG - 777, 777, 0);
  check_pattern(fs, "/SMALL.TXT", 0, 5000, 251);

  // hello.txt exact bytes.
  char hb[32] = {0};
  CHECK(fs.readFile("/hello.txt", 0, hb, 20));
  check_mem("hello.txt", hb, "hello from cerberus\n", 20);

  // Reading a directory as a file must fail; a missing path must fail.
  CHECK(!fs.readFile("/sub", 0, hb, 4));
  CHECK(fs.listDir("/nope", e, 64) < 0);
  CHECK(!fs.stat("/nope", info));

  return report();
}
