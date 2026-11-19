#!/usr/bin/env python3
"""Temporary helper: count "Not Found SA" retries per SA id in a plaintext hilog stream.

    python3 tools/_nf_sa.py <stream>
"""
import collections
import re
import sys

counts = collections.Counter()
pat = re.compile(r"NF\W{0,3}SA:\s*(\d{4})")
with open(sys.argv[1], errors="replace") as f:
    for line in f:
        m = pat.search(line)
        if m:
            counts[m.group(1)] += 1
for sa, n in counts.most_common(20):
    print(f"SA {sa}: {n} retries")
print(f"total {sum(counts.values())}")
