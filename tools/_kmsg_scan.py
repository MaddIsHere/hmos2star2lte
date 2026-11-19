#!/usr/bin/env python3
"""Temporary helper: count matching kmsg lines per 10 s bucket.

    python3 tools/_kmsg_scan.py <kmsg> <substr> [<substr> ...]

Prints first/last uptime and a per-10-s histogram, so a spam source can be tied to a moment of the boot.
"""
import re
import sys

path = sys.argv[1]
needles = [n.lower() for n in sys.argv[2:]]
buckets = {}
first = last = None
total = 0
pat = re.compile(r"^\d+,\d+,(\d+),")
with open(path, errors="replace") as f:
    for line in f:
        low = line.lower()
        if not any(n in low for n in needles):
            continue
        m = pat.match(line)
        if not m:
            continue
        sec = int(m.group(1)) / 1e6
        first = sec if first is None else first
        last = sec
        total += 1
        b = int(sec // 10) * 10
        buckets[b] = buckets.get(b, 0) + 1

print("matches %d, first %.1f s, last %.1f s" % (total, first or 0, last or 0))
for b in sorted(buckets):
    print("  %5d-%5d s : %5d %s" % (b, b + 9, buckets[b], "#" * min(60, buckets[b] // 20)))
