/*

  Cerberus - hosttest/test_ntfs.cpp

  Host-side unit test for the read-only NTFS driver (../fs_ntfs.h), run against
  work/ntfs.img built by gen_ntfs.sh. Root-level only (ntfscp cannot create
  subdirs), which still exercises the MFT, directory index, USA fixup and both
  resident and non-resident $DATA (runlists). Not part of the Arduino build.

    g++ -std=c++11 -Wall -Wextra -o test_ntfs test_ntfs.cpp
    ./test_ntfs work/ntfs.img

*/

#include "harness.h"
#include "../fs_ntfs.h"

static const uint32_t BIG = 2u * 1024 * 1024;

static void check_pattern(NtfsReader& fs, const char* path, uint64_t off,
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
  if (argc < 2) { printf("usage: %s <ntfs.img>\n", argv[0]); return 2; }
  FileBlockDev dev;
  if (!dev.open(argv[1])) return 2;

  NtfsReader fs;
  CHECK(fs.begin(dev, 0, dev.sectorCount()));
  CHECK(strcmp(fs.fsName(), "NTFS") == 0);

  char label[32];
  CHECK(fs.getLabel(label, sizeof(label)));
  printf("label='%s'\n", label);
  CHECK(strcmp(label, "CERBNTFS") == 0);

  DirEntry e[64];
  int n = fs.listDir("/", e, 64);
  dump_listing("/", e, n);
  CHECK(find_entry(e, n, "BIG2MB.BIN") >= 0);
  CHECK(find_entry(e, n, "SMALL.TXT")  >= 0);
  CHECK(find_entry(e, n, "hello.txt")  >= 0);
  CHECK(find_entry(e, n, ".")  < 0);
  CHECK(find_entry(e, n, "..") < 0);

  DirEntry info;
  CHECK(fs.stat("/BIG2MB.BIN", info)); CHECK_EQ_U64(info.size, BIG); CHECK(!info.dir);
  CHECK(fs.stat("/SMALL.TXT", info));  CHECK_EQ_U64(info.size, 5000);
  CHECK(fs.stat("/hello.txt", info));  CHECK_EQ_U64(info.size, 20);

  // Non-resident runlist read (head, interior window, tail) + resident-ish file.
  check_pattern(fs, "/BIG2MB.BIN", 0, 4096, 0);
  check_pattern(fs, "/BIG2MB.BIN", 1000 * 1024 + 123, 8192, 0);
  check_pattern(fs, "/BIG2MB.BIN", BIG - 777, 777, 0);
  check_pattern(fs, "/SMALL.TXT", 0, 5000, 251);

  char hb[32] = {0};
  CHECK(fs.readFile("/hello.txt", 0, hb, 20));
  check_mem("hello.txt", hb, "hello from cerberus\n", 20);

  // Case-insensitive lookup (NTFS is case-insensitive).
  CHECK(fs.stat("/bIg2Mb.BiN", info)); CHECK_EQ_U64(info.size, BIG);

  // A missing path must fail.
  CHECK(fs.listDir("/nope", e, 64) < 0);
  CHECK(!fs.stat("/nope", info));

  return report();
}
