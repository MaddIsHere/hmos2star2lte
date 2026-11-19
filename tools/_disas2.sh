#!/bin/bash
set -e
cd /tmp
aarch64-linux-gnu-objdump -d ueventd.bin > /tmp/ueventd.dis 2>/dev/null
grep -nE '^\s+b[cdef][0-9a-f][0-9a-f]:' /tmp/ueventd.dis | head -1
sed -n '/^    bc00:/,/^    be60:/p' /tmp/ueventd.dis
