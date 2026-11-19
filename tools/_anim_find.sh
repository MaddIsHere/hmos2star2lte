#!/bin/bash
# Temporary helper: find the boot animation assets in the OTA images.
set -u
cd ~/hmos2star2lte || exit 1
for img in work/ota/system.img work/ota/sys_prod.img work/ota/cust.img work/ota/vendor.img; do
    echo "== $img"
    for d in /system/etc /system/media /system/bin /etc /media /bin /app /hw /global; do
        out=$(tools/_img_find.sh "$img" "$d" anim 2>/dev/null)
        [ -n "$out" ] && echo "-- $d" && echo "$out"
    done
done
