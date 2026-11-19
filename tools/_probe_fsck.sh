#!/bin/bash
# temporary: list fsck/format tools inside the built HarmonyOS images
cd /home/rommerfy/hmos2star2lte || exit 1
for img in "$@"; do
  echo "== $img =="
  debugfs -R "ls -l /bin" "$img" 2>/dev/null | grep -Ei 'fsck|e2fs|f2fs|mkfs|tune2fs' || echo "  (none in /bin)"
  debugfs -R "ls -l /sbin" "$img" 2>/dev/null | grep -Ei 'fsck|e2fs|f2fs|mkfs|tune2fs' || echo "  (none in /sbin)"
  debugfs -R "ls -l /system/bin" "$img" 2>/dev/null | grep -Ei 'fsck|e2fs|f2fs|mkfs|tune2fs' || echo "  (none in /system/bin)"
done
