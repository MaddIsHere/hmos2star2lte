#!/bin/bash
# Temporary helper: show the DECON enable/disable/release sequence of each boot log.
cd "${1:-/mnt/c/hmos-logs2/cache}" || exit 1
for f in kmsg-*.log; do
    [ "$(stat -c %s "$f")" -gt 0 ] || continue
    echo "== $f"
    grep -a -n -e 'decon_release +' -e 'decon_enable' -e 'decon_disable +' -e 'invalid cmd' "$f" | sed -E 's/^([0-9]+):[^;]*;/\1: /' | head -40
done
