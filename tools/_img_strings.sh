#!/bin/bash
# Temporary helper: dump a file out of an ext4 image and grep it with strings.
#   tools/_img_strings.sh <img> <path-in-image> <keyword> [context-lines]
# The keyword is a plain substring (case-insensitive); no regex metacharacters.
set -e
img=${1:?img}
inner=${2:?path}
pat=${3:-.}
ctx=${4:-0}
out=/tmp/img_strings_$(basename "$inner").txt
rm -f "$out"
debugfs -R "dump $inner $out" "$img" 2>&1 | grep -v -e "debugfs 1" -e "^$" || true
[ -f "$out" ] || { echo "dump failed"; exit 1; }
echo "== $inner ($(stat -c %s "$out") bytes)"
strings -a -t d "$out" > "$out.str"
if [ "$ctx" -gt 0 ]; then
    grep -n -i -F "$pat" "$out.str" | while IFS=: read -r n rest; do
        lo=$((n - ctx)); [ "$lo" -lt 1 ] && lo=1
        hi=$((n + ctx))
        sed -n "${lo},${hi}p" "$out.str"
        echo "---"
    done | head -80
else
    strings -a "$out" | grep -i -F "$pat" | head -40
fi
