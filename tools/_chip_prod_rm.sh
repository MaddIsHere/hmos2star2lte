#!/bin/bash
# Temporary helper for fast iteration: drop /etc/init/CameraDaemon.cfg from out/chip_prod.img
# instead of running a full build. The image may be hard linked with work/ota/, so it is
# replaced by a private copy first. tools/gen_patch_docs.py unrelated.
set -e
cd "$(dirname "$0")/.."
img=out/chip_prod.img
[ -f "$img" ] || { echo "$img missing"; exit 1; }
if [ "$(stat -c %h "$img")" -gt 1 ]; then
    echo "breaking hard link to work/ota"
    cp --remove-destination "$img" "$img.new"
    mv "$img.new" "$img"
fi
printf 'rm /etc/init/CameraDaemon.cfg\n' > /tmp/rm_chip_prod.txt
debugfs -w -f /tmp/rm_chip_prod.txt "$img" 2>&1 | grep -v "^debugfs " || true
if debugfs -R "ls /etc/init" "$img" 2>/dev/null | grep -q CameraDaemon; then
    echo "FAILED: CameraDaemon.cfg still there"
    exit 1
fi
e2fsck -fn "$img" >/dev/null 2>&1 || { echo "FAILED: e2fsck complains"; exit 1; }
echo "ok: $(ls -l "$img" | cut -d' ' -f5) bytes, clean"
