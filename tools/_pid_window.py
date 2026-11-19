#!/usr/bin/env python3
"""Temporary helper: print all lines of one or more pids in a time range (full message).

    python3 tools/_pid_window.py <dir> <pid>[,<pid>...] --from TS --to TS [--limit N]
"""
import glob
import os
import sys

args = sys.argv[1:]
d = args.pop(0)
pids = set(args.pop(0).split(","))
from_ts, to_ts, limit, only_tid = 0.0, float("inf"), 200, None
i = 0
while i < len(args):
    if args[i] == "--from":
        i += 1
        from_ts = float(args[i])
    elif args[i] == "--to":
        i += 1
        to_ts = float(args[i])
    elif args[i] == "--limit":
        i += 1
        limit = int(args[i])
    elif args[i] == "--tid":
        i += 1
        only_tid = args[i]
    i += 1

shown = 0
for path in sorted(glob.glob(os.path.join(d, "*.log"))):
    with open(path, errors="replace") as f:
        for line in f:
            p = line.split(None, 6)
            if len(p) != 7 or p[1] not in pids:
                continue
            try:
                ts = float(p[0])
            except ValueError:
                continue
            if ts < from_ts or ts > to_ts:
                continue
            if only_tid and p[2] != only_tid:
                continue
            print(p[0], p[1], p[2], p[6].rstrip()[:300])
            shown += 1
            if shown >= limit:
                sys.exit(0)
