#!/usr/bin/env python3
"""Temporary helper: grep decoded hilog files for substrings, printing time/pid/level/tag/message.

    python3 tools/_log_grep.py <dir> <substr> [<substr> ...] [--from TS] [--to TS] [--limit N]
                               [--levels EW]
"""
import glob
import os
import sys

args = sys.argv[1:]
d = args.pop(0)
sinkeys, from_ts, to_ts, limit, levels = [], 0.0, float("inf"), 200, None
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
    elif a == "--levels":
        i += 1
        levels = set(args[i])
    else:
        sinkeys.append(a.lower())
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
            if levels is not None and p[3] not in levels:
                continue
            low = line.lower()
            if any(k in low for k in sinkeys):
                print(p[0], p[1], p[2], p[3], p[6].rstrip()[:200])
                shown += 1
                if shown >= limit:
                    sys.exit(0)
