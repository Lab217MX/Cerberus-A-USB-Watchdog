#!/usr/bin/env bash
#
# Cerberus - hosttest/run.sh
#
# Builds the filesystem test images and runs every driver unit test against the
# same headers that ship in the firmware (../fs_ext.h, ../fs_ntfs.h, ../fs_iso.h).
# Rootless: images are populated with mke2fs -d / ntfscp / xorriso -- no mount.
#
# Requires: g++, python3, mke2fs, mkntfs + ntfscp (ntfsprogs), xorriso.
#
#   ./run.sh
#
# Exits non-zero if any test fails.

set -e
cd "$(dirname "$0")"

echo "== generating images =="
./gen_ext.sh  >/dev/null
./gen_ntfs.sh >/dev/null
./gen_iso.sh  >/dev/null

echo "== building tests =="
CXX="${CXX:-g++}"
FLAGS="-std=c++11 -Wall -Wextra -O2"
$CXX $FLAGS -o test_ext  test_ext.cpp
$CXX $FLAGS -o test_ntfs test_ntfs.cpp
$CXX $FLAGS -o test_iso  test_iso.cpp

rc=0
run() { echo "== $* =="; "$@" | tail -n 1; "$@" >/dev/null || rc=1; }

run ./test_ext  work/ext4.img CERBEXT4
run ./test_ext  work/ext2.img CERBEXT2
run ./test_ntfs work/ntfs.img
run ./test_iso  img/iso_plain.iso  plain
run ./test_iso  img/iso_joliet.iso joliet
run ./test_iso  img/iso_notiso.img none

echo
[ $rc -eq 0 ] && echo "ALL DRIVER TESTS PASSED" || echo "SOME DRIVER TESTS FAILED"
exit $rc
