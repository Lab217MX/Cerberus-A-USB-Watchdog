#!/usr/bin/env bash
#
# Cerberus - hosttest/gen_ntfs.sh
#
# Generates the NTFS test image used by test_ntfs.cpp into ./work/ntfs.img.
# Rootless: mkntfs writes the filesystem to a plain file and ntfscp copies files
# in without mounting. ntfscp cannot create subdirectories, so the NTFS test is
# root-level only (still exercises the MFT, directory index, USA fixup and both
# resident and non-resident $DATA / runlists).
#
#   work/ntfs.img   NTFS, 512-byte sectors, 4K clusters  (label CERBNTFS)
#
# Known-content files (reproduced byte-for-byte by test_ntfs.cpp):
#   /BIG2MB.BIN   2 MiB, byte i = i & 0xFF   (non-resident, multi-cluster runlist)
#   /SMALL.TXT    5000 bytes, byte i = i % 251
#   /hello.txt    "hello from cerberus\n"    (small; may be resident)

set -e
cd "$(dirname "$0")"
mkdir -p work
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

python3 - "$STAGE" <<'PY'
import os, sys
d = sys.argv[1]
open(os.path.join(d, "BIG2MB.BIN"), "wb").write(bytes((i & 0xFF) for i in range(2 * 1024 * 1024)))
open(os.path.join(d, "SMALL.TXT"), "wb").write(bytes((i % 251) for i in range(5000)))
open(os.path.join(d, "hello.txt"), "wb").write(b"hello from cerberus\n")
PY

rm -f work/ntfs.img
truncate -s 96M work/ntfs.img
mkntfs --force --fast --label CERBNTFS --sector-size 512 --cluster-size 4096 work/ntfs.img >/dev/null 2>&1
ntfscp work/ntfs.img "$STAGE/BIG2MB.BIN" /BIG2MB.BIN
ntfscp work/ntfs.img "$STAGE/SMALL.TXT"  /SMALL.TXT
ntfscp work/ntfs.img "$STAGE/hello.txt"  /hello.txt
ls -l work/ntfs.img
