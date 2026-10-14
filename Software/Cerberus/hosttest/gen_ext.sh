#!/usr/bin/env bash
#
# Cerberus - hosttest/gen_ext.sh
#
# Generates the ext test images used by test_ext.cpp into ./work/.
# Rootless: mke2fs -d populates the image from a staging dir (no mount needed).
#
#   work/ext4.img   ext4, 4K blocks, extents + 64bit  (label CERBEXT4)
#   work/ext2.img   ext2, 1K blocks, indirect mapping  (label CERBEXT2)
#
# Known-content files (reproduced byte-for-byte by test_ext.cpp):
#   /BIG2MB.BIN     2 MiB, byte i = i & 0xFF
#   /SMALL.TXT      5000 bytes, byte i = i % 251
#   /hello.txt      "hello from cerberus\n"
#   /sub/NESTED.TXT "nested line 1\nline 2\n"

set -e
cd "$(dirname "$0")"
mkdir -p work
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/sub"

python3 - "$STAGE" <<'PY'
import os, sys
d = sys.argv[1]
open(os.path.join(d, "BIG2MB.BIN"), "wb").write(bytes((i & 0xFF) for i in range(2 * 1024 * 1024)))
open(os.path.join(d, "SMALL.TXT"), "wb").write(bytes((i % 251) for i in range(5000)))
open(os.path.join(d, "hello.txt"), "wb").write(b"hello from cerberus\n")
open(os.path.join(d, "sub", "NESTED.TXT"), "wb").write(b"nested line 1\nline 2\n")
PY

rm -f work/ext4.img work/ext2.img
mke2fs -q -F -t ext4 -b 4096 -L CERBEXT4 -d "$STAGE" work/ext4.img 65536k
mke2fs -q -F -t ext2 -b 1024 -L CERBEXT2 -d "$STAGE" work/ext2.img 32768k
ls -l work/ext4.img work/ext2.img
