#!/data/data/com.termux/files/usr/bin/sh
# Temporary helper (run on device, TWRP): compare stale *_tmp module dirs
# against the bundle list in $1 (one bundle name per line).
#
#   adb push tools/_tmp_probe.sh /tmp/_tmp_probe.sh
#   adb shell sh /tmp/_tmp_probe.sh /tmp/missing20.txt
LIST=${1:-/tmp/missing20.txt}
BASE=/data/app/el1/bundle/public

echo "== stale *_tmp dirs per bundle"
for b in $(ls "$BASE"); do
    n=0
    libs=""
    for t in "$BASE/$b"/*_tmp; do
        [ -d "$t" ] || continue
        n=$((n + 1))
        [ -d "$t/libs/arm64" ] && libs="$libs $(basename "$t")"
    done
    [ "$n" -gt 0 ] && echo "$b tmp=$n libs:$libs"
done

echo "== missing bundles"
while read -r b; do
    [ -n "$b" ] || continue
    d="$BASE/$b"
    if [ -d "$d" ]; then
        echo "$b present: $(ls "$d" | tr '\n' ' ')"
    else
        echo "$b ABSENT"
    fi
done < "$LIST"
