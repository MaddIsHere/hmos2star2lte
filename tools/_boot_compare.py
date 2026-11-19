#!/usr/bin/env python3
"""Temporary helper: compare the decoded hilog chunks of several boots.

    python3 tools/_boot_compare.py <dir> [keyword ...]

For every chunk prints the time span, the render_service pids, and counts of the given keywords
(default: MESA import / RcdProcessInfo / SetDisplayClientBuffer).
"""
import collections
import glob
import os
import re
import sys

d = sys.argv[1]
keys = sys.argv[2:] or ["ohos: import", "RcdProcessInfo", "Composer:", "EGL:"]
def parse(line):
    p = line.split(None, 6)
    if len(p) != 7 or ":" not in p[6]:
        return None
    ts, pid, _tid, _level, _dom, rest = p[0], p[1], p[2], p[3], p[5], p[6]
    if not ts.replace(".", "", 1).isdigit():
        return None
    tag, msg = rest.split(":", 1)
    return ts, pid, tag, msg.strip()

for path in sorted(glob.glob(os.path.join(d, "*.log"))):
    span = ["?", "?"]
    pids = collections.Counter()
    counts = collections.Counter()
    imports = collections.Counter()
    with open(path, errors="replace") as f:
        for line in f:
            parsed = parse(line)
            if not parsed:
                continue
            ts, pid, tag, msg = parsed
            span[0] = span[0] if span[0] != "?" else ts
            span[1] = ts
            if tag.startswith("render_service"):
                pids[pid] += 1
                for k in keys:
                    if k in line:
                        counts[k] += 1
                if "ohos: import" in msg:
                    imports[msg.split("ohos: import", 1)[1][:46]] += 1
    print(os.path.basename(path), "span", span[0], "->", span[1])
    print("   render_service pids:", dict(pids.most_common(4)))
    print("   counts:", dict(counts))
    for k, v in imports.most_common(6):
        print("   import", v, k)
