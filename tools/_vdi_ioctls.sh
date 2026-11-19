#!/bin/bash
# Temporary helper: show the ioctl command words used by the display VDI libs.
set -e
dir=${1:-/mnt/c/hmos-logs2/vendl}
for f in "$dir"/*.so; do
    echo "== $(basename "$f")"
    aarch64-linux-gnu-objdump -d "$f" | grep -E "mov\s+w[0-9]+, #0x[0-9a-f]{6,8}" | sed -E 's/.*#(0x[0-9a-f]+).*/\1/' | sort | uniq -c
    echo "-- symbols"
    aarch64-linux-gnu-nm -D --defined-only "$f" 2>/dev/null | grep -i "init\|present\|power\|backlight" | head -10
done
