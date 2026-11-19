#!/bin/bash
# Decompress the gzip'd hilog persister chunks of a pull into /tmp/<name>-h and
# extract printable strings. Prints the resulting files. Temporary helper.
#   tools/_hilog_decode.sh <pull-dir> <out-prefix>
set -e
src=${1:-/mnt/c/hmos-logs2/hilog}
pfx=${2:-h2}
out=/tmp/$pfx
rm -rf "$out"
mkdir -p "$out"
for f in "$src"/hilog.[0-9]*.gz; do
    [ -f "$f" ] || continue
    b=$(basename "$f" .gz)
    python3 - "$f" "$out/$b.bin" <<'EOF'
import gzip, sys
with gzip.open(sys.argv[1], 'rb') as fi, open(sys.argv[2], 'wb') as fo:
    fo.write(fi.read())
EOF
    strings -n 6 "$out/$b.bin" > "$out/$b.txt"
done
ls -la "$out"
