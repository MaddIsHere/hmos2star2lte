#!/bin/bash
# List a CACHE pull: file times, sizes and the content of each boot-* directory.
# Temporary helper: tools/_cache_list.sh [cache-dir]
cd "${1:-/mnt/c/hmos-logs2/cache}" || exit 1
ls -la --time-style=+%m-%d_%H:%M:%S
echo "=== boot dirs"
for d in boot-*; do
    [ -d "$d" ] || continue
    echo "--- $d"
    ls -la --time-style=+%m-%d_%H:%M:%S "$d"
done
