#!/bin/bash
# one-off: dump ueventd from out/system.img and show relevant strings
set -e
cd "$(dirname "$0")/.."
rm -f /tmp/ueventd.bin
debugfs -R 'dump /system/bin/ueventd /tmp/ueventd.bin' out/system.img >/dev/null 2>&1
ls -l /tmp/ueventd.bin
echo '=== strings ==='
strings -a /tmp/ueventd.bin | grep -nE 'boot_devices|is not match|by-name|block/' | head -40
