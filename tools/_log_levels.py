#!/usr/bin/env python3
"""Temporary helper: print only E/W/F-level lines of decoded hilog files in a time range.

    python3 tools/_log_levels.py <dir> --from TS --to TS [--limit N] [--tag SUBSTR]
"""
import glob
import os
import sys

args = sys.argv[1:]
d = args.pop(0)
from_ts, to_ts, limit, tag = 0.0, float("inf"), 120, ""
i = 0
while i < len(args):
    a = args[i]
    if a == "--from":
        i += 1
        from_ts = float(args[i])
    elif a == "--to":
        i += 1
        to_ts = float(args[i])
    elif a == "--limit":
        i += 1
        limit = int(args[i])
    elif a == "--tag":
        i += 1
        tag = args[i].lower()
    i += 1

shown = 0
for path in sorted(glob.glob(os.path.join(d, "*.log"))):
    with open(path, errors="replace") as f:
        for line in f:
            p = line.split(None, 6)
            if len(p) != 7 or ":" not in p[6]:
                continue
            try:
                ts = float(p[0])
            except ValueError:
                continue
            if ts < from_ts or ts > to_ts:
                continue
            if p[3] not in ("E", "W", "F"):
                continue
            if tag and tag not in p[6].lower():
                continue
            print(p[0], p[1], p[3], p[6].rstrip()[:200])
            shown += 1
            if shown >= limit:
                sys.exit(0)
