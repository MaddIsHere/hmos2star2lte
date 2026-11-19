#!/bin/bash
# Temporary helper: list /sys_prod/etc/bootanimation and dump its config files.
set -u
cd ~/hmos2star2lte || exit 1
IMG=work/ota/sys_prod.img
debugfs -R "ls -l /etc/bootanimation" "$IMG" 2>/dev/null
echo "== recursive"
debugfs -R "rdump /etc/bootanimation /tmp/ba" "$IMG" 2>/dev/null
find /tmp/ba -maxdepth 3 -type f 2>/dev/null | head -20
for f in /tmp/ba/*.json /tmp/ba/*/*.json; do
    [ -f "$f" ] && echo "== $f" && head -40 "$f"
done
