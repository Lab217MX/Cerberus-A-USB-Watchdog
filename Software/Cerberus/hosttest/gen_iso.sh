#!/usr/bin/env bash
#
# Cerberus - hosttest/gen_iso.sh
#
# Generates the ISO9660/Joliet test images used by test_iso.cpp into ./img/.
# Idempotent: images are rebuilt from scratch on every run (staging in mktemp).
#
#   img/iso_plain.iso   ISO9660 only (d-character upper-case names)
#   img/iso_joliet.iso  same tree + Joliet SVD (mixed-case long names)
#   img/iso_trunc.iso   first 64 KiB of the joliet image (truncated)
#   img/iso_notiso.img  1 MiB of zeros (not an ISO)
#

set -e
cd "$(dirname "$0")"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/tree/SUBDIR" "$STAGE/tree/EMPTYDIR"

# Known-content files (byte patterns are reproduced by the C++ test):
#   BIG2MB.BIN : 2 MiB, byte i = i & 0xFF
#   SMALL.TXT  : 5000 bytes, byte i = i % 251
python3 - "$STAGE/tree" <<'PY'
import os, sys
tree = sys.argv[1]
big = (bytes(range(256)) * (2048 * 1024 // 256 + 1))[:2048 * 1024]
open(os.path.join(tree, "BIG2MB.BIN"), "wb").write(big)
small = (bytes(range(251)) * (5000 // 251 + 1))[:5000]
open(os.path.join(tree, "SMALL.TXT"), "wb").write(small)
open(os.path.join(tree, "hello world.txt"), "wb").write(b"hello from cerberus\n")
open(os.path.join(tree, "LongMixedCaseName.txt"), "wb").write(b"long mixed case name content\n")
open(os.path.join(tree, "SUBDIR", "NESTED.TXT"), "wb").write(b"nested file content line 1\nline 2\n")
PY

mkdir -p img

xorriso -as mkisofs -quiet -V CERBPLAIN -o img/iso_plain.iso "$STAGE/tree"
xorriso -as mkisofs -quiet -V JOLIETVL -J -o img/iso_joliet.iso "$STAGE/tree"

head -c 65536 img/iso_joliet.iso > img/iso_trunc.iso
dd if=/dev/zero of=img/iso_notiso.img bs=1M count=1 status=none

ls -l img/
