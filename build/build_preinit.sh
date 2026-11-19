#!/bin/bash
# Build only hmos-preinit (static aarch64) into prebuilt/ramdisk/init.
# Needs: aarch64-linux-gnu-gcc (glibc, static). The other scripts in build/ are the sweet (Snapdragon) ones.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$REPO/prebuilt/ramdisk"
aarch64-linux-gnu-gcc -static -O2 -w -I"$REPO/src/preinit" -o "$REPO/prebuilt/ramdisk/init" "$REPO/src/preinit/preinit.c"
aarch64-linux-gnu-strip "$REPO/prebuilt/ramdisk/init"
echo "built $REPO/prebuilt/ramdisk/init"
