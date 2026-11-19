#!/bin/bash
# Summarise the CACHE kernel logs of a pull: time range, display/backlight/composer
# mentions and the last lines. Temporary helper.
cd "${1:-/mnt/c/hmos-logs2/cache}" || exit 1
for f in kmsg-*.log; do
    sz=$(stat -c %s "$f")
    echo "== $f  ($sz bytes)"
    [ "$sz" -gt 0 ] || continue
    first=$(head -1 "$f")
    last=$(tail -1 "$f")
    echo "  first: ${first:0:120}"
    echo "  last : ${last:0:120}"
    echo "  display/decon/backlight/composer lines: $(grep -ci "decon\|backlight\|composer\|drm\|fb0\|lcd" "$f")"
done
