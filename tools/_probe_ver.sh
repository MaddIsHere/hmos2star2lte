#!/bin/bash
# temporary: show the compiled build stamp inside kernel images
cd /home/rommerfy/hmos2star2lte || exit 1
for f in "$@"; do
  printf '== %s ==\n' "$f"
  grep -a -o 'SMP PREEMPT.\{0,60\}' "$f" | head -3 | tr -d '\000' | tr '\n' '|'
  printf '\n'
done
