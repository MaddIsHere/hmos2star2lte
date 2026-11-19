#!/bin/bash
set -e
cd /tmp
readelf -SW ueventd.bin | grep -E 'rodata|text' || true
echo '=== code refs to 0x2e90 area ==='
objdump -d ueventd.bin 2>/dev/null | grep -nE 'adrp.*0x(2f|30|[0-9a-f]+)000' | head -5
objdump -d ueventd.bin 2>/dev/null | grep -nE 'add.*#0xe90|add.*#0x2e90|#0x2e90' | head -20
