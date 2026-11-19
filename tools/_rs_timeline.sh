#!/bin/bash
# Temporary helper: render_service activity timeline from decoded hilog.
#   tools/_rs_timeline.sh <dir> [from-epoch] [top-tags]
cd "${1:?dir}" || exit 1
from=${2:-0}
top=${3:-25}
awk -v f="$from" '$1 >= f' *.log | grep 'render_service/' > /tmp/rs_lines.txt
echo "lines: $(wc -l < /tmp/rs_lines.txt)"
sed -E 's/^.* render_service\///' /tmp/rs_lines.txt | cut -d: -f1 | sort | uniq -c | sort -rn | head -n "$top"
