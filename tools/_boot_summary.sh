#!/bin/bash
# Show boot events, display-related params and processes for each boot-* snapshot dir.
cd "${1:-/mnt/c/hmos-logs2/cache}" || exit 1
for d in boot-*; do
    [ -d "$d" ] || continue
    echo "===== $d"
    for p in "$d"/snap*-param.txt; do
        [ -f "$p" ] || continue
        echo "-- $(basename "$p")"
        grep -E "bootevent\.(wms|boot\.completed|renderservice|appfwk|sceneboard)|persist\.sys\.graphic|persist\.display|persist\.brightness" "$p"
    done
    for m in "$d"/snap*-mounts.txt; do
        [ -f "$m" ] || continue
        echo "-- $(basename "$m") data/log/vendor lines"
        grep -E " /(data|log|vendor) " "$m"
    done
done
