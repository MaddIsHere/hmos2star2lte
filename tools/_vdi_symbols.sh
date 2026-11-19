#!/bin/bash
# Temporary helper: compare the exported/dynamic surface of the original Huawei
# display composer VDI with the one we build, to see whether composer_host can
# bind to it.
set -e
orig=${1:-/tmp/orig_vdi.so}
ours=${2:-/mnt/c/hmos-logs2/vendl/libdisplay_composer_vdi_impl.z.so}
for f in "$orig" "$ours"; do
    echo "== $(basename "$f")  $(stat -c %s "$f") bytes"
    echo "-- defined dynamic symbols (Create* / Vdi / *Composer*)"
    aarch64-linux-gnu-nm -D --defined-only "$f" | grep -Ei "create|vdi|composer" | head -20
    echo "-- needed"
    aarch64-linux-gnu-readelf -d "$f" | grep NEEDED | sed -E 's/.*\[(.*)\]/\1/' | tr '\n' ' '
    echo
done
echo "== diff of defined dynamic symbols (orig vs ours)"
aarch64-linux-gnu-nm -D --defined-only "$orig" | awk '{print $3}' | sort > /tmp/sym_orig.txt
aarch64-linux-gnu-nm -D --defined-only "$ours" | awk '{print $3}' | sort > /tmp/sym_ours.txt
comm -23 /tmp/sym_orig.txt /tmp/sym_ours.txt | head -30
